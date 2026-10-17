#include "SceneSnapshot.h"

#include <png.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// Declared by the engine. Registering the callback enables one RGBA8 readback;
// unregistering it restores the zero-cost presentation path.
extern "C" void SceneRendererSetLiveFrameCallback(
    void (*cb)(const uint8_t* rgba, uint32_t width, uint32_t height, void* userdata),
    void* userdata);

namespace mirage {
namespace {

// The callback can already be executing when it is unregistered. Shared
// ownership keeps the destination valid until both the waiter and callback
// have released it, avoiding a render-thread use-after-free.
struct FrameSink {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::uint8_t> pixels;
    std::uint32_t width {0U};
    std::uint32_t height {0U};
    bool filled {false};
};

std::shared_ptr<FrameSink>& ActiveSink() {
    static std::shared_ptr<FrameSink> sink;
    return sink;
}

std::mutex& ActiveSinkMutex() {
    static std::mutex mutex;
    return mutex;
}

void OnLiveFrame(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                 void*) {
    if (rgba == nullptr || width == 0U || height == 0U) return;

    std::shared_ptr<FrameSink> sink;
    {
        std::scoped_lock lock(ActiveSinkMutex());
        sink = ActiveSink();
    }
    if (!sink) return;

    std::scoped_lock lock(sink->mutex);
    if (sink->filled) return;
    const std::size_t bytes = static_cast<std::size_t>(width) *
                              static_cast<std::size_t>(height) * 4U;
    sink->pixels.assign(rgba, rgba + bytes);
    sink->width = width;
    sink->height = height;
    sink->filled = true;
    sink->ready.notify_all();
}

} // namespace

bool WriteSceneSnapshot(const std::string& path, double timeout_seconds,
                        std::function<void()> request_frame) {
    if (path.empty() || timeout_seconds <= 0.0) return false;

    // The engine exposes one process-wide callback. The control channel is
    // already serial, while this mutex also preserves that ownership contract
    // for direct callers and tests.
    static std::mutex capture_mutex;
    std::scoped_lock capture_lock(capture_mutex);

    const auto sink = std::make_shared<FrameSink>();
    {
        std::scoped_lock lock(ActiveSinkMutex());
        ActiveSink() = sink;
    }
    SceneRendererSetLiveFrameCallback(&OnLiveFrame, nullptr);
    if (request_frame) request_frame();

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
        if (ActiveSink() == sink) ActiveSink().reset();
    }
    if (!filled) return false;

    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    {
        std::scoped_lock lock(sink->mutex);
        pixels = std::move(sink->pixels);
        width = sink->width;
        height = sink->height;
    }
    const std::size_t expected = static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 4U;
    if (width == 0U || height == 0U || pixels.size() != expected) return false;

    FILE* output = std::fopen(path.c_str(), "wb");
    if (output == nullptr) {
        const int open_error = errno;
        std::fprintf(stderr, "SceneWallpaper: cannot open snapshot '%s': %s\n",
                     path.c_str(), std::strerror(open_error));
        return false;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr) {
        const int close_result = std::fclose(output);
        if (close_result != 0) std::fprintf(stderr, "SceneWallpaper: cannot close failed snapshot output\n");
        if (std::remove(path.c_str()) != 0) {
            std::fprintf(stderr, "SceneWallpaper: cannot remove incomplete snapshot\n");
        }
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        const int close_result = std::fclose(output);
        if (close_result != 0) std::fprintf(stderr, "SceneWallpaper: cannot close failed snapshot output\n");
        if (std::remove(path.c_str()) != 0) {
            std::fprintf(stderr, "SceneWallpaper: cannot remove incomplete snapshot\n");
        }
        return false;
    }

    // libpng reports encoding failures through its documented longjmp boundary.
    // No C++ object requiring destruction is created after this checkpoint.
    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_write_struct(&png, &info);
        const int close_result = std::fclose(output);
        if (close_result != 0) std::fprintf(stderr, "SceneWallpaper: cannot close failed snapshot output\n");
        if (std::remove(path.c_str()) != 0) {
            std::fprintf(stderr, "SceneWallpaper: cannot remove incomplete snapshot\n");
        }
        return false;
    }

    png_init_io(png, output);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (std::uint32_t row = 0U; row < height; ++row) {
        png_write_row(png, pixels.data() + static_cast<std::size_t>(row) * width * 4U);
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    const bool closed = std::fclose(output) == 0;
    if (!closed && std::remove(path.c_str()) != 0) {
        std::fprintf(stderr, "SceneWallpaper: cannot remove snapshot after close failure\n");
    }
    return closed;
}

} // namespace mirage
