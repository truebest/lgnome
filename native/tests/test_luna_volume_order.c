/* Exercise the real coalescing/callback path with a controlled LS2 transport. */
#include "../src/luna_volume.c"
#include <assert.h>

typedef struct Call { LSFilterFunc reply; void *ctx; unsigned token; bool set; } Call;
static Call calls[16];
static unsigned call_count;
void LSErrorInit(LSError *error) { error->message=NULL; }
void LSErrorFree(LSError *error) { (void)error; }
bool LSCallCancel(LSHandle *handle, LSMessageToken token, LSError *error) {
    (void)handle; (void)token; (void)error; return true;
}
bool LSCallOneReply(LSHandle *handle, const char *uri, const char *payload, LSFilterFunc callback,
                    void *ctx, LSMessageToken *token, LSError *error) {
    (void)handle; (void)payload; (void)error; assert(call_count<16);
    *token=call_count+1;
    calls[call_count++]=(Call){callback,ctx,*token,strstr(uri,"setVolume")!=NULL};
    return true;
}
LSMessageToken LSMessageGetResponseToken(LSMessage *message) { return message->token; }
const char *LSMessageGetPayload(LSMessage *message) { return message->payload; }
gint64 g_get_monotonic_time(void) { return 1000000; }
GSource *g_idle_source_new(void) { assert(false); return NULL; }
void g_source_set_callback(GSource *source, GSourceFunc callback, gpointer data, void (*notify)(gpointer)) {
    (void)source; (void)callback; (void)data; (void)notify; assert(false);
}
unsigned g_source_attach(GSource *source, GMainContext *context) { (void)source; (void)context; assert(false); return 0; }
void g_source_unref(GSource *source) { (void)source; assert(false); }
static void reply(unsigned index,const char *json) {
    assert(index<call_count); Call *call=&calls[index]; LSMessage message={call->token,json};
    assert(call->reply(NULL,&message,call->ctx));
}

static void test_old_get(bool after_set_reply) {
    call_count=0;
    NativeLunaVolume lv={0}; LSHandle handle={0};
    NativeLunaVolumeImpl impl={.owner=&lv,.handle=&handle}; lv.impl=&impl;
    atomic_init(&impl.desired_pct,-1); atomic_init(&impl.loop_ready,false); atomic_init(&impl.invoke_queued,false);
    assert(pthread_mutex_init(&impl.cache_lock,NULL)==0);
    atomic_store(&lv.volume_pct,30);
    luna_volume_refresh_cb(&impl); assert(call_count==1 && !calls[0].set);
    native_luna_volume_set(&lv,33); luna_volume_dispatch_cb(&impl);
    native_luna_volume_set(&lv,36);
    if (after_set_reply) {
        reply(1,"{\"returnValue\":true}"); assert(call_count==3 && calls[2].set);
        reply(2,"{\"returnValue\":true}");
    }
    reply(0,"{\"returnValue\":true,\"volume\":30}");
    assert(native_luna_volume_cached(&lv)==36 && native_luna_volume_reply_seq(&lv)==0);
    native_luna_volume_set(&lv,native_luna_volume_cached(&lv)+3);
    assert(native_luna_volume_cached(&lv)==39);
    if (!after_set_reply) { reply(1,"{\"returnValue\":true}"); reply(2,"{\"returnValue\":true}"); }
    else { luna_volume_dispatch_cb(&impl); reply(3,"{\"returnValue\":true}"); }
    luna_volume_refresh_cb(&impl); assert(!calls[call_count-1].set);
    reply(call_count-1,"{\"returnValue\":true,\"volumeStatus\":{\"volume\":39,\"muteStatus\":true}}");
    assert(native_luna_volume_cached(&lv)==39 && atomic_load(&lv.muted));
    assert(native_luna_volume_reply_seq(&lv)==1);
    assert(pthread_mutex_destroy(&impl.cache_lock)==0);
}
static void test_rejected_set_can_reconcile_with_fresh_get(void) {
    call_count=0;
    NativeLunaVolume lv={0}; LSHandle handle={0};
    NativeLunaVolumeImpl impl={.owner=&lv,.handle=&handle}; lv.impl=&impl;
    atomic_init(&impl.desired_pct,-1); atomic_init(&impl.loop_ready,false); atomic_init(&impl.invoke_queued,false);
    assert(pthread_mutex_init(&impl.cache_lock,NULL)==0);
    native_luna_volume_set(&lv,36); luna_volume_dispatch_cb(&impl);
    reply(0,"{\"returnValue\":false}");
    luna_volume_refresh_cb(&impl); reply(1,"{\"returnValue\":true,\"volume\":30}");
    assert(native_luna_volume_cached(&lv)==30 && native_luna_volume_available(&lv));
    assert(pthread_mutex_destroy(&impl.cache_lock)==0);
}
int main(void) {
    test_old_get(false); test_old_get(true); test_rejected_set_can_reconcile_with_fresh_get();
    return 0;
}
