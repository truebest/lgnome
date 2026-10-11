/* Use the real producer callback and pointer consumer with a controlled SDL cursor. */
#include "../src/native_rdp_callbacks.c"
#include "native_pointer.h"
#include <assert.h>
#include <sched.h>
#include <string.h>

static unsigned applies, warps;
static int last_x, last_y;
static unsigned key_releases, button_releases;
bool native_input_key(NativeInput *input, uint8_t scancode, bool down, bool extended) {
    (void)input; (void)extended; assert(scancode==0x2a && !down); key_releases++; return true;
}
bool native_input_pointer_button(NativeInput *input, int x, int y, NativeInputButton button, bool down) {
    (void)input; (void)x; (void)y; assert(button==NATIVE_INPUT_BUTTON_LEFT && !down); button_releases++; return true;
}
void native_cursor_apply(NativeCursor *cursor, uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    (void)cursor; (void)a; (void)b; (void)c; (void)d; applies++;
}
void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y) {
    (void)window;
    assert(x == y);
    last_x=x; last_y=y; warps++;
}
static void init(App *app) {
    memset(app,0,sizeof(*app));
    assert(pthread_mutex_init(&app->video_lock,NULL)==0);
    app->streaming_visible=true;
    atomic_store(&app->input.active,true);
    app->sessions[0].app=app; app->sessions[0].index=0;
    app->sessions[1].app=app; app->sessions[1].index=1;
    atomic_store(&app->input.desktop_width,1024); atomic_store(&app->input.desktop_height,1024);
    atomic_store(&app->input.window_width,1024); atomic_store(&app->input.window_height,1024);
    applies=warps=0;
}
static void test_local_pointer_ownership(void) {
    App app; SDL_Window window={0}; init(&app);
    bool *blocked[]={&app.mixer_overlay_visible,&app.window_unfocused,&app.app_backgrounded};
    for (size_t i=0;i<3;i++) {
        *blocked[i]=true;
        on_pointer_position(&app.sessions[0],100,100);
        native_cursor_tick(&app); native_drain_pointer_warp(&app,&window);
        assert(applies==0 && warps==0 && !atomic_load(&app.pointer_warp_pending));
        *blocked[i]=false;
    }
    app.streaming_visible=false;
    on_pointer_position(&app.sessions[0],100,100); native_cursor_tick(&app); native_drain_pointer_warp(&app,&window);
    assert(applies==0 && warps==0);
    app.streaming_visible=true;
    on_pointer_position(&app.sessions[1],200,200); native_drain_pointer_warp(&app,&window);
    assert(warps==0);
    on_pointer_position(&app.sessions[0],300,300); native_cursor_tick(&app); native_drain_pointer_warp(&app,&window);
    assert(applies==1 && warps==1 && last_x==300 && last_y==300);
    assert(pthread_mutex_destroy(&app.video_lock)==0);
}
typedef struct Producer { App *app; atomic_bool done; } Producer;
static void *publish_positions(void *ctx) {
    Producer *producer=ctx;
    for (unsigned i=0;i<100000;i++) {
        uint16_t value=(uint16_t)(100+i%400);
        on_pointer_position(&producer->app->sessions[0],value,value);
    }
    atomic_store(&producer->done,true);
    return NULL;
}
static void test_concurrent_positions_stay_coherent(void) {
    App app; SDL_Window window={0}; init(&app);
    Producer producer={.app=&app}; atomic_init(&producer.done,false);
    pthread_t thread; assert(pthread_create(&thread,NULL,publish_positions,&producer)==0);
    while (!atomic_load(&producer.done)) { native_drain_pointer_warp(&app,&window); sched_yield(); }
    assert(pthread_join(thread,NULL)==0); native_drain_pointer_warp(&app,&window);
    assert(warps>0 && last_x==last_y);
    assert(pthread_mutex_destroy(&app.video_lock)==0);
}
static void test_reset_flushes_held_remote_input(void) {
    App app; init(&app);
    native_track_key(&app,0x2a,false,true);
    native_track_button(&app,NATIVE_INPUT_BUTTON_LEFT,true);
    native_flush_held_inputs(&app);
    assert(key_releases==1 && button_releases==1 && !app.held_mouse[0]);
    native_flush_held_inputs(&app);
    assert(key_releases==1 && button_releases==1);
    assert(pthread_mutex_destroy(&app.video_lock)==0);
}

static void test_positions_while_input_disarmed_are_not_replayed(void) {
    App app; SDL_Window window={0}; init(&app);
    atomic_store(&app.input.active,false);
    app.streaming_visible=false;
    on_pointer_position(&app.sessions[0],100,100);
    app.streaming_visible=true; atomic_store(&app.input.active,true);
    native_drain_pointer_warp(&app,&window);
    assert(warps==0);
    app.mixer_overlay_visible=true; atomic_store(&app.input.active,false);
    on_pointer_position(&app.sessions[0],200,200);
    app.mixer_overlay_visible=false; atomic_store(&app.input.active,true);
    native_drain_pointer_warp(&app,&window);
    assert(warps==0);
    assert(pthread_mutex_destroy(&app.video_lock)==0);
}
int main(void) {
    test_local_pointer_ownership(); test_concurrent_positions_stay_coherent(); test_reset_flushes_held_remote_input();
    test_positions_while_input_disarmed_are_not_replayed();
    return 0;
}
