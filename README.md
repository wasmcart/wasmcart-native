# wasmcart-native

Standalone [wasmcart](https://github.com/wasmcart/wasmcart) player. Runs `.wasc` game
carts with [V8](https://v8.dev) for instant WASM startup (via
[libnode](https://github.com/wasmcart/build-libnode)), [SDL2](https://www.libsdl.org)
for display/input/audio, and EGL/GLES3 for GPU carts.

One binary. No runtime dependencies beyond system GL. Drop it on any Linux, macOS, or Windows machine and play.

## Usage

```
wasmcart-run game.wasc [options]

Options:
  --res WxH       Window resolution (e.g. --res 1920x1080)
  --scale N       Integer scale factor for 2D carts (default: 1)
  --fullscreen    Start in fullscreen mode
  --fps           Show FPS counter
  --uncapped      Disable vsync and frame cap
```

## What It Runs

Every `.wasc` cart that runs in the browser or Node.js also runs here. Same WASM, same GL calls, same audio, same input.

| Cart type | Rendering | Examples |
|-----------|-----------|----------|
| 2D framebuffer | SDL2 accelerated renderer, letterboxed | Snake, Doom, ccleste, pygame carts |
| GL (GLES3) | EGL + direct GL, FBO redirect, letterboxed | OpenArena, [GZDoom](https://zdoom.org), [Neverball](https://neverball.org), ETR |
| [Godot](https://godotengine.org) 4.x | GL + GLES3 Compatibility renderer | Warlords, RoboBlast, Kenney Platformer |

## Performance

V8's Liftoff baseline compiler starts WASM instantly — no compilation delay, even for 52MB Godot carts (356ms load time).

| Cart | FPS (uncapped, 1080p) |
|------|---------------|
| Snake (320x240 2D) | 4,900 |
| [Three.js](https://threejs.org) (WebGL2) | 2,470 |
| OpenArena ([ioquake3](https://github.com/ioquake/ioq3) GL) | 830 |
| Adventure AI ([Skia](https://skia.org) Ganesh GPU) | 716 |
| Warlords (Godot GL) | 430 |

## Build

### Prerequisites

- CMake 3.16+
- C/C++ compiler (gcc/clang)
- [SDL2](https://www.libsdl.org) dev headers (`sudo apt install libsdl2-dev`)
- EGL + GLES dev headers (`sudo apt install libegl-dev libgles-dev`)

### Build from source

```bash
# Download pre-built libnode
mkdir -p deps/libnode
curl -sL https://github.com/wasmcart/build-libnode/releases/download/v26.3.0-jsg9/libnode-linux-x86_64.tar.gz \
  | tar xz -C deps/libnode

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run
./wasmcart-run /path/to/game.wasc
```

### Pre-built binaries

Download from [Releases](https://github.com/wasmcart/wasmcart-native/releases) —
Linux (x86_64/aarch64), macOS (x86_64/aarch64) and Windows x86_64.

On Linux and macOS, if the binary arrives without its execute bit:

```bash
chmod +x wasmcart-run
```

The release `.tar.gz` preserves the mode. What does not is the **ZIP that
GitHub Actions wraps around build artifacts** — downloading from the *Actions*
run page (rather than the Releases page) gives you a zip of the tarball, and
that layer drops the Unix mode. Copying between filesystems can do the same.

Worth knowing because the failure is misleading: the file is a perfectly valid
ELF, so `Permission denied` reads like a missing dependency or a broken build
rather than a file mode. `ls -l wasmcart-run` settles it in one command —
`-rw-rw-r--` means chmod, not a rebuild.

Building from source is unaffected; the linker sets the bit.

The ZIP reader is [miniz](https://github.com/richgel999/miniz) and manifest parsing
uses [cJSON](https://github.com/DaveGamble/cJSON), both vendored under `deps/`.

This same host core is shared with
[wasmcart-libretro](https://github.com/wasmcart/wasmcart-libretro) via git submodule,
so the GL bridge and asset loader stay in lockstep between the standalone player and
the RetroArch core.

## Architecture

```
wasmcart-run (75MB, statically linked)
│
├── V8 engine (libnode.a)    — WASM compile + execute (instant via Liftoff JIT)
├── SDL2 (libSDL2.a)         — Window, gamepad, keyboard, audio
├── EGL + GLES3              — GL context for GPU carts
│
├── cart_host.cpp             — wasmcart ABI: load .wasc, manage V8, run frames
├── gl_imports.cpp            — 160 GL functions registered as V8 callbacks
├── asset_loader.c            — .wasc ZIP reading (miniz) + manifest parsing (cJSON)
├── egl_context.c             — EGL pbuffer + window surface management
└── main.c                    — SDL2 event loop, input, display, audio queue
```

## Input

- **Gamepad**: SDL2 GameController API with 2,182 built-in controller mappings
- **Keyboard**: Arrow keys / WASD / Z / X mapped to gamepad for pad-only carts
- **4 players**: Up to 4 controllers supported simultaneously
- **Hot-plug**: Controllers can be connected/disconnected during play

## Resolution

The host passes preferred resolution to the cart via `--res`. The cart decides its actual rendering resolution. The host scales the output to fit the window, preserving aspect ratio with letterboxing. Without `--res`, the window matches the cart's native resolution.

## Rendering Modes

Determined by the cart's `gpu_api` field:

| gpu_api | Mode | Display path |
|---------|------|-------------|
| 0 | 2D framebuffer | SDL2 accelerated renderer + letterboxing |
| 1 | WebGL2 / GLES3 | EGL window surface + FBO redirect + letterboxing |

## Development Notes

### GL Import Bridge (gl_imports.cpp)

~190 GLES3 functions registered as V8 FunctionCallbacks. Key non-obvious behaviors:

**FBO Redirect**: All `glBindFramebuffer(GL_FRAMEBUFFER, 0)` calls are intercepted and redirected to a capture FBO. This lets the host blit the cart's output to the screen with letterboxing. `glClear` on the redirect FBO is suppressed after a cart blit to prevent wiping captured content.

**GL Version Filtering**: `glGetString(GL_VERSION)` returns `"OpenGL ES 3.0 wasmcart"` regardless of actual driver. Extensions are filtered to a WebGL2-compatible subset (7 extensions). This prevents Skia/Ganesh from probing for ES 3.1+ functions that aren't in the WASM import table. 28 ES 3.1+ functions are registered as no-op stubs for safety.

**`glGetInternalformativ`**: Required for Skia Ganesh GPU rendering. Ganesh queries max MSAA samples — without this function, `maxSamples=0` → render target creation fails → software fallback at ~60 FPS instead of 700+ FPS GPU.

**Signed Blit Coordinates**: `glBlitFramebuffer` source rect dimensions must be computed with signed math. Ganesh uses Y-inverted blits (srcY0 > srcY1). Storing the height as `uint32_t` causes wrap-around to ~4 billion → corrupted blit → black screen.

**Client-Side Vertex Arrays**: gl4es carts pass WASM memory offsets as "pointers" to `glVertexAttribPointer` with no VBO bound. The bridge tracks `GL_ARRAY_BUFFER` binding state and uploads client-side data to temp VBOs at draw time.

### Performance: Native vs Node.js Host

The Node.js host ([retroemu](https://github.com/monteslu/retroemu) +
[native-gles](https://github.com/monteslu/native-gles)) currently beats the native
host on some GL carts (~860 vs ~716 FPS for Skia Ganesh). Both use V8 for WASM and the same GPU for GL. The gap is in the GL call overhead:

- **Node.js host**: GL calls go through N-API (native-gles addon) — one function pointer call per GL function, minimal marshaling.
- **Native host**: GL calls go through V8 FunctionCallbacks — each call enters V8's callback machinery, extracts args from `v8::FunctionCallbackInfo`, converts types. This overhead multiplies across ~73 GL calls per frame (Ganesh) or ~193 GL calls in complex scenes.

**Potential fix**: Register GL imports as [fast API calls](https://v8.dev/docs/embed) (`v8::CFunction`) instead of regular FunctionCallbacks. V8's fast API path bypasses the full callback machinery for simple functions with known signatures — could eliminate most of the per-call overhead.

### Wayland vs X11

SDL2 may choose X11 (via XWayland) on Wayland sessions. The `egl_create_window_surface` code does a runtime check on `wm_info.subsystem` (not compile-time `#ifdef`). Both backends work, but compositor behavior may differ for vsync.

### Platform-Specific

- **macOS / Windows**: Require [ANGLE](https://github.com/google/angle) for GLES3 (no native GLES). Set `ANGLE_DIR` in cmake.
- **macOS**: Link `-framework Security -framework SystemConfiguration` (libnode TLS).
- **Windows**: MSVC needs `/std:c++20 /Zc:__cplusplus` (CXX only) + static CRT (`/MT`).
- **Windows**: `clock_gettime` → `QueryPerformanceCounter` (`#ifdef _WIN32`).

## License

MIT
