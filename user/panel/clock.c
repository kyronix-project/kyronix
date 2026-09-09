#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>

#include "clock.h"

static gboolean update_clock(gpointer data)
{
    GtkLabel *label = GTK_LABEL(data);
    time_t now = time(NULL);
    struct tm tm;
    char buf[64];

    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);

    gtk_label_set_text(label, buf);
    return G_SOURCE_CONTINUE;
}

GtkWidget *clock_label_new(void)
{
    GtkWidget *label = gtk_label_new("");
    gtk_widget_set_name(label, "clock-label");
    update_clock(label);
    g_timeout_add_seconds(1, update_clock, label);
    return label;
}
