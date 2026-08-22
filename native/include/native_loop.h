#ifndef GNOMECAST_NATIVE_LOOP_H
#define GNOMECAST_NATIVE_LOOP_H

/* The app loop (see native_loop.c). Returns the process exit code. */

typedef struct App App;
typedef struct NativeSettings NativeSettings;

int native_run_app_loop(App *app, NativeSettings *settings);

#endif
