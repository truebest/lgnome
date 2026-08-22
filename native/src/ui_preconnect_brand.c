#include "ui_preconnect_internal.h"

#include <math.h>
#include <string.h>

#include "ui_fonts.h"
#include "ui_slot_palette.h"

#include "clog.h"

clog_define(g_native_log_ui_brand, cLogLevelInfo, cLogFlags_Default, "ui.brand", NULL);

typedef struct UiCubeVec3 {
    float x;
    float y;
    float z;
} UiCubeVec3;

typedef struct UiCubeFace {
    uint8_t vertices[4];
    UiCubeVec3 normal;
    uint32_t color;
    float depth;
} UiCubeFace;

static UiCubeVec3 ui_cube_rotate(UiCubeVec3 point, float sin_y, float cos_y) {
    const float sin_x = -0.43837115f; /* rotateX(-26deg), matching the HTML prototype */
    const float cos_x = 0.89879405f;
    float x1 = point.x * cos_y + point.z * sin_y;
    float z1 = -point.x * sin_y + point.z * cos_y;
    UiCubeVec3 result = {x1, point.y * cos_x - z1 * sin_x, point.y * sin_x + z1 * cos_x};
    return result;
}

static void ui_brand_cube_draw(lv_event_t *event) {
    NativeUiCube *cube = (NativeUiCube *)lv_event_get_user_data(event);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    if (!cube || !cube->obj || !draw_ctx) {
        return;
    }

    static const UiCubeVec3 base_vertices[8] = {
        {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
        {-1.0f, -1.0f, 1.0f},  {1.0f, -1.0f, 1.0f},  {1.0f, 1.0f, 1.0f},  {-1.0f, 1.0f, 1.0f},
    };
    static const UiCubeFace face_templates[6] = {
        {{4, 5, 6, 7}, {0.0f, 0.0f, 1.0f}, NATIVE_UI_SLOT_RED_RGB, 0.0f},     /* front / red */
        {{5, 1, 2, 6}, {1.0f, 0.0f, 0.0f}, NATIVE_UI_SLOT_BLUE_RGB, 0.0f},    /* right / blue */
        {{1, 0, 3, 2}, {0.0f, 0.0f, -1.0f}, NATIVE_UI_SLOT_GREEN_RGB, 0.0f},  /* back / green */
        {{0, 4, 7, 3}, {-1.0f, 0.0f, 0.0f}, NATIVE_UI_SLOT_YELLOW_RGB, 0.0f}, /* left / yellow */
        {{0, 1, 5, 4}, {0.0f, -1.0f, 0.0f}, 0x2a303c, 0.0f},                  /* top */
        {{7, 6, 2, 3}, {0.0f, 1.0f, 0.0f}, 0x12151b, 0.0f},                   /* bottom */
    };

    float radians = (float)cube->angle_tenths * (3.14159265359f / 1800.0f);
    float sin_y = sinf(radians);
    float cos_y = cosf(radians);
    UiCubeVec3 vertices[8];
    for (size_t i = 0; i < 8; i++) {
        vertices[i] = ui_cube_rotate(base_vertices[i], sin_y, cos_y);
    }

    UiCubeFace visible[6];
    size_t visible_count = 0;
    for (size_t i = 0; i < 6; i++) {
        UiCubeVec3 normal = ui_cube_rotate(face_templates[i].normal, sin_y, cos_y);
        if (normal.z <= 0.001f) {
            continue;
        }
        visible[visible_count] = face_templates[i];
        visible[visible_count].depth = 0.0f;
        for (size_t p = 0; p < 4; p++) {
            visible[visible_count].depth += vertices[visible[visible_count].vertices[p]].z;
        }
        visible[visible_count].depth *= 0.25f;
        visible_count++;
    }
    for (size_t i = 1; i < visible_count; i++) {
        UiCubeFace face = visible[i];
        size_t j = i;
        while (j > 0 && visible[j - 1].depth > face.depth) {
            visible[j] = visible[j - 1];
            j--;
        }
        visible[j] = face;
    }

    lv_area_t area;
    lv_obj_get_coords(cube->obj, &area);
    float center_x = ((float)area.x1 + (float)area.x2) * 0.5f;
    float center_y = ((float)area.y1 + (float)area.y2) * 0.5f;
    for (size_t i = 0; i < visible_count; i++) {
        lv_point_t points[4];
        for (size_t p = 0; p < 4; p++) {
            UiCubeVec3 point = vertices[visible[i].vertices[p]];
            points[p].x = (lv_coord_t)lroundf(center_x + point.x * cube->half_edge);
            points[p].y = (lv_coord_t)lroundf(center_y + point.y * cube->half_edge);
        }
        lv_draw_rect_dsc_t draw_dsc;
        lv_draw_rect_dsc_init(&draw_dsc);
        draw_dsc.bg_color = lv_color_hex(visible[i].color);
        draw_dsc.bg_opa = LV_OPA_COVER;
        lv_draw_polygon(draw_ctx, &draw_dsc, points, 4);
    }
}

static void ui_brand_cube_anim_exec(void *object, int32_t angle_tenths) {
    lv_obj_t *obj = (lv_obj_t *)object;
    NativeUiCube *cube = obj ? (NativeUiCube *)lv_obj_get_user_data(obj) : NULL;
    if (!cube) {
        return;
    }
    cube->angle_tenths = angle_tenths;
    lv_obj_invalidate(obj);
}

void native_ui_preconnect_make_brand_cube(lv_obj_t *parent, int x, int y, int size, float half_edge,
                                          NativeUiCube *cube) {
    memset(cube, 0, sizeof(*cube));
    cube->half_edge = half_edge;
    cube->obj = native_ui_preconnect_make_box(parent, x, y, size, size);
    lv_obj_clear_flag(cube->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(cube->obj, cube);
    lv_obj_add_event_cb(cube->obj, ui_brand_cube_draw, LV_EVENT_DRAW_MAIN, cube);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, cube->obj);
    lv_anim_set_exec_cb(&animation, ui_brand_cube_anim_exec);
    lv_anim_set_values(&animation, 0, 3599);
    lv_anim_set_time(&animation, 14000);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&animation);
    clog(cLogLevelTrace, "brand cube created");
}

void native_ui_preconnect_make_wordmark(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, const lv_font_t *font,
                                        lv_opa_t underscore_opa) {
    lv_obj_t *word = native_ui_preconnect_make_label(parent, "gnomecast", &ui->title_style);
    lv_obj_set_pos(word, x, y);
    lv_obj_set_style_text_font(word, font, 0);
    lv_obj_set_style_text_letter_space(word, -1, 0);
    lv_obj_update_layout(word);

    lv_obj_t *underscore = native_ui_preconnect_make_label(parent, "_", &ui->title_style);
    lv_obj_set_pos(underscore, x + lv_obj_get_width(word), y);
    lv_obj_set_style_text_font(underscore, font, 0);
    lv_obj_set_style_text_letter_space(underscore, -1, 0);
    lv_obj_set_style_text_opa(underscore, underscore_opa, 0);
}
