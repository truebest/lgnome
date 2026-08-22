#include "ui_preconnect.h"
#include "ui_preconnect_internal.h"

#include "rdp_ffi.h"
#include "ui_key_queue.h"
#include "ui_mixer.h"
#include "ui_vu_meter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw/sdl/lv_draw_sdl.h"
#include "lvgl.h"

#include "clog.h"

clog_define(g_native_log_ui, cLogLevelInfo, cLogFlags_Default, "ui.preconnect", NULL);

_Static_assert(NATIVE_UI_KEY_QUEUE_CAP - 1u >= 2u * (UI_USERNAME_MAX - 1u),
               "key queue must hold username paste");
_Static_assert(NATIVE_UI_KEY_QUEUE_CAP - 1u >= 2u * (UI_DOMAIN_MAX - 1u),
               "key queue must hold domain paste");
_Static_assert(NATIVE_UI_KEY_QUEUE_CAP - 1u >= 2u * (UI_NAME_MAX - 1u),
               "key queue must hold profile-name paste");
_Static_assert(NATIVE_UI_KEY_QUEUE_CAP - 1u >= 2u * UI_PASSWORD_TEXT_MAX,
               "key queue must hold password paste");

static void ui_update_card_audio_meters(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }

    const uint32_t ticks = SDL_GetTicks();
    const float fall_db = ui->card_audio_meter_ticks == 0
                              ? 0.0f
                              : (float)(ticks - ui->card_audio_meter_ticks) *
                                    (NATIVE_MIXER_METER_DECAY_DB_S / 1000.0f);
    ui->card_audio_meter_ticks = ticks;

    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (!ui->card_audio_groups[slot]) {
            continue;
        }
        bool group_hidden = lv_obj_has_flag(ui->card_audio_groups[slot], LV_OBJ_FLAG_HIDDEN);
        for (int side = 0; side < UI_CARD_AUDIO_METER_COUNT; side++) {
            if (!ui->slot_audio_stream_open[slot]) {
                native_ui_vu_meter_reset(&ui->card_audio_meters[slot][side]);
                continue;
            }
            if (group_hidden) {
                continue;
            }
            (void)native_ui_vu_meter_update(&ui->card_audio_meters[slot][side],
                                            ui->slot_audio_peaks[slot][side], fall_db);
        }
    }
}

NativePreconnectUi *native_preconnect_ui_create(SDL_Window *window,
                                                SDL_Renderer *renderer,
                                                const NativeSettings *settings) {
    if (!window || !renderer || !settings) {
        return NULL;
    }
    const NativeSessionConfig *sessions = settings->sessions;
    uint16_t audio_codec = settings->audio_codec;

    NativePreconnectUi *ui = (NativePreconnectUi *)calloc(1, sizeof(*ui));
    if (!ui) {
        return NULL;
    }
    ui->window = window;
    ui->renderer = renderer;
    ui->visible = true;
    ui->keyboard_available = true;
    ui->selected_slot = NATIVE_SESSION_SLOT_RED;
    bool found_configured = false;
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        ui->slot_values[slot] = sessions[slot];
        ui->committed_values[slot] = sessions[slot];
        (void)snprintf(ui->slot_port_text[slot], UI_PORT_MAX, "%u",
                       (unsigned)(sessions[slot].port ? sessions[slot].port : 3389));
        ui->slot_port_valid[slot] = true;
        if (ui_slot_configured(&sessions[slot])) {
            ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_OFFLINE;
            if (!found_configured) {
                ui->selected_slot = slot;
                found_configured = true;
            }
        } else {
            ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_NOT_SET_UP;
        }
    }
    ui->onboarding_visible = !found_configured;
    ui->requested_slot = ui->selected_slot;
    ui->activated_slot = ui->selected_slot;
    ui->saved_slot = ui->selected_slot;
    ui->deleted_slot = ui->selected_slot;
    ui->committed_audio_codec = audio_codec;
    ui->camera_enabled = settings->camera_enabled;
    (void)snprintf(ui->camera_device_id, sizeof(ui->camera_device_id), "%s",
                   settings->camera_device_id);
    ui->camera_width = settings->camera_width;
    ui->camera_height = settings->camera_height;
    ui->camera_fps = settings->camera_fps;
    ui->audio_input_enabled = settings->audio_input_enabled;
    (void)snprintf(ui->audio_input_device_id,
                   sizeof(ui->audio_input_device_id), "%s",
                   settings->audio_input_device_id);
    ui->audio_input_gain_db = settings->audio_input_gain_db;
    ui->committed_camera_enabled = ui->camera_enabled;
    ui->committed_audio_input_enabled = ui->audio_input_enabled;
    ui_commit_capture_settings(ui);
    ui_enumerate_capture_devices(ui);

    SDL_GetWindowSize(window, &ui->width, &ui->height);
    if (ui->width <= 0 || ui->height <= 0) {
        ui->width = 1920;
        ui->height = 1080;
    }
    ui->pointer_x = ui->width / 2;
    ui->pointer_y = ui->height / 2;

    lv_init();
    ui->texture = lv_draw_sdl_create_screen_texture(renderer, ui->width, ui->height);
    if (!ui->texture) {
        clog(cLogLevelError, "failed to create LVGL screen texture");
        free(ui);
        return NULL;
    }
    lv_disp_draw_buf_init(&ui->draw_buf, ui->texture, NULL, (uint32_t)ui->width * (uint32_t)ui->height);
    lv_disp_drv_init(&ui->disp_drv);
    ui->draw_param.renderer = renderer;
    ui->draw_param.user_data = ui;
    ui->disp_drv.user_data = &ui->draw_param;
    ui->disp_drv.draw_buf = &ui->draw_buf;
    ui->disp_drv.flush_cb = native_ui_preconnect_display_flush;
    ui->disp_drv.clear_cb = native_ui_preconnect_display_clear;
    ui->disp_drv.hor_res = (lv_coord_t)ui->width;
    ui->disp_drv.ver_res = (lv_coord_t)ui->height;
    ui->disp_drv.dpi = LV_DPI_DEF;
    SDL_SetRenderTarget(renderer, ui->texture);
    ui->disp = lv_disp_drv_register(&ui->disp_drv);
    if (!ui->disp) {
        SDL_DestroyTexture(ui->texture);
        free(ui);
        return NULL;
    }
    ui->disp->bg_opa = LV_OPA_TRANSP;

    lv_indev_drv_init(&ui->pointer_drv);
    ui->pointer_drv.type = LV_INDEV_TYPE_POINTER;
    ui->pointer_drv.user_data = ui;
    ui->pointer_drv.read_cb = native_ui_preconnect_pointer_read;
    ui->pointer_indev = lv_indev_drv_register(&ui->pointer_drv);

    memset(&ui->key_drv, 0, sizeof(ui->key_drv));
    lv_indev_drv_init(&ui->key_drv.base);
    ui->key_drv.base.type = LV_INDEV_TYPE_KEYPAD;
    ui->key_drv.base.user_data = ui;
    ui->key_drv.base.read_cb = native_ui_preconnect_key_read;
    ui->key_drv.state = LV_INDEV_STATE_RELEASED;
    ui->key_indev = lv_indev_drv_register(&ui->key_drv.base);

    native_ui_preconnect_theme_init(ui);
    native_ui_preconnect_widgets_ready();
    native_ui_preconnect_forms_ready();
    const NativeSessionConfig *initial = &ui->slot_values[ui->selected_slot];
    native_ui_preconnect_build(ui, initial->host, initial->port, initial->username,
                               initial->password, initial->domain, initial->fps,
                               audio_codec);
    ui_load_slot_into_form(ui, ui->selected_slot);
    ui_update_hub(ui);
    native_preconnect_ui_set_keyboard_available(ui, true);
    native_preconnect_ui_set_mouse_available(ui, true);
    if (ui->key_indev && ui->group) {
        lv_indev_set_group(ui->key_indev, ui->group);
    }
    /* A failed mixer create degrades gracefully: the live overlay remains unavailable. */
    ui->mixer = native_ui_mixer_create(renderer);
    native_ui_mixer_set_texture(ui->mixer, ui->texture);
    native_ui_mixer_set_profiles(ui->mixer, ui->slot_values);
    return ui;
}

void native_preconnect_ui_destroy(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    if (ui->pointer_indev) {
        lv_indev_delete(ui->pointer_indev);
        ui->pointer_indev = NULL;
    }
    if (ui->key_indev) {
        lv_indev_delete(ui->key_indev);
        ui->key_indev = NULL;
    }
    if (ui->group) {
        lv_group_remove_all_objs(ui->group);
        lv_group_del(ui->group);
    }
    if (ui->root) {
        lv_obj_del(ui->root);
    }
    native_ui_mixer_destroy(ui->mixer);
    native_ui_preconnect_theme_reset(ui);
    if (ui->disp) {
        lv_disp_remove(ui->disp);
    }
    if (ui->texture) {
        SDL_DestroyTexture(ui->texture);
    }
    free(ui);
}

void native_preconnect_ui_resize(NativePreconnectUi *ui, int width, int height) {
    if (!ui || width <= 0 || height <= 0) {
        return;
    }
    ui->width = width;
    ui->height = height;
    ui->pointer_x = ui->pointer_x >= width ? width - 1 : ui->pointer_x;
    ui->pointer_y = ui->pointer_y >= height ? height - 1 : ui->pointer_y;
    if (ui->texture) {
        SDL_DestroyTexture(ui->texture);
    }
    ui->texture = lv_draw_sdl_create_screen_texture(ui->renderer, width, height);
    lv_disp_draw_buf_init(&ui->draw_buf, ui->texture, NULL, (uint32_t)width * (uint32_t)height);
    ui->disp_drv.hor_res = (lv_coord_t)width;
    ui->disp_drv.ver_res = (lv_coord_t)height;
    SDL_SetRenderTarget(ui->renderer, ui->texture);
    lv_disp_drv_update(ui->disp, &ui->disp_drv);
    native_ui_mixer_set_texture(ui->mixer, ui->texture);
    lv_obj_invalidate(lv_scr_act());
}

void native_preconnect_ui_tick(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    if (!ui->visible) {
        if (ui->hidden_cleared) {
            return;
        }
        SDL_SetRenderTarget(ui->renderer, NULL);
        SDL_SetRenderDrawBlendMode(ui->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ui->renderer, 0, 0, 0, 0);
        SDL_RenderClear(ui->renderer);
        SDL_RenderPresent(ui->renderer);
        SDL_SetRenderTarget(ui->renderer, ui->texture);
        ui->hidden_cleared = true;
        return;
    }
    ui->hidden_cleared = false;
    SDL_SetRenderTarget(ui->renderer, ui->texture);
    ui_update_card_audio_meters(ui);
    lv_timer_handler();
}

void native_preconnect_ui_set_visible(NativePreconnectUi *ui, bool visible) {
    if (!ui || ui->visible == visible) {
        return;
    }
    /* Visibility can change while LVGL still considers BACK or the pointer pressed.
     * Reset both the driver queue and LVGL's processed state at the boundary; otherwise
     * a hidden BACK-up is consumed by the streaming loop and the stale PRESSED state
     * repeats as soon as HUB is shown again. */
    native_ui_preconnect_reset_input_state(ui);
    ui->visible = visible;
    ui->hidden_cleared = false;
    if (!visible) {
        ui->hub_close_requested = false;
        ui->hub_closing = false;
    }
    if (ui->root) {
        if (visible) {
            lv_obj_clear_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui->root);
        } else {
            lv_obj_add_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void native_preconnect_ui_set_background_drawer(NativePreconnectUi *ui,
                                                NativePreconnectUiBackgroundDrawFn draw, void *ctx) {
    if (!ui) {
        return;
    }
    ui->background_draw = draw;
    ui->background_draw_ctx = ctx;
}

void native_preconnect_ui_set_hardware_video_plane(NativePreconnectUi *ui, bool available) {
    if (!ui || ui->hardware_video_plane == available) {
        return;
    }
    ui->hardware_video_plane = available;
    if (ui->root) {
        lv_obj_invalidate(ui->root);
    }
}

void native_preconnect_ui_set_slot_state(NativePreconnectUi *ui, int slot,
                                         NativePreconnectSessionState state, const char *detail) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    if (!ui_slot_configured(&ui->slot_values[slot]) && state != NATIVE_PRECONNECT_SESSION_CONNECTING &&
        state != NATIVE_PRECONNECT_SESSION_ERROR) {
        state = NATIVE_PRECONNECT_SESSION_NOT_SET_UP;
    }
    const char *next_detail = detail ? detail : "";
    bool same_detail;
    if (detail) {
        same_detail = strcmp(ui->slot_details[slot], next_detail) == 0;
    } else {
        same_detail = state == NATIVE_PRECONNECT_SESSION_ERROR || ui->slot_details[slot][0] == '\0';
    }
    if (ui->slot_states[slot] == state && same_detail) {
        return;
    }
    ui->slot_states[slot] = state;
    if (detail) {
        (void)snprintf(ui->slot_details[slot], UI_DETAIL_MAX, "%s", detail);
    } else if (state != NATIVE_PRECONNECT_SESSION_ERROR) {
        ui->slot_details[slot][0] = '\0';
    }
    if (slot == ui->selected_slot) {
        ui_update_hub(ui);
    } else {
        char fallback[32];
        lv_label_set_text(ui->card_name_labels[slot], ui_slot_display_name(ui, slot, fallback, sizeof(fallback)));
        lv_label_set_text(ui->card_badge_labels[slot], ui_badge_text(state));
        ui_update_hub(ui);
    }
    ui_update_connect_state(ui);
}

void native_preconnect_ui_set_slot_runtime(NativePreconnectUi *ui, int slot, uint16_t desktop_width,
                                           uint16_t desktop_height, uint32_t session_minutes,
                                           bool audio_stream_open, uint32_t audio_codec,
                                           uint32_t audio_sample_rate, uint16_t audio_channels,
                                           int32_t audio_peak_left, int32_t audio_peak_right) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    ui->slot_audio_peaks[slot][0] = audio_peak_left > 0 ? audio_peak_left : 0;
    ui->slot_audio_peaks[slot][1] = audio_peak_right > 0 ? audio_peak_right : 0;
    bool format_valid = audio_stream_open &&
                        (audio_codec == RDP_AUDIO_CODEC_OPUS || audio_codec == RDP_AUDIO_CODEC_PCM_S16LE) &&
                        audio_sample_rate != 0 && audio_channels != 0;
    if (!format_valid) {
        audio_codec = 0;
        audio_sample_rate = 0;
        audio_channels = 0;
    }
    if (ui->slot_desktop_width[slot] == desktop_width && ui->slot_desktop_height[slot] == desktop_height &&
        ui->slot_session_minutes[slot] == session_minutes &&
        ui->slot_audio_stream_open[slot] == audio_stream_open && ui->slot_audio_codec[slot] == audio_codec &&
        ui->slot_audio_sample_rate[slot] == audio_sample_rate && ui->slot_audio_channels[slot] == audio_channels) {
        return;
    }
    ui->slot_desktop_width[slot] = desktop_width;
    ui->slot_desktop_height[slot] = desktop_height;
    ui->slot_session_minutes[slot] = session_minutes;
    ui->slot_audio_stream_open[slot] = audio_stream_open;
    ui->slot_audio_codec[slot] = audio_codec;
    ui->slot_audio_sample_rate[slot] = audio_sample_rate;
    ui->slot_audio_channels[slot] = audio_channels;
    ui_update_hub(ui);
}

void native_preconnect_ui_set_keyboard_available(NativePreconnectUi *ui, bool available) {
    if (!ui) {
        return;
    }
    ui->keyboard_available = available;
    lv_obj_set_style_bg_color(ui->keyboard_dot, lv_color_hex(available ? 0x43d47e : 0xe8c15a), 0);
    if (available) {
        lv_obj_add_flag(ui->keyboard_warning, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(ui->keyboard_warning_label,
                          "No USB keyboard detected - connect one to type on the remote desktop.");
        lv_obj_clear_flag(ui->keyboard_warning, LV_OBJ_FLAG_HIDDEN);
    }
}

void native_preconnect_ui_set_mouse_available(NativePreconnectUi *ui, bool available) {
    if (!ui) {
        return;
    }
    lv_obj_set_style_bg_color(ui->mouse_dot, lv_color_hex(available ? 0x43d47e : 0xe8c15a), 0);
}

void native_preconnect_ui_set_input_unavailable(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    ui->keyboard_available = false;
    lv_obj_set_style_bg_color(ui->keyboard_dot, lv_color_hex(0xe35d55), 0);
    lv_obj_set_style_bg_color(ui->mouse_dot, lv_color_hex(0xe35d55), 0);
    lv_label_set_text(ui->keyboard_warning_label,
                      "USB input capture is unavailable - reconnect the devices or restart GnomeCast.");
    lv_obj_clear_flag(ui->keyboard_warning, LV_OBJ_FLAG_HIDDEN);
}

NativeUiMixer *native_preconnect_ui_mixer(NativePreconnectUi *ui) {
    return ui ? ui->mixer : NULL;
}
