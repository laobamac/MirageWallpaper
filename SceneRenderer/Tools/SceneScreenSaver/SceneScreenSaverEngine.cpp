#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

import rstd.cppstd;
import rstd.log;
import sr.json;
import sr.scene_wallpaper;
import sr.utils;

extern "C" void* MirageSceneSaverHostCreate(void* ns_view, std::uint32_t drawable_width,
                                               std::uint32_t drawable_height);
extern "C" void* MirageSceneDesktopHostCreate(void* ca_layer, std::uint32_t drawable_width,
                                                std::uint32_t drawable_height);
extern "C" void MirageSceneSaverHostDestroy(void* host);
extern "C" void MirageSceneSaverHostPresent(void* host, void* texture, std::uint32_t width,
                                               std::uint32_t height);

namespace {

using FirstFrameCallback = void (*)(void*);

struct SaverEngine {
    sr::SceneWallpaper wallpaper;
    std::string configuration_key;
    bool first_frame_presented { false };
    bool paused { true };
};

struct SaverInstance {
    SaverEngine* engine { nullptr };
    void* host { nullptr };
    bool paused { false };
    FirstFrameCallback first_frame_callback { nullptr };
    void* first_frame_userdata { nullptr };
};

std::mutex g_engine_mutex;
std::mutex g_creation_mutex;
std::vector<SaverEngine*> g_engines;
std::vector<SaverInstance*> g_instances;

void UpdatePlayback(SaverEngine* engine) {
    const bool paused = std::none_of(g_instances.begin(), g_instances.end(), [engine](auto* instance) {
        return instance->engine == engine && !instance->paused;
    });
    if (engine->paused == paused) return;
    engine->paused = paused;
    if (paused) engine->wallpaper.pause();
    else engine->wallpaper.play();
}

void Present(void* texture, std::uint32_t width, std::uint32_t height, void* userdata) {
    auto* engine = static_cast<SaverEngine*>(userdata);
    std::scoped_lock lock(g_engine_mutex);
    if (engine == nullptr) return;
    for (auto* instance : g_instances) {
        if (instance->engine == engine && !instance->paused) {
            MirageSceneSaverHostPresent(instance->host, texture, width, height);
        }
    }
}

bool LoadProperties(const char* json, sr::SceneWallpaperConfig& config) {
    if (json == nullptr || json[0] == '\0') return true;
    auto parsed = sr::ParseJson(json, { .allow_comments = false });
    if (parsed.is_err()) return false;
    auto value = parsed.unwrap();
    if (!value.is_object()) return false;
    auto object = value.as_object();
    (*object)->iter().for_each([&](auto entry) {
        auto [key, property] = entry;
        config.user_properties.insert(::alloc::string::String::make(key->as_str()), property->clone());
    });
    return true;
}

sr::FillMode ParseFillMode(const char* value) {
    const std::string mode = value == nullptr ? "cover" : value;
    if (mode == "contain") return sr::FillMode::ASPECTFIT;
    if (mode == "stretch") return sr::FillMode::STRETCH;
    return sr::FillMode::ASPECTCROP;
}

void* CreateInstance(void* host, const char* assets_dir, const char* scene_pkg,
                     const char* properties_json, std::uint32_t width,
                     std::uint32_t height, std::uint32_t fps,
                     sr::FillMode fill_mode = sr::FillMode::ASPECTCROP,
                     sr::WallpaperPosition position = {}, bool paused = true) {
    std::unique_ptr<void, decltype(&MirageSceneSaverHostDestroy)> owned_host(host, MirageSceneSaverHostDestroy);
    if (host == nullptr || assets_dir == nullptr || scene_pkg == nullptr || width == 0 || height == 0) return nullptr;
    std::scoped_lock creation_lock(g_creation_mutex);
    position = position.Normalized();
    width = std::clamp<std::uint32_t>(width, 500u, 8192u);
    height = std::clamp<std::uint32_t>(height, 500u, 8192u);
    static rstd::log::EnvLogger logger;
    static std::once_flag logger_once;
    std::call_once(logger_once, [] {
        rstd::log::set_logger(logger);
        rstd::log::set_max_level(logger.filter());
    });
    const std::string configuration_key =
        std::string(assets_dir) + "\n" + scene_pkg + "\n" +
        (properties_json == nullptr ? "" : properties_json) + "\n" + std::to_string(fps) + "\n" +
        std::to_string(width) + "x" + std::to_string(height) + "\n" +
        std::to_string(static_cast<int>(fill_mode)) + "\n" +
        std::to_string(std::bit_cast<std::uint64_t>(position.x)) + "\n" +
        std::to_string(std::bit_cast<std::uint64_t>(position.y));
    std::unique_lock lock(g_engine_mutex);
    auto existing = std::find_if(g_engines.begin(), g_engines.end(), [&](auto* candidate) {
        return candidate->configuration_key == configuration_key;
    });
    if (existing != g_engines.end()) {
        auto* instance = new SaverInstance { *existing, host, paused };
        g_instances.push_back(instance);
        UpdatePlayback(*existing);
        owned_host.release();
        return instance;
    }
    lock.unlock();
    sr::SceneWallpaperConfig config;
    config.assets_dir = assets_dir;
    config.source_pkg_path = scene_pkg;
    config.cache_dir = sr::platform::GetCachePath("MirageDynamicWallpaper");
    config.fps = std::clamp<std::uint32_t>(fps, 10u, 60u);
    config.muted = true;
    config.fill_mode = fill_mode;
    config.position = position;
    if (!LoadProperties(properties_json, config)) return nullptr;
    auto engine = std::make_unique<SaverEngine>();
    auto* engine_pointer = engine.get();
    engine->wallpaper.setOnFirstFrame([engine_pointer] {
        std::vector<std::pair<FirstFrameCallback, void*>> callbacks;
        {
            std::scoped_lock lock(g_engine_mutex);
            engine_pointer->first_frame_presented = true;
            for (auto* instance : g_instances) {
                if (instance->engine == engine_pointer && instance->first_frame_callback != nullptr) {
                    callbacks.emplace_back(instance->first_frame_callback,
                                           instance->first_frame_userdata);
                }
            }
        }
        for (const auto& [callback, userdata] : callbacks) callback(userdata);
    });
    engine->configuration_key = configuration_key;
    auto instance = std::make_unique<SaverInstance>(engine.get(), host, paused);
    if (!engine->wallpaper.init()) return nullptr;
    engine->wallpaper.pause();
    sr::RenderInitInfo info;
    info.offscreen = true;
    info.width = width;
    info.height = height;
    info.msaa_samples = 1;
    info.metal_frame_callback = [engine = engine.get()](void* texture, void*, std::uint32_t frame_width,
                                                        std::uint32_t frame_height) {
        Present(texture, frame_width, frame_height, engine);
    };
    engine->wallpaper.configure(std::move(config));
    engine->wallpaper.initVulkan(std::move(info));
    if (!engine->wallpaper.waitVulkanInited(30000)) return nullptr;
    lock.lock();
    auto* created = engine.release();
    g_engines.push_back(created);
    instance->engine = created;
    g_instances.push_back(instance.get());
    UpdatePlayback(created);
    owned_host.release();
    lock.unlock();
    return instance.release();
}

}

extern "C" void* MirageSceneSaverCreate(void* ns_view, const char* assets_dir,
                                          const char* scene_pkg, const char* properties_json,
                                          std::uint32_t width, std::uint32_t height,
                                          std::uint32_t drawable_width,
                                          std::uint32_t drawable_height,
                                          std::uint32_t fps) {
    void* host = MirageSceneSaverHostCreate(ns_view, drawable_width, drawable_height);
    return CreateInstance(host, assets_dir, scene_pkg, properties_json, width, height, fps);
}

extern "C" void* MirageSceneDesktopCreate(void* ca_layer, const char* assets_dir,
                                            const char* scene_pkg, const char* properties_json,
                                            std::uint32_t width, std::uint32_t height,
                                            std::uint32_t fps) {
    void* host = MirageSceneDesktopHostCreate(ca_layer, width, height);
    return CreateInstance(host, assets_dir, scene_pkg, properties_json, width, height, fps,
                          sr::FillMode::ASPECTCROP, {}, false);
}

extern "C" void* MirageSceneSaverCreateWithPosition(
    void* ns_view, const char* assets_dir, const char* scene_pkg, const char* properties_json,
    std::uint32_t width, std::uint32_t height, std::uint32_t drawable_width,
    std::uint32_t drawable_height, std::uint32_t fps, const char* fill_mode, double x, double y) {
    void* host = MirageSceneSaverHostCreate(ns_view, drawable_width, drawable_height);
    return CreateInstance(host, assets_dir, scene_pkg, properties_json, width, height, fps,
                          ParseFillMode(fill_mode), { x, y });
}

extern "C" void* MirageSceneDesktopCreateWithPosition(
    void* ca_layer, const char* assets_dir, const char* scene_pkg, const char* properties_json,
    std::uint32_t width, std::uint32_t height, std::uint32_t fps,
    const char* fill_mode, double x, double y) {
    void* host = MirageSceneDesktopHostCreate(ca_layer, width, height);
    return CreateInstance(host, assets_dir, scene_pkg, properties_json, width, height, fps,
                          ParseFillMode(fill_mode), { x, y }, false);
}

extern "C" void MirageSceneSaverSetPaused(void* handle, int paused) {
    auto* instance = static_cast<SaverInstance*>(handle);
    std::scoped_lock lock(g_engine_mutex);
    if (std::find(g_instances.begin(), g_instances.end(), instance) == g_instances.end()) return;
    auto* engine = instance->engine;
    instance->paused = paused != 0;
    UpdatePlayback(engine);
}

extern "C" void MirageSceneDesktopSetPaused(void* handle, int paused) {
    MirageSceneSaverSetPaused(handle, paused);
}

extern "C" void MirageSceneDesktopSetFirstFrameCallback(void* handle,
                                                          FirstFrameCallback callback,
                                                          void* userdata) {
    auto* instance = static_cast<SaverInstance*>(handle);
    bool first_frame_presented = false;
    {
        std::scoped_lock lock(g_engine_mutex);
        if (instance == nullptr || instance->engine == nullptr) return;
        instance->first_frame_callback = callback;
        instance->first_frame_userdata = userdata;
        first_frame_presented = instance->engine->first_frame_presented;
    }
    if (callback != nullptr && first_frame_presented) callback(userdata);
}

extern "C" void MirageSceneSaverDestroy(void* handle) {
    auto* instance = static_cast<SaverInstance*>(handle);
    if (instance == nullptr) return;
    std::unique_lock lock(g_engine_mutex);
    if (std::find(g_instances.begin(), g_instances.end(), instance) == g_instances.end()) return;
    void* host = instance->host;
    auto* engine = instance->engine;
    std::erase(g_instances, instance);
    delete instance;
    const bool last = engine != nullptr && std::none_of(g_instances.begin(), g_instances.end(), [engine](auto* item) {
        return item->engine == engine;
    });
    if (last) std::erase(g_engines, engine);
    else if (engine != nullptr) UpdatePlayback(engine);
    lock.unlock();
    MirageSceneSaverHostDestroy(host);
    if (last) delete engine;
}

extern "C" void MirageSceneDesktopDestroy(void* handle) {
    MirageSceneSaverDestroy(handle);
}
