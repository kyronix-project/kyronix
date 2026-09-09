#ifndef KYRONIX_TASKBAR_H
#define KYRONIX_TASKBAR_H

#include <gtk/gtk.h>
#include <sys/types.h>

/* Horizontal strip of running-application icons. Each icon tracks a process
 * (the shell child launched from the menu); clicking it activates/minimizes/
 * restores the matching window through the desktop-shell "kyronix_wm"
 * protocol, identifying the window by its client PID (with app_id as fallback). */
GtkWidget *taskbar_new(void);

/* Register a just-launched process so an icon appears. The icon is removed
 * automatically when the process exits. */
void taskbar_track(GtkWidget *taskbar, pid_t pid, const char *app_id,
                   const char *name, const char *icon_path);

/* Remove the icon for a PID (process exited / no longer relevant). */
void taskbar_untrack(GtkWidget *taskbar, pid_t pid);

#endif