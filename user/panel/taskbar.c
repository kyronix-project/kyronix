#include "taskbar.h"
#include "icons.h"

#include <gdk/gdkwayland.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

/*
 * Minimal client-side description of the desktop-shell "kyronix_wm" global.
 * Requests (in order): activate(app_id "s", pid "i"), minimize(app_id, pid),
 * restore(app_id, pid), toggle(app_id, pid).  Each request carries the client
 * PID so the shell can target the exact window the taskbar icon tracks.
 *
 * The shell is the source of truth for whether a window is minimized (a window
 * can be minimized by other means, not only through this panel).  On click the
 * panel sends toggle(pid); the shell inspects the window's current state and
 * either minimizes or restores it, then reports the new state back via the
 * "minimized" event so the panel never drifts out of sync.
 */
static const struct wl_message kyronix_wm_requests[] = {
	{ "activate", "si", NULL },
	{ "minimize", "si", NULL },
	{ "restore", "si", NULL },
	{ "toggle", "si", NULL },
};

static const struct wl_message kyronix_wm_events[] = {
	{ "minimized", "uu", NULL },
};

static const struct wl_interface kyronix_wm_interface = {
	"kyronix_wm", 3, 4, kyronix_wm_requests, 1, kyronix_wm_events
};

struct task_item {
	struct taskbar *taskbar;
	struct wl_proxy *wm;
	pid_t pid;
	guint watch_src;
	char app_id[64];
	char name[64];
	GtkWidget *button;
	gboolean minimized;
	gboolean restore_pending;
};

struct taskbar {
	GtkWidget *box;
	struct wl_proxy *wm;
	GHashTable *items; /* pid -> task_item* */
};

/* -- registry/binding ---------------------------------------------------- */

static void
registry_handle_global(void *data, struct wl_registry *registry, uint32_t name,
		       const char *interface, uint32_t version)
{
	struct taskbar *taskbar = data;

	(void)version;
	if (strcmp(interface, kyronix_wm_interface.name) == 0)
		taskbar->wm = wl_registry_bind(registry, name,
					       &kyronix_wm_interface, 3);
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
	struct taskbar *taskbar = data;

	(void)registry;
	(void)name;
	if (taskbar->wm != NULL) {
		wl_proxy_destroy(taskbar->wm);
		taskbar->wm = NULL;
	}
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove,
};

/* wl_proxy listener for the kyronix_wm "minimized" event: trusted update of a
 * task item's minimized state straight from the compositor. */
static void
wm_handle_minimized(void *data, struct wl_proxy *wm, uint32_t pid,
		    uint32_t minimized)
{
	struct taskbar *taskbar = data;
	struct task_item *item;

	(void)wm;
	item = g_hash_table_lookup(taskbar->items, GINT_TO_POINTER((gint)pid));
	if (item == NULL)
		return;
	item->minimized = minimized ? TRUE : FALSE;
	gtk_widget_set_state_flags(item->button, GTK_STATE_FLAG_ACTIVE,
				   item->minimized);
}

struct kyronix_wm_listener {
	void (*minimized)(void *data, struct wl_proxy *wm, uint32_t pid,
			  uint32_t minimized);
};

static const struct kyronix_wm_listener wm_listener = {
	.minimized = wm_handle_minimized,
};

static void
taskbar_bind_wm(struct taskbar *taskbar)
{
	struct wl_registry *registry;
	struct wl_display *display;

	if (!GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default()))
		return;

	display = gdk_wayland_display_get_wl_display(
		GDK_WAYLAND_DISPLAY(gdk_display_get_default()));
	if (display == NULL)
		return;

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, taskbar);
	wl_display_roundtrip(display);
	wl_registry_destroy(registry);

	if (taskbar->wm != NULL)
		wl_proxy_add_listener(taskbar->wm,
				      (void (**)(void)) &wm_listener, taskbar);
}

/* -- actions -------------------------------------------------------------- */

static void
task_item_send(struct task_item *item, uint32_t opcode)
{
	struct wl_display *display;

	if (item->wm == NULL) {
		fprintf(stderr, "kyronix-panel: task_item_send: wm NULL\n");
		return;
	}

	display = gdk_wayland_display_get_wl_display(
		GDK_WAYLAND_DISPLAY(gdk_display_get_default()));
	if (display == NULL) {
		fprintf(stderr, "kyronix-panel: task_item_send: no display\n");
		return;
	}

	fprintf(stderr, "kyronix-panel: opcode=%u app_id=\"%s\" pid=%d "
		"minimized=%d\n", opcode, item->app_id, item->pid,
		item->minimized);
	wl_proxy_marshal(item->wm, opcode, item->app_id, item->pid);
	wl_display_flush(display);
}

static void
task_item_clicked(GtkWidget *widget, gpointer user_data)
{
	struct task_item *item = user_data;

	/* Ask the shell to flip the window's minimized state.  The shell is the
	 * source of truth here (the window may have been minimized by other
	 * means), so the panel does not decide minimize vs restore itself; the
	 * resulting state arrives via the "minimized" event. */
	task_item_send(item, 3); /* toggle */
	(void)widget;
}

/* -- lifecycle ------------------------------------------------------------ */

static void
task_item_exited(GPid pid, gint status, gpointer user_data)
{
	struct task_item *item = user_data;

	(void)pid;
	(void)status;
	item->watch_src = 0;
	taskbar_untrack(item->taskbar->box, item->pid);
}

static void
task_item_free(gpointer data)
{
	struct task_item *item = data;

	if (item->button != NULL)
		gtk_widget_destroy(item->button);
	g_free(item);
}

void
taskbar_track(GtkWidget *taskbar_widget, pid_t pid, const char *app_id,
	      const char *name, const char *icon_path)
{
	struct taskbar *taskbar = g_object_get_data(
		G_OBJECT(taskbar_widget), "taskbar-data");
	struct task_item *item;
	GtkWidget *image;

	if (taskbar == NULL || pid <= 0)
		return;

	item = g_hash_table_lookup(taskbar->items, GINT_TO_POINTER(pid));
	if (item != NULL)
		return;

	item = g_new0(struct task_item, 1);
	item->taskbar = taskbar;
	item->wm = taskbar->wm;
	item->pid = pid;
	snprintf(item->app_id, sizeof(item->app_id), "%s",
		 app_id ? app_id : "");
	snprintf(item->name, sizeof(item->name), "%s", name ? name : "");

	image = gtk_image_new_from_file(icon_path);
	gtk_image_set_pixel_size(GTK_IMAGE(image), 24);

	item->button = gtk_button_new();
	gtk_widget_set_name(item->button, "task-item");
	gtk_widget_set_tooltip_text(item->button, item->name);
	gtk_button_set_relief(GTK_BUTTON(item->button), GTK_RELIEF_NONE);
	gtk_button_set_image(GTK_BUTTON(item->button), image);

	gtk_box_pack_start(GTK_BOX(taskbar->box), item->button, FALSE, FALSE,
			   0);
	g_signal_connect(item->button, "clicked", G_CALLBACK(task_item_clicked),
			 item);

	g_hash_table_insert(taskbar->items, GINT_TO_POINTER(pid), item);
	gtk_widget_show(item->button);
	item->watch_src = g_child_watch_add(pid, task_item_exited, item);
}

void
taskbar_untrack(GtkWidget *taskbar_widget, pid_t pid)
{
	struct taskbar *taskbar = g_object_get_data(
		G_OBJECT(taskbar_widget), "taskbar-data");
	struct task_item *item;

	if (taskbar == NULL)
		return;

	item = g_hash_table_lookup(taskbar->items, GINT_TO_POINTER(pid));
	if (item == NULL)
		return;

	if (item->watch_src != 0)
		g_source_remove(item->watch_src);
	g_hash_table_remove(taskbar->items, GINT_TO_POINTER(pid));
}

/* -- construction --------------------------------------------------------- */

GtkWidget *
taskbar_new(void)
{
	GtkWidget *box;
	struct taskbar *taskbar;

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_set_name(box, "taskbar");

	taskbar = g_new0(struct taskbar, 1);
	taskbar->box = box;
	taskbar->items = g_hash_table_new_full(g_direct_hash, g_direct_equal,
					       NULL, task_item_free);
	g_object_set_data_full(G_OBJECT(box), "taskbar-data", taskbar, NULL);

	taskbar_bind_wm(taskbar);

	return box;
}