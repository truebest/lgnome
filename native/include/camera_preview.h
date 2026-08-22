#ifndef GNOMECAST_CAMERA_PREVIEW_H
#define GNOMECAST_CAMERA_PREVIEW_H

/* Diagnostic local-camera mode (see camera_preview.c). SDL builds only. */
#ifdef HELLOLG_TARGET_WEBOS

#include <stdatomic.h>

#include <SDL.h>

/* Opens the first streaming V4L2 capture node that accepts 640x480 YUYV,
 * renders frames until BACK/app termination, and closes it. */
int native_camera_preview_run(SDL_Renderer *renderer, atomic_bool *running);

#endif
#endif
