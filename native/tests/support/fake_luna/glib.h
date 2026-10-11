#ifndef LGNOME_TEST_GLIB_H
#define LGNOME_TEST_GLIB_H
#include <stdint.h>
typedef int gboolean;
typedef void *gpointer;
typedef int64_t gint64;
typedef struct GMainContext GMainContext;
typedef struct GMainLoop GMainLoop;
typedef struct GSource GSource;
typedef gboolean (*GSourceFunc)(gpointer);
#define FALSE 0
#define G_SOURCE_REMOVE 0
#define G_USEC_PER_SEC 1000000
GMainContext *g_main_context_new(void);
void g_main_context_push_thread_default(GMainContext *context);
void g_main_context_pop_thread_default(GMainContext *context);
void g_main_context_unref(GMainContext *context);
GMainLoop *g_main_loop_new(GMainContext *context, gboolean running);
void g_main_loop_run(GMainLoop *loop);
void g_main_loop_quit(GMainLoop *loop);
void g_main_loop_unref(GMainLoop *loop);
GSource *g_idle_source_new(void);
void g_source_set_callback(GSource *source, GSourceFunc callback, gpointer data, void (*notify)(gpointer));
unsigned g_source_attach(GSource *source, GMainContext *context);
void g_source_unref(GSource *source);
void g_usleep(unsigned long microseconds);
gint64 g_get_monotonic_time(void);
#endif
