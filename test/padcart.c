/*
 * padcart -- fixture for the four pads (test/pads_test.sh here, wasmcart-
 * libretro's test/run-tests.sh): it shows and publishes what the host wrote
 * into each wc_pad_t, so a test can drive pads 1-3 and see that each one
 * reached the cart in its own slot.
 *
 * 64x64, a 16-pixel column per pad (pad 0 on the left), four 16-row bands:
 *   rows  0-15  connected: green (22ff22), else red (ff2222)
 *   rows 16-31  buttons: the colour IS the low 24 bits of the mask (0xRRGGBB)
 *   rows 32-47  left stick: red = (left_x >> 8) + 128, green = (left_y >> 8) + 128
 *   rows 48-63  triggers: red = left_trigger >> 7, green = right_trigger >> 7
 * A disconnected pad's bands below the first are black.
 * Debug fields (wc_debug_state): pad.connected (u8 x4), pad.buttons (u32 x4),
 * pad.lx / pad.ly (i16 x4).
 *
 * Rebuild (wasi-sdk's clang; a wasmcart checkout for wasmcart.h / wc_cart.h):
 *   $WASI_SDK/bin/clang --target=wasm32 -O2 -nostdlib -I../wasmcart/include \
 *     -Wl,--no-entry -Wl,--allow-undefined -o padcart.wasm test/padcart.c
 *   npx wasmcart-pack --wasm padcart.wasm --name padcart --players 4 -o test/padcart.wasc
 */
#include "wasmcart.h"
#include "wc_cart.h"

_Static_assert(WC_ABI_VERSION == 4, "padcart expects wasmcart ABI v4");

#define WIDTH  64
#define HEIGHT 64

static uint32_t framebuffer[WIDTH * HEIGHT];
static wc_pad_t pads[4];
static wc_time_t time_info;
static wc_info_t info;
static uint8_t connected[4];
static uint32_t buttons[4];
static int16_t lx[4], ly[4];

WC_DEBUG_FIELDS(
    WC_DBG_ARR("pad.connected", connected, WC_DBG_U8, 4),
    WC_DBG_ARR("pad.buttons", buttons, WC_DBG_U32, 4),
    WC_DBG_ARR("pad.lx", lx, WC_DBG_I16, 4),
    WC_DBG_ARR("pad.ly", ly, WC_DBG_I16, 4)
)

__attribute__((export_name("wc_get_info")))
wc_info_t* wc_get_info(void) {
    info.version = WC_ABI_VERSION;
    info.width = WIDTH;
    info.height = HEIGHT;
    info.fb_ptr = (uint32_t)framebuffer;
    info.input_ptr = (uint32_t)pads;
    info.time_ptr = (uint32_t)&time_info;
    info.flags = WC_FLAG_DEBUG;
    return &info;
}

__attribute__((export_name("wc_init")))
void wc_init(void) {}

static void band(int pad, int row, uint32_t rgb) {
    for (int y = row; y < row + 16; y++)
        for (int x = pad * 16; x < pad * 16 + 16; x++) framebuffer[y * WIDTH + x] = 0xff000000u | rgb;
}

__attribute__((export_name("wc_render")))
void wc_render(void) {
    for (int i = 0; i < 4; i++) {
        const wc_pad_t* p = &pads[i];
        connected[i] = p->connected;
        buttons[i] = p->connected ? p->buttons : 0;
        lx[i] = p->connected ? p->left_x : 0;
        ly[i] = p->connected ? p->left_y : 0;
        band(i, 0, p->connected ? 0x22ff22 : 0xff2222);
        band(i, 16, buttons[i] & 0xffffff);
        band(i, 32, p->connected ? (uint32_t)((lx[i] >> 8) + 128) << 16 | (uint32_t)((ly[i] >> 8) + 128) << 8 : 0);
        band(i, 48, p->connected ? (uint32_t)(p->left_trigger >> 7) << 16 | (uint32_t)(p->right_trigger >> 7) << 8 : 0);
    }
}
