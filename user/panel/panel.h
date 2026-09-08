#ifndef KYRONIX_PANEL_H
#define KYRONIX_PANEL_H

#include <gtk/gtk.h>

#define PANEL_HEIGHT 36

/* The single toplevel window that holds the panel bar. */
GtkWidget *panel_window_new(void);

/* Pin the panel to the top edge of the primary monitor on map. */
void panel_window_place(GtkWidget *win, gpointer user_data);

#endif
