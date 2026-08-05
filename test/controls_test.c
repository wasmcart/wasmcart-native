// controls_test.c — manifest `controls` presentation hint parses correctly.
//
// Build (no libnode needed, links the parser directly):
//   gcc -O0 -o controls_test test/controls_test.c src/asset_loader.c \
//       deps/cJSON.c deps/miniz.c -Iinclude -Isrc -Ideps -lm
//
// Cases: token mapping, unknown tokens ignored (forward compat), absent
// field leaves controls_set false, empty array is set-but-zero.

#include "cart_host.h"
#include <stdio.h>
#include <string.h>

// wc_log's shared state normally lives in cart_host.cpp
FILE* _wc_log_file = NULL;
long _wc_log_bytes = 0;

static int failures = 0;

static wc_manifest_t parse(const char* json) {
    struct wc_host host;
    memset(&host, 0, sizeof(host));
    wc_parse_manifest(&host, json, strlen(json));
    return host.manifest;
}

static void expect(const char* what, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    wc_manifest_t m;

    m = parse("{\"controls\":[\"dpad\",\"a\",\"b\",\"start\"]}");
    expect("basic set flag", m.controls_set);
    expect("basic mask", m.controls == (WC_CTRL_DPAD | WC_CTRL_A | WC_CTRL_B | WC_CTRL_START));

    m = parse("{\"controls\":[\"left_stick\",\"warp_drive\",\"r3\",42]}");
    expect("unknown tokens ignored", m.controls == (WC_CTRL_LSTICK | WC_CTRL_R3));
    expect("unknown tokens still set flag", m.controls_set);

    m = parse("{\"name\":\"nocontrols\"}");
    expect("absent field: controls_set false", !m.controls_set);

    m = parse("{\"controls\":[]}");
    expect("empty array: set but zero", m.controls_set && m.controls == 0);

    m = parse("{\"controls\":[\"left_trigger\",\"right_trigger\",\"l3\",\"select\","
              "\"x\",\"y\",\"l\",\"r\",\"right_stick\"]}");
    expect("full vocabulary", m.controls ==
        (WC_CTRL_LTRIG | WC_CTRL_RTRIG | WC_CTRL_L3 | WC_CTRL_SELECT |
         WC_CTRL_X | WC_CTRL_Y | WC_CTRL_L | WC_CTRL_R | WC_CTRL_RSTICK));

    // must-fail control: a wrong expectation must be detectable
    m = parse("{\"controls\":[\"a\"]}");
    expect("sanity: mask is not zero", m.controls != 0);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS: controls manifest parsing\n");
    return 0;
}
