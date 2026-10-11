#!/bin/sh
# The compiled-code cache (src/code_cache.cpp): a cart's compiled code is kept
# between runs, in a file named by the SHA-256 of its wasm, and a later run
# starts from it. A file that does not fit (garbage, another V8) is ignored
# and the cart compiles as usual.
#
# --wasm-caching-threshold=1 makes V8 offer the cache after any optimized
# function (a game writes it after 256 KB of them), so a small cart writes one.
#
# Run: sh test/code_cache_test.sh [path/to/wasmcart-run]
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="${1:-$HERE/../build/wasmcart-run}"
OUT="${TMPDIR:-/tmp}/wasmcart-code-cache-test.$$"
mkdir -p "$OUT/cache"
cp "$HERE/snake.wasc" "$OUT/snake.wasc"  # its .sav is written beside it
fail=0
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy WASMCART_NO_WGPU=1 XDG_CACHE_HOME="$OUT/cache"
export WASMCART_V8_FLAGS="--wasm-caching-threshold=1"
check() {  # check WHAT GOT WANT
  if [ "$2" = "$3" ]; then echo "  ok    $1"; else echo "*** FAIL $1: got '$2', want '$3'"; fail=1; fi
}
# run NAME SECONDS: snake for a while, then stopped
run() {
  "$RUN" "$OUT/snake.wasc" --uncapped > "$OUT/$1.log" 2>&1 &
  pid=$!
  sleep "$2"
  kill -INT $pid 2>/dev/null
  wait $pid
}
files() { ls "$OUT/cache/wasmcart/code"/*.v8code 2>/dev/null | wc -l | tr -d ' '; }

run first 4
check "first run: compiled without a cache"          "$(grep -c 'no code cache yet' "$OUT/first.log")" "1"
check "first run: one cache file written"            "$(files)" "1"
run second 2
check "second run: started from the cache"           "$(grep -c 'from the code cache' "$OUT/second.log")" "1"
check "second run: the cart loaded"                  "$(grep -c 'loaded snake' "$OUT/second.log")" "1"
f=$(ls "$OUT/cache/wasmcart/code"/*.v8code)
check "the file is named by the wasm's SHA-256"      "$(basename "$f" .v8code | grep -cE '^[0-9a-f]{64}$')" "1"
# keep the header (magic + hash), garble what V8 would deserialize
head -c 72 "$f" > "$OUT/garbled" && head -c 4096 /dev/urandom >> "$OUT/garbled" && cp "$OUT/garbled" "$f"
run garbled 2
check "a garbled cache: recompiled, not used"        "$(grep -c 'did not fit this V8: recompiled' "$OUT/garbled.log")" "1"
check "a garbled cache: the cart still loads"        "$(grep -c 'loaded snake' "$OUT/garbled.log")" "1"
rm -rf "$OUT"
[ $fail -eq 0 ] && echo "all code cache checks passed"
exit $fail
