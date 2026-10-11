#!/bin/sh
# Frame pacing without vsync (src/main.c): no window surface (SDL's offscreen
# driver) means nothing waits for the display, so the player sleeps to a 60 Hz
# deadline that does not drift. It used to sleep 16 ms minus the frame's time,
# 62.5 fps, and did the same with vsync on, capping a 120 or 144 Hz display.
#
# Run: sh test/pacing_test.sh [path/to/wasmcart-run]
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="${1:-$HERE/../build/wasmcart-run}"
DIR="${TMPDIR:-/tmp}/wasmcart-pacing-test.$$"
LOG="$DIR/run.log"
mkdir -p "$DIR"
cp "$HERE/snake.wasc" "$DIR/snake.wasc"  # its .sav is written beside it
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy WASMCART_NO_WGPU=1
"$RUN" "$DIR/snake.wasc" --fps > "$LOG" 2>&1 &
pid=$!
sleep 11
kill -INT $pid 2>/dev/null
wait $pid
# the second 5-second window (the first has the load in it)
fps=$(grep 'FPS:' "$LOG" | sed -n 2p | sed 's/.*FPS: //')
rm -rf "$DIR"
if [ -z "$fps" ]; then echo "*** FAIL no FPS line"; exit 1; fi
ok=$(node -e "console.log(+process.argv[1] >= 58.5 && +process.argv[1] <= 61 ? 1 : 0)" "$fps")
if [ "$ok" = 1 ]; then echo "  ok    paced to 60 Hz without vsync ($fps fps)"; echo "all pacing checks passed"; exit 0; fi
echo "*** FAIL paced at $fps fps, want 60 (58.5 to 61)"
exit 1
