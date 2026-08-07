// mac_layer.h — macOS native-window plumbing for ANGLE's EGL.
//
// ANGLE validates EGLNativeWindowType as a CALayer*, not the NSWindow* SDL's
// SDL_SysWMinfo carries. These resolve the handle and expose the two knobs
// EGL cannot reach on the Metal backend: displaySyncEnabled (real vsync —
// ANGLE accepts eglSwapInterval but never syncs) and contentsScale (stale
// after a drag between displays with different backing scales).

#ifndef WC_MAC_LAYER_H
#define WC_MAC_LAYER_H

#include <stdbool.h>

#ifdef __APPLE__
// NSWindow*/NSView*/CALayer* -> the backing CALayer*, contentsScale matched
// to the window's current display. NULL if the handle is none of those.
void* wc_mac_layer_for_native_window(void* handle);

// Set displaySyncEnabled on the CAMetalLayer ANGLE hangs under our layer.
// Returns false while that layer does not exist yet (ANGLE creates it on its
// first present) — retry after a swap.
bool wc_mac_set_display_sync(void* layer_handle, bool enabled);

// Re-match layer contentsScale to the display the window currently sits on.
// Cheap; call once per swap.
void wc_mac_sync_backing_scale(void* window_handle, void* layer_handle);
#endif

#endif // WC_MAC_LAYER_H
