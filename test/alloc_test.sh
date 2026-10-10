#!/bin/sh
# Cart memory the host writes (wasmcart SPEC.md): glGetString, glGetStringi
# and glMapBufferRange land in blocks the CART allocates with its wc_alloc,
# a cart that never reaches such a path needs no allocator, one that does gets
# a clear error instead of a silent write into its memory, and every pointer
# the cart returns is checked before the host writes through it.
#
# Fixtures: test/alloc/*.wasm, built from test/alloc/alloccart.c by
# test/alloc/build.sh (which says what each one exports and calls). GL carts
# run headless on SDL's offscreen driver; they clear green when every check
# the cart made passed and red otherwise.
#
# Run: sh test/alloc_test.sh [path/to/wasmcart-run]
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="${1:-$HERE/../build/wasmcart-run}"
FX="$HERE/alloc"
OUT="$(dirname "$RUN")/alloc-test"
mkdir -p "$OUT"
fail=0
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy

# run CART NAME: wasmcart-run with --shot 5, stopped by SIGINT once the shot
# exists or the player exits on its own (a trapped cart). Leaves NAME.log,
# NAME.ppm (if any) and NAME.status in $OUT.
run() {
  cart=$1; name=$2
  rm -f "$OUT/$name.ppm"
  "$RUN" "$FX/$cart.wasm" --fixed-step 16 --shot 5 "$OUT/$name.ppm" > "$OUT/$name.log" 2>&1 &
  pid=$!
  i=0
  while [ $i -lt 80 ] && kill -0 $pid 2>/dev/null && [ ! -f "$OUT/$name.ppm" ]; do sleep 0.25; i=$((i + 1)); done
  sleep 0.3
  kill -INT $pid 2>/dev/null
  i=0
  while [ $i -lt 40 ] && kill -0 $pid 2>/dev/null; do sleep 0.25; i=$((i + 1)); done
  kill -KILL $pid 2>/dev/null
  wait $pid
  echo $? > "$OUT/$name.status"
}

# pixel NAME X Y -> "r,g,b" from a binary PPM (P6, maxval 255)
pixel() {
  [ -f "$OUT/$1.ppm" ] || { echo none; return; }
  node -e '
    const b = require("fs").readFileSync(process.argv[1]); let o = 0, t = [];
    while (t.length < 4) { let e = o; while (b[e] > 32) e++; t.push(b.slice(o, e).toString()); o = e + 1; }
    const w = +t[1], x = +process.argv[2], y = +process.argv[3];
    console.log([...b.slice(o + (y * w + x) * 3, o + (y * w + x) * 3 + 3)].join(","));
  ' "$OUT/$1.ppm" "$2" "$3"
}

ok()   { echo "  ok    $1"; }
bad()  { echo "*** FAIL $1"; fail=1; }
# has NAME TEXT DESC: the log contains TEXT (fixed string)
has()  { if grep -qF -- "$2" "$OUT/$1.log"; then ok "$1: $3"; else bad "$1: $3 (log: $OUT/$1.log)"; fi; }
hasre(){ if grep -qE -- "$2" "$OUT/$1.log"; then ok "$1: $3"; else bad "$1: $3 (log: $OUT/$1.log)"; fi; }
lacks(){ if grep -qF -- "$2" "$OUT/$1.log"; then bad "$1: $3 (log: $OUT/$1.log)"; else ok "$1: $3"; fi; }
# no crash: SIGINT (130) or a clean exit, never a signal death like SIGSEGV
clean(){ s=$(cat "$OUT/$1.status"); if [ "$s" -lt 128 ] || [ "$s" = 130 ]; then ok "$1: exits without crashing ($s)"; else bad "$1: died with status $s"; fi; }
green(){ p=$(pixel "$1" 32 32); if [ "$p" = "0,255,0" ]; then ok "$1: renders green"; else bad "$1: center pixel $p, want 0,255,0"; fi; }

MISSING="must write into the cart's memory, but the cart does not export wc_alloc/wc_free (its malloc, if any, is not used). Export wc_alloc(size, align) and wc_free(ptr): wasmcart.h defines them for C/C++ (include it), wasmcart's CMake helper exports them, and Rust carts get them from the wasmcart-alloc crate. See SPEC.md, \"Cart memory the host writes\"."

echo "wc_alloc/wc_free: GL strings and mappings in the cart's own blocks"
run wcalloc_gl wcalloc_gl
has   wcalloc_gl 'alloccart: version="OpenGL ES 3.0 wasmcart" cached=yes alloc_calls=1' "glGetString through wc_alloc, once per string"
hasre wcalloc_gl 'alloccart: ext0=GL_[A-Za-z0-9_]+ alloc_calls=2$' "glGetStringi through wc_alloc"
has   wcalloc_gl 'alloccart: map read=ok write=ok aligned16=yes map_allocs=2 map_frees=2' "glMapBufferRange: wc_alloc at map, copied back and wc_free at unmap"
has   wcalloc_gl 'alloccart: done ok=1' "cart's own checks pass"
lacks wcalloc_gl 'deprecated' "no deprecation warning"
green wcalloc_gl
clean wcalloc_gl

# blue NAME X Y / grey NAME X Y: the pixel is the quad's blue or the clear's grey
blue() { p=$(pixel "$1" $2 $3); if [ "$p" = "51,153,255" ]; then ok "$1: ($2,$3) is the quad's blue"; else bad "$1: pixel ($2,$3) $p, want 51,153,255"; fi; }
grey() { p=$(pixel "$1" $2 $3); if [ "$p" = "76,76,76" ]; then ok "$1: ($2,$3) is the clear grey"; else bad "$1: pixel ($2,$3) $p, want 76,76,76"; fi; }

echo "draw from a write-only, unsynchronized mapping of a buffer sub-range (Godot's 2D canvas)"
run mapdraw_gl mapdraw_gl
blue  mapdraw_gl 32 32
blue  mapdraw_gl 20 44
grey  mapdraw_gl 4 4
grey  mapdraw_gl 60 60
lacks mapdraw_gl 'GL error' "no GL errors"
clean mapdraw_gl

echo "no allocator, no path that needs one: loads and runs"
run noalloc_plain noalloc_plain
has   noalloc_plain 'alloccart: done ok=1' "runs"
lacks noalloc_plain 'allocator' "no allocator error"
green noalloc_plain
clean noalloc_plain

echo "no allocator, glGetString: a clear error, not a silent write"
run noalloc_getstr noalloc_getstr
has   noalloc_getstr "wasmcart: glGetString $MISSING" "error names glGetString and wc_alloc"
has   noalloc_getstr 'cart trapped' "the cart stops"
lacks noalloc_getstr 'alloccart: version=' "glGetString never returned to the cart"
clean noalloc_getstr

echo "no allocator, glMapBufferRange"
run noalloc_map noalloc_map
has   noalloc_map "wasmcart: glMapBufferRange $MISSING" "error names glMapBufferRange"
has   noalloc_map 'cart trapped' "the cart stops"
clean noalloc_map

# wasmcart 0.32 is a clean break: a cart that exports malloc (with or without
# free/memalign) but not wc_alloc/wc_free gets exactly the no-allocator error.
# Its malloc is never called and nothing warns about a deprecated path.
malloc_only() {  # malloc_only NAME CALL DESC (CALL: the first host write it reaches)
  echo "$3: the same error as no allocator, malloc never used"
  run $1 $1
  has   $1 "wasmcart: $2 $MISSING" "error names $2 and wc_alloc/wc_free"
  has   $1 'cart trapped' "the cart stops"
  lacks $1 'alloccart: version=' "glGetString never returned to the cart"
  lacks $1 'alloccart: map read=' "glMapBufferRange never returned to the cart"
  lacks $1 'malloc(' "no allocation through malloc"
  lacks $1 'deprecat' "no deprecation warning"
  clean $1
}
malloc_only malloc_gl         glGetString      "malloc/memalign/free only"
malloc_only mallocnofree_gl   glMapBufferRange "malloc, no free"
malloc_only mallocmisalign_gl glMapBufferRange "malloc only (8-byte aligned)"

echo "bad pointers from wc_alloc are refused before any write"
run badptr_gl badptr_gl
has   badptr_gl "wasmcart: glGetString: the cart's wc_alloc(23, 1) returned 0xfffffff0, which is outside its 131072-byte memory" "0xFFFFFFF0 refused"
lacks badptr_gl 'alloccart: version=' "the cart never got the pointer"
clean badptr_gl
run memsize_gl memsize_gl
has   memsize_gl "wasmcart: glGetString: the cart's wc_alloc(23, 1) returned 0x20000, which is outside its 131072-byte memory" "one-past-the-end refused"
clean memsize_gl
run misalign_gl misalign_gl
has   misalign_gl "wasmcart: glMapBufferRange: the cart's wc_alloc(16, 16) returned 0x" "misaligned pointer refused..."
has   misalign_gl ", which is not 16-byte aligned" "...as misaligned"
clean misalign_gl

[ $fail = 0 ] && echo "all alloc checks passed" || echo "FAILED"
exit $fail
