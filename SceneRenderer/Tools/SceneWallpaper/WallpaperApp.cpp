#include "../../Sources/SceneRenderer/Host/macOS/MacDesktopHost.h"
#include "ControlChannel.h"
#include "SceneSnapshot.h"

#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/event.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <xlocale.h>

import sr.json;
import rstd.cppstd;
import rstd.log;
import sr.scene_wallpaper;
import sr.utils;

// Crash handler: print backtrace on fatal signals
namespace {
void CrashHandler(int sig) {
    const char* name = "UNKNOWN";
    switch (sig) {
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGABRT: name = "SIGABRT"; break;
    case SIGBUS:  name = "SIGBUS";  break;
    case SIGILL:  name = "SIGILL";  break;
    case SIGFPE:  name = "SIGFPE";  break;
    }
    std::cerr << "[SceneWallpaper] CRASH: signal " << sig << " (" << name << ")" << std::endl;
    void* callstack[64];
    int frames = backtrace(callstack, 64);
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

// Terminate when the parent dies.
//
// This renderer is spawned as a child of Mirage.app and driven over stdin. If
// the parent crashes or is force-quit, an orphaned renderer keeps drawing a
// fullscreen desktop-level window that the user has no way to close, so the
// child must not outlive the parent. Stdin EOF already covers a clean parent
// exit (see SceneControlChannel), but the control channel is optional and is
// only started after Vulkan init + scene load, so it does not cover the
// startup window or a --control-stdin-less run.
//
// kqueue/EVFILT_PROC/NOTE_EXIT is the reliable macOS mechanism: it fires as
// soon as the watched pid exits, regardless of how it died. If registration
// fails (parent already gone, pid reused, kqueue unavailable) we fall back to
// polling getppid(), which becomes 1 once we are reparented to launchd.
void ParentDeathWatchdogLoop(pid_t parent) {
    const int kq = ::kqueue();
    if (kq >= 0) {
        struct kevent change {};
        EV_SET(&change, (uintptr_t)parent, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, nullptr);
        if (::kevent(kq, &change, 1, nullptr, 0, nullptr) == 0) {
            for (;;) {
                struct kevent event {};
                const int     n = ::kevent(kq, nullptr, 0, &event, 1, nullptr);
                if (n < 0 && errno == EINTR) continue;
                break; // n > 0: NOTE_EXIT delivered. n < 0: the filter is gone.
            }
            ::close(kq);
            return;
        }
        ::close(kq);
    }
    while (::getppid() == parent) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void StartParentDeathWatchdog() {
    const pid_t parent = ::getppid();
    // Already reparented to launchd (or launched directly by it): there is no
    // owning parent to follow, so watching would mean quitting immediately.
    if (parent <= 1) return;
    std::thread([parent]() {
        ParentDeathWatchdogLoop(parent);
        std::cerr << "[SceneWallpaper] parent process exited; shutting down" << std::endl;
        SceneRendererMacDesktopStop(nullptr);
        // The graceful path needs a running NSApp run loop, which may not have
        // started yet (the parent can die during Vulkan/scene init while the
        // main thread is still blocked). Give it a moment, then leave for real
        // rather than linger as an unclosable fullscreen window.
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::_Exit(0);
    }).detach();
}
} // namespace

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
    double                    render_scale { 1.0 };
    std::uint32_t             screen { 0 };
    std::uint32_t             display_id { 0 };
    int                       run_seconds { 0 };
    bool                      valid_layer { false };
    bool                      graphviz { false };
    bool                      muted { false };
    bool                      control_stdin { false };
    bool                      deferred_show { false };
    bool                      spectrum_enabled { true };
    bool                      external_spectrum { false };
    bool                      load_from_memory { false };
    bool                      metalfx { false };
};

struct AppState {
    sr::SceneWallpaper* wallpaper { nullptr };
    void*               desktop { nullptr };
    std::chrono::steady_clock::time_point started_at { std::chrono::steady_clock::now() };
};

std::mutex& LifecycleOutputMutex() {
    static std::mutex mutex;
    return mutex;
}

void EmitLifecycleEvent(const AppState* state, std::string_view event) {
    const auto elapsed = state == nullptr
                             ? 0
                             : std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - state->started_at)
                                   .count();
    std::lock_guard lock(LifecycleOutputMutex());
    std::cout << "{\"event\":\"" << event << "\",\"elapsed_ms\":" << elapsed << "}\n"
              << std::flush;
}

void EmitAudioDemand(bool needed) {
    std::lock_guard lock(LifecycleOutputMutex());
    std::cout << "{\"event\":\"audio-demand\",\"needed\":"
              << (needed ? "true" : "false") << "}\n" << std::flush;
}

std::string JsonEscaped(std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string           out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (byte < 0x20) {
            out += "\\u00";
            out.push_back(kHex[(byte >> 4) & 0xF]);
            out.push_back(kHex[byte & 0xF]);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void EmitUserShortcut(std::string_view name, std::string_view target) {
    if (target.empty()) return;
    std::lock_guard lock(LifecycleOutputMutex());
    std::cout << "{\"event\":\"open-shortcut\",\"name\":\"" << JsonEscaped(name)
              << "\",\"value\":\"" << JsonEscaped(target) << "\"}\n" << std::flush;
}

/// Answers a {"cmd":"snapshot"} request. The token is echoed verbatim so the app
/// can match the reply; it is a UUID the app generated, but quote-escape it
/// anyway rather than trusting the shape of an inbound string.
void EmitSnapshotDone(const std::string& token, bool ok) {
    std::string escaped;
    escaped.reserve(token.size());
    for (const char c : token) {
        if (c == '"' || c == '\\') escaped.push_back('\\');
        // Control characters would produce invalid JSON; a UUID has none.
        if (static_cast<unsigned char>(c) < 0x20) continue;
        escaped.push_back(c);
    }
    std::lock_guard lock(LifecycleOutputMutex());
    std::cout << "{\"event\":\"snapshot-done\",\"token\":\"" << escaped
              << "\",\"ok\":" << (ok ? "true" : "false") << "}\n" << std::flush;
}

void PrintUsage(const char* argv0) {
    std::cerr
        << "Usage: " << (argv0 != nullptr ? argv0 : "SceneWallpaper")
        << " [options] <assets> <scene.pkg>\n\n"
        << "Options:\n"
        << "  -f, --fps N                 Render FPS (default 30)\n"
        << "  -R, --resolution WxH        Override render resolution\n"
        << "      --render-scale S        Render scale 0.25..1.0 (default 1.0)\n"
        << "      --metalfx               Enable MetalFX Spatial scaling\n"
        << "  -C, --cache-path DIR        Cache directory\n"
        << "  -M, --msaa N                MSAA samples for screen RT\n"
        << "  -P, --user-properties FILE  JSON object of WE user properties\n"
        << "  -V, --valid-layer           Enable Vulkan validation layer\n"
        << "  -G, --graphviz              Emit graph.dot for the render graph\n"
        << "      --mouse-position X,Y    Initial normalized mouse position\n"
        << "      --input-hz N            Desktop mouse polling rate (default 60)\n"
        << "      --screen N              Screen index to cover (default 0 = main)\n"
        << "      --display-id N          Core Graphics display ID to cover\n"
        << "      --muted                 Start with audio muted\n"
        << "      --control-stdin         Accept live JSON control commands on stdin\n"
        << "      --deferred-show         Keep the window transparent until activated\n"
        << "      --no-spectrum           Disable audio response\n"
        << "      --external-spectrum     Receive spectrum from stdin\n"
        << "      --load-from-memory      Keep the wallpaper package in memory\n"
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
    static locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    errno = 0;
    const double value = c_locale != nullptr
                             ? strtod_l(value_text.c_str(), &parsed_end, c_locale)
                             : std::strtod(value_text.c_str(), &parsed_end);
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
        } else if (arg == "--deferred-show") {
            out.deferred_show = true;
        } else if (arg == "--no-spectrum") {
            out.spectrum_enabled = false;
        } else if (arg == "--external-spectrum") {
            out.external_spectrum = true;
        } else if (arg == "--load-from-memory") {
            out.load_from_memory = true;
        } else if (arg == "--metalfx") {
            out.metalfx = true;
        } else if (arg == "--screen") {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.screen)) return false;
        } else if (arg == "--display-id") {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.display_id) || out.display_id == 0) return false;
        } else if (arg == "-f" || arg == "--fps") {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.fps)) return false;
        } else if (arg == "-R" || arg == "--resolution") {
            const char* value = require_value(i, arg);
            if (value == nullptr) return false;
            out.resolution = ParseResolution(value);
            if (! out.resolution) return false;
        } else if (arg == "--render-scale") {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseDouble(value, out.render_scale)) return false;
            out.render_scale = std::clamp(out.render_scale, 0.25, 1.0);
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

void ActivationFailedCallback(void* userdata) {
    auto* state = static_cast<AppState*>(userdata);
    EmitLifecycleEvent(state, "activation-failed");
}

void DeactivatedCallback(void* userdata) {
    auto* state = static_cast<AppState*>(userdata);
    EmitLifecycleEvent(state, "deactivated");
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

    setenv("MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER", "1", /*overwrite=*/0);
    // MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS is deliberately NOT set: it makes
    // every vkQueueSubmit block the calling thread until the Metal command
    // buffer completes, which stalls the host on top of the frame's own fence
    // wait and defeats any CPU/GPU overlap. The renderer's synchronisation is
    // explicit (fences + semaphores), so the asynchronous default is correct.
    // Still overrideable from the shell for debugging.

    auto     wallpaper = std::make_unique<sr::SceneWallpaper>();
    AppState state;
    state.wallpaper = wallpaper.get();

    SceneRendererMacDesktopConfig desktop_config {
        .title        = "SceneRenderer Wallpaper",
        .input_hz     = options.input_hz,
        .screen_index = options.screen,
        .display_id   = options.display_id,
        .deferred_show = options.deferred_show,
    };
    SceneRendererMacDesktopCallbacks callbacks {
        .mouse_move   = MouseMoveCallback,
        .mouse_button = MouseButtonCallback,
        .mouse_enter  = MouseEnterCallback,
        .closed       = nullptr,
        .first_frame_presented = FirstFramePresentedCallback,
        .activated    = ActivatedCallback,
        .activation_failed = ActivationFailedCallback,
        .deactivated  = DeactivatedCallback,
        .userdata     = &state,
    };
    state.desktop = SceneRendererMacDesktopCreate(&desktop_config, callbacks);
    if (state.desktop == nullptr) {
        std::cerr << "Failed to create macOS desktop wallpaper host\n";
        return 1;
    }

    // From here on there is a window on screen; never let it outlive the
    // parent that owns it. Installed before the (long) Vulkan + scene init so
    // a parent that dies during startup is covered too.
    StartParentDeathWatchdog();

    std::uint32_t render_width  = SceneRendererMacDesktopPixelWidth(state.desktop);
    std::uint32_t render_height = SceneRendererMacDesktopPixelHeight(state.desktop);
    if (options.resolution) {
        render_width  = options.resolution->width;
        render_height = options.resolution->height;
    } else {
        render_width = static_cast<std::uint32_t>(
            std::lround(static_cast<double>(render_width) * options.render_scale));
        render_height = static_cast<std::uint32_t>(
            std::lround(static_cast<double>(render_height) * options.render_scale));
    }

    if (! wallpaper->init()) {
        std::cerr << "Failed to initialize SceneWallpaper runtime\n";
        wallpaper.reset();
        state.wallpaper = nullptr;
        SceneRendererMacDesktopDestroy(state.desktop);
        return 1;
    }

    sr::SceneWallpaperConfig config;
    config.assets_dir      = options.assets_dir;
    config.source_pkg_path = options.scene_pkg;
    config.graphviz        = options.graphviz;
    config.fps             = options.fps;
    config.muted           = options.muted;
    config.spectrum_enabled = options.spectrum_enabled;
    config.external_spectrum = options.external_spectrum;
    config.load_from_memory = options.load_from_memory;
    if (options.cache_dir.empty())
        config.cache_dir = sr::platform::GetCachePath("SceneRenderer");
    else
        config.cache_dir = options.cache_dir;

    if (! LoadUserProperties(options.user_properties, config)) {
        wallpaper.reset();
        state.wallpaper = nullptr;
        SceneRendererMacDesktopDestroy(state.desktop);
        return 1;
    }

    sr::RenderInitInfo info;
    info.enable_valid_layer = options.valid_layer;
    const bool use_metalfx = options.metalfx &&
                             SceneRendererMacDesktopPrepareMetalFX(state.desktop);
    info.offscreen          = use_metalfx;
    info.width              = ClampRenderExtent(render_width, 1920);
    info.height             = ClampRenderExtent(render_height, 1080);
    info.msaa_samples       = options.msaa;
    info.redraw_callback    = [&state]() {
        SceneRendererMacDesktopWake(state.desktop);
    };
    info.failure_callback = [&state](VkResult) {
        EmitLifecycleEvent(&state, "renderer-error");
        SceneRendererMacDesktopStop(state.desktop);
    };
    if (use_metalfx) {
        info.metal_frame_callback = [&state](void* texture, void* command_queue,
                                             std::uint32_t width,
                                             std::uint32_t height) {
            SceneRendererMacDesktopPresentMetalFrame(state.desktop, texture, command_queue,
                                                      width, height);
        };
    } else {
        void* metal_layer = SceneRendererMacDesktopMetalLayer(state.desktop);
        if (metal_layer == nullptr) {
            std::cerr << "Failed to obtain CAMetalLayer for Vulkan surface\n";
            wallpaper.reset();
            state.wallpaper = nullptr;
            SceneRendererMacDesktopDestroy(state.desktop);
            return 1;
        }
        info.surface_info.instanceExts.emplace_back(VK_KHR_SURFACE_EXTENSION_NAME);
        info.surface_info.instanceExts.emplace_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
        info.surface_info.createSurfaceOp =
            [metal_layer](VkInstance instance, VkSurfaceKHR* surface) {
                auto create_surface = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
                    vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
                if (create_surface == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;
                VkMetalSurfaceCreateInfoEXT create_info {
                    .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
                    .pNext = nullptr,
                    .flags = 0,
                    .pLayer = metal_layer,
                };
                return create_surface(instance, &create_info, nullptr, surface);
            };
    }

    wallpaper->setOnAudioDemand(EmitAudioDemand);
    wallpaper->setOnUserShortcut(EmitUserShortcut);
    wallpaper->configure(std::move(config));
    wallpaper->initVulkan(std::move(info));

    if (options.mouse_position) {
        wallpaper->mouseEnter(true);
        wallpaper->mouseInput((*options.mouse_position)[0], (*options.mouse_position)[1]);
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
    //
    // Wait for Vulkan init AND scene load to finish before starting the stdin
    // control thread — otherwise a race between RenderInit/LoadScene message
    // dispatch and a premature stdin EOF (triggering NSApp stop / cleanup) can
    // cause "Sender::acquire on null" panics in the mpsc channel layer.
    if (! wallpaper->waitVulkanInited(30000)) {
        std::cerr << "Vulkan initialization timed out\n";
        wallpaper.reset();
        state.wallpaper = nullptr;
        SceneRendererMacDesktopDestroy(state.desktop);
        return 1;
    }
    EmitLifecycleEvent(&state, "vulkan-ready");
    {
        using clock   = std::chrono::steady_clock;
        auto deadline = clock::now() + std::chrono::seconds(30);
        while (clock::now() < deadline) {
            if (wallpaper->sceneReady()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (! wallpaper->sceneReady()) {
            std::cerr << "Scene load timed out\n";
            wallpaper.reset();
            state.wallpaper = nullptr;
            SceneRendererMacDesktopDestroy(state.desktop);
            return 1;
        }
    }
    EmitLifecycleEvent(&state, "scene-ready");
    std::optional<mirage::SceneControlChannel> control;
    if (options.control_stdin) {
        void* desktop = state.desktop;
        control.emplace(
            *wallpaper,
            [desktop]() { SceneRendererMacDesktopStop(desktop); },
            [desktop]() { SceneRendererMacDesktopActivate(desktop); },
            [desktop]() { SceneRendererMacDesktopDeactivate(desktop); },
            [](const std::string& path, const std::string& token) {
                const bool ok = ! path.empty() && mirage::WriteSceneSnapshot(path);
                EmitSnapshotDone(token, ok);
            });
        control->start();
    }

    const int ok = SceneRendererMacDesktopRun(state.desktop);

    if (control) {
        control->stop();
        control.reset();
    }
    state.wallpaper = nullptr;
    wallpaper.reset();
    SceneRendererMacDesktopDestroy(state.desktop);
    state.desktop = nullptr;
    return ok ? 0 : 1;
}
