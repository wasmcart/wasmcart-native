# wasmcart-native

Standalone wasmcart player. Runs `.wasc` game carts with V8 for instant WASM startup, SDL2 for display/input/audio, and EGL/GLES3 for GPU carts.

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
| GL (GLES3) | EGL + direct GL, FBO redirect, letterboxed | OpenArena, GZDoom, Neverball, ETR |
| Godot 4.x | GL + GLES3 Compatibility renderer | Warlords, RoboBlast, Kenney Platformer |

## Performance

V8's Liftoff baseline compiler starts WASM instantly — no compilation delay, even for 52MB Godot carts (356ms load time).

| Cart | FPS (uncapped) |
|------|---------------|
| Snake (320x240 2D) | 4,900 |
| OpenArena (1080p GL) | 830 |
| Warlords (Godot GL) | 430 |

## Build

### Prerequisites

- CMake 3.16+
- C/C++ compiler (gcc/clang)
- SDL2 dev headers (`sudo apt install libsdl2-dev`)
- EGL + GLES dev headers (`sudo apt install libegl-dev libgles-dev`)

### Build from source

```bash
# Download pre-built libnode
mkdir -p deps/libnode
curl -sL https://github.com/wasmcart/build-libnode/releases/download/v24.14.1/libnode-linux-x86_64.tar.gz \
  | tar xz -C deps/libnode

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run
./wasmcart-run /path/to/game.wasc
```

### Pre-built binaries

Download from [Releases](https://github.com/wasmcart/wasmcart-native/releases).

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

## License

MIT
