#include "style.h"

static const char *css =
        "window.panel {"
        "  background-image: linear-gradient(to bottom, "
        "      rgba(22, 20, 24, 0.94), "
        "      rgba(22, 20, 24, 0.82));"
        "  background-color: transparent;"
        "  border: none;"
        "  box-shadow: 0 1px 3px rgba(0,0,0,0.6);"
        "  min-height: 36px;"
        "}"
        "window.panel > box {"
        "  border-top: 1px solid rgba(255, 255, 255, 0.05);"
        "}"
        "label#clock-label {"
        "  color: rgb(205, 214, 244);"
        "  font-family: 'DejaVu Sans';"
        "  font-size: 13px;"
        "}"
        "window.panel button {"
        "  color: rgb(205, 214, 244);"
        "  background-color: transparent;"
        "  background-image: none;"
        "  border: none;"
        "  box-shadow: none;"
        "  border-radius: 0px;"
        "  padding: 4px 10px;"
        "}"
        "window.panel button#launcher-button {"
        "  padding: 4px 12px;"
        "}"
        "window.panel button#launcher-button:hover {"
        "  background-color: rgba(60, 55, 70, 0.3);"
        "  background-image: none;"
        "}"
        "window.panel button#launcher-button:active,"
        "window.panel button#launcher-button:checked {"
        "  background-color: rgba(205, 214, 244, 0.15);"
        "  border-bottom: 2px solid rgba(205, 214, 244, 0.3);"
        "}"

        /* Start-menu: a plain toplevel window with a vertical list of apps.
         * No popup machinery -> no grab, no zero-allocation draw races.
         * Keep the window node dead simple (no border/padding on a toplevel,
         * they can confuse child layout under the minimal stack). */
        "window#start-menu {"
        "  background-color: #1a1a22;"
        "}"
        "window#start-menu button#start-item {"
        "  color: rgb(205, 214, 244);"
        "  background-color: transparent;"
        "  background-image: none;"
        "  border: none;"
        "  border-radius: 4px;"
        "  box-shadow: none;"
        "  padding: 3px 6px;"
        "  min-height: 28px;"
        "  font-size: 12px;"
        "}"
        "window#start-menu button#start-item label {"
        "  font-size: 12px;"
        "}"
        "window#start-menu button#start-item image {"
        "  font-size: 12px;"
        "}"
        "window#start-menu button#start-item:hover {"
        "  background-color: rgba(90, 84, 120, 0.35);"
        "  background-image: none;"
        "}"
        "window#start-menu button#start-item:active {"
        "  background-color: rgba(205, 214, 244, 0.18);"
        "}"

        /* Taskbar application indicators. */
        "box#taskbar {"
        "  padding: 0 4px;"
        "}"
        "box#taskbar button#task-item {"
        "  background-color: transparent;"
        "  background-image: none;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 3px;"
        "  min-width: 30px;"
        "  min-height: 30px;"
        "  box-shadow: none;"
        "}"
        "box#taskbar button#task-item:hover {"
        "  background-color: rgba(205, 214, 244, 0.12);"
        "}"
        "box#taskbar button#task-item:active,"
        "box#taskbar button#task-item:checked {"
        "  background-color: rgba(205, 214, 244, 0.2);"
        "  border-top: 2px solid rgba(205, 214, 244, 0.45);"
        "}"
        "";

void style_apply(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    GdkScreen *screen = gdk_screen_get_default();

    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(screen,
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_object_unref(provider);
}
