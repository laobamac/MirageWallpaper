#include "DesktopHost.h"
#include "ControlChannel.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(__APPLE__)
#include <xlocale.h>
#else
#include <locale.h>
#endif

import rstd.cppstd;
import rstd.log;
import sr.json;
import sr.scene_wallpaper;
import sr.utils;

namespace
{

void CrashHandler(int sig) {
    const char* name = "UNKNOWN";
    switch (sig) {
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGABRT: name = "SIGABRT"; break;
    case SIGBUS:  name = "SIGBUS"; break;
    case SIGILL:  name = "SIGILL"; break;
    case SIGFPE:  name = "SIGFPE"; break;
    }
    std::cerr << "[SceneWallpaper] CRASH: signal " << sig << " (" << name << ")" << std::endl;
    void* callstack[64];
    int   frames = backtrace(callstack, 64);
    char** symbols = backtrace_symbols(callstack, frames);
    std::cerr << "[SceneWallpaper] Backtrace (" << frames << " frames):" << std::endl;
    for (int i = 0; i < frames; ++i) {
        std::cerr << "  " << i << ": " << (symbols[i] ? symbols[i] : "???") << std::endl;
    }
    free(symbols);
    std::cerr << std::flush;
    std::_Exit(128 + sig);
}

void InstallCrashHandler() {
    signal(SIGSEGV, CrashHandler);
    signal(SIGABRT, CrashHandler);
    signal(SIGBUS,  CrashHandler);
    signal(SIGILL,  CrashHandler);
    signal(SIGFPE,  CrashHandler);
}

struct Resolution {
    std::uint32_t width { 0 };
    std::uint32_t height { 0 };
};

struct Options {
    std::string               assets_dir;
    std::string               scene_pkg;
    std::string               cache_dir;
    std::string               user_properties;
    std::optional<Resolution> resolution;
    std::optional<std::array<double, 2>> mouse_position;
    std::uint32_t             fps { 30 };
    std::uint32_t             input_hz { 60 };
    std::uint32_t             msaa { 1 };
    std::uint32_t             screen { 0 };
    int                       run_seconds { 0 };
    bool                      valid_layer { false };
    bool                      graphviz { false };
    bool                      muted { false };
    bool                      control_stdin { false };
    bool                      deferred_show { false };
    bool                      spectrum_enabled { true };
    bool                      external_spectrum { false };
};

struct AppState {
    sr::SceneWallpaper* wallpaper { nullptr };
    void*               desktop { nullptr };
    std::chrono::steady_clock::time_point started_at { std::chrono::steady_clock::now() };
};

void EmitLifecycleEvent(const AppState* state, std::string_view event) {
    static std::mutex output_mutex;
    const auto elapsed = state == nullptr
                             ? 0
                             : std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - state->started_at)
                                   .count();
    std::lock_guard lock(output_mutex);
    std::cout << "{\"event\":\"" << event << "\",\"elapsed_ms\":" << elapsed << "}\n"
              << std::flush;
}

class DesktopHandle {
public:
    DesktopHandle() = default;
    ~DesktopHandle() { reset(); }

    DesktopHandle(const DesktopHandle&) = delete;
    DesktopHandle& operator=(const DesktopHandle&) = delete;

    void reset(void* handle = nullptr) {
        if (handle_ != nullptr) sr::host::DesktopDestroy(handle_);
        handle_ = handle;
    }

    [[nodiscard]] void* get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    void* handle_ { nullptr };
};

class StopTimer {
public:
    StopTimer() = default;
    ~StopTimer() { stop(); }

    StopTimer(const StopTimer&) = delete;
    StopTimer& operator=(const StopTimer&) = delete;

    void start(void* desktop, int seconds) {
        stop();
        if (desktop == nullptr || seconds <= 0) return;

        {
            std::lock_guard lock(mutex_);
            stop_requested_ = false;
        }
        worker_ = std::thread([this, desktop, seconds]() {
            std::unique_lock lock(mutex_);
            const bool stopped = cv_.wait_for(lock, std::chrono::seconds(seconds), [this]() {
                return stop_requested_;
            });
            lock.unlock();
            if (! stopped) sr::host::DesktopStop(desktop);
        });
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stop_requested_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    stop_requested_ { false };
    std::thread             worker_;
};

void PrintUsage(const char* argv0) {
    std::cerr
        << "Usage: " << (argv0 != nullptr ? argv0 : "SceneWallpaper")
        << " [options] <assets> <scene.pkg>\n\n"
        << "Options:\n"
        << "  -f, --fps N                 Render FPS (default 30)\n"
        << "  -R, --resolution WxH        Override render resolution\n"
        << "  -C, --cache-path DIR        Cache directory\n"
        << "  -M, --msaa N                MSAA samples for screen RT\n"
        << "  -P, --user-properties FILE  JSON object of WE user properties\n"
        << "  -V, --valid-layer           Enable Vulkan validation layer\n"
        << "  -G, --graphviz              Emit graph.dot for the render graph\n"
        << "      --mouse-position X,Y    Initial normalized mouse position\n"
        << "      --input-hz N            Desktop mouse polling rate (default 60)\n"
        << "      --screen N              Screen index to cover (default 0 = main)\n"
        << "      --muted                 Start with audio muted\n"
        << "      --control-stdin         Accept live JSON control commands on stdin\n"
        << "      --deferred-show         Keep the window transparent until activated\n"
        << "      --no-spectrum           Disable audio response\n"
        << "      --external-spectrum     Receive spectrum from stdin\n"
        << "      --run-seconds N         Exit after N seconds (test helper)\n";
}

bool ParseUInt(std::string_view text, std::uint32_t& out) {
    if (text.empty()) return false;
    std::uint32_t value = 0;
    auto          r     = std::from_chars(text.data(), text.data() + text.size(), value);
    if (r.ec != std::errc {} || r.ptr != text.data() + text.size()) return false;
    out = value;
    return true;
}

bool ParseInt(std::string_view text, int& out) {
    if (text.empty()) return false;
    int  value = 0;
    auto r     = std::from_chars(text.data(), text.data() + text.size(), value);
    if (r.ec != std::errc {} || r.ptr != text.data() + text.size()) return false;
    out = value;
    return true;
}

bool ParseDouble(std::string_view text, double& out) {
    if (text.empty()) return false;
    std::string value_text(text);
    char*       parsed_end = nullptr;
    errno = 0;
#if defined(__APPLE__)
    static locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    const double value = c_locale != nullptr
                             ? strtod_l(value_text.c_str(), &parsed_end, c_locale)
                             : std::strtod(value_text.c_str(), &parsed_end);
#else
    const double value = std::strtod(value_text.c_str(), &parsed_end);
#endif
    if (errno == ERANGE || parsed_end != value_text.data() + value_text.size() ||
        ! std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

std::optional<Resolution> ParseResolution(std::string_view text) {
    const auto x = text.find('x');
    if (x == std::string_view::npos) return std::nullopt;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    if (! ParseUInt(text.substr(0, x), w)) return std::nullopt;
    if (! ParseUInt(text.substr(x + 1), h)) return std::nullopt;
    if (w == 0 || h == 0) return std::nullopt;
    return Resolution { w, h };
}

std::optional<std::array<double, 2>> ParseMousePosition(std::string_view text) {
    const auto comma = text.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    double x = 0.0;
    double y = 0.0;
    auto   xs = text.substr(0, comma);
    auto   ys = text.substr(comma + 1);
    if (! ParseDouble(xs, x) || ! ParseDouble(ys, y)) return std::nullopt;
    return std::array { std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0) };
}

bool ParseArgs(int argc, char** argv, Options& out) {
    std::vector<std::string> positional;
    auto require_value = [&](int& i, std::string_view opt) -> const char* {
        if (i + 1 >= argc) {
            std::cerr << opt << " requires a value\n";
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view arg { argv[i] };
        if (arg.compare("-h") == 0 || arg.compare("--help") == 0) {
            PrintUsage(argv[0]);
            std::exit(0);
        } else if (arg.compare("-V") == 0 || arg.compare("--valid-layer") == 0) {
            out.valid_layer = true;
        } else if (arg.compare("-G") == 0 || arg.compare("--graphviz") == 0) {
            out.graphviz = true;
        } else if (arg.compare("--muted") == 0) {
            out.muted = true;
        } else if (arg.compare("--control-stdin") == 0) {
            out.control_stdin = true;
        } else if (arg.compare("--deferred-show") == 0) {
            out.deferred_show = true;
        } else if (arg.compare("--no-spectrum") == 0) {
            out.spectrum_enabled = false;
        } else if (arg.compare("--external-spectrum") == 0) {
            out.external_spectrum = true;
        } else if (arg.compare("--screen") == 0) {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.screen)) return false;
        } else if (arg.compare("-f") == 0 || arg.compare("--fps") == 0) {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.fps)) return false;
        } else if (arg.compare("-R") == 0 || arg.compare("--resolution") == 0) {
            const char* value = require_value(i, arg);
            if (value == nullptr) return false;
            out.resolution = ParseResolution(value);
            if (! out.resolution) return false;
        } else if (arg.compare("-C") == 0 || arg.compare("--cache-path") == 0) {
            const char* value = require_value(i, arg);
            if (value == nullptr) return false;
            out.cache_dir = value;
        } else if (arg.compare("-M") == 0 || arg.compare("--msaa") == 0) {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.msaa)) return false;
        } else if (arg.compare("-P") == 0 || arg.compare("--user-properties") == 0) {
            const char* value = require_value(i, arg);
            if (value == nullptr) return false;
            out.user_properties = value;
        } else if (arg.compare("--mouse-position") == 0) {
            const char* value = require_value(i, arg);
            if (value == nullptr) return false;
            out.mouse_position = ParseMousePosition(value);
            if (! out.mouse_position) return false;
        } else if (arg.compare("--input-hz") == 0) {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.input_hz)) return false;
        } else if (arg.compare("--run-seconds") == 0) {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseInt(value, out.run_seconds)) return false;
        } else if (! arg.empty() && arg.front() == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        } else {
            positional.emplace_back(arg);
        }
    }

    if (positional.size() != 2) {
        PrintUsage(argv[0]);
        return false;
    }
    out.assets_dir = positional[0];
    out.scene_pkg  = positional[1];
    if (out.fps < 5) out.fps = 5;
    if (out.input_hz == 0) out.input_hz = 60;
    return true;
}

bool LoadUserProperties(const std::string& path, sr::SceneWallpaperConfig& config) {
    if (path.empty()) return true;
    std::ifstream is(path);
    if (! is) {
        std::cerr << "--user-properties: cannot open '" << path << "'\n";
        return false;
    }
    std::stringstream ss;
    ss << is.rdbuf();
    auto parsed = sr::ParseJson(ss.str(), { .allow_comments = true });
    if (parsed.is_err()) {
        std::cerr << "--user-properties: '" << path << "' is not a JSON object\n";
        return false;
    }
    auto value = parsed.unwrap();
    if (! value.is_object()) {
        std::cerr << "--user-properties: '" << path << "' is not a JSON object\n";
        return false;
    }
    auto object = value.as_object();
    (*object)->iter().for_each([&](auto entry) {
        auto [key, value] = entry;
        config.user_properties.insert(
            ::alloc::string::String::make(key->as_str()), value->clone());
    });
    return true;
}

#if defined(__APPLE__)
extern "C" void SceneRendererSetLiveMetalFrameCallback(
    void (*cb)(void*, std::uint32_t, std::uint32_t, void*), void* userdata);

void LiveMetalFrameCallback(void* texture, std::uint32_t width, std::uint32_t height,
                            void* userdata) {
    auto* state = static_cast<AppState*>(userdata);
    if (state == nullptr || state->desktop == nullptr) return;
    sr::host::DesktopPresent(state->desktop, texture, width, height);
}

class LiveMetalFrameCallbackGuard {
public:
    ~LiveMetalFrameCallbackGuard() { SceneRendererSetLiveMetalFrameCallback(nullptr, nullptr); }
};
#endif

void MouseMoveCallback(double x, double y, void* userdata) {
    auto* state = static_cast<AppState*>(userdata);
    if (state == nullptr || state->wallpaper == nullptr) return;
    state->wallpaper->mouseInput(x, y);
}

void MouseButtonCallback(int button, int down, void* userdata) {
    auto* state = static_cast<AppState*>(userdata);
    if (state == nullptr || state->wallpaper == nullptr) return;
    state->wallpaper->mouseButton(button, down != 0);
}

void MouseEnterCallback(int entered, void* userdata) {
    auto* state = static_cast<AppState*>(userdata);
    if (state == nullptr || state->wallpaper == nullptr) return;
    state->wallpaper->mouseEnter(entered != 0);
}

void FirstFramePresentedCallback(void* userdata) {
    auto* state = static_cast<AppState*>(userdata);
    EmitLifecycleEvent(state, "first-frame-presented");
}

void ActivatedCallback(void* userdata) {
    auto* state = static_cast<AppState*>(userdata);
    EmitLifecycleEvent(state, "activated");
}

std::uint32_t ClampRenderExtent(std::uint32_t value, std::uint32_t fallback) {
    if (value == 0) value = fallback;
    return std::clamp<std::uint32_t>(value, 500u, 65535u);
}

} // namespace

int main(int argc, char** argv) {
    static rstd::log::EnvLogger logger;
    rstd::log::set_logger(logger);
    rstd::log::set_max_level(logger.filter());

    InstallCrashHandler();

    Options options;
    if (! ParseArgs(argc, argv, options)) return 1;

#if defined(__APPLE__)
    setenv("MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER", "1", /*overwrite=*/0);
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "1", /*overwrite=*/0);
#endif

    DesktopHandle      desktop;
    sr::SceneWallpaper wallpaper;
    StopTimer          run_timer;
    AppState           state;
    state.wallpaper = &wallpaper;

    sr::host::DesktopConfig desktop_config {
        .title         = "SceneRenderer Wallpaper",
        .input_hz      = options.input_hz,
        .screen_index  = options.screen,
        .deferred_show = options.deferred_show,
    };
    sr::host::DesktopCallbacks callbacks {
        .mouse_move            = MouseMoveCallback,
        .mouse_button          = MouseButtonCallback,
        .mouse_enter           = MouseEnterCallback,
        .closed                = nullptr,
        .first_frame_presented = FirstFramePresentedCallback,
        .activated             = ActivatedCallback,
        .userdata              = &state,
    };
    desktop.reset(sr::host::DesktopCreate(&desktop_config, callbacks));
    state.desktop = desktop.get();
    if (! desktop) {
        std::cerr << "Failed to create desktop wallpaper host\n";
        return 1;
    }

    std::uint32_t render_width  = sr::host::DesktopPixelWidth(desktop.get());
    std::uint32_t render_height = sr::host::DesktopPixelHeight(desktop.get());
    if (options.resolution) {
        render_width  = options.resolution->width;
        render_height = options.resolution->height;
    }

    if (! wallpaper.init()) {
        std::cerr << "Failed to initialize SceneWallpaper runtime\n";
        return 1;
    }

    sr::SceneWallpaperConfig config;
    config.assets_dir        = options.assets_dir;
    config.source_pkg_path   = options.scene_pkg;
    config.graphviz          = options.graphviz;
    config.fps               = options.fps;
    config.muted             = options.muted;
    config.spectrum_enabled = options.spectrum_enabled;
    config.external_spectrum = options.external_spectrum;
    if (options.cache_dir.empty()) {
        config.cache_dir = sr::platform::GetCachePath("SceneRenderer");
    } else {
        config.cache_dir = options.cache_dir;
    }

    if (! LoadUserProperties(options.user_properties, config)) {
        return 1;
    }

#if defined(__APPLE__)
    SceneRendererSetLiveMetalFrameCallback(LiveMetalFrameCallback, &state);
    LiveMetalFrameCallbackGuard live_metal_guard;
#endif

    sr::RenderInitInfo info;
    info.enable_valid_layer = options.valid_layer;
#if defined(__APPLE__)
    info.offscreen = true;
#else
    info.offscreen = false;
    sr::host::DesktopSurfaceInfo surface_info;
    if (! sr::host::DesktopGetSurfaceInfo(desktop.get(), surface_info)) {
        std::cerr << "Failed to create desktop Vulkan surface info\n";
        return 1;
    }
    info.surface_info.instanceExts = std::move(surface_info.instance_extensions);
    info.surface_info.createSurfaceOp = std::move(surface_info.create_surface);
#endif
    info.width           = ClampRenderExtent(render_width, 1920);
    info.height          = ClampRenderExtent(render_height, 1080);
    info.msaa_samples    = options.msaa;
    info.redraw_callback = [desktop = desktop.get()]() {
        sr::host::DesktopWake(desktop);
    };

    wallpaper.configure(std::move(config));
    wallpaper.initVulkan(std::move(info));

    if (options.mouse_position) {
        wallpaper.mouseEnter(true);
        wallpaper.mouseInput((*options.mouse_position)[0], (*options.mouse_position)[1]);
    }

    if (options.run_seconds > 0) {
        run_timer.start(desktop.get(), options.run_seconds);
    }

    // Live control channel: Mirage.app pipes JSON commands on stdin to drive
    // property edits / pause / volume without restarting. EOF (parent died)
    // stops the run loop so the wallpaper never outlives its owner.
    //
    // Wait for Vulkan init AND scene load to finish before starting the stdin
    // control thread; otherwise a race between RenderInit/LoadScene message
    // dispatch and a premature stdin EOF (triggering NSApp stop / cleanup) can
    // cause "Sender::acquire on null" panics in the mpsc channel layer.
    if (! wallpaper.waitVulkanInited(30000)) {
        std::cerr << "Vulkan initialization timed out\n";
        return 1;
    }
    EmitLifecycleEvent(&state, "vulkan-ready");
    {
        using clock   = std::chrono::steady_clock;
        auto deadline = clock::now() + std::chrono::seconds(30);
        while (clock::now() < deadline) {
            if (wallpaper.sceneReady()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (! wallpaper.sceneReady()) {
            std::cerr << "Scene load timed out\n";
            return 1;
        }
    }
    EmitLifecycleEvent(&state, "scene-ready");
    std::optional<mirage::SceneControlChannel> control;
    if (options.control_stdin) {
        void* desktop_handle = desktop.get();
        control.emplace(
            wallpaper,
            [desktop_handle]() { sr::host::DesktopStop(desktop_handle); },
            [desktop_handle]() { sr::host::DesktopActivate(desktop_handle); });
        control->start();
    }

    const int ok = sr::host::DesktopRun(desktop.get());

    if (control) control->stop();
    run_timer.stop();
    state.wallpaper = nullptr;
    state.desktop = nullptr;
    return ok ? 0 : 1;
}
