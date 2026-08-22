/* Diagnostic local-camera preview: opens the first streaming V4L2 node,
 * renders frames until BACK/app termination. Split from camera_v4l2.c so the
 * device-capture header stays SDL-free. SDL builds only. */
#ifdef HELLOLG_TARGET_WEBOS

#include "camera_preview.h"

#include <SDL.h>

#include "camera_v4l2.h"

#include "clog.h"

clog_define(g_native_log_camera_preview, cLogLevelInfo, cLogFlags_Default, "camera.preview", NULL);

static bool native_camera_preview_event(const SDL_Event *event, atomic_bool *running) {
    switch (event->type) {
    case SDL_QUIT:
    case SDL_APP_TERMINATING:
    case SDL_APP_WILLENTERBACKGROUND:
    case SDL_APP_DIDENTERBACKGROUND:
        atomic_store(running, false);
        return false;
    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_CLOSE || event->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
            event->window.event == SDL_WINDOWEVENT_HIDDEN || event->window.event == SDL_WINDOWEVENT_MINIMIZED) {
            clog(cLogLevelInfo, "camera preview lost foreground ownership; closing capture");
            atomic_store(running, false);
            return false;
        }
        break;
    case SDL_KEYDOWN:
        if (event->key.keysym.scancode == 482 /* SDL_WEBOS_SCANCODE_BACK */ || event->key.keysym.sym == SDLK_ESCAPE) {
            clog(cLogLevelNotice, "BACK closes camera preview");
            atomic_store(running, false);
            return false;
        }
        break;
    default:
        break;
    }
    return true;
}

static bool native_camera_present(SDL_Renderer *renderer, SDL_Texture *texture, uint32_t frame_width,
                                  uint32_t frame_height) {
    int output_width = 0;
    int output_height = 0;
    if (SDL_GetRendererOutputSize(renderer, &output_width, &output_height) != 0 || output_width <= 0 ||
        output_height <= 0) {
        clog(cLogLevelError, "camera preview renderer size unavailable: %s", SDL_GetError());
        return false;
    }

    double scale_x = (double)output_width / frame_width;
    double scale_y = (double)output_height / frame_height;
    double scale = scale_x < scale_y ? scale_x : scale_y;
    SDL_Rect destination = {
        .w = (int)(frame_width * scale),
        .h = (int)(frame_height * scale),
    };
    destination.x = (output_width - destination.w) / 2;
    destination.y = (output_height - destination.h) / 2;

    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    if (SDL_RenderClear(renderer) != 0 || SDL_RenderCopy(renderer, texture, NULL, &destination) != 0) {
        clog(cLogLevelError, "camera preview render failed: %s", SDL_GetError());
        return false;
    }
    SDL_RenderPresent(renderer);
    return true;
}

int native_camera_preview_run(SDL_Renderer *renderer, atomic_bool *running) {
    if (!renderer || !running) {
        return 4;
    }

    NativeCameraCapture *capture = NULL;
    if (!native_camera_capture_open_yuyv_preview(640, 480, 15, &capture)) {
        return 5;
    }
    uint32_t width = native_camera_capture_width(capture);
    uint32_t height = native_camera_capture_height(capture);
    uint32_t pitch = native_camera_capture_pitch(capture);
    if ((size_t)width > SIZE_MAX / 4u || (size_t)height > SIZE_MAX / ((size_t)width * 4u)) {
        native_camera_capture_close(capture);
        return 5;
    }
    size_t rgba_len = (size_t)width * height * 4u;
    uint8_t *rgba = (uint8_t *)malloc(rgba_len);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)width,
                                             (int)height);
    if (!rgba || !texture) {
        clog(cLogLevelError, "camera preview allocation failed%s%s", texture ? "" : ": ",
             texture ? "" : SDL_GetError());
        free(rgba);
        if (texture) {
            SDL_DestroyTexture(texture);
        }
        native_camera_capture_close(capture);
        return 5;
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
    clog(cLogLevelInfo, "camera preview active; BACK closes it");

    bool received_frame = false;
    int result = 0;
    while (atomic_load(running)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            (void)native_camera_preview_event(&event, running);
        }
        if (!atomic_load(running)) {
            break;
        }

        NativeCameraFrame frame;
        NativeCameraAcquireResult acquire_result = native_camera_capture_acquire(capture, 50, &frame);
        if (acquire_result == NATIVE_CAMERA_ACQUIRE_TIMEOUT) {
            continue;
        }
        if (acquire_result != NATIVE_CAMERA_ACQUIRE_FRAME) {
            clog(cLogLevelError, "camera preview capture needs recovery");
            result = 5;
            break;
        }
        size_t frame_len = frame.len;
        bool frame_ok = native_camera_yuyv_to_rgba(frame.data, frame.len, width, height, pitch, rgba, rgba_len);
        native_camera_capture_release(capture, &frame);

        if (!frame_ok) {
            clog(cLogLevelError, "invalid camera frame (index=%u bytes=%u expected-at-least=%u)", 0u,
                 (unsigned)frame_len, pitch * height);
            result = 5;
        } else if (SDL_UpdateTexture(texture, NULL, rgba, (int)(width * 4u)) != 0) {
            clog(cLogLevelError, "camera texture update failed: %s", SDL_GetError());
            result = 5;
        } else if (!native_camera_present(renderer, texture, width, height)) {
            result = 5;
        } else if (!received_frame) {
            clog(cLogLevelNotice, "received and presented first camera frame (%u bytes)", (unsigned)frame_len);
            received_frame = true;
        }
        if (result != 0) {
            break;
        }
    }

    SDL_DestroyTexture(texture);
    free(rgba);
    native_camera_capture_close(capture);
    return result;
}

#endif
