/*
 * net_test — does node's networking actually work inside the embedded host?
 *
 * It did not, for a reason that hid well. net.Socket.connect() defers its dial
 * through process.nextTick, and the nextTick queue is drained by node's
 * CallbackScope, not by uv_run. Without one, a socket sat in `connecting`
 * forever: no error, no close event, and strace showed the process never made
 * a single network syscall.
 *
 * Everything nearby worked, which is what made it expensive to find --
 * setImmediate, queueMicrotask, setTimeout, fs.readFile, even dns.lookup all
 * fired, and the raw tcp_wrap binding connected on the first try. Only the
 * layers that route through nextTick were dead.
 *
 * So this test asserts on nextTick DIRECTLY as well as on a real connection.
 * A future change that drops the CallbackScope from pump_node() would still
 * pass a "does WebSocket connect" test on a fast local server if the timing
 * happened to work out; it cannot pass this one.
 *
 * Build (needs a built libwasmcart.a and libnode):
 *   gcc -O0 -o net_test test/net_test.c -Iinclude -Isrc \
 *       build/libwasmcart.a deps/libnode/libnode.a -lstdc++ -lm -lpthread -ldl
 *
 * Run (needs wasmcart's test server on the given port):
 *   node ../wasmcart/test/wsserver.mjs --port 8796 &
 *   ./net_test 8796
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasmcart_host.h"

int wc_test_eval_flag(const char* code, const char* flag, int iters);

static int failures = 0;

static void check(const char* what, int got, int want) {
    if (got == want) {
        printf("  ok    %s\n", what);
    } else {
        printf("*** FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

int main(int argc, char** argv) {
    const char* port = (argc > 1) ? argv[1] : "8796";

    wc_host_t* h = wc_host_create();
    wc_host_enter_v8();

    /* The module system must exist at all: LoadEnvironment("") leaves require
       undefined, and node's networking is reached through it. */
    check("require is available",
          wc_test_eval_flag("globalThis.__f = (typeof globalThis.__wc_require === 'function');",
                            "__f", 1), 1);

    /* The actual root cause. If this regresses, sockets die and the symptom
       will point somewhere else entirely. */
    check("process.nextTick runs",
          wc_test_eval_flag("globalThis.__f=false; process.nextTick(()=>{globalThis.__f=true;});",
                            "__f", 100), 1);

    char js[512];

    /* A raw TCP connection, no DNS involved. */
    snprintf(js, sizeof js,
        "globalThis.__f=false;"
        "const s=globalThis.__wc_require('net').connect(%s,'127.0.0.1',()=>{globalThis.__f=true;s.end();});"
        "s.on('error',()=>{globalThis.__f=false;});", port);
    check("net.connect reaches the server", wc_test_eval_flag(js, "__f", 400), 1);

    /* And the thing this was all for: a WebSocket round trip. */
    snprintf(js, sizeof js,
        "globalThis.__f=false;"
        "const w=new WebSocket('ws://127.0.0.1:%s/echo');"
        "w.onopen=()=>w.send('ping');"
        "w.onmessage=e=>{globalThis.__f=(String(e.data)==='ping'); w.close();};", port);
    check("WebSocket echo round-trips", wc_test_eval_flag(js, "__f", 400), 1);

    wc_host_exit_v8();
    wc_host_destroy(h);

    printf(failures ? "\nFAILED (%d)\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
