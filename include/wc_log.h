#ifndef WC_LOG_H
#define WC_LOG_H

#include <stdio.h>
#include <stdarg.h>

// Shared log file — defined in one translation unit, extern everywhere else.
// On Android, logs go to both logcat and a file (logcat buffer is tiny).
// On other platforms, logs go to stderr only.

extern FILE* _wc_log_file;
extern long _wc_log_bytes;
#define WC_LOG_MAX_BYTES (1L * 1024 * 1024)  // 1MB cap

static inline void wc_log_set_file(const char* path) {
    if (_wc_log_file) { fclose(_wc_log_file); _wc_log_file = NULL; }
    if (path) {
        _wc_log_file = fopen(path, "w");
        _wc_log_bytes = 0;
    }
}

#ifdef __ANDROID__
#include <android/log.h>
#define WC_LOG_TAG "wasmcart"

static inline void wc_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, WC_LOG_TAG, fmt, args);
    va_end(args);
    if (_wc_log_file && _wc_log_bytes < WC_LOG_MAX_BYTES) {
        va_start(args, fmt);
        _wc_log_bytes += vfprintf(_wc_log_file, fmt, args);
        va_end(args);
        fflush(_wc_log_file);
    }
}

static inline void wc_log_err(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR, WC_LOG_TAG, fmt, args);
    va_end(args);
    if (_wc_log_file && _wc_log_bytes < WC_LOG_MAX_BYTES) {
        va_start(args, fmt);
        _wc_log_bytes += vfprintf(_wc_log_file, fmt, args);
        va_end(args);
        fflush(_wc_log_file);
    }
}

#else

static inline void wc_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (_wc_log_file && _wc_log_bytes < WC_LOG_MAX_BYTES) {
        va_start(args, fmt);
        _wc_log_bytes += vfprintf(_wc_log_file, fmt, args);
        va_end(args);
        fflush(_wc_log_file);
    }
}

static inline void wc_log_err(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (_wc_log_file && _wc_log_bytes < WC_LOG_MAX_BYTES) {
        va_start(args, fmt);
        _wc_log_bytes += vfprintf(_wc_log_file, fmt, args);
        va_end(args);
        fflush(_wc_log_file);
    }
}

#endif

#endif // WC_LOG_H
