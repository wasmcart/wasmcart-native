/*
 * The embedder side of input actions (wasmcart SPEC.md, "Input actions"):
 * wc_host_actions lists what test/actioncart.wasc declared, and
 * wc_host_action_bind remaps and restores. Exit 0 on success.
 *
 * Run: ./actions_api_test test/actioncart.wasc
 */
#include <stdio.h>
#include <string.h>
#include "wasmcart_host.h"

static int fail = 0;
#define CHECK(c, what) do { if (c) printf("  ok    %s\n", what); else { printf("*** FAIL %s\n", what); fail = 1; } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: actions_api_test <actioncart.wasc>\n"); return 2; }
    wc_host_t* h = wc_host_create();
    wc_host_options_t o; memset(&o, 0, sizeof o);
    o.preferred_width = 32; o.preferred_height = 32; o.host_fps = 60;
    if (wc_host_load_file(h, argv[1], &o) != 0) { printf("load failed\n"); return 1; }
    wc_host_enter_v8();
    wc_pad_t pads[WC_MAX_PADS]; memset(pads, 0, sizeof pads);
    pads[0].connected = 1;
    wc_host_set_pads(h, pads);
    wc_host_run_frame(h);

    wc_host_action_t a[8];
    int n = wc_host_actions(h, 0, a, 8);
    CHECK(n == 3, "three actions declared");
    CHECK(n == 3 && !strcmp(a[0].name, "Confirm") && !strcmp(a[0].set, "menu") && !a[0].active, "Confirm in menu, inactive");
    CHECK(n == 3 && !strcmp(a[1].name, "Pedal") && a[1].kind == 0 && a[1].input == 0 && a[1].active, "Pedal digital on A, active");
    CHECK(n == 3 && !strcmp(a[2].name, "Steer") && a[2].kind == 1 && a[2].input == 32, "Steer analog on the left stick");

    wc_host_action_bind(h, 0, 1, 35);
    wc_host_actions(h, 0, a, 8);
    CHECK(a[1].input == 35 && a[1].default_input == 0, "remap: Pedal on the right trigger");
    wc_host_action_bind(h, 0, 1, -2);
    wc_host_actions(h, 0, a, 8);
    CHECK(a[1].input == 0, "MUST FAIL control: -2 restores the default (a no-op bind would leave 35)");
    CHECK(wc_host_actions(h, 0, NULL, 0) == 3, "count without a buffer");

    wc_host_exit_v8();
    wc_host_destroy(h);
    return fail;
}
