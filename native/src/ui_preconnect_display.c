#include "ui_preconnect_internal.h"

#include <string.h>

#include "clog.h"

clog_define(g_native_log_ui_display, cLogLevelInfo, cLogFlags_Default, "ui.display", NULL);

void native_ui_preconnect_display_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *src) {
    (void)src;
    if (area->x2 < 0 || area->y2 < 0 || area->x1 > drv->hor_res - 1 || area->y1 > drv->ver_res - 1) {
        lv_disp_flush_ready(drv);
        return;
    }
    if (lv_disp_flush_is_last(drv)) {
        lv_draw_sdl_drv_param_t *param = (lv_draw_sdl_drv_param_t *)drv->user_data;
        NativePreconnectUi *ui = (NativePreconnectUi *)param->user_data;
        SDL_SetRenderTarget(ui->renderer, NULL);
        SDL_SetRenderDrawBlendMode(ui->renderer, SDL_BLENDMODE_BLEND);
        bool software_background_drawn = ui->background_draw &&
                                         ui->background_draw(ui->background_draw_ctx, ui->renderer);
        if (!software_background_drawn) {
            bool live_plane = native_ui_mixer_active(ui->mixer) || ui->hardware_video_plane;
            if (live_plane) {
                SDL_SetRenderDrawColor(ui->renderer, 0, 0, 0, 0);
            } else {
                SDL_SetRenderDrawColor(ui->renderer, 0x07, 0x08, 0x0b, 0xff);
            }
            SDL_RenderClear(ui->renderer);
        }
        SDL_SetTextureBlendMode(ui->texture, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(ui->renderer, ui->texture, NULL, NULL);
        SDL_RenderPresent(ui->renderer);
        SDL_SetRenderTarget(ui->renderer, ui->texture);
    }
    lv_disp_flush_ready(drv);
}

void native_ui_preconnect_display_clear(lv_disp_drv_t *drv, uint8_t *buf, uint32_t size) {
    (void)drv;
    (void)buf;
    (void)size;
}

void native_ui_preconnect_pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    NativePreconnectUi *ui = (NativePreconnectUi *)drv->user_data;
    SDL_Event event;
    data->continue_reading = SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEBUTTONUP) > 0;
    if (data->continue_reading) {
        switch (event.type) {
        case SDL_MOUSEMOTION:
            ui->pointer_x = event.motion.x;
            ui->pointer_y = event.motion.y;
            ui->pointer_pressed = (event.motion.state & SDL_BUTTON_LMASK) != 0;
            break;
        case SDL_MOUSEBUTTONDOWN:
            ui->pointer_x = event.button.x;
            ui->pointer_y = event.button.y;
            if (event.button.button == SDL_BUTTON_LEFT) {
                ui->pointer_pressed = true;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            ui->pointer_x = event.button.x;
            ui->pointer_y = event.button.y;
            if (event.button.button == SDL_BUTTON_LEFT) {
                ui->pointer_pressed = false;
            }
            break;
        default:
            break;
        }
    }
    data->point.x = (lv_coord_t)ui->pointer_x;
    data->point.y = (lv_coord_t)ui->pointer_y;
    data->state = ui->pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static bool key_from_sdl(const SDL_KeyboardEvent *event, uint32_t *key) {
    if (event->keysym.scancode == 482 /* SDL_WEBOS_SCANCODE_BACK */) {
        *key = LV_KEY_ESC;
        return true;
    }
    switch (event->keysym.sym) {
    case SDLK_UP:
        *key = LV_KEY_UP;
        return true;
    case SDLK_DOWN:
        *key = LV_KEY_DOWN;
        return true;
    case SDLK_LEFT:
        *key = LV_KEY_LEFT;
        return true;
    case SDLK_RIGHT:
        *key = LV_KEY_RIGHT;
        return true;
    case SDLK_ESCAPE:
        *key = LV_KEY_ESC;
        return true;
    case SDLK_DELETE:
        *key = LV_KEY_DEL;
        return true;
    case SDLK_BACKSPACE:
        *key = LV_KEY_BACKSPACE;
        return true;
    case SDLK_KP_ENTER:
    case SDLK_RETURN:
    case SDLK_RETURN2:
        *key = LV_KEY_ENTER;
        return true;
    case SDLK_TAB:
        *key = (event->keysym.mod & KMOD_SHIFT) ? LV_KEY_PREV : LV_KEY_NEXT;
        return true;
    case SDLK_HOME:
        *key = LV_KEY_HOME;
        return true;
    case SDLK_END:
        *key = LV_KEY_END;
        return true;
    default:
        return false;
    }
}

static bool text_key_from_sdl(const SDL_KeyboardEvent *event, uint32_t *key) {
    if (!event || event->type != SDL_KEYDOWN || !key) {
        return false;
    }

    SDL_Keycode sym = event->keysym.sym;
    bool shift = (event->keysym.mod & KMOD_SHIFT) != 0;
    bool caps = (event->keysym.mod & KMOD_CAPS) != 0;

    if (sym >= SDLK_a && sym <= SDLK_z) {
        char ch = (char)('a' + (sym - SDLK_a));
        if (shift ^ caps) {
            ch = (char)('A' + (sym - SDLK_a));
        }
        *key = (uint8_t)ch;
        return true;
    }
    if (sym >= SDLK_0 && sym <= SDLK_9) {
        static const char shifted_digits[] = ")!@#$%^&*(";
        *key = (uint8_t)(shift ? shifted_digits[sym - SDLK_0] : (char)('0' + (sym - SDLK_0)));
        return true;
    }
    if (sym >= SDLK_KP_0 && sym <= SDLK_KP_9) {
        *key = (uint8_t)('0' + (sym - SDLK_KP_0));
        return true;
    }

    switch (sym) {
    case SDLK_SPACE:
        *key = ' ';
        return true;
    case SDLK_PERIOD:
    case SDLK_KP_PERIOD:
        *key = shift ? '>' : '.';
        return true;
    case SDLK_COMMA:
        *key = shift ? '<' : ',';
        return true;
    case SDLK_SEMICOLON:
        *key = shift ? ':' : ';';
        return true;
    case SDLK_COLON:
        *key = ':';
        return true;
    case SDLK_SLASH:
    case SDLK_KP_DIVIDE:
        *key = shift ? '?' : '/';
        return true;
    case SDLK_BACKSLASH:
        *key = shift ? '|' : '\\';
        return true;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        *key = shift ? '_' : '-';
        return true;
    case SDLK_EQUALS:
    case SDLK_KP_EQUALS:
        *key = shift ? '+' : '=';
        return true;
    case SDLK_LEFTBRACKET:
        *key = shift ? '{' : '[';
        return true;
    case SDLK_RIGHTBRACKET:
        *key = shift ? '}' : ']';
        return true;
    case SDLK_QUOTE:
        *key = shift ? '"' : '\'';
        return true;
    case SDLK_BACKQUOTE:
        *key = shift ? '~' : '`';
        return true;
    case SDLK_KP_MULTIPLY:
        *key = '*';
        return true;
    case SDLK_KP_PLUS:
        *key = '+';
        return true;
    default:
        return false;
    }
}

void native_ui_preconnect_reset_input_state(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    native_ui_key_queue_clear(&ui->key_drv.queue);
    ui->key_drv.key = 0;
    ui->key_drv.state = LV_INDEV_STATE_RELEASED;
    ui->pointer_pressed = false;
    if (ui->key_indev) {
        lv_indev_reset(ui->key_indev, NULL);
        lv_indev_wait_release(ui->key_indev);
    }
    if (ui->pointer_indev) {
        lv_indev_reset(ui->pointer_indev, NULL);
    }
}

static void enqueue_key(NativePreconnectKeyDriver *state, uint32_t key, lv_indev_state_t key_state) {
    (void)native_ui_key_queue_enqueue(&state->queue, key, (uint32_t)key_state);
}

static void enqueue_tap(NativePreconnectKeyDriver *state, uint32_t key) {
    enqueue_key(state, key, LV_INDEV_STATE_PRESSED);
    enqueue_key(state, key, LV_INDEV_STATE_RELEASED);
}

static size_t utf8_char_len(unsigned char first) {
    if ((first & 0x80u) == 0) {
        return 1;
    }
    if ((first & 0xe0u) == 0xc0u) {
        return 2;
    }
    if ((first & 0xf0u) == 0xe0u) {
        return 3;
    }
    if ((first & 0xf8u) == 0xf0u) {
        return 4;
    }
    return 1;
}

static void enqueue_text(NativePreconnectKeyDriver *state, const char *value) {
    if (!value) {
        return;
    }
    size_t len = strlen(value);
    for (size_t i = 0; i < len;) {
        size_t char_len = utf8_char_len((unsigned char)value[i]);
        if (char_len > len - i) {
            char_len = 1;
        }
        uint32_t key = 0;
        memcpy(&key, value + i, char_len);
        enqueue_tap(state, key);
        i += char_len;
    }
}

static bool event_batch_has_textinput(const SDL_Event *events, int count) {
    for (int i = 0; i < count; i++) {
        if (events[i].type == SDL_TEXTINPUT) {
            return true;
        }
    }
    return false;
}

static void drain_sdl_key_events(NativePreconnectKeyDriver *state) {
    SDL_Event events[64];
    for (;;) {
        int count = SDL_PeepEvents(events, (int)(sizeof(events) / sizeof(events[0])), SDL_GETEVENT, SDL_KEYDOWN,
                                   SDL_TEXTINPUT);
        if (count <= 0) {
            break;
        }

        bool text_input_pending = event_batch_has_textinput(events, count) || SDL_HasEvent(SDL_TEXTINPUT);
        for (int i = 0; i < count; i++) {
            uint32_t key = 0;
            switch (events[i].type) {
            case SDL_TEXTINPUT:
                enqueue_text(state, events[i].text.text);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (key_from_sdl(&events[i].key, &key)) {
                    enqueue_key(state, key,
                                events[i].type == SDL_KEYDOWN ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED);
                } else if (events[i].type == SDL_KEYDOWN && !text_input_pending &&
                           text_key_from_sdl(&events[i].key, &key)) {
                    enqueue_tap(state, key);
                }
                break;
            default:
                break;
            }
        }
    }
}

void native_ui_preconnect_key_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    NativePreconnectUi *ui = (NativePreconnectUi *)drv->user_data;
    NativePreconnectKeyDriver *state = (NativePreconnectKeyDriver *)drv;

    if (ui->connecting) {
        SDL_FlushEvents(SDL_KEYDOWN, SDL_TEXTINPUT);
        native_ui_key_queue_clear(&state->queue);
        state->state = LV_INDEV_STATE_RELEASED;
        data->key = state->key;
        data->state = state->state;
        data->continue_reading = false;
        return;
    }

    drain_sdl_key_events(state);
    if (state->queue.overflowed) {
        clog(cLogLevelWarning, "pre-connect key queue overflow; dropping burst and resetting key input");
        native_ui_key_queue_clear(&state->queue);
        state->key = 0;
        state->state = LV_INDEV_STATE_RELEASED;
        data->key = state->key;
        data->state = state->state;
        data->continue_reading = false;
        return;
    }

    NativeUiQueuedKey event;
    if (native_ui_key_queue_dequeue(&state->queue, &event)) {
        state->key = event.key;
        state->state = (lv_indev_state_t)event.state;
    }
    data->key = state->key;
    data->state = state->state;
    data->continue_reading = !native_ui_key_queue_empty(&state->queue);
}
