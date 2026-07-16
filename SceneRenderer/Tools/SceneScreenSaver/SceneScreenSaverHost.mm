#import <AppKit/AppKit.h>

#include <atomic>
#include <cstdint>

extern "C" void* SceneRendererMacMetalDisplayCreateForNSView(void* ns_view);
extern "C" void* SceneRendererMacMetalDisplayCreateForNSViewWithDrawableSize(
    void* ns_view, std::uint32_t width, std::uint32_t height);
extern "C" void SceneRendererMacMetalDisplayDestroy(void* handle);
extern "C" void SceneRendererMacMetalDisplayDraw(void* handle, void* texture,
                                                 std::uint32_t width, std::uint32_t height,
                                                 void (*presented)(void*), void* userdata);

@interface MirageSaverHostReference : NSObject
@property(nonatomic, assign) void* host;
@end
@implementation MirageSaverHostReference
@end

namespace {
struct SaverHost {
    void* display { nullptr };
    std::atomic<void*> texture { nullptr };
    std::atomic<std::uint32_t> width { 0 };
    std::atomic<std::uint32_t> height { 0 };
    std::atomic<bool> scheduled { false };
    MirageSaverHostReference* reference { nil };
};
}

extern "C" void* MirageSceneSaverHostCreate(void* ns_view, std::uint32_t drawable_width,
                                             std::uint32_t drawable_height) {
    auto create = ^void* {
        auto* host = new SaverHost();
        host->display = drawable_width > 0 && drawable_height > 0
            ? SceneRendererMacMetalDisplayCreateForNSViewWithDrawableSize(
                  ns_view, drawable_width, drawable_height)
            : SceneRendererMacMetalDisplayCreateForNSView(ns_view);
        if (host->display == nullptr) {
            delete host;
            return nullptr;
        }
        host->reference = [MirageSaverHostReference new];
        host->reference.host = host;
        return host;
    };
    if (NSThread.isMainThread) return create();
    __block void* result = nullptr;
    dispatch_sync(dispatch_get_main_queue(), ^{ result = create(); });
    return result;
}

extern "C" void MirageSceneSaverHostPresent(void* handle, void* texture,
                                              std::uint32_t width, std::uint32_t height) {
    auto* host = static_cast<SaverHost*>(handle);
    if (host == nullptr || texture == nullptr) return;
    host->texture.store(texture);
    host->width.store(width);
    host->height.store(height);
    if (host->scheduled.exchange(true)) return;
    MirageSaverHostReference* reference = host->reference;
    dispatch_async(dispatch_get_main_queue(), ^{
        auto* current = static_cast<SaverHost*>(reference.host);
        if (current == nullptr) return;
        current->scheduled.store(false);
        SceneRendererMacMetalDisplayDraw(current->display, current->texture.load(),
                                         current->width.load(), current->height.load(), nullptr,
                                         nullptr);
    });
}

extern "C" void MirageSceneSaverHostDestroy(void* handle) {
    auto* host = static_cast<SaverHost*>(handle);
    if (host == nullptr) return;
    host->reference.host = nullptr;
    auto destroy = ^{
        SceneRendererMacMetalDisplayDestroy(host->display);
        host->display = nullptr;
    };
    if (NSThread.isMainThread) destroy();
    else dispatch_sync(dispatch_get_main_queue(), destroy);
    delete host;
}
