#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib-object.h>
#include "clog.h"

clog_define(omx_probe_log, cLogLevelInfo, "avc444.omx-probe");

void g_object_set_valist(GObject *object, const gchar *first, va_list args)
{
    void (*original)(GObject *, const gchar *, va_list) = dlsym(RTLD_NEXT, "g_object_set_valist");
    if (!original) _exit(125);
    const char *type = G_OBJECT_TYPE_NAME(object);
    if (first && !strcmp(first, "app-type") &&
        (!strcmp(type, "GstOMXH264DecBin") || !strcmp(type, "GstRvSinkBin"))) {
        va_list copy;
        va_copy(copy, args);
        const char *value = va_arg(copy, const char *);
        if (value && !strcmp(value, "DIRECTMEDIA")) {
            GValue replacement = G_VALUE_INIT;
            g_value_init(&replacement, G_TYPE_STRING);
            const char *normal_delay = getenv("PROBE_OMX_NORMAL_DELAY");
            const char *mode = normal_delay && !strcmp(normal_delay, "1") ? "" : "RTC";
            const char *requested = getenv("PROBE_OMX_APP_TYPE");
            if (requested) {
                if (strcmp(requested, "RTC") && strcmp(requested, "MIRACAST") &&
                    strcmp(requested, "APPCAST") && strcmp(requested, "AIRPLAY"))
                    _exit(2);
                mode = requested;
            }
            g_value_set_static_string(&replacement, mode);
            g_object_set_property(object, first, &replacement);
            g_value_unset(&replacement);
            const char *dma = getenv("PROBE_OMX_DMABUF");
            if (dma && !strcmp(dma, "1") && !strcmp(type, "GstOMXH264DecBin")) {
                g_object_set(object, "output-mode", "dmabuf", "render-mode", "GRAPHIC_MODE", NULL);
                clog(cLogLevelInfo, "requested decoder DMA-BUF graphic output");
            }
            const char *next = va_arg(copy, const char *);
            if (next) original(object, next, copy);
            va_end(copy);
            clog(cLogLevelInfo, "selected OMX app-type=%s for %s; resource assignment unchanged", mode, type);
            return;
        }
        va_end(copy);
    }
    original(object, first, args);
}
