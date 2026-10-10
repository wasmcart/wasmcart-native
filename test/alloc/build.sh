#!/bin/sh
# Rebuild the alloc fixtures (test/alloc/*.wasm) from alloccart.c.
# Needs wasi-sdk's clang: WASI_SDK_PATH=<wasi-sdk> sh test/alloc/build.sh
# No libc is linked (-nostdlib), so the target only has to be wasm32.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${WASI_SDK_PATH:?set WASI_SDK_PATH to a wasi-sdk install}/bin/clang"
b() { # name defines...
  name=$1; shift
  "$CC" --target=wasm32 -nostdlib -O2 -Wall -Wno-unused-function "$@" -Wl,--no-entry \
    -o "$HERE/$name.wasm" "$HERE/alloccart.c"
}
b wcalloc_gl       -DALLOC=1 -DCALLS=7   # wc_alloc/wc_free; glGetString, glGetStringi, map/unmap
b mapdraw_gl       -DALLOC=1 -DCALLS=8   # draws vertices written through a WRITE|UNSYNCHRONIZED sub-range mapping
b noalloc_plain    -DALLOC=0 -DCALLS=0   # no allocator, never needs one
b noalloc_getstr   -DALLOC=0 -DCALLS=1   # no allocator, calls glGetString
b noalloc_map      -DALLOC=0 -DCALLS=2   # no allocator, calls glMapBufferRange
b malloc_gl        -DALLOC=2 -DCALLS=7   # malloc/memalign/free only: refused like no allocator
b mallocnofree_gl  -DALLOC=6 -DCALLS=2   # malloc, no free: refused like no allocator
b mallocmisalign_gl -DALLOC=7 -DCALLS=2  # malloc only: refused like no allocator
b badptr_gl        -DALLOC=3 -DCALLS=1   # wc_alloc returns 0xFFFFFFF0
b memsize_gl       -DALLOC=4 -DCALLS=1   # wc_alloc returns the memory size
b misalign_gl      -DALLOC=5 -DCALLS=2   # wc_alloc returns ptr % 16 == 1
b text_wcalloc     -DALLOC=1 -DTEXT=1    # 2D: wc_on_text / wc_peer_on_message
b text_malloc      -DALLOC=2 -DTEXT=1    # malloc only: refused like no allocator
b text_noalloc     -DALLOC=0 -DTEXT=1
