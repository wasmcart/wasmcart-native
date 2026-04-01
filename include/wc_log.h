#ifndef WC_LOG_H
#define WC_LOG_H

#include <stdio.h>
#include <stdarg.h>

#ifdef __ANDROID__
#include <android/log.h>
#define WC_LOG_TAG "wasmcart"

static inline void wc_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, WC_LOG_TAG, fmt, args);
    va_end(args);
    // Also write to stderr for non-Android hosts that capture it
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static inline void wc_log_err(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR, WC_LOG_TAG, fmt, args);
    va_end(args);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
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
