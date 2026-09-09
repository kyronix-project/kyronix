#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include "panel.h"
#include "style.h"

#define SOCKET_WAIT_MS 30000
#define POLL_STEP_MS   250

/* Set the Wayland environment for this process. The panel is started by
 * init as a standalone service, so it must provide its own display env. */
static void ensure_wayland_env(void)
{
    setenv("GDK_BACKEND", "wayland", 1);
    setenv("XDG_RUNTIME_DIR", "/run/user/0", 0);
    setenv("WAYLAND_DISPLAY", "wayland-0", 0);
}

/* Wait until weston's wayland socket exists. Returns 0 on success, -1 on
 * timeout so the caller can report and exit. */
static int wait_for_display(void)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    const char *display = getenv("WAYLAND_DISPLAY");
    if (!dir || !display)
        return -1;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, display);

    clock_t start = clock();
    for (;;) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode))
            return 0;

        long elapsed_ms = (clock() - start) * 1000 / CLOCKS_PER_SEC;
        if (elapsed_ms >= SOCKET_WAIT_MS)
            return -1;

        usleep(POLL_STEP_MS * 1000);
    }
}

int main(int argc, char **argv)
{
    ensure_wayland_env();

    if (wait_for_display() != 0) {
        fprintf(stderr, "kyronix-panel: wayland display not available\n");
        return 1;
    }

    g_set_prgname("kyronix-panel");
    gtk_init(&argc, &argv);

    style_apply();

    GtkWidget *win = panel_window_new();

    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(win, "map", G_CALLBACK(panel_window_place), NULL);
    gtk_widget_show_all(win);

    gtk_main();
    return 0;
}