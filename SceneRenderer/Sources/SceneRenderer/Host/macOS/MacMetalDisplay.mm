#if defined(SCENERENDERER_MACMETALDISPLAY_WITH_GLFW)
#    define GLFW_EXPOSE_NATIVE_COCOA
#    include <GLFW/glfw3.h>
#    include <GLFW/glfw3native.h>
#endif

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cmath>
#include <cstdint>
#include <os/log.h>

namespace
{

struct MacMetalDisplay {
    CAMetalLayer*             layer { nil };
    NSView*                   view { nil };
    id<MTLDevice>             device { nil };
    id<MTLCommandQueue>       queue { nil };
    id<MTLRenderPipelineState> pipeline { nil };
    id<MTLSamplerState>       sampler { nil };
    dispatch_group_t          warmup_group { nil };
    CGSize                    fixed_drawable_size { 0.0, 0.0 };
    CGSize                    logged_corrected_bounds_size { 0.0, 0.0 };
    CGSize                    logged_view_size { 0.0, 0.0 };
    CGSize                    logged_layer_size { 0.0, 0.0 };
    CGSize                    logged_drawable_size { 0.0, 0.0 };
    std::uint32_t             logged_source_width { 0 };
    std::uint32_t             logged_source_height { 0 };
};

NSString* ShaderSource() {
    return @"#include <metal_stdlib>\n"
            "using namespace metal;\n"
            "struct VSOut { float4 position [[position]]; float2 uv; };\n"
            "vertex VSOut vs_main(uint vid [[vertex_id]]) {\n"
            "    float2 pos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
            "    float2 uv[3]  = { float2(0.0, 1.0), float2(2.0, 1.0), float2(0.0, -1.0) };\n"
            "    VSOut o;\n"
            "    o.position = float4(pos[vid], 0.0, 1.0);\n"
            "    o.uv = uv[vid];\n"
            "    return o;\n"
            "}\n"
            "fragment float4 fs_main(VSOut in [[stage_in]],\n"
            "                        texture2d<float> tex [[texture(0)]],\n"
            "                        sampler smp [[sampler(0)]]) {\n"
            "    return tex.sample(smp, in.uv);\n"
            "}\n";
}

bool BuildPipeline(MacMetalDisplay* display, id<MTLDevice> device) {
    if (display == nullptr || device == nil) return false;
    display->pipeline = nil;
    display->sampler  = nil;
    display->queue    = nil;

    display->device = device;

    display->queue = [device newCommandQueue];
    if (display->queue == nil) return false;

    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:ShaderSource() options:nil error:&error];
    if (library == nil) {
        NSLog(@"SceneRenderer Metal display shader compile failed: %@", error);
        return false;
    }

    id<MTLFunction> vs = [library newFunctionWithName:@"vs_main"];
    id<MTLFunction> fs = [library newFunctionWithName:@"fs_main"];
    if (vs == nil || fs == nil) return false;

    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vs;
    desc.fragmentFunction = fs;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    display->pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (display->pipeline == nil) {
        NSLog(@"SceneRenderer Metal display pipeline creation failed: %@", error);
        return false;
    }

    MTLSamplerDescriptor* sampler_desc = [[MTLSamplerDescriptor alloc] init];
    sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    display->sampler = [device newSamplerStateWithDescriptor:sampler_desc];
    return display->sampler != nil;
}

bool EnsurePipeline(MacMetalDisplay* display, id<MTLTexture> source_texture) {
    if (display == nullptr || source_texture == nil) return false;
    if (display->warmup_group != nil) {
        dispatch_group_wait(display->warmup_group, DISPATCH_TIME_FOREVER);
        display->warmup_group = nil;
    }
    id<MTLDevice> device = source_texture.device;
    if (device == nil) return false;
    display->layer.device = device;
    display->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    display->layer.framebufferOnly = YES;
    display->layer.opaque = YES;
    if (display->pipeline != nil && display->device == device) return true;
    return BuildPipeline(display, device);
}

void UpdateDrawableSize(MacMetalDisplay* display) {
    if (display == nullptr || display->layer == nil) return;
    NSView* view = display->view;
    if (view == nil) return;

    const bool has_fixed_drawable = display->fixed_drawable_size.width > 0.0 &&
                                    display->fixed_drawable_size.height > 0.0;
    if (has_fixed_drawable) {
        // On macOS 26 the legacy screen-saver ViewBridge can replace a full-screen
        // view's logical (point) bounds with the display's physical pixel size
        // after initialization. That makes the hosted layer two times too large
        // in both axes on a Retina display, so only one quarter is visible. The
        // host may repeat this mutation after layout; normalize on every present,
        // not only when the saver is created. Preview instances intentionally use
        // the dynamic path below and are never constrained to a display size.
        NSScreen* screen = view.window.screen ?: NSScreen.mainScreen;
        const CGFloat backing_scale = screen.backingScaleFactor;
        if (backing_scale > 0.0) {
            const CGSize expected_bounds = CGSizeMake(
                display->fixed_drawable_size.width / backing_scale,
                display->fixed_drawable_size.height / backing_scale);
            NSRect normalized_bounds = view.bounds;
            const CGSize reported_bounds = normalized_bounds.size;
            if (expected_bounds.width > 0.0 && expected_bounds.height > 0.0 &&
                (std::fabs(reported_bounds.width - expected_bounds.width) > 0.5 ||
                 std::fabs(reported_bounds.height - expected_bounds.height) > 0.5)) {
                normalized_bounds.size = expected_bounds;
                view.bounds = normalized_bounds;
                if (! CGSizeEqualToSize(reported_bounds,
                                        display->logged_corrected_bounds_size)) {
                    display->logged_corrected_bounds_size = reported_bounds;
                    static os_log_t logger =
                        os_log_create("cn.laobamac.Mirage.ScreenSaver", "MetalDisplay");
                    os_log_info(logger,
                                "Corrected host bounds %{public}.0fx%{public}.0f to "
                                "%{public}.0fx%{public}.0f (drawable "
                                "%{public}.0fx%{public}.0f, scale %{public}.2f)",
                                reported_bounds.width, reported_bounds.height,
                                expected_bounds.width, expected_bounds.height,
                                display->fixed_drawable_size.width,
                                display->fixed_drawable_size.height, backing_scale);
                }
            }
        }
    }

    const CGSize bounds = view.bounds.size;
    const CGSize backing = [view convertRectToBacking:view.bounds].size;
    const CGSize drawable = has_fixed_drawable ? display->fixed_drawable_size : backing;
    if (bounds.width <= 0.0 || bounds.height <= 0.0 ||
        drawable.width <= 0.0 || drawable.height <= 0.0) return;
    // Screen-saver ViewBridge occasionally reports physical pixel dimensions
    // as logical view bounds. Its caller therefore supplies the owning
    // CGDisplay's authoritative pixel extent. Desktop windows keep the dynamic
    // AppKit backing conversion by leaving fixed_drawable_size empty.
    const CGFloat scale = drawable.width / bounds.width;
    display->layer.contentsScale = scale;
    display->layer.frame = view.bounds;
    display->layer.drawableSize = drawable;
}

void LogDisplayGeometryIfChanged(MacMetalDisplay* display, id<MTLTexture> source_texture,
                                 std::uint32_t source_width, std::uint32_t source_height) {
    if (display == nullptr || display->view == nil || display->layer == nil) return;
    const CGSize view_size = display->view.bounds.size;
    const CGSize layer_size = display->layer.frame.size;
    const CGSize drawable_size = display->layer.drawableSize;
    if (CGSizeEqualToSize(view_size, display->logged_view_size) &&
        CGSizeEqualToSize(layer_size, display->logged_layer_size) &&
        CGSizeEqualToSize(drawable_size, display->logged_drawable_size) &&
        source_width == display->logged_source_width &&
        source_height == display->logged_source_height) return;

    display->logged_view_size = view_size;
    display->logged_layer_size = layer_size;
    display->logged_drawable_size = drawable_size;
    display->logged_source_width = source_width;
    display->logged_source_height = source_height;
    static os_log_t logger = os_log_create("cn.laobamac.Mirage.ScreenSaver", "MetalDisplay");
    os_log_info(logger,
                "MetalDisplay source=%{public}ux%{public}u texture=%{public}lux%{public}lu "
                "view=%{public}.0fx%{public}.0f "
                "layer=%{public}.0fx%{public}.0f drawable=%{public}.0fx%{public}.0f "
                "scale=%{public}.2f",
                source_width, source_height, source_texture.width, source_texture.height,
                view_size.width, view_size.height, layer_size.width, layer_size.height,
                drawable_size.width, drawable_size.height, display->layer.contentsScale);
}

void* CreateForNSView(void* ns_view, CGSize fixed_drawable_size) {
    if (ns_view == nullptr) return nullptr;
    NSView* content_view = (__bridge NSView*)ns_view;
    if (content_view == nil) return nullptr;
    auto* display = new MacMetalDisplay();
    display->layer = [CAMetalLayer layer];
    display->view = content_view;
    display->fixed_drawable_size = fixed_drawable_size;
    display->layer.contentsGravity = kCAGravityResizeAspect;
    display->layer.opaque = YES;

    content_view.wantsLayer = YES;
    content_view.layer = display->layer;
    UpdateDrawableSize(display);

    // Shader source compilation used to happen synchronously on the very first
    // scene frame. Prewarm it on a background queue while Vulkan, the VFS and
    // the scene graph initialize. EnsurePipeline joins only if that work has
    // not completed by the time the first exported texture arrives.
    if (id<MTLDevice> default_device = MTLCreateSystemDefaultDevice()) {
        display->layer.device = default_device;
        display->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        display->layer.framebufferOnly = YES;
        display->layer.opaque = YES;
        display->warmup_group = dispatch_group_create();
        dispatch_group_async(display->warmup_group,
                             dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
          BuildPipeline(display, default_device);
        });
    }
    return display;
}

} // namespace

extern "C" void* SceneRendererMacMetalDisplayCreateForNSView(void* ns_view) {
    return CreateForNSView(ns_view, CGSizeZero);
}

extern "C" void* SceneRendererMacMetalDisplayCreateForNSViewWithDrawableSize(
    void* ns_view, std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return nullptr;
    return CreateForNSView(ns_view, CGSizeMake(width, height));
}

#if defined(SCENERENDERER_MACMETALDISPLAY_WITH_GLFW)
extern "C" void* SceneRendererMacMetalDisplayCreate(GLFWwindow* window) {
    if (window == nullptr) return nullptr;
    NSWindow* ns_window = glfwGetCocoaWindow(window);
    if (ns_window == nil) return nullptr;
    return SceneRendererMacMetalDisplayCreateForNSView((__bridge void*)ns_window.contentView);
}
#endif

extern "C" void SceneRendererMacMetalDisplayDestroy(void* handle) {
    auto* display = static_cast<MacMetalDisplay*>(handle);
    if (display == nullptr) return;
    if (display->warmup_group != nil) {
        dispatch_group_wait(display->warmup_group, DISPATCH_TIME_FOREVER);
        display->warmup_group = nil;
    }
    delete display;
}

extern "C" void SceneRendererMacMetalDisplayDraw(void* handle, void* texture,
                                                 uint32_t source_width,
                                                 uint32_t source_height,
                                                 void (*presented)(void*), void* userdata) {
    auto* display = static_cast<MacMetalDisplay*>(handle);
    if (display == nullptr || texture == nullptr) return;

    id<MTLTexture> source_texture = (__bridge id<MTLTexture>)texture;
    if (! EnsurePipeline(display, source_texture)) return;
    UpdateDrawableSize(display);
    LogDisplayGeometryIfChanged(display, source_texture, source_width, source_height);

    id<CAMetalDrawable> drawable = [display->layer nextDrawable];
    if (drawable == nil) return;

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.02, 0.02, 0.02, 1.0);

    id<MTLCommandBuffer> command_buffer = [display->queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:display->pipeline];
    [encoder setFragmentTexture:source_texture atIndex:0];
    [encoder setFragmentSamplerState:display->sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    if (presented != nullptr) {
        // addPresentedHandler is the first point at which the drawable has
        // actually been presented, rather than merely encoded or committed.
        id strong_userdata = userdata != nullptr ? (__bridge id)userdata : nil;
        [drawable addPresentedHandler:^(id<MTLDrawable>) {
          presented((__bridge void*)strong_userdata);
        }];
    }
    [command_buffer presentDrawable:drawable];
    [command_buffer commit];
}
