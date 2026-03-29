cmake_minimum_required(VERSION 3.16)
project(wasmcart-native C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ─── Node.js / libnode ───────────────────────────────────────────────────

set(LIBNODE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/libnode" CACHE PATH "Path to libnode.a + headers")

if(NOT EXISTS "${LIBNODE_DIR}/libnode.a" AND NOT EXISTS "${LIBNODE_DIR}/libnode.lib")
    message(FATAL_ERROR "libnode not found at ${LIBNODE_DIR}\n"
        "Download from: https://github.com/wasmcart/build-libnode/releases\n"
        "  mkdir -p deps/libnode && curl -sL <url>/libnode-linux-x86_64.tar.gz | tar xz -C deps/libnode")
endif()

# ─── Source files ────────────────────────────────────────────────────────

set(LIBWASMCART_SOURCES
    src/cart_host.cpp
    src/gl_imports.cpp
    src/asset_loader.c
    deps/miniz.c
    deps/cJSON.c
)

# ─── libwasmcart (static core library) ──────────────────────────────────

add_library(wasmcart STATIC ${LIBWASMCART_SOURCES})
target_include_directories(wasmcart PUBLIC include)
target_include_directories(wasmcart PRIVATE
    src
    deps
    ${LIBNODE_DIR}/include
)
target_link_libraries(wasmcart PUBLIC ${LIBNODE_DIR}/libnode.a pthread dl m)

# GLES3
find_library(GLESV2_LIB GLESv2)
if(GLESV2_LIB)
    target_link_libraries(wasmcart PUBLIC ${GLESV2_LIB})
endif()

# ─── wasmcart-run (standalone player) ───────────────────────────────────

add_executable(wasmcart-run src/main.c src/egl_context.c)
target_link_libraries(wasmcart-run PRIVATE wasmcart)

# SDL2 — prefer static linking for single-binary distribution
find_library(SDL2_STATIC libSDL2.a SDL2 PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
if(SDL2_STATIC AND EXISTS "${SDL2_STATIC}")
    message(STATUS "Using static SDL2: ${SDL2_STATIC}")
    target_link_libraries(wasmcart-run PRIVATE ${SDL2_STATIC})
    # SDL2 static needs these system deps
    target_link_libraries(wasmcart-run PRIVATE
        asound pulse samplerate
        X11 Xext Xcursor Xi Xfixes Xrandr Xss
        wayland-client wayland-egl wayland-cursor
        xkbcommon decor-0
        drm gbm
    )
else()
    # Fallback to shared SDL2
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SDL2 sdl2)
        if(SDL2_FOUND)
            target_include_directories(wasmcart-run PRIVATE ${SDL2_INCLUDE_DIRS})
            target_link_libraries(wasmcart-run PRIVATE ${SDL2_LIBRARIES})
        endif()
    endif()
endif()

# SDL2 headers (needed regardless of static/shared)
find_path(SDL2_INCLUDE SDL2/SDL.h)
if(SDL2_INCLUDE)
    target_include_directories(wasmcart-run PRIVATE ${SDL2_INCLUDE})
endif()

# EGL
find_library(EGL_LIB EGL)
if(EGL_LIB)
    target_link_libraries(wasmcart-run PRIVATE ${EGL_LIB})
endif()

# ─── Status ─────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "wasmcart-native:")
message(STATUS "  libnode: ${LIBNODE_DIR}")
message(STATUS "  SDL2:    ${SDL2_STATIC}")
message(STATUS "  EGL:     ${EGL_LIB}")
message(STATUS "  GLESv2:  ${GLESV2_LIB}")
message(STATUS "")
