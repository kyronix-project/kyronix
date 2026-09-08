#ifndef KYRONIX_DE_H
#define KYRONIX_DE_H

#include "menu.h"

/*
 * Minimal .desktop entry parser (freedesktop Desktop Entry Specification,
 * the subset relevant for launchers). Loads [Desktop Entry] blocks, reads
 * Name/Icon/Exec, and appends them to a NULL-terminated array.
 *
 * Returns the number of entries loaded, or 0 on error.
 */
int de_load_dir(LauncherEntry ***out, const char *dir);

/* Frees a single entry previously returned by de_load_dir. */
void launcher_entry_free(LauncherEntry *e);

/* Deep-copies an entry (caller frees with launcher_entry_free). */
LauncherEntry *launcher_entry_clone(const LauncherEntry *e);

#endif
