#if defined(__APPLE__)

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <QuartzCore/CAMetalLayer.h>

@interface OpenS4kaMetalView : NSView
@end

@implementation OpenS4kaMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (BOOL)wantsUpdateLayer { return YES; }
@end

static NSWindow* OpenS4kaRuntimeWindow(NSApplication* application) {
    NSWindow* window = application.keyWindow ?: application.mainWindow;
    if (window) return window;
    for (NSWindow* candidate in application.windows)
        if (candidate.visible) return candidate;
    return nil;
}

extern "C" void opens4ka_prepare_macos_vulkan_surface(void) {
    @autoreleasepool {
        NSApplication* application = [NSApplication sharedApplication];
        NSWindow* window = OpenS4kaRuntimeWindow(application);
        NSView* content = window.contentView;
        if (!content) return;
        OpenS4kaMetalView* metalView = nil;
        for (NSView* view in content.subviews) {
            if ([view isKindOfClass:[OpenS4kaMetalView class]]) {
                metalView = (OpenS4kaMetalView*)view;
                break;
            }
        }
        if (!metalView) {
            metalView = [[OpenS4kaMetalView alloc] initWithFrame:content.bounds];
            metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            metalView.wantsLayer = YES;
            metalView.layer = [CAMetalLayer layer];
            [content addSubview:metalView positioned:NSWindowBelow relativeTo:nil];
        }
        if (![metalView.layer isKindOfClass:[CAMetalLayer class]]) {
            metalView.wantsLayer = YES;
            metalView.layer = [CAMetalLayer layer];
        }
        CAMetalLayer* layer = (CAMetalLayer*)metalView.layer;
        layer.contentsScale = 1.0;
        layer.drawableSize = content.bounds.size;
    }
}

extern "C" void opens4ka_activate_macos_runtime(void) {
    @autoreleasepool {
        NSApplication* application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        [application finishLaunching];
        [application activateIgnoringOtherApps:YES];
        opens4ka_prepare_macos_vulkan_surface();
    }
}

#endif
