#ifndef KYRONIX_MENU_H
#define KYRONIX_MENU_H

#include <gtk/gtk.h>

/*
 * A single launcher entry. This is deliberately a small, toolkit-agnostic
 * struct so that the menu can later be backed by /usr/share/applications
 * .desktop files (see de.h) without rewriting the UI code.
 */
typedef struct {
    char *name;   /* display name            */
    char *icon;   /* icon name (optional)    */
    char *exec;   /* command line to launch  */
} LauncherEntry;

/* Builds the application launcher menu as a plain toplevel window with a
 * vertical list of entries. Clicking an entry launches the app and
 * registers it in `taskbar`. Returns the GtkWindow. */
GtkWidget *menu_new(GtkWidget *button, GtkWidget *taskbar);

/* Toggle `menu` open/closed. */
void menu_popup(GtkWidget *menu, GtkWidget *button);

#endif