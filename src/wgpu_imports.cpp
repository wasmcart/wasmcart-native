// wgpu_imports.cpp — a WebGPU cart's hot imports as direct calls into Dawn.
//
// A WebGPU cart's imports are wasmcart's emdawnwebgpu glue (JavaScript, run by
// wgpu_bridge.cjs) over Dawn's Node binding: every draw went wasm -> the glue
// -> the binding's argument conversion -> Dawn, about 4x the main-thread CPU
// of the same frame on GL, whose imports are C++ callbacks (gl_imports.cpp).
// The calls a frame makes most (a render or compute pass's state and draws,
// and the queue's buffer writes) are V8 C++ callbacks here that call Dawn's
// C API (webgpu.h) on the objects the glue made, the way GL is done.
//
// The glue still makes every object: it reports each handle the cart gets
// (its pointer in cart memory) with the object (wasmcart's createWgpuSession
// onObject / onRelease), and the bridge asks dawn.node for the object's
// native handle (native-dawn's objectHandle). This file keeps the cart
// pointer -> native handle table per host. A call naming a handle that is not
// in the table (none should be) goes to the glue's function instead, so the
// result is the same either way.
//
// Dawn's C API is the shared library dawn.node itself links (libwebgpu_dawn,
// in the wgpu/ directory): resolved from the already loaded library at run
// time, so a build without WebGPU support needs nothing of it.

#include "v8.h"
#include "v8-fast-api-calls.h"
#include <utility>
extern "C" {
#include "cart_host.h"
}
#include "wc_log.h"
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

extern v8::Isolate* g_isolate;
extern v8::Local<v8::Context> ctx();
extern "C" void wc_refresh_memory(wc_host_t* host);
// WebGPU calls served by Dawn directly, every host's (the performance log)
uint64_t wc_wgpu_native_calls_total = 0;

#ifdef WASMCART_WGPU_NATIVE
#include <webgpu/webgpu.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static void* g_dawn_lib = nullptr;  // the library dawn.node loaded

namespace {

// ── Dawn's C API, from the loaded library ─────────────────────────────────

#define WC_WGPU_PROCS(X) \
    X(wgpuRenderPassEncoderSetPipeline) \
    X(wgpuRenderPassEncoderSetBindGroup) \
    X(wgpuRenderPassEncoderSetVertexBuffer) \
    X(wgpuRenderPassEncoderSetIndexBuffer) \
    X(wgpuRenderPassEncoderDraw) \
    X(wgpuRenderPassEncoderDrawIndexed) \
    X(wgpuRenderPassEncoderDrawIndirect) \
    X(wgpuRenderPassEncoderDrawIndexedIndirect) \
    X(wgpuRenderPassEncoderSetViewport) \
    X(wgpuRenderPassEncoderSetScissorRect) \
    X(wgpuRenderPassEncoderSetStencilReference) \
    X(wgpuRenderPassEncoderSetBlendConstant) \
    X(wgpuRenderPassEncoderBeginOcclusionQuery) \
    X(wgpuRenderPassEncoderEndOcclusionQuery) \
    X(wgpuRenderPassEncoderExecuteBundles) \
    X(wgpuRenderPassEncoderEnd) \
    X(wgpuComputePassEncoderSetPipeline) \
    X(wgpuComputePassEncoderSetBindGroup) \
    X(wgpuComputePassEncoderDispatchWorkgroups) \
    X(wgpuComputePassEncoderDispatchWorkgroupsIndirect) \
    X(wgpuComputePassEncoderEnd) \
    X(wgpuQueueWriteBuffer) \
    X(wgpuCommandEncoderBeginRenderPass) \
    X(wgpuRenderPassEncoderRelease)

struct Procs {
#define X(name) decltype(&::name) name = nullptr;
    WC_WGPU_PROCS(X)
#undef X
};
Procs P;
bool g_procs_ok = false;

// The library dawn.node loaded (this resolves from it, never loads another).
bool resolve_procs(const std::string& dir) {
#if defined(_WIN32)
    (void)dir;
    HMODULE lib = GetModuleHandleA("webgpu_dawn.dll");
    if (!lib) return false;
    g_dawn_lib = (void*)lib;
    auto sym = [&](const char* n) { return (void*)GetProcAddress(lib, n); };
#else
#if defined(__APPLE__)
    std::string path = dir + "/libwebgpu_dawn.dylib";
#else
    std::string path = dir + "/libwebgpu_dawn.so";
#endif
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (!lib) return false;
    g_dawn_lib = lib;
    auto sym = [&](const char* n) { return dlsym(lib, n); };
#endif
    bool ok = true;
#define X(name) P.name = (decltype(P.name))sym(#name); if (!P.name) { wc_log("wasmcart: Dawn has no %s\n", #name); ok = false; }
    WC_WGPU_PROCS(X)
#undef X
    return ok;
}

// ── the cart pointer -> native handle table, per host ─────────────────────

// Open addressing with linear probing over cart pointers (never 0): a lookup
// is a multiply and, nearly always, one probe, where std::unordered_map
// chased a node per lookup (the direct calls make two or three a call).
// Deletion shifts the run back, so there are no tombstones.
class HandleMap {
    std::vector<uint32_t> keys_;
    std::vector<void*> vals_;
    uint32_t mask_ = 0, count_ = 0;
    int shift_ = 32;
    uint32_t slot(uint32_t k) const { return (uint32_t)((k * 2654435769u) >> shift_) & mask_; }
    void grow() {
        std::vector<uint32_t> ok = std::move(keys_);
        std::vector<void*> ov = std::move(vals_);
        uint32_t cap = ok.empty() ? 1024 : (uint32_t)ok.size() * 2;
        keys_.assign(cap, 0);
        vals_.assign(cap, nullptr);
        mask_ = cap - 1;
        int bits = 0;
        while ((1u << bits) < cap) bits++;
        shift_ = 32 - bits;
        count_ = 0;
        for (size_t i = 0; i < ok.size(); i++) if (ok[i]) set(ok[i], ov[i]);
    }
public:
    template <class F> void each(F f) const { for (size_t i = 0; i < keys_.size(); i++) if (keys_[i]) f(keys_[i], vals_[i]); }
    void* get(uint32_t k) const {
        if (!count_) return nullptr;
        for (uint32_t i = slot(k);; i = (i + 1) & mask_) {
            uint32_t c = keys_[i];
            if (c == k) return vals_[i];
            if (!c) return nullptr;
        }
    }
    void set(uint32_t k, void* v) {
        if ((count_ + 1) * 4 >= (mask_ + 1) * 3 || keys_.empty()) grow();
        for (uint32_t i = slot(k);; i = (i + 1) & mask_) {
            if (keys_[i] == k) { vals_[i] = v; return; }
            if (!keys_[i]) { keys_[i] = k; vals_[i] = v; count_++; return; }
        }
    }
    void erase(uint32_t k) {
        if (!count_) return;
        uint32_t i = slot(k);
        while (keys_[i] != k) { if (!keys_[i]) return; i = (i + 1) & mask_; }
        // shift the following entries of the run back over the hole
        for (uint32_t j = i;;) {
            keys_[i] = 0;
            vals_[i] = nullptr;
            for (;;) {
                j = (j + 1) & mask_;
                if (!keys_[j]) { count_--; return; }
                uint32_t h = slot(keys_[j]);
                // keys_[j] may move to i unless its home lies cyclically in (i, j]
                bool stays = i <= j ? (i < h && h <= j) : (i < h || h <= j);
                if (!stays) break;
            }
            keys_[i] = keys_[j];
            vals_[i] = vals_[j];
            i = j;
        }
    }
};

struct Session;
struct CallData { Session* s; int index; };
struct Session {
    wc_host_t* host;
    HandleMap handles;
    // the glue's function for each native import, called when a handle is missing
    std::vector<v8::Global<v8::Function>> fallback;
    std::vector<CallData> calls;  // each native import's callback data
    uint64_t native_calls = 0, fallback_calls = 0;
    // a fast call that named an object with no native handle: the first one's
    // import, and how many (the cart is stopped after the frame)
    const char* failed = nullptr;
    uint64_t failed_calls = 0;
    // wgpuCommandEncoderBeginRenderPass served here (begin_pass_cb): the
    // cart's emwgpuCreateRenderPassEncoder export, which makes the pass's
    // handle in cart memory, the glue's function for a descriptor this does
    // not take, and the passes made here (their reference is released when
    // the cart releases the handle)
    v8::Global<v8::Function> create_pass, begin_pass_glue;
    HandleMap owned;
    CallData begin_call{ nullptr, -1 };
};
std::unordered_map<wc_host_t*, Session*> g_sessions;

inline void* H(Session* s, uint32_t ptr) {
    if (!ptr) return nullptr;
    return s->handles.get(ptr);
}

// ── arguments ─────────────────────────────────────────────────────────────
// A native import has two entry points sharing one body, as the GL imports
// do (gl_imports.cpp): a V8 callback (wasm i32 -> Number, i64 -> BigInt,
// f32 -> Number), and a V8 fast API function in the import's C types, which
// the cart's wasm calls directly when the signatures match.

struct SlowArgs {
    const v8::FunctionCallbackInfo<v8::Value>& a;
    uint32_t u32(int i) const {
        v8::Local<v8::Value> v = a[i];
        if (v->IsInt32()) return (uint32_t)v.As<v8::Int32>()->Value();
        return v->Uint32Value(ctx()).FromMaybe(0);
    }
    int32_t i32(int i) const {
        v8::Local<v8::Value> v = a[i];
        if (v->IsInt32()) return v.As<v8::Int32>()->Value();
        return v->Int32Value(ctx()).FromMaybe(0);
    }
    double f64(int i) const {
        v8::Local<v8::Value> v = a[i];
        if (v->IsNumber()) return v.As<v8::Number>()->Value();
        return v->NumberValue(ctx()).FromMaybe(0);
    }
    uint64_t u64(int i) const {
        v8::Local<v8::Value> v = a[i];
        if (v->IsBigInt()) return v.As<v8::BigInt>()->Uint64Value();
        return (uint64_t)f64(i);
    }
};
struct FastArgs {
    union { int32_t i; uint32_t u; float f; uint64_t l; } v[8];
    uint32_t u32(int i) const { return v[i].u; }
    int32_t i32(int i) const { return v[i].i; }
    double f64(int i) const { return v[i].f; }
    uint64_t u64(int i) const { return v[i].l; }
    void put(int n, int32_t x) { v[n].l = 0; v[n].i = x; }
    void put(int n, float x) { v[n].l = 0; v[n].f = x; }
    void put(int n, int64_t x) { v[n].l = (uint64_t)x; }
};

void call_fallback(Session* s, int index, const v8::FunctionCallbackInfo<v8::Value>& a) {
    s->fallback_calls++;
    std::vector<v8::Local<v8::Value>> argv(a.Length());
    for (int i = 0; i < a.Length(); i++) argv[i] = a[i];
    v8::Local<v8::Value> r;
    if (s->fallback[index].Get(g_isolate)->Call(ctx(), v8::Undefined(g_isolate), (int)argv.size(), argv.data()).ToLocal(&r))
        a.GetReturnValue().Set(r);
}

// The native imports. Each: its name, its wasm signature (i = i32, f = f32,
// l = i64: emdawnwebgpu's webgpu.h on wasm32), and a body that returns false
// when a handle it needs is missing. From the callback, the glue's function
// then runs instead; a fast call cannot run JavaScript, so it records the
// call and the host stops the cart after the frame, saying which import
// (every object these calls name is registered when the glue makes it, so
// none should be missing).
#define S(i) H(s, a.u32(i))
#define NEED(h) if (!(h)) return false
#define NATIVE(nm, sig_str, ...) \
    struct N_##nm { \
        static constexpr const char* name = #nm; \
        static constexpr const char sig[] = sig_str; \
        template <class A> static bool run(Session* s, const A& a) __VA_ARGS__ \
    };

NATIVE(wgpuRenderPassEncoderSetPipeline, "ii>", {
    void* pass = S(0); void* pipe = S(1); NEED(pass); NEED(pipe);
    P.wgpuRenderPassEncoderSetPipeline((WGPURenderPassEncoder)pass, (WGPURenderPipeline)pipe);
    return true; })
NATIVE(wgpuRenderPassEncoderSetBindGroup, "iiiii>", {
    void* pass = S(0); uint32_t gptr = a.u32(2); void* group = H(s, gptr); NEED(pass); if (gptr) NEED(group);
    uint32_t count = a.u32(3), offs = a.u32(4);
    const uint32_t* offsets = nullptr;
    if (count) {
        wc_refresh_memory(s->host);
        if ((uint64_t)offs + (uint64_t)count * 4 > s->host->memory_size) return false;
        offsets = (const uint32_t*)(s->host->memory + offs);
    }
    P.wgpuRenderPassEncoderSetBindGroup((WGPURenderPassEncoder)pass, a.u32(1), (WGPUBindGroup)group, count, offsets);
    return true; })
NATIVE(wgpuRenderPassEncoderSetVertexBuffer, "iiill>", {
    void* pass = S(0); uint32_t bptr = a.u32(2); void* buf = H(s, bptr); NEED(pass); if (bptr) NEED(buf);
    P.wgpuRenderPassEncoderSetVertexBuffer((WGPURenderPassEncoder)pass, a.u32(1), (WGPUBuffer)buf, a.u64(3), a.u64(4));
    return true; })
NATIVE(wgpuRenderPassEncoderSetIndexBuffer, "iiill>", {
    void* pass = S(0); void* buf = S(1); NEED(pass); NEED(buf);
    P.wgpuRenderPassEncoderSetIndexBuffer((WGPURenderPassEncoder)pass, (WGPUBuffer)buf, (WGPUIndexFormat)a.u32(2), a.u64(3), a.u64(4));
    return true; })
NATIVE(wgpuRenderPassEncoderDraw, "iiiii>", {
    void* pass = S(0); NEED(pass);
    P.wgpuRenderPassEncoderDraw((WGPURenderPassEncoder)pass, a.u32(1), a.u32(2), a.u32(3), a.u32(4));
    return true; })
NATIVE(wgpuRenderPassEncoderDrawIndexed, "iiiiii>", {
    void* pass = S(0); NEED(pass);
    P.wgpuRenderPassEncoderDrawIndexed((WGPURenderPassEncoder)pass, a.u32(1), a.u32(2), a.u32(3), a.i32(4), a.u32(5));
    return true; })
NATIVE(wgpuRenderPassEncoderDrawIndirect, "iil>", {
    void* pass = S(0); void* buf = S(1); NEED(pass); NEED(buf);
    P.wgpuRenderPassEncoderDrawIndirect((WGPURenderPassEncoder)pass, (WGPUBuffer)buf, a.u64(2));
    return true; })
NATIVE(wgpuRenderPassEncoderDrawIndexedIndirect, "iil>", {
    void* pass = S(0); void* buf = S(1); NEED(pass); NEED(buf);
    P.wgpuRenderPassEncoderDrawIndexedIndirect((WGPURenderPassEncoder)pass, (WGPUBuffer)buf, a.u64(2));
    return true; })
NATIVE(wgpuRenderPassEncoderSetViewport, "iffffff>", {
    void* pass = S(0); NEED(pass);
    P.wgpuRenderPassEncoderSetViewport((WGPURenderPassEncoder)pass, (float)a.f64(1), (float)a.f64(2), (float)a.f64(3),
                                       (float)a.f64(4), (float)a.f64(5), (float)a.f64(6));
    return true; })
NATIVE(wgpuRenderPassEncoderSetScissorRect, "iiiii>", {
    void* pass = S(0); NEED(pass);
    P.wgpuRenderPassEncoderSetScissorRect((WGPURenderPassEncoder)pass, a.u32(1), a.u32(2), a.u32(3), a.u32(4));
    return true; })
NATIVE(wgpuRenderPassEncoderSetStencilReference, "ii>", {
    void* pass = S(0); NEED(pass);
    P.wgpuRenderPassEncoderSetStencilReference((WGPURenderPassEncoder)pass, a.u32(1));
    return true; })
NATIVE(wgpuRenderPassEncoderSetBlendConstant, "ii>", {
    void* pass = S(0); NEED(pass);
    uint32_t c = a.u32(1);
    wc_refresh_memory(s->host);
    if ((c & 7) || (uint64_t)c + sizeof(WGPUColor) > s->host->memory_size) return false;
    P.wgpuRenderPassEncoderSetBlendConstant((WGPURenderPassEncoder)pass, (const WGPUColor*)(s->host->memory + c));
    return true; })
NATIVE(wgpuRenderPassEncoderBeginOcclusionQuery, "ii>", {
    void* pass = S(0); NEED(pass);
    P.wgpuRenderPassEncoderBeginOcclusionQuery((WGPURenderPassEncoder)pass, a.u32(1));
    return true; })
NATIVE(wgpuRenderPassEncoderEndOcclusionQuery, "i>", {
    void* pass = S(0); NEED(pass);
    P.wgpuRenderPassEncoderEndOcclusionQuery((WGPURenderPassEncoder)pass);
    return true; })
NATIVE(wgpuRenderPassEncoderExecuteBundles, "iii>", {
    void* pass = S(0); NEED(pass);
    uint32_t n = a.u32(1), list = a.u32(2);
    wc_refresh_memory(s->host);
    if (n > 4096 || (list & 3) || (uint64_t)list + (uint64_t)n * 4 > s->host->memory_size) return false;
    WGPURenderBundle bundles[4096];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p; memcpy(&p, s->host->memory + list + i * 4, 4);
        NEED(bundles[i] = (WGPURenderBundle)H(s, p));
    }
    P.wgpuRenderPassEncoderExecuteBundles((WGPURenderPassEncoder)pass, n, bundles);
    return true; })
NATIVE(wgpuRenderPassEncoderEnd, "i>", {
    void* pass = S(0); NEED(pass);
    P.wgpuRenderPassEncoderEnd((WGPURenderPassEncoder)pass);
    return true; })
NATIVE(wgpuComputePassEncoderSetPipeline, "ii>", {
    void* pass = S(0); void* pipe = S(1); NEED(pass); NEED(pipe);
    P.wgpuComputePassEncoderSetPipeline((WGPUComputePassEncoder)pass, (WGPUComputePipeline)pipe);
    return true; })
NATIVE(wgpuComputePassEncoderSetBindGroup, "iiiii>", {
    void* pass = S(0); uint32_t gptr = a.u32(2); void* group = H(s, gptr); NEED(pass); if (gptr) NEED(group);
    uint32_t count = a.u32(3), offs = a.u32(4);
    const uint32_t* offsets = nullptr;
    if (count) {
        wc_refresh_memory(s->host);
        if ((uint64_t)offs + (uint64_t)count * 4 > s->host->memory_size) return false;
        offsets = (const uint32_t*)(s->host->memory + offs);
    }
    P.wgpuComputePassEncoderSetBindGroup((WGPUComputePassEncoder)pass, a.u32(1), (WGPUBindGroup)group, count, offsets);
    return true; })
NATIVE(wgpuComputePassEncoderDispatchWorkgroups, "iiii>", {
    void* pass = S(0); NEED(pass);
    P.wgpuComputePassEncoderDispatchWorkgroups((WGPUComputePassEncoder)pass, a.u32(1), a.u32(2), a.u32(3));
    return true; })
NATIVE(wgpuComputePassEncoderDispatchWorkgroupsIndirect, "iil>", {
    void* pass = S(0); void* buf = S(1); NEED(pass); NEED(buf);
    P.wgpuComputePassEncoderDispatchWorkgroupsIndirect((WGPUComputePassEncoder)pass, (WGPUBuffer)buf, a.u64(2));
    return true; })
NATIVE(wgpuComputePassEncoderEnd, "i>", {
    void* pass = S(0); NEED(pass);
    P.wgpuComputePassEncoderEnd((WGPUComputePassEncoder)pass);
    return true; })
NATIVE(wgpuQueueWriteBuffer, "iilii>", {
    void* q = S(0); void* buf = S(1); NEED(q); NEED(buf);
    uint32_t data = a.u32(3), size = a.u32(4);
    wc_refresh_memory(s->host);
    if ((uint64_t)data + size > s->host->memory_size) return false;
    P.wgpuQueueWriteBuffer((WGPUQueue)q, (WGPUBuffer)buf, a.u64(2), s->host->memory + data, size);
    return true; })
#undef S
#undef NEED
#undef NATIVE

template <size_t N> struct Sig {
    char s[N];
    constexpr Sig(const char (&x)[N]) { for (size_t i = 0; i < N; i++) s[i] = x[i]; }
    constexpr size_t nparams() const { size_t i = 0; while (s[i] != '>') i++; return i; }
    constexpr bool wide() const { for (size_t i = 0; i < N; i++) if (s[i] == 'l') return true; return false; }
};
template <char C> struct CType;
template <> struct CType<'i'> { using T = int32_t; };
template <> struct CType<'f'> { using T = float; };
template <> struct CType<'l'> { using T = int64_t; };

template <class Nt>
void slow_cb(const v8::FunctionCallbackInfo<v8::Value>& a) {
    auto* d = (CallData*)a.Data().As<v8::External>()->Value();
    if (Nt::run(d->s, SlowArgs{ a })) { d->s->native_calls++; wc_wgpu_native_calls_total++; return; }
    call_fallback(d->s, d->index, a);
}
template <class Nt, class... P>
void fast_cb(v8::Local<v8::Value>, P... p, v8::FastApiCallbackOptions& o) {
    auto* d = (CallData*)o.data.As<v8::External>()->Value();
    FastArgs fa;
    int k = 0;
    (fa.put(k++, p), ...);
    (void)k;
    if (Nt::run(d->s, fa)) { d->s->native_calls++; wc_wgpu_native_calls_total++; return; }
    if (!d->s->failed) d->s->failed = Nt::name;
    d->s->failed_calls++;
}
template <class Nt, Sig S, size_t... I>
const v8::CFunction* cfunc_of(std::index_sequence<I...>) {
    static const v8::CFunction cf = v8::CFunction::Make(
        &fast_cb<Nt, typename CType<S.s[I]>::T...>,
        S.wide() ? v8::CFunctionInfo::Int64Representation::kBigInt : v8::CFunctionInfo::Int64Representation::kNumber);
    return &cf;
}
template <class Nt>
const v8::CFunction* cfunc() {
    constexpr Sig sig(Nt::sig);
    return cfunc_of<Nt, sig>(std::make_index_sequence<sig.nparams()>{});
}

struct Native { const char* name; v8::FunctionCallback slow; const v8::CFunction* (*fast)(); };
#define E(nm) { N_##nm::name, &slow_cb<N_##nm>, &cfunc<N_##nm> }
const Native kNatives[] = {
    E(wgpuRenderPassEncoderSetPipeline), E(wgpuRenderPassEncoderSetBindGroup), E(wgpuRenderPassEncoderSetVertexBuffer),
    E(wgpuRenderPassEncoderSetIndexBuffer), E(wgpuRenderPassEncoderDraw), E(wgpuRenderPassEncoderDrawIndexed),
    E(wgpuRenderPassEncoderDrawIndirect), E(wgpuRenderPassEncoderDrawIndexedIndirect), E(wgpuRenderPassEncoderSetViewport),
    E(wgpuRenderPassEncoderSetScissorRect), E(wgpuRenderPassEncoderSetStencilReference), E(wgpuRenderPassEncoderSetBlendConstant),
    E(wgpuRenderPassEncoderBeginOcclusionQuery), E(wgpuRenderPassEncoderEndOcclusionQuery), E(wgpuRenderPassEncoderExecuteBundles),
    E(wgpuRenderPassEncoderEnd), E(wgpuComputePassEncoderSetPipeline), E(wgpuComputePassEncoderSetBindGroup),
    E(wgpuComputePassEncoderDispatchWorkgroups), E(wgpuComputePassEncoderDispatchWorkgroupsIndirect), E(wgpuComputePassEncoderEnd),
    E(wgpuQueueWriteBuffer),
};
#undef E
constexpr int kNativeCount = (int)(sizeof kNatives / sizeof kNatives[0]);

// wgpuCommandEncoderBeginRenderPass(encoder, descriptor) -> pass. Through the
// glue it built the descriptor as JavaScript objects for dawn.node to convert
// back (about 8 us a pass; three.c's frames have ~30 passes). Here the
// descriptor is read from cart memory into webgpu.h's structs and Dawn is
// called directly; the pass gets its handle from the cart's own
// emwgpuCreateRenderPassEncoder, as the glue's does, and only this table
// knows it (installed only when every pass call the cart imports is served
// here). A descriptor with chained structs or timestamp writes, or naming an
// object with no native handle, goes to the glue's function.
void begin_pass_cb(const v8::FunctionCallbackInfo<v8::Value>& a) {
    auto* d = (CallData*)a.Data().As<v8::External>()->Value();
    Session* s = d->s;
    SlowArgs A{ a };
    auto glue = [&] {
        s->fallback_calls++;
        v8::Local<v8::Value> argv[2] = { a[0], a[1] };
        v8::Local<v8::Value> r;
        if (s->begin_pass_glue.Get(g_isolate)->Call(ctx(), v8::Undefined(g_isolate), 2, argv).ToLocal(&r)) a.GetReturnValue().Set(r);
    };
    void* enc = H(s, A.u32(0));
    uint32_t dp = A.u32(1);
    if (!enc || !dp || s->create_pass.IsEmpty()) return glue();
    wc_refresh_memory(s->host);
    const uint8_t* m = s->host->memory;
    const uint64_t size = s->host->memory_size;
    auto u32at = [&](uint64_t off) { uint32_t v; memcpy(&v, m + off, 4); return v; };
    if ((uint64_t)dp + 32 > size) return glue();
    uint32_t count = u32at(dp + 12), cap = u32at(dp + 16), dsa = u32at(dp + 20), occ = u32at(dp + 24);
    if (u32at(dp) || u32at(dp + 28) || count > 8) return glue();
    WGPURenderPassColorAttachment ca[8];
    for (uint32_t i = 0; i < count; i++) {
        uint64_t b = (uint64_t)cap + 56ull * i;
        if (b + 56 > size || u32at(b)) return glue();
        uint32_t view = u32at(b + 4), resolve = u32at(b + 12);
        ca[i] = WGPURenderPassColorAttachment{};
        ca[i].view = (WGPUTextureView)H(s, view);
        ca[i].resolveTarget = (WGPUTextureView)H(s, resolve);
        if ((view && !ca[i].view) || (resolve && !ca[i].resolveTarget)) return glue();
        ca[i].depthSlice = u32at(b + 8);
        ca[i].loadOp = (WGPULoadOp)u32at(b + 16);
        ca[i].storeOp = (WGPUStoreOp)u32at(b + 20);
        memcpy(&ca[i].clearValue, m + b + 24, sizeof(WGPUColor));
    }
    WGPURenderPassDepthStencilAttachment ds{};
    if (dsa) {
        if ((uint64_t)dsa + 40 > size || u32at(dsa)) return glue();
        ds.view = (WGPUTextureView)H(s, u32at(dsa + 4));
        if (!ds.view) return glue();
        ds.depthLoadOp = (WGPULoadOp)u32at(dsa + 8);
        ds.depthStoreOp = (WGPUStoreOp)u32at(dsa + 12);
        memcpy(&ds.depthClearValue, m + dsa + 16, 4);
        ds.depthReadOnly = u32at(dsa + 20);
        ds.stencilLoadOp = (WGPULoadOp)u32at(dsa + 24);
        ds.stencilStoreOp = (WGPUStoreOp)u32at(dsa + 28);
        ds.stencilClearValue = u32at(dsa + 32);
        ds.stencilReadOnly = u32at(dsa + 36);
    }
    WGPURenderPassDescriptor desc{};
    desc.label = WGPUStringView{ nullptr, WGPU_STRLEN };
    desc.colorAttachmentCount = count;
    desc.colorAttachments = count ? ca : nullptr;
    desc.depthStencilAttachment = dsa ? &ds : nullptr;
    desc.occlusionQuerySet = (WGPUQuerySet)H(s, occ);
    if (occ && !desc.occlusionQuerySet) return glue();
    // the cart's handle first (it runs the cart's code: memory may grow)
    v8::Local<v8::Value> zero = v8::Integer::New(g_isolate, 0), r;
    if (!s->create_pass.Get(g_isolate)->Call(ctx(), v8::Undefined(g_isolate), 1, &zero).ToLocal(&r)) return;  // trapped
    uint32_t ptr = r->Uint32Value(ctx()).FromMaybe(0);
    if (!ptr) return glue();
    WGPURenderPassEncoder pass = P.wgpuCommandEncoderBeginRenderPass((WGPUCommandEncoder)enc, &desc);
    s->handles.set(ptr, pass);
    s->owned.set(ptr, pass);
    s->native_calls++;
    wc_wgpu_native_calls_total++;
    a.GetReturnValue().Set(ptr);
}

void register_cb(const v8::FunctionCallbackInfo<v8::Value>& a) {
    auto* s = (Session*)a.Data().As<v8::External>()->Value();
    if (a.Length() < 2 || !a[1]->IsBigInt()) return;
    uint32_t ptr = SlowArgs{ a }.u32(0);
    void* h = (void*)(uintptr_t)a[1].As<v8::BigInt>()->Uint64Value();
    if (ptr && h) s->handles.set(ptr, h);
}
void forget_cb(const v8::FunctionCallbackInfo<v8::Value>& a) {
    auto* s = (Session*)a.Data().As<v8::External>()->Value();
    if (a.Length() < 1) return;
    uint32_t ptr = SlowArgs{ a }.u32(0);
    s->handles.erase(ptr);
    if (void* pass = s->owned.get(ptr)) {
        P.wgpuRenderPassEncoderRelease((WGPURenderPassEncoder)pass);
        s->owned.erase(ptr);
    }
}

}  // namespace

// Any of Dawn's C functions, from the library the bridge loaded (NULL before
// it loaded, or in a build without WebGPU): wgpu_gl_share.cpp's.
void* wc_wgpu_proc(const char* name) {
#if defined(_WIN32)
    return g_dawn_lib ? (void*)GetProcAddress((HMODULE)g_dawn_lib, name) : nullptr;
#else
    return g_dawn_lib ? dlsym(g_dawn_lib, name) : nullptr;
#endif
}

// Resolve Dawn's C API from the library the bridge loaded (once).
extern "C" bool wc_wgpu_native_init(const char* dir) {
    static bool tried = false;
    if (!tried) {
        tried = true;
        g_procs_ok = resolve_procs(dir);
        if (!g_procs_ok) wc_log("wasmcart: WebGPU calls go through JavaScript: Dawn's C API was not found in %s\n", dir);
    }
    return g_procs_ok;
}

void wc_wgpu_native_drop(wc_host_t* host);

// The two functions the bridge reports handles to, for one host:
// { register(ptr, handle), forget(ptr) }. Empty when Dawn's C API is not
// available (every call then stays in the glue).
v8::Local<v8::Value> wc_wgpu_native_table(wc_host_t* host) {
    if (!g_procs_ok) return v8::Undefined(g_isolate);
    wc_wgpu_native_drop(host);
    auto* s = new Session();
    s->host = host;
    g_sessions[host] = s;
    auto data = v8::External::New(g_isolate, s);
    auto o = v8::Object::New(g_isolate);
    o->Set(ctx(), v8::String::NewFromUtf8Literal(g_isolate, "register"),
           v8::Function::New(ctx(), register_cb, data).ToLocalChecked()).Check();
    o->Set(ctx(), v8::String::NewFromUtf8Literal(g_isolate, "forget"),
           v8::Function::New(ctx(), forget_cb, data).ToLocalChecked()).Check();
    return o;
}

// Replace the cart's hot WebGPU imports in `env` (and the `wgpu` module
// object) with the native ones, keeping the glue's as their fallback.
// Returns how many were replaced.
int wc_wgpu_native_install(wc_host_t* host, v8::Local<v8::Object> env, v8::Local<v8::Object> wgpu_mod) {
    auto it = g_sessions.find(host);
    if (it == g_sessions.end()) return 0;
    Session* s = it->second;
    s->fallback.clear();
    s->fallback.resize(kNativeCount);
    s->calls.clear();
    s->calls.reserve(kNativeCount);  // never reallocated: the callbacks keep pointers into it
    for (int i = 0; i < kNativeCount; i++) s->calls.push_back(CallData{ s, i });
    int n = 0;
    for (int i = 0; i < kNativeCount; i++) {
        auto key = v8::String::NewFromUtf8(g_isolate, kNatives[i].name).ToLocalChecked();
        v8::Local<v8::Value> glue;
        if (!wgpu_mod->Get(ctx(), key).ToLocal(&glue) || !glue->IsFunction()) continue;
        s->fallback[i].Reset(g_isolate, glue.As<v8::Function>());
        // the session's own template (its data is this session's CallData)
        auto t = v8::FunctionTemplate::New(g_isolate, kNatives[i].slow, v8::External::New(g_isolate, &s->calls[i]),
                                           v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
                                           v8::SideEffectType::kHasSideEffect, kNatives[i].fast());
        auto fn = t->GetFunction(ctx()).ToLocalChecked();
        env->Set(ctx(), key, fn).Check();
        wgpu_mod->Set(ctx(), key, fn).Check();
        n++;
    }
    // wgpuCommandEncoderBeginRenderPass here too, when every pass call the
    // cart imports is served here (a pass made here has no glue object)
    bool passes_native = true;
    v8::Local<v8::Value> begin_glue;
    auto begin_key = v8::String::NewFromUtf8Literal(g_isolate, "wgpuCommandEncoderBeginRenderPass");
    v8::Local<v8::Array> names;
    if (!wgpu_mod->Get(ctx(), begin_key).ToLocal(&begin_glue) || !begin_glue->IsFunction() ||
        !wgpu_mod->GetOwnPropertyNames(ctx()).ToLocal(&names)) passes_native = false;
    for (uint32_t i = 0; passes_native && i < names->Length(); i++) {
        v8::Local<v8::Value> k;
        if (!names->Get(ctx(), i).ToLocal(&k)) { passes_native = false; break; }
        v8::String::Utf8Value nm(g_isolate, k);
        if (!*nm || strncmp(*nm, "wgpuRenderPassEncoder", 21) != 0) continue;
        bool served = false;
        for (int j = 0; j < kNativeCount; j++) if (!strcmp(*nm, kNatives[j].name)) served = true;
        if (!served) passes_native = false;
    }
    if (passes_native) {
        s->begin_pass_glue.Reset(g_isolate, begin_glue.As<v8::Function>());
        s->begin_call = CallData{ s, -1 };
        auto t = v8::FunctionTemplate::New(g_isolate, begin_pass_cb, v8::External::New(g_isolate, &s->begin_call));
        auto fn = t->GetFunction(ctx()).ToLocalChecked();
        env->Set(ctx(), begin_key, fn).Check();
        wgpu_mod->Set(ctx(), begin_key, fn).Check();
        n++;
    }
    return n;
}

// The cart's instance exists: the export begin_pass_cb makes pass handles with.
void wc_wgpu_native_attach(wc_host_t* host, v8::Local<v8::Object> exports) {
    auto it = g_sessions.find(host);
    if (it == g_sessions.end()) return;
    v8::Local<v8::Value> f;
    if (exports->Get(ctx(), v8::String::NewFromUtf8Literal(g_isolate, "emwgpuCreateRenderPassEncoder")).ToLocal(&f) && f->IsFunction())
        it->second->create_pass.Reset(g_isolate, f.As<v8::Function>());
}

// The host unloaded its cart (whose instance, and so its imports, are gone):
// its table goes with it.
void wc_wgpu_native_drop(wc_host_t* host) {
    auto it = g_sessions.find(host);
    if (it == g_sessions.end()) return;
    // every handle these calls name should be in the table: say so when not
    if (it->second->fallback_calls)
        wc_log("wasmcart: %llu of %llu native WebGPU calls named an object with no native handle (the glue answered them)\n",
               (unsigned long long)it->second->fallback_calls,
               (unsigned long long)(it->second->fallback_calls + it->second->native_calls));
    for (auto& f : it->second->fallback) f.Reset();
    it->second->owned.each([](uint32_t, void* pass) { P.wgpuRenderPassEncoderRelease((WGPURenderPassEncoder)pass); });
    it->second->create_pass.Reset();
    it->second->begin_pass_glue.Reset();
    delete it->second;
    g_sessions.erase(it);
}

// A fast native call that named an object with no native handle (it was
// skipped: a fast call cannot hand it to the glue): that import's name, else
// NULL. The host stops the cart with it after the frame.
const char* wc_wgpu_native_failed(wc_host_t* host, uint64_t* calls) {
    auto it = g_sessions.find(host);
    if (it == g_sessions.end() || !it->second->failed) return nullptr;
    if (calls) *calls = it->second->failed_calls;
    return it->second->failed;
}

// For a host's performance log: calls served natively, and by the glue.
extern "C" void wc_host_wgpu_native_stats(wc_host_t* host, uint64_t* native_calls, uint64_t* fallback_calls) {
    auto it = g_sessions.find(host);
    *native_calls = it == g_sessions.end() ? 0 : it->second->native_calls;
    *fallback_calls = it == g_sessions.end() ? 0 : it->second->fallback_calls;
}

#else  // no WebGPU support in this build

extern "C" bool wc_wgpu_native_init(const char*) { return false; }
void* wc_wgpu_proc(const char*) { return nullptr; }
v8::Local<v8::Value> wc_wgpu_native_table(wc_host_t*) { return v8::Undefined(g_isolate); }
int wc_wgpu_native_install(wc_host_t*, v8::Local<v8::Object>, v8::Local<v8::Object>) { return 0; }
void wc_wgpu_native_attach(wc_host_t*, v8::Local<v8::Object>) {}
void wc_wgpu_native_drop(wc_host_t*) {}
extern "C" void wc_host_wgpu_native_stats(wc_host_t*, uint64_t* n, uint64_t* f) { *n = *f = 0; }
const char* wc_wgpu_native_failed(wc_host_t*, uint64_t*) { return nullptr; }

#endif
