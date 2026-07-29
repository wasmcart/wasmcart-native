// cart_host.h — Internal cart host types and functions

#ifndef WC_CART_HOST_INTERNAL_H
#define WC_CART_HOST_INTERNAL_H

#include "../include/wasmcart_host.h"
#include "abi.h"

// ─── Internal host struct ──────────────────────────────────────────────────

struct wc_host {
    // V8 state (opaque, managed by cart_host.cpp)
    void* v8_state;

    // WASM memory (refreshed after grow)
    uint8_t* memory;
    uint32_t memory_size;

    // Raw WASM bytes (kept for module compilation)
    uint8_t* wasm_bytes;
    size_t   wasm_bytes_len;

    // Function export handles (opaque, point into v8_state)
    void* fn_wc_get_info;
    void* fn_wc_init;
    void* fn_wc_render;

    // Cart info (parsed from wc_get_info return)
    wc_cart_info_t info;
    wc_manifest_t manifest;

    // GL state
    bool uses_gl;
    wc_gl_get_proc_fn gl_loader;

    // Audio ring buffer state
    uint32_t audio_read_cursor;

    // Error state
    bool trapped;
    bool init_deferred;  // _initialize/wc_init not yet called
    wc_host_options_t deferred_opts;  // saved for finish_init

    // .wasc archive (kept open for wc_load_asset)
    void* archive;  // mz_zip_archive*

    // Rumble backend supplied by the embedder (libretro, SDL, ...).
    // has_rumble == NULL means "no backend": the imports become silent no-ops
    // and wc_pad_has_rumble reports 0.
    wc_rumble_backend_t rumble;

    // Text input (ABI v3). Characters, not scancodes -- the platform has
    // already applied layout, shift, dead keys and IME composition, which is
    // work a cart must never redo. Same split every engine uses: SDL_TEXTINPUT
    // vs SDL_KEYDOWN, glfwSetCharCallback vs glfwSetKeyCallback, WM_CHAR vs
    // WM_KEYDOWN.
    //
    // Off until the cart calls wc_text_input_begin(), so a cart that never asks
    // cannot be surprised by text and an embedder may forward platform text
    // unconditionally. Queued strings are dropped on end(), so what was typed
    // into one field cannot resurface in the next.
    bool  text_active;
    char* text_queue;        // NUL-separated UTF-8 strings
    size_t text_queue_len;
    size_t text_queue_cap;
};

// ─── Asset loading ─────────────────────────────────────────────────────

int  wc_archive_open(wc_host_t* host, const char* path);
int  wc_archive_open_memory(wc_host_t* host, const uint8_t* data, size_t len);
void wc_archive_close(wc_host_t* host);

int32_t wc_archive_load_asset(wc_host_t* host, const char* path, uint8_t* dest, uint32_t max_size);
int32_t wc_archive_asset_size(wc_host_t* host, const char* path);

int wc_parse_manifest(wc_host_t* host, const char* json, size_t len);

// ─── GL imports ────────────────────────────────────────────────────────

void wc_gl_imports_init(wc_host_t* host);

// ─── WASI stubs ────────────────────────────────────────────────────────
// (handled in cart_host.cpp for V8 version)

// ─── Env imports ───────────────────────────────────────────────────────
// (handled in cart_host.cpp for V8 version)

#endif // WC_CART_HOST_INTERNAL_H
