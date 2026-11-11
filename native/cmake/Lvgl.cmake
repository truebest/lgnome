set(_lvgl_revision "185ea1fc61dd01fac61867d2d6b56892e80c6058")
get_filename_component(_lvgl_cache "${CMAKE_CURRENT_LIST_DIR}/../../build/deps" ABSOLUTE)
set(LGNOME_LVGL_SOURCE_DIR "${_lvgl_cache}/lvgl-${_lvgl_revision}")
file(MAKE_DIRECTORY "${_lvgl_cache}")
file(LOCK "${_lvgl_cache}/lvgl.lock" GUARD PROCESS TIMEOUT 180)
file(DOWNLOAD
  "https://github.com/mariotaku/lvgl/archive/${_lvgl_revision}.zip"
  "${_lvgl_cache}/lvgl-${_lvgl_revision}.zip"
  EXPECTED_HASH SHA256=e3c54648b975212d423802c105d654db374d3f09b5938f996cdb4299ca77243b
  TLS_VERIFY ON TIMEOUT 180 INACTIVITY_TIMEOUT 30
)
if(NOT EXISTS "${LGNOME_LVGL_SOURCE_DIR}/.lgnome-extracted")
  file(REMOVE_RECURSE "${LGNOME_LVGL_SOURCE_DIR}")
  file(ARCHIVE_EXTRACT INPUT "${_lvgl_cache}/lvgl-${_lvgl_revision}.zip"
    DESTINATION "${_lvgl_cache}")
  file(WRITE "${LGNOME_LVGL_SOURCE_DIR}/.lgnome-extracted" "")
endif()
file(LOCK "${_lvgl_cache}/lvgl.lock" RELEASE)
if(CMAKE_SCRIPT_MODE_FILE)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${LGNOME_LVGL_SOURCE_DIR}")
endif()
