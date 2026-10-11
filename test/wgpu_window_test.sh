#!/bin/sh
# A WebGPU cart in a real window (X11; run it under xvfb-run on a machine
# without a display): when the window is the cart's size, the cart draws
# straight into the window's texture (no copy, wgpu_bridge.cjs present()), and
# the frame it draws there is the frame it draws offscreen.
#
# Run: xvfb-run -a -s "-screen 0 640x480x24" sh test/wgpu_window_test.sh [path/to/wasmcart-run]
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="${1:-$HERE/../build/wasmcart-run}"
OUT="${TMPDIR:-/tmp}/wasmcart-wgpu-window-test.$$"
mkdir -p "$OUT"
fail=0
export SDL_AUDIODRIVER=dummy WASMCART_WGPU_VALIDATION=1
if [ -z "${DISPLAY:-}" ]; then echo "*** FAIL no DISPLAY: run it under xvfb-run"; exit 1; fi
SDL_VIDEODRIVER=x11 timeout 120 "$RUN" "$HERE/wgpu/wgpucart.wasc" --fixed-step 16 --shot 8 "$OUT/window.ppm" > "$OUT/window.log" 2>&1 &
pid=$!; i=0
while [ $i -lt 240 ] && kill -0 $pid 2>/dev/null && [ ! -f "$OUT/window.ppm" ]; do sleep 0.25; i=$((i + 1)); done
sleep 0.3; kill -INT $pid 2>/dev/null; wait $pid
SDL_VIDEODRIVER=offscreen timeout 120 "$RUN" "$HERE/wgpu/wgpucart.wasc" --fixed-step 16 --shot 8 "$OUT/offscreen.ppm" > "$OUT/offscreen.log" 2>&1 &
pid=$!; i=0
while [ $i -lt 240 ] && kill -0 $pid 2>/dev/null && [ ! -f "$OUT/offscreen.ppm" ]; do sleep 0.25; i=$((i + 1)); done
sleep 0.3; kill -INT $pid 2>/dev/null; wait $pid
grep -q "drawn straight into the 256x" "$OUT/window.log" \
  && echo "  ok    a window the cart's size gets the cart's frames directly" \
  || { echo "*** FAIL direct presentation did not start"; tail -5 "$OUT/window.log"; fail=1; }
if [ -f "$OUT/window.ppm" ] && cmp -s "$OUT/window.ppm" "$OUT/offscreen.ppm"; then
  echo "  ok    the frame drawn into the window is the offscreen frame"
else echo "*** FAIL the window's frame differs from the offscreen one"; fail=1; fi
grep -q "WebGPU validation error\|\\[error\\] WebGPU" "$OUT/window.log" && { echo "*** FAIL WebGPU errors in the window run"; fail=1; }
rm -rf "$OUT"
[ $fail -eq 0 ] && echo "all wgpu window checks passed"
exit $fail
