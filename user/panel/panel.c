#include "panel.h"
#include "menu.h"
#include "clock.h"
#include "taskbar.h"
#include "icons.h"

static void on_launcher_clicked(GtkWidget *button, gpointer user_data)
{
    menu_popup(user_data, button);
}

void panel_window_place(GtkWidget *win, gpointer user_data)
{
    (void)win;
    (void)user_data;
    /* The compositor docks the panel to the bottom edge (so: shell hook);
     * GNOME/Wayland needs no client-side positioning here. */
}

GtkWidget *panel_window_new(void)
{
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(win, "panel-window");
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "panel");
    gtk_window_set_title(GTK_WINDOW(win), "Kyronix Panel");
    gtk_window_set_default_size(GTK_WINDOW(win), 1280, PANEL_HEIGHT);
    gtk_widget_set_size_request(GTK_WIDGET(win), -1, PANEL_HEIGHT);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_gravity(GTK_WINDOW(win), GDK_GRAVITY_NORTH_WEST);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);

    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(main, "bar");
    gtk_container_add(GTK_CONTAINER(win), main);

    /* Menu / application launcher button (far left). */
    GtkWidget *launcher = gtk_button_new();
    GtkWidget *l_img = gtk_image_new_from_file(launcher_icon_path());
    gtk_widget_set_name(launcher, "launcher-button");
    gtk_button_set_relief(GTK_BUTTON(launcher), GTK_RELIEF_NONE);
    gtk_button_set_image(GTK_BUTTON(launcher), l_img);
    gtk_image_set_pixel_size(GTK_IMAGE(l_img), 24);
    gtk_widget_set_tooltip_text(launcher, "Applications");
    gtk_widget_set_name(launcher, "launcher-button");
    gtk_box_pack_start(GTK_BOX(main), launcher, FALSE, FALSE, 0);

    /* Taskbar (running apps), placed right after the launcher. */
    GtkWidget *taskbar = taskbar_new();
    gtk_box_pack_start(GTK_BOX(main), taskbar, FALSE, FALSE, 0);

    GtkWidget *menu = menu_new(launcher, taskbar);
    gtk_widget_set_hexpand(launcher, FALSE);
    g_signal_connect(launcher, "clicked", G_CALLBACK(on_launcher_clicked), menu);

    /* Spacer pushes the clock to the right edge. */
    GtkWidget *spacer = gtk_label_new(NULL);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(main), spacer, TRUE, TRUE, 0);

    /* Clock on the right. */
    GtkWidget *clock = clock_label_new();
    gtk_widget_set_margin_end(clock, 14);
    gtk_widget_set_margin_top(clock, 5);
    gtk_widget_set_margin_bottom(clock, 5);
    gtk_box_pack_start(GTK_BOX(main), clock, FALSE, FALSE, 0);

    return win;
}