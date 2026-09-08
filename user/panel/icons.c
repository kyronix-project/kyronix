#include "icons.h"

#include <glib.h>
#include <string.h>

#define ICON_DIR "/usr/share/icons/kyronix/24x24"

static const char *
lookup(const char *icon_name)
{
	if (icon_name == NULL)
		return NULL;

	if (strcmp(icon_name, "utilities-terminal") == 0)
		return "/usr/share/icons/kyronix/24x24/app-terminal.png";
	if (strcmp(icon_name, "shell") == 0)
		return "/usr/share/icons/kyronix/24x24/app-shell.png";
	if (strcmp(icon_name, "system-run") == 0)
		return "/usr/share/icons/kyronix/24x24/app-system-info.png";
	if (strcmp(icon_name, "system-reboot") == 0)
		return "/usr/share/icons/kyronix/24x24/app-reboot.png";

	return NULL;
}

const char *
icon_path_for(const char *icon_name)
{
	const char *path = lookup(icon_name);

	if (path != NULL && g_file_test(path, G_FILE_TEST_EXISTS))
		return path;

	return "/usr/share/icons/kyronix/24x24/app-generic.png";
}

const char *
launcher_icon_path(void)
{
	return ICON_DIR "/launcher.png";
}