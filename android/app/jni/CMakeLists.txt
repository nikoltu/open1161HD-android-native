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
