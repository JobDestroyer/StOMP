cmake_minimum_required(VERSION 3.21)
project(vibe C CXX)

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
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libswresample libavutil)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(GL REQUIRED gl)
pkg_check_modules(DBUS REQUIRED dbus-1)

add_executable(vibe-core
    src/main.c
    src/app.c
    src/platform_sdl.c
    src/audio.c
    src/decode.c
    src/ringbuf.c
    src/library.c
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
    ${SDL2_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
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

target_link_libraries(vibe-core PRIVATE
    libprojectM::playlist
    ${SDL2_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${GL_LIBRARIES}
    ${DBUS_LIBRARIES}
    "${CMAKE_SOURCE_DIR}/third_party/libsqlite3.a"
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
            --file "${CMAKE_SOURCE_DIR}/fonts/DejaVuSans.ttf:fonts/DejaVuSans.ttf"
            --file "${CMAKE_SOURCE_DIR}/fonts/DroidSansFallback.ttf:fonts/DroidSansFallback.ttf"
            --dir "${CMAKE_SOURCE_DIR}/presets:presets"
            --dir "${CMAKE_SOURCE_DIR}/textures:textures"
    COMMAND chmod +x "${CMAKE_BINARY_DIR}/stomp"
    DEPENDS vibe-core vibe-stub packembed projectM projectM_playlist
            ${VIBE_TEXTURES}
            "${CMAKE_SOURCE_DIR}/fonts/DejaVuSans.ttf"
            "${CMAKE_SOURCE_DIR}/fonts/DroidSansFallback.ttf"
            "${CMAKE_SOURCE_DIR}/presets/cream-of-the-crop/README.md"
    COMMENT "Pack inner player, libprojectM, presets, textures, and font into ./stomp"
    VERBATIM
)

add_custom_target(stomp-pack ALL DEPENDS "${CMAKE_BINARY_DIR}/stomp")
add_custom_target(vibe-pack DEPENDS stomp-pack)
