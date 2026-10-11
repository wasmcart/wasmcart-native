// host_threads_test.c — wc_host_info_t.hardware_threads (wasmcart SPEC.md,
// "wc_host_info_t"): written for a cart that sets WC_FLAG_HOST_THREADS, and
// nothing past the 20-byte struct of a cart that does not.
//
// The fixtures copy what they saw at wc_init into debug fields: "threads"
// (hostthreads.wasc) and "sentinel", the word after a legacy cart's 20-byte
// struct (hostthreads_legacy.wasc, which must still read 0x5e771e15).
//
// Usage: ./host_threads_test ../wasmcart/test/fixtures/hostthreads.wasc \
//                            ../wasmcart/test/fixtures/hostthreads_legacy.wasc

#include "wasmcart_host.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t rd32(const uint8_t* m, uint32_t off) { uint32_t v; memcpy(&v, m + off, 4); return v; }

// the u32 debug field `name` after one frame
static uint32_t debug_u32(const char* cart_path, const char* name) {
    wc_host_t* host = wc_host_create();
    if (!host) { fprintf(stderr, "FAIL: wc_host_create\n"); exit(2); }
    wc_host_options_t opts = {0};
    if (wc_host_load_file(host, cart_path, &opts) != 0) { fprintf(stderr, "FAIL: load %s\n", cart_path); exit(2); }
    wc_host_enter_v8();
    wc_host_set_time(host, 0, 16.7, 0);
    wc_host_run_frame(host);
    uint32_t table = wc_host_debug_state(host);
    uint32_t size = 0;
    const uint8_t* mem = (const uint8_t*)wc_host_get_memory(host, &size);
    if (!table) { fprintf(stderr, "FAIL: %s has no debug state\n", cart_path); exit(2); }
    for (uint32_t e = table; e + 16 <= size && rd32(mem, e); e += 16) {
        if (strcmp((const char*)mem + rd32(mem, e), name) != 0) continue;
        uint32_t v = rd32(mem, rd32(mem, e + 4));
        wc_host_exit_v8();
        wc_host_destroy(host);
        return v;
    }
    fprintf(stderr, "FAIL: no debug field %s in %s\n", name, cart_path);
    exit(2);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <hostthreads.wasc> <hostthreads_legacy.wasc>\n", argv[0]);
        return 1;
    }
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    uint32_t threads = debug_u32(argv[1], "threads");
    uint32_t sentinel = debug_u32(argv[2], "sentinel");
    int ok = 1;
    if (threads == 0 || (long)threads != cores) {
        fprintf(stderr, "FAIL: the cart read %u hardware threads, this machine has %ld\n", threads, cores);
        ok = 0;
    }
    if (sentinel != 0x5e771e15u) {
        fprintf(stderr, "FAIL: the word after a 20-byte host info became %08x (the host wrote past the struct)\n", sentinel);
        ok = 0;
    }
    if (!ok) return 1;
    printf("PASS: hardware_threads %u; a cart without the flag keeps its word after the struct\n", threads);
    return 0;
}
