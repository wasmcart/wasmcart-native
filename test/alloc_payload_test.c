/*
 * alloc_payload_test -- text and peer payloads reach the cart in blocks the
 * CART allocates (wasmcart SPEC.md, "Cart memory the host writes").
 *
 * wc_on_text and wc_peer_on_message take (ptr, len) into the cart's memory.
 * The host allocates that block with the cart's wc_alloc, copies the bytes,
 * calls the cart, then wc_frees it. A cart with no allocator at all is stopped
 * with an error (it used to have text silently dropped, and peer messages
 * written into a page the host grew onto the end of its memory). So is a cart
 * that exports only malloc/memalign/free: wasmcart 0.32 never calls malloc.
 *
 * Fixtures: test/alloc/text_{wcalloc,malloc,noalloc}.wasm (2D carts built
 * from test/alloc/alloccart.c with -DTEXT=1). Each keeps its counters at the
 * start of its framebuffer: [0] allocations, [1] frees, [2] wc_on_text calls,
 * [3] text bytes, [4] peer messages, [5] peer bytes; the text it received is
 * at byte 32, the peer bytes at byte 160.
 *
 * Build (needs a built libwasmcart.a and libnode):
 *   gcc -O0 -o alloc_payload_test test/alloc_payload_test.c -Iinclude -Isrc \
 *       build/libwasmcart.a deps/libnode/libnode.a -lstdc++ -lm -lpthread -ldl
 * Run:
 *   ./alloc_payload_test test/alloc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasmcart_host.h"

static int failures = 0;

static void check(const char* what, long got, long want) {
    if (got == want) {
        printf("  ok    %-44s %ld\n", what, got);
    } else {
        printf("*** FAIL %-44s got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

static const uint8_t* fbmem(wc_host_t* h) {
    uint32_t size = 0;
    const uint8_t* m = (const uint8_t*)wc_host_get_memory(h, &size);
    uint32_t fb = wc_host_get_cart_info(h)->fb_ptr;
    if (!m || fb + 1024 > size) return NULL;
    return m + fb;
}

static long fbword(wc_host_t* h, int i) {
    const uint8_t* f = fbmem(h);
    uint32_t v = 0;
    if (f) memcpy(&v, f + i * 4, 4);
    return f ? (long)v : -1;
}

static int send_nothing(void* user, const uint8_t* data, uint32_t len) {
    (void)user; (void)data; (void)len;
    return 0;
}

static wc_host_t* load(const char* dir, const char* name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.wasm", dir, name);
    wc_host_t* h = wc_host_create();
    wc_host_enter_v8();  /* AFTER create: create() initialises V8 */
    wc_host_options_t opts;
    memset(&opts, 0, sizeof opts);
    if (wc_host_load_file(h, path, &opts) != 0) {
        printf("*** FAIL load %s\n", path);
        failures++;
        return NULL;
    }
    wc_host_run_frame(h);  /* the cart begins text input on its first frame */
    return h;
}

/* Text + a host-supplied peer message through an allocating cart. */
static void allocating(const char* dir, const char* name, long frees_per_payload) {
    printf("%s\n", name);
    wc_host_t* h = load(dir, name);
    if (!h) return;
    const char* text = "h\xc3\xa9llo";   /* 6 bytes */
    wc_host_push_text(h, text, 6);
    wc_host_run_frame(h);
    check("wc_on_text called", fbword(h, 2), 1);
    check("text bytes", fbword(h, 3), 6);
    check("text matches", fbmem(h) && memcmp(fbmem(h) + 32, text, 6) == 0, 1);
    check("allocations after text", fbword(h, 0), 1);
    check("frees after text", fbword(h, 1), frees_per_payload);

    int32_t id = wc_host_add_peer(h, "peer", send_nothing, NULL, WC_TRANSPORT_RELIABLE);
    wc_host_peer_recv(h, id, (const uint8_t*)"ping!", 5);
    wc_host_run_frame(h);
    check("wc_peer_on_message called", fbword(h, 4), 1);
    check("peer bytes", fbword(h, 5), 5);
    check("peer bytes match", fbmem(h) && memcmp(fbmem(h) + 160, "ping!", 5) == 0, 1);
    check("allocations after peer message", fbword(h, 0), 2);
    check("frees after peer message", fbword(h, 1), 2 * frees_per_payload);
    check("cart still running", wc_host_has_trapped(h), 0);
    wc_host_exit_v8();
    wc_host_destroy(h);
}

/* No wc_alloc/wc_free (none at all, or only malloc/memalign/free): the first
 * payload stops the cart instead of landing anywhere, and malloc is never
 * called (the cart counts its own allocations in fb word 0). */
static void no_allocator(const char* dir, const char* name) {
    printf("%s\n", name);
    wc_host_t* h = load(dir, name);
    if (!h) return;
    check("runs before any payload", wc_host_has_trapped(h), 0);
    wc_host_push_text(h, "x", 1);
    wc_host_run_frame(h);
    check("text without wc_alloc traps the cart", wc_host_has_trapped(h), 1);
    check("wc_on_text never called", fbword(h, 2), 0);
    check("the cart's own allocator never called", fbword(h, 0), 0);
    wc_host_exit_v8();
    wc_host_destroy(h);
}

static void no_allocator_peer(const char* dir, const char* name) {
    printf("%s (peer message)\n", name);
    wc_host_t* h = load(dir, name);
    if (!h) return;
    int32_t id = wc_host_add_peer(h, "peer", send_nothing, NULL, WC_TRANSPORT_RELIABLE);
    wc_host_peer_recv(h, id, (const uint8_t*)"ping!", 5);
    wc_host_run_frame(h);
    check("peer message without wc_alloc traps the cart", wc_host_has_trapped(h), 1);
    check("wc_peer_on_message never called", fbword(h, 4), 0);
    check("the cart's own allocator never called", fbword(h, 0), 0);
    wc_host_exit_v8();
    wc_host_destroy(h);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <test/alloc directory>\n", argv[0]);
        return 2;
    }
    const char* which = argc > 2 ? argv[2] : "all";
    if (!strcmp(which, "all") || !strcmp(which, "wcalloc")) allocating(argv[1], "text_wcalloc", 1);
    if (!strcmp(which, "all") || !strcmp(which, "noalloc")) no_allocator(argv[1], "text_noalloc");
    if (!strcmp(which, "all") || !strcmp(which, "noallocpeer")) no_allocator_peer(argv[1], "text_noalloc");
    if (!strcmp(which, "all") || !strcmp(which, "malloc"))  no_allocator(argv[1], "text_malloc");
    if (!strcmp(which, "all") || !strcmp(which, "mallocpeer")) no_allocator_peer(argv[1], "text_malloc");
    printf(failures ? "\nFAILED (%d)\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
