/*
 * wgpu_alloc_test -- a WebGPU cart's adapter info and mapped ranges reach it
 * through ITS wc_alloc on wasmcart-native (wasmcart SPEC.md, "Cart memory the
 * host writes"). The WebGPU glue is wasmcart's own JS (wgpu/ beside the
 * build); this checks the native host hands it a cart whose allocator it uses.
 *
 * Fixture: test/wgpu/wgpucart.wasc, a copy of wasmcart's test/fixtures build
 * with a counting wc_alloc (alloc_count.h): t_allocs(1) counts allocations
 * made for adapter info, t_allocs(2) for mapped ranges, t_frees() wc_free.
 *
 * Build (needs a build configured with WebGPU support, so build/wgpu exists):
 *   gcc -O0 -o build/wgpu_alloc_test test/wgpu_alloc_test.c -Iinclude -Isrc \
 *       build/libwasmcart.a deps/libnode/libnode.a -lstdc++ -lm -lpthread -ldl
 * Run:
 *   WASMCART_WGPU_DIR=build/wgpu build/wgpu_alloc_test test/wgpu/wgpucart.wasc
 */
#include <stdio.h>
#include <string.h>
#include "wasmcart_host.h"

int32_t wc_test_call_export(wc_host_t*, const char*, uint32_t, uint32_t, uint32_t, int);

static int failures = 0;
static void check(const char* what, int ok, long got) {
    printf(ok ? "  ok    %-46s %ld\n" : "*** FAIL %-46s %ld\n", what, got);
    if (!ok) failures++;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <wgpucart.wasc>\n", argv[0]); return 2; }
    wc_host_t* h = wc_host_create();
    wc_host_enter_v8();  /* AFTER create: create() initialises V8 */
    wc_host_options_t opts;
    memset(&opts, 0, sizeof opts);
    if (wc_host_load_file(h, argv[1], &opts) != 0) { printf("*** FAIL load %s\n", argv[1]); return 1; }
    check("cart is a WebGPU cart", wc_host_uses_wgpu(h), wc_host_uses_wgpu(h));
    /* the compute result is read back with mapAsync a few frames in */
    int i;
    for (i = 0; i < 120 && wc_test_call_export(h, "t_allocs", 2, 0, 0, 1) <= 0 && !wc_host_has_trapped(h); i++)
        wc_host_run_frame(h);
    for (int k = 0; k < 3; k++) wc_host_run_frame(h);
    long info = wc_test_call_export(h, "t_allocs", 1, 0, 0, 1);
    long mapped = wc_test_call_export(h, "t_allocs", 2, 0, 0, 1);
    long frees = wc_test_call_export(h, "t_frees", 0, 0, 0, 0);
    check("adapter info allocated through wc_alloc", info > 0, info);
    check("mapped range allocated through wc_alloc", mapped > 0, mapped);
    check("host released blocks with wc_free", frees > 0, frees);
    check("cart did not trap", !wc_host_has_trapped(h), wc_host_has_trapped(h));
    wc_host_exit_v8();
    wc_host_destroy(h);
    printf(failures ? "\nFAILED (%d)\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
