cmake_minimum_required(VERSION 3.16)
project(wasmcart-native C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# MSVC: force C++20 via compile features
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++20")
endif()

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
if(WIN32)
    target_link_libraries(wasmcart PUBLIC ${LIBNODE_DIR}/libnode.lib winmm dbghelp ws2_32 crypt32 iphlpapi psapi userenv bcrypt)
else()
    target_link_libraries(wasmcart PUBLIC ${LIBNODE_DIR}/libnode.a pthread dl m)
endif()

# GLES3 + EGL — use ANGLE on macOS/Windows, system libs on Linux
set(ANGLE_DIR "" CACHE PATH "Path to ANGLE (lib/ + include/)")
if(ANGLE_DIR AND EXISTS "${ANGLE_DIR}/include")
    message(STATUS "Using ANGLE from: ${ANGLE_DIR}")
    target_include_directories(wasmcart PUBLIC ${ANGLE_DIR}/include)
    if(APPLE)
        target_link_libraries(wasmcart PUBLIC
            ${ANGLE_DIR}/lib/libEGL.dylib
            ${ANGLE_DIR}/lib/libGLESv2.dylib)
    elseif(WIN32)
        target_link_libraries(wasmcart PUBLIC
            ${ANGLE_DIR}/lib/libEGL.lib
            ${ANGLE_DIR}/lib/libGLESv2.lib)
    endif()
else()
    find_library(GLESV2_LIB GLESv2)
    find_library(EGL_LIB_FOR_WASMCART EGL)
    if(GLESV2_LIB)
        target_link_libraries(wasmcart PUBLIC ${GLESV2_LIB})
    endif()
    if(EGL_LIB_FOR_WASMCART)
        target_link_libraries(wasmcart PUBLIC ${EGL_LIB_FOR_WASMCART})
    endif()
endif()

# ─── wasmcart-run (standalone player) ───────────────────────────────────

add_executable(wasmcart-run src/main.c src/egl_context.c)
target_link_libraries(wasmcart-run PRIVATE wasmcart)

# SDL2 — prefer static linking for single-binary distribution
if(WIN32)
    # Windows: use SDL2 from cmake package (set via -DSDL2_DIR)
    find_package(SDL2 CONFIG)
    if(SDL2_FOUND)
        target_include_directories(wasmcart-run PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(wasmcart-run PRIVATE SDL2::SDL2-static SDL2::SDL2main)
        set(SDL2_STATIC "win32-package")
    endif()
else()
    find_library(SDL2_STATIC libSDL2.a PATHS
        /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/local/lib
        /opt/homebrew/lib /usr/local/opt/sdl2/lib)
endif()
if(SDL2_STATIC)
    message(STATUS "Using static SDL2: ${SDL2_STATIC}")
    target_link_libraries(wasmcart-run PRIVATE ${SDL2_STATIC})
    # Platform-specific SDL2 system deps
    if(APPLE)
        target_link_libraries(wasmcart-run PRIVATE
            "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreHaptics"
            "-framework GameController" "-framework CoreVideo" "-framework Cocoa"
            "-framework IOKit" "-framework ForceFeedback" "-framework Carbon"
            "-framework CoreFoundation" "-framework Metal" "-framework QuartzCore"
            "-framework Security" "-framework SystemConfiguration"
            iconv)
    elseif(WIN32)
        target_link_libraries(wasmcart-run PRIVATE
            winmm imm32 setupapi version ole32 oleaut32 gdi32 user32 shell32 advapi32)
    else()
        target_link_libraries(wasmcart-run PRIVATE
            asound pulse samplerate
            X11 Xext Xcursor Xi Xfixes Xrandr Xss
            wayland-client wayland-egl wayland-cursor
            xkbcommon decor-0
            drm gbm)
    endif()
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
