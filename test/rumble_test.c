/*
 * rumble_test — drive wasmcart's rumble fixture through the C host with a
 * recording backend, so the ABI's clamping rules are checked without hardware.
 *
 * The fixture (wasmcart/test/fixtures/rumble.wasc) deliberately sends
 * out-of-range magnitudes, an over-long duration, and an invalid pad id.
 * SPEC.md says the host CLAMPS rather than rejects, so this asserts on what
 * the backend actually receives.
 *
 * Build (needs a built libwasmcart.a and libnode):
 *   gcc -O0 -o rumble_test test/rumble_test.c -Iinclude -Isrc \
 *       build/libwasmcart.a deps/libnode/libnode.a -lstdc++ -lm -lpthread -ldl
 *
 * Run:
 *   ./rumble_test ../wasmcart/test/fixtures/rumble.wasc
 *
 * Expected: no "*** FAIL" lines, and exactly 2 rumble() calls -- the third is
 * pad 99, which must be dropped.
 */
#include <stdio.h>
#include <string.h>
#include "wasmcart_host.h"

static int calls = 0;
static int has_q = 0;

static int t_has(void* u, uint32_t pad) { (void)u; has_q++;
    printf("  has_rumble(pad=%u) -> 1\n", pad); return 1; }

static void t_rumble(void* u, uint32_t pad, float lo, float hi, uint32_t ms) {
    (void)u; calls++;
    printf("  rumble(pad=%u, low=%.3f, high=%.3f, ms=%u)\n", pad, lo, hi, ms);
    if (lo < 0.f || lo > 1.f || hi < 0.f || hi > 1.f)
        printf("  *** FAIL: magnitude outside 0..1\n");
    if (ms > WC_RUMBLE_MAX_MS)
        printf("  *** FAIL: duration above cap %d\n", WC_RUMBLE_MAX_MS);
    if (pad >= WC_MAX_PADS)
        printf("  *** FAIL: pad id out of range\n");
}

static void t_stop(void* u, uint32_t pad) { (void)u;
    printf("  rumble_stop(pad=%u)\n", pad); }

int main(int argc, char** argv) {
    wc_host_t* h = wc_host_create();
    if (!h) { printf("host create failed\n"); return 1; }
    wc_rumble_backend_t be = { t_has, t_rumble, t_stop, NULL };
    wc_host_set_rumble_backend(h, &be);

    wc_host_options_t o; memset(&o, 0, sizeof o);
    o.preferred_width = 64; o.preferred_height = 64; o.host_fps = 60;
    if (wc_host_load_file(h, argv[1], &o) != 0) {
        printf("load failed\n"); return 1;
    }
    printf("loaded. has_rumble queried %d time(s) during init\n", has_q);
    wc_host_enter_v8();   /* the header says hosts calling from one thread hold this */
    wc_pad_t pads[WC_MAX_PADS]; memset(pads, 0, sizeof pads);
    for (int f = 1; f <= 5; f++) {
        printf("frame %d:\n", f);
        wc_host_set_pads(h, pads);
        wc_host_run_frame(h);
    }
    printf("\ntotal rumble() calls: %d\n", calls);
    wc_host_exit_v8();
    wc_host_destroy(h);
    return 0;
}
