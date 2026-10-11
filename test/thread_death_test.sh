#!/bin/sh
# A cart thread that dies after it was spawned stops the cart, loudly
# (src/thread_worker_js.h, run_frame): it used to be logged by the worker while
# the cart's pool ran a worker short. test/threaddie.wasc is wasmcart's
# fixture (test/fixtures/threaddie.c): its thread traps at once.
#
# Run: sh test/thread_death_test.sh [path/to/wasmcart-run]
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="${1:-$HERE/../build/wasmcart-run}"
LOG="${TMPDIR:-/tmp}/wasmcart-thread-death-test.$$.log"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy WASMCART_NO_WGPU=1
timeout 30 "$RUN" "$HERE/threaddie.wasc" --fixed-step 16 > "$LOG" 2>&1
status=$?
fail=0
grep -q "cart thread [0-9]* died; the cart's thread pool is a worker short, so the cart is stopped" "$LOG" \
  && echo "  ok    the dead thread is named and the cart stopped" || { echo "*** FAIL no thread-death error"; fail=1; }
grep -q "cart trapped, exiting" "$LOG" \
  && echo "  ok    the player exits" || { echo "*** FAIL the player did not exit (status $status)"; fail=1; }
rm -f "$LOG"
[ $fail -eq 0 ] && echo "all thread death checks passed"
exit $fail
