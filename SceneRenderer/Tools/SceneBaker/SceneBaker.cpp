//
//  SceneBaker.cpp
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "BakeSupport.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <cmath>
import rstd.cppstd;
import rstd.log;
import sr.json;
import sr.scene_wallpaper;

extern "C" void SceneRendererSetLiveFrameCallback(
    void (*)(const uint8_t*, uint32_t, uint32_t, void*), void*);

namespace {
struct Sink {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
};
void Frame(const uint8_t* bytes, uint32_t width, uint32_t height, void* opaque) {
    auto& sink = *static_cast<Sink*>(opaque);
    sink.pixels.assign(bytes, bytes + static_cast<size_t>(width) * height * 4);
    sink.width = width; sink.height = height;
}
}

int MBRunScene(const MBBakeSceneOptions* o, void* writer) {
    rstd::log::EnvLogger logger;
    rstd::log::set_logger(logger);
    rstd::log::set_max_level(logger.filter());
    sr::SceneWallpaperConfig config;
    config.source_pkg_path = o->source; config.assets_dir = o->assets; config.cache_dir = o->cache;
    config.offline = true; config.random_seed = o->seed; config.fps = o->fps;
    config.speed = o->speed; config.volume = o->volume;
    config.spectrum_enabled = false; config.script_storage_snapshot = o->storage;
    config.position.x = o->position_x; config.position.y = o->position_y;
    config.fill_mode = std::string(o->fill) == "contain" ? sr::FillMode::ASPECTFIT :
        std::string(o->fill) == "stretch" ? sr::FillMode::STRETCH : sr::FillMode::ASPECTCROP;
    auto props = sr::ParseJson(o->properties, {.allow_comments = false});
    if (props.is_err()) return 1;
    auto value = props.unwrap();
    if (!value.is_object()) return 1;
    (*value.as_object())->iter().for_each([&](auto entry) {
        auto [key, property] = entry;
        config.user_properties.insert(::alloc::string::String::make(key->as_str()), property->clone());
    });
    Sink sink;
    sr::SceneWallpaper wallpaper;
    if (!wallpaper.init()) return 1;
    sr::RenderInitInfo info;
    info.offscreen = true; info.manual_frames = true; info.random_seed = o->seed;
    info.width = o->width; info.height = o->height; info.video_hwdec = "none";
    wallpaper.configure(std::move(config));
    wallpaper.initVulkan(std::move(info));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (!wallpaper.sceneReady()) {
        if (MBShouldStop()) return 4;
        if (std::chrono::steady_clock::now() > deadline) return 5;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    SceneRendererSetLiveFrameCallback(Frame, &sink);
    int result = 0;
    int64_t audio_offset = 0;
    for (uint32_t i = 0; i < o->frames + o->warmup; ++i) {
        if (MBShouldStop()) { result = 4; break; }
        struct Completion {
            std::mutex mutex;
            std::condition_variable ready;
            bool done = false;
            int status = 1;
            std::vector<float> audio;
        };
        auto completion = std::make_shared<Completion>();
        sink.pixels.clear();
        uint32_t samples = static_cast<uint32_t>(std::llround((i + 1) * 48000.0 * o->speed / o->fps) -
                                                std::llround(i * 48000.0 * o->speed / o->fps));
        wallpaper.renderOfflineFrame(i * o->speed / o->fps, o->speed / o->fps, samples,
            [completion](int status, std::vector<float> audio) {
                std::lock_guard lock(completion->mutex);
                completion->status = status; completion->audio = std::move(audio); completion->done = true;
                completion->ready.notify_one();
            });
        std::unique_lock lock(completion->mutex);
        if (!completion->ready.wait_for(lock, std::chrono::seconds(60), [&]{ return completion->done; })) {
            result = 5; break;
        }
        if (completion->status) { result = completion->status; break; }
        if (sink.pixels.empty()) { result = 3; break; }
        if (i < o->warmup) continue;
        uint32_t frame = i - o->warmup;
        if (!MBWriteFrame(writer, sink.pixels.data(), sink.width, sink.height, frame)) { result = 3; break; }
        uint32_t output_samples = 48000 / o->fps;
        std::vector<float> resampled(static_cast<size_t>(output_samples) * 2, 0.0f);
        if (samples && completion->audio.size() == samples * 2) {
            for (uint32_t s = 0; s < output_samples; ++s) {
                double pos = s * static_cast<double>(samples) / output_samples;
                uint32_t a = std::min(static_cast<uint32_t>(pos), samples - 1), b = std::min(a + 1, samples - 1);
                float mix = pos - a;
                for (uint32_t channel = 0; channel < 2; ++channel)
                    resampled[s * 2 + channel] = completion->audio[a * 2 + channel] * (1 - mix) + completion->audio[b * 2 + channel] * mix;
            }
        }
        if (!MBWriteAudio(writer, resampled.data(), output_samples, audio_offset)) { result = 3; break; }
        audio_offset += output_samples;
        if (frame == 0 || (frame + 1) % std::max(1u, o->fps / 4) == 0 || frame + 1 == o->frames)
            MBProgress(frame + 1, o->frames);
    }
    SceneRendererSetLiveFrameCallback(nullptr, nullptr);
    return result;
}
