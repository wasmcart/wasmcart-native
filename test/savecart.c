/*
 * savecart -- save-region fixture (SPEC "Saving is host-managed").
 *
 * The save region is 16 bytes; its first u32 counts the runs. wc_init reads
 * it (the host loaded any save before wc_init), keeps that value as `seen`
 * and writes seen + 1 back, so a host that persists the region and loads it
 * on the next run sees the count go up. The screen shows what wc_init read:
 * seen 0 red (ff2222), 1 green (22ff22), 2 blue (2222ff), 3+ white (ffffff).
 *
 * Rebuild (emcc + a wasmcart checkout for wasmcart.h / wc_cart.h):
 *   emcc test/savecart.c -O2 -I../wasmcart/include -s STANDALONE_WASM=1 --no-entry \
 *     -s EXPORTED_FUNCTIONS='["_wc_init","_wc_render","_wc_get_info","_wc_debug_state"]' \
 *     -s ERROR_ON_UNDEFINED_SYMBOLS=0 -o savecart.wasm
 *   npx wasmcart-pack --wasm savecart.wasm --name savecart -o test/savecart.wasc
 */
#include "wasmcart.h"
#include "wc_cart.h"

_Static_assert(WC_ABI_VERSION == 4, "savecart expects wasmcart ABI v4");

#define WIDTH  64
#define HEIGHT 64

static uint32_t framebuffer[WIDTH * HEIGHT];
static wc_pad_t pads[4];
static wc_time_t time_info;
static wc_info_t info;
static wc_host_info_t host_info;
static uint32_t save[4];
static uint32_t seen;

WC_DEBUG_FIELDS(
    WC_DBG("seen", seen, WC_DBG_U32)
)

__attribute__((export_name("wc_get_info")))
wc_info_t* wc_get_info(void) {
    info.version = WC_ABI_VERSION;
    info.width = WIDTH;
    info.height = HEIGHT;
    info.fb_ptr = (uint32_t)framebuffer;
    info.input_ptr = (uint32_t)pads;
    info.save_ptr = (uint32_t)save;
    info.save_size = sizeof save;
    info.time_ptr = (uint32_t)&time_info;
    info.host_info_ptr = (uint32_t)&host_info;
    info.flags = WC_FLAG_DEBUG;
    return &info;
}

__attribute__((export_name("wc_init")))
void wc_init(void) {
    seen = save[0];
    save[0] = seen + 1;
}

__attribute__((export_name("wc_render")))
void wc_render(void) {
    static const uint32_t colour[4] = { 0xffff2222, 0xff22ff22, 0xff2222ff, 0xffffffff };
    uint32_t c = colour[seen < 3 ? seen : 3];
    for (int i = 0; i < WIDTH * HEIGHT; i++) framebuffer[i] = c;
}
