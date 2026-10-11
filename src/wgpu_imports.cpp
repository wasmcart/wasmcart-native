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
    X(wgpuQueueWriteBuffer)

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

struct Session;
struct CallData { Session* s; int index; };
struct Session {
    wc_host_t* host;
    std::unordered_map<uint32_t, void*> handles;
    // the glue's function for each native import, called when a handle is missing
    std::vector<v8::Global<v8::Function>> fallback;
    std::vector<CallData> calls;  // each native import's callback data
    uint64_t native_calls = 0, fallback_calls = 0;
};
std::unordered_map<wc_host_t*, Session*> g_sessions;

inline void* H(Session* s, uint32_t ptr) {
    if (!ptr) return nullptr;
    auto it = s->handles.find(ptr);
    return it == s->handles.end() ? nullptr : it->second;
}

// ── argument helpers (wasm i32 -> Number, i64 -> BigInt, f32 -> Number) ──

inline uint32_t U32(const v8::FunctionCallbackInfo<v8::Value>& a, int i) {
    v8::Local<v8::Value> v = a[i];
    if (v->IsInt32()) return (uint32_t)v.As<v8::Int32>()->Value();
    return v->Uint32Value(ctx()).FromMaybe(0);
}
inline int32_t I32(const v8::FunctionCallbackInfo<v8::Value>& a, int i) {
    v8::Local<v8::Value> v = a[i];
    if (v->IsInt32()) return v.As<v8::Int32>()->Value();
    return v->Int32Value(ctx()).FromMaybe(0);
}
inline double F64(const v8::FunctionCallbackInfo<v8::Value>& a, int i) {
    v8::Local<v8::Value> v = a[i];
    if (v->IsNumber()) return v.As<v8::Number>()->Value();
    return v->NumberValue(ctx()).FromMaybe(0);
}
inline uint64_t U64(const v8::FunctionCallbackInfo<v8::Value>& a, int i) {
    v8::Local<v8::Value> v = a[i];
    if (v->IsBigInt()) return v.As<v8::BigInt>()->Uint64Value();
    return (uint64_t)F64(a, i);
}

void call_fallback(Session* s, int index, const v8::FunctionCallbackInfo<v8::Value>& a) {
    s->fallback_calls++;
    std::vector<v8::Local<v8::Value>> argv(a.Length());
    for (int i = 0; i < a.Length(); i++) argv[i] = a[i];
    v8::Local<v8::Value> r;
    if (s->fallback[index].Get(g_isolate)->Call(ctx(), v8::Undefined(g_isolate), (int)argv.size(), argv.data()).ToLocal(&r))
        a.GetReturnValue().Set(r);
}

// The native imports. Each: its name, and a body that returns false when a
// handle it needs is missing (the glue's function runs instead).
using Body = bool (*)(Session*, const v8::FunctionCallbackInfo<v8::Value>&);
struct Native { const char* name; Body body; };

#define S(i) H(s, U32(a, i))
#define NEED(h) if (!(h)) return false

const Native kNatives[] = {
    { "wgpuRenderPassEncoderSetPipeline", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); void* pipe = S(1); NEED(pass); NEED(pipe);
        P.wgpuRenderPassEncoderSetPipeline((WGPURenderPassEncoder)pass, (WGPURenderPipeline)pipe);
        return true; } },
    { "wgpuRenderPassEncoderSetBindGroup", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); uint32_t gptr = U32(a, 2); void* group = H(s, gptr); NEED(pass); if (gptr) NEED(group);
        uint32_t count = U32(a, 3), offs = U32(a, 4);
        const uint32_t* offsets = nullptr;
        if (count) {
            wc_refresh_memory(s->host);
            if ((uint64_t)offs + (uint64_t)count * 4 > s->host->memory_size) return false;
            offsets = (const uint32_t*)(s->host->memory + offs);
        }
        P.wgpuRenderPassEncoderSetBindGroup((WGPURenderPassEncoder)pass, U32(a, 1), (WGPUBindGroup)group, count, offsets);
        return true; } },
    { "wgpuRenderPassEncoderSetVertexBuffer", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); uint32_t bptr = U32(a, 2); void* buf = H(s, bptr); NEED(pass); if (bptr) NEED(buf);
        P.wgpuRenderPassEncoderSetVertexBuffer((WGPURenderPassEncoder)pass, U32(a, 1), (WGPUBuffer)buf, U64(a, 3), U64(a, 4));
        return true; } },
    { "wgpuRenderPassEncoderSetIndexBuffer", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); void* buf = S(1); NEED(pass); NEED(buf);
        P.wgpuRenderPassEncoderSetIndexBuffer((WGPURenderPassEncoder)pass, (WGPUBuffer)buf, (WGPUIndexFormat)U32(a, 2), U64(a, 3), U64(a, 4));
        return true; } },
    { "wgpuRenderPassEncoderDraw", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        P.wgpuRenderPassEncoderDraw((WGPURenderPassEncoder)pass, U32(a, 1), U32(a, 2), U32(a, 3), U32(a, 4));
        return true; } },
    { "wgpuRenderPassEncoderDrawIndexed", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        P.wgpuRenderPassEncoderDrawIndexed((WGPURenderPassEncoder)pass, U32(a, 1), U32(a, 2), U32(a, 3), I32(a, 4), U32(a, 5));
        return true; } },
    { "wgpuRenderPassEncoderDrawIndirect", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); void* buf = S(1); NEED(pass); NEED(buf);
        P.wgpuRenderPassEncoderDrawIndirect((WGPURenderPassEncoder)pass, (WGPUBuffer)buf, U64(a, 2));
        return true; } },
    { "wgpuRenderPassEncoderDrawIndexedIndirect", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); void* buf = S(1); NEED(pass); NEED(buf);
        P.wgpuRenderPassEncoderDrawIndexedIndirect((WGPURenderPassEncoder)pass, (WGPUBuffer)buf, U64(a, 2));
        return true; } },
    { "wgpuRenderPassEncoderSetViewport", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        P.wgpuRenderPassEncoderSetViewport((WGPURenderPassEncoder)pass, (float)F64(a, 1), (float)F64(a, 2), (float)F64(a, 3),
                                           (float)F64(a, 4), (float)F64(a, 5), (float)F64(a, 6));
        return true; } },
    { "wgpuRenderPassEncoderSetScissorRect", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        P.wgpuRenderPassEncoderSetScissorRect((WGPURenderPassEncoder)pass, U32(a, 1), U32(a, 2), U32(a, 3), U32(a, 4));
        return true; } },
    { "wgpuRenderPassEncoderSetStencilReference", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        P.wgpuRenderPassEncoderSetStencilReference((WGPURenderPassEncoder)pass, U32(a, 1));
        return true; } },
    { "wgpuRenderPassEncoderSetBlendConstant", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        uint32_t c = U32(a, 1);
        wc_refresh_memory(s->host);
        if ((c & 7) || (uint64_t)c + sizeof(WGPUColor) > s->host->memory_size) return false;
        P.wgpuRenderPassEncoderSetBlendConstant((WGPURenderPassEncoder)pass, (const WGPUColor*)(s->host->memory + c));
        return true; } },
    { "wgpuRenderPassEncoderBeginOcclusionQuery", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        P.wgpuRenderPassEncoderBeginOcclusionQuery((WGPURenderPassEncoder)pass, U32(a, 1));
        return true; } },
    { "wgpuRenderPassEncoderEndOcclusionQuery", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        P.wgpuRenderPassEncoderEndOcclusionQuery((WGPURenderPassEncoder)pass);
        return true; } },
    { "wgpuRenderPassEncoderExecuteBundles", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        uint32_t n = U32(a, 1), list = U32(a, 2);
        wc_refresh_memory(s->host);
        if (n > 4096 || (list & 3) || (uint64_t)list + (uint64_t)n * 4 > s->host->memory_size) return false;
        WGPURenderBundle bundles[4096];
        for (uint32_t i = 0; i < n; i++) {
            uint32_t p; memcpy(&p, s->host->memory + list + i * 4, 4);
            NEED(bundles[i] = (WGPURenderBundle)H(s, p));
        }
        P.wgpuRenderPassEncoderExecuteBundles((WGPURenderPassEncoder)pass, n, bundles);
        return true; } },
    { "wgpuRenderPassEncoderEnd", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        P.wgpuRenderPassEncoderEnd((WGPURenderPassEncoder)pass);
        return true; } },
    { "wgpuComputePassEncoderSetPipeline", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); void* pipe = S(1); NEED(pass); NEED(pipe);
        P.wgpuComputePassEncoderSetPipeline((WGPUComputePassEncoder)pass, (WGPUComputePipeline)pipe);
        return true; } },
    { "wgpuComputePassEncoderSetBindGroup", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); uint32_t gptr = U32(a, 2); void* group = H(s, gptr); NEED(pass); if (gptr) NEED(group);
        uint32_t count = U32(a, 3), offs = U32(a, 4);
        const uint32_t* offsets = nullptr;
        if (count) {
            wc_refresh_memory(s->host);
            if ((uint64_t)offs + (uint64_t)count * 4 > s->host->memory_size) return false;
            offsets = (const uint32_t*)(s->host->memory + offs);
        }
        P.wgpuComputePassEncoderSetBindGroup((WGPUComputePassEncoder)pass, U32(a, 1), (WGPUBindGroup)group, count, offsets);
        return true; } },
    { "wgpuComputePassEncoderDispatchWorkgroups", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        P.wgpuComputePassEncoderDispatchWorkgroups((WGPUComputePassEncoder)pass, U32(a, 1), U32(a, 2), U32(a, 3));
        return true; } },
    { "wgpuComputePassEncoderDispatchWorkgroupsIndirect", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); void* buf = S(1); NEED(pass); NEED(buf);
        P.wgpuComputePassEncoderDispatchWorkgroupsIndirect((WGPUComputePassEncoder)pass, (WGPUBuffer)buf, U64(a, 2));
        return true; } },
    { "wgpuComputePassEncoderEnd", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* pass = S(0); NEED(pass);
        P.wgpuComputePassEncoderEnd((WGPUComputePassEncoder)pass);
        return true; } },
    { "wgpuQueueWriteBuffer", [](Session* s, const v8::FunctionCallbackInfo<v8::Value>& a) {
        void* q = S(0); void* buf = S(1); NEED(q); NEED(buf);
        uint32_t data = U32(a, 3), size = U32(a, 4);
        wc_refresh_memory(s->host);
        if ((uint64_t)data + size > s->host->memory_size) return false;
        P.wgpuQueueWriteBuffer((WGPUQueue)q, (WGPUBuffer)buf, U64(a, 2), s->host->memory + data, size);
        return true; } },
};
#undef S
#undef NEED
constexpr int kNativeCount = (int)(sizeof kNatives / sizeof kNatives[0]);

void native_cb(const v8::FunctionCallbackInfo<v8::Value>& a) {
    auto* d = (CallData*)a.Data().As<v8::External>()->Value();
    if (kNatives[d->index].body(d->s, a)) { d->s->native_calls++; return; }
    call_fallback(d->s, d->index, a);
}

void register_cb(const v8::FunctionCallbackInfo<v8::Value>& a) {
    auto* s = (Session*)a.Data().As<v8::External>()->Value();
    if (a.Length() < 2 || !a[1]->IsBigInt()) return;
    uint32_t ptr = U32(a, 0);
    void* h = (void*)(uintptr_t)a[1].As<v8::BigInt>()->Uint64Value();
    if (ptr && h) s->handles[ptr] = h;
}
void forget_cb(const v8::FunctionCallbackInfo<v8::Value>& a) {
    auto* s = (Session*)a.Data().As<v8::External>()->Value();
    if (a.Length() >= 1) s->handles.erase(U32(a, 0));
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
        auto fn = v8::Function::New(ctx(), native_cb, v8::External::New(g_isolate, &s->calls[i])).ToLocalChecked();
        env->Set(ctx(), key, fn).Check();
        wgpu_mod->Set(ctx(), key, fn).Check();
        n++;
    }
    return n;
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
    delete it->second;
    g_sessions.erase(it);
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
void wc_wgpu_native_drop(wc_host_t*) {}
extern "C" void wc_host_wgpu_native_stats(wc_host_t*, uint64_t* n, uint64_t* f) { *n = *f = 0; }

#endif
