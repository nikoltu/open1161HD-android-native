cmake_minimum_required(VERSION 3.22)
project(openbw_android C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# android/app/jni -> repository root
get_filename_component(OPENBW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SDL/CMakeLists.txt")
    message(FATAL_ERROR "SDL2 source not found at ${CMAKE_CURRENT_SOURCE_DIR}/SDL. Run android/scripts/fetch-sdl.sh first.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SDL_mixer/CMakeLists.txt")
    message(FATAL_ERROR "SDL2_mixer source not found at ${CMAKE_CURRENT_SOURCE_DIR}/SDL_mixer. Run android/scripts/fetch-sdl.sh first.")
endif()

add_subdirectory(SDL EXCLUDE_FROM_ALL)

# The sound backend only needs WAV chunks (mixing, volume, channels); every
# external codec is disabled so SDL2_mixer builds with no extra dependencies.
set(SDL2MIXER_VENDORED OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_CMD OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_FLAC OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_GME OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MOD OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MP3 OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MIDI OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_OPUS OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_VORBIS "" CACHE STRING "" FORCE)
set(SDL2MIXER_WAVE ON CACHE BOOL "" FORCE)
set(SDL2MIXER_WAVPACK OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL_mixer EXCLUDE_FROM_ALL)

add_library(main SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/main/cpp/android_main.cpp"
    "${OPENBW_ROOT}/ui/sdl2.cpp"
    # SMK video decoder (talking-head portraits, briefings, cinematics)
    "${OPENBW_ROOT}/ui/libsmacker/smacker.c"
    # open1161HD Faze 4.7 Variant B: hardware WebM cinematic decode (AMediaCodec)
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/main/cpp/cine_hw.cpp"
)

target_include_directories(main PRIVATE
    "${OPENBW_ROOT}"
    "${OPENBW_ROOT}/ui"
)

target_compile_definitions(main PRIVATE
    OPENBW_NO_SDL_IMAGE
    OPENBW_ENABLE_UI
)

target_compile_options(main PRIVATE -fexceptions -frtti -Wno-deprecated-declarations
    # bwgame.h is heavily templated; -O0 debug builds are ~10x too slow to play.
    # No -ffast-math: the simulation relies on deterministic fixed-point math.
    $<$<CONFIG:Debug>:-O1>
    $<$<NOT:$<CONFIG:Debug>>:-O3 -flto=thin -fvisibility=hidden -fvisibility-inlines-hidden>)
target_link_options(main PRIVATE $<$<NOT:$<CONFIG:Debug>>:-flto=thin>)

target_link_libraries(main SDL2::SDL2 SDL2_mixer::SDL2_mixer)
if(TARGET SDL2::SDL2main)
    target_link_libraries(main SDL2::SDL2main)
endif()
target_link_libraries(main log android mediandk GLESv2)  # SP shader pipeline (ui/sdl2.cpp post_gl)

# v0.53: tiny SDL-free library for the launcher's map-preview minimap generator.
# Pulls in only the header-only data-loading layer, so SetupActivity can load it
# without starting the game engine.
add_library(openbw_minimap SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/main/cpp/minimap_jni.cpp"
    # Launcher portraits (LAUNCHER-REDESIGN F1): SMK portrait clips decoded from the
    # user's MPQs for the animated profile portrait / gallery.
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/main/cpp/portrait_jni.cpp"
    "${OPENBW_ROOT}/ui/libsmacker/smacker.c"
)
target_include_directories(openbw_minimap PRIVATE "${OPENBW_ROOT}" "${OPENBW_ROOT}/ui")
target_compile_options(openbw_minimap PRIVATE -fexceptions -frtti
    $<$<CONFIG:Debug>:-O1>
    $<$<NOT:$<CONFIG:Debug>>:-O2 -fvisibility=hidden>)
target_link_libraries(openbw_minimap log)

# v1.0.27: first-run import from RAR / 7z archives (ArchiveImporter.java). RARLab
# UnRAR (decompression only, deps/unrar/license.txt) + the public-domain LZMA SDK
# 7z decoder (deps/lzma). Separate library so the launcher loads it on demand.
set(UNRAR_SRC rar strlist strfn pathfn smallfn global file filefn filcreat archive arcread
    unicode system crypt crc rawread encname resource match timefn rdwrfn consio options
    errhnd rarvm secpassword rijndael getbits sha1 sha256 blake2s hash extinfo extract
    volume list find unpack headers threadpool rs16 cmddata ui largepage filestr scantree
    dll qopen)
set(LZMA_SRC 7zAlloc 7zArcIn 7zBuf 7zCrc 7zCrcOpt 7zDec 7zFile 7zStream Bcj2 Bra Bra86
    BraIA64 CpuArch Delta Lzma2Dec LzmaDec Ppmd7 Ppmd7Dec)
set(ARCIMPORT_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../src/main/cpp/arc_import_jni.cpp")
foreach(f ${UNRAR_SRC})
    list(APPEND ARCIMPORT_FILES "${OPENBW_ROOT}/deps/unrar/${f}.cpp")
endforeach()
foreach(f ${LZMA_SRC})
    list(APPEND ARCIMPORT_FILES "${OPENBW_ROOT}/deps/lzma/C/${f}.c")
endforeach()
add_library(arcimport SHARED ${ARCIMPORT_FILES})
target_compile_definitions(arcimport PRIVATE RARDLL _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE Z7_PPMD_SUPPORT)
target_compile_options(arcimport PRIVATE -w -fexceptions -frtti
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/../src/main/cpp/arc_compat.h"
    $<$<CONFIG:Debug>:-O1>
    $<$<NOT:$<CONFIG:Debug>>:-O2 -fvisibility=hidden>)
target_link_libraries(arcimport log)
