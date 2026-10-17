#!/bin/sh
# Asset prefetch (wasmcart SPEC.md, "Asset prefetch") through the native player.
# test/prefetch.wasc is wasmcart's test/fixtures/prefetch.c (copied here with
# its source, so both hosts prove the same contract from the same fields): on
# its first frame it prefetches big.bin (8 MiB, deflated), small.bin (stored)
# and a missing path, polls every frame, reads big in one wc_load_asset once
# READY and checks every byte (hash is its FNV-1a), then prefetches small
# again, cancels it, asks once more, and reads it once READY.
#
# Checks (the player's --debug-dump of the cart's fields, and its log):
#   - the host wrote WC_HOST_FLAG_ASSET_PREFETCH, the missing path answered
#     MISSING, big became READY on a later frame than the call that started
#     it (frames_to_ready >= 2: the fetch is asynchronous)
#   - both reads were served from the host's copy (the log says so, and big
#     was never "loaded" from the archive), with the right bytes and sizes
#   - cancel + prefetch again reached READY (cancel_ok)
# The must-fail control runs the same cart with WASMCART_NO_PREFETCH=1, which
# is an older host: the imports are auto-stubbed (logged, returning 0) and the
# flag is clear. The cart never calls them (calls 0), reads synchronously and
# still checks out (hash_ok, small_ok), and every prefetch check above must
# FAIL on that run.
#
# Run: sh test/prefetch_test.sh [path/to/wasmcart-run]
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="${1:-$HERE/../build/wasmcart-run}"
OUT="$HERE/../out/prefetch-test"
mkdir -p "$OUT"
fail=0
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy WASMCART_NO_WGPU=1
# FNV-1a of big.bin's pattern, what the cart's `hash` must be
HASH=$(node -e 'let h=2166136261;for(let i=0;i<(8<<20);i++){h=Math.imul(h^((i*31+7)&255),16777619)>>>0}console.log(h)')

# run NAME [env...]: the cart to frame 120, dumping its state after it
run() {
  name=$1; shift
  rm -f "$OUT/$name.json"
  env "$@" timeout -s KILL 60 "$RUN" "$HERE/prefetch.wasc" --fixed-step 16.6667 \
    --debug-dump 120 "$OUT/$name.json" > "$OUT/$name.log" 2>&1 &
  pid=$!
  n=0
  while [ ! -s "$OUT/$name.json" ] && [ $n -lt 400 ]; do sleep 0.05; n=$((n + 1)); done
  kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
  [ -s "$OUT/$name.json" ] || { echo "*** FAIL $name: no dump (the run ended?)"; tail -5 "$OUT/$name.log"; fail=1; }
}
field() { # dump, name -> value
  node -e "const s=JSON.parse(require('fs').readFileSync(process.argv[1],'utf8'));console.log(JSON.stringify(s[process.argv[2]]))" "$1" "$2" 2>/dev/null
}
say() { [ "$show" = show ] && echo "$1"; }
# the prefetch checks: 0 when every one holds, 1 otherwise (quiet unless $show = show)
prefetch_checks() {
  name=$1; bad=0
  d="$OUT/$name.json"
  want() { # field, expected
    got=$(field "$d" "$1")
    if [ "$got" = "$2" ]; then say "  ok    $name: $1 $got"
    else say "*** FAIL $name: $1 is ${got:-missing}, want $2"; bad=1; fi
  }
  want has_flag 1
  want state_missing -1
  want state_big 1
  want size_big 8388608
  want read_bytes 8388608
  want hash "$HASH"
  want hash_ok 1
  want state_small_re 1
  want cancel_ok 1
  want size_small 4096
  want small_ok 1
  want phase 4
  fr=$(field "$d" frames_to_ready)
  if [ -n "$fr" ] && [ "$fr" -ge 2 ] 2>/dev/null; then say "  ok    $name: big.bin READY on frame $fr (the call that started it answered PENDING)"
  else say "*** FAIL $name: frames_to_ready is ${fr:-missing}, want >= 2"; bad=1; fi
  for a in 'big.bin served from prefetch (8388608 bytes)' 'small.bin served from prefetch (4096 bytes)'; do
    if grep -q "asset $a" "$OUT/$name.log"; then say "  ok    $name: $a"
    else say "*** FAIL $name: no '$a' in the log"; bad=1; fi
  done
  if grep -q 'loaded asset big.bin' "$OUT/$name.log"; then say "*** FAIL $name: big.bin was read from the archive after all"; bad=1
  else say "  ok    $name: big.bin never read from the archive"; fi
  if grep -q 'stubbed missing import env.wc_asset_prefetch' "$OUT/$name.log"; then say "*** FAIL $name: the host stubbed the prefetch imports"; bad=1
  else say "  ok    $name: the host provided the prefetch imports"; fi
  return $bad
}

run prefetch
show=show prefetch_checks prefetch || fail=1

# the control: an older host (no imports, no flag); the cart's fallback still reads both
run control WASMCART_NO_PREFETCH=1
d="$OUT/control.json"
for pair in has_flag=0 calls=0 state_big=99 read_bytes=8388608 hash="$HASH" hash_ok=1 small_ok=1 phase=4; do
  k=${pair%%=*}; v=${pair#*=}; got=$(field "$d" "$k")
  if [ "$got" = "$v" ]; then echo "  ok    control: $k $got"; else echo "*** FAIL control: $k is ${got:-missing}, want $v"; fail=1; fi
done
if grep -q 'stubbed missing import env.wc_asset_prefetch (returns 0)' "$OUT/control.log"; then echo "  ok    control: the import was stubbed, as an older host does"
else echo "*** FAIL control: the prefetch import was not stubbed"; fail=1; fi
if show= prefetch_checks control; then echo "*** FAIL control: the prefetch checks passed without the prefetch (they prove nothing)"; fail=1
else echo "  ok    control: the prefetch checks fail without the prefetch"; fi

[ "$fail" = 0 ] && echo "all prefetch checks passed" || echo "FAILED"
exit $fail
