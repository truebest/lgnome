#include <assert.h>
#include "native_app.h"
#include "native_config_storage.h"
#include "native_mixer_overlay.h"

static unsigned saves;
static int applied_gain;
static bool save_ok = true;
uint32_t SDL_GetTicks(void) { return 100; }
int32_t native_ui_mixer_gain_db_to_q15(int gain) { return gain; }
void native_audio_pipeline_set_source_gain(NativeAudioPipeline *pipeline, int source, int32_t gain) {
    (void)pipeline;
    assert(source == 1);
    applied_gain = gain;
}
void native_audio_pipeline_set_source_muted(NativeAudioPipeline *pipeline, int source, bool muted) {
    (void)pipeline;
    assert(source == 1 && muted);
}
void native_publish_effective_solo_mask(App *app) { assert(app->mixer_solo_mask == 2); }
const char *native_session_slot_name(int slot) { (void)slot; return "green"; }
bool native_config_save_persisted(const NativeSettings *settings) {
    assert(settings->sessions[1].mixer_gain_db == -12);
    saves++;
    return save_ok;
}
int main(void) {
    NativeSettings settings = {0};
    App app = {0};
    app.settings = &settings;
    app.mixer_overlay_dragging = true;
    native_mixer_overlay_set_db(&app, 1, -6);
    native_mixer_overlay_set_db(&app, 1, -12);
    assert(applied_gain == -12 && saves == 0);
    assert(settings.sessions[1].mixer_gain_db == -12 && app.mixer_settings_dirty);
    assert(app.mixer_settings_save_ticks == 400);
    save_ok = false;
    native_mixer_overlay_flush_settings(&app);
    assert(saves == 1 && app.mixer_settings_dirty);
    save_ok = true;
    native_mixer_overlay_flush_settings(&app);
    assert(saves == 2 && !app.mixer_settings_dirty);
    native_mixer_overlay_flush_settings(&app);
    native_mixer_overlay_set_db(&app, 1, -12);
    assert(saves == 2 && !app.mixer_settings_dirty);
    save_ok = false;
    native_mixer_overlay_toggle_mute(&app, 1);
    assert(settings.sessions[1].mixer_muted && app.mixer_settings_dirty);
    native_mixer_overlay_flush_settings(&app);
    assert(saves == 3 && app.mixer_settings_dirty);
    save_ok = true;
    native_mixer_overlay_flush_settings(&app);
    assert(saves == 4 && !app.mixer_settings_dirty);
    native_mixer_overlay_toggle_solo(&app, 1);
    assert(settings.sessions[1].mixer_solo && app.mixer_settings_dirty);
    native_mixer_overlay_flush_settings(&app);
    assert(saves == 5 && !app.mixer_settings_dirty);
    return 0;
}
