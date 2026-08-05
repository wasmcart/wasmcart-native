// seed_test.c — RNG seeding works in both directions.
//
// Two normal loads must render DIFFERENT frames (entropy: the every-boot-
// same-shuffle bug stays dead), and two loads pinned to the same rng_seed
// must render IDENTICAL frames (replay determinism is intact). Asserting
// both means the test can't pass vacuously: a host that ignores seeding
// entirely fails the first check, one that always randomizes fails the
// second.
//
// Usage: ./seed_test ../wasmcart/test/fixtures/detrng.wasc

#include "wasmcart_host.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t load_and_hash(const char* cart_path, const uint32_t* pinned) {
    wc_host_t* host = wc_host_create();
    if (!host) { fprintf(stderr, "FAIL: wc_host_create\n"); exit(2); }

    wc_host_options_t opts = {0};
    if (pinned) {
        opts.rng_seed = *pinned;
        opts.rng_seed_set = true;
    }
    if (wc_host_load_file(host, cart_path, &opts) != 0) {
        fprintf(stderr, "FAIL: load %s\n", cart_path);
        exit(2);
    }

    // enter_v8 only after create() — create() initialises V8
    wc_host_enter_v8();
    for (int i = 0; i < 3; i++) {
        wc_host_set_time(host, i * 16.7, 16.7, (uint32_t)i);
        wc_host_run_frame(host);
    }

    uint32_t w = 0, h = 0;
    const uint8_t* fb = wc_host_get_framebuffer(host, &w, &h);
    if (!fb || !w || !h) { fprintf(stderr, "FAIL: no framebuffer\n"); exit(2); }

    uint32_t hash = 2166136261u; // FNV-1a
    for (uint32_t i = 0; i < w * h * 4; i++) hash = (hash ^ fb[i]) * 16777619u;

    wc_host_exit_v8();
    wc_host_destroy(host);
    return hash;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <detrng.wasc>\n", argv[0]);
        return 1;
    }
    const char* cart = argv[1];

    uint32_t a = load_and_hash(cart, NULL);
    uint32_t b = load_and_hash(cart, NULL);
    uint32_t pin = 1234;
    uint32_t c = load_and_hash(cart, &pin);
    uint32_t d = load_and_hash(cart, &pin);

    int ok = 1;
    if (a == b) {
        fprintf(stderr, "FAIL: two unpinned loads rendered identical frames "
                        "(%08x) — entropy seeding not happening\n", a);
        ok = 0;
    }
    if (c != d) {
        fprintf(stderr, "FAIL: two loads pinned to the same seed differ "
                        "(%08x vs %08x) — determinism broken\n", c, d);
        ok = 0;
    }
    if (!ok) return 1;

    printf("PASS: unpinned %08x != %08x, pinned %08x == %08x\n", a, b, c, d);
    return 0;
}
