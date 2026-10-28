# SPDX-License-Identifier: LGPL-2.1-only
# Copyright (C) 2026 JobDestroyer
cmake_minimum_required(VERSION 3.21)
project(StOMP VERSION 0.3.0 LANGUAGES C CXX)

# Do not force a strict C standard globally: libprojectM's eval C sources
# need POSIX APIs (strdup, strcasecmp) which vanish under -std=c17.
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# libprojectM 4.x: shared library, C API only. Do not static-link (LGPL).
set(ENABLE_SYSTEM_GLM OFF CACHE BOOL "" FORCE)
set(ENABLE_SYSTEM_PROJECTM_EVAL OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL_UI OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_CXX_INTERFACE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(ENABLE_PLAYLIST ON CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/projectm EXCLUDE_FROM_ALL)

# Packed binary extracts .so files next to the inner stomp ($ORIGIN).
set_target_properties(projectM projectM_playlist PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH TRUE
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(GL REQUIRED gl)
pkg_check_modules(DBUS REQUIRED dbus-1)
find_package(ZLIB REQUIRED)

# Private LGPL FFmpeg 7.1 (shared) packed next to the inner player. Do not
# link the distro FFmpeg: SONAME moves every major (so.61 vs so.62/63) and
# Debian's build is GPL. HTTPS uses the system GnuTLS (libgnutls.so.30).
set(STOMP_FFMPEG_PREFIX "${CMAKE_BINARY_DIR}/ffmpeg-prefix")
set(STOMP_FFMPEG_PACK "${STOMP_FFMPEG_PREFIX}/pack")
set(STOMP_FFMPEG_LIBS
    libavformat.so.61
    libavcodec.so.61
    libavutil.so.59
    libswresample.so.5
)
set(STOMP_FFMPEG_PACK_FILES)
foreach(_so IN LISTS STOMP_FFMPEG_LIBS)
    list(APPEND STOMP_FFMPEG_PACK_FILES "${STOMP_FFMPEG_PACK}/${_so}")
endforeach()
add_custom_command(
    OUTPUT ${STOMP_FFMPEG_PACK_FILES}
           "${STOMP_FFMPEG_PREFIX}/include/libavformat/avformat.h"
    COMMAND "${CMAKE_SOURCE_DIR}/tools/build-bundled-ffmpeg.sh"
            "${STOMP_FFMPEG_PREFIX}"
            "${CMAKE_BINARY_DIR}/ffmpeg-work"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/build-bundled-ffmpeg.sh"
    COMMENT "Build bundled LGPL FFmpeg 7.1.5"
    VERBATIM
)
add_custom_target(stomp-ffmpeg DEPENDS
    ${STOMP_FFMPEG_PACK_FILES}
    "${STOMP_FFMPEG_PREFIX}/include/libavformat/avformat.h"
)
add_library(stomp_avformat SHARED IMPORTED)
add_library(stomp_avcodec SHARED IMPORTED)
add_library(stomp_avutil SHARED IMPORTED)
add_library(stomp_swresample SHARED IMPORTED)
set_target_properties(stomp_avformat PROPERTIES
    IMPORTED_LOCATION "${STOMP_FFMPEG_PACK}/libavformat.so.61"
    INTERFACE_INCLUDE_DIRECTORIES "${STOMP_FFMPEG_PREFIX}/include"
)
set_target_properties(stomp_avcodec PROPERTIES
    IMPORTED_LOCATION "${STOMP_FFMPEG_PACK}/libavcodec.so.61"
    INTERFACE_INCLUDE_DIRECTORIES "${STOMP_FFMPEG_PREFIX}/include"
)
set_target_properties(stomp_avutil PROPERTIES
    IMPORTED_LOCATION "${STOMP_FFMPEG_PACK}/libavutil.so.59"
    INTERFACE_INCLUDE_DIRECTORIES "${STOMP_FFMPEG_PREFIX}/include"
)
set_target_properties(stomp_swresample PROPERTIES
    IMPORTED_LOCATION "${STOMP_FFMPEG_PACK}/libswresample.so.5"
    INTERFACE_INCLUDE_DIRECTORIES "${STOMP_FFMPEG_PREFIX}/include"
)
add_dependencies(stomp_avformat stomp-ffmpeg)
add_dependencies(stomp_avcodec stomp-ffmpeg)
add_dependencies(stomp_avutil stomp-ffmpeg)
add_dependencies(stomp_swresample stomp-ffmpeg)

# SQLite amalgamation (public domain). Built here so the tree is complete source.
add_library(stomp-sqlite STATIC third_party/sqlite3.c)
set_target_properties(stomp-sqlite PROPERTIES
    C_STANDARD 99
    C_EXTENSIONS ON
)
target_include_directories(stomp-sqlite PUBLIC "${CMAKE_SOURCE_DIR}/third_party")
target_compile_definitions(stomp-sqlite PRIVATE
    SQLITE_THREADSAFE=2
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_LOAD_EXTENSION
)
target_compile_options(stomp-sqlite PRIVATE -w)

add_executable(vibe-core
    src/main.c
    src/app.c
    src/platform_sdl.c
    src/audio.c
    src/decode.c
    src/ringbuf.c
    src/library.c
    src/karaoke.c
    src/zipread.c
    src/cdg.c
    src/playlist.c
    src/input.c
    src/ui.c
    src/viz.c
    src/config.c
    src/mpris.c
    src/radio.c
)

target_include_directories(vibe-core PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party"
    "${STOMP_FFMPEG_PREFIX}/include"
    ${SDL2_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS}
)

target_compile_options(vibe-core PRIVATE
    -Wall -Wextra -Wshadow
    -Wno-format-truncation
    ${SDL2_CFLAGS_OTHER}
    ${DBUS_CFLAGS_OTHER}
)

target_compile_definitions(vibe-core PRIVATE
    VIBE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_dependencies(vibe-core stomp-ffmpeg)
target_link_libraries(vibe-core PRIVATE
    libprojectM::playlist
    ${SDL2_LIBRARIES}
    stomp_avformat
    stomp_avcodec
    stomp_swresample
    stomp_avutil
    ${FREETYPE_LIBRARIES}
    ${GL_LIBRARIES}
    ${DBUS_LIBRARIES}
    ZLIB::ZLIB
    stomp-sqlite
    m
    pthread
    dl
)

set_target_properties(vibe-core PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
    OUTPUT_NAME vibe-core
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH TRUE
)

# Place bundled FFmpeg next to vibe-core so $ORIGIN resolves in the build tree.
add_custom_command(TARGET vibe-core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${STOMP_FFMPEG_PACK_FILES}
            "$<TARGET_FILE_DIR:vibe-core>"
    COMMENT "Copy bundled FFmpeg into the build directory"
)

add_executable(vibe-stub src/stub.c)
set_target_properties(vibe-stub PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
    OUTPUT_NAME vibe-stub
)

add_executable(packembed tools/packembed.c)
set_target_properties(packembed PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)

file(GLOB VIBE_TEXTURES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/textures/*.png")

# Do not list ~9800 .milk files as ninja DEPENDS: the concatenated path is too long.
# --dir walks the trees at pack time. Touch cream-of-the-crop/README.md (or rebuild
# vibe-core) after adding presets if the packed binary must pick them up.
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/stomp"
    COMMAND "$<TARGET_FILE:packembed>"
            -o "${CMAKE_BINARY_DIR}/stomp"
            "$<TARGET_FILE:vibe-stub>"
            --file "$<TARGET_FILE:vibe-core>:stomp"
            --file "$<TARGET_FILE:projectM>:libprojectM-4.so.4"
            --file "$<TARGET_FILE:projectM_playlist>:libprojectM-4-playlist.so.4"
            --file "${STOMP_FFMPEG_PACK}/libavformat.so.61:libavformat.so.61"
            --file "${STOMP_FFMPEG_PACK}/libavcodec.so.61:libavcodec.so.61"
            --file "${STOMP_FFMPEG_PACK}/libavutil.so.59:libavutil.so.59"
            --file "${STOMP_FFMPEG_PACK}/libswresample.so.5:libswresample.so.5"
            --file "${CMAKE_SOURCE_DIR}/fonts/DejaVuSans.ttf:fonts/DejaVuSans.ttf"
            --file "${CMAKE_SOURCE_DIR}/fonts/DroidSansFallback.ttf:fonts/DroidSansFallback.ttf"
            --dir "${CMAKE_SOURCE_DIR}/presets:presets"
            --dir "${CMAKE_SOURCE_DIR}/textures:textures"
    COMMAND chmod +x "${CMAKE_BINARY_DIR}/stomp"
    DEPENDS vibe-core vibe-stub packembed projectM projectM_playlist stomp-ffmpeg
            ${VIBE_TEXTURES}
            "${CMAKE_SOURCE_DIR}/fonts/DejaVuSans.ttf"
            "${CMAKE_SOURCE_DIR}/fonts/DroidSansFallback.ttf"
            "${CMAKE_SOURCE_DIR}/presets/cream-of-the-crop/README.md"
    COMMENT "Pack inner player, libprojectM, FFmpeg, presets, textures, and font into ./stomp"
    VERBATIM
)

add_custom_target(stomp-pack ALL DEPENDS "${CMAKE_BINARY_DIR}/stomp")
add_custom_target(vibe-pack DEPENDS stomp-pack)
