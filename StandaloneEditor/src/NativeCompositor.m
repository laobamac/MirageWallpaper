// Mirage Wallpaper
// Copyright © 2026 王孝慈. All rights reserved.

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <mach-o/dyld.h>

// CEF is the only visible GPU surface. Scene swapchains remain available for
// rendering, but their pixels reach the UI exclusively through shared textures.
static IMP originalCreateMetalView;
static char surfaceOrderKey;
static NSUInteger surfaceOrder;
static IMP originalDisplaySync;
static char interfaceLayerKey;

static void setDisplaySync(CAMetalLayer *layer, SEL selector, BOOL enabled) {
    ((void (*)(id, SEL, BOOL))originalDisplaySync)(layer, selector,
        objc_getAssociatedObject(layer, &interfaceLayerKey) ? NO : enabled);
}

static id createMetalView(id owner, SEL selector, id device) {
    NSView *view = ((id (*)(id, SEL, id))originalCreateMetalView)(owner, selector, device);
    NSView *container = owner;
    NSNumber *order = objc_getAssociatedObject(container, &surfaceOrderKey);
    if (!order) {
        order = @(surfaceOrder++);
        objc_setAssociatedObject(container, &surfaceOrderKey, order, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    container.layer.zPosition = 0;
    container.layer.opacity = order.unsignedLongValue ? 0 : 1;
    container.identifier = order.unsignedLongValue ? @"MirageGPUPreview" : @"MirageCEFUI";
    container.layer.masksToBounds = YES;
    container.layer.actions = @{@"position":NSNull.null, @"bounds":NSNull.null,
                                @"sublayers":NSNull.null, @"hidden":NSNull.null};
    [CATransaction commit];
    if (!order.unsignedLongValue && [view.layer isKindOfClass:CAMetalLayer.class]) {
        objc_setAssociatedObject(view.layer, &interfaceLayerKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        view.layer.opaque = YES;
        ((CAMetalLayer *)view.layer).displaySyncEnabled = NO;
    }
    fprintf(stderr, "[Mirage compositor] GPU surface order %lu, size %.0fx%.0f\n",
            order.unsignedLongValue, container.frame.size.width, container.frame.size.height);
    return view;
}

static void install(unsigned attempts) {
    Class content = NSClassFromString(@"WineContentView");
    Method create = content ? class_getInstanceMethod(content, sel_registerName("newMetalViewWithDevice:")) : NULL;
    if (!create) {
        if (attempts < 200) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50*NSEC_PER_MSEC),
            dispatch_get_main_queue(), ^{ install(attempts + 1); });
        return;
    }
    originalCreateMetalView = method_setImplementation(create, (IMP)createMetalView);
    originalDisplaySync = method_setImplementation(class_getInstanceMethod(CAMetalLayer.class,
        @selector(setDisplaySyncEnabled:)), (IMP)setDisplaySync);
}

__attribute__((constructor)) static void initialize(void) {
    char path[4096]; uint32_t capacity = sizeof(path);
    if (_NSGetExecutablePath(path, &capacity) || !strstr(path, "Mirage WE Editor.app/")) return;
    dispatch_async(dispatch_get_main_queue(), ^{ install(0); });
}
