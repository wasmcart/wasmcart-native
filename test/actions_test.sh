#!/bin/sh
# Input actions (wasmcart SPEC.md, "Input actions") through the native player.
# test/actioncart.wasc is wasmcart's test/fixtures/actioncart.c: it declares
# sets "menu" and "driving", actions Confirm/Pedal (A) and Steer (left stick),
# and prints "Press [..] to Pedal, [..] to Steer" when wc_input_revision moves.
# Controllers are hidden, so player 0 is on the keyboard: Pedal is on Z (the
# player's key map), Steer is unbound there. The must-fail controls: an
# unknown kind is refused, and Pedal is not "unknown" (-1).
#
# Run: sh test/actions_test.sh <path to wasmcart-run>
RUN="${1:-build/wasmcart-run}"
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/../out/actions-test"
mkdir -p "$OUT"
fail=0
rm -f "$OUT/dump.json"
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 \
  timeout -s KILL 30 "$RUN" "$DIR/actioncart.wasc" --fixed-step 16.6667 \
  --debug-dump 3 "$OUT/dump.json" > "$OUT/run.log" 2>&1 &
pid=$!
for _ in $(seq 1 200); do [ -s "$OUT/dump.json" ] && break; sleep 0.05; done
kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
check() { # $1 = label, $2 = file, $3 = expected substring
  if grep -q "$3" "$2"; then echo "  ok    $1"; else echo "*** FAIL $1 (wanted $3 in $2)"; fail=1; fi
}
check "declared: sets 0/1, actions 0-2, bad kind refused, redeclare = same id" "$OUT/dump.json" \
  '"set_menu": 0, "set_drive": 1, "act_confirm": 0, "act_pedal": 1, "act_steer": 2, "bad_kind": -1, "dup_pedal": 1'
check "keyboard: Pedal on Z (HID 0x1d), Steer unbound, device keyboard" "$OUT/dump.json" \
  '"pedal_len": 1, "steer_len": 0, "pedal_glyph": 1309, "device": 1'
check "the cart printed its prompt" "$OUT/run.log" 'Press \[Z\] to Pedal, \[\] to Steer'
exit $fail
