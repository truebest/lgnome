cmake_minimum_required(VERSION 3.19)
project(lgnome_native C)
include(CTest)
include(GNUInstallDirs)
include(CheckSymbolExists)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(LGNOME_LINK_RDP_FFI "Link the Rust RDP FFI static library" OFF)
option(LGNOME_WITH_NDL "Build the NDL DirectMedia hardware media backend (webOS)" OFF)

# The DirectMedia state machine is a standalone subproject. Only the three
# NativeMedia/NativeVideo/NativeAudio adapters remain owned by lgnome.
set(BACKEND_NDL_BUILD_TESTS ${BUILD_TESTING})
set(BACKEND_NDL_INSTALL OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../third_party/backend_ndl"
                 "${CMAKE_CURRENT_BINARY_DIR}/backend_ndl")

check_symbol_exists(clock_gettime "time.h" CLOGX_HAVE_CLOCK_GETTIME)

add_library(category-log STATIC src/category_log.c)
target_include_directories(category-log PUBLIC include)
target_compile_definitions(category-log PRIVATE
  $<$<BOOL:${CLOGX_HAVE_CLOCK_GETTIME}>:CLOGX_HAVE_CLOCK_GETTIME=1>
)

set(LGNOME_LVGL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lvgl" CACHE PATH "Path to the pinned LVGL checkout")
set(LGNOME_MINIAUDIO_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio" CACHE PATH "Path to pinned miniaudio sources")

get_filename_component(LGNOME_C_COMPILER_NAME "${CMAKE_C_COMPILER}" NAME)
set(LGNOME_TARGET_WEBOS OFF)
if(LGNOME_C_COMPILER_NAME MATCHES "arm-webos-linux-gnueabi-.*" OR CMAKE_C_COMPILER_TARGET MATCHES "(i[3-6]86)-webos-linux-gnu")
  set(LGNOME_TARGET_WEBOS ON)
endif()

# Default ON only for product (webOS) builds: ExternalOPUS downloads the libopus source
# tarball at build time, which would break the documented offline host smoke build.
# Host test runs opt in with -DLGNOME_WITH_OPUS=ON to cover the audio-opus ctest.
option(LGNOME_WITH_OPUS "Decode rdpsnd Opus streams in-process for the audio pipeline" ${LGNOME_TARGET_WEBOS})

if(LGNOME_TARGET_WEBOS)
  if(NOT LGNOME_LINK_RDP_FFI)
    message(FATAL_ERROR "webOS product builds require LGNOME_LINK_RDP_FFI=ON")
  endif()
  # System-volume bridge (mixer MASTER fader) talks to the Luna bus via the NATIVE
  # luna-service2 client. A luna-send-pub subprocess is NOT an option: fork()ing this
  # process while the NDL/OMX pipeline is (re)loading raced its in-process decoder
  # state (black screens observed live), and a pipe cannot deliver subscribe events.
  if(NOT LGNOME_WITH_NDL)
    message(FATAL_ERROR "webOS product builds require LGNOME_WITH_NDL=ON for H.264 hardware decode")
  endif()
endif()

add_executable(lgnome-native
  src/main.c
  src/rdp_log.c
  src/au_snapshot.c
  src/camera_v4l2.c
  src/camera_h264_stream.c
  src/audio_input_alsa.c
  src/audio_input_pcm.c
  src/capture_audio_worker.c
  src/capture_camera_worker.c
  src/capture_redirect.c
  src/capture_slots.c
  src/luna_volume.c
  src/webos_platform.c
  src/config_paths.c
  src/native_config.c
  src/native_config_launch.c
  src/native_config_storage.c
  src/native_json.c
  src/native_capture_glue.c
  src/native_hub_actions.c
  src/native_hub_actions_connect.c
  src/native_hub_actions_navigation.c
  src/native_hub_actions_profiles.c
  src/native_input_route.c
  src/native_lifecycle.c
  src/native_hub_runtime.c
  src/native_loop.c
  src/native_session_supervisor.c
  src/native_streaming_transition.c
  src/native_mixer_overlay.c
  src/native_pointer.c
  src/native_presentation.c
  src/native_settings.c
  src/native_rdp_audio.c
  src/native_rdp_callbacks.c
  src/native_rdp_media.c
  src/native_rdp_state.c
  src/native_rdp_video.c
  src/native_rgba_owner.c
  src/native_sdl_events.c
  src/native_session.c
  src/native_switch.c
  src/native_switch_handoff.c
  src/native_switch_navigation.c
  src/native_switch_recovery.c
  src/settings_json.c
  src/video_rgba_sdl.c
  src/audio_pipeline.c
  src/audio_pipeline_source.c
  src/audio_jitter.c
  src/audio_opus.c
  src/h264_annexb.c
  src/input_sdl.c
  src/cursor_sdl.c
  src/camera_preview.c
  src/ui_mixer.c
  src/ui_mixer_geometry.c
  src/ui_vu_meter.c
  src/ui_key_queue.c
)

find_package(Threads REQUIRED)

add_library(lgnome-ndl-adapter STATIC
  src/ndl_adapter/media_ndl.c
  src/ndl_adapter/video_ndl.c
  src/ndl_adapter/audio_ndl.c
)
target_include_directories(lgnome-ndl-adapter
  PUBLIC include
  PRIVATE src/ndl_adapter
)
target_compile_definitions(lgnome-ndl-adapter PRIVATE
  $<$<BOOL:${LGNOME_WITH_NDL}>:LGNOME_WITH_NDL=1>
)
target_link_libraries(lgnome-ndl-adapter PRIVATE backend_ndl::backend_ndl category-log)

if(NOT EXISTS "${LGNOME_MINIAUDIO_SOURCE_DIR}/miniaudio.c" OR
   NOT EXISTS "${LGNOME_MINIAUDIO_SOURCE_DIR}/miniaudio.h")
  message(FATAL_ERROR "Pinned miniaudio sources are missing from ${LGNOME_MINIAUDIO_SOURCE_DIR}")
endif()
add_library(lgnome-miniaudio STATIC "${LGNOME_MINIAUDIO_SOURCE_DIR}/miniaudio.c")
target_include_directories(lgnome-miniaudio SYSTEM PUBLIC "${LGNOME_MINIAUDIO_SOURCE_DIR}")
target_compile_definitions(lgnome-miniaudio PRIVATE
  # ma_engine owns the headless mixer graph and ma_sound voices. Keep both the engine and
  # node graph enabled even though device I/O and file/resource loading stay disabled.
  MA_NO_DEVICE_IO
  MA_NO_DECODING
  MA_NO_ENCODING
  MA_NO_RESOURCE_MANAGER
  MA_NO_GENERATION
  MA_NO_THREADING
  MA_NO_RUNTIME_LINKING
)
if(LGNOME_TARGET_WEBOS)
  # miniaudio's node clocks use 64-bit atomics; armv7 GCC lowers those through libatomic.
  target_link_libraries(lgnome-miniaudio PUBLIC atomic)
endif()

target_include_directories(lgnome-native PRIVATE include)
target_compile_definitions(lgnome-native PRIVATE
  $<$<BOOL:${LGNOME_TARGET_WEBOS}>:LGNOME_TARGET_WEBOS=1>
  $<$<BOOL:${LGNOME_WITH_NDL}>:LGNOME_WITH_NDL=1>
)

# libm: the volume overlay's meter math (log10f) lives in SDL builds, but linking m
# unconditionally is harmless and keeps the host build in sync.
target_link_libraries(lgnome-native PRIVATE
  lgnome-miniaudio
  lgnome-ndl-adapter
  category-log
  Threads::Threads
  ${CMAKE_DL_LIBS}
  m
)

if(LGNOME_TARGET_WEBOS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
  target_link_libraries(lgnome-native PRIVATE PkgConfig::GLIB2 luna-service2)
endif()

if(LGNOME_WITH_OPUS)
  # Builds libopus 1.4 from source under the active toolchain (recipe in native/cmake).
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  include(ExternalOPUS)
  target_compile_definitions(lgnome-native PRIVATE LGNOME_WITH_OPUS=1)
  target_include_directories(lgnome-native SYSTEM PRIVATE ${OPUS_INCLUDE_DIRS})
  target_link_libraries(lgnome-native PRIVATE ${OPUS_LIBRARIES})
  ExternalProject_Get_Property(ext_opus INSTALL_DIR)
  set_property(TARGET lgnome-native APPEND PROPERTY BUILD_RPATH "${INSTALL_DIR}/lib")
  # Ship our own copy: webOS carries a system libopus.so.0, but do not depend on it.
  install(DIRECTORY "${INSTALL_DIR}/lib/" DESTINATION ${CMAKE_INSTALL_LIBDIR}
    FILES_MATCHING PATTERN "libopus.so*")
  # BSD-3 requires the notice next to the shipped binary; COPYING comes from the
  # unpacked release tarball (exists by install time).
  ExternalProject_Get_Property(ext_opus SOURCE_DIR)
  install(FILES "${SOURCE_DIR}/COPYING" DESTINATION licenses RENAME opus-BSD-3.txt)
  # The packaged libopus lives next to the binary; keep it findable on the TV.
  set_property(TARGET lgnome-native APPEND PROPERTY INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

if(LGNOME_LINK_RDP_FFI)
  if(NOT DEFINED RDP_FFI_LIB)
    message(FATAL_ERROR "LGNOME_LINK_RDP_FFI=ON requires -DRDP_FFI_LIB=/path/to/libwebrdp_min.a")
  endif()
  if(NOT EXISTS "${RDP_FFI_LIB}")
    message(FATAL_ERROR "RDP_FFI_LIB does not exist: ${RDP_FFI_LIB}")
  endif()
  target_link_libraries(lgnome-native PRIVATE "${RDP_FFI_LIB}" Threads::Threads m dl)
else()
  target_sources(lgnome-native PRIVATE src/rdp_ffi_stub.c)
endif()

if(LGNOME_TARGET_WEBOS)
  find_package(SDL2 REQUIRED)
  if(TARGET SDL2::SDL2)
    target_link_libraries(lgnome-native PRIVATE SDL2::SDL2)
  else()
    target_include_directories(lgnome-native PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(lgnome-native PRIVATE ${SDL2_LIBRARIES})
  endif()

  pkg_check_modules(LIBEVDEV REQUIRED libevdev)
  find_library(LGNOME_LIBEVDEV_STATIC_LIBRARY NAMES libevdev.a PATHS ${LIBEVDEV_LIBRARY_DIRS} NO_DEFAULT_PATH)
  if(NOT LGNOME_LIBEVDEV_STATIC_LIBRARY)
    message(FATAL_ERROR "webOS SDL builds require static libevdev.a in the target sysroot")
  endif()
  target_include_directories(lgnome-native PRIVATE ${LIBEVDEV_INCLUDE_DIRS})
  target_link_libraries(lgnome-native PRIVATE "${LGNOME_LIBEVDEV_STATIC_LIBRARY}" rt)
  message(STATUS "Using libevdev for native mouse/keyboard readers")

  include(CheckIncludeFile)
  check_include_file("linux/input.h" LGNOME_HAVE_LINUX_INPUT_H)
  if(NOT LGNOME_HAVE_LINUX_INPUT_H)
    message(FATAL_ERROR "linux/input.h not found: libevdev input readers require Linux input event codes")
  endif()

  target_sources(lgnome-native PRIVATE src/input_evdev.c src/input_event_queue.c)
endif()

if(LGNOME_TARGET_WEBOS)
  if(NOT EXISTS "${LGNOME_LVGL_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "webOS product builds require initialized LVGL at ${LGNOME_LVGL_SOURCE_DIR}; run git submodule update --init third_party/lvgl")
  endif()

  set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/include/lv_conf.h" CACHE STRING "lgnome LVGL config" FORCE)
  set(LV_CONF_INCLUDE_SIMPLE ON CACHE BOOL "Use simple lv_conf.h include" FORCE)
  set(LV_LVGL_H_INCLUDE_SIMPLE ON CACHE BOOL "Use simple lvgl.h include" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build LVGL static for native package" FORCE)
  add_subdirectory("${LGNOME_LVGL_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/third_party/lvgl" EXCLUDE_FROM_ALL)
  target_sources(lgnome-native PRIVATE
    src/ui_preconnect.c
    src/ui_preconnect_build.c
    src/ui_preconnect_capture.c
    src/ui_preconnect_capture_form.c
    src/ui_preconnect_display.c
    src/ui_preconnect_forms.c
    src/ui_preconnect_hub.c
    src/ui_preconnect_onboarding.c
    src/ui_preconnect_profile.c
    src/ui_preconnect_screen.c
    src/ui_preconnect_theme.c
    src/ui_preconnect_brand.c
    src/ui_preconnect_widgets.c
    src/fonts/lv_font_ibm_plex_sans_semibold_86.c
    src/fonts/lv_font_ibm_plex_sans_semibold_86_profile.c
    src/fonts/lv_font_ibm_plex_sans_semibold_40.c
    src/fonts/lv_font_ibm_plex_sans_semibold_40_profile.c
    src/fonts/lv_font_ibm_plex_sans_semibold_24.c
    src/fonts/lv_font_ibm_plex_sans_semibold_24_profile.c
    src/fonts/lv_font_ibm_plex_sans_regular_24.c
    src/fonts/lv_font_ibm_plex_sans_regular_20.c
    src/fonts/lv_font_ibm_plex_mono_semibold_28.c
    src/fonts/lv_font_ibm_plex_mono_semibold_18.c
    src/fonts/lv_font_ibm_plex_mono_regular_22.c
    src/fonts/lv_font_ibm_plex_mono_regular_22_profile.c
    src/fonts/lv_font_ibm_plex_mono_regular_16.c
    src/fonts/lv_font_jetbrains_mono_semibold_28.c
  )
  target_include_directories(lgnome-native PRIVATE "${LGNOME_LVGL_SOURCE_DIR}/src")
  target_link_libraries(lgnome-native PRIVATE lvgl)
  if(TARGET SDL2::SDL2)
    target_link_libraries(lvgl PUBLIC SDL2::SDL2)
  else()
    target_include_directories(lvgl SYSTEM PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
  endif()
endif()

install(TARGETS lgnome-native RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES deploy/webos/appinfo.json DESTINATION .)
install(FILES deploy/webos/icon.png DESTINATION .)
install(FILES deploy/webos/splash.png DESTINATION .)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/PROVENANCE.md"
  DESTINATION licenses
  RENAME THIRD_PARTY_PROVENANCE.md
)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IBMPlex-OFL.txt"
  DESTINATION licenses
  RENAME IBMPlex-OFL-1.1.txt
)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/JetBrainsMono-OFL.txt"
  DESTINATION licenses
  RENAME JetBrainsMono-OFL-1.1.txt
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/LICENSE-MIT")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/LICENSE-MIT" DESTINATION licenses RENAME IronRDP-MIT.txt)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/LICENSE-APACHE")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/LICENSE-APACHE" DESTINATION licenses RENAME IronRDP-APACHE-2.0.txt)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/PROVENANCE.md")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/PROVENANCE.md" DESTINATION licenses RENAME IronRDP_PROVENANCE.md)
endif()
if(EXISTS "${LGNOME_LVGL_SOURCE_DIR}/LICENCE.txt")
  install(FILES "${LGNOME_LVGL_SOURCE_DIR}/LICENCE.txt" DESTINATION licenses RENAME lvgl-MIT.txt)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libevdev-COPYING")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libevdev-COPYING" DESTINATION licenses RENAME libevdev-COPYING.txt)
endif()
if(EXISTS "${LGNOME_MINIAUDIO_SOURCE_DIR}/LICENSE")
  install(FILES "${LGNOME_MINIAUDIO_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME miniaudio-MIT-0.txt)
endif()
set(LGNOME_NATIVE_STAGE_DIR "${CMAKE_BINARY_DIR}/webos-stage" CACHE PATH "Staging directory for native webOS packaging")
add_custom_target(webos-stage-native
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --prefix "${LGNOME_NATIVE_STAGE_DIR}"
  DEPENDS lgnome-native
  COMMENT "Stage native-only webOS package contents"
)
add_custom_target(webos-package-native
  COMMAND "${CMAKE_COMMAND}" -E env NATIVE_WEBOS_BUILD_DIR="${CMAKE_BINARY_DIR}" NATIVE_WEBOS_SKIP_BUILD=1 NATIVE_WEBOS_STAGING_DIR="${LGNOME_NATIVE_STAGE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build-native-webos.sh"
  DEPENDS webos-stage-native
  COMMENT "Package native webOS app with ares-package"
)
set(LGNOME_NATIVE_IPK "" CACHE FILEPATH "Explicit IPK to install with webos-install-native")
add_custom_target(webos-install-native
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../tools/deploy-native-webos.sh" --ipk "${LGNOME_NATIVE_IPK}" --no-launch
  VERBATIM
  COMMENT "Install native webOS package with ares-install"
)
add_custom_target(webos-launch-native
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../tools/deploy-native-webos.sh" --no-install
  DEPENDS webos-install-native
  COMMENT "Launch native webOS app with ares-launch"
)

if(BUILD_TESTING)
  add_executable(test-category-log
    tests/test_category_log.c
    tests/clog_helper_one.c
    tests/clog_helper_two.c
    src/category_log.c
  )
  target_include_directories(test-category-log PRIVATE include)
  target_compile_definitions(test-category-log PRIVATE
    CLOGX_TESTING=1
    $<$<BOOL:${CLOGX_HAVE_CLOCK_GETTIME}>:CLOGX_HAVE_CLOCK_GETTIME=1>
  )
  target_link_libraries(test-category-log PRIVATE Threads::Threads)
  add_test(NAME category-log COMMAND test-category-log)

  add_executable(test-category-log-ndebug
    tests/test_category_log_ndebug.c
  )
  target_compile_definitions(test-category-log-ndebug PRIVATE
    NDEBUG=1
    CLOGX_COMPILED_MIN_LEVEL=CLOGX_LEVEL_VALUE_INFO
  )
  target_include_directories(test-category-log-ndebug PRIVATE include)
  add_test(NAME category-log-ndebug COMMAND test-category-log-ndebug)

  add_executable(test-rdp-ffi-layout
    tests/test_rdp_ffi_layout.c
  )
  target_include_directories(test-rdp-ffi-layout PRIVATE include)
  add_test(NAME rdp-ffi-layout COMMAND test-rdp-ffi-layout)

  add_executable(test-rdp-log
    tests/test_rdp_log.c
    src/category_log.c
    src/rdp_log.c
  )
  target_compile_definitions(test-rdp-log PRIVATE CLOGX_TESTING=1
    $<$<BOOL:${CLOGX_HAVE_CLOCK_GETTIME}>:CLOGX_HAVE_CLOCK_GETTIME=1>)
  target_include_directories(test-rdp-log PRIVATE include)
  add_test(NAME rdp-log COMMAND test-rdp-log)

  add_executable(test-h264-annexb
    tests/test_h264_annexb.c
    src/h264_annexb.c
  )
  target_include_directories(test-h264-annexb PRIVATE include)
  add_test(NAME h264-annexb COMMAND test-h264-annexb)

  add_executable(test-video-rgba-sdl
    tests/test_video_rgba_sdl.c
    src/video_rgba_sdl.c
  )
  target_compile_definitions(test-video-rgba-sdl PRIVATE LGNOME_TARGET_WEBOS=1)
  target_include_directories(test-video-rgba-sdl PRIVATE tests/support/fake_sdl include)
  add_test(NAME video-rgba-sdl COMMAND test-video-rgba-sdl)

  add_executable(test-ui-profile-name
    tests/test_ui_profile_name.c
  )
  target_include_directories(test-ui-profile-name PRIVATE include)
  add_test(NAME ui-profile-name COMMAND test-ui-profile-name)

  add_executable(test-ui-host
    tests/test_ui_host.c
  )
  target_include_directories(test-ui-host PRIVATE include)
  add_test(NAME ui-host COMMAND test-ui-host)

  add_executable(test-ui-key-queue
    tests/test_ui_key_queue.c
    src/ui_key_queue.c
  )
  target_include_directories(test-ui-key-queue PRIVATE include)
  target_link_libraries(test-ui-key-queue PRIVATE category-log)
  add_test(NAME ui-key-queue COMMAND test-ui-key-queue)

  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME ui-font-coverage
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_ui_font_coverage.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/fonts/lv_font_ibm_plex_mono_regular_22_profile.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/fonts/lv_font_ibm_plex_sans_semibold_24_profile.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/fonts/lv_font_ibm_plex_sans_semibold_40_profile.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/fonts/lv_font_ibm_plex_sans_semibold_86_profile.c"
    )
  endif()

  add_executable(test-input-event-queue tests/test_input_event_queue.c src/input_event_queue.c)
  target_include_directories(test-input-event-queue PRIVATE include)
  target_link_libraries(test-input-event-queue PRIVATE category-log)
  add_test(NAME input-event-queue COMMAND test-input-event-queue)

  add_executable(test-input-sdl
    tests/test_input_sdl.c
    src/input_event_queue.c
    src/input_sdl.c
  )
  target_include_directories(test-input-sdl PRIVATE include)
  add_test(NAME input-sdl COMMAND test-input-sdl)

  add_executable(test-cursor-restore tests/test_cursor_restore.c src/cursor_sdl.c)
  target_include_directories(test-cursor-restore PRIVATE tests/support/fake_cursor_sdl include)
  target_compile_definitions(test-cursor-restore PRIVATE LGNOME_TARGET_WEBOS=1)
  target_link_libraries(test-cursor-restore PRIVATE category-log Threads::Threads)
  add_test(NAME cursor-restore COMMAND test-cursor-restore)

  add_executable(test-cursor-sdl
    tests/test_cursor_sdl.c
    src/cursor_sdl.c
  )
  target_include_directories(test-cursor-sdl PRIVATE include)
  target_link_libraries(test-cursor-sdl PRIVATE Threads::Threads)
  add_test(NAME cursor-sdl COMMAND test-cursor-sdl)

  add_executable(test-config-paths
    tests/test_config_paths.c
    src/config_paths.c
  )
  target_include_directories(test-config-paths PRIVATE include)
  add_test(NAME config-paths COMMAND test-config-paths)

  add_executable(test-settings-json
    tests/test_settings_json.c
    src/native_config_launch.c
    src/native_config.c
    src/native_config_storage.c
    src/native_settings.c
    src/settings_json.c
    src/native_json.c
    src/config_paths.c
  )
  target_include_directories(test-settings-json PRIVATE include)
  add_test(NAME settings-json COMMAND test-settings-json)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME deploy-webos COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test-deploy-native-webos.py")

  add_executable(test-native-rdp-video
    tests/test_native_rdp_video.c
    src/native_rdp_video.c
    src/native_rdp_state.c
    src/native_rgba_owner.c
    src/video_rgba_sdl.c
    src/h264_annexb.c
    src/au_snapshot.c
    src/native_settings.c
  )
  target_include_directories(test-native-rdp-video PRIVATE include src)
  target_link_libraries(test-native-rdp-video PRIVATE category-log Threads::Threads)
  add_test(NAME native-rdp-video COMMAND test-native-rdp-video)

  add_executable(test-native-time tests/test_native_time.c)
  target_include_directories(test-native-time PRIVATE include)
  add_test(NAME native-time COMMAND test-native-time)

  add_executable(test-au-snapshot
    tests/test_au_snapshot.c
    src/au_snapshot.c
    src/h264_annexb.c
  )
  target_include_directories(test-au-snapshot PRIVATE include)
  add_test(NAME au-snapshot COMMAND test-au-snapshot)

  add_executable(test-video-ndl
    tests/test_video_ndl.c
    src/ndl_adapter/video_ndl.c
    src/h264_annexb.c
  )
  target_compile_definitions(test-video-ndl PRIVATE LGNOME_WITH_NDL=1)
  target_include_directories(test-video-ndl PRIVATE include src/ndl_adapter)
  target_link_libraries(test-video-ndl PRIVATE backend_ndl::test_support category-log)
  add_test(NAME video-ndl COMMAND test-video-ndl)

  add_executable(test-audio-ndl
    tests/test_audio_ndl.c
    src/ndl_adapter/audio_ndl.c
  )
  target_compile_definitions(test-audio-ndl PRIVATE LGNOME_WITH_NDL=1)
  target_include_directories(test-audio-ndl PRIVATE include src/ndl_adapter)
  target_link_libraries(test-audio-ndl PRIVATE backend_ndl::test_support category-log)
  add_test(NAME audio-ndl COMMAND test-audio-ndl)

  add_executable(test-media-ndl-log-level
    tests/test_media_ndl_log_level.c
    src/ndl_adapter/media_ndl.c
  )
  target_compile_definitions(test-media-ndl-log-level PRIVATE
    LGNOME_WITH_NDL=1
    LGNOME_NDL_ADAPTER_TESTING=1
  )
  target_include_directories(test-media-ndl-log-level PRIVATE include src/ndl_adapter)
  target_link_libraries(test-media-ndl-log-level PRIVATE backend_ndl::test_support category-log)
  add_test(NAME media-ndl-log-level COMMAND test-media-ndl-log-level)

  add_executable(test-ui-mixer-geometry
    tests/test_ui_mixer_geometry.c
    src/ui_mixer_geometry.c
    src/category_log.c
  )
  target_include_directories(test-ui-mixer-geometry PRIVATE include)
  add_test(NAME ui-mixer-geometry COMMAND test-ui-mixer-geometry)

  add_executable(test-luna-volume
    tests/test_luna_volume.c
    src/luna_volume.c
    src/native_json.c
  )
  target_include_directories(test-luna-volume PRIVATE include)
  target_link_libraries(test-luna-volume PRIVATE Threads::Threads)
  add_test(NAME luna-volume COMMAND test-luna-volume)

  add_executable(test-webos-platform
    tests/test_webos_platform.c
    src/webos_platform.c
    src/native_json.c
  )
  target_include_directories(test-webos-platform PRIVATE include)
  add_test(NAME webos-platform COMMAND test-webos-platform)

  add_executable(test-camera-v4l2
    tests/test_camera_v4l2.c
    src/camera_v4l2.c
  )
  target_include_directories(test-camera-v4l2 PRIVATE include src)
  target_compile_definitions(test-camera-v4l2 PRIVATE LGNOME_CAMERA_V4L2_TESTING=1)
  add_test(NAME camera-v4l2 COMMAND test-camera-v4l2)

  add_executable(test-native-rdp-state
    tests/test_native_rdp_state.c
    src/native_rdp_state.c
  )
  target_include_directories(test-native-rdp-state PRIVATE include)
  target_link_libraries(test-native-rdp-state PRIVATE Threads::Threads)
  add_test(NAME native-rdp-state COMMAND test-native-rdp-state)

  add_executable(test-camera-h264-stream
    tests/test_camera_h264_stream.c
    src/camera_h264_stream.c
    src/h264_annexb.c
  )
  target_include_directories(test-camera-h264-stream PRIVATE include)
  add_test(NAME camera-h264-stream COMMAND test-camera-h264-stream)

  add_executable(test-audio-input-pcm
    tests/test_audio_input_pcm.c
    src/audio_input_pcm.c
  )
  target_include_directories(test-audio-input-pcm PRIVATE include)
  target_link_libraries(test-audio-input-pcm PRIVATE m)
  add_test(NAME audio-input-pcm COMMAND test-audio-input-pcm)

  add_executable(test-audio-input-alsa
    tests/test_audio_input_alsa.c
    src/audio_input_alsa.c
  )
  target_include_directories(test-audio-input-alsa PRIVATE include)
  target_compile_definitions(test-audio-input-alsa PRIVATE LGNOME_AUDIO_INPUT_ALSA_TESTING=1)
  target_link_libraries(test-audio-input-alsa PRIVATE ${CMAKE_DL_LIBS})
  add_test(NAME audio-input-alsa COMMAND test-audio-input-alsa)

  add_executable(test-capture-redirect
    tests/test_capture_redirect.c
    src/capture_audio_worker.c
    src/capture_camera_worker.c
    src/camera_h264_stream.c
    src/capture_redirect.c
    src/capture_slots.c
    src/native_capture_glue.c
    src/audio_input_pcm.c
    src/h264_annexb.c
  )
  target_include_directories(test-capture-redirect PRIVATE include)
  target_compile_definitions(test-capture-redirect PRIVATE LGNOME_CAPTURE_REDIRECT_TESTING=1)
  target_link_libraries(test-capture-redirect PRIVATE Threads::Threads m)
  add_test(NAME capture-redirect COMMAND test-capture-redirect)
  add_test(NAME capture-redirect-init-failure COMMAND test-capture-redirect --init-failure)

  add_executable(test-audio-pipeline
    tests/test_audio_pipeline.c
    src/audio_pipeline.c
    src/audio_pipeline_source.c
    src/audio_jitter.c
  )
  target_include_directories(test-audio-pipeline PRIVATE include)
  target_compile_definitions(test-audio-pipeline PRIVATE LGNOME_AUDIO_PIPELINE_TESTING=1)
  target_link_libraries(test-audio-pipeline PRIVATE lgnome-miniaudio Threads::Threads m)
  add_test(NAME audio-pipeline COMMAND test-audio-pipeline)

  add_executable(test-audio-jitter
    tests/test_audio_jitter.c
    src/audio_jitter.c
  )
  target_include_directories(test-audio-jitter PRIVATE include)
  add_test(NAME audio-jitter COMMAND test-audio-jitter)

  if(LGNOME_WITH_OPUS)
    add_executable(test-audio-opus
      tests/test_audio_opus.c
      src/audio_opus.c
    )
    target_compile_definitions(test-audio-opus PRIVATE LGNOME_WITH_OPUS=1)
    target_include_directories(test-audio-opus PRIVATE include)
    target_include_directories(test-audio-opus SYSTEM PRIVATE ${OPUS_INCLUDE_DIRS})
    target_link_libraries(test-audio-opus PRIVATE ${OPUS_LIBRARIES} m)
    set_property(TARGET test-audio-opus APPEND PROPERTY BUILD_RPATH "${INSTALL_DIR}/lib")
    add_test(NAME audio-opus COMMAND test-audio-opus)
  endif()

  foreach(native_test_target IN ITEMS
      test-rdp-ffi-layout test-h264-annexb test-video-rgba-sdl test-input-sdl
      test-cursor-sdl test-config-paths test-settings-json test-au-snapshot
      test-ui-mixer-geometry test-luna-volume test-webos-platform test-camera-v4l2
      test-native-rdp-state
      test-camera-h264-stream test-audio-input-pcm test-audio-input-alsa test-capture-redirect
      test-audio-pipeline test-audio-jitter
      test-audio-opus)
    if(TARGET ${native_test_target})
      target_link_libraries(${native_test_target} PRIVATE category-log)
    endif()
  endforeach()
endif()
