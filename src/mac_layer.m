// mac_layer.m — see mac_layer.h. Direct port of the same fixes proven in
// native-gles 0.6.0: resolve SDL's NSWindow* to the contentView's backing
// CALayer for eglCreateWindowSurface, drive vsync through
// CAMetalLayer.displaySyncEnabled (ANGLE's Metal backend ignores
// eglSwapInterval), and keep contentsScale synced to the window's display.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include "mac_layer.h"

void* wc_mac_layer_for_native_window(void* handle) {
    if (!handle) return NULL;
    if (![NSThread isMainThread]) return NULL;  // AppKit is main-thread-only

    id obj = (id)handle;
    if ([obj isKindOfClass:[CALayer class]]) return handle;

    NSView* view = nil;
    if ([obj isKindOfClass:[NSView class]]) {
        view = (NSView*)obj;
    } else if ([obj isKindOfClass:[NSWindow class]]) {
        view = ((NSWindow*)obj).contentView;
    }
    if (!view) return NULL;

    [view setWantsLayer:YES];
    // ANGLE sizes its CAMetalLayer drawable from this layer's contentsScale,
    // which defaults to 1.0 (points). On Retina that halves the resolution
    // and the picture lands in a quarter of the window.
    CGFloat scale = view.window ? view.window.backingScaleFactor
                                : (NSScreen.mainScreen ? NSScreen.mainScreen.backingScaleFactor : 1.0);
    view.layer.contentsScale = scale;
    return (void*)view.layer;
}

static CAMetalLayer* find_metal_layer(CALayer* layer, int depth) {
    if ([layer isKindOfClass:[CAMetalLayer class]]) return (CAMetalLayer*)layer;
    if (depth <= 0) return nil;
    for (CALayer* sub in layer.sublayers) {
        CAMetalLayer* found = find_metal_layer(sub, depth - 1);
        if (found) return found;
    }
    return nil;
}

bool wc_mac_set_display_sync(void* layer_handle, bool enabled) {
    if (!layer_handle) return false;
    if (![NSThread isMainThread]) return false;
    CAMetalLayer* metal = find_metal_layer((CALayer*)layer_handle, 3);
    if (!metal) return false;
    if (@available(macOS 10.13, *)) {
        metal.displaySyncEnabled = enabled ? YES : NO;
        return true;
    }
    return false;
}

void wc_mac_sync_backing_scale(void* window_handle, void* layer_handle) {
    if (!window_handle || !layer_handle) return;
    if (![NSThread isMainThread]) return;
    id obj = (id)window_handle;
    NSView* view = nil;
    if ([obj isKindOfClass:[NSView class]]) {
        view = (NSView*)obj;
    } else if ([obj isKindOfClass:[NSWindow class]]) {
        view = ((NSWindow*)obj).contentView;
    }
    if (!view || !view.window) return;
    CGFloat scale = view.window.backingScaleFactor;
    CALayer* root = (CALayer*)layer_handle;
    if (root.contentsScale != scale) root.contentsScale = scale;
    CAMetalLayer* metal = find_metal_layer(root, 3);
    if (metal && metal.contentsScale != scale) metal.contentsScale = scale;
}
