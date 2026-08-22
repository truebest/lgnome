#include "ui_preconnect_internal.h"

#include "clog.h"

clog_define(g_native_log_ui_screen, cLogLevelInfo, cLogFlags_Default, "ui.screen", NULL);

typedef struct NativeUiScreenVtable {
    lv_obj_t *(*root)(NativePreconnectUi *ui);
    void (*prepare)(NativePreconnectUi *ui);
    void (*focus)(NativePreconnectUi *ui);
    const char *name;
} NativeUiScreenVtable;

static lv_obj_t *screen_hub_root(NativePreconnectUi *ui) {
    (void)ui;
    return NULL;
}

static lv_obj_t *screen_setup_root(NativePreconnectUi *ui) {
    return ui->setup_scrim;
}

static lv_obj_t *screen_capture_root(NativePreconnectUi *ui) {
    return ui->capture_scrim;
}

static lv_obj_t *screen_onboarding_root(NativePreconnectUi *ui) {
    return ui->onboarding_scrim;
}

static void screen_prepare_none(NativePreconnectUi *ui) {
    (void)ui;
}

static void screen_prepare_setup(NativePreconnectUi *ui) {
    ui->delete_armed = false;
    if (ui->delete_label) {
        lv_label_set_text(ui->delete_label, "Delete profile");
    }
}

static void screen_prepare_capture(NativePreconnectUi *ui) {
    ui_load_capture_settings_form(ui);
    ui_set_text(ui->capture_status_label, "");
    lv_obj_clear_state(ui->capture_save_btn, LV_STATE_DISABLED);
    lv_obj_clear_state(ui->capture_cancel_btn, LV_STATE_DISABLED);
}

static void screen_focus_hub(NativePreconnectUi *ui) {
    if (!ui || !ui->group) {
        return;
    }
    int selected = ui->selected_slot;
    ui->rebuilding_group = true;
    lv_group_remove_all_objs(ui->group);
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        lv_group_add_obj(ui->group, ui->slot_buttons[slot]);
    }
    lv_group_add_obj(ui->group, ui->hero_action_btn);
    lv_group_add_obj(ui->group, ui->hero_edit_btn);
    lv_group_add_obj(ui->group, ui->capture_settings_btn);
    lv_group_add_obj(ui->group, ui->help_btn);
    lv_group_focus_obj(ui->slot_buttons[selected]);
    ui->rebuilding_group = false;
}

static void screen_focus_setup(NativePreconnectUi *ui) {
    if (!ui || !ui->group) {
        return;
    }
    lv_group_remove_all_objs(ui->group);
    lv_group_add_obj(ui->group, ui->name_input);
    lv_group_add_obj(ui->group, ui->host_input);
    lv_group_add_obj(ui->group, ui->port_input);
    lv_group_add_obj(ui->group, ui->username_input);
    lv_group_add_obj(ui->group, ui->domain_input);
    lv_group_add_obj(ui->group, ui->password_input);
    lv_group_add_obj(ui->group, ui->fps_dropdown);
    lv_group_add_obj(ui->group, ui->audio_codec_dropdown);
    lv_group_add_obj(ui->group, ui->profile_camera_checkbox);
    lv_group_add_obj(ui->group, ui->profile_audio_input_checkbox);
    lv_group_add_obj(ui->group, ui->delete_btn);
    lv_group_add_obj(ui->group, ui->connect_btn);
    lv_group_add_obj(ui->group, ui->save_btn);
    lv_group_add_obj(ui->group, ui->cancel_btn);
    lv_group_focus_obj(ui->name_input);
}

static void screen_focus_capture(NativePreconnectUi *ui) {
    if (!ui || !ui->group) {
        return;
    }
    lv_group_remove_all_objs(ui->group);
    lv_group_add_obj(ui->group, ui->camera_input_dropdown);
    lv_group_add_obj(ui->group, ui->camera_resolution_dropdown);
    lv_group_add_obj(ui->group, ui->camera_fps_dropdown);
    lv_group_add_obj(ui->group, ui->audio_input_dropdown);
    lv_group_add_obj(ui->group, ui->audio_input_gain_dropdown);
    lv_group_add_obj(ui->group, ui->capture_save_btn);
    lv_group_add_obj(ui->group, ui->capture_cancel_btn);
    lv_group_focus_obj(ui->camera_input_dropdown);
}

static void screen_focus_onboarding(NativePreconnectUi *ui) {
    if (!ui || !ui->group) {
        return;
    }
    lv_group_remove_all_objs(ui->group);
    lv_group_add_obj(ui->group, ui->onboarding_btn);
    lv_group_focus_obj(ui->onboarding_btn);
}

static const NativeUiScreenVtable UI_SCREENS[NATIVE_UI_SCREEN_COUNT] = {
    [NATIVE_UI_SCREEN_HUB] = {screen_hub_root, screen_prepare_none, screen_focus_hub, "hub"},
    [NATIVE_UI_SCREEN_SETUP] = {screen_setup_root, screen_prepare_setup, screen_focus_setup, "setup"},
    [NATIVE_UI_SCREEN_CAPTURE] = {screen_capture_root, screen_prepare_capture, screen_focus_capture, "capture"},
    [NATIVE_UI_SCREEN_ONBOARDING] =
        {
            screen_onboarding_root,
            screen_prepare_none,
            screen_focus_onboarding,
            "onboarding",
        },
};

static void screen_set_flags(NativePreconnectUi *ui, NativeUiScreenId screen) {
    ui->setup_visible = screen == NATIVE_UI_SCREEN_SETUP;
    ui->capture_settings_visible = screen == NATIVE_UI_SCREEN_CAPTURE;
    ui->onboarding_visible = screen == NATIVE_UI_SCREEN_ONBOARDING;
}

static void screen_apply(NativePreconnectUi *ui, NativeUiScreenId screen) {
    if (!ui || screen < NATIVE_UI_SCREEN_HUB || screen >= NATIVE_UI_SCREEN_COUNT) {
        return;
    }
    for (NativeUiScreenId id = NATIVE_UI_SCREEN_SETUP; id < NATIVE_UI_SCREEN_COUNT; id++) {
        lv_obj_t *root = UI_SCREENS[id].root(ui);
        if (root) {
            lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        }
    }

    ui->screen = screen;
    screen_set_flags(ui, screen);
    UI_SCREENS[screen].prepare(ui);
    lv_obj_t *root = UI_SCREENS[screen].root(ui);
    if (root) {
        lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root);
    }
    UI_SCREENS[screen].focus(ui);
    clog(cLogLevelTrace, "screen switched to %s", UI_SCREENS[screen].name);
}

void native_ui_preconnect_screen_init(NativePreconnectUi *ui, NativeUiScreenId initial) {
    if (!ui) {
        return;
    }
    ui->return_screen = NATIVE_UI_SCREEN_HUB;
    screen_apply(ui, initial);
}

void native_ui_preconnect_screen_refocus(NativePreconnectUi *ui) {
    if (ui && ui->screen >= NATIVE_UI_SCREEN_HUB && ui->screen < NATIVE_UI_SCREEN_COUNT) {
        UI_SCREENS[ui->screen].focus(ui);
    }
}

static void screen_show(NativePreconnectUi *ui, NativeUiScreenId screen) {
    if (!ui) {
        return;
    }
    if (screen == NATIVE_UI_SCREEN_ONBOARDING && ui->screen != NATIVE_UI_SCREEN_ONBOARDING) {
        ui->return_screen = ui->screen;
    }
    screen_apply(ui, screen);
}

static void screen_close(NativePreconnectUi *ui, NativeUiScreenId screen) {
    if (!ui) {
        return;
    }
    if (ui->screen == NATIVE_UI_SCREEN_ONBOARDING && screen != NATIVE_UI_SCREEN_ONBOARDING) {
        if (ui->return_screen == screen) {
            ui->return_screen = NATIVE_UI_SCREEN_HUB;
        }
        lv_obj_t *root = UI_SCREENS[screen].root(ui);
        if (root) {
            lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (ui->screen != screen) {
        return;
    }
    NativeUiScreenId next = screen == NATIVE_UI_SCREEN_ONBOARDING ? ui->return_screen : NATIVE_UI_SCREEN_HUB;
    screen_apply(ui, next);
}

void ui_show_setup(NativePreconnectUi *ui, bool visible) {
    if (visible) {
        screen_show(ui, NATIVE_UI_SCREEN_SETUP);
    } else {
        screen_close(ui, NATIVE_UI_SCREEN_SETUP);
    }
}

void ui_show_capture_settings(NativePreconnectUi *ui, bool visible) {
    if (visible) {
        screen_show(ui, NATIVE_UI_SCREEN_CAPTURE);
    } else {
        screen_close(ui, NATIVE_UI_SCREEN_CAPTURE);
    }
}

void ui_show_onboarding(NativePreconnectUi *ui, bool visible) {
    if (visible) {
        screen_show(ui, NATIVE_UI_SCREEN_ONBOARDING);
    } else {
        screen_close(ui, NATIVE_UI_SCREEN_ONBOARDING);
    }
}

/* Shared keypad routing for setup/capture forms and screen dismissal. */
static void ui_scroll_focused_into_view(NativePreconnectUi *ui) {
    if (!ui || !ui->group) {
        return;
    }
    lv_obj_t *focused = lv_group_get_focused(ui->group);
    if (focused) {
        lv_obj_scroll_to_view_recursive(focused, LV_ANIM_OFF);
    }
}

void native_ui_preconnect_form_key_event(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || !ui->group) {
        return;
    }
    uint32_t key = lv_event_get_key(event);
    /* An OPEN dropdown owns UP/DOWN for option selection; only navigate the form with
     * them while every dropdown is closed. */
    lv_obj_t *target = lv_event_get_target(event);
    if ((target == ui->fps_dropdown && lv_dropdown_is_open(ui->fps_dropdown)) ||
        (target == ui->audio_codec_dropdown &&
         lv_dropdown_is_open(ui->audio_codec_dropdown)) ||
        (target == ui->camera_input_dropdown &&
         lv_dropdown_is_open(ui->camera_input_dropdown)) ||
        (target == ui->camera_resolution_dropdown &&
         lv_dropdown_is_open(ui->camera_resolution_dropdown)) ||
        (target == ui->camera_fps_dropdown &&
         lv_dropdown_is_open(ui->camera_fps_dropdown)) ||
        (target == ui->audio_input_dropdown &&
         lv_dropdown_is_open(ui->audio_input_dropdown)) ||
        (target == ui->audio_input_gain_dropdown &&
         lv_dropdown_is_open(ui->audio_input_gain_dropdown))) {
        if (key == LV_KEY_ESC) {
            if (ui->key_indev) {
                lv_indev_wait_release(ui->key_indev);
            }
        }
        return;
    }

    if (key == LV_KEY_ESC) {
        if (ui->setup_visible || ui->capture_settings_visible ||
            ui->onboarding_visible) {
            if (ui->onboarding_visible) {
                ui_show_onboarding(ui, false);
            } else if (ui->capture_settings_visible) {
                ui_capture_cancel_clicked(event);
            } else {
                ui_cancel_clicked(event);
            }
            if (ui->key_indev) {
                lv_indev_wait_release(ui->key_indev);
            }
        } else {
            native_preconnect_ui_cancel_pending_navigation(ui);
            ui->hub_close_requested = true;
            ui->hub_closing = true;
        }
        lv_event_stop_processing(event);
    } else if (ui->hub_closing || ui->connecting ||
               ui->capture_save_pending) {
        return;
    } else if (ui_hub_navigate(ui, target, key)) {
        lv_event_stop_processing(event);
    } else if (key == LV_KEY_DOWN) {
        lv_group_focus_next(ui->group);
        ui_scroll_focused_into_view(ui);
        lv_event_stop_processing(event);
    } else if (key == LV_KEY_UP) {
        lv_group_focus_prev(ui->group);
        ui_scroll_focused_into_view(ui);
        lv_event_stop_processing(event);
    }
}
