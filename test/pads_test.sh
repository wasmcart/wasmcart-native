#!/bin/sh
# Four controllers reach a cart, each in its own pad slot, through the player's
# real controller path: SDL's joystick driver reading Linux input devices.
# test/vpad.c makes virtual controllers with uinput and test/padcart.wasc
# publishes what the host wrote into each wc_pad_t (--debug-dump, at several
# frames of one run):
#   1. two controllers present at start: pads 0 and 1, pads 2 and 3 empty.
#      SDL reports a controller present at start twice (the device list and a
#      queued SDL_CONTROLLERDEVICEADDED); the player opened both, so pad 0
#      showed up again as pad 2, and pad 1 as pad 3.
#   2. two more plugged in while it runs: pads 2 and 3, each its own buttons.
#   3. pads 1 and 2 unplugged: empty, and pad 3 stays pad 3.
#   4. a controller plugged in again: the first free slot, pad 1.
#
# Linux only. Needs write access to /dev/uinput and read access to the event
# devices it makes (a desktop session's uaccess gives both; CI sets them up).
#
# Run: sh test/pads_test.sh [path/to/wasmcart-run]
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="${1:-$HERE/../build/wasmcart-run}"
DIR="${TMPDIR:-/tmp}/wasmcart-pads-test.$$"
mkdir -p "$DIR"
fail=0
cleanup() { exec 3>&- 2>/dev/null; [ -n "$pid" ] && kill -KILL "$pid" 2>/dev/null; rm -rf "$DIR"; }
trap cleanup EXIT
if [ ! -w /dev/uinput ]; then echo "*** FAIL /dev/uinput is not writable (virtual controllers need it)"; exit 1; fi
${CC:-cc} -O2 -o "$DIR/vpad" "$HERE/vpad.c" || { echo "*** FAIL vpad did not build"; exit 1; }
cp "$HERE/padcart.wasc" "$DIR/padcart.wasc"
mkfifo "$DIR/vpad.in"
"$DIR/vpad" < "$DIR/vpad.in" > "$DIR/vpad.log" 2>&1 &
exec 3> "$DIR/vpad.in"
send() { printf '%s\n' "$@" >&3; }
# only the virtual controllers: a pad plugged into the machine must not join
export SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x1209/0x7764
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy WASMCART_NO_WGPU=1
send "add 0" "wait 300" "add 1" "wait 500" "btn 0 south 1" "btn 1 east 1" "abs 1 lx 16384"
sleep 1
"$RUN" "$DIR/padcart.wasc" --debug-dump 60 "$DIR/d1.json" --debug-dump 240 "$DIR/d2.json" \
  --debug-dump 420 "$DIR/d3.json" --debug-dump 600 "$DIR/d4.json" > "$DIR/run.log" 2>&1 &
pid=$!
wait_for() { # file: up to 20 s
  n=0
  while [ ! -s "$1" ] && [ $n -lt 200 ]; do sleep 0.1; n=$((n + 1)); done
  [ -s "$1" ]
}
check() { # name, dump, field, expected JSON array
  got=$(node -e "const s=JSON.parse(require('fs').readFileSync(process.argv[1],'utf8'));console.log(JSON.stringify(s[process.argv[2]]))" "$2" "$3" 2>/dev/null)
  if [ "$got" = "$4" ]; then echo "  ok    $1: $3 $got"
  else echo "*** FAIL $1: $3 is ${got:-missing}, want $4"; fail=1; fi
}
step() { # name, dump; then field expected pairs
  name=$1 dump=$2; shift 2
  if ! wait_for "$dump"; then echo "*** FAIL $name: no dump (the run ended?)"; tail -5 "$DIR/run.log"; fail=1; return; fi
  while [ $# -ge 2 ]; do check "$name" "$dump" "$1" "$2"; shift 2; done
}
step "two controllers at start" "$DIR/d1.json" pad.connected "[1,1,0,0]" pad.buttons "[1,2,0,0]" pad.lx "[0,16384,0,0]"
# one at a time: two plugged in together reach SDL in either order
send "add 2" "wait 300" "add 3" "wait 300" "btn 2 west 1" "btn 3 north 1"
step "two more plugged in" "$DIR/d2.json" pad.connected "[1,1,1,1]" pad.buttons "[1,2,4,8]"
send "del 1" "del 2"
step "pads 1 and 2 unplugged" "$DIR/d3.json" pad.connected "[1,0,0,1]" pad.buttons "[1,0,0,8]"
send "add 4" "wait 300" "btn 4 start 1"
step "one plugged in again" "$DIR/d4.json" pad.connected "[1,1,0,1]" pad.buttons "[1,64,0,8]"
if grep -q "^error" "$DIR/vpad.log"; then echo "*** FAIL vpad: $(grep '^error' "$DIR/vpad.log" | head -1)"; fail=1; fi
[ "$fail" = 0 ] && echo "all pad checks passed" || { echo "FAILED"; grep -i controller "$DIR/run.log" | sed 's/^/    /'; }
exit $fail
