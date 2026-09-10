#ifndef LGNOME_UI_PRECONNECT_INTERNAL_H
#define LGNOME_UI_PRECONNECT_INTERNAL_H

#include "ui_preconnect.h"
#include "ui_preconnect_screen.h"
#include "ui_preconnect_display.h"

#include "audio_input_alsa.h"
#include "camera_v4l2.h"
#include "ui_key_queue.h"
#include "ui_vu_meter.h"

#include "draw/sdl/lv_draw_sdl.h"
#include "lvgl.h"

#define UI_HOST_MAX 256u
#define UI_NAME_MAX NATIVE_SETTINGS_STRING_MAX
#define UI_USERNAME_MAX 256u
#define UI_DOMAIN_MAX 256u
#define UI_PASSWORD_TEXT_MAX 511u
#define UI_PASSWORD_MAX (UI_PASSWORD_TEXT_MAX + 1u)
#define UI_PORT_MAX 6u
#define UI_DETAIL_MAX 192u
#define UI_SETUP_PANEL_WIDTH 680
#define UI_CANVAS_WIDTH 1920
#define UI_CANVAS_HEIGHT 1080
#define UI_CARD_HEIGHT 218
#define UI_CARD_GAP 28
#define UI_FORM_KEY_EVENT (LV_EVENT_KEY | LV_EVENT_PREPROCESS)
#define UI_BRAND_CUBE_SIZE 34
#define UI_ONBOARDING_CUBE_SIZE 38
#define UI_CARD_AUDIO_METER_COUNT 2
#define UI_CARD_AUDIO_METER_WIDTH 6
#define UI_CARD_AUDIO_METER_HEIGHT 22
#define UI_CARD_AUDIO_METER_TOP 8
#define UI_CARD_AUDIO_METER_BASELINE (UI_CARD_AUDIO_METER_TOP + UI_CARD_AUDIO_METER_HEIGHT)
#define UI_HUB_MASK_COLOR 0x040508
#define UI_HUB_MASK_SEGMENT_COUNT 4
#define UI_HUB_X_AT_PERCENT(pct) ((UI_CANVAS_WIDTH * (pct) + 50) / 100)
#define UI_HUB_Y_FROM_BOTTOM_PERCENT(pct) (UI_CANVAS_HEIGHT - (UI_CANVAS_HEIGHT * (pct) + 50) / 100)
#define UI_KEY_QUEUE_TEXT_MAX UI_PASSWORD_TEXT_MAX
#define UI_CAPTURE_DEVICE_OPTIONS 8u
/* Both lists come from the selected camera, so they are sized for what a device can
 * report rather than for a fixed set of presets. The resolution cap is a display bound
 * only and is deliberately below NATIVE_CAMERA_FRAME_SIZE_MAX: the drawer filters the
 * full enumerated list by usable frame rate and keeps the first entries that pass, so a
 * camera with many sizes cannot hide a usable one behind unusable larger ones. */
#define UI_CAMERA_FPS_OPTION_MAX NATIVE_CAMERA_FRAME_RATE_MAX
#define UI_CAMERA_RESOLUTION_OPTION_MAX 12u

typedef struct NativePreconnectKeyDriver {
    lv_indev_drv_t base;
    lv_indev_state_t state;
    uint32_t key;
    bool back_pressed;
    NativeUiKeyQueue queue;
} NativePreconnectKeyDriver;

typedef struct NativeUiCube {
    lv_obj_t *obj;
    int32_t angle_tenths;
    float half_edge;
} NativeUiCube;

struct NativePreconnectUi {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    lv_disp_draw_buf_t draw_buf;
    lv_disp_drv_t disp_drv;
    lv_draw_sdl_drv_param_t draw_param;
    lv_disp_t *disp;
    lv_indev_drv_t pointer_drv;
    lv_indev_t *pointer_indev;
    NativePreconnectUiBackgroundDrawFn background_draw;
    void *background_draw_ctx;
    bool hardware_video_plane;
    NativePreconnectKeyDriver key_drv;
    lv_indev_t *key_indev;
    lv_group_t *group;
    lv_obj_t *root;
    lv_obj_t *slot_buttons[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *card_badges[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *card_badge_labels[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *card_name_labels[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *card_tabs[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *hero_chip;
    lv_obj_t *hero_slot_label;
    lv_obj_t *hero_name_label;
    lv_obj_t *hero_state_dot;
    lv_obj_t *hero_state_label;
    lv_obj_t *hero_meta_label;
    lv_obj_t *hero_detail_panel;
    lv_obj_t *hero_detail_label;
    lv_obj_t *hero_audio_group;
    lv_obj_t *hero_audio_label;
    lv_obj_t *hero_action_btn;
    lv_obj_t *hero_action_label;
    lv_obj_t *hero_edit_btn;
    lv_obj_t *keyboard_pill;
    lv_obj_t *keyboard_dot;
    lv_obj_t *mouse_dot;
    lv_obj_t *keyboard_warning;
    lv_obj_t *keyboard_warning_label;
    lv_obj_t *capture_settings_btn;
    lv_obj_t *help_btn;
    NativeUiCube brand_cube;
    lv_obj_t *setup_scrim;
    lv_obj_t *setup_panel;
    lv_obj_t *form_title;
    lv_obj_t *name_input;
    lv_obj_t *host_input;
    lv_obj_t *port_input;
    lv_obj_t *color_choices[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *username_input;
    lv_obj_t *domain_input;
    lv_obj_t *password_input;
    lv_obj_t *fps_dropdown;
    lv_obj_t *desktop_dropdown;
    lv_obj_t *audio_codec_dropdown;
    lv_obj_t *profile_camera_checkbox;
    lv_obj_t *profile_audio_input_checkbox;
    lv_obj_t *profile_capture_hint;
    lv_obj_t *capture_scrim;
    lv_obj_t *capture_panel;
    lv_obj_t *camera_input_dropdown;
    lv_obj_t *audio_input_dropdown;
    lv_obj_t *camera_resolution_dropdown;
    lv_obj_t *camera_fps_dropdown;
    lv_obj_t *audio_input_gain_dropdown;
    lv_obj_t *capture_hint;
    lv_obj_t *capture_save_btn;
    lv_obj_t *capture_cancel_btn;
    lv_obj_t *capture_status_label;
    lv_obj_t *connect_btn;
    lv_obj_t *save_btn;
    lv_obj_t *cancel_btn;
    lv_obj_t *delete_btn;
    lv_obj_t *delete_label;
    lv_obj_t *status_label;
    lv_obj_t *onboarding_scrim;
    lv_obj_t *onboarding_btn;
    NativeUiCube onboarding_cube;
    lv_obj_t *card_audio_groups[NATIVE_SETTINGS_MAX_SESSIONS];
    NativeUiVuMeter card_audio_meters[NATIVE_SETTINGS_MAX_SESSIONS][UI_CARD_AUDIO_METER_COUNT];
    int32_t slot_audio_peaks[NATIVE_SETTINGS_MAX_SESSIONS][UI_CARD_AUDIO_METER_COUNT];
    uint32_t card_audio_meter_ticks;
    lv_grad_dsc_t hub_horizontal_gradients[UI_HUB_MASK_SEGMENT_COUNT];
    lv_grad_dsc_t hub_vertical_gradients[UI_HUB_MASK_SEGMENT_COUNT];
    NativeUiMixer *mixer;
    lv_style_t root_style;
    lv_style_t nav_style;
    lv_style_t detail_style;
    lv_style_t title_style;
    lv_style_t hero_title_style;
    lv_style_t label_style;
    lv_style_t eyebrow_style;
    lv_style_t muted_style;
    lv_style_t input_style;
    lv_style_t input_focus_style;
    lv_style_t input_cursor_style;
    lv_style_t dropdown_style;
    lv_style_t dropdown_focus_style;
    lv_style_t dropdown_list_style;
    lv_style_t dropdown_selected_style;
    lv_style_t button_style;
    lv_style_t secondary_button_style;
    lv_style_t button_focus_style;
    lv_style_t button_disabled_style;
    lv_style_t card_style;
    lv_style_t card_focus_style;
    lv_style_t status_style;
    int width;
    int height;
    int pointer_x;
    int pointer_y;
    bool pointer_pressed;
    bool visible;
    bool hidden_cleared;
    bool connecting;
    bool connect_requested;
    bool activate_requested;
    bool hub_close_requested;
    bool hub_closing;
    bool save_requested;
    bool delete_requested;
    bool save_pending;
    bool connect_save_pending;
    bool delete_pending;
    bool delete_armed;
    bool loading_form;
    bool rebuilding_group;
    bool setup_visible;
    bool capture_settings_visible;
    bool onboarding_visible;
    bool keyboard_available;
    bool current_fps_option;
    bool current_camera_resolution_option;
    bool current_camera_fps_option;
    bool camera_current_resolution_available;
    bool camera_current_fps_available;
    bool current_audio_input_gain_option;
    uint16_t current_fps;
    uint16_t selected_fps;
    uint16_t selected_desktop_width;
    uint16_t selected_desktop_height;
    NativeSessionConfig slot_values[NATIVE_SETTINGS_MAX_SESSIONS];
    NativeSessionConfig committed_values[NATIVE_SETTINGS_MAX_SESSIONS];
    char slot_port_text[NATIVE_SETTINGS_MAX_SESSIONS][UI_PORT_MAX];
    bool slot_port_valid[NATIVE_SETTINGS_MAX_SESSIONS];
    NativePreconnectSessionState slot_states[NATIVE_SETTINGS_MAX_SESSIONS];
    RdpDisconnectReason slot_reasons[NATIVE_SETTINGS_MAX_SESSIONS];
    char slot_details[NATIVE_SETTINGS_MAX_SESSIONS][UI_DETAIL_MAX];
    uint16_t slot_desktop_width[NATIVE_SETTINGS_MAX_SESSIONS];
    uint16_t slot_desktop_height[NATIVE_SETTINGS_MAX_SESSIONS];
    uint32_t slot_session_minutes[NATIVE_SETTINGS_MAX_SESSIONS];
    bool slot_audio_stream_open[NATIVE_SETTINGS_MAX_SESSIONS];
    uint32_t slot_audio_codec[NATIVE_SETTINGS_MAX_SESSIONS];
    uint32_t slot_audio_sample_rate[NATIVE_SETTINGS_MAX_SESSIONS];
    uint16_t slot_audio_channels[NATIVE_SETTINGS_MAX_SESSIONS];
    int selected_slot;
    int requested_slot;
    int activated_slot;
    int saved_slot;
    int deleted_slot;
    NativePreconnectSessionState requested_previous_state;
    RdpDisconnectReason requested_previous_reason;
    char requested_previous_detail[UI_DETAIL_MAX];
    char requested_host[UI_HOST_MAX];
    char requested_username[UI_USERNAME_MAX];
    char requested_domain[UI_DOMAIN_MAX];
    char requested_password[UI_PASSWORD_MAX];
    uint16_t requested_port;
    uint16_t requested_fps;
    uint16_t requested_audio_codec;
    uint16_t committed_audio_codec;
    NativeCameraDeviceInfo camera_devices[UI_CAPTURE_DEVICE_OPTIONS];
    size_t camera_device_count;
    NativeAudioInputDeviceInfo audio_input_devices[UI_CAPTURE_DEVICE_OPTIONS];
    size_t audio_input_device_count;
    bool camera_enabled;
    char camera_device_id[NATIVE_SETTINGS_STRING_MAX];
    uint16_t camera_width;
    uint16_t camera_height;
    uint16_t camera_fps;
    uint16_t camera_fps_options[UI_CAMERA_FPS_OPTION_MAX];
    size_t camera_fps_option_count;
    uint16_t camera_resolution_options[UI_CAMERA_RESOLUTION_OPTION_MAX][2];
    size_t camera_resolution_option_count;
    bool audio_input_enabled;
    char audio_input_device_id[NATIVE_SETTINGS_STRING_MAX];
    int16_t audio_input_gain_db;
    bool committed_camera_enabled;
    char committed_camera_device_id[NATIVE_SETTINGS_STRING_MAX];
    bool committed_audio_input_enabled;
    char committed_audio_input_device_id[NATIVE_SETTINGS_STRING_MAX];
    uint16_t committed_camera_width;
    uint16_t committed_camera_height;
    uint16_t committed_camera_fps;
    int16_t committed_audio_input_gain_db;
    bool capture_save_requested;
    bool capture_save_pending;
    bool requested_requires_save;
    NativeUiScreenId screen;
    NativeUiScreenId return_screen;
};

void native_ui_preconnect_theme_init(NativePreconnectUi *ui);
void native_ui_preconnect_theme_reset(NativePreconnectUi *ui);
void native_ui_preconnect_input_changed(lv_event_t *event);
void native_ui_preconnect_form_key_event(lv_event_t *event);
void native_ui_preconnect_back(NativePreconnectUi *ui);
lv_obj_t *native_ui_preconnect_make_label(lv_obj_t *parent, const char *text, lv_style_t *style);
lv_obj_t *native_ui_preconnect_make_dropdown(NativePreconnectUi *ui, lv_obj_t *parent, lv_coord_t width);
lv_obj_t *native_ui_preconnect_make_box(lv_obj_t *parent, int x, int y, int width, int height);
lv_obj_t *native_ui_preconnect_make_alpha_gradient(lv_obj_t *parent, int x, int y, int width, int height,
                                                   lv_grad_dir_t direction, lv_opa_t start_alpha, lv_opa_t end_alpha,
                                                   lv_grad_dsc_t *gradient);
lv_obj_t *native_ui_preconnect_make_audio_indicator(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y,
                                                    bool compact, NativeUiVuMeter meters[UI_CARD_AUDIO_METER_COUNT],
                                                    lv_obj_t **label_out);
lv_obj_t *native_ui_preconnect_make_button(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, int width,
                                           int height, const char *text, bool primary, lv_event_cb_t callback);
lv_obj_t *native_ui_preconnect_make_field_label(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, int width,
                                                const char *text);
lv_obj_t *native_ui_preconnect_make_input(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, int width,
                                          const char *text, const char *placeholder, size_t max_length,
                                          const char *accepted, bool password);
void native_ui_preconnect_make_brand_cube(lv_obj_t *parent, int x, int y, int size, float half_edge,
                                          NativeUiCube *cube);
void native_ui_preconnect_make_wordmark(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, const lv_font_t *font,
                                        lv_opa_t underscore_opa);
bool ui_parse_port(const char *text, uint16_t *port);
bool ui_host_valid(const char *host);
size_t ui_fps_option_count(void);
bool ui_find_fps_option(uint16_t fps, size_t *index);
size_t ui_select_fps_index(NativePreconnectUi *ui, uint16_t fps);
void ui_set_selected_fps(NativePreconnectUi *ui, size_t index);

typedef struct UiDesktopSize {
    uint16_t width;
    uint16_t height;
} UiDesktopSize;

size_t ui_desktop_option_count(void);
UiDesktopSize ui_desktop_option(size_t index);
size_t ui_desktop_option_index(uint16_t width, uint16_t height);
void ui_set_desktop_options(NativePreconnectUi *ui);
void ui_set_selected_desktop(NativePreconnectUi *ui, size_t index);
void ui_desktop_changed(lv_event_t *event);
void ui_set_fps_options(NativePreconnectUi *ui);
bool ui_form_valid(NativePreconnectUi *ui);
void ui_set_hidden(lv_obj_t *obj, bool hidden);
void ui_set_disabled(lv_obj_t *obj, bool disabled);
bool ui_profile_action_pending(const NativePreconnectUi *ui);
bool ui_any_action_pending(const NativePreconnectUi *ui);
bool ui_drawer_open(const NativePreconnectUi *ui);
void ui_update_connect_state(NativePreconnectUi *ui);
void ui_set_text(lv_obj_t *label, const char *text);
uint16_t ui_current_audio_codec(const NativePreconnectUi *ui);
void ui_sanitize_option(char *text);
void ui_enumerate_capture_devices(NativePreconnectUi *ui);
void ui_set_capture_options(NativePreconnectUi *ui);
uint16_t ui_capture_selected_index(const char *device_id, const char *const *ids, size_t count);
uint16_t ui_camera_selected_index(const NativePreconnectUi *ui);
uint16_t ui_audio_input_selected_index(const NativePreconnectUi *ui);
void ui_recompute_capture_enabled(NativePreconnectUi *ui);
void ui_store_profile_capture_toggles(NativePreconnectUi *ui, int slot);
void ui_refresh_camera_resolution_options(NativePreconnectUi *ui);
void ui_set_camera_resolution_options(NativePreconnectUi *ui);
void ui_refresh_camera_fps_options(NativePreconnectUi *ui);
void ui_set_camera_fps_options(NativePreconnectUi *ui);
void ui_set_audio_input_gain_options(NativePreconnectUi *ui);
void ui_load_capture_settings_form(NativePreconnectUi *ui);
void ui_restore_capture_settings(NativePreconnectUi *ui);
void ui_commit_capture_settings(NativePreconnectUi *ui);
void ui_store_capture_settings_form(NativePreconnectUi *ui);
bool ui_profile_dirty(const NativePreconnectUi *ui, int slot);
bool ui_slot_configured(const NativeSessionConfig *values);
const char *ui_slot_display_name(const NativePreconnectUi *ui, int slot, char *fallback, size_t fallback_cap);
void ui_discard_form_changes(NativePreconnectUi *ui);
void ui_load_slot_into_form(NativePreconnectUi *ui, int slot);
bool ui_hub_navigate(NativePreconnectUi *ui, lv_obj_t *target, uint32_t key);
void ui_profile_name_insert(lv_event_t *event);
void ui_fps_changed(lv_event_t *event);
void ui_profile_capture_changed(lv_event_t *event);
void ui_capture_settings_changed(lv_event_t *event);
void ui_hub_key_event(lv_event_t *event);
void ui_connect_clicked(lv_event_t *event);
void ui_save_clicked(lv_event_t *event);
void ui_cancel_clicked(lv_event_t *event);
void ui_cancel_setup(NativePreconnectUi *ui);
void ui_delete_clicked(lv_event_t *event);
void ui_setup_scrim_clicked(lv_event_t *event);
void ui_capture_scrim_clicked(lv_event_t *event);
void ui_capture_settings_clicked(lv_event_t *event);
void ui_capture_save_clicked(lv_event_t *event);
void ui_capture_cancel_clicked(lv_event_t *event);
void ui_cancel_capture_settings(NativePreconnectUi *ui);
void ui_edit_clicked(lv_event_t *event);
void ui_hero_action_clicked(lv_event_t *event);
void ui_help_clicked(lv_event_t *event);
void ui_onboarding_close_clicked(lv_event_t *event);
void ui_slot_button_clicked(lv_event_t *event);
void ui_slot_button_focused(lv_event_t *event);
void ui_update_hub(NativePreconnectUi *ui);
void ui_show_setup(NativePreconnectUi *ui, bool visible);
void ui_show_capture_settings(NativePreconnectUi *ui, bool visible);
void ui_show_onboarding(NativePreconnectUi *ui, bool visible);
void native_ui_preconnect_build(NativePreconnectUi *ui, const char *host, uint16_t port, const char *username,
                                const char *password, const char *domain, uint16_t fps, uint16_t desktop_width,
                                uint16_t desktop_height, uint16_t audio_codec);
void native_ui_preconnect_build_onboarding(NativePreconnectUi *ui);
void native_ui_preconnect_screen_init(NativePreconnectUi *ui, NativeUiScreenId initial);
void native_ui_preconnect_screen_refocus(NativePreconnectUi *ui);

#endif
