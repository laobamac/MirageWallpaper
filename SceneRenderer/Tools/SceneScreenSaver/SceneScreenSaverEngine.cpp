#include <algorithm>
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
    std::vector<void*> hosts;
    std::string configuration_key;
    bool first_frame_presented { false };
};

struct SaverInstance {
    SaverEngine* engine { nullptr };
    void* host { nullptr };
    bool paused { false };
    FirstFrameCallback first_frame_callback { nullptr };
    void* first_frame_userdata { nullptr };
};

std::mutex g_engine_mutex;
std::vector<SaverEngine*> g_engines;
std::vector<SaverInstance*> g_instances;

void Present(void* texture, std::uint32_t width, std::uint32_t height, void* userdata) {
    auto* engine = static_cast<SaverEngine*>(userdata);
    std::scoped_lock lock(g_engine_mutex);
    if (engine == nullptr) return;
    for (void* host : engine->hosts) {
        MirageSceneSaverHostPresent(host, texture, width, height);
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

void* CreateInstance(void* host, const char* assets_dir, const char* scene_pkg,
                     const char* properties_json, std::uint32_t width,
                     std::uint32_t height, std::uint32_t fps) {
    if (host == nullptr || assets_dir == nullptr || scene_pkg == nullptr) return nullptr;
    static rstd::log::EnvLogger logger;
    static bool logger_set = false;
    if (!logger_set) {
        rstd::log::set_logger(logger);
        rstd::log::set_max_level(logger.filter());
        logger_set = true;
    }
    const std::string configuration_key =
        std::string(assets_dir) + "\n" + scene_pkg + "\n" +
        (properties_json == nullptr ? "" : properties_json) + "\n" + std::to_string(fps);
    std::unique_lock lock(g_engine_mutex);
    auto existing = std::find_if(g_engines.begin(), g_engines.end(), [&](auto* candidate) {
        return candidate->configuration_key == configuration_key;
    });
    if (existing != g_engines.end()) {
        auto* instance = new SaverInstance { *existing, host, false };
        (*existing)->hosts.push_back(host);
        g_instances.push_back(instance);
        return instance;
    }
    lock.unlock();
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
    engine->hosts.push_back(host);
    engine->configuration_key = configuration_key;
    auto instance = std::make_unique<SaverInstance>(engine.get(), host, false);
    {
        std::scoped_lock instance_lock(g_engine_mutex);
        g_instances.push_back(instance.get());
    }
    if (!engine->wallpaper.init()) {
        {
            std::scoped_lock instance_lock(g_engine_mutex);
            std::erase(g_instances, instance.get());
        }
        MirageSceneSaverHostDestroy(host);
        return nullptr;
    }
    sr::SceneWallpaperConfig config;
    config.assets_dir = assets_dir;
    config.source_pkg_path = scene_pkg;
    config.cache_dir = sr::platform::GetCachePath("MirageDynamicWallpaper");
    config.fps = std::clamp<std::uint32_t>(fps, 10u, 60u);
    config.muted = true;
    if (!LoadProperties(properties_json, config)) {
        {
            std::scoped_lock instance_lock(g_engine_mutex);
            std::erase(g_instances, instance.get());
        }
        MirageSceneSaverHostDestroy(host);
        return nullptr;
    }
    sr::RenderInitInfo info;
    info.offscreen = true;
    info.width = std::clamp<std::uint32_t>(width, 500u, 8192u);
    info.height = std::clamp<std::uint32_t>(height, 500u, 8192u);
    info.msaa_samples = 1;
    info.metal_frame_callback = [engine = engine.get()](void* texture, void*, std::uint32_t frame_width,
                                                        std::uint32_t frame_height) {
        Present(texture, frame_width, frame_height, engine);
    };
    engine->wallpaper.configure(std::move(config));
    engine->wallpaper.initVulkan(std::move(info));
    if (!engine->wallpaper.waitVulkanInited(30000)) {
        {
            std::scoped_lock instance_lock(g_engine_mutex);
            std::erase(g_instances, instance.get());
        }
        MirageSceneSaverHostDestroy(host);
        return nullptr;
    }
    lock.lock();
    auto* created = engine.release();
    g_engines.push_back(created);
    instance->engine = created;
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
    return CreateInstance(host, assets_dir, scene_pkg, properties_json, width, height, fps);
}

extern "C" void MirageSceneSaverSetPaused(void* handle, int paused) {
    auto* instance = static_cast<SaverInstance*>(handle);
    std::scoped_lock lock(g_engine_mutex);
    if (instance == nullptr || instance->engine == nullptr) return;
    auto* engine = instance->engine;
    instance->paused = paused != 0;
    const bool all_paused = std::all_of(g_instances.begin(), g_instances.end(), [engine](auto* item) {
        return item->engine != engine || item->paused;
    });
    if (all_paused) engine->wallpaper.pause();
    else engine->wallpaper.play();
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
    void* host = instance->host;
    auto* engine = instance->engine;
    std::erase(g_instances, instance);
    if (engine != nullptr) std::erase(engine->hosts, host);
    delete instance;
    const bool last = engine != nullptr && std::none_of(g_instances.begin(), g_instances.end(), [engine](auto* item) {
        return item->engine == engine;
    });
    if (last) std::erase(g_engines, engine);
    lock.unlock();
    if (last) delete engine;
    MirageSceneSaverHostDestroy(host);
}

extern "C" void MirageSceneDesktopDestroy(void* handle) {
    MirageSceneSaverDestroy(handle);
}
