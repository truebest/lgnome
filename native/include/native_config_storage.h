#ifndef GNOMECAST_NATIVE_CONFIG_STORAGE_H
#define GNOMECAST_NATIVE_CONFIG_STORAGE_H

#include <stdbool.h>

#include "native_settings.h"

/* Explicit config file and persisted on-TV settings acquisition. */
bool native_config_load_file(NativeSettings *settings, const char *path, bool required);
bool native_config_load_persisted(NativeSettings *settings, bool force_ignore);
bool native_config_save_persisted(const NativeSettings *settings);

/* Atomic save (0600 temp file + rename) to one already-selected path. */
bool native_settings_save_file(const NativeSettings *settings, const char *path);

#endif
