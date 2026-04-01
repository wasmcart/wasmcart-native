#ifndef WC_LOG_H
#define WC_LOG_H

#include <stdio.h>
#include <stdarg.h>

#ifdef __ANDROID__
#include <android/log.h>
#define WC_LOG_TAG "wasmcart"

static FILE* _wc_log_file = NULL;

static inline void _wc_log_ensure_file(void) {
    if (!_wc_log_file) {
        _wc_log_file = fopen("/storage/emulated/0/racarts/wasmcart.log", "w");
    }
}

static inline void wc_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, WC_LOG_TAG, fmt, args);
    va_end(args);
    _wc_log_ensure_file();
    if (_wc_log_file) {
        va_start(args, fmt);
        vfprintf(_wc_log_file, fmt, args);
        va_end(args);
        fflush(_wc_log_file);
    }
}

static inline void wc_log_err(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR, WC_LOG_TAG, fmt, args);
    va_end(args);
    _wc_log_ensure_file();
    if (_wc_log_file) {
        va_start(args, fmt);
        vfprintf(_wc_log_file, fmt, args);
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
}

static inline void wc_log_err(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

#endif

#endif // WC_LOG_H
