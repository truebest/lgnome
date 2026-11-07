# LVGL 8.3's SDL line implementation still takes a fifth argument; draw_ctx passes four.
function(lgnome_lvgl_sdl_line output)
  set(original "${LGNOME_LVGL_SOURCE_DIR}/src/draw/sdl/lv_draw_sdl_line.c")
  file(SHA256 "${original}" hash)
  if(NOT hash STREQUAL "3a3855045b758b717cfaf6578a26dc365ab45edfafd87a2c7da95109f23144bd")
    message(FATAL_ERROR "LVGL SDL line source changed; review/remove the callback ABI compatibility fix")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
  file(READ "${original}" source)
  string(REPLACE "point2, bool * in_cache)" "point2)" source "${source}")
  string(REPLACE "    SDL_Texture * texture = lv_draw_sdl_texture_cache_get"
                 "    bool in_cache = true;\n    SDL_Texture * texture = lv_draw_sdl_texture_cache_get" source "${source}")
  string(REPLACE "        *in_cache =" "        if(!texture) return;\n        in_cache =" source "${source}")
  string(REPLACE "    lv_draw_sdl_composite_end(sdl_ctx, &apply_area, dsc->blend_mode);"
                 "    lv_draw_sdl_composite_end(sdl_ctx, &apply_area, dsc->blend_mode);\n    if(!in_cache) SDL_DestroyTexture(texture);" source "${source}")
  string(REPLACE "    SDL_Texture * target = SDL_GetRenderTarget"
                 "    if(!texture) return NULL;\n    SDL_Texture * target = SDL_GetRenderTarget" source "${source}")
  set(patched "${CMAKE_CURRENT_BINARY_DIR}/lvgl_sdl_line.c")
  file(CONFIGURE OUTPUT "${patched}" CONTENT "${source}" @ONLY)
  set(${output} "${patched}" PARENT_SCOPE)
endfunction()
