#if defined(SCENERENDERER_MACMETALDISPLAY_WITH_GLFW)
#    define GLFW_EXPOSE_NATIVE_COCOA
#    include <GLFW/glfw3.h>
#    include <GLFW/glfw3native.h>
#endif

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace
{

struct MacMetalDisplay {
    CAMetalLayer*             layer { nil };
    NSView*                   view { nil };
    id<MTLDevice>             device { nil };
    id<MTLCommandQueue>       queue { nil };
    id<MTLRenderPipelineState> pipeline { nil };
    id<MTLSamplerState>       sampler { nil };
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

bool EnsurePipeline(MacMetalDisplay* display, id<MTLTexture> source_texture) {
    if (display == nullptr || source_texture == nil) return false;
    id<MTLDevice> device = source_texture.device;
    if (device == nil) return false;
    if (display->pipeline != nil && display->device == device) return true;

    display->device = device;
    display->layer.device = device;
    display->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    display->layer.framebufferOnly = YES;
    display->layer.opaque = YES;

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

void UpdateDrawableSize(MacMetalDisplay* display) {
    if (display == nullptr || display->layer == nil) return;
    NSView* view = display->view;
    if (view == nil) return;
    const CGFloat scale = view.window.backingScaleFactor > 0.0 ? view.window.backingScaleFactor
                                                               : NSScreen.mainScreen.backingScaleFactor;
    const CGSize bounds = view.bounds.size;
    display->layer.contentsScale = scale;
    display->layer.frame = view.bounds;
    display->layer.drawableSize = CGSizeMake(bounds.width * scale, bounds.height * scale);
}

} // namespace

extern "C" void* SceneRendererMacMetalDisplayCreateForNSView(void* ns_view) {
    if (ns_view == nullptr) return nullptr;
    NSView* content_view = (__bridge NSView*)ns_view;
    if (content_view == nil) return nullptr;
    auto* display = new MacMetalDisplay();
    display->layer = [CAMetalLayer layer];
    display->view = content_view;
    display->layer.contentsGravity = kCAGravityResizeAspect;
    display->layer.opaque = YES;

    content_view.wantsLayer = YES;
    content_view.layer = display->layer;
    UpdateDrawableSize(display);
    return display;
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
    delete display;
}

extern "C" void SceneRendererMacMetalDisplayDraw(void* handle, void* texture, uint32_t, uint32_t) {
    auto* display = static_cast<MacMetalDisplay*>(handle);
    if (display == nullptr || texture == nullptr) return;

    id<MTLTexture> source_texture = (__bridge id<MTLTexture>)texture;
    if (! EnsurePipeline(display, source_texture)) return;
    UpdateDrawableSize(display);

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
    [command_buffer presentDrawable:drawable];
    [command_buffer commit];
}
