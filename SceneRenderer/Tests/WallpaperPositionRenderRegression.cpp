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
    bool                        idle { false };
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

static void Run(const char* assets, const std::filesystem::path& directory, bool vertical,
                bool direct_metal = false, unsigned msaa = 1, bool effect = false,
                bool media = false) {
    std::filesystem::create_directories(directory / "cache");
    if (effect) {
        std::filesystem::create_directories(directory / "effects/performance");
        std::ofstream(directory / "effects/performance/effect.json") << R"({
            "passes":[
                {"material":"materials/util/effectpassthrough.json","target":"_rt_Performance",
                 "bind":[{"name":"previous","index":0}]},
                {"material":"materials/util/effectpassthrough.json",
                 "bind":[{"name":"_rt_Performance","index":0}]}],
            "fbos":[{"name":"_rt_Performance","scale":1,"format":"rgba_backbuffer"}]
        })";
    }
    if (media) {
        std::filesystem::create_directories(directory / "models");
        std::filesystem::create_directories(directory / "materials");
        std::ofstream(directory / "models/media.json") <<
            R"({"material":"materials/media.json","solidlayer":true})";
        std::ofstream(directory / "materials/media.json") << R"({"passes":[{
            "shader":"genericimage2","textures":["util/white"],"combos":{"VERSION":2},
            "blending":"translucent","depthtest":"disabled","depthwrite":"disabled","cullmode":"nocull"
        }]})";
    }
    const int width = vertical ? 1080 : 1920;
    const int height = vertical ? 1920 : 1080;
    std::ostringstream json;
    json << "{\"camera\":{},\"general\":{\"clearcolor\":[0,0,0],\"orthogonalprojection\":{\"width\":"
         << width << ",\"height\":" << height << "}},\"objects\":[";
    for (int i = 0; i < 3; ++i) {
        if (i != 0) json << ',';
        json << "{\"id\":" << i + 1 << ",\"image\":\""
             << (media ? "models/media.json" : "models/util/solidlayer.json") << "\",\"origin\":["
             << (vertical ? 540 : 320 + i * 640) << ',' << (vertical ? 1600 - i * 640 : 540)
             << ",0],\"size\":[" << (vertical ? 1080 : 640) << ',' << (vertical ? 640 : 1080)
             << "],\"color\":[" << (i == 0 ? 1 : 0) << ',' << (i == 1 ? 1 : 0) << ','
             << (i == 2 ? 1 : 0) << "],\"visible\":true";
        if (effect)
            json << R"(,"effects":[{"file":"effects/performance/effect.json","visible":true}])";
        if (media)
            json << R"(,"instance":{"usertextures":[{"type":"system","name":"$mediaThumbnail"}]})";
        json << '}';
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
        info.msaa_samples            = msaa;
        info.allow_on_demand         = true;
        info.frame_activity_callback = [&](bool running) {
            std::lock_guard lock(capture.mutex);
            capture.idle = ! running;
            capture.ready.notify_all();
        };
        info.enable_valid_layer = std::getenv("SCENERENDERER_TEST_VALIDATION") != nullptr;
        if (direct_metal)
            info.metal_frame_callback = [](void*, void*, std::uint32_t, std::uint32_t) {
            };
        info.width = vertical ? 192 : 108;
        info.height = vertical ? 108 : 192;
        wallpaper.configure(std::move(config));
        wallpaper.initVulkan(std::move(info));
        if (!wallpaper.waitVulkanInited(30000)) throw std::runtime_error("Vulkan initialization failed");
        if (direct_metal && wallpaper.exSwapchain() != nullptr)
            throw std::runtime_error("direct Metal output allocated unused swapchain slots");
        WaitForColor(capture, 0, 0);
        if (! effect) {
            std::unique_lock lock(capture.mutex);
            if (! capture.ready.wait_for(lock, std::chrono::seconds(5), [&] {
                    return capture.idle;
                }))
                throw std::runtime_error("static scene did not enter on-demand mode");
        }
        unsigned idle_count;
        {
            std::lock_guard lock(capture.mutex);
            idle_count = capture.count;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        {
            std::lock_guard lock(capture.mutex);
            if (! effect && capture.count != idle_count)
                throw std::runtime_error("static scene kept submitting frames");
        }
        if (!effect && !media) {
            sr::MediaStatus unrelated;
            unrelated.title   = "Unrelated media";
            unrelated.art_url = "/unused-artwork";
            wallpaper.setMediaStatus(std::move(unrelated));
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            std::lock_guard lock(capture.mutex);
            if (capture.count != idle_count)
                throw std::runtime_error("unconsumed media updates woke a static scene");
        }
        wallpaper.requestFrame();
        WaitForColor(capture, idle_count, 0);
        if (effect) {
            std::mutex              diagnostic_mutex;
            std::condition_variable diagnostic_ready;
            bool                    checked = false, limited_usage = false;
            wallpaper.requestPreparedPassDiagnostics([&](auto passes) {
                std::lock_guard lock(diagnostic_mutex);
                for (const auto& pass : passes)
                    for (const auto& texture : pass.texture_requests)
                        if (texture.request && texture.request->cache_key) {
                            const auto usage = texture.request->cache_key->image_usage;
                            if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
                                ! (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
                                limited_usage = true;
                        }
                checked = true;
                diagnostic_ready.notify_all();
            });
            std::unique_lock lock(diagnostic_mutex);
            if (! diagnostic_ready.wait_for(lock,
                                            std::chrono::seconds(5),
                                            [&] {
                                                return checked;
                                            }) ||
                ! limited_usage)
                throw std::runtime_error("effect targets did not get restricted image usage");
        }
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
        if (media) {
            wallpaper.play();
            for (int index = 0; index < 12; ++index) {
                const int channel = index % 3;
                const auto image = directory / ("art-" + std::to_string(index) + ".ppm");
                {
                    std::ofstream pixels(image, std::ios::binary);
                    pixels << "P6\n2 2\n255\n";
                    for (int pixel = 0; pixel < 4; ++pixel)
                        for (int component = 0; component < 3; ++component)
                            pixels.put(component == channel ? char(255) : char(0));
                }
                unsigned previous;
                { std::lock_guard lock(capture.mutex); previous = capture.count; }
                sr::MediaStatus status;
                status.art_url = image.string();
                wallpaper.setMediaStatus(std::move(status));
                WaitForColor(capture, previous, channel);
            }
            unsigned previous;
            { std::lock_guard lock(capture.mutex); previous = capture.count; }
            wallpaper.setMediaStatus({});
            WaitForColor(capture, previous, 0);
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
        Run(assets, directory / "direct-metal", false, true);
        Run(assets, directory / "direct-metal-msaa", false, true, 2);
        Run(assets, directory / "effect", false, true, 2, true);
        Run(assets, directory / "media", false, true, 1, false, true);
        std::filesystem::remove_all(directory);
        std::cout << "WallpaperPositionRenderRegression: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
}
