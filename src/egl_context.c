// egl_context.c — EGL context for GL carts in standalone player

#include "egl_context.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static EGLConfig egl_config;
static bool initialized = false;

int egl_create_context(uint32_t width, uint32_t height) {
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "wasmcart: eglGetDisplay failed\n");
        return -1;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        fprintf(stderr, "wasmcart: eglInitialize failed\n");
        return -1;
    }
    fprintf(stderr, "wasmcart: EGL %d.%d\n", major, minor);

    // Request GLES3 context
    EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT | EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs) || num_configs == 0) {
        fprintf(stderr, "wasmcart: eglChooseConfig failed\n");
        return -1;
    }

    EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };

    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, ctx_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "wasmcart: eglCreateContext failed (0x%x)\n", eglGetError());
        return -1;
    }

    // Create a pbuffer surface (will be replaced by window surface later if available)
    EGLint pbuffer_attribs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_NONE
    };

    egl_surface = eglCreatePbufferSurface(egl_display, egl_config, pbuffer_attribs);
    if (egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "wasmcart: eglCreatePbufferSurface failed (0x%x)\n", eglGetError());
        return -1;
    }

    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        fprintf(stderr, "wasmcart: eglMakeCurrent failed\n");
        return -1;
    }

    initialized = true;

    fprintf(stderr, "wasmcart: GL: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "wasmcart: GL version: %s\n", glGetString(GL_VERSION));

    return 0;
}

int egl_create_window_surface(void* native_window) {
    if (!initialized) return -1;

    // Destroy the pbuffer surface
    if (egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display, egl_surface);
    }

    egl_surface = eglCreateWindowSurface(egl_display, egl_config,
        (EGLNativeWindowType)native_window, NULL);
    if (egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "wasmcart: eglCreateWindowSurface failed (0x%x)\n", eglGetError());
        return -1;
    }

    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    return 0;
}

void egl_make_current(void) {
    if (initialized) {
        eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    }
}

void egl_swap_buffers(void) {
    if (initialized) {
        eglSwapBuffers(egl_display, egl_surface);
    }
}

void egl_destroy(void) {
    if (!initialized) return;
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_surface != EGL_NO_SURFACE) eglDestroySurface(egl_display, egl_surface);
    if (egl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, egl_context);
    if (egl_display != EGL_NO_DISPLAY) eglTerminate(egl_display);
    initialized = false;
}

void* egl_get_proc_address(const char* name) {
    return (void*)eglGetProcAddress(name);
}

bool egl_is_initialized(void) {
    return initialized;
}

EGLDisplay egl_get_display(void) {
    return egl_display;
}
