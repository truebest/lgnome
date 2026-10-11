#include "../src/ui_preconnect_display.c"
#include "../src/ui_preconnect_forms.c"
#define clog_private_file_category ui_profile_file_category
#include "../src/ui_preconnect_profile.c"
#undef clog_private_file_category
#include "draw/sw/lv_draw_sw.h"
#include <assert.h>
#include <stdio.h>
static SDL_Event queued[64];
static int n;
static Uint32 now_ticks;
Uint32 SDL_GetTicks(void) { return now_ticks; }
int SDL_PeepEvents(SDL_Event *events, int max, SDL_eventaction action, Uint32 lo, Uint32 hi) {
    int out = 0;
    for (int i=0; i<n && out<max;) {
        if (queued[i].type>=lo && queued[i].type<=hi) {
            events[out++] = queued[i];
            if (action==SDL_GETEVENT) { memmove(queued+i, queued+i+1, (n-i-1)*sizeof(*queued)); n--; continue; }
        }
        i++;
    }
    return out;
}
SDL_bool SDL_HasEvent(Uint32 type) { for(int i=0; i<n; i++) if(queued[i].type==type) return SDL_TRUE; return SDL_FALSE; }
void native_ui_preconnect_back(NativePreconnectUi *ui) { (void)ui; }
void lv_draw_sdl_init_ctx(lv_disp_drv_t *drv, lv_draw_ctx_t *ctx) { lv_draw_sw_init_ctx(drv, ctx); }
void lv_draw_sdl_deinit_ctx(lv_disp_drv_t *drv, lv_draw_ctx_t *ctx) { lv_draw_sw_deinit_ctx(drv, ctx); }
static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *buf) { (void)area; (void)buf; lv_disp_flush_ready(drv); }
static void test_click_then_type(void) {
    lv_init();
    NativePreconnectUi ui={0};
    static lv_color_t pixels[800*480];
    static lv_disp_draw_buf_t buf;
    lv_disp_draw_buf_init(&buf,pixels,NULL,800*480);
    static lv_disp_drv_t disp;
    lv_disp_drv_init(&disp); disp.draw_buf=&buf; disp.hor_res=800; disp.ver_res=480; disp.flush_cb=flush;
    assert(lv_disp_drv_register(&disp));
    lv_obj_t *first=lv_textarea_create(lv_scr_act()), *second=lv_textarea_create(lv_scr_act());
    lv_obj_set_pos(first,20,20); lv_obj_set_size(first,300,70); lv_textarea_set_text(first,"");
    lv_obj_set_pos(second,20,150); lv_obj_set_size(second,300,70); lv_textarea_set_text(second,"");
    lv_obj_update_layout(lv_scr_act());
    lv_group_t *group=lv_group_create(); lv_group_add_obj(group,first); lv_group_add_obj(group,second); lv_group_focus_obj(first);
    lv_indev_drv_init(&ui.pointer_drv); ui.pointer_drv.type=LV_INDEV_TYPE_POINTER;
    ui.pointer_drv.user_data=&ui; ui.pointer_drv.read_cb=native_ui_preconnect_pointer_read;
    ui.pointer_indev=lv_indev_drv_register(&ui.pointer_drv); lv_indev_set_group(ui.pointer_indev,group);
    lv_indev_drv_init(&ui.key_drv.base); ui.key_drv.base.type=LV_INDEV_TYPE_KEYPAD;
    ui.key_drv.base.user_data=&ui; ui.key_drv.base.read_cb=native_ui_preconnect_key_read;
    ui.key_indev=lv_indev_drv_register(&ui.key_drv.base); lv_indev_set_group(ui.key_indev,group);
    queued[0].type=SDL_MOUSEBUTTONDOWN; queued[0].button.button=SDL_BUTTON_LEFT; queued[0].button.x=50; queued[0].button.y=170;
    queued[1].type=SDL_MOUSEBUTTONUP; queued[1].button.button=SDL_BUTTON_LEFT; queued[1].button.x=50; queued[1].button.y=170;
    queued[2].type=SDL_TEXTINPUT; strcpy(queued[2].text.text,"A"); n=3;
    now_ticks=5;
    lv_timer_handler();
    now_ticks += 5;
    lv_timer_handler();
    assert(!lv_textarea_get_text(first)[0] && strcmp(lv_textarea_get_text(second), "A") == 0);
    /* Reverse order must preserve the original focus for the text. */
    lv_textarea_set_text(first, ""); lv_textarea_set_text(second, ""); lv_group_focus_obj(first);
    SDL_Event down = queued[0];
    down.type=SDL_MOUSEBUTTONDOWN; down.button.button=SDL_BUTTON_LEFT; down.button.x=50; down.button.y=170;
    queued[0]=(SDL_Event){.type=SDL_TEXTINPUT}; strcpy(queued[0].text.text,"B");
    queued[1]=down; queued[2]=down; queued[2].type=SDL_MOUSEBUTTONUP; n=3;
    now_ticks += 5; lv_timer_handler(); now_ticks += 5; lv_timer_handler();
    assert(strcmp(lv_textarea_get_text(first), "B") == 0 && !lv_textarea_get_text(second)[0]);
    lv_indev_delete(ui.key_indev); lv_indev_delete(ui.pointer_indev);
    lv_group_del(group); lv_obj_del(first); lv_obj_del(second);
}

static void test_keypad_and_text_deduplication(void) {
    NativePreconnectUi ui={0}; ui.key_drv.base.user_data=&ui;
    for (int digit=0; digit<10; digit++) {
        queued[0]=(SDL_Event){.type=SDL_KEYDOWN};
        queued[0].key.keysym.sym=digit ? SDLK_KP_1+digit-1 : SDLK_KP_0;
        queued[0].key.keysym.mod=KMOD_NUM; n=1;
        lv_indev_data_t data={0}; native_ui_preconnect_key_read(&ui.key_drv.base,&data);
        assert(data.key==(uint32_t)('0'+digit) && data.state==LV_INDEV_STATE_PRESSED);
        native_ui_preconnect_key_read(&ui.key_drv.base,&data);
        assert(data.state==LV_INDEV_STATE_RELEASED);
    }
    queued[0]=(SDL_Event){.type=SDL_KEYDOWN}; queued[0].key.keysym.sym=SDLK_KP_1;
    queued[1]=(SDL_Event){.type=SDL_TEXTINPUT}; strcpy(queued[1].text.text,"1"); n=2;
    lv_indev_data_t data={0}; native_ui_preconnect_key_read(&ui.key_drv.base,&data);
    assert(data.key=='1' && data.state==LV_INDEV_STATE_PRESSED);
    native_ui_preconnect_key_read(&ui.key_drv.base,&data);
    assert(data.state==LV_INDEV_STATE_RELEASED && !data.continue_reading);
}

static void test_credentials_byte_limits(void) {
    NativePreconnectUi ui={0};
    ui.name_input=lv_textarea_create(lv_scr_act()); ui.host_input=lv_textarea_create(lv_scr_act());
    ui.port_input=lv_textarea_create(lv_scr_act()); ui.username_input=lv_textarea_create(lv_scr_act());
    ui.domain_input=lv_textarea_create(lv_scr_act()); ui.password_input=lv_textarea_create(lv_scr_act());
    lv_textarea_set_max_length(ui.username_input,UI_USERNAME_MAX-1);
    lv_textarea_set_max_length(ui.domain_input,UI_DOMAIN_MAX-1);
    lv_textarea_set_max_length(ui.password_input,UI_PASSWORD_TEXT_MAX);
    lv_textarea_set_text(ui.host_input,"host.local"); lv_textarea_set_text(ui.port_input,"3389");
    lv_textarea_set_text(ui.username_input,"user"); lv_textarea_set_text(ui.domain_input,"");
    strcpy(ui.slot_values[0].password,"saved password");
    char text[1025];
    for (int i=0;i<512;i++) { text[i*2]=(char)0xc3; text[i*2+1]=(char)0xa9; } text[1024]=0;
    text[512]=0; lv_textarea_set_text(ui.password_input,text);
    assert(!ui_form_valid(&ui)); ui_store_form_to_slot(&ui,0);
    assert(strcmp(ui.slot_values[0].password,"saved password")==0);
    text[510]=0; lv_textarea_set_text(ui.password_input,text); assert(ui_form_valid(&ui));
    lv_textarea_set_text(ui.username_input,text); assert(!ui_form_valid(&ui));
    lv_textarea_set_text(ui.username_input,"user"); lv_textarea_set_text(ui.domain_input,text); assert(!ui_form_valid(&ui));
    lv_obj_del(ui.name_input); lv_obj_del(ui.host_input); lv_obj_del(ui.port_input);
    lv_obj_del(ui.username_input); lv_obj_del(ui.domain_input); lv_obj_del(ui.password_input);
}

int main(void) {
    test_click_then_type(); test_keypad_and_text_deduplication(); test_credentials_byte_limits();
    return 0;
}

void native_ui_mixer_set_profiles(NativeUiMixer *mixer, const NativeSessionConfig *values) { (void)mixer; (void)values; }
