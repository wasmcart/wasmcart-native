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
  --uncapped      Disable vsync and frame cap (with vsync the display paces
                  the frames, at its own rate: 60, 120, 144 Hz; with no vsync
                  to wait on, a 60 Hz timer does)
  --max-memory GB Stop the player (exit 3) once its resident memory passes GB
                  (default 10; 0 = no limit)
  --msaa N        Multisampled window surface (N samples) for GL carts, a browser's antialias: true
  --no-direct     Always present GL carts through the redirect FBO (by default a cart
                  draws straight onto the window when it is exactly the cart's size)
  --fixed-step MS The host clock advances exactly MS milliseconds per frame
                  (time_ms = frame * MS) instead of wall time (tests). The run
                  neither loads nor writes the cart's .sav, so earlier progress
                  can't change what a test plays
  --shot N FILE   Save frame N of a GL or WebGPU cart as a PPM (tests); with
                  --fixed-step the capture is reproducible

Environment:
  WASMCART_NO_WGPU=1     Act as a host without WebGPU (refuse WebGPU-only carts,
                         run dual carts on GL)
  WASMCART_WGPU_DIR=DIR  Where the WebGPU support files are (default: wgpu/
                         beside the executable)
  WASMCART_NO_PREFETCH=1 Act as a host built before asset prefetch: the
                         wc_asset_prefetch imports are stubbed and the flag
                         is not written (the tests' control)
  WASMCART_WGPU_POWER=low-power|high-performance
                         Which GPU WebGPU carts get on a two-GPU machine
                         (low-power = integrated); the choice is logged
  WASMCART_WGPU_VALIDATION=1|0
                         Keep or skip Dawn's command validation. Default:
                         skipped (it costs CPU on every call), kept in a
                         --fixed-step (test) run
  XDG_CACHE_HOME         The compiled-code cache lives in
                         $XDG_CACHE_HOME/wasmcart/code (~/.cache/wasmcart/code;
                         ~/Library/Caches/wasmcart/code on macOS,
                         %LOCALAPPDATA%\wasmcart\cache\code on Windows)
  SDL_VIDEODRIVER=offscreen (or dummy)
                         Headless: no window, and GL runs on an EGL surfaceless
                         context, so tests never touch the X or Wayland server.
                         Pin the GPU for tests too (WASMCART_WGPU_POWER for
                         WebGPU, MESA_VK_DEVICE_SELECT / DRI_PRIME for drivers)
```

## What It Runs

Every `.wasc` cart that runs in the browser or Node.js also runs here. Same WASM, same GL calls, same audio, same input.

| Cart type | Rendering | Examples |
|-----------|-----------|----------|
| 2D framebuffer | SDL2 accelerated renderer, letterboxed | Snake, Doom, ccleste, pygame carts |
| GL (GLES3) | EGL + direct GL, FBO redirect, letterboxed | OpenArena, [GZDoom](https://zdoom.org), [Neverball](https://neverball.org), ETR |
| [Godot](https://godotengine.org) 4.x | GL + GLES3 Compatibility renderer | Warlords, RoboBlast, Kenney Platformer |
| WebGPU (`gpu_api` 2) | Dawn (Vulkan on Linux, Metal on macOS, D3D12 on Windows) through native-dawn, letterboxed into a WebGPU window surface. Needs a build with WebGPU (below) | [Defold](https://defold.com) WebGPU carts, three.js WebGPURenderer carts, wasi-sdk carts built with wasmcart's `wgpu-wasi/` (threads included) |

## Performance

V8's Liftoff baseline compiler starts WASM instantly — no compilation delay, even for 52MB Godot carts (356ms load time).

A cart's compiled code is kept between runs (V8's serialized module, one file
per cart named by the SHA-256 of its wasm, in the cache directory above,
capped at 1 GB, oldest first): V8 writes it each time it has optimized another
256 KB of the cart, and the next start runs that optimized code from its first
frame instead of the baseline code it would otherwise run for the first
seconds. A file another V8 cannot use is ignored and the cart compiles as
usual.

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
- Linux builds with WebGPU: the Vulkan headers (`sudo apt install libvulkan-dev`), for
  sharing WebGPU frames with GL in an embedder's context (the libretro core); Vulkan
  itself is loaded at run time, never linked

### Build from source

```bash
# Download pre-built libnode
mkdir -p deps/libnode
curl -sL https://github.com/wasmcart/build-libnode/releases/download/v26.3.0-jsg10/libnode-linux-x86_64.tar.gz \
  | tar xz -C deps/libnode

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run
./wasmcart-run /path/to/game.wasc
```

### WebGPU support

WebGPU carts (wasmcart SPEC.md, "WebGPU") run on the reference host's own
JavaScript, inside this player's embedded Node, with Dawn from
[native-dawn](https://github.com/monteslu/native-dawn). Point CMake at both:

```bash
cmake .. -DWASMCART_WGPU_JS_DIR=<wasmcart 0.34.4 or later>/src/wgpu \
         -DNATIVE_DAWN_DIR=<native-dawn 0.1.6 or later>/dist/linux-x64
```

`scripts/fetch-wgpu-deps.sh <target> <dir>` fetches both the way the release
builds do: native-dawn's release archive for `linux-x64`, `linux-arm64`,
`darwin-x64`, `darwin-arm64` or `win32-x64` (sha256 pinned in
the script) into
`<dir>/native-dawn`, and the wasmcart npm package into `<dir>/wasmcart`.

Every build then refreshes a `wgpu/` directory beside `wasmcart-run`: the
bridge (`src/wgpu_bridge.cjs`), wasmcart's `host.js` and generated glue (in
`src/wgpu/`, beside the `src/cartMemory.js` they import), and `dawn.node` +
Dawn's library (`libwebgpu_dawn.so`, `libwebgpu_dawn.dylib` on macOS;
on Windows `webgpu_dawn.dll`, `dxcompiler.dll`, `dxil.dll` and the Visual C++
runtime DLLs they need). Ship that directory with the binary.
Without it the player has no WebGPU: a WebGPU-only cart is refused at load
with that reason, and a cart that also imports GL runs on GL (which is how
the libretro and Android builds behave today).

The calls a frame makes most (a render or compute pass's state, draws and
dispatches, and the queue's buffer writes) call Dawn's C API from C++
(`src/wgpu_imports.cpp`), as the GL imports do, on the objects the glue
made (native-dawn's `objectHandle`); the rest go through the glue. They are
V8 fast API functions, as the GL imports are (below). When every render-pass
call the cart imports is one of these, `wgpuCommandEncoderBeginRenderPass` is
served from C++ too: the descriptor is read from cart memory into webgpu.h's
structs (the glue built it as JavaScript objects for dawn.node to convert back,
about 8 us a pass), and the pass gets its handle from the cart's own
`emwgpuCreateRenderPassEncoder`. Dawn's
validation is off unless asked for (above). When the cart's canvas is the
window's size, it draws straight into the window's texture; any other size is
letterboxed with a copy.

If the GPU device is lost while a WebGPU cart runs (a driver reset, the GPU
removed), the cart cannot continue: the player stops running it, prints the
reason, shows a message box when it has a window, and exits with status 4
(wasmcart docs/webgpu.md, "Errors and device loss").

The executable exports Node-API and libuv symbols (`-Wl,--dynamic-list`) so
`dawn.node` can load at all; libnode is linked statically and its symbols are
otherwise hidden. On macOS the executable exports them already (strip it with
`strip -x`, which keeps them) and the cart's frame is presented through a
CAMetalLayer.

On Windows, `dawn.node` (native-dawn 0.1.2 or later) delay-loads Node-API
from `node.exe` and its delay-load hook resolves that to the program loading
it. libnode's N-API functions are declared `__declspec(dllexport)`, so
`wasmcart-run.exe` exports them; `node test/wgpu_exports.mjs
build/wasmcart-run.exe` checks that it exports every one `dawn.node` needs.

`test/wgpu_test.sh` runs the WebGPU fixtures headless (render, compute
readback, a resized surface, a lost device, dual carts, refusals, repeated
clean exits).

### Pre-built binaries

Download from [Releases](https://github.com/wasmcart/wasmcart-native/releases) —
Linux (x86_64/aarch64), macOS (x86_64/aarch64) and Windows (x86_64).
Every archive includes WebGPU: keep the `wgpu/` directory beside
`wasmcart-run`.

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

This same host core is shared via git submodule with
[wasmcart-libretro](https://github.com/wasmcart/wasmcart-libretro) and with the
standalone Android player, so the GL bridge, asset loader, and ABI handling
stay in lockstep across all three embeddings.

## Architecture

```
wasmcart-run (75MB, statically linked)
│
├── V8 engine (libnode.a)    — WASM compile + execute (instant via Liftoff JIT)
├── SDL2 (libSDL2.a)         — Window, gamepad, keyboard, audio
├── EGL + GLES3              — GL context for GPU carts
│
├── cart_host.cpp             — wasmcart ABI: load .wasc, manage V8, run frames
├── gl_imports.cpp            — ~209 GL functions registered as V8 callbacks
├── asset_loader.c            — .wasc ZIP reading (miniz) + manifest parsing (cJSON)
├── egl_context.c             — EGL pbuffer + window surface management
└── main.c                    — SDL2 event loop, input, display, audio queue
```

## Input

- **Gamepad**: SDL2 GameController API with 2,182 built-in controller mappings
- **Keyboard**: Arrow keys / WASD / Z / X mapped to gamepad for pad-only carts
- **4 players**: Up to 4 controllers supported simultaneously, each in its own
  pad slot (pad N is player N + 1)
- **Hot-plug**: Controllers can be connected/disconnected during play; one that
  goes away leaves the others in their slots, and a new one takes the first
  free slot. `test/pads_test.sh` checks it with virtual controllers (uinput)
- **Rumble**: `wc_pad_rumble` routed to SDL2's haptics, with the ABI's clamping
  applied once in the host library so every embedder behaves the same
- **Text input**: `wc_on_text` fed from `SDL_TEXTINPUT`, so a cart receives
  characters the OS already composed -- layout, shift, dead keys and IME -- and
  never has to reimplement a keyboard layout. `SDL_StartTextInput` is mirrored
  from the cart's own state, which is also the on-screen keyboard signal on
  platforms that have one

While a cart is taking text, the keyboard STOPS acting as a virtual gamepad --
otherwise typing a name also plays the game ("w" walks the player, Q/E fire the
shoulders, Return presses Start). Real gamepads keep working throughout, so a
cart can drive a d-pad character picker while a field is open.

The keyboard-as-gamepad fallback is not conditional on whether a controller is
plugged in. Making it so would mean unplugging a pad mid-game silently changed
what the keys did.

## RNG Seeding

A normal load seeds the cart's `wc_set_seed` export (if it has one) with fresh
entropy, so every boot deals a different shuffle. Embedders pin it for
deterministic replay via `wc_host_options_t`:

```c
wc_host_options_t opts = { .rng_seed = 1234, .rng_seed_set = true };
```

This mirrors the JS hosts' `deterministic:{seed}` exactly: unpinned differs
every run, pinned reproduces bit-for-bit. `test/seed_test.c` asserts both
directions against the `detrng` fixture.

## Threads

Threaded carts (`wasm32-wasip1-threads`: `wasi.thread-spawn` plus a shared,
imported `WebAssembly.Memory`) run as the spec requires. The host builds the
imported memory from the limits the module declares, and each spawned thread is a
node `worker_thread` (its own V8 isolate on its own native thread) that
instantiates the same compiled module against the same shared memory and calls
`wasi_thread_start(tid, start_arg)`. Threads may spawn threads. This is the same
model as the JS host (`CartHost.js` + `cartWorker.js`).

A thread has the main thread's import table: WASI (`fd_write` to stdout/stderr,
clocks, `poll_oneoff` sleeps, `sched_yield`, ...), `wc_log` and asset loading
work everywhere; GL, WebGPU and the other `wc_*` imports are main-thread only and throw if
a thread calls them (`wc_asset_prefetch` answers `-3`, refused, and its cancel
is a no-op: a thread reads the archive as before).

## Asset prefetch

wasmcart SPEC.md, "Asset prefetch" (additive to ABI v4): the host writes
`WC_HOST_FLAG_ASSET_PREFETCH` (`0x04`) into `wc_host_info_t.flags`, and a cart
that sees it may call `wc_asset_prefetch(path, len, priority)` for an asset it
will read soon. The archive entry is extracted on libuv's threadpool (one
`uv_queue_work` job with a ZIP reader of its own) into a buffer the host keeps;
the same call polls the state, which becomes `READY` (1) between frames, and
`wc_asset_size` / `wc_load_asset` of that path then answer from the copy with
no archive read and no inflate. A whole read drops the copy;
`wc_asset_prefetch_cancel` drops it earlier. Up to 256 paths pending or ready,
4 extracting at once, 512 MiB held; beyond that a call answers `-3` (refused,
ask again later). A path the archive lacks answers `-1`, a failed extract `-2`.
A cart that never calls it is untouched, and a cart built against the imports
still loads on an older build of this player (the stub answers 0, the flag is
clear, and the cart reads synchronously). A cart that imports `thread-spawn` without exporting
`wasi_thread_start` (or the reverse) is refused. Threads are terminated when the
host is destroyed. A thread that dies (it traps, or its worker fails) stops the
cart at its next frame with an error naming the thread: the cart's pool would
otherwise run a worker short, and could wait on it forever.

## Resolution

The host passes preferred resolution to the cart via `--res`. The cart decides its actual rendering resolution. The host scales the output to fit the window, preserving aspect ratio with letterboxing. Without `--res`, the window matches the cart's native resolution.

## Networking

Carts reach the network through the `wc_peer_*` family, gated two ways: the cart
sets `WC_FLAG_NET_PEER` to ask, and the packager grants specific hosts in the
manifest. Neither alone is enough, and with no grant the host fails closed.

Two ways a peer reaches a cart, with deliberately different security:

| | Who dials | Grant needed |
|---|---|---|
| `wc_peer_open()` | the **cart** | `WC_FLAG_NET_PEER` **and** a manifest `net.domains` entry for that host |
| `wc_host_add_peer()` | the **host** | none -- the host already chose it |

The asymmetry is the point. Dialling out is allowlisted because the cart names a
destination the packager may not have anticipated. A peer the host established
needs no cart-side grant; requiring an embedder to write a manifest key
permitting its own action would be ceremony.

Dial-out is handled entirely inside the host using node's own WebSocket -- the
same implementation the Node host uses, so a cart that talks to a server in the
browser talks to it here without change, and an embedder does nothing. For a
host-supplied peer the embedder provides a send callback, feeds inbound bytes
with `wc_host_peer_recv()`, and reports a drop with `wc_host_remove_peer()`.

Payloads (like `wc_on_text` strings) are copied into a block the cart
allocates and freed after the call; see "Cart memory the host writes" below.

Async work (connections, messages, timers) advances once per frame from
`wc_host_run_frame()`. An embedder driving async work outside a frame loop --
waiting for a connection before starting the cart, say -- can call
`wc_host_pump()` directly.

> Embedding note: node's event loop needs three things pumped together, and
> missing any one breaks a different part of node in ways that look unrelated.
> `process.nextTick` is drained by `node::CallbackScope`, NOT by `uv_run`, and
> `net.Socket.connect()` defers its dial through nextTick -- so without it
> sockets sit in `connecting` forever with no error and no syscall ever issued,
> while timers, `setImmediate`, `fs` and even `dns.lookup` all keep working.
> `test/net_test.c` pins this.

## Rendering Modes

Determined by the cart's `gpu_api` field:

| gpu_api | Mode | Display path |
|---------|------|-------------|
| 0 | 2D framebuffer | SDL2 accelerated renderer + letterboxing |
| 1 | WebGL2 / GLES3 | EGL window surface + FBO redirect + letterboxing |
| 2 | WebGPU | WebGPU surface on the window (Wayland, X11, a CAMetalLayer on macOS, a Win32 window on Windows), the cart's frame drawn letterboxed. Selected by the cart's WebGPU imports; a cart importing both GPU APIs gets WebGPU when this build has it, else GL |

Any other `gpu_api`, and 2 with no WebGPU imports, is refused at load.

## Development Notes

### Tests

Standalone C programs against the built library, not a framework. Each takes a
cart and asserts on what the cart observed, so a passing run means the ABI
worked end to end rather than that a mock was called.

```bash
# build the library first, then:
gcc -O0 -o net_test  test/net_test.c  -Iinclude -Isrc \
    build/libwasmcart.a deps/libnode/libnode.a -lstdc++ -lm -lpthread -ldl

node ../wasmcart/test/wsserver.mjs --port 8796 &   # from the wasmcart repo
./net_test 8796         # require, nextTick, net.connect, WebSocket round-trip
./rumble_test ../wasmcart/test/fixtures/rumble.wasc
./text_test  test/textauto.wasc 5452 5456 5460 5472
sh test/input_guard_test.sh   # keyboard is not also a gamepad while typing
sh test/actions_test.sh build/wasmcart-run   # input actions: declarations, keyboard origins, prompt
./actions_api_test test/actioncart.wasc   # wc_host_actions lists, wc_host_action_bind remaps
./peer_test 8796 <granted.wasc> <ungranted.wasc>   # wc_peer_* end to end
./seed_test ../wasmcart/test/fixtures/detrng.wasc  # entropy differs, pinned reproduces
./host_threads_test ../wasmcart/test/fixtures/hostthreads.wasc ../wasmcart/test/fixtures/hostthreads_legacy.wasc
                              # hardware_threads written only for a cart with WC_FLAG_HOST_THREADS
sh test/wgpu_test.sh          # WebGPU carts (needs a build with WebGPU support)
sh test/prefetch_test.sh      # asset prefetch: READY across frames, reads served from
                              # the copy, cancel; the control is an older host (no flag)
sh test/alloc_test.sh         # GL strings/mappings in the cart's wc_alloc blocks
./alloc_payload_test test/alloc   # text and peer payloads through wc_alloc
WASMCART_WGPU_DIR=$PWD/build/wgpu ./wgpu_alloc_test test/wgpu/wgpucart.wasc
                              # WebGPU adapter info + mapped ranges through wc_alloc
                              # (link it with -Wl,--dynamic-list=build/node-api-exports.list)
sh test/code_cache_test.sh    # compiled code kept between runs, a garbled file ignored
sh test/pacing_test.sh        # 60 Hz without vsync
sh test/thread_death_test.sh  # a cart thread that dies stops the cart
xvfb-run -a sh test/wgpu_window_test.sh   # a WebGPU cart drawn straight into a window its size
```

`text_test` takes the cart's debug-field offsets as arguments because they move
whenever the fixture is recompiled -- linking an allocator alone shifted them 16
bytes. Read them with wasmcart's `readDebugState()`.

Two traps worth knowing before writing another one:

- `wc_host_enter_v8()` must be called **after** `wc_host_create()`; create() is
  what initialises V8. Calling it first segfaults before `main()` prints
  anything, which reads like a host bug rather than a harness bug.
- A static library does not relink automatically. Rebuilding `libwasmcart.a`
  and re-running a stale test binary produces confident wrong answers.

### Cart memory the host writes

Some host calls must hand the cart bytes at an address in its memory:
`glGetString`/`glGetStringi` return a string pointer, `glMapBufferRange` a
mapping, and `wc_on_text`/`wc_peer_on_message` take `(ptr, len)`. The host
writes these only into blocks the cart allocates with its optional
`wc_alloc(size, align)` export and releases with `wc_free(ptr)` (wasmcart 0.32;
see wasmcart's SPEC.md, "Cart memory the host writes"; `wasmcart.h` defines and
exports them). Every pointer the cart returns is checked against its memory and
the alignment asked for before anything is written. A cart that never reaches
such a path needs no allocator. One that does and lacks `wc_alloc`/`wc_free` is
stopped at that call with an error naming it -- including a cart built before
0.32 that exports only `malloc`/`free`: the host never calls a cart's `malloc`
(rebuild it against 0.32's `wasmcart.h`). A WebGPU callback that hits this
inside Node's event loop stops the cart on the next frame. GL strings
are allocated once each and never freed; a mapping is allocated at map and
freed at unmap; a payload is freed when the cart's handler returns.
(`wc_cart_alloc` is in `cart_host.cpp`; `test/alloc_test.sh` and
`test/alloc_payload_test.c` pin it.)

### GL Import Bridge (gl_imports.cpp)

~209 GLES3 functions (the count is logged at startup: `GL imports registered
(N functions, ...)`). Each is a V8 function with two entry points sharing one
body: a fast API function in the import's C types (`v8::CFunction`), which a
cart's wasm calls directly when the signatures match (V8's wasm-to-fast-API
wrapper: no JS values on the way), and a FunctionCallback for any other
caller. A body that runs the cart's code (`glGetString`, `glMapBufferRange`
and `glUnmapBuffer` allocate in cart memory) has only the callback. Imports
find cart memory through its backing store (`wc_refresh_memory`), with no
V8 call. Key non-obvious behaviors:

**FBO Redirect**: All `glBindFramebuffer(GL_FRAMEBUFFER, 0)` calls are intercepted and redirected to a capture FBO. This lets the host blit the cart's output to the screen with letterboxing. `glClear` on the redirect FBO is suppressed after a cart blit to prevent wiping captured content.

**GL Version Filtering**: `glGetString(GL_VERSION)` returns `"OpenGL ES 3.0 wasmcart"` regardless of actual driver. Extensions are filtered to a WebGL2-compatible subset (7 extensions). This prevents Skia/Ganesh from probing for ES 3.1+ functions that aren't in the WASM import table. 28 ES 3.1+ functions are registered as no-op stubs for safety.

**`glGetInternalformativ`**: Required for Skia Ganesh GPU rendering. Ganesh queries max MSAA samples — without this function, `maxSamples=0` → render target creation fails → software fallback at ~60 FPS instead of 700+ FPS GPU.

**Signed Blit Coordinates**: `glBlitFramebuffer` source rect dimensions must be computed with signed math. Ganesh uses Y-inverted blits (srcY0 > srcY1). Storing the height as `uint32_t` causes wrap-around to ~4 billion → corrupted blit → black screen.

**Client-Side Vertex Arrays**: gl4es carts pass WASM memory offsets as "pointers" to `glVertexAttribPointer` with no VBO bound. The bridge tracks `GL_ARRAY_BUFFER` binding state and uploads client-side data to temp VBOs at draw time.

### Import call cost

GL imports and the direct WebGPU imports are V8 fast API functions (above):
the cart's wasm calls the C function directly. As FunctionCallbacks, each
call entered V8's callback machinery and converted its arguments from JS
values, which cost 0.3-0.8 ms a frame on cartwheel's 3D carts on a Ryzen AI
9 HX 370 (wasmcart-libretro's performance log).

### Wayland vs X11

SDL2 may choose X11 (via XWayland) on Wayland sessions. The `egl_create_window_surface` code does a runtime check on `wm_info.subsystem` (not compile-time `#ifdef`). Both backends work, but compositor behavior may differ for vsync.

### Platform-Specific

- **macOS / Windows**: Require [ANGLE](https://github.com/google/angle) for GLES3 (no native GLES). Set `ANGLE_DIR` in cmake.
- **macOS**: Link `-framework Security -framework SystemConfiguration` (libnode TLS).
- **Windows**: MSVC needs `/std:c++20 /Zc:__cplusplus` (CXX only) + static CRT (`/MT`).
- **Windows**: `clock_gettime` → `QueryPerformanceCounter` (`#ifdef _WIN32`).

## License

MIT
