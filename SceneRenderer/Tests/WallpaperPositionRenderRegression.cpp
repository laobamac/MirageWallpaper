//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

import sr.scene_wallpaper;
import rstd.log;

extern "C" void SceneRendererSetLiveFrameCallback(
    void (*callback)(const std::uint8_t*, std::uint32_t, std::uint32_t, void*), void* userdata);

struct FrameCapture {
    std::mutex mutex;
    std::condition_variable ready;
    std::array<std::uint8_t, 3> color {};
    unsigned count {};
    std::uint32_t width {}, height {};
};

static void Capture(const std::uint8_t* data, std::uint32_t width,
                    std::uint32_t height, void* userdata) {
    auto& capture = *static_cast<FrameCapture*>(userdata);
    if (width == 0 || height == 0) return;
    std::lock_guard lock(capture.mutex);
    const auto* pixel = data + (static_cast<std::size_t>(height / 2) * width + width / 2) * 4;
    capture.color = { pixel[0], pixel[1], pixel[2] };
    capture.width = width;
    capture.height = height;
    ++capture.count;
    capture.ready.notify_all();
}

static void WaitForColor(FrameCapture& capture, unsigned previous, int channel) {
    std::unique_lock lock(capture.mutex);
    if (!capture.ready.wait_for(lock, std::chrono::seconds(15), [&] {
        return capture.count > previous && capture.color[channel] > 200 &&
               capture.color[(channel + 1) % 3] < 40 && capture.color[(channel + 2) % 3] < 40;
    })) {
        std::ostringstream message;
        message << "expected channel " << channel << ", got " << int(capture.color[0]) << ','
                << int(capture.color[1]) << ',' << int(capture.color[2]) << " after " << capture.count << " frames";
        throw std::runtime_error(message.str());
    }
}

static void Run(const char* assets, const std::filesystem::path& directory, bool vertical) {
    std::filesystem::create_directories(directory / "cache");
    const int width = vertical ? 1080 : 1920;
    const int height = vertical ? 1920 : 1080;
    std::ostringstream json;
    json << "{\"camera\":{},\"general\":{\"clearcolor\":[0,0,0],\"orthogonalprojection\":{\"width\":"
         << width << ",\"height\":" << height << "}},\"objects\":[";
    for (int i = 0; i < 3; ++i) {
        if (i != 0) json << ',';
        json << "{\"id\":" << i + 1 << ",\"image\":\"models/util/solidlayer.json\",\"origin\":["
             << (vertical ? 540 : 320 + i * 640) << ',' << (vertical ? 1600 - i * 640 : 540)
             << ",0],\"size\":[" << (vertical ? 1080 : 640) << ',' << (vertical ? 640 : 1080)
             << "],\"color\":[" << (i == 0 ? 1 : 0) << ',' << (i == 1 ? 1 : 0) << ','
             << (i == 2 ? 1 : 0) << "],\"visible\":true}";
    }
    json << "]}";
    std::ofstream(directory / "scene.json") << json.str();
    FrameCapture capture;
    SceneRendererSetLiveFrameCallback(Capture, &capture);
    {
        sr::SceneWallpaper wallpaper;
        if (!wallpaper.init()) throw std::runtime_error("scene runtime initialization failed");
        sr::SceneWallpaperConfig config;
        config.assets_dir = assets;
        config.source_pkg_path = (directory / "scene.json").string();
        config.cache_dir = (directory / "cache").string();
        config.script_storage_dir = (directory / "storage").string();
        config.muted = true;
        config.spectrum_enabled = false;
        config.position = { 0, 0 };
        sr::RenderInitInfo info;
        info.offscreen = true;
        info.width = vertical ? 192 : 108;
        info.height = vertical ? 108 : 192;
        wallpaper.configure(std::move(config));
        wallpaper.initVulkan(std::move(info));
        if (!wallpaper.waitVulkanInited(30000)) throw std::runtime_error("Vulkan initialization failed");
        WaitForColor(capture, 0, 0);
        wallpaper.pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        for (int channel : { 1, 2, 0 }) {
            unsigned previous;
            {
                std::lock_guard lock(capture.mutex);
                previous = capture.count;
            }
            const double position = channel == 1 ? 0.5 : channel == 2 ? 1.0 : 0.0;
            wallpaper.setPosition(vertical ? sr::WallpaperPosition { 0.5, position }
                                           : sr::WallpaperPosition { position, 0.5 });
            WaitForColor(capture, previous, channel);
            unsigned paused_count;
            {
                std::lock_guard lock(capture.mutex);
                paused_count = capture.count;
                if (capture.width != (vertical ? 192u : 108u) || capture.height != (vertical ? 108u : 192u))
                    throw std::runtime_error("output dimensions changed while moving the crop");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard lock(capture.mutex);
            if (capture.count != paused_count) throw std::runtime_error("moving the crop resumed playback");
        }
    }
    SceneRendererSetLiveFrameCallback(nullptr, nullptr);
}

int main() {
    rstd::log::EnvLogger logger;
    rstd::log::set_logger(logger);
    rstd::log::set_max_level(logger.filter());
    const char* assets = std::getenv("SCENERENDERER_ASSETS_DIR");
    if (assets == nullptr || assets[0] == '\0') return 77;
    const auto directory = std::filesystem::temp_directory_path() /
        ("mirage-position-render-" + std::to_string(getpid()));
    try {
        Run(assets, directory / "horizontal", false);
        Run(assets, directory / "vertical", true);
        std::filesystem::remove_all(directory);
        std::cout << "WallpaperPositionRenderRegression: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
}
