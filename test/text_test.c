/*
 * text_test — drive wasmcart's text-input ABI through the C host.
 *
 * Text input hands the cart CHARACTERS the platform already composed (layout,
 * shift, dead keys, IME), not scancodes. The interesting cases are therefore
 * multi-byte: a UTF-8 sequence split or truncated in transit would still
 * "arrive" if you only counted callbacks, so this asserts on the exact bytes
 * the cart accumulated.
 *
 * Build (needs a built libwasmcart.a and libnode):
 *   gcc -O0 -o text_test test/text_test.c -Iinclude -Isrc \
 *       build/libwasmcart.a deps/libnode/libnode.a -lstdc++ -lm -lpthread -ldl
 *
 * Run:
 *   ./text_test <textauto.wasc> <active_off> <calls_off> <len_off> <buf_off>
 *
 * The offsets are the cart's debug-field addresses. They shift whenever the
 * fixture is recompiled (linking malloc alone moved them 16 bytes), so they are
 * passed in rather than baked -- read them with wasmcart's readDebugState().
 *
 * NOTE ON ORDERING: wc_host_enter_v8() must be called AFTER wc_host_create(),
 * because create() is what initialises V8. Calling it first segfaults before
 * main() prints anything, which reads like a bug in the host rather than in
 * the harness.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasmcart_host.h"

static int failures = 0;

static void check(const char* what, long got, long want) {
    if (got == want) {
        printf("  ok    %-34s %ld\n", what, got);
    } else {
        printf("*** FAIL %-34s got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

static uint32_t rd32(wc_host_t* h, uint32_t off) {
    uint32_t size = 0;
    const uint8_t* m = wc_host_get_memory(h, &size);
    if (!m || off + 4 > size) return 0xFFFFFFFFu;
    uint32_t v;
    memcpy(&v, m + off, 4);
    return v;
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <cart.wasc> <active> <calls> <len> <buf>\n", argv[0]);
        return 2;
    }
    const char* cart = argv[1];
    uint32_t off_active = (uint32_t)strtoul(argv[2], NULL, 0);
    uint32_t off_calls  = (uint32_t)strtoul(argv[3], NULL, 0);
    uint32_t off_len    = (uint32_t)strtoul(argv[4], NULL, 0);
    uint32_t off_buf    = (uint32_t)strtoul(argv[5], NULL, 0);

    wc_host_t* h = wc_host_create();
    wc_host_enter_v8();  /* AFTER create -- see note above */

    wc_host_options_t opts;
    memset(&opts, 0, sizeof opts);
    if (wc_host_load_file(h, cart, &opts) != 0) {
        fprintf(stderr, "load failed: %s\n", cart);
        return 1;
    }

    /* Text pushed before the cart enables input must be ignored. That is what
       lets an embedder forward every platform text event unconditionally. */
    wc_host_push_text(h, "ignored", 7);
    wc_host_run_frame(h);  /* the fixture enables text input during render */

    check("cart sees active", rd32(h, off_active), 1);
    check("host reports active", wc_host_text_input_active(h), 1);
    check("no text before begin()", rd32(h, off_calls), 0);

    /* ASCII plus 2-, 3- and 4-byte sequences. A cart must never have to know a
       keyboard layout to receive these. */
    wc_host_push_text(h, "Hi @", 4);
    wc_host_push_text(h, "\xc3\xa9", 2);          /* U+00E9  e-acute      */
    wc_host_push_text(h, "\xe3\x81\x82", 3);      /* U+3042  hiragana a   */
    wc_host_push_text(h, "\xf0\x9f\x8e\xae", 4);  /* U+1F3AE game pad     */
    wc_host_run_frame(h);

    check("wc_on_text call count", rd32(h, off_calls), 4);
    check("bytes received (UTF-8)", rd32(h, off_len), 13);

    uint32_t size = 0;
    const uint8_t* m = wc_host_get_memory(h, &size);
    const char* want = "Hi @\xc3\xa9\xe3\x81\x82\xf0\x9f\x8e\xae";
    int same = m && (memcmp(m + off_buf, want, 13) == 0);
    check("bytes match exactly", same, 1);
    if (!same && m) {
        printf("       got: \"");
        for (uint32_t i = 0; i < 13; i++) putchar(m[off_buf + i]);
        printf("\"\n");
    }

    wc_host_exit_v8();
    wc_host_destroy(h);

    printf(failures ? "\nFAILED (%d)\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
