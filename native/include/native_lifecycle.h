#ifndef GNOMECAST_NATIVE_LIFECYCLE_H
#define GNOMECAST_NATIVE_LIFECYCLE_H

/* Process-level bring-up and teardown: webOS environment defaults, the stderr
 * log redirect, the fatal-signal/atexit hooks that release the DirectMedia
 * pipeline (see native_lifecycle.c for why), and the SDL runtime. No App
 * dependency; main() calls these around everything else. */

void native_prepare_webos_environment(void);
void native_prepare_webos_logging(void);
void native_install_termination_hooks(void);
/* Returns 0 on success, or a process exit code. Idempotent. */
int native_prepare_sdl_runtime(void);
void native_shutdown_sdl_runtime(void);

#endif
