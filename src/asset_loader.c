// asset_loader.c — .wasc ZIP archive reading and manifest parsing

#include "cart_host.h"

#include "../deps/miniz.h"
#include "../deps/cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uv.h>
#include "wc_log.h"

// ─── Archive open/close ────────────────────────────────────────────────────

static int read_file(const char* path, uint8_t** out, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = malloc(size);
    if (!data) { fclose(f); return -1; }
    fread(data, 1, size, f);
    fclose(f);
    *out = data;
    *out_len = (size_t)size;
    return 0;
}

int wc_archive_open(wc_host_t* host, const char* path) {
    size_t len = strlen(path);
    bool is_wasc = (len > 5 && strcmp(path + len - 5, ".wasc") == 0);

    if (!is_wasc) {
        // Bare .wasm file
        if (read_file(path, &host->wasm_bytes, &host->wasm_bytes_len) != 0) return -1;

        // Default manifest
        const char* basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        strncpy(host->manifest.name, basename, sizeof(host->manifest.name) - 1);
        host->manifest.abi = WC_ABI_VERSION;
        strncpy(host->manifest.entry, "cart.wasm", sizeof(host->manifest.entry) - 1);
        host->manifest.players = 1;
        return 0;
    }

    // .wasc ZIP archive — open from file (no need to load entire ZIP into RAM)
    mz_zip_archive* zip = calloc(1, sizeof(mz_zip_archive));
    if (!mz_zip_reader_init_file(zip, path, 0)) {
        wc_log("wasmcart: failed to open ZIP: %s\n", path);
        free(zip);
        return -1;
    }
    host->archive = zip;
    host->archive_path = strdup(path);

    // Read manifest.json
    int idx = mz_zip_reader_locate_file(zip, "manifest.json", NULL, 0);
    if (idx >= 0) {
        size_t json_size;
        char* json = mz_zip_reader_extract_to_heap(zip, idx, &json_size, 0);
        if (json) {
            wc_parse_manifest(host, json, json_size);
            free(json);
        }
    } else {
        // No manifest — use defaults
        const char* basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        strncpy(host->manifest.name, basename, sizeof(host->manifest.name) - 1);
        host->manifest.abi = WC_ABI_VERSION;
        strncpy(host->manifest.entry, "cart.wasm", sizeof(host->manifest.entry) - 1);
        host->manifest.players = 1;
    }

    // Read cart.wasm (or manifest entry)
    const char* entry = host->manifest.entry[0] ? host->manifest.entry : "cart.wasm";
    idx = mz_zip_reader_locate_file(zip, entry, NULL, 0);
    if (idx < 0) {
        wc_log("wasmcart: entry '%s' not found in archive\n", entry);
        mz_zip_reader_end(zip);
        free(zip);
        host->archive = NULL;
        return -1;
    }

    host->wasm_bytes = mz_zip_reader_extract_to_heap(zip, idx, &host->wasm_bytes_len, 0);
    if (!host->wasm_bytes) {
        wc_log("wasmcart: failed to extract %s\n", entry);
        mz_zip_reader_end(zip);
        free(zip);
        host->archive = NULL;
        return -1;
    }

    return 0;
}

int wc_archive_open_memory(wc_host_t* host, const uint8_t* data, size_t len) {
    mz_zip_archive* zip = calloc(1, sizeof(mz_zip_archive));
    if (!mz_zip_reader_init_mem(zip, data, len, 0)) {
        free(zip);
        return -1;
    }
    host->archive = zip;
    host->archive_mem = data;
    host->archive_mem_len = len;

    // Same flow as above for manifest + wasm extraction
    int idx = mz_zip_reader_locate_file(zip, "manifest.json", NULL, 0);
    if (idx >= 0) {
        size_t json_size;
        char* json = mz_zip_reader_extract_to_heap(zip, idx, &json_size, 0);
        if (json) { wc_parse_manifest(host, json, json_size); free(json); }
    }

    const char* entry = host->manifest.entry[0] ? host->manifest.entry : "cart.wasm";
    idx = mz_zip_reader_locate_file(zip, entry, NULL, 0);
    if (idx < 0) return -1;
    host->wasm_bytes = mz_zip_reader_extract_to_heap(zip, idx, &host->wasm_bytes_len, 0);
    return host->wasm_bytes ? 0 : -1;
}

void wc_archive_close(wc_host_t* host) {
    wc_prefetch_close(host);
    free(host->archive_path);
    host->archive_path = NULL;
    host->archive_mem = NULL;
    host->archive_mem_len = 0;
    if (host->file_list) {
        free(host->file_list);
        host->file_list = NULL;
        host->file_list_len = 0;
    }
    if (host->archive) {
        mz_zip_reader_end((mz_zip_archive*)host->archive);
        free(host->archive);
        host->archive = NULL;
    }
    if (host->wasm_bytes) {
        free(host->wasm_bytes);
        host->wasm_bytes = NULL;
        host->wasm_bytes_len = 0;
    }
}

// ─── Asset loading ─────────────────────────────────────────────────────────

/*
 * Resolve a cart-relative asset path to a ZIP entry index.
 *
 * A cart says "roms/game.prg"; the archive may hold it bare, under the
 * manifest's own asset root ("app/roms/game.prg"), or under the legacy
 * "assets/" the packer uses when it generates a manifest itself. Try the
 * declared root first so a cart that ships both spellings gets its own.
 */
static int locate_asset(mz_zip_archive* zip, const wc_manifest_t* man, const char* path) {
    char buf[512];
    if (man->assets[0]) {
        snprintf(buf, sizeof(buf), "%s%s", man->assets, path);
        int idx = mz_zip_reader_locate_file(zip, buf, NULL, 0);
        if (idx >= 0) return idx;
    }
    int idx = mz_zip_reader_locate_file(zip, path, NULL, 0);
    if (idx >= 0) return idx;
    snprintf(buf, sizeof(buf), "assets/%s", path);
    return mz_zip_reader_locate_file(zip, buf, NULL, 0);
}

/*
 * The virtual "_filelist.txt": every asset path, newline separated, so a cart
 * can enumerate what it shipped with. The ABI has no directory call, so this
 * is how a ROM picker or a bezel picker finds its choices.
 *
 * Names are emitted ONCE and with the manifest's asset root stripped, i.e.
 * exactly the spelling a cart passes back to wc_load_asset. The JS host does
 * the same; a cart must not see a different list depending on which host ran
 * it. manifest.json and the wasm entry are not assets and are left out.
 *
 * Built on demand and cached: most carts never ask.
 */
static const char* build_file_list(wc_host_t* host, uint32_t* out_len) {
    if (host->file_list) { *out_len = host->file_list_len; return host->file_list; }
    if (!host->archive) return NULL;
    mz_zip_archive* zip = (mz_zip_archive*)host->archive;

    const char* root = host->manifest.assets;
    size_t rootlen = root[0] ? strlen(root) : 0;
    const char* entry = host->manifest.entry[0] ? host->manifest.entry : "cart.wasm";

    mz_uint n = mz_zip_reader_get_num_files(zip);
    size_t cap = 4096, len = 0;
    char* buf = malloc(cap);
    if (!buf) return NULL;

    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(zip, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(zip, i)) continue;
        const char* name = st.m_filename;
        if (strcmp(name, "manifest.json") == 0 || strcmp(name, entry) == 0) continue;
        if (rootlen && strncmp(name, root, rootlen) == 0) name += rootlen;
        else if (strncmp(name, "assets/", 7) == 0) name += 7;
        if (!name[0]) continue;

        size_t nl = strlen(name);
        if (len + nl + 2 > cap) {
            while (len + nl + 2 > cap) cap *= 2;
            char* grown = realloc(buf, cap);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
        }
        if (len) buf[len++] = '\n';
        memcpy(buf + len, name, nl);
        len += nl;
    }
    buf[len] = 0;
    host->file_list = buf;
    host->file_list_len = (uint32_t)len;
    *out_len = host->file_list_len;
    return host->file_list;
}

// ─── Asset prefetch ────────────────────────────────────────────────────────
//
// wasmcart SPEC.md, "Asset prefetch". The cart names an asset it will read
// soon; the host extracts it on libuv's threadpool into a buffer of its own
// and the cart's next wc_load_asset of that path is a memcpy. The table is
// keyed by path, the call is also the poll, and the whole state machine runs
// on the main thread: a job never touches the host, only its own reader (one
// mz_zip_archive is not safe to share between threads) and its own buffer.
//
// A state the cart can see changes only in wc_prefetch_settle, which the frame
// calls before wc_render: a job landing in the middle of a cart call (a uv_run
// inside a WebGPU wait) marks the entry DONE, which still polls as PENDING.

enum { PF_QUEUED, PF_INFLIGHT, PF_DONE, PF_READY, PF_FAILED };

// The floors the SPEC sets are 64 pending or ready at once and one asset of
// the maximum size (256 MiB); these are above both.
#define PF_MAX_ENTRIES   256
#define PF_MAX_INFLIGHT  4              // libuv's default threadpool
#define PF_CACHE_BYTES   (512u << 20)   // READY copies plus the fetches in flight

typedef struct wc_pf_entry wc_pf_entry;

typedef struct {
    uv_work_t       req;
    wc_pf_entry*    entry;     // NULL once the entry was dropped under it
    // the job's own view of the archive: no host state
    char*           zip_path;
    const uint8_t*  zip_mem;
    size_t          zip_mem_len;
    int             idx;
    uint32_t        size;
    uint8_t*        buf;
    int             ok;
} wc_pf_job;

struct wc_pf_entry {
    char*     path;
    int       state;
    int32_t   priority;
    bool      cancelled;   // in flight: dropped when the job lands
    bool      failed;      // DONE: the job did not deliver the bytes
    int       idx;
    uint32_t  size;
    uint8_t*  data;        // READY: the bytes
    wc_pf_job* job;
};

typedef struct {
    wc_host_t*    host;      // NULL once closed: a job still landing touches only the table
    wc_pf_entry** entries;
    uint32_t      count;
    uint32_t      cap;
    uint32_t      inflight;
    size_t        held;      // bytes reserved: READY copies and fetches in flight
    uv_loop_t*    loop;
} wc_prefetch_t;

static wc_prefetch_t* pf_table(wc_host_t* host, uv_loop_t* loop) {
    if (!host->prefetch) {
        wc_prefetch_t* pf = calloc(1, sizeof(wc_prefetch_t));
        if (!pf) return NULL;
        pf->host = host;
        pf->loop = loop;
        host->prefetch = pf;
    }
    return (wc_prefetch_t*)host->prefetch;
}

static wc_pf_entry* pf_find(wc_prefetch_t* pf, const char* path) {
    for (uint32_t i = 0; i < pf->count; i++)
        if (strcmp(pf->entries[i]->path, path) == 0) return pf->entries[i];
    return NULL;
}

static void pf_remove(wc_prefetch_t* pf, wc_pf_entry* e) {
    for (uint32_t i = 0; i < pf->count; i++) {
        if (pf->entries[i] != e) continue;
        pf->entries[i] = pf->entries[--pf->count];
        break;
    }
    if (e->job) e->job->entry = NULL;   // the job lands on its own and releases the bytes
    else if (!e->failed) pf->held -= e->size;
    free(e->data);
    free(e->path);
    free(e);
}

static void pf_work(uv_work_t* req) {
    wc_pf_job* job = (wc_pf_job*)req;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    mz_bool opened = job->zip_path ? mz_zip_reader_init_file(&zip, job->zip_path, 0)
                                   : mz_zip_reader_init_mem(&zip, job->zip_mem, job->zip_mem_len, 0);
    if (!opened) { job->ok = 0; return; }
    mz_zip_archive_file_stat st;
    job->ok = mz_zip_reader_file_stat(&zip, job->idx, &st) && st.m_uncomp_size == job->size &&
              (job->size == 0 || mz_zip_reader_extract_to_mem(&zip, job->idx, job->buf, job->size, 0));
    mz_zip_reader_end(&zip);
}

static void pf_start_queued(wc_prefetch_t* pf);
static void pf_free_table(wc_prefetch_t* pf);

static void pf_after(uv_work_t* req, int status) {
    wc_pf_job* job = (wc_pf_job*)req;
    wc_prefetch_t* pf = (wc_prefetch_t*)req->data;
    wc_pf_entry* e = job->entry;
    pf->inflight--;
    if (!e || e->cancelled) {
        // nobody wants the bytes any more
        if (e) pf_remove(pf, e);     // pf_remove unlinks the job first, so the size is ours to release
        pf->held -= job->size;
        free(job->buf);
    } else if (status != 0) {
        // uv_cancel took it before the work ran (a cancel), and the cart has
        // asked again since: start over, the bytes are still owed
        e->job = NULL;
        e->state = PF_QUEUED;
        free(job->buf);
    } else {
        e->job = NULL;
        e->state = PF_DONE;
        if (job->ok) {
            e->data = job->buf;
        } else {
            e->failed = true;
            pf->held -= job->size;
            free(job->buf);
            wc_log("wasmcart: prefetch of asset %s failed\n", e->path);
        }
    }
    free(job->zip_path);
    free(job);
    if (!pf->host) {
        // closed while this was in flight: the last job frees the table
        if (pf->inflight == 0) pf_free_table(pf);
        return;
    }
    pf_start_queued(pf);
}

// Start the highest-priority queued fetches up to the concurrency limit.
static void pf_start_queued(wc_prefetch_t* pf) {
    wc_host_t* host = pf->host;
    while (pf->inflight < PF_MAX_INFLIGHT) {
        wc_pf_entry* best = NULL;
        for (uint32_t i = 0; i < pf->count; i++) {
            wc_pf_entry* e = pf->entries[i];
            if (e->state == PF_QUEUED && (!best || e->priority > best->priority)) best = e;
        }
        if (!best) return;
        wc_pf_job* job = calloc(1, sizeof(wc_pf_job));
        uint8_t* buf = job ? malloc(best->size ? best->size : 1) : NULL;
        if (!buf) {
            free(job);
            best->state = PF_DONE;
            best->failed = true;
            pf->held -= best->size;
            continue;
        }
        job->req.data = pf;
        job->entry = best;
        job->zip_path = host->archive_path ? strdup(host->archive_path) : NULL;
        job->zip_mem = host->archive_mem;
        job->zip_mem_len = host->archive_mem_len;
        job->idx = best->idx;
        job->size = best->size;
        job->buf = buf;
        if (uv_queue_work(pf->loop, &job->req, pf_work, pf_after) != 0) {
            free(buf);
            free(job->zip_path);
            free(job);
            best->state = PF_DONE;
            best->failed = true;
            pf->held -= best->size;
            continue;
        }
        best->job = job;
        best->state = PF_INFLIGHT;
        pf->inflight++;
    }
}

static int32_t pf_state(const wc_pf_entry* e) {
    switch (e->state) {
    case PF_READY:  return WC_PREFETCH_READY;
    case PF_FAILED: return WC_PREFETCH_FAILED;
    default:        return WC_PREFETCH_PENDING;   // queued, in flight, or landed but not settled
    }
}

int32_t wc_prefetch_request(wc_host_t* host, void* uv_loop, const char* path, int32_t priority) {
    if (!host->archive) return WC_PREFETCH_MISSING;   // a bare .wasm has no assets
    mz_zip_archive* zip = (mz_zip_archive*)host->archive;
    wc_prefetch_t* pf = pf_table(host, (uv_loop_t*)uv_loop);
    if (!pf) return WC_PREFETCH_REFUSED;

    wc_pf_entry* e = pf_find(pf, path);
    if (e) {
        // the poll; a repeat asks again for a cancelled fetch still in flight,
        // and may raise the priority (never lower it)
        if (priority > e->priority) e->priority = priority;
        if (e->cancelled) e->cancelled = false;
        return pf_state(e);
    }

    int idx = -1;
    uint32_t size = 0;
    const char* at_once = NULL;   // bytes a host may copy inside the call
    if (strcmp(path, "_filelist.txt") == 0) {
        uint32_t flen = 0;
        at_once = build_file_list(host, &flen);
        if (!at_once) return WC_PREFETCH_MISSING;
        size = flen;
    } else {
        idx = locate_asset(zip, &host->manifest, path);
        if (idx < 0) return WC_PREFETCH_MISSING;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(zip, idx, &st)) return WC_PREFETCH_MISSING;
        if (st.m_uncomp_size > 0x7fffffffu) return WC_PREFETCH_MISSING;   // as wc_asset_size could not return it
        size = (uint32_t)st.m_uncomp_size;
    }

    if (pf->count >= PF_MAX_ENTRIES) return WC_PREFETCH_REFUSED;
    bool fits_ever = size <= PF_CACHE_BYTES;
    if (fits_ever && pf->held + size > PF_CACHE_BYTES && pf->held > 0) return WC_PREFETCH_REFUSED;
    if (pf->count == pf->cap) {
        uint32_t cap = pf->cap ? pf->cap * 2 : 16;
        wc_pf_entry** grown = realloc(pf->entries, cap * sizeof(*grown));
        if (!grown) return WC_PREFETCH_REFUSED;
        pf->entries = grown;
        pf->cap = cap;
    }
    e = calloc(1, sizeof(wc_pf_entry));
    if (!e) return WC_PREFETCH_REFUSED;
    e->path = strdup(path);
    if (!e->path) { free(e); return WC_PREFETCH_REFUSED; }
    e->priority = priority;
    e->idx = idx;
    e->size = size;
    pf->entries[pf->count++] = e;

    if (!fits_ever) {
        // the host could not hold the bytes: FAILED until the cart cancels
        e->state = PF_FAILED;
        e->failed = true;
        return WC_PREFETCH_FAILED;
    }
    pf->held += size;
    if (at_once) {
        e->data = malloc(size ? size : 1);
        if (!e->data) { pf_remove(pf, e); return WC_PREFETCH_REFUSED; }
        memcpy(e->data, at_once, size);
        e->state = PF_READY;
        return WC_PREFETCH_READY;
    }
    e->state = PF_QUEUED;
    pf_start_queued(pf);
    return WC_PREFETCH_PENDING;
}

void wc_prefetch_cancel(wc_host_t* host, const char* path) {
    wc_prefetch_t* pf = (wc_prefetch_t*)host->prefetch;
    if (!pf) return;
    wc_pf_entry* e = pf_find(pf, path);
    if (!e) return;
    if (e->state == PF_INFLIGHT) {
        // the transfer finishes on its own; the bytes are dropped when it lands
        e->cancelled = true;
        uv_cancel((uv_req_t*)&e->job->req);   // succeeds only if the work has not started
        return;
    }
    pf_remove(pf, e);
}

void wc_prefetch_settle(wc_host_t* host) {
    wc_prefetch_t* pf = (wc_prefetch_t*)host->prefetch;
    if (!pf) return;
    for (uint32_t i = 0; i < pf->count; i++) {
        wc_pf_entry* e = pf->entries[i];
        if (e->state != PF_DONE) continue;
        e->state = e->failed ? PF_FAILED : PF_READY;
        static int _done = 0;
        if (_done < 5 && !e->failed) { wc_log("wasmcart: prefetched asset %s (%u bytes)\n", e->path, e->size); _done++; }
    }
}

static void pf_free_table(wc_prefetch_t* pf) {
    free(pf->entries);
    free(pf);
}

uint32_t wc_prefetch_inflight(wc_host_t* host) {
    wc_prefetch_t* pf = (wc_prefetch_t*)host->prefetch;
    return pf ? pf->inflight : 0;
}

void wc_prefetch_close(wc_host_t* host) {
    wc_prefetch_t* pf = (wc_prefetch_t*)host->prefetch;
    if (!pf) return;
    while (pf->count) {
        wc_pf_entry* e = pf->entries[pf->count - 1];
        if (e->job) uv_cancel((uv_req_t*)&e->job->req);   // not started yet: lands at once as cancelled
        pf_remove(pf, e);
    }
    host->prefetch = NULL;
    pf->host = NULL;
    // A job in flight holds a reader of its own and a buffer of its own and
    // touches nothing else; the last one to land frees the table (pf_after).
    // wc_host_destroy pumps the loop until none is in flight before it gets
    // here, so normally that is now.
    if (pf->inflight == 0) pf_free_table(pf);
}

// A READY copy the reads answer from, or NULL.
static wc_pf_entry* pf_ready(wc_host_t* host, const char* path) {
    wc_prefetch_t* pf = (wc_prefetch_t*)host->prefetch;
    if (!pf) return NULL;
    wc_pf_entry* e = pf_find(pf, path);
    return e && e->state == PF_READY ? e : NULL;
}

int32_t wc_archive_asset_size(wc_host_t* host, const char* path) {
    if (!host->archive) return -1;
    mz_zip_archive* zip = (mz_zip_archive*)host->archive;

    wc_pf_entry* ready = pf_ready(host, path);
    if (ready) return (int32_t)ready->size;

    if (strcmp(path, "_filelist.txt") == 0) {
        uint32_t flen = 0;
        return build_file_list(host, &flen) ? (int32_t)flen : -1;
    }

    int idx = locate_asset(zip, &host->manifest, path);
    if (idx < 0) return -1;

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(zip, idx, &stat)) return -1;
    return (int32_t)stat.m_uncomp_size;
}

int32_t wc_archive_load_asset(wc_host_t* host, const char* path, uint8_t* dest, uint32_t max_size) {
    if (!host->archive) return -1;
    mz_zip_archive* zip = (mz_zip_archive*)host->archive;

    wc_pf_entry* ready = pf_ready(host, path);
    if (ready) {
        // The copy the prefetch promised: no archive, no inflate. A whole read
        // ends the prefetch (the cart has its own copy now); a truncated one
        // keeps it, as the SPEC's "returns the asset's full size" says.
        uint32_t n = ready->size;
        if (n > max_size) {
            wc_log("wasmcart: asset %s: size %u > max %u, truncating\n", path, n, max_size);
            n = max_size;
        }
        memcpy(dest, ready->data, n);
        static int _served = 0;
        if (_served < 5) { wc_log("wasmcart: asset %s served from prefetch (%u bytes)\n", path, n); _served++; }
        if (n == ready->size) pf_remove((wc_prefetch_t*)host->prefetch, ready);
        return (int32_t)n;
    }

    if (strcmp(path, "_filelist.txt") == 0) {
        uint32_t flen = 0;
        const char* list = build_file_list(host, &flen);
        if (!list) return -1;
        if (flen > max_size) return -1;
        memcpy(dest, list, flen);
        return (int32_t)flen;
    }

    int idx = locate_asset(zip, &host->manifest, path);
    if (idx < 0) {
        static int _miss = 0;
        if (_miss < 3) { wc_log( "wasmcart: asset not found: %s\n", path); _miss++; }
        return -1;
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(zip, idx, &stat)) return -1;

    uint32_t read_size = (uint32_t)stat.m_uncomp_size;
    if (read_size > max_size) {
        wc_log( "wasmcart: asset %s: size %u > max %u, truncating\n", path, read_size, max_size);
        read_size = max_size;
    }

    if (!mz_zip_reader_extract_to_mem(zip, idx, dest, read_size, 0)) {
        wc_log( "wasmcart: asset %s: extract failed\n", path);
        return -1;
    }
    static int _load = 0;
    if (_load < 5) { wc_log( "wasmcart: loaded asset %s (%u bytes, max_size=%u, uncomp=%u)\n", path, read_size, max_size, (uint32_t)stat.m_uncomp_size); _load++; }
    return (int32_t)read_size;
}

// ─── Manifest parsing ──────────────────────────────────────────────────────

int wc_parse_manifest(wc_host_t* host, const char* json, size_t len) {
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root) {
        wc_log( "wasmcart: failed to parse manifest.json\n");
        return -1;
    }

    cJSON* item;

    item = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(item))
        strncpy(host->manifest.name, item->valuestring, sizeof(host->manifest.name) - 1);

    item = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsString(item))
        strncpy(host->manifest.version, item->valuestring, sizeof(host->manifest.version) - 1);

    item = cJSON_GetObjectItem(root, "abi");
    host->manifest.abi = cJSON_IsNumber(item) ? item->valueint : 2;

    item = cJSON_GetObjectItem(root, "entry");
    if (cJSON_IsString(item))
        strncpy(host->manifest.entry, item->valuestring, sizeof(host->manifest.entry) - 1);
    else
        strncpy(host->manifest.entry, "cart.wasm", sizeof(host->manifest.entry) - 1);

    /*
     * Asset root. The packer writes assets under whatever prefix the cart's
     * manifest declares ("app/" for the emulator carts), so resolving against
     * a hardcoded "assets/" finds nothing. Normalize to a single trailing
     * slash so the lookups can concatenate blindly.
     */
    item = cJSON_GetObjectItem(root, "assets");
    if (cJSON_IsString(item) && item->valuestring[0]) {
        strncpy(host->manifest.assets, item->valuestring, sizeof(host->manifest.assets) - 2);
        size_t n = strlen(host->manifest.assets);
        if (n && host->manifest.assets[n - 1] != '/') {
            host->manifest.assets[n] = '/';
            host->manifest.assets[n + 1] = 0;
        }
    }

    item = cJSON_GetObjectItem(root, "width");
    host->manifest.width = cJSON_IsNumber(item) && item->valueint > 0 ? (uint32_t)item->valueint : 0;
    item = cJSON_GetObjectItem(root, "height");
    host->manifest.height = cJSON_IsNumber(item) && item->valueint > 0 ? (uint32_t)item->valueint : 0;

    item = cJSON_GetObjectItem(root, "players");
    host->manifest.players = cJSON_IsNumber(item) ? item->valueint : 1;

    item = cJSON_GetObjectItem(root, "pointer");
    host->manifest.pointer = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "keyboard");
    host->manifest.keyboard = cJSON_IsTrue(item);

    cJSON* controls = cJSON_GetObjectItem(root, "controls");
    if (cJSON_IsArray(controls)) {
        // Presentation hint (SPEC: Manifest > Fields). Unknown tokens are
        // ignored by rule, so new tokens never break old hosts.
        static const struct { const char* token; uint32_t bit; } ctrl_map[] = {
            {"dpad", WC_CTRL_DPAD}, {"a", WC_CTRL_A}, {"b", WC_CTRL_B},
            {"x", WC_CTRL_X}, {"y", WC_CTRL_Y}, {"l", WC_CTRL_L},
            {"r", WC_CTRL_R}, {"start", WC_CTRL_START},
            {"select", WC_CTRL_SELECT}, {"left_stick", WC_CTRL_LSTICK},
            {"right_stick", WC_CTRL_RSTICK}, {"left_trigger", WC_CTRL_LTRIG},
            {"right_trigger", WC_CTRL_RTRIG}, {"l3", WC_CTRL_L3},
            {"r3", WC_CTRL_R3},
        };
        host->manifest.controls = 0;
        host->manifest.controls_set = true;
        cJSON* tok;
        cJSON_ArrayForEach(tok, controls) {
            if (!cJSON_IsString(tok)) continue;
            for (size_t i = 0; i < sizeof(ctrl_map) / sizeof(ctrl_map[0]); i++) {
                if (strcmp(tok->valuestring, ctrl_map[i].token) == 0) {
                    host->manifest.controls |= ctrl_map[i].bit;
                    break;
                }
            }
        }
    }

    cJSON* net = cJSON_GetObjectItem(root, "net");
    if (cJSON_IsObject(net)) {
        // The presence of `net` at all is half the gate: wc_peer_open refuses
        // without it even if the cart set WC_FLAG_NET_PEER.
        host->manifest.has_net = true;

        // net.domains is the current spelling; net.websocket is the superseded
        // one, still read so manifests written before the wc_peer_* merge keep
        // working. Same precedence as the JS host.
        cJSON* domains = cJSON_GetObjectItem(net, "domains");
        if (!cJSON_IsArray(domains)) domains = cJSON_GetObjectItem(net, "websocket");
        if (cJSON_IsArray(domains)) {
            host->manifest.websocket = true;
            host->manifest.ws_domain_count = 0;
            cJSON* domain;
            cJSON_ArrayForEach(domain, domains) {
                if (cJSON_IsString(domain) && host->manifest.ws_domain_count < 8) {
                    strncpy(host->manifest.ws_domains[host->manifest.ws_domain_count],
                            domain->valuestring, 255);
                    host->manifest.ws_domains[host->manifest.ws_domain_count][255] = 0;
                    host->manifest.ws_domain_count++;
                }
            }
        }
        item = cJSON_GetObjectItem(net, "data-channel");
        host->manifest.data_channel = cJSON_IsTrue(item);
    }

    cJSON_Delete(root);
    return 0;
}
