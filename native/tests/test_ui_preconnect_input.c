#include "../src/ui_preconnect_display.c"
#include "../src/ui_preconnect_forms.c"
#define clog_private_file_category ui_profile_file_category
#include "../src/ui_preconnect_profile.c"
#undef clog_private_file_category
#include "../src/ui_preconnect_statistics.c"
#include "../src/ui_preconnect_statistics_tabs.c"
#include "draw/sw/lv_draw_sw.h"
#include <stdlib.h>
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
void native_ui_preconnect_screen_refocus(NativePreconnectUi *ui) { (void)ui; }
void native_ui_preconnect_form_key_event(lv_event_t *event) { (void)event; }
void lv_draw_sdl_init_ctx(lv_disp_drv_t *drv, lv_draw_ctx_t *ctx) { lv_draw_sw_init_ctx(drv, ctx); }
void lv_draw_sdl_deinit_ctx(lv_disp_drv_t *drv, lv_draw_ctx_t *ctx) { lv_draw_sw_deinit_ctx(drv, ctx); }
static unsigned char preview[1080][1920][3];
static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *buf) {
    if (getenv("LGNOME_STATS_PREVIEW_DIR")) {
        for (int y = area->y1; y <= area->y2; y++) for (int x = area->x1; x <= area->x2; x++) {
            lv_color32_t c = {.full = lv_color_to32(*buf++)};
            if (x >= 0 && x < 1920 && y >= 0 && y < 1080) {
                preview[y][x][0] = c.ch.red; preview[y][x][1] = c.ch.green; preview[y][x][2] = c.ch.blue;
            }
        }
    }
    lv_disp_flush_ready(drv);
}
static void statistics_preview(unsigned tab, unsigned category, bool help) {
    const char *dir = getenv("LGNOME_STATS_PREVIEW_DIR");
    if (!dir) return;
    char path[512]; snprintf(path, sizeof(path), "%s/tab-%u-category-%u%s.ppm", dir, tab, category, help ? "-help" : "");
    FILE *f = fopen(path, "wb"); assert(f);
    fprintf(f, "P6\n1920 1080\n255\n");
    assert(fwrite(preview, 1, sizeof(preview), f) == sizeof(preview)); fclose(f);
}
static void test_click_then_type(void) {
    lv_init();
    NativePreconnectUi ui={0};
    static lv_color_t pixels[800*480];
    static lv_disp_draw_buf_t buf;
    lv_disp_draw_buf_init(&buf,pixels,NULL,800*480);
    static lv_disp_drv_t disp;
    lv_disp_drv_init(&disp); disp.draw_buf=&buf; disp.hor_res=UI_CANVAS_WIDTH; disp.ver_res=UI_CANVAS_HEIGHT; disp.flush_cb=flush;
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
    lv_textarea_set_text(ui.domain_input, "");
    ui.desktop_scale_dropdown = lv_dropdown_create(lv_scr_act());
    lv_dropdown_set_options_static(ui.desktop_scale_dropdown, "100%\n150%\n200%");
    ui.selected_desktop_width = 3840;
    ui.selected_desktop_height = 2160;
    for (uint16_t index = 0; index < 3; index++) {
        lv_dropdown_set_selected(ui.desktop_scale_dropdown, index);
        NativeSessionConfig saved;
        assert(native_preconnect_ui_get_slot_values(&ui, 0, &saved));
        assert(saved.desktop_scale_factor == 100 + 50 * index);
        assert(saved.desktop_width == 3840 && saved.desktop_height == 2160);
    }
    lv_obj_del(ui.desktop_scale_dropdown);
    lv_obj_del(ui.name_input); lv_obj_del(ui.host_input); lv_obj_del(ui.port_input);
    lv_obj_del(ui.username_input); lv_obj_del(ui.domain_input); lv_obj_del(ui.password_input);
}

static int statistics_presents;
int SDL_SetRenderTarget(SDL_Renderer *r, SDL_Texture *t) { (void)r; (void)t; return 0; }
int SDL_SetRenderDrawBlendMode(SDL_Renderer *r, int mode) { (void)r; (void)mode; return 0; }
int SDL_SetRenderDrawColor(SDL_Renderer *r, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    (void)r; (void)a; (void)b; (void)c; (void)d; return 0;
}
int SDL_RenderClear(SDL_Renderer *r) { (void)r; return 0; }
int SDL_SetTextureBlendMode(SDL_Texture *t, int mode) { (void)t; (void)mode; return 0; }
int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *a, const SDL_Rect *b) {
    (void)r; (void)t; (void)a; (void)b; return 0;
}
void SDL_RenderPresent(SDL_Renderer *r) { (void)r; statistics_presents++; }

static void statistics_press(NativePreconnectUi *ui, NativeStatisticsKey key) {
    bool hub;
    assert(native_preconnect_ui_statistics_key(ui, key, true, false, &hub));
    assert(native_preconnect_ui_statistics_key(ui, key, false, false, &hub));
}

static void statistics_check_labels(lv_obj_t *object) {
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); i++) {
        lv_obj_t *child = lv_obj_get_child(object, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_check_type(child, &lv_label_class)) {
            lv_point_t size;
            lv_txt_get_size(&size, lv_label_get_text(child), lv_obj_get_style_text_font(child, 0),
                0, 0, lv_obj_get_content_width(child), LV_TEXT_FLAG_NONE);
            if (size.y > lv_obj_get_height(child)) {
                fprintf(stderr, "Clipped text (%d > %d): %s\n", size.y, lv_obj_get_height(child), lv_label_get_text(child));
                assert(false);
            }
        }
        lv_area_t rect, bounds; lv_obj_get_coords(child, &rect); lv_obj_get_coords(object, &bounds);
        if (rect.x1 < bounds.x1 || rect.x2 > bounds.x2 || rect.y1 < bounds.y1 || rect.y2 > bounds.y2)
            fprintf(stderr, "Bounds child %d,%d-%d,%d parent %d,%d-%d,%d: %s\n", rect.x1, rect.y1, rect.x2, rect.y2, bounds.x1, bounds.y1, bounds.x2, bounds.y2, lv_obj_check_type(child, &lv_label_class) ? lv_label_get_text(child) : "container");
        assert(rect.x1 >= bounds.x1 && rect.x2 <= bounds.x2);
        assert(rect.y1 >= bounds.y1 && rect.y2 <= bounds.y2);
        statistics_check_labels(child);
    }
}

static void statistics_check_zero_line(lv_event_t *event) {
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(event);
    if (!d || d->type != LV_CHART_DRAW_PART_LINE_AND_POINT || d->value != 0 ||
        !d->p1 || !d->p2 || d->p1->y != d->p2->y) return;
    const lv_area_t *clip = d->draw_ctx->clip_area;
    assert(d->p1->y >= clip->y1);
    assert(d->p1->y + d->line_dsc->width - 1 <= clip->y2);
    unsigned *segments = lv_event_get_user_data(event);
    (*segments)++;
}

static void test_statistics_layout_and_noninteractive_refresh(void) {
    NativePreconnectUi ui = {0};
    static lv_color_t pixels[1920 * 1080];
    static lv_disp_draw_buf_t buf;
    lv_disp_draw_buf_init(&buf, pixels, NULL, 1920 * 1080);
    lv_disp_drv_t drv; lv_disp_drv_init(&drv);
    drv.draw_buf = &buf; drv.hor_res = 1920; drv.ver_res = 1080; drv.flush_cb = flush;
    ui.disp = lv_disp_drv_register(&drv); assert(ui.disp); lv_disp_set_default(ui.disp);
    ui.statistics_btn = lv_btn_create(lv_scr_act()); lv_label_create(ui.statistics_btn);
    lv_obj_add_event_cb(ui.statistics_btn, ui_statistics_clicked, LV_EVENT_CLICKED, &ui);
    ui_statistics_build(&ui);
    assert(!native_preconnect_ui_statistics_enabled(&ui));
    assert(strcmp(lv_label_get_text(lv_obj_get_child(ui.statistics_btn, 0)), "Stats: Off") == 0);
    lv_event_send(ui.statistics_btn, LV_EVENT_CLICKED, NULL);
    assert(native_preconnect_ui_statistics_enabled(&ui));
    assert(strcmp(lv_label_get_text(lv_obj_get_child(ui.statistics_btn, 0)), "Stats: On") == 0);
    lv_obj_add_flag(ui.statistics_btn, LV_OBJ_FLAG_HIDDEN);
    assert(native_preconnect_ui_statistics_visible(&ui, true));
    NativeDiagnosticsSnapshot s = {.slot = 1, .width = 3840, .height = 2160, .at_ms = 1000,
        .connected = true, .connection_status = "Active", .session_uptime_valid = true, .session_uptime_s = 3661,
        .video_available = true, .status = "Active", .cpu_valid = true, .cpu_percent = 18,
        .memory_valid = true, .rss_bytes = 60000000, .counters = {.has_au = true, .last_au_ms = 999},
        .rates = {.valid = true, .input_aus = 120, .input_mbps = 1000, .input_auxiliary_mbps = 400}};
    memset(s.name, 'W', sizeof(s.name) - 1);
    for (int i = 0; i < 4; i++) s.cores[i] = (NativeCpuUsage){.present = true, .valid = true, .percent = i * 10};
    for (int i = 0; i < 3; i++) s.audio[i] = (NativeDiagnosticsAudio){.connected = true, .codec = RDP_AUDIO_CODEC_OPUS,
        .sample_rate = 48000, .channels = 2, .source = {.open = true, .queue_ms = 30, .target_delay_ms = 40,
        .underruns = UINT64_MAX, .hard_corrections = UINT64_MAX, .overflows = UINT64_MAX}};
    NativeStatisticsTimeline timeline = {0};
    for (unsigned at = 0; at <= 60000; at += 500) {
        s.at_ms = at;
        s.network = (RdpNetworkStats){.generation = 1, .active = 1, .rate_valid = 1,
            .started_ms = 1, .sampled_ms = at + 1, .observed_ms = at + 1,
            .last_rx_ms = at + 1, .report_ms = at + 1, .base_rtt_ms_at = at + 1,
            .bandwidth_ms_at = at + 1, .reports = at / 500 + 1,
            .rx_kbps = 12000 + (at / 500 % 8) * 1000, .tx_kbps = 500,
            .average_rtt_ms = 4, .base_rtt_ms = 0, .bandwidth_kbps = 100000,
            .tcp_valid = 63, .tcp_srtt_us = 1250, .tcp_rttvar_us = 500,
            .tcp_rto_us = 200000, .tcp_unacked = 2, .tcp_cwnd = 10,
            .rx_bytes = 104857600, .tx_bytes = 5242880, .tcp_retransmits = 3};
        s.counters.last_au_ms = at;
        s.rates.input_aus = 30 + (at / 500) % 12;
        s.rates.input_mbps = 8 + (at / 500) % 8;
        s.rates.input_auxiliary_mbps = 3;
        s.decoder.available = true;
        s.decoder.primary_total += 15;
        if (at && at % 5000 == 0) {
            s.decoder.at_ms = at; s.decoder.window_ms = 5000; s.decoder.primary = 150;
        }
        native_statistics_sample(&timeline, &s);
    }
    n = 1; queued[0] = (SDL_Event){.type = SDL_KEYDOWN}; queued[0].key.keysym.sym = SDLK_a;
    assert(native_preconnect_ui_statistics_prepare(&ui, &s));
    assert(n == 1 && !statistics_presents);
    assert(!native_preconnect_ui_statistics_prepare(&ui, &s));
    for (unsigned tab = 0; tab < NATIVE_STATS_TABS; tab++) {
        assert(ui.statistics_navigation.tab == tab);
        NativeStatisticsPage *page = &ui.statistics_pages[tab];
        unsigned categories = tab == NATIVE_STATS_VIDEO ? 4 : tab == NATIVE_STATS_AUDIO || tab == NATIVE_STATS_NETWORK ? 3 : 1;
        for (unsigned group = 0; group < categories; group++) {
            ui.statistics_navigation.video_category = group % 4;
            ui.statistics_navigation.audio_category = group % 3;
            ui.statistics_navigation.network_category = group % 3;
            ui.statistics_dirty = true;
            assert(native_preconnect_ui_statistics_prepare(&ui, &s));
            lv_obj_update_layout(ui.statistics_panel);
            assert(lv_obj_get_width(ui.statistics_panel) == 960 && lv_obj_get_height(ui.statistics_panel) == 680);
            assert(lv_obj_get_x(ui.statistics_panel) == 24 && lv_obj_get_y(ui.statistics_panel) == 24);
            assert(lv_obj_get_width(ui.statistics_tabs) == 928 && lv_obj_get_height(ui.statistics_tabs) == 570);
            assert(lv_obj_get_width(ui.statistics_help_panel) == 888 && lv_obj_get_height(ui.statistics_help_panel) == 490);
            assert(lv_obj_get_style_text_font(ui.statistics_name, 0) == &lv_font_ibm_plex_sans_semibold_16);
            assert(lv_obj_get_style_text_font(ui.statistics_format, 0) == &lv_font_ibm_plex_mono_semibold_12);
            if (tab < NATIVE_STATS_DIAGNOSTICS) for (unsigned i = 0; i < 4; i++) {
                lv_obj_t *card = lv_obj_get_parent(page->cards[i].value);
                assert(lv_obj_get_width(card) == 223 && lv_obj_get_height(card) == 80);
                assert(lv_obj_get_style_text_font(page->cards[i].label, 0) == &lv_font_ibm_plex_mono_semibold_12);
                NativeMetricValue value = native_statistics_value(&s, page->cards[i].metric);
                assert(lv_obj_get_style_text_font(page->cards[i].value, 0) ==
                    (value.state == NATIVE_METRIC_VALID ? &lv_font_ibm_plex_mono_regular_22 : &lv_font_ibm_plex_sans_semibold_16));
            }
            assert(!lv_obj_has_flag(page->object, LV_OBJ_FLAG_SCROLLABLE));
            statistics_check_labels(page->object);
            for (unsigned i = 0; i < page->field_count; i++) {
                NativeMetricValue v = native_statistics_value(&s, page->fields[i].metric);
                const char *unit = native_statistics_metric(page->fields[i].metric)->unit;
                char expected[112];
                snprintf(expected, sizeof(expected), "%s%s%s", v.text, v.state == NATIVE_METRIC_VALID && *unit ? " " : "", v.state == NATIVE_METRIC_VALID ? unit : "");
                assert(strcmp(expected, lv_label_get_text(page->fields[i].value)) == 0);
            }
            statistics_preview(tab, group, false);
            statistics_press(&ui, NATIVE_STATS_KEY_DOWN);
            if (categories > 1) statistics_press(&ui, NATIVE_STATS_KEY_DOWN);
            assert(native_preconnect_ui_statistics_prepare(&ui, &s));
            assert(lv_obj_get_style_outline_width(ui.statistics_hint, 0) == 1);
            statistics_press(&ui, NATIVE_STATS_KEY_OK);
            assert(ui.statistics_navigation.help);
            assert(native_preconnect_ui_statistics_prepare(&ui, &s));
            assert(lv_obj_get_style_outline_width(ui.statistics_hint, 0) == 0);
            statistics_check_labels(ui.statistics_help_panel);
            statistics_preview(tab, group, true);
            statistics_press(&ui, NATIVE_STATS_KEY_RIGHT);
            assert(native_preconnect_ui_statistics_prepare(&ui, &s));
            statistics_check_labels(ui.statistics_help_panel);
            statistics_press(&ui, NATIVE_STATS_KEY_BACK);
            statistics_press(&ui, NATIVE_STATS_KEY_BACK);
            assert(native_preconnect_ui_statistics_prepare(&ui, &s));
            assert(lv_obj_get_style_outline_width(ui.statistics_hint, 0) == 0);
        }
        if (tab == NATIVE_STATS_AUDIO) {
            for (int i = 0; i < 4; i++) assert(page->sessions[i] && !lv_obj_has_flag(page->sessions[i], LV_OBJ_FLAG_HIDDEN));
            assert(strstr(lv_label_get_text(page->sessions[3]), "Off"));
        }
        if (tab == NATIVE_STATS_NETWORK) {
            for (int i = 0; i < 4; i++) assert(native_statistics_value(&s, page->cards[i].metric).state == NATIVE_METRIC_VALID);
            for (int i = 0; i < 2; i++) for (unsigned j = 0; j < page->charts[i].count; j++) {
                lv_coord_t *y = lv_chart_get_y_array(page->charts[i].object, page->charts[i].series[j]);
                assert(y[0] != LV_CHART_POINT_NONE);
            }
            assert(ui.statistics_navigation.network_category == 2);
            assert(native_preconnect_ui_statistics_visible(&ui, false));
            assert(native_preconnect_ui_statistics_visible(&ui, true));
            assert(ui.statistics_navigation.tab == NATIVE_STATS_NETWORK && ui.statistics_navigation.network_category == 2);
            assert(native_preconnect_ui_statistics_prepare(&ui, &s));
        }
        if (tab == NATIVE_STATS_PERFORMANCE) {
            for (int i = 0; i < 8; i++) {
                assert(lv_obj_has_flag(page->core_labels[i], LV_OBJ_FLAG_HIDDEN) == (i >= 4));
                assert(lv_obj_has_flag(page->cores[i], LV_OBJ_FLAG_HIDDEN) == (i >= 4));
            }
            s.cores[4] = (NativeCpuUsage){.present = true};
            ui.statistics_dirty = true; assert(native_preconnect_ui_statistics_prepare(&ui, &s));
            assert(!lv_obj_has_flag(page->core_labels[4], LV_OBJ_FLAG_HIDDEN));
            assert(strstr(lv_label_get_text(page->core_labels[4]), "Collecting"));
            assert(lv_obj_has_flag(page->cores[4], LV_OBJ_FLAG_HIDDEN));
            s.cores[4] = (NativeCpuUsage){.present = true, .valid = true, .percent = 25};
            ui.statistics_dirty = true; assert(native_preconnect_ui_statistics_prepare(&ui, &s));
            assert(!lv_obj_has_flag(page->cores[4], LV_OBJ_FLAG_HIDDEN));
            assert(lv_bar_get_value(page->cores[4]) == 25);
            s.cores[4] = (NativeCpuUsage){0};
            ui.statistics_dirty = true; assert(native_preconnect_ui_statistics_prepare(&ui, &s));
            assert(lv_obj_has_flag(page->core_labels[4], LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_has_flag(page->cores[4], LV_OBJ_FLAG_HIDDEN));
        }
        statistics_press(&ui, NATIVE_STATS_KEY_RIGHT);
        assert(native_preconnect_ui_statistics_prepare(&ui, &s));
        assert(n == 1 && !statistics_presents);
    }
    statistics_press(&ui, NATIVE_STATS_KEY_RIGHT);
    statistics_press(&ui, NATIVE_STATS_KEY_DOWN);
    statistics_press(&ui, NATIVE_STATS_KEY_RIGHT);
    assert(ui.statistics_navigation.tab == NATIVE_STATS_VIDEO && ui.statistics_navigation.video_category == 1);
    assert(ui.statistics_navigation.focus == 1);
    assert(native_preconnect_ui_statistics_visible(&ui, false));
    assert(native_preconnect_ui_statistics_visible(&ui, true));
    assert(ui.statistics_navigation.tab == NATIVE_STATS_VIDEO && ui.statistics_navigation.video_category == 1);
    assert(ui.statistics_navigation.focus == 0 && !ui.statistics_navigation.held);
    assert(native_preconnect_ui_statistics_prepare(&ui, &s));
    NativeStatisticsChart *chart = &ui.statistics_pages[NATIVE_STATS_VIDEO].charts[0];
    assert(lv_chart_get_type(chart->object) == LV_CHART_TYPE_SCATTER);
    lv_coord_t *x = lv_chart_get_x_array(chart->object, chart->series[0]);
    assert(x[0] == 0 && x[60] == 300);
    assert(lv_chart_get_y_array(chart->object, chart->series[0])[61] == LV_CHART_POINT_NONE);
    NativeStatisticsChart *decoded = &ui.statistics_pages[NATIVE_STATS_VIDEO].charts[1];
    lv_coord_t *decoder_x = lv_chart_get_x_array(decoded->object, decoded->series[0]);
    lv_coord_t *decoder_y = lv_chart_get_y_array(decoded->object, decoded->series[0]);
    for (unsigned i = 0; i < NATIVE_DIAGNOSTICS_HISTORY_POINTS; i++) {
        assert(decoder_x[i] == i * 5);
        assert(decoder_y[i] == 500);
    }
    NativeStatisticsSeries *bitrate = &timeline.series[NATIVE_STATS_MBPS];
    bitrate->count = 7;
    for (unsigned i = 0; i < bitrate->count; i++)
        bitrate->points[i] = (NativeStatisticsPoint){.at_ms = 57000 + i * 500, .value = i ? 0 : 2, .valid = i != 4};
    unsigned zero_segments = 0;
    lv_obj_add_event_cb(chart->object, statistics_check_zero_line, LV_EVENT_DRAW_PART_BEGIN, &zero_segments);
    ui.statistics_dirty = true;
    assert(native_preconnect_ui_statistics_prepare(&ui, &s));
    assert(zero_segments == 3);
    lv_coord_t *y = lv_chart_get_y_array(chart->object, chart->series[1]);
    assert(y[1] == 0 && y[4] == LV_CHART_POINT_NONE && y[6] == 0);
    statistics_preview(NATIVE_STATS_VIDEO, 4, false);
    lv_obj_remove_event_cb(chart->object, statistics_check_zero_line);
    lv_event_send(ui.statistics_btn, LV_EVENT_CLICKED, NULL);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(ui.statistics_btn, 0)), "Stats: Off") == 0);
    lv_obj_del(ui.statistics_panel); lv_obj_del(ui.statistics_btn); lv_disp_remove(ui.disp);
}

static void test_certificate_prompt(void) {
    NativePreconnectUi ui = {0};
    NativeSessionConfig session = {0};
    strcpy(session.host, "test.invalid");
    session.port = 3389;
    const char *pin = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    ui.root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ui.root, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT);
    ui.group = lv_group_create();
    ui.connecting = true;
    ui.key_drv.base.user_data = &ui;
    native_preconnect_ui_show_certificate(&ui, &session, pin, "Unknown issuer");
    assert(ui.certificate_visible);
    assert(lv_group_get_focused(ui.group) == ui.certificate_cancel);
    assert(!lv_obj_has_state(ui.certificate_remember, LV_STATE_CHECKED));
    assert(strstr(lv_label_get_text(ui.certificate_detail), "test.invalid:3389"));
    queued[0] = (SDL_Event){.type = SDL_KEYDOWN};
    queued[0].key.keysym.sym = SDLK_RETURN;
    n = 1;
    lv_indev_data_t data = {0};
    native_ui_preconnect_key_read(&ui.key_drv.base, &data);
    assert(data.key == LV_KEY_ENTER && data.state == LV_INDEV_STATE_PRESSED);

    bool accept, remember;
    lv_event_send(ui.certificate_cancel, LV_EVENT_CLICKED, NULL);
    assert(native_preconnect_ui_take_certificate(&ui, &accept, &remember));
    assert(!accept && !remember);
    native_preconnect_ui_close_certificate(&ui);
    assert(!ui.certificate_visible);
    native_preconnect_ui_show_certificate(&ui, &session, pin, "Unknown issuer");
    lv_obj_add_state(ui.certificate_remember, LV_STATE_CHECKED);
    lv_event_send(ui.certificate_continue, LV_EVENT_CLICKED, NULL);
    assert(native_preconnect_ui_take_certificate(&ui, &accept, &remember));
    assert(accept && remember);
    native_preconnect_ui_close_certificate(&ui);
    native_preconnect_ui_show_certificate(&ui, &session, pin, "Certificate changed");
    assert(!lv_obj_has_state(ui.certificate_remember, LV_STATE_CHECKED));
    lv_event_send(ui.certificate_continue, LV_EVENT_CLICKED, NULL);
    assert(native_preconnect_ui_take_certificate(&ui, &accept, &remember));
    assert(accept && !remember);
    native_preconnect_ui_close_certificate(&ui);
    lv_group_del(ui.group);
    lv_obj_del(ui.root);
}

int main(void) {
    test_click_then_type(); test_keypad_and_text_deduplication(); test_credentials_byte_limits();
    test_statistics_layout_and_noninteractive_refresh();
    test_certificate_prompt();
    return 0;
}

void native_ui_mixer_set_profiles(NativeUiMixer *mixer, const NativeSessionConfig *values) { (void)mixer; (void)values; }
