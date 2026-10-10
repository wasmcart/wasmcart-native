/*
 * alloccart -- the host-writes-cart-memory contract (wasmcart SPEC.md, "Cart
 * memory the host writes") on wasmcart-native. One source, built into several
 * fixtures that differ only in which allocator the cart exports and which host
 * paths it reaches; test/alloc_test.sh builds and runs them.
 *
 *   -DALLOC=0  no allocator at all
 *   -DALLOC=1  wc_alloc/wc_free (counted)
 *   -DALLOC=2  malloc/memalign/free only (the host must refuse it: no wc_alloc)
 *   -DALLOC=3  wc_alloc returns 0xFFFFFFF0 (outside memory)
 *   -DALLOC=4  wc_alloc returns the memory size (one past the end)
 *   -DALLOC=5  wc_alloc returns a pointer one byte off its alignment
 *   -DALLOC=6  malloc only (16-aligned), no free
 *   -DALLOC=7  malloc only, whose blocks are 8- but never 16-byte aligned
 *   -DCALLS=   bitmask: 1 glGetString(GL_VERSION), 2 glMapBufferRange/
 *              glUnmapBuffer, 4 glGetStringi(GL_EXTENSIONS, 0)
 *   -DTEXT=1   2D cart taking text input and peer messages (alloc_test.c)
 *
 * The cart reports through wc_log ("alloccart: ...") and, for the C harness,
 * counters at the start of its framebuffer. GL variants clear green when
 * every check passed and red otherwise.
 *
 * Self-contained: no libc, no wasmcart.h (the point is to hand-export exactly
 * what each case needs). Build: see test/alloc_test.sh.
 */
#include <stdint.h>

#ifndef ALLOC
#define ALLOC 1
#endif
#ifndef CALLS
#define CALLS 0
#endif
#ifndef TEXT
#define TEXT 0
#endif

#define EXPORT(n) __attribute__((export_name(n)))
#define IMPORT(m, n) __attribute__((import_module(m), import_name(n)))

IMPORT("env", "wc_log") extern void wc_log(const char* s, uint32_t len);
#if TEXT
IMPORT("env", "wc_text_input_begin") extern void wc_text_input_begin(void);
#else
IMPORT("gl", "glClearColor") extern void glClearColor(float r, float g, float b, float a);
IMPORT("gl", "glClear") extern void glClear(uint32_t mask);
#if CALLS & 1
IMPORT("gl", "glGetString") extern const char* glGetString(uint32_t name);
#endif
#if CALLS & 4
IMPORT("gl", "glGetStringi") extern const char* glGetStringi(uint32_t name, uint32_t i);
#endif
#if CALLS & 2
IMPORT("gl", "glGenBuffers") extern void glGenBuffers(int32_t n, uint32_t* out);
IMPORT("gl", "glBindBuffer") extern void glBindBuffer(uint32_t target, uint32_t buf);
IMPORT("gl", "glBufferData") extern void glBufferData(uint32_t target, int32_t size, const void* data, uint32_t usage);
IMPORT("gl", "glMapBufferRange") extern void* glMapBufferRange(uint32_t target, int32_t off, int32_t len, uint32_t access);
IMPORT("gl", "glUnmapBuffer") extern uint32_t glUnmapBuffer(uint32_t target);
#endif
#endif

enum { W = 64, H = 64, AUDIO_CAP = 512 };
static uint32_t fb[W * H];
static float    audio[AUDIO_CAP * 2];
static uint32_t audio_write;
static uint8_t  input[4 * 16];
static double   time_ms;
static uint8_t  host_info[128];
static uint32_t info[20];

/* ---- a tiny bump allocator with a free list of one size class ---------- */
extern unsigned char __heap_base;
static uint32_t heap_top;
static uint32_t alloc_calls, free_calls;

static uint32_t bump(uint32_t size, uint32_t align) {
  if (!heap_top) heap_top = (uint32_t)(uintptr_t)&__heap_base;
  uint32_t p = (heap_top + align - 1) & ~(align - 1);
  uint32_t end = p + size;
  uint32_t have = (uint32_t)__builtin_wasm_memory_size(0) * 65536u;
  if (end > have) {
    if (__builtin_wasm_memory_grow(0, (end - have + 65535) / 65536) == (__SIZE_TYPE__)-1) return 0;
  }
  heap_top = end;
  return p;
}

#if ALLOC == 1 || ALLOC == 3 || ALLOC == 4 || ALLOC == 5
EXPORT("wc_alloc") uint32_t wc_alloc(uint32_t size, uint32_t align) {
  alloc_calls++;
#if ALLOC == 3
  return 0xFFFFFFF0u;
#elif ALLOC == 4
  return (uint32_t)__builtin_wasm_memory_size(0) * 65536u;
#elif ALLOC == 5
  return bump(size + 64, 64) + 1;   /* misaligned for any align > 1 */
#else
  return bump(size, align);
#endif
}
EXPORT("wc_free") void wc_free(uint32_t ptr) { (void)ptr; free_calls++; }
#endif

#if ALLOC == 2
EXPORT("malloc") void* malloc(unsigned long size) { alloc_calls++; return (void*)(uintptr_t)bump((uint32_t)size, 8); }
EXPORT("memalign") void* memalign(unsigned long align, unsigned long size) { alloc_calls++; return (void*)(uintptr_t)bump((uint32_t)size, (uint32_t)align); }
EXPORT("free") void free(void* p) { (void)p; free_calls++; }
#elif ALLOC == 6
EXPORT("malloc") void* malloc(unsigned long size) { alloc_calls++; return (void*)(uintptr_t)bump((uint32_t)size, 16); }
#elif ALLOC == 7
EXPORT("malloc") void* malloc(unsigned long size) { alloc_calls++; return (void*)(uintptr_t)(bump((uint32_t)size + 8, 16) + 8); }
#endif

/* ---- logging without libc ----------------------------------------------- */
static char logbuf[256];
static uint32_t loglen;
static void lput(const char* s) { while (*s && loglen < sizeof logbuf) logbuf[loglen++] = *s++; }
static void lnum(uint32_t v) {
  char t[12]; int n = 0;
  do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n && loglen < sizeof logbuf) logbuf[loglen++] = t[--n];
}
static void lflush(void) { wc_log(logbuf, loglen); loglen = 0; }

EXPORT("wc_get_info") uint32_t* wc_get_info(void) {
  info[0] = 4; info[1] = W; info[2] = H;
  info[3] = (uint32_t)(uintptr_t)fb;
  info[4] = (uint32_t)(uintptr_t)audio; info[5] = AUDIO_CAP;
  info[6] = (uint32_t)(uintptr_t)&audio_write;
  info[7] = (uint32_t)(uintptr_t)input;
  info[10] = (uint32_t)(uintptr_t)&time_ms;
  info[11] = (uint32_t)(uintptr_t)host_info;
  info[16] = TEXT ? 0 : 1;   /* gpu_api */
  return info;
}

EXPORT("wc_init") void wc_init(void) {}

#if TEXT
/* fb[0] alloc calls, fb[1] free calls, fb[2] wc_on_text calls,
 * fb[3] text bytes, fb[4] peer messages, fb[5] peer bytes, fb[6] last ptr,
 * fb[8..] (byte 32) the text received, fb[40..] (byte 160) the peer bytes. */
EXPORT("wc_on_text") void wc_on_text(const char* s, uint32_t len) {
  fb[2]++;
  fb[6] = (uint32_t)(uintptr_t)s;
  for (uint32_t i = 0; i < len && fb[3] < 120; i++) ((char*)&fb[8])[fb[3]++] = s[i];
}
EXPORT("wc_peer_on_message") void wc_peer_on_message(int32_t id, const void* d, uint32_t len) {
  (void)id;
  fb[4]++;
  fb[6] = (uint32_t)(uintptr_t)d;
  for (uint32_t i = 0; i < len && fb[5] < 120; i++) ((char*)&fb[40])[fb[5]++] = ((const char*)d)[i];
}
EXPORT("wc_render") void wc_render(void) {
  static int started;
  if (!started) { started = 1; wc_text_input_begin(); }
  fb[0] = alloc_calls;
  fb[1] = free_calls;
}
#else
static int frame, ok = 1;

EXPORT("wc_render") void wc_render(void) {
  if (frame++ == 2) {
#if CALLS & 1
    const char* v = glGetString(0x1F02);   /* GL_VERSION */
    const char* v2 = glGetString(0x1F02);
    lput("alloccart: version=\""); lput(v ? v : "(null)"); lput("\"");
    lput(" cached="); lput(v == v2 ? "yes" : "no");
    if (!v || v != v2) ok = 0;
    lput(" alloc_calls="); lnum(alloc_calls); lput(" free_calls="); lnum(free_calls);
    lflush();
#endif
#if CALLS & 4
    const char* e = glGetStringi(0x1F03, 0);   /* GL_EXTENSIONS */
    lput("alloccart: ext0="); lput(e ? e : "(null)"); lput(" alloc_calls="); lnum(alloc_calls);
    if (!e) ok = 0;
    lflush();
#endif
#if CALLS & 2
    {
      static uint8_t init[16];
      for (int i = 0; i < 16; i++) init[i] = (uint8_t)(i + 1);
      uint32_t buf = 0;
      glGenBuffers(1, &buf);
      glBindBuffer(0x8892, buf);                     /* GL_ARRAY_BUFFER */
      glBufferData(0x8892, 16, init, 0x88E8);        /* GL_DYNAMIC_DRAW */
      uint32_t a0 = alloc_calls, f0 = free_calls;
      uint8_t* m = (uint8_t*)glMapBufferRange(0x8892, 0, 16, 1 | 2);   /* READ|WRITE */
      int read_ok = m != 0, write_ok = 0, aligned = ((uintptr_t)m % 16) == 0;
      if (m) {
        for (int i = 0; i < 16; i++) if (m[i] != i + 1) read_ok = 0;
        for (int i = 0; i < 16; i++) m[i] = (uint8_t)(0xA0 + i);
        glUnmapBuffer(0x8892);
        uint8_t* r = (uint8_t*)glMapBufferRange(0x8892, 0, 16, 1);      /* READ */
        if (r) {
          write_ok = 1;
          for (int i = 0; i < 16; i++) if (r[i] != 0xA0 + i) write_ok = 0;
          glUnmapBuffer(0x8892);
        }
      }
      lput("alloccart: map read="); lput(read_ok ? "ok" : "BAD");
      lput(" write="); lput(write_ok ? "ok" : "BAD");
      lput(" aligned16="); lput(aligned ? "yes" : "no");
      lput(" map_allocs="); lnum(alloc_calls - a0);
      lput(" map_frees="); lnum(free_calls - f0);
      if (!read_ok || !write_ok || !aligned) ok = 0;
      lflush();
    }
#endif
    lput("alloccart: done ok="); lnum(ok); lput(" alloc_calls="); lnum(alloc_calls);
    lput(" free_calls="); lnum(free_calls);
    lflush();
  }
  if (ok) glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
  else glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  glClear(0x4000);
}
#endif
