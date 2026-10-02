#include "VideoRendererEngine.h"

#include "VideoManifest.h"

#include <mpv/client.h>
#include <mpv/render.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// 打开视频的最长等待时间：loadfile 后 mpv 异步加载，等待 FILE_LOADED 事件。
constexpr std::int64_t kOpenTimeoutSeconds = 15;

} // namespace

// libmpv 后端实现。
//
// 线程模型（libmpv 约束）：所有 mpv_* 调用（mpv_wakeup 除外）必须在同一
// 线程执行。因此 mpv_handle 的创建、事件读取、渲染与销毁全部位于渲染线程
// （eventLoop）；外部控制（play/pause/setVolume/setMuted/close）通过命令
// 队列 + mpv_wakeup 投递到渲染线程执行。mpv_handle / mpv_render_context
// 的创建/销毁均由渲染线程完成（创建方=销毁方），外部线程不直接触碰。
//
// 渲染路径：MPV_RENDER_API_TYPE_SW。mpv 0.41 没有公开的 Vulkan render API
// （mpv#18343 未合入），vo=libmpv 仅提供 opengl/sw 后端，因此 SW 是唯一
// 受支持的集成点；必须显式设置 vo=libmpv，否则 mpv 会打开默认 VO 窗口。
// 硬件解码交给 mpv 内建 hwdec=auto-copy（GPU 解码、帧回 CPU），覆盖
// NVIDIA（nvdec-copy/cuda-copy）与 Intel/AMD（vaapi-copy/vulkan-copy）；
// 无可用后端时 mpv 自动回退软解——本代码不含任何运行时探测/回退分支。
//
// EOF 语义：autoplay=true 时 mpv 以 loop-file=inf 自动循环；autoplay=false
// 时播放到 EOF 触发 videoDidEnd 回调，此后 play() 执行 seek 0 + 解除暂停。
class VRVideoRendererEngine::Impl {
public:
    Impl(VRVideoEngineConfig config, Callbacks callbacks)
        : m_config(config),
          m_callbacks(std::move(callbacks)),
          m_volume(VRClampVideoVolume(config.initialVolume)),
          m_muted(config.muted),
          m_fill_mode(config.fillMode) {}

    ~Impl() { close(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool open(const VRVideoManifest& manifest, std::string* error) {
        if (m_opened) {
            setError(error, "video engine is already open");
            return false;
        }
        const auto& path = manifest.videoPath();
        if (path.empty()) {
            setError(error, "video manifest has no local file");
            return false;
        }
        m_video_path = path;

        m_stop.store(false);
        m_open_done.store(false);
        m_open_ok.store(false);
        m_open_error.clear();
        m_eof.store(false);
        m_thread = std::thread([this] { eventLoop(); });

        std::unique_lock<std::mutex> lock(m_open_mutex);
        if (!m_open_cv.wait_for(lock, std::chrono::seconds(kOpenTimeoutSeconds),
                                [this] { return m_open_done.load(); })) {
            setError(error, "timeout waiting for libmpv to open the video");
            close();
            return false;
        }
        if (!m_open_ok.load()) {
            setError(error, m_open_error.empty() ? "cannot open video with libmpv"
                                                 : m_open_error);
            close();
            return false;
        }
        std::fprintf(stderr, "VideoRenderer: opened %s (libmpv, hwdec delegated)\n",
                     m_video_path.c_str());
        m_opened = true;
        return true;
    }

    void play() {
        m_playing.store(true);
        post([this] {
            // EOF 后重播：先 seek 到 0，再解除暂停（与"EOF→videoDidEnd→外部 play()"语义一致）。
            if (m_eof.load()) {
                m_eof.store(false);
                const char* seek[] = {"seek", "0", "absolute", nullptr};
                if (mpv_command(m_mpv, seek) < 0) {
                    std::fprintf(stderr, "VideoRenderer: mpv seek failed\n");
                }
            }
            int paused = 0;
            if (mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused) < 0) {
                std::fprintf(stderr, "VideoRenderer: mpv pause=0 failed\n");
            }
        });
    }

    void pause() {
        m_playing.store(false);
        post([this] {
            int paused = 1;
            if (mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused) < 0) {
                std::fprintf(stderr, "VideoRenderer: mpv pause=1 failed\n");
            }
        });
    }

    void setVolume(float value) {
        const float clamped = VRClampVideoVolume(value);
        m_volume.store(clamped);
        post([this, clamped] {
            // mpv volume 属性范围 0..100；引擎 API 约定 0..1。
            double mpv_volume = static_cast<double>(clamped) * 100.0;
            if (mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &mpv_volume) < 0) {
                std::fprintf(stderr, "VideoRenderer: mpv volume set failed\n");
            }
        });
    }

    void setMuted(bool value) {
        m_muted.store(value);
        post([this, value] {
            int mute = value ? 1 : 0;
            if (mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &mute) < 0) {
                std::fprintf(stderr, "VideoRenderer: mpv mute set failed\n");
            }
        });
    }

    void setFillMode(VRVideoFillMode mode) { m_fill_mode.store(mode); }

    bool loaded() const noexcept { return m_opened; }
    float volume() const noexcept { return m_volume.load(); }
    bool muted() const noexcept { return m_muted.load(); }
    VRVideoFillMode fillMode() const noexcept { return m_fill_mode.load(); }

    // 最近一帧 RGBA 数据。缓冲区在下一帧渲染前保持有效（调用方需在此前
    // 完成上传）；serial 单调递增用于调用方去重。返回 false 表示尚无帧。
    bool currentFrame(Frame& out) const {
        std::lock_guard<std::mutex> lock(m_frame_mutex);
        if (m_frame.empty() || m_frame_w <= 0 || m_frame_h <= 0) return false;
        out.rgb = m_frame.data();
        out.width = m_frame_w;
        out.height = m_frame_h;
        out.stride = m_frame_stride;
        out.serial = m_frame_serial;
        return true;
    }

private:
    void setError(std::string* error, const std::string& message) const {
        if (error) *error = message;
        if (m_callbacks.playbackError) m_callbacks.playbackError(message);
    }

    // 向渲染线程投递命令：加锁写入队列，并唤醒可能阻塞在 mpv_wait_event
    // 的渲染线程。mpv_wakeup 是唯一允许跨线程调用的 mpv API；调用保持在
    // 锁内，与 cleanup() 中 m_mpv 置空互斥，避免 wakeup 指向已销毁的 handle。
    void post(std::function<void()> command) {
        std::lock_guard<std::mutex> lock(m_control_mutex);
        m_commands.push_back(std::move(command));
        if (m_mpv != nullptr) mpv_wakeup(m_mpv);
    }

    // 渲染线程入口：创建 mpv → 加载视频 → 事件/渲染主循环 → 清理。
    void eventLoop() {
        std::string open_error;
        const bool ok = createMpv(&open_error);
        {
            std::lock_guard<std::mutex> lock(m_open_mutex);
            m_open_error = std::move(open_error);
            m_open_ok.store(ok);
            m_open_done.store(true);
        }
        m_open_cv.notify_all();
        if (!ok) {
            cleanup();
            return;
        }

        auto last_report = std::chrono::steady_clock::now();
        while (!m_stop.load()) {
            processCommands();

            // 阻塞短暂超时：有事件立即返回，无事件每 10ms 轮询一次渲染。
            mpv_event* event = mpv_wait_event(m_mpv, 0.01);
            if (event->event_id != MPV_EVENT_NONE) handleEvent(event);
            if (m_stop.load()) break;

            const uint64_t flags = mpv_render_context_update(m_render);
            if ((flags & MPV_RENDER_UPDATE_FRAME) != 0u) renderFrame();

            // hwdec-current 诊断日志：报告 mpv 实际选用的解码路径（软解为
            // "no"）。仅读取属性打日志，不参与任何决策分支。
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::seconds(2)) {
                last_report = now;
                char* hwdec = mpv_get_property_string(m_mpv, "hwdec-current");
                std::fprintf(stderr, "VideoRenderer: hwdec-current=%s\n",
                             hwdec != nullptr ? hwdec : "?");
                mpv_free(hwdec);
            }
        }
        cleanup();
    }

    bool createMpv(std::string* error) {
        m_mpv = mpv_create();
        if (m_mpv == nullptr) {
            *error = "cannot create libmpv handle";
            return false;
        }

        // 行为可预测：不读取用户 mpv.conf，不加载 lua 脚本。
        const struct {
            const char* name;
            const char* value;
        } options[] = {
            {"config", "no"},
            {"load-scripts", "no"},
            {"vo", "libmpv"}, // 必须显式：默认 VO 在无窗口会话会崩溃
            {"hwdec", "auto-copy"}, // GPU 解码+帧回 CPU，mpv 内建决策
            {"ao", "pipewire,pulseaudio,alsa"}, // 按系统可用音频后端依次尝试
            {"loop-file", m_config.autoplay ? "inf" : "no"},
            {"keep-open", "yes"}, // EOF 后保持核心存活，供 play() 重播
        };
        for (const auto& option : options) {
            if (mpv_set_option_string(m_mpv, option.name, option.value) < 0) {
                *error = std::string("cannot set libmpv option ") + option.name;
                return false;
            }
        }
        if (mpv_initialize(m_mpv) < 0) {
            *error = "libmpv initialization failed";
            return false;
        }

        const char* api = MPV_RENDER_API_TYPE_SW;
        mpv_render_param render_params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(api)},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (mpv_render_context_create(&m_render, m_mpv, render_params) < 0) {
            *error = "libmpv software render context creation failed";
            return false;
        }

        // 初始属性（volume 0..100、mute、初始暂停状态）。
        double initial_volume =
            static_cast<double>(VRClampVideoVolume(m_config.initialVolume)) * 100.0;
        if (mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &initial_volume) < 0) {
            *error = "cannot set libmpv volume";
            return false;
        }
        int initial_mute = m_config.muted ? 1 : 0;
        if (mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &initial_mute) < 0) {
            *error = "cannot set libmpv mute";
            return false;
        }
        int initial_pause = m_config.autoplay ? 0 : 1;
        if (mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &initial_pause) < 0) {
            *error = "cannot set libmpv pause";
            return false;
        }

        const char* load_command[] = {"loadfile", m_video_path.c_str(), nullptr};
        if (mpv_command(m_mpv, load_command) < 0) {
            *error = "libmpv loadfile failed";
            return false;
        }

        // 等待 FILE_LOADED 或加载错误（最多 kOpenTimeoutSeconds）。
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(kOpenTimeoutSeconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_stop.load()) {
                *error = "engine closed while opening video";
                return false;
            }
            mpv_event* event = mpv_wait_event(m_mpv, 0.05);
            if (event->event_id == MPV_EVENT_FILE_LOADED) return true;
            if (event->event_id == MPV_EVENT_END_FILE) {
                const auto* end = static_cast<const mpv_event_end_file*>(event->data);
                if (end->reason == MPV_END_FILE_REASON_ERROR) {
                    const char* text = mpv_error_string(end->error);
                    *error = std::string("libmpv failed to load video: ") +
                             (text != nullptr ? text : "unknown error");
                    return false;
                }
            }
        }
        *error = "timeout loading video with libmpv";
        return false;
    }

    void processCommands() {
        std::deque<std::function<void()>> commands;
        {
            std::lock_guard<std::mutex> lock(m_control_mutex);
            commands.swap(m_commands);
        }
        for (auto& command : commands) command();
    }

    void handleEvent(const mpv_event* event) {
        switch (event->event_id) {
        case MPV_EVENT_END_FILE: {
            const auto* end = static_cast<const mpv_event_end_file*>(event->data);
            if (end->reason == MPV_END_FILE_REASON_EOF) {
                m_eof.store(true);
                if (!m_config.autoplay && m_callbacks.videoDidEnd) {
                    m_callbacks.videoDidEnd();
                }
            } else if (end->reason == MPV_END_FILE_REASON_ERROR) {
                const char* text = mpv_error_string(end->error);
                setError(nullptr, std::string("libmpv playback error: ") +
                                      (text != nullptr ? text : "unknown error"));
            }
            break;
        }
        default:
            break;
        }
    }

    // 将 mpv 当前帧渲染进内部 RGBA 缓冲并换出到 currentFrame 可见区。
    void renderFrame() {
        long long width = 0;
        long long height = 0;
        if (mpv_get_property(m_mpv, "video-params/w", MPV_FORMAT_INT64, &width) < 0 ||
            mpv_get_property(m_mpv, "video-params/h", MPV_FORMAT_INT64, &height) < 0) {
            return; // 尚无视频参数（加载中），跳过本帧
        }
        if (width <= 0 || height <= 0) return;

        const std::size_t stride_bytes = static_cast<std::size_t>(width) * 4u;
        const std::size_t needed = stride_bytes * static_cast<std::size_t>(height);
        // 渲染缓冲可能因上一帧被 swap 出去而变空，须按当前视频尺寸重分配。
        if (static_cast<long long>(m_buf_w) != width ||
            static_cast<long long>(m_buf_h) != height || m_buf.size() < needed) {
            m_buf_w = static_cast<int>(width);
            m_buf_h = static_cast<int>(height);
            m_buf_stride = static_cast<int>(stride_bytes);
            m_buf.assign(needed, 0u);
        }
        if (m_buf.empty()) return;

        int sw_size[2] = {m_buf_w, m_buf_h};
        mpv_render_param render_params[] = {
            {MPV_RENDER_PARAM_SW_SIZE, sw_size},
            {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char*>("rgba")},
            {MPV_RENDER_PARAM_SW_STRIDE, &m_buf_stride},
            {MPV_RENDER_PARAM_SW_POINTER, m_buf.data()},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (mpv_render_context_render(m_render, render_params) < 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_frame_mutex);
        m_frame.swap(m_buf);
        m_frame_w = m_buf_w;
        m_frame_h = m_buf_h;
        m_frame_stride = m_buf_stride;
        m_frame_serial += 1u;
    }

    // 渲染线程内释放 mpv 资源（与创建同线程）。置空操作加锁，避免与
    // post()/close() 跨线程读 m_mpv 竞争。
    void cleanup() {
        mpv_render_context* render = nullptr;
        mpv_handle* handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_control_mutex);
            render = m_render;
            m_render = nullptr;
            handle = m_mpv;
            m_mpv = nullptr;
        }
        if (render != nullptr) mpv_render_context_free(render);
        if (handle != nullptr) mpv_terminate_destroy(handle);
    }

    void close() {
        m_stop.store(true);
        {
            std::lock_guard<std::mutex> lock(m_control_mutex);
            if (m_mpv != nullptr) mpv_wakeup(m_mpv);
        }
        if (m_thread.joinable()) m_thread.join();
        m_opened = false;
    }

    VRVideoEngineConfig m_config;
    Callbacks m_callbacks;

    std::string m_video_path;
    bool m_opened { false };

    // libmpv 状态（渲染线程独占）
    mpv_handle* m_mpv { nullptr };
    mpv_render_context* m_render { nullptr };

    // 渲染缓冲（RGBA）
    std::vector<std::uint8_t> m_buf;
    int m_buf_w { 0 };
    int m_buf_h { 0 };
    int m_buf_stride { 0 };

    // 帧换出区（渲染线程写，currentFrame 读）
    mutable std::mutex m_frame_mutex;
    std::vector<std::uint8_t> m_frame;
    int m_frame_w { 0 };
    int m_frame_h { 0 };
    int m_frame_stride { 0 };
    std::uint64_t m_frame_serial { 0 };

    // 控制队列（外部线程写，渲染线程处理）
    std::mutex m_control_mutex;
    std::deque<std::function<void()>> m_commands;

    // 打开同步（open() 等待渲染线程加载完成）
    std::mutex m_open_mutex;
    std::condition_variable m_open_cv;
    std::atomic<bool> m_open_done { false };
    std::atomic<bool> m_open_ok { false };
    std::string m_open_error;

    // 状态
    std::thread m_thread;
    std::atomic<bool> m_stop { false };
    std::atomic<bool> m_playing { false };
    std::atomic<bool> m_eof { false };
    std::atomic<float> m_volume { 1.0f };
    std::atomic<bool> m_muted { false };
    std::atomic<VRVideoFillMode> m_fill_mode { VRVideoFillModeCover };
};

VRVideoRendererEngine::VRVideoRendererEngine(VRVideoEngineConfig config, Callbacks callbacks)
    : m_impl(std::make_unique<Impl>(config, std::move(callbacks))) {}

VRVideoRendererEngine::~VRVideoRendererEngine() = default;

VRVideoEngineConfig VRVideoRendererEngine::defaultConfig() noexcept {
    return VRDefaultVideoEngineConfig();
}

bool VRVideoRendererEngine::openWallpaper(const VRVideoManifest& manifest, std::string* error) const {
    return m_impl->open(manifest, error);
}

void VRVideoRendererEngine::play() const { m_impl->play(); }
void VRVideoRendererEngine::pause() const { m_impl->pause(); }
void VRVideoRendererEngine::setVolume(float volume) const { m_impl->setVolume(volume); }
void VRVideoRendererEngine::setMuted(bool muted) const { m_impl->setMuted(muted); }
void VRVideoRendererEngine::setFillMode(VRVideoFillMode mode) const { m_impl->setFillMode(mode); }

bool VRVideoRendererEngine::loaded() const noexcept { return m_impl->loaded(); }
float VRVideoRendererEngine::volume() const noexcept { return m_impl->volume(); }
bool VRVideoRendererEngine::muted() const noexcept { return m_impl->muted(); }
VRVideoFillMode VRVideoRendererEngine::fillMode() const noexcept { return m_impl->fillMode(); }

bool VRVideoRendererEngine::currentFrame(Frame& out) const { return m_impl->currentFrame(out); }
