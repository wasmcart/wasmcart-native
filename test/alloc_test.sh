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

MISSING="must write into the cart's memory, but the cart exports no allocator. Export wc_alloc(size, align) and wc_free(ptr): wasmcart.h defines them for C/C++ (include it), wasmcart's CMake helper exports them, and Rust carts get them from the wasmcart-alloc crate. See SPEC.md, \"Cart memory the host writes\"."
DEPRECATED="wasmcart: this cart has no wc_alloc/wc_free, so the host is allocating in it through its exported malloc/free. That fallback is deprecated and will be removed: rebuild against wasmcart 0.32's wasmcart.h (or export wc_alloc and wc_free yourself)."

echo "wc_alloc/wc_free: GL strings and mappings in the cart's own blocks"
run wcalloc_gl wcalloc_gl
has   wcalloc_gl 'alloccart: version="OpenGL ES 3.0 wasmcart" cached=yes alloc_calls=1' "glGetString through wc_alloc, once per string"
hasre wcalloc_gl 'alloccart: ext0=GL_[A-Za-z0-9_]+ alloc_calls=2$' "glGetStringi through wc_alloc"
has   wcalloc_gl 'alloccart: map read=ok write=ok aligned16=yes map_allocs=2 map_frees=2' "glMapBufferRange: wc_alloc at map, copied back and wc_free at unmap"
has   wcalloc_gl 'alloccart: done ok=1' "cart's own checks pass"
lacks wcalloc_gl 'deprecated' "no deprecation warning"
green wcalloc_gl
clean wcalloc_gl

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

echo "malloc/free only: the deprecated transition path"
run malloc_gl malloc_gl
has   malloc_gl 'alloccart: map read=ok write=ok aligned16=yes map_allocs=2 map_frees=2' "mapping through malloc/free"
has   malloc_gl 'alloccart: done ok=1' "cart's own checks pass"
n=$(grep -cF -- "$DEPRECATED" "$OUT/malloc_gl.log")
if [ "$n" = 1 ]; then ok "malloc_gl: deprecation warning printed exactly once"; else bad "malloc_gl: deprecation warning printed $n times, want 1"; fi
green malloc_gl
clean malloc_gl

echo "malloc, no free: never freed, the released mapping is reused"
run mallocnofree_gl mallocnofree_gl
has   mallocnofree_gl 'alloccart: map read=ok write=ok aligned16=yes map_allocs=1 map_frees=0' "second mapping reuses the first block"
green mallocnofree_gl
clean mallocnofree_gl

echo "malloc only, 8-byte aligned, and a 16-byte-aligned mapping: refused, naming malloc"
run mallocmisalign_gl mallocmisalign_gl
has   mallocmisalign_gl "wasmcart: glMapBufferRange: the cart's malloc(16, 16) returned 0x" "the deprecated path is bounds-checked too..."
has   mallocmisalign_gl ", which is not 16-byte aligned" "...as misaligned"
clean mallocmisalign_gl

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
