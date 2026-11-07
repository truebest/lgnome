#include <assert.h>
#include <glib-object.h>
#include <stdarg.h>
#include <string.h>

typedef struct { GObject parent; char *app_type; guint marker; } ProbeObject;
static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    (void)spec;
    ProbeObject *probe = (ProbeObject *)object;
    if (id == 1) { g_free(probe->app_type); probe->app_type = g_value_dup_string(value); }
    else if (id == 2) probe->marker = g_value_get_uint(value);
    else assert(0);
}
static void finalize(GObject *object)
{
    g_free(((ProbeObject *)object)->app_type);
    G_OBJECT_CLASS(g_type_class_peek(G_TYPE_OBJECT))->finalize(object);
}
static void class_init(gpointer klass, gpointer data)
{
    (void)data;
    GObjectClass *object = klass;
    object->set_property = set_property;
    object->finalize = finalize;
    g_object_class_install_property(object, 1, g_param_spec_string("app-type", "type", "type", "", G_PARAM_WRITABLE));
    g_object_class_install_property(object, 2, g_param_spec_uint("marker", "marker", "marker", 0, 100, 0, G_PARAM_WRITABLE));
}
static void assign(GObject *object, const char *first, ...)
{
    va_list args;
    va_start(args, first);
    g_object_set_valist(object, first, args);
    va_end(args);
}
int main(void)
{
    g_unsetenv("PROBE_OMX_NORMAL_DELAY");
    g_unsetenv("PROBE_OMX_APP_TYPE");
    const char *names[] = {"GstOMXH264DecBin", "GstRvSinkBin", "UnrelatedProbeObject"};
    for (unsigned i = 0; i < G_N_ELEMENTS(names); ++i) {
        GType type = g_type_register_static_simple(G_TYPE_OBJECT, names[i], sizeof(GObjectClass),
                                                   class_init, sizeof(ProbeObject), NULL, 0);
        ProbeObject *object = g_object_new(type, NULL);
        assign(G_OBJECT(object), "app-type", "DIRECTMEDIA", "marker", 42u, NULL);
        assert(!strcmp(object->app_type, i < 2 ? "RTC" : "DIRECTMEDIA"));
        assert(object->marker == 42);
        const char *modes[] = {"RTC", "MIRACAST", "APPCAST", "AIRPLAY"};
        for (unsigned m = 0; m < G_N_ELEMENTS(modes); m++) {
            assert(g_setenv("PROBE_OMX_APP_TYPE", modes[m], TRUE));
            assign(G_OBJECT(object), "app-type", "DIRECTMEDIA", "marker", 43u, NULL);
            assert(!strcmp(object->app_type, i < 2 ? modes[m] : "DIRECTMEDIA"));
            assert(object->marker == 43);
        }
        g_unsetenv("PROBE_OMX_APP_TYPE");
        assign(G_OBJECT(object), "app-type", "OTHER", NULL);
        assert(!strcmp(object->app_type, "OTHER"));
        assign(G_OBJECT(object), "marker", 7u, NULL);
        assert(object->marker == 7);
        assert(g_setenv("PROBE_OMX_NORMAL_DELAY", "1", TRUE));
        assign(G_OBJECT(object), "app-type", "DIRECTMEDIA", NULL);
        assert(!strcmp(object->app_type, i < 2 ? "" : "DIRECTMEDIA"));
        g_unsetenv("PROBE_OMX_NORMAL_DELAY");
        g_object_unref(object);
    }
    return 0;
}
