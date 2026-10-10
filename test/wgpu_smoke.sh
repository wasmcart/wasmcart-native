#!/bin/sh
# WebGPU smoke for release builds on runners that may have no WebGPU adapter
# (test/wgpu_test.sh is the full suite and needs one).
#
# Runs test/wgpu/wgpucart.wasc headless. Passes when the player loaded its
# wgpu/ support (bridge, wasmcart's JS, dawn.node and Dawn's library) and
# either rendered the cart's triangle (an adapter exists) or reported that no
# adapter is available. Fails when WebGPU support did not load, or the cart was
# refused or rendered wrong.
#
# Run: sh test/wgpu_smoke.sh [path/to/wasmcart-run]
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="${1:-$HERE/../build/wasmcart-run}"
OUT="${TMPDIR:-/tmp}/wasmcart-wgpu-smoke.$$"
mkdir -p "$OUT"
# WGPU_SMOKE_VIDEODRIVER picks another SDL video driver: SDL's Windows build
# has no offscreen driver, so Windows runs it on a real window (windows).
export SDL_VIDEODRIVER="${WGPU_SMOKE_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy

"$RUN" "$HERE/wgpu/wgpucart.wasc" --fixed-step 16 --shot 8 "$OUT/shot.ppm" > "$OUT/log" 2>&1 &
pid=$!
i=0
while [ $i -lt 240 ] && kill -0 $pid 2>/dev/null && [ ! -f "$OUT/shot.ppm" ]; do sleep 0.25; i=$((i + 1)); done
sleep 0.3
kill -INT $pid 2>/dev/null
wait $pid
status=$?

result=1
if grep -q "wasmcart-run: WebGPU on " "$OUT/log"; then
  px=$(node -e '
    const b = require("fs").readFileSync(process.argv[1]); let o = 0, t = [];
    while (t.length < 4) { let e = o; while (b[e] > 32) e++; t.push(b.slice(o, e).toString()); o = e + 1; }
    const w = +t[1], i = o + (110 * w + 128) * 3;
    console.log([...b.slice(i, i + 3)].join(","));
  ' "$OUT/shot.ppm" 2>/dev/null)
  if [ "$px" = "255,128,64" ] && [ "$status" = 0 ]; then
    echo "PASS: WebGPU support loaded, cart rendered ($(grep -o 'WebGPU on .*' "$OUT/log" | head -1))"
    result=0
  else
    echo "FAIL: WebGPU adapter found but the cart's frame is wrong (pixel '$px', want 255,128,64; exit $status)"
  fi
elif grep -q "no WebGPU adapter is available" "$OUT/log"; then
  echo "PASS (load only): WebGPU support loaded; this machine has no WebGPU adapter, so rendering is not checked"
  result=0
else
  echo "FAIL: WebGPU support did not load"
fi
[ $result = 0 ] || cat "$OUT/log"
rm -rf "$OUT"
exit $result
