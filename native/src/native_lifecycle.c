#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* XSI superset of the above for sigaltstack()/SA_ONSTACK. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "native_lifecycle.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef LGNOME_TARGET_WEBOS
#include <SDL.h>
#endif

#include "media_backend.h"

#include "clog.h"

clog_define(g_native_log_lifecycle, cLogLevelInfo, "native");

void native_prepare_webos_environment(void) {
    if (!getenv("EGL_PLATFORM") && setenv("EGL_PLATFORM", "wayland", 0) != 0) {
        clog(cLogLevelWarning, "failed to set EGL_PLATFORM=wayland: %s", strerror(errno));
    }
    if (!getenv("XDG_RUNTIME_DIR") && setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0) != 0) {
        clog(cLogLevelWarning, "failed to set XDG_RUNTIME_DIR=/tmp/xdg: %s", strerror(errno));
    }
    clog(cLogLevelDebug, "webOS env APPID=%s EGL_PLATFORM=%s XDG_RUNTIME_DIR=%s",
         getenv("APPID") ? getenv("APPID") : "(unset)",
         getenv("EGL_PLATFORM") ? getenv("EGL_PLATFORM") : "(unset)",
         getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)");
}

void native_prepare_webos_logging(void) {
    const char *path = getenv("LGNOME_NATIVE_LOG_PATH");
    if (!path || !path[0]) {
        path = "/tmp/lgnome-native.log";
    }
    if (freopen(path, "w", stderr)) {
        setvbuf(stderr, NULL, _IOLBF, 0);
    }
}

/* webOS keeps the DirectMedia pipeline acquired until NDL_DirectMediaQuit, so a
 * process that dies without it leaves the media server holding the resource and
 * the next launch can fail to init until the server notices the dead client.
 * The orderly teardown runs off SDL_QUIT / SDL_APP_TERMINATING (user close, and
 * system close via SDL's own SIGINT/SIGTERM handlers); the hooks below cover
 * the paths that bypass it: fatal signals and stray exit() calls. SIGINT and
 * SIGTERM are deliberately untouched — SDL only installs its graceful quit
 * handlers over SIG_DFL. */
static void native_fatal_signal_release(int sig) {
    /* NDL quit talks to the media server; if that hangs in a dying process, let
     * SIGALRM finish the termination instead of leaving a stuck card. */
    alarm(3);
    native_media_emergency_release();
    /* SA_RESETHAND restored the default disposition: die by the original signal
     * so webOS crash reporting still sees the real cause. */
    raise(sig);
}

static void native_release_media_at_exit(void) {
    native_media_emergency_release();
}

void native_install_termination_hooks(void) {
    /* A stack-overflow SIGSEGV cannot run the release handler on the exhausted
     * stack; give the main thread an alternate one. Worker threads keep their
     * own stacks (no per-thread sigaltstack) — best effort there. */
    static char fatal_stack[32768];
    stack_t stack_info;
    memset(&stack_info, 0, sizeof(stack_info));
    stack_info.ss_sp = fatal_stack;
    stack_info.ss_size = sizeof(fatal_stack);
    bool have_alt_stack = sigaltstack(&stack_info, NULL) == 0;

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = native_fatal_signal_release;
    action.sa_flags = SA_RESETHAND | SA_NODEFER | (have_alt_stack ? SA_ONSTACK : 0);
    sigemptyset(&action.sa_mask);
    static const int fatal_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE,
                                        SIGABRT, SIGHUP, SIGQUIT};
    int failed = 0;
    for (size_t i = 0; i < sizeof(fatal_signals) / sizeof(fatal_signals[0]); i++) {
        if (sigaction(fatal_signals[i], &action, NULL) != 0) {
            failed++;
        }
    }
    /* IronRDP is a Rust staticlib under a C main, so Rust's usual startup
     * SIGPIPE ignore is absent; a peer-dropped socket must surface as EPIPE on
     * the write, not terminate the process. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        failed++;
    }
    if (atexit(native_release_media_at_exit) != 0) {
        failed++;
    }
    if (!have_alt_stack || failed > 0) {
        clog(cLogLevelWarning,
             "termination hooks incomplete (alt-stack=%s, %d registrations failed); "
             "an abnormal exit may leave DirectMedia acquired",
             have_alt_stack ? "yes" : "no", failed);
    }
}

#ifdef LGNOME_TARGET_WEBOS
static bool g_sdl_runtime_initialized = false;

int native_prepare_sdl_runtime(void) {
    if (!g_sdl_runtime_initialized) {
        if (SDL_Init(0) != 0) {
            clog(cLogLevelError, "SDL_Init(0) failed: %s", SDL_GetError());
            return 4;
        }
        g_sdl_runtime_initialized = true;
    }

    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    /* The EXIT key stays with the system (no ACCESS_POLICY_KEYS_EXIT hint): webOS closes
     * the app itself and we shut down on the resulting SDL_QUIT/SDL_APP_TERMINATING. */
    /* LSM's pointer auto-hide (mrcu standby) timer. moonlight-tv uses 5000ms because the
     * game host draws its own cursor; for a remote desktop the pointer must not vanish —
     * once LSM detaches it, motion and button events stop being delivered entirely and
     * only wheel/keyboard input can re-summon it. Effectively disable the auto-hide;
     * hide-while-typing still works via the explicit SDL_webOSCursorVisibility calls. */
    SDL_SetHint("SDL_WEBOS_CURSOR_SLEEP_TIME", "86400000");
    SDL_SetHint("SDL_WEBOS_CURSOR_FREQUENCY", "60");
    SDL_SetHint("SDL_WEBOS_CURSOR_CALIBRATION_DISABLE", "true");
    SDL_SetHint("SDL_WEBOS_HIDAPI_IGNORE_BLUETOOTH_DEVICES", "0x057e/0x0000");
    return 0;
}

void native_shutdown_sdl_runtime(void) {
    if (g_sdl_runtime_initialized) {
        SDL_Quit();
        g_sdl_runtime_initialized = false;
    }
}
#else
int native_prepare_sdl_runtime(void) {
    return 0;
}

void native_shutdown_sdl_runtime(void) {
}
#endif
