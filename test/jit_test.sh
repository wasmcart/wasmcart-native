#!/bin/sh
# Runtime code generation (wasmcart SPEC.md) through the native player.
# test/jitcart.wasc is wasmcart's test/fixtures/jitcart.c: it links a generated
# module, calls it through a function pointer, unlinks it and links it again
# into the freed slot. With WASMCART_JIT=0 (the must-fail control) every link
# is -1 and the player prints the loud notice.
#
# Run: sh test/jit_test.sh <path to wasmcart-run>
RUN="${1:-build/wasmcart-run}"
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/../out/jit-test"
mkdir -p "$OUT"
fail=0
run() { # $1 = WASMCART_JIT value
  rm -f "$OUT/dump$1.json"
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy WASMCART_JIT=$1 \
    timeout -s KILL 30 "$RUN" "$DIR/jitcart.wasc" --fixed-step 16.6667 \
    --debug-dump 3 "$OUT/dump$1.json" > "$OUT/run$1.log" 2>&1 &
  pid=$!
  for _ in $(seq 1 200); do [ -s "$OUT/dump$1.json" ] && break; sleep 0.05; done
  kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
}
check() { # $1 = label, $2 = file, $3 = expected substring
  if grep -q "$3" "$2"; then echo "  ok    $1"; else echo "*** FAIL $1 (wanted $3 in $2)"; fail=1; fi
}
run 1
check "link, call through the slot, unlink, re-link into the same slot" "$OUT/dump1.json" \
  '"off": -1, "max_module": 4194304, "link_ret": 2, "cell": 2, "call_result": 12, "relink_ret": 2, "cell2": 2, "done": 1'
run 0
check "off switch: the cart sees key 1 = 1 and -1 from the link" "$OUT/dump0.json" '"off": 1, "max_module": 4194304, "link_ret": -1, "cell": -1'
check "off switch: the loud notice" "$OUT/run0.log" 'RUNTIME CODE GENERATION IS OFF'
node "$DIR/jit_generation.mjs" || fail=1
exit $fail
