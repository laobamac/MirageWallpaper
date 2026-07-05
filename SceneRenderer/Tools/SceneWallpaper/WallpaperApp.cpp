#include "../../Sources/SceneRenderer/Host/macOS/MacDesktopHost.h"
#include "ControlChannel.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

import nlohmann.json;
import rstd.log;
import sr.scene_wallpaper;
import sr.utils;

extern "C" void SceneRendererSetLiveMetalFrameCallback(
    void (*cb)(void*, std::uint32_t, std::uint32_t, void*), void* userdata);

namespace
{

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
};

struct AppState {
    sr::SceneWallpaper* wallpaper { nullptr };
    void*               desktop { nullptr };
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
    auto   xr = std::from_chars(xs.data(), xs.data() + xs.size(), x);
    auto   yr = std::from_chars(ys.data(), ys.data() + ys.size(), y);
    if (xr.ec != std::errc {} || yr.ec != std::errc {}) return std::nullopt;
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
        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else if (arg == "-V" || arg == "--valid-layer") {
            out.valid_layer = true;
        } else if (arg == "-G" || arg == "--graphviz") {
            out.graphviz = true;
        } else if (arg == "--muted") {
            out.muted = true;
        } else if (arg == "--control-stdin") {
            out.control_stdin = true;
        } else if (arg == "--screen") {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.screen)) return false;
        } else if (arg == "-f" || arg == "--fps") {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.fps)) return false;
        } else if (arg == "-R" || arg == "--resolution") {
            const char* value = require_value(i, arg);
            if (value == nullptr) return false;
            out.resolution = ParseResolution(value);
            if (! out.resolution) return false;
        } else if (arg == "-C" || arg == "--cache-path") {
            const char* value = require_value(i, arg);
            if (value == nullptr) return false;
            out.cache_dir = value;
        } else if (arg == "-M" || arg == "--msaa") {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.msaa)) return false;
        } else if (arg == "-P" || arg == "--user-properties") {
            const char* value = require_value(i, arg);
            if (value == nullptr) return false;
            out.user_properties = value;
        } else if (arg == "--mouse-position") {
            const char* value = require_value(i, arg);
            if (value == nullptr) return false;
            out.mouse_position = ParseMousePosition(value);
            if (! out.mouse_position) return false;
        } else if (arg == "--input-hz") {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.input_hz)) return false;
        } else if (arg == "--run-seconds") {
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
    auto parsed = nlohmann::json::parse(ss.str(),
                                        /*cb*/ nullptr,
                                        /*allow_exceptions*/ false,
                                        /*ignore_comments*/ true);
    if (! parsed.is_object()) {
        std::cerr << "--user-properties: '" << path << "' is not a JSON object\n";
        return false;
    }
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        config.user_properties.emplace(it.key(), it.value());
    }
    return true;
}

void LiveMetalFrameCallback(void* texture, std::uint32_t width, std::uint32_t height,
                            void* userdata) {
    auto* state = static_cast<AppState*>(userdata);
    if (state == nullptr || state->desktop == nullptr) return;
    SceneRendererMacDesktopPresent(state->desktop, texture, width, height);
}

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

std::uint32_t ClampRenderExtent(std::uint32_t value, std::uint32_t fallback) {
    if (value == 0) value = fallback;
    return std::clamp<std::uint32_t>(value, 500u, 65535u);
}

} // namespace

int main(int argc, char** argv) {
    static rstd::log::EnvLogger logger;
    rstd::log::set_logger(logger);
    rstd::log::set_max_level(logger.filter());

    Options options;
    if (! ParseArgs(argc, argv, options)) return 1;

    setenv("MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER", "1", /*overwrite=*/0);
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "1", /*overwrite=*/0);

    sr::SceneWallpaper wallpaper;
    AppState           state;
    state.wallpaper = &wallpaper;

    SceneRendererMacDesktopConfig desktop_config {
        .title        = "SceneRenderer Wallpaper",
        .input_hz     = options.input_hz,
        .screen_index = options.screen,
    };
    SceneRendererMacDesktopCallbacks callbacks {
        .mouse_move   = MouseMoveCallback,
        .mouse_button = MouseButtonCallback,
        .mouse_enter  = MouseEnterCallback,
        .closed       = nullptr,
        .userdata     = &state,
    };
    state.desktop = SceneRendererMacDesktopCreate(&desktop_config, callbacks);
    if (state.desktop == nullptr) {
        std::cerr << "Failed to create macOS desktop wallpaper host\n";
        return 1;
    }

    std::uint32_t render_width  = SceneRendererMacDesktopPixelWidth(state.desktop);
    std::uint32_t render_height = SceneRendererMacDesktopPixelHeight(state.desktop);
    if (options.resolution) {
        render_width  = options.resolution->width;
        render_height = options.resolution->height;
    }

    if (! wallpaper.init()) {
        std::cerr << "Failed to initialize SceneWallpaper runtime\n";
        SceneRendererMacDesktopDestroy(state.desktop);
        return 1;
    }

    sr::SceneWallpaperConfig config;
    config.assets_dir      = options.assets_dir;
    config.source_pkg_path = options.scene_pkg;
    config.graphviz        = options.graphviz;
    config.fps             = options.fps;
    config.muted           = options.muted;
    if (options.cache_dir.empty())
        config.cache_dir = sr::platform::GetCachePath("SceneRenderer");
    else
        config.cache_dir = options.cache_dir;

    if (! LoadUserProperties(options.user_properties, config)) {
        SceneRendererMacDesktopDestroy(state.desktop);
        return 1;
    }

    SceneRendererSetLiveMetalFrameCallback(LiveMetalFrameCallback, &state);

    sr::RenderInitInfo info;
    info.enable_valid_layer = options.valid_layer;
    info.offscreen          = true;
    info.width              = ClampRenderExtent(render_width, 1920);
    info.height             = ClampRenderExtent(render_height, 1080);
    info.msaa_samples       = options.msaa;
    info.redraw_callback    = [&state]() {
        SceneRendererMacDesktopWake(state.desktop);
    };

    wallpaper.configure(std::move(config));
    wallpaper.initVulkan(std::move(info));

    if (options.mouse_position) {
        wallpaper.mouseEnter(true);
        wallpaper.mouseInput((*options.mouse_position)[0], (*options.mouse_position)[1]);
    }

    if (options.run_seconds > 0) {
        void* desktop = state.desktop;
        std::thread([desktop, seconds = options.run_seconds]() {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            SceneRendererMacDesktopStop(desktop);
        }).detach();
    }

    // Live control channel: Mirage.app pipes JSON commands on stdin to drive
    // property edits / pause / volume without restarting. EOF (parent died)
    // stops the run loop so the wallpaper never outlives its owner.
    std::optional<mirage::SceneControlChannel> control;
    if (options.control_stdin) {
        void* desktop = state.desktop;
        control.emplace(wallpaper, [desktop]() { SceneRendererMacDesktopStop(desktop); });
        control->start();
    }

    const int ok = SceneRendererMacDesktopRun(state.desktop);

    if (control) control->stop();
    SceneRendererSetLiveMetalFrameCallback(nullptr, nullptr);
    SceneRendererMacDesktopDestroy(state.desktop);
    state.desktop = nullptr;
    return ok ? 0 : 1;
}
