#include "SceneSnapshot.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// Declared by the engine (Gpu/Pipeline/PresentPass.cpp). Registering a callback
// makes FinPass copy the composited frame back to host memory each frame;
// unregistering restores the zero-cost path.
extern "C" void SceneRendererSetLiveFrameCallback(
    void (*cb)(const uint8_t* rgba, uint32_t width, uint32_t height, void* userdata),
    void* userdata);

namespace mirage {

namespace {

/// One frame handed over from the render thread.
///
/// Held by shared_ptr and deliberately never destroyed by the waiter: a frame
/// callback can still be in flight inside the engine when we unregister, so the
/// sink has to outlive this call rather than be a stack object the late callback
/// would write into after it went away.
struct FrameSink {
    std::mutex              mutex;
    std::condition_variable ready;
    std::vector<uint8_t>    pixels;
    uint32_t                width { 0 };
    uint32_t                height { 0 };
    bool                    filled { false };
};

std::shared_ptr<FrameSink>& ActiveSink() {
    static std::shared_ptr<FrameSink> sink;
    return sink;
}

std::mutex& ActiveSinkMutex() {
    static std::mutex mutex;
    return mutex;
}

/// Runs on the render thread, inside finishFrameDump. Does the minimum possible:
/// copies the bytes and wakes the waiter. Encoding here would stall rendering.
void OnLiveFrame(const uint8_t* rgba, uint32_t width, uint32_t height, void* /*userdata*/) {
    if (rgba == nullptr || width == 0 || height == 0) return;

    std::shared_ptr<FrameSink> sink;
    {
        std::scoped_lock lock(ActiveSinkMutex());
        sink = ActiveSink();
    }
    if (! sink) return;

    std::scoped_lock lock(sink->mutex);
    if (sink->filled) return; // one-shot: ignore every frame after the first
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    sink->pixels.assign(rgba, rgba + bytes);
    sink->width  = width;
    sink->height = height;
    sink->filled = true;
    sink->ready.notify_all();
}

bool WriteImage(CGImageRef image, NSString* path, CFStringRef type) {
    NSURL* url = [NSURL fileURLWithPath:path];
    CGImageDestinationRef dest =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, type, 1, nullptr);
    if (dest == nullptr) return false;
    NSDictionary* options = @{ (id)kCGImageDestinationLossyCompressionQuality: @0.9 };
    CGImageDestinationAddImage(dest, image, (__bridge CFDictionaryRef)options);
    const bool ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    return ok;
}

/// HEIC keeps a 5K still in the low hundreds of KB; Macs with no HEVC encoder
/// return a null destination, so fall back to JPEG rather than writing nothing.
bool EncodeRGBA(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
                const std::string& path) {
    const std::size_t expected = static_cast<std::size_t>(width) * height * 4;
    if (pixels.size() < expected) return false;

    CFDataRef data = CFDataCreate(nullptr, pixels.data(), static_cast<CFIndex>(expected));
    if (data == nullptr) return false;
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    if (provider == nullptr) {
        CFRelease(data);
        return false;
    }

    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // The engine delivers VK_FORMAT_R8G8B8A8_UNORM: R first, alpha last.
    // The scene composite is opaque, so alpha is skipped rather than trusted —
    // a premultiplied read of a stale alpha channel would darken the still.
    const uint32_t bitmap_info =
        static_cast<uint32_t>(kCGImageAlphaNoneSkipLast) |
        static_cast<uint32_t>(kCGBitmapByteOrderDefault);
    CGImageRef image = CGImageCreate(
        width, height, 8, 32, static_cast<std::size_t>(width) * 4, space,
        static_cast<CGBitmapInfo>(bitmap_info),
        provider, nullptr, false, kCGRenderingIntentDefault);

    bool ok = false;
    if (image != nullptr) {
        NSString* ns_path = [NSString stringWithUTF8String:path.c_str()];
        if (ns_path != nil) {
            ok = WriteImage(image, ns_path, (__bridge CFStringRef)UTTypeHEIC.identifier);
            if (! ok) {
                ok = WriteImage(image, ns_path, (__bridge CFStringRef)UTTypeJPEG.identifier);
            }
        }
        CGImageRelease(image);
    }
    if (space != nullptr) CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(data);
    return ok;
}

} // namespace

bool WriteSceneSnapshot(const std::string& path, double timeout_seconds) {
    if (path.empty()) return false;

    auto sink = std::make_shared<FrameSink>();
    {
        std::scoped_lock lock(ActiveSinkMutex());
        // Serialised by the caller (one control-channel thread), so an existing
        // sink here would mean a concurrent request; the newest one wins.
        ActiveSink() = sink;
    }
    SceneRendererSetLiveFrameCallback(&OnLiveFrame, nullptr);

    bool filled = false;
    {
        std::unique_lock lock(sink->mutex);
        filled = sink->ready.wait_for(
            lock, std::chrono::duration<double>(timeout_seconds),
            [&sink] { return sink->filled; });
    }

    SceneRendererSetLiveFrameCallback(nullptr, nullptr);
    {
        std::scoped_lock lock(ActiveSinkMutex());
        // Drop our claim, but only if it is still ours. The sink itself stays
        // alive in `sink` until this function returns, so a callback that was
        // already past its null check writes somewhere valid.
        if (ActiveSink() == sink) ActiveSink().reset();
    }

    if (! filled) return false;

    std::vector<uint8_t> pixels;
    uint32_t             width  = 0;
    uint32_t             height = 0;
    {
        std::scoped_lock lock(sink->mutex);
        pixels = std::move(sink->pixels);
        width  = sink->width;
        height = sink->height;
    }
    return EncodeRGBA(pixels, width, height, path);
}

} // namespace mirage
