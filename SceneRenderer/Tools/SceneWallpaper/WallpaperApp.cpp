#include "DesktopHost.h"
#include "ControlChannel.h"
#if defined(SCENERENDERER_MIRAGE_DISPLAY)
#include <mirage_display_producer.h>
#include <mirage_display_vulkan_export.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
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
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(SCENERENDERER_MIRAGE_DISPLAY)
#include <poll.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif
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
#if defined(SCENERENDERER_MIRAGE_DISPLAY)
import sr.vulkan;
#endif

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
    std::string               display_output_id;
    std::string               display_socket;
    std::optional<Resolution> resolution;
    std::optional<std::array<double, 2>> mouse_position;
    std::uint32_t             fps { 30 };
    std::uint32_t             input_hz { 60 };
    std::uint32_t             msaa { 1 };
    double                    render_scale { 1.0 };
    std::uint32_t             screen { 0 };
    int                       run_seconds { 0 };
    bool                      valid_layer { false };
    bool                      graphviz { false };
    bool                      muted { false };
    bool                      control_stdin { false };
    bool                      deferred_show { false };
    bool                      spectrum_enabled { true };
    bool                      external_spectrum { false };
    bool                      load_from_memory { false };
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

    void start(std::function<void()> stop_callback, int seconds) {
        stop();
        if (!stop_callback || seconds <= 0) return;

        {
            std::lock_guard lock(mutex_);
            stop_requested_ = false;
        }
        worker_ = std::thread([this, stop_callback = std::move(stop_callback), seconds]() {
            std::unique_lock lock(mutex_);
            const bool stopped = cv_.wait_for(lock, std::chrono::seconds(seconds), [this]() {
                return stop_requested_;
            });
            lock.unlock();
            if (! stopped) stop_callback();
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
        << "      --display-output-id ID  Stable DE output identity for protocol mode\n"
        << "      --display-socket PATH   mirage-display broker socket\n"
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
        } else if (arg == "--screen") {
            const char* value = require_value(i, arg);
            if (value == nullptr || ! ParseUInt(value, out.screen)) return false;
        } else if (arg == "--display-output-id") {
            const char* value = require_value(i, arg);
            if (value == nullptr || *value == '\0') return false;
            out.display_output_id = value;
        } else if (arg == "--display-socket") {
            const char* value = require_value(i, arg);
            if (value == nullptr || *value == '\0') return false;
            out.display_socket = value;
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

#if defined(SCENERENDERER_MIRAGE_DISPLAY)

constexpr std::uint32_t MirageDrmFormat(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

constexpr std::uint32_t MirageDrmXrgb8888 = MirageDrmFormat('X', 'R', '2', '4');
constexpr std::uint32_t MirageDrmArgb8888 = MirageDrmFormat('A', 'R', '2', '4');
constexpr std::uint32_t MirageDrmXbgr8888 = MirageDrmFormat('X', 'B', '2', '4');
constexpr std::uint32_t MirageDrmAbgr8888 = MirageDrmFormat('A', 'B', '2', '4');

class MirageProtocolHost {
public:
    MirageProtocolHost(std::string socket_path, std::string output_id,
                       sr::host::DesktopCallbacks callbacks)
        : m_socket_path(std::move(socket_path)),
          m_output_id(std::move(output_id)),
          m_callbacks(callbacks) {}

    ~MirageProtocolHost() { stop(); }

    MirageProtocolHost(const MirageProtocolHost&) = delete;
    MirageProtocolHost& operator=(const MirageProtocolHost&) = delete;

    bool start() {
        if (m_socket_path.empty() || m_output_id.empty()) return false;
        {
            std::lock_guard lock(m_producer_mutex);
            if (!connectProducerLocked()) return false;
        }
        m_running.store(true);
        m_io_thread = std::thread([this] { ioLoop(); });
        std::unique_lock state_lock(m_state_mutex);
        return m_state_cv.wait_for(state_lock, std::chrono::seconds(10), [this] {
            return m_config_version != 0 || !m_running.load();
        }) && m_config_version != 0;
    }

    void stop() {
        const bool was_running = m_running.exchange(false);
        m_state_cv.notify_all();
        m_run_cv.notify_all();
        if (was_running) {
            std::lock_guard lock(m_producer_mutex);
            if (m_producer != nullptr) md_producer_close(m_producer);
        }
        if (m_io_thread.joinable()) m_io_thread.join();
        std::lock_guard lock(m_producer_mutex);
        if (m_producer != nullptr) {
            md_producer_free(m_producer);
            m_producer = nullptr;
        }
    }

    int run() {
        std::unique_lock lock(m_run_mutex);
        m_run_cv.wait(lock, [this] { return !m_running.load(); });
        return 1;
    }

    bool snapshotConfig(std::uint64_t last_version, std::uint64_t last_epoch,
                        md_producer_config_t& config, std::uint64_t& version,
                        std::uint64_t& epoch) const {
        std::lock_guard lock(m_state_mutex);
        if (m_config_version == 0 ||
            (m_config_version == last_version && m_connection_epoch == last_epoch)) {
            return false;
        }
        config = m_config;
        version = m_config_version;
        epoch = m_connection_epoch;
        return true;
    }

    bool currentConfig(md_producer_config_t& config, std::uint64_t& version,
                       std::uint64_t& epoch) const {
        return snapshotConfig(0, 0, config, version, epoch);
    }

    std::uint64_t takeRetireGeneration() {
        std::lock_guard lock(m_state_mutex);
        return std::exchange(m_retire_generation, UINT64_C(0));
    }

    std::uint64_t nextGeneration() { return m_next_generation.fetch_add(1); }

    int offerPool(const md_buffer_pool_t* pool) {
        if (pool == nullptr) return MD_ERR_INVALID;
        std::lock_guard lock(m_producer_mutex);
        if (m_producer == nullptr ||
            md_producer_connection_state(m_producer) != MD_CONNECTION_READY) {
            return MD_ERR_DISCONNECTED;
        }
        int result = md_producer_offer_buffers(m_producer, pool);
        if (result != MD_OK) return result;
        md_display_config_t display_config {
            .generation = pool->generation,
            .source = {0.0f, 0.0f, static_cast<float>(pool->width),
                       static_cast<float>(pool->height)},
            .destination = {0.0f, 0.0f, static_cast<float>(pool->width),
                            static_cast<float>(pool->height)},
            .transform = MD_TRANSFORM_NORMAL,
            .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
        };
        return md_producer_set_config(m_producer, &display_config);
    }

    int submitFrame(std::uint64_t generation, std::uint32_t index, std::uint64_t sequence,
                    int acquire_fd, int release_fd) {
        std::lock_guard lock(m_producer_mutex);
        if (m_producer == nullptr ||
            md_producer_connection_state(m_producer) != MD_CONNECTION_READY) {
            if (acquire_fd >= 0) close(acquire_fd);
            if (release_fd >= 0) close(release_fd);
            return MD_ERR_DISCONNECTED;
        }
        return md_producer_submit_frame(m_producer, generation, index, sequence,
                                        acquire_fd, release_fd);
    }

    void retireDone(std::uint64_t generation) {
        std::lock_guard lock(m_producer_mutex);
        if (m_producer != nullptr &&
            md_producer_connection_state(m_producer) == MD_CONNECTION_READY) {
            (void)md_producer_retire_done(m_producer, generation);
        }
    }

    void notifyFirstFrame() {
        if (!m_first_frame.exchange(true) && m_callbacks.first_frame_presented != nullptr) {
            m_callbacks.first_frame_presented(m_callbacks.userdata);
        }
    }

    void notifyActivated() {
        if (m_callbacks.activated != nullptr) m_callbacks.activated(m_callbacks.userdata);
    }

private:
    static void OnConnected(void* opaque, std::uint64_t, std::uint64_t) {
        auto* self = static_cast<MirageProtocolHost*>(opaque);
        {
            std::lock_guard lock(self->m_state_mutex);
            ++self->m_connection_epoch;
            self->m_retire_generation = 0;
        }
        self->m_state_cv.notify_all();
    }

    static void OnOutputConfig(void* opaque, const md_producer_config_t* config) {
        auto* self = static_cast<MirageProtocolHost*>(opaque);
        if (config == nullptr) return;
        {
            std::lock_guard lock(self->m_state_mutex);
            self->m_config = *config;
            ++self->m_config_version;
        }
        self->m_state_cv.notify_all();
    }

    static void OnRetire(void* opaque, std::uint64_t generation) {
        auto* self = static_cast<MirageProtocolHost*>(opaque);
        std::lock_guard lock(self->m_state_mutex);
        self->m_retire_generation = generation;
    }

    static void OnPointerEnter(void* opaque, const md_pointer_enter_t*) {
        auto* self = static_cast<MirageProtocolHost*>(opaque);
        if (self->m_callbacks.mouse_enter != nullptr) {
            self->m_callbacks.mouse_enter(1, self->m_callbacks.userdata);
        }
    }

    static void OnPointerLeave(void* opaque, std::uint64_t) {
        auto* self = static_cast<MirageProtocolHost*>(opaque);
        if (self->m_callbacks.mouse_enter != nullptr) {
            self->m_callbacks.mouse_enter(0, self->m_callbacks.userdata);
        }
    }

    static void OnPointerMotion(void* opaque, const md_pointer_motion_t* event) {
        auto* self = static_cast<MirageProtocolHost*>(opaque);
        if (event == nullptr || self->m_callbacks.mouse_move == nullptr) return;
        md_producer_config_t config {};
        {
            std::lock_guard lock(self->m_state_mutex);
            config = self->m_config;
        }
        if (config.physical_width == 0 || config.physical_height == 0) return;
        const double x = std::clamp(static_cast<double>(event->x) /
                                        static_cast<double>(config.physical_width),
                                    0.0, 1.0);
        const double y = std::clamp(static_cast<double>(event->y) /
                                        static_cast<double>(config.physical_height),
                                    0.0, 1.0);
        self->m_callbacks.mouse_move(x, y, self->m_callbacks.userdata);
    }

    static void OnPointerButton(void* opaque, const md_pointer_button_t* event) {
        auto* self = static_cast<MirageProtocolHost*>(opaque);
        if (event == nullptr || self->m_callbacks.mouse_button == nullptr) return;
        int button = -1;
        switch (event->button) {
        case 0x110u: button = 0; break;
        case 0x111u: button = 1; break;
        case 0x112u: button = 2; break;
        default: return;
        }
        self->m_callbacks.mouse_button(button,
                                       event->state == MD_BUTTON_PRESSED ? 1 : 0,
                                       self->m_callbacks.userdata);
    }

    static void OnPointerAxis(void*, const md_pointer_axis_t*) {}

    static void OnDisconnected(void* opaque, md_result_t, const char*) {
        auto* self = static_cast<MirageProtocolHost*>(opaque);
        self->m_state_cv.notify_all();
    }

    bool connectProducerLocked() {
        if (m_producer != nullptr) md_producer_free(m_producer);
        md_producer_callbacks_t callbacks {
            .on_connected = OnConnected,
            .on_output_config = OnOutputConfig,
            .on_retire_buffers = OnRetire,
            .on_pointer_enter = OnPointerEnter,
            .on_pointer_leave = OnPointerLeave,
            .on_pointer_motion = OnPointerMotion,
            .on_pointer_button = OnPointerButton,
            .on_pointer_axis = OnPointerAxis,
            .on_disconnected = OnDisconnected,
            .user_data = this,
        };
        m_producer = md_producer_new(&callbacks);
        if (m_producer == nullptr) return false;
        /* Offer only R8G8B8A8-mapped fourccs (XBGR/ABGR). The Qt Quick
         * Vulkan consumer's QSGVulkanTexture::fromNative assumes RGBA8 for
         * external images, so a B8G8R8A8 slot (XRGB/ARGB) would render with
         * swapped R/B channels. */
        const md_format_cap_t formats[] = {
            {.fourcc = MirageDrmXbgr8888, .plane_count = 1, .modifier = 0},
            {.fourcc = MirageDrmAbgr8888, .plane_count = 1, .modifier = 0},
        };
        md_producer_info_t info {
            .stable_output_id = m_output_id.c_str(),
            .kind = "scene",
            .drm_render_major = 0,
            .drm_render_minor = 0,
            .device_uuid = {},
            .driver_uuid = {},
            .formats = formats,
            .format_count = static_cast<std::uint32_t>(std::size(formats)),
        };
        const int result = md_producer_connect(m_producer, m_socket_path.c_str(),
                                               "SceneWallpaper", "0.1.0", &info, 3000);
        if (result == MD_OK) return true;
        md_producer_free(m_producer);
        m_producer = nullptr;
        return false;
    }

    void ioLoop() {
        while (m_running.load()) {
            int fd = -1;
            bool wants_write = false;
            {
                std::lock_guard lock(m_producer_mutex);
                if (m_producer != nullptr) {
                    fd = md_producer_get_fd(m_producer);
                    wants_write = md_producer_wants_writable(m_producer);
                }
            }
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::lock_guard lock(m_producer_mutex);
                if (m_running.load()) (void)connectProducerLocked();
                continue;
            }
            pollfd descriptor {
                .fd = fd,
                .events = static_cast<short>(POLLIN | (wants_write ? POLLOUT : 0)),
                .revents = 0,
            };
            int ready = poll(&descriptor, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            bool reconnect = ready < 0 ||
                             (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            if (!reconnect && ready > 0) {
                std::lock_guard lock(m_producer_mutex);
                if (m_producer == nullptr) continue;
                if ((descriptor.revents & POLLIN) != 0 &&
                    md_producer_dispatch(m_producer) < 0) {
                    reconnect = true;
                }
                if (!reconnect && (descriptor.revents & POLLOUT) != 0 &&
                    md_producer_handle_writable(m_producer) < 0) {
                    reconnect = true;
                }
            }
            if (reconnect && m_running.load()) {
                std::lock_guard lock(m_producer_mutex);
                if (m_producer != nullptr) {
                    md_producer_free(m_producer);
                    m_producer = nullptr;
                }
            }
        }
        m_run_cv.notify_all();
    }

    std::string m_socket_path;
    std::string m_output_id;
    sr::host::DesktopCallbacks m_callbacks;

    mutable std::mutex m_state_mutex;
    std::condition_variable m_state_cv;
    md_producer_config_t m_config {};
    std::uint64_t m_config_version { 0 };
    std::uint64_t m_connection_epoch { 0 };
    std::uint64_t m_retire_generation { 0 };
    std::atomic_uint64_t m_next_generation { 1 };
    std::atomic_bool m_first_frame { false };

    std::mutex m_producer_mutex;
    md_producer_t* m_producer { nullptr };
    std::atomic_bool m_running { false };
    std::thread m_io_thread;
    std::mutex m_run_mutex;
    std::condition_variable m_run_cv;
};

class MirageProtocolSwapchain final : public sr::ExSwapchain {
public:
    MirageProtocolSwapchain(MirageProtocolHost& host, VkInstance instance,
                            VkPhysicalDevice physical_device, VkDevice device,
                            VkQueue queue, std::uint32_t queue_family,
                            unsigned width, unsigned height)
        : m_host(host), m_device(device), m_width(width), m_height(height) {
        VkPhysicalDeviceDrmPropertiesEXT drm {};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 properties {};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &drm;
        vkGetPhysicalDeviceProperties2(physical_device, &properties);
        md_vk_export_context_t context {
            .instance = instance,
            .physical_device = physical_device,
            .device = device,
            .queue = queue,
            .queue_family_index = queue_family,
            .drm_render_fd = -1,
            .drm_render_major = drm.hasRender == VK_TRUE
                                    ? static_cast<std::uint32_t>(drm.renderMajor)
                                    : 0u,
            .drm_render_minor = drm.hasRender == VK_TRUE
                                    ? static_cast<std::uint32_t>(drm.renderMinor)
                                    : 0u,
        };
        m_exporter = md_vk_exporter_new(&context);
        md_producer_config_t config {};
        if (m_exporter != nullptr && m_host.currentConfig(config, m_config_version,
                                                          m_connection_epoch)) {
            rebuild(config);
        }
    }

    ~MirageProtocolSwapchain() override { md_vk_exporter_free(m_exporter); }

    void poll() override {
        const std::uint64_t retire_generation = m_host.takeRetireGeneration();
        if (retire_generation != 0 && retire_generation == m_generation) {
            setReady(false);
            md_vk_exporter_release_pool(m_exporter);
            m_generation = 0;
            m_acquired.reset();
            m_host.retireDone(retire_generation);
        }
        md_producer_config_t config {};
        std::uint64_t version = 0;
        std::uint64_t epoch = 0;
        if (m_host.snapshotConfig(m_config_version, m_connection_epoch,
                                  config, version, epoch)) {
            m_config_version = version;
            m_connection_epoch = epoch;
            rebuild(config);
        }
    }

    bool acquireRenderTarget(sr::vulkan::ImageParameters& out) override {
        if (!m_ready || m_exporter == nullptr || m_acquired.has_value()) return false;
        std::uint32_t index = 0;
        if (md_vk_exporter_acquire(m_exporter, &index) != MD_OK) return false;
        m_acquired = index;
        out = {};
        out.handle = md_vk_exporter_image(m_exporter, index);
        out.extent = {m_width, m_height, 1};
        out.mipmap_level = 1;
        out.generation = m_generation;
        return out.handle != VK_NULL_HANDLE;
    }

    void submitRendered(VkSemaphore acquire_semaphore) override {
        if (!m_acquired.has_value()) return;
        const std::uint32_t index = *m_acquired;
        m_acquired.reset();
        int acquire_fd = -1;
        int release_fd = -1;
        int result = md_vk_exporter_export_frame(m_exporter, index, acquire_semaphore,
                                                 &acquire_fd, &release_fd);
        if (result == MD_OK) {
            result = m_host.submitFrame(m_generation, index, m_sequence++,
                                        acquire_fd, release_fd);
        }
        if (result != MD_OK) {
            md_vk_exporter_cancel_frame(m_exporter, index);
            return;
        }
        m_host.notifyFirstFrame();
    }

    unsigned width() const override { return m_width; }
    unsigned height() const override { return m_height; }
    VkFormat format() const override { return md_vk_exporter_format(m_exporter); }
    VkImageLayout producerOutputLayout() const override { return VK_IMAGE_LAYOUT_GENERAL; }
    std::uint32_t releaseTargetQueueFamily() const override {
        return VK_QUEUE_FAMILY_FOREIGN_EXT;
    }
    bool ready() const override { return m_ready; }

    void setOnReadyChanged(std::function<void(const sr::ExSwapchainReadyEvent&)> callback) override {
        m_ready_callback = std::move(callback);
        notifyReady();
    }

private:
    void rebuild(const md_producer_config_t& config) {
        if (m_exporter == nullptr || config.physical_width == 0 ||
            config.physical_height == 0) return;
        setReady(false);
        const std::uint64_t generation = m_host.nextGeneration();
        md_vk_export_pool_info_t pool_info {
            .generation = generation,
            .buffer_count = 3,
            .width = config.physical_width,
            .height = config.physical_height,
            .fourcc = config.fourcc,
            .plane_count = config.plane_count,
            .modifier = config.modifier,
        };
        if (md_vk_exporter_create_pool(m_exporter, &pool_info) != MD_OK) return;
        const md_buffer_pool_t* pool = md_vk_exporter_pool(m_exporter);
        if (m_host.offerPool(pool) != MD_OK) {
            md_vk_exporter_release_pool(m_exporter);
            return;
        }
        m_generation = generation;
        m_width = config.physical_width;
        m_height = config.physical_height;
        m_format = md_vk_exporter_format(m_exporter);
        setReady(true);
    }

    void setReady(bool value) {
        if (m_ready == value) return;
        m_ready = value;
        notifyReady();
    }

    void notifyReady() {
        if (!m_ready_callback) return;
        m_ready_callback(sr::ExSwapchainReadyEvent {
            .ready = m_ready,
            .width = m_width,
            .height = m_height,
            .format = m_format,
        });
    }

    MirageProtocolHost& m_host;
    VkDevice m_device { VK_NULL_HANDLE };
    md_vk_exporter_t* m_exporter { nullptr };
    std::optional<std::uint32_t> m_acquired;
    std::function<void(const sr::ExSwapchainReadyEvent&)> m_ready_callback;
    unsigned m_width { 0 };
    unsigned m_height { 0 };
    VkFormat m_format { VK_FORMAT_UNDEFINED };
    std::uint64_t m_generation { 0 };
    std::uint64_t m_sequence { 1 };
    std::uint64_t m_config_version { 0 };
    std::uint64_t m_connection_epoch { 0 };
    bool m_ready { false };
};

#endif

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
    const bool protocol_mode = !options.display_output_id.empty();
#if !defined(SCENERENDERER_MIRAGE_DISPLAY)
    if (protocol_mode) {
        std::cerr << "This SceneWallpaper build has no mirage-display support\n";
        return 1;
    }
#endif
    if (protocol_mode && options.display_socket.empty()) {
        std::cerr << "--display-socket is required with --display-output-id\n";
        return 1;
    }

#if defined(__APPLE__)
    setenv("MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER", "1", /*overwrite=*/0);
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "1", /*overwrite=*/0);
#endif

    AppState      state;
    DesktopHandle desktop;
#if defined(SCENERENDERER_MIRAGE_DISPLAY)
    std::unique_ptr<MirageProtocolHost> protocol_host;
#endif
    sr::SceneWallpaper wallpaper;
    StopTimer          run_timer;
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
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
#if defined(SCENERENDERER_MIRAGE_DISPLAY)
    if (protocol_mode) {
        protocol_host = std::make_unique<MirageProtocolHost>(
            options.display_socket, options.display_output_id, callbacks);
        if (!protocol_host->start()) {
            std::cerr << "Failed to connect to the mirage-display broker or receive output configuration\n";
            return 1;
        }
        md_producer_config_t producer_config {};
        std::uint64_t config_version = 0;
        std::uint64_t connection_epoch = 0;
        if (!protocol_host->currentConfig(producer_config, config_version, connection_epoch)) {
            std::cerr << "mirage-display did not provide an output configuration\n";
            return 1;
        }
        render_width = producer_config.physical_width;
        render_height = producer_config.physical_height;
    } else
#endif
    {
        desktop.reset(sr::host::DesktopCreate(&desktop_config, callbacks));
        state.desktop = desktop.get();
        if (!desktop) {
            std::cerr << "Failed to create desktop wallpaper host\n";
            return 1;
        }
        render_width = sr::host::DesktopPixelWidth(desktop.get());
        render_height = sr::host::DesktopPixelHeight(desktop.get());
    }
    if (options.resolution) {
        render_width  = options.resolution->width;
        render_height = options.resolution->height;
    } else {
        render_width = static_cast<std::uint32_t>(
            std::lround(static_cast<double>(render_width) * options.render_scale));
        render_height = static_cast<std::uint32_t>(
            std::lround(static_cast<double>(render_height) * options.render_scale));
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
    config.load_from_memory = options.load_from_memory;
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
#if defined(SCENERENDERER_MIRAGE_DISPLAY)
    if (protocol_mode) {
        info.offscreen = true;
        MirageProtocolHost* host = protocol_host.get();
        info.ex_swapchain_factory =
            [host](VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                   VkQueue queue, std::uint32_t queue_family, unsigned width,
                   unsigned height) -> std::unique_ptr<sr::ExSwapchain> {
                return std::make_unique<MirageProtocolSwapchain>(
                    *host, instance, physical_device, device, queue, queue_family,
                    width, height);
            };
    } else
#endif
    {
        info.offscreen = false;
        sr::host::DesktopSurfaceInfo surface_info;
        if (!sr::host::DesktopGetSurfaceInfo(desktop.get(), surface_info)) {
            std::cerr << "Failed to create desktop Vulkan surface info\n";
            return 1;
        }
        info.surface_info.instanceExts = std::move(surface_info.instance_extensions);
        info.surface_info.createSurfaceOp = std::move(surface_info.create_surface);
    }
#endif
    info.width           = ClampRenderExtent(render_width, 1920);
    info.height          = ClampRenderExtent(render_height, 1080);
    info.msaa_samples    = options.msaa;
    if (!protocol_mode) {
        info.redraw_callback = [desktop = desktop.get()]() {
            sr::host::DesktopWake(desktop);
        };
    }

    wallpaper.configure(std::move(config));
    wallpaper.initVulkan(std::move(info));

    if (options.mouse_position) {
        wallpaper.mouseEnter(true);
        wallpaper.mouseInput((*options.mouse_position)[0], (*options.mouse_position)[1]);
    }

    if (options.run_seconds > 0) {
#if defined(SCENERENDERER_MIRAGE_DISPLAY)
        if (protocol_mode) {
            run_timer.start([host = protocol_host.get()] { host->stop(); },
                            options.run_seconds);
        } else
#endif
        {
            run_timer.start([desktop = desktop.get()] {
                sr::host::DesktopStop(desktop);
            }, options.run_seconds);
        }
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
#if defined(SCENERENDERER_MIRAGE_DISPLAY)
        if (protocol_mode) {
            MirageProtocolHost* host = protocol_host.get();
            control.emplace(wallpaper,
                            [host]() { host->stop(); },
                            [host]() { host->notifyActivated(); });
        } else
#endif
        {
            void* desktop_handle = desktop.get();
            control.emplace(
                wallpaper,
                [desktop_handle]() { sr::host::DesktopStop(desktop_handle); },
                [desktop_handle]() { sr::host::DesktopActivate(desktop_handle); });
        }
        control->start();
    }

    int ok = 0;
#if defined(SCENERENDERER_MIRAGE_DISPLAY)
    if (protocol_mode) {
        ok = protocol_host->run();
    } else
#endif
    {
        ok = sr::host::DesktopRun(desktop.get());
    }

    if (control) control->stop();
    run_timer.stop();
    state.wallpaper = nullptr;
    state.desktop = nullptr;
    return ok ? 0 : 1;
}
