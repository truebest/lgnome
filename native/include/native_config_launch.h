#ifndef LGNOME_NATIVE_CONFIG_LAUNCH_H
#define LGNOME_NATIVE_CONFIG_LAUNCH_H

#include <stdbool.h>

#include "native_settings.h"

/* Flat --name value CLI scanning shared with main(). */
const char *native_arg_value(int argc, char **argv, const char *name, const char *fallback);
bool native_arg_exists(int argc, char **argv, const char *name);

/* Apply webOS launch JSON and flat host CLI overrides. Flat settings target
 * the GREEN profile; launch-only controls never become persisted settings. */
bool native_config_apply_launch_params(NativeSettings *settings, int argc, char **argv);
bool native_config_apply_cli(NativeSettings *settings, int argc, char **argv);
bool native_config_launch_ignores_saved_config(int argc, char **argv);
bool native_camera_preview_requested(int argc, char **argv);
int native_connect_slot_requested(int argc, char **argv);

#endif
