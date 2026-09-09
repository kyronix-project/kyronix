#ifndef KYRONIX_ICONS_H
#define KYRONIX_ICONS_H

/* Maps a .desktop Icon= value to an absolute path in the Kyronix icon set.
 * Returns a static string; falls back to app-generic.png. */
const char *icon_path_for(const char *icon_name);

/* Absolute path to the launcher (menu) icon. */
const char *launcher_icon_path(void);

#endif