// code_cache.cpp — a cart's compiled code kept between runs.
//
// Without it every start compiled the cart with V8's baseline compiler
// (Liftoff) and then optimized it again function by function as it ran: the
// first seconds of every start ran on baseline code (a 3D game at about 30 fps
// where it holds 52 once optimized). V8 can serialize a compiled module, the
// optimized functions included, and start from that next time.
//
// The cart compiles through WebAssembly module compilation with a caching
// callback: when a file for the cart's wire bytes exists (named by their
// SHA-256), V8 takes the serialized module from it instead of compiling (it
// refuses one from another V8 version or flags, and then compiles as usual).
// Whenever V8 has optimized a significant batch of functions it says so, and
// the module is serialized again on a background thread and the file replaced
// (written to a temporary name, then renamed), so the next start gets every
// optimized function this run produced. The directory is capped: the oldest
// files go once it holds more than kMaxBytes.

#include "v8.h"
#include "v8-wasm.h"
extern "C" {
#include "cart_host.h"
}
#include "wc_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

extern v8::Isolate* g_isolate;

namespace {

constexpr uint64_t kMaxBytes = 1ull << 30;  // the directory's cap
constexpr char kMagic[8] = { 'W', 'C', 'C', 'O', 'D', 'E', '1', 0 };

// ── SHA-256 (FIPS 180-4) ──────────────────────────────────────────────────

struct Sha256 {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string hex(const uint8_t* data, size_t len) {
        size_t i = 0;
        for (; i + 64 <= len; i += 64) block(data + i);
        uint8_t tail[128] = { 0 };
        size_t rest = len - i;
        memcpy(tail, data + i, rest);
        tail[rest] = 0x80;
        size_t n = rest + 1 + 8 <= 64 ? 64 : 128;
        uint64_t bits = (uint64_t)len * 8;
        for (int j = 0; j < 8; j++) tail[n - 1 - j] = (uint8_t)(bits >> (8 * j));
        block(tail);
        if (n == 128) block(tail + 64);
        char out[65];
        for (int j = 0; j < 8; j++) snprintf(out + j * 8, 9, "%08x", h[j]);
        return std::string(out, 64);
    }
};

// ── the directory ─────────────────────────────────────────────────────────

std::string default_dir() {
#if defined(_WIN32)
    const char* base = getenv("LOCALAPPDATA");
    return base && *base ? std::string(base) + "\\wasmcart\\cache\\code" : "";
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    return home && *home ? std::string(home) + "/Library/Caches/wasmcart/code" : "";
#else
    const char* xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/wasmcart/code";
    const char* home = getenv("HOME");
    return home && *home ? std::string(home) + "/.cache/wasmcart/code" : "";
#endif
}

std::optional<std::vector<uint8_t>> read_cache(const std::string& path, const std::string& hash) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    char head[8 + 64];
    if (!f.read(head, sizeof head) || memcmp(head, kMagic, 8) != 0 || memcmp(head + 8, hash.data(), 64) != 0) return std::nullopt;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty()) return std::nullopt;
    return data;
}

// Remove the oldest files past the cap, and temporary files a killed run left.
void trim(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    uint64_t total = 0;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        auto p = e.path();
        if (p.extension() == ".tmp") {
            if (fs::last_write_time(p, ec) < fs::file_time_type::clock::now() - std::chrono::hours(1)) fs::remove(p, ec);
            continue;
        }
        if (p.extension() != ".v8code") continue;
        total += e.file_size(ec);
        files.emplace_back(fs::last_write_time(p, ec), p);
    }
    std::sort(files.begin(), files.end());
    for (auto& [t, p] : files) {
        if (total <= kMaxBytes) break;
        uint64_t sz = fs::file_size(p, ec);
        if (fs::remove(p, ec)) total -= sz;
    }
}

// ── background serialization ──────────────────────────────────────────────

struct Writer {
    std::mutex m;
    std::condition_variable cv;
    std::optional<v8::CompiledWasmModule> pending;
    std::string path, hash, dir;
    bool busy = false;
    std::thread thread;
};
Writer* g_writer = nullptr;

void write_loop(Writer* w) {
    std::unique_lock<std::mutex> lock(w->m);
    while (w->pending) {
        v8::CompiledWasmModule module = std::move(*w->pending);
        w->pending.reset();
        std::string path = w->path, hash = w->hash, dir = w->dir;
        lock.unlock();
        v8::OwnedBuffer data = module.Serialize();
        if (data.size) {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::string tmp = path + "." + std::to_string((unsigned long long)std::hash<std::thread::id>()(std::this_thread::get_id())) + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                f.write(kMagic, 8);
                f.write(hash.data(), 64);
                f.write((const char*)data.buffer.get(), (std::streamsize)data.size);
                if (!f) ec = std::make_error_code(std::errc::io_error);
            }
            if (!ec) std::filesystem::rename(tmp, path, ec);
            if (ec) {
                std::filesystem::remove(tmp, ec);
                wc_log("wasmcart: could not write the compiled-code cache %s\n", path.c_str());
            } else {
                trim(dir);
            }
        }
        lock.lock();
    }
    w->busy = false;
    w->cv.notify_all();
}

void queue_write(v8::CompiledWasmModule module) {
    Writer* w = g_writer;
    if (!w) return;
    std::lock_guard<std::mutex> lock(w->m);
    w->pending.emplace(std::move(module));  // the newest batch replaces one not yet written
    if (w->busy) return;
    w->busy = true;
    if (w->thread.joinable()) w->thread.join();
    w->thread = std::thread(write_loop, w);
}

}  // namespace

// Wait for a cache write still in progress (before the host goes away).
extern "C" void wc_code_cache_flush(void) {
    Writer* w = g_writer;
    if (!w) return;
    std::unique_lock<std::mutex> lock(w->m);
    w->cv.wait(lock, [&] { return !w->busy; });
    lock.unlock();
    if (w->thread.joinable()) w->thread.join();
}

// Compile the cart's wasm, from the cache when it has this cart. cache_dir:
// NULL = the platform's cache directory, "" = no cache. pump runs the event
// loop while V8 finishes (its result arrives as a foreground task).
v8::MaybeLocal<v8::WasmModuleObject> wc_compile_cached(wc_host_t* host, v8::Local<v8::Context> context, const uint8_t* bytes,
                                                       size_t len, const char* cache_dir, void (*pump)(wc_host_t*),
                                                       std::string* error) {
    std::string dir = cache_dir ? std::string(cache_dir) : default_dir();
    std::string hash, path;
    std::optional<std::vector<uint8_t>> cached;
    if (!dir.empty()) {
        Sha256 sha;
        hash = sha.hex(bytes, len);
        path = dir + "/" + hash + ".v8code";
        cached = read_cache(path, hash);
        wc_code_cache_flush();  // a write for the previous cart, if one is running
        if (!g_writer) g_writer = new Writer();
        std::lock_guard<std::mutex> lock(g_writer->m);
        g_writer->path = path;
        g_writer->hash = hash;
        g_writer->dir = dir;
    }

    v8::WasmModuleCompilation compilation;
    if (cached) compilation.SetHasCompiledModuleBytes();
    if (!dir.empty()) compilation.SetMoreFunctionsCanBeSerializedCallback([](v8::CompiledWasmModule m) { queue_write(std::move(m)); });
    compilation.OnBytesReceived(bytes, len);

    bool done = false, used_cache = false;
    v8::Global<v8::WasmModuleObject> result;
    v8::Global<v8::Value> failure;
    auto t0 = std::chrono::steady_clock::now();
    // (a caching callback only with SetHasCompiledModuleBytes: V8 insists)
    v8::WasmModuleCompilation::ModuleCachingCallback from_cache;
    if (cached) from_cache = [&](v8::WasmStreaming::ModuleCachingInterface& iface) {
        used_cache = iface.SetCachedCompiledModuleBytes(v8::MemorySpan<const uint8_t>(cached->data(), cached->size()));
    };
    compilation.Finish(g_isolate, from_cache,
        [&](std::variant<v8::Local<v8::WasmModuleObject>, v8::Local<v8::Value>> r) {
            if (r.index() == 0) result.Reset(g_isolate, std::get<0>(r));
            else failure.Reset(g_isolate, std::get<1>(r));
            done = true;
        });
    for (auto start = std::chrono::steady_clock::now(); !done;) {
        pump(host);
        if (done) break;
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(300)) {
            if (error) *error = "compiling the cart took longer than 5 minutes";
            return {};
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (result.IsEmpty()) {
        if (error) {
            v8::String::Utf8Value e(g_isolate, failure.Get(g_isolate));
            *error = *e ? *e : "compile failed";
        }
        return {};
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!dir.empty())
        wc_log("wasmcart: compiled in %.0f ms (%s)\n", ms,
               used_cache ? "from the code cache" : cached ? "the code cache did not fit this V8: recompiled" : "no code cache yet");
    (void)context;
    return result.Get(g_isolate);
}
