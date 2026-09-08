#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <gdk/gdkkeysyms.h>

#include "menu.h"
#include "taskbar.h"
#include "icons.h"
#include "de.h"

/* Derive the compositor app_id for a launcher entry from its Exec line:
 * the first token. weston-terminal -> "weston-terminal", ksh -> "ksh". */
static char *
app_id_from_exec(const char *exec)
{
	const char *s = exec;

	while (*s == ' ' || *s == '\t')
		s++;

	const char *end = s;
	while (*end != '\0' && *end != ' ' && *end != '\t' &&
	       *end != ';' && *end != '&')
		end++;

	return g_strndup(s, end - s);
}

/* Split `s` in place into whitespace-separated argv tokens (quotes are not
 * honoured; only used for simple Exec lines such as "weston-terminal -s ...").
 * Returns the token count, stopping before the NUL-terminated argv slot. */
static int split_command(char *s, char **argv, int max)
{
	int n = 0;

	for (;;) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '\0')
			break;
		if (n >= max - 1)
			break;
		argv[n++] = s;
		while (*s != '\0' && *s != ' ' && *s != '\t')
			s++;
		if (*s != '\0')
			*s++ = '\0';
	}
	argv[n] = NULL;
	return n;
}

/* Return non-zero if `s` contains shell metacharacters that a plain
 * argv-split would mangle (quotes, pipes, redirection, sequencing, globs). */
static int has_shell_metachars(const char *s)
{
	for (; *s; s++)
		if (strchr(";&|<>*?[]{}$`'\"\\", *s))
			return 1;
	return 0;
}

/* Launch `cmd` fully detached from the panel. The panel should not outlive
 * its children, and the launched app must survive the panel closing, so we
 * emulate nohup: ignore SIGHUP, start a new session and redirect stdio to
 * /dev/null. A plain command line (no shell metacharacters) is exec'd
 * directly, making the child PID equal to the app PID; that is the value
 * returned to the panel and used for per-window taskbar tracking. */
static pid_t launch(const char *cmd)
{
	pid_t pid = fork();

	if (pid == 0) {
		int fd;

		signal(SIGHUP, SIG_IGN);
		setsid();

		fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}

		if (!has_shell_metachars(cmd)) {
			char *copy = strdup(cmd);
			char *argv[64];
			if (copy && split_command(copy, argv, 64) > 0) {
				execvp(argv[0], argv);
				free(copy);
			}
		}
		/* Fall back to a shell for complex command lines. */
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}

	if (pid < 0)
		return -1;

	return pid;
}

static void on_menu_activate(GtkButton *button, gpointer user_data)
{
	(void)user_data;
	GtkWidget *menu = gtk_widget_get_toplevel(GTK_WIDGET(button));
	GtkWidget *taskbar = g_object_get_data(G_OBJECT(menu), "taskbar");
	LauncherEntry *e = g_object_get_data(G_OBJECT(button), "entry-data");
	pid_t pid;
	char *app_id;

	if (e == NULL)
		return;

	pid = launch(e->exec);
	if (pid > 0 && taskbar != NULL) {
		app_id = app_id_from_exec(e->exec);
		taskbar_track(GTK_WIDGET(taskbar), pid, app_id, e->name,
			      icon_path_for(e->icon));
		g_free(app_id);
	}

	gtk_widget_hide(menu);
}

static gboolean on_menu_keypress(GtkWidget *win, GdkEventKey *event,
				 gpointer user_data)
{
	(void)user_data;

	if (event->keyval == GDK_KEY_Escape) {
		gtk_widget_hide(win);
		return TRUE;
	}

	return FALSE;
}

static void add_entry(GtkWidget *list, GtkWidget *menu,
		      const LauncherEntry *e)
{
	GtkWidget *button = gtk_button_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *image = gtk_image_new_from_file(icon_path_for(e->icon));
	GtkWidget *label = gtk_label_new(e->name);

	gtk_widget_set_name(button, "start-item");
	gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);

	gtk_image_set_pixel_size(GTK_IMAGE(image), 18);
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	gtk_widget_set_hexpand(label, TRUE);

	gtk_box_pack_start(GTK_BOX(row), image, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(button), row);

	g_object_set_data_full(G_OBJECT(button), "entry-data",
			       launcher_entry_clone(e),
			       (GDestroyNotify)launcher_entry_free);
	g_signal_connect(button, "clicked", G_CALLBACK(on_menu_activate),
			 menu);

	gtk_container_add(GTK_CONTAINER(list), button);
}

/* Build the application launcher as a plain, always-interactive toplevel
 * window (not a GtkMenu/GtkPopover popup: popups never receive pointer
 * input under our Weston, while a normal window does, just like the panel
 * itself). The compositor decides where it appears. Returns the window. */
GtkWidget *
menu_new(GtkWidget *button, GtkWidget *taskbar)
{
	(void)button;
	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

	gtk_widget_set_name(win, "start-menu");
	gtk_window_set_title(GTK_WINDOW(win), "Kyronix Start Menu");
	gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
	gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
	gtk_window_set_accept_focus(GTK_WINDOW(win), TRUE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(win), 130, 340);
	gtk_widget_set_size_request(GTK_WIDGET(win), 130, 200);

	gtk_widget_set_name(list, "start-body");
	gtk_container_add(GTK_CONTAINER(win), list);
	g_object_set_data(G_OBJECT(win), "taskbar", taskbar);
	g_object_set_data(G_OBJECT(win), "body", list);
	gtk_widget_set_size_request(list, 130, -1);
	g_signal_connect(win, "key-press-event", G_CALLBACK(on_menu_keypress),
			 NULL);

	gtk_widget_set_hexpand(list, TRUE);
	gtk_widget_set_vexpand(list, TRUE);

	LauncherEntry *static_entries[] = {
		&(LauncherEntry){ "Terminal", "utilities-terminal",
				  "weston-terminal" },
		&(LauncherEntry){ "Shell", "shell", "ksh" },
		&(LauncherEntry){ "System Info", "system-run",
				  "ksh -c 'uname -a; echo; free; echo; "
				  "df -h; echo; read'" },
		&(LauncherEntry){ "Reboot", "system-reboot", "reboot" },
	};

	LauncherEntry **desktop = NULL;
	int nd = de_load_dir(&desktop, "/usr/share/applications");

	if (nd > 0) {
		for (int i = 0; i < nd; i++)
			add_entry(list, win, desktop[i]);
		for (int i = 0; i < nd; i++)
			launcher_entry_free(desktop[i]);
		free(desktop);
	} else {
		for (size_t i = 0; i < sizeof(static_entries) /
			  sizeof(static_entries[0]);
		     i++)
			add_entry(list, win, static_entries[i]);
	}

	return win;
}

/* Toggle `menu` open/closed. */
void
menu_popup(GtkWidget *menu, GtkWidget *button)
{
	(void)button;

	if (gtk_widget_get_visible(menu)) {
		gtk_widget_hide(menu);
		return;
	}

	gtk_widget_show_all(menu);
	gtk_widget_queue_resize(menu);
}