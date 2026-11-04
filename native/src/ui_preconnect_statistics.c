#include "ui_preconnect_internal.h"

void ui_statistics_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = lv_event_get_user_data(event);
    if (!ui || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    ui->statistics_enabled = !ui->statistics_enabled;
    lv_label_set_text(lv_obj_get_child(ui->statistics_btn, 0), ui->statistics_enabled ? "Stats: On" : "Stats: Off");
}

bool native_preconnect_ui_statistics_enabled(const NativePreconnectUi *ui) {
    return ui && ui->statistics_enabled;
}

void native_preconnect_ui_statistics_reset_focus(NativePreconnectUi *ui) {
    if (!ui) return;
    native_statistics_focus_reset(&ui->statistics_navigation);
    ui->statistics_dirty = true;
}

bool native_preconnect_ui_statistics_key(NativePreconnectUi *ui, NativeStatisticsKey key,
                                         bool down, bool repeat, bool *open_hub) {
    *open_hub = false;
    if (!ui || !ui->statistics_enabled || !ui->statistics_visible) return false;
    if (!native_statistics_key(&ui->statistics_navigation, key, down, repeat, open_hub)) return false;
    lv_tabview_set_act(ui->statistics_tabs, ui->statistics_navigation.tab, LV_ANIM_OFF);
    ui->statistics_dirty = true;
    return true;
}

bool native_preconnect_ui_statistics_visible(NativePreconnectUi *ui, bool visible) {
    if (!ui || ui->statistics_visible == visible) return false;
    ui->statistics_visible = visible;
    native_preconnect_ui_statistics_reset_focus(ui);
    if (visible) {
        lv_obj_clear_flag(ui->statistics_panel, LV_OBJ_FLAG_HIDDEN);
        ui->hidden_cleared = true;
    } else lv_obj_add_flag(ui->statistics_panel, LV_OBJ_FLAG_HIDDEN);
    return true;
}

bool native_preconnect_ui_statistics_prepare(NativePreconnectUi *ui, const NativeDiagnosticsSnapshot *s) {
    if (!ui || !s || !ui->statistics_visible) return false;
    if (!ui->statistics_dirty && ui->statistics_at_ms == s->at_ms && ui->statistics_slot == s->slot &&
        ui->statistics_epoch == s->counters.epoch && ui->statistics_generation == s->counters.generation) return false;
    ui->statistics_at_ms = s->at_ms;
    ui->statistics_epoch = s->counters.epoch;
    ui->statistics_generation = s->counters.generation;
    ui->statistics_slot = s->slot;
    ui_statistics_update(ui, s);
    /* The shared texture may still hold HUB or mixer pixels when this view returns. */
    SDL_SetRenderTarget(ui->renderer, ui->texture);
    SDL_SetRenderDrawBlendMode(ui->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ui->renderer, 0, 0, 0, 0);
    SDL_RenderClear(ui->renderer);
    lv_obj_invalidate(lv_scr_act());
    ui->statistics_rendering = true;
    lv_refr_now(ui->disp);
    ui->statistics_rendering = false;
    SDL_SetRenderTarget(ui->renderer, NULL);
    ui->statistics_dirty = false;
    return true;
}

void native_preconnect_ui_statistics_draw(NativePreconnectUi *ui) {
    if (!ui || !ui->statistics_visible) return;
    SDL_SetTextureBlendMode(ui->texture, SDL_BLENDMODE_BLEND);
    SDL_RenderCopy(ui->renderer, ui->texture, NULL, NULL);
}
