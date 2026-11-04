#include "SceneSnapshot.h"
#include "../../Sources/SceneRenderer/Kernel/Support/include/Utils/PlatformFile.hpp"
#include <png.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

extern "C" void SceneRendererSetLiveFrameCallback(
    void (*callback)(const std::uint8_t*, std::uint32_t, std::uint32_t, void*), void*);

namespace mirage {
namespace {
struct FrameSink {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::uint8_t> pixels;
    std::uint32_t width { 0 }, height { 0 };
    bool filled { false };
};
std::mutex sink_mutex;
std::shared_ptr<FrameSink> active_sink;

void OnLiveFrame(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, void*) {
    if (!pixels || width == 0 || height == 0) return;
    std::shared_ptr<FrameSink> sink;
    { std::lock_guard lock(sink_mutex); sink = active_sink; }
    if (!sink) return;
    std::lock_guard lock(sink->mutex);
    if (sink->filled) return;
    sink->pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4U);
    sink->width = width;
    sink->height = height;
    sink->filled = true;
    sink->ready.notify_one();
}
} // namespace

bool WriteSceneSnapshot(const std::string& path, double timeout_seconds,
                        std::function<void()> request_frame) {
    if (path.empty() || timeout_seconds <= 0) return false;
    static std::mutex capture_mutex;
    std::lock_guard capture_lock(capture_mutex);
    const auto sink = std::make_shared<FrameSink>();
    { std::lock_guard lock(sink_mutex); active_sink = sink; }
    SceneRendererSetLiveFrameCallback(OnLiveFrame, nullptr);
    if (request_frame) request_frame();
    bool filled;
    {
        std::unique_lock lock(sink->mutex);
        filled = sink->ready.wait_for(lock, std::chrono::duration<double>(timeout_seconds),
                                      [&] { return sink->filled; });
    }
    SceneRendererSetLiveFrameCallback(nullptr, nullptr);
    { std::lock_guard lock(sink_mutex); active_sink.reset(); }
    if (!filled) return false;
    std::lock_guard lock(sink->mutex);
    auto* output = sr::platform::OpenFile(path, "wb");
    if (!output) return false;
    png_image image {};
    image.version = PNG_IMAGE_VERSION;
    image.width = sink->width;
    image.height = sink->height;
    image.format = PNG_FORMAT_RGBA;
    const bool written = png_image_write_to_stdio(&image, output, 0, sink->pixels.data(), 0, nullptr) != 0;
    png_image_free(&image);
    const bool closed = std::fclose(output) == 0;
    if (!written || !closed) {
        std::error_code error;
        std::filesystem::remove(sr::platform::NativePath(path), error);
    }
    return written && closed;
}
} // namespace mirage
