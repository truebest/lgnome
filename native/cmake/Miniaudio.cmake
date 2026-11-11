# Shared by CMake builds and the standalone syntax check.
get_filename_component(_miniaudio_cache "${CMAKE_CURRENT_LIST_DIR}/../../build/deps" ABSOLUTE)
set(LGNOME_MINIAUDIO_SOURCE_DIR "${_miniaudio_cache}/miniaudio-0.11.25")
file(MAKE_DIRECTORY "${_miniaudio_cache}")
file(LOCK "${_miniaudio_cache}/miniaudio.lock" GUARD PROCESS TIMEOUT 180)
file(DOWNLOAD
  https://github.com/mackron/miniaudio/archive/refs/tags/0.11.25.zip
  "${_miniaudio_cache}/miniaudio-0.11.25.zip"
  EXPECTED_HASH SHA256=580b1635c99621971c8053cd487fbc905d1c24d4395bb09214ff77026e5608e1
  TLS_VERIFY ON TIMEOUT 180 INACTIVITY_TIMEOUT 30
)
if(NOT EXISTS "${LGNOME_MINIAUDIO_SOURCE_DIR}/miniaudio.c" OR
   NOT EXISTS "${LGNOME_MINIAUDIO_SOURCE_DIR}/miniaudio.h" OR
   NOT EXISTS "${LGNOME_MINIAUDIO_SOURCE_DIR}/LICENSE")
  file(ARCHIVE_EXTRACT INPUT "${_miniaudio_cache}/miniaudio-0.11.25.zip"
    DESTINATION "${_miniaudio_cache}"
    PATTERNS "miniaudio-0.11.25/miniaudio.c" "miniaudio-0.11.25/miniaudio.h" "miniaudio-0.11.25/LICENSE")
endif()
file(LOCK "${_miniaudio_cache}/miniaudio.lock" RELEASE)
if(CMAKE_SCRIPT_MODE_FILE)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${LGNOME_MINIAUDIO_SOURCE_DIR}")
endif()
