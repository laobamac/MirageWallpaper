#pragma once

#include "VideoRendererTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class VRVideoManifest;

// Linux playback engine backed by libmpv (system libmpv, no vendored copy).
// Design decisions (frozen after Phase-1 prototype):
//   - Rendering path: MPV_RENDER_API_TYPE_SW only. mpv 0.41 has no public
//     Vulkan render API (mpv#18343 still open), and vo=libmpv only offers
//     "opengl"/"sw" backends; using libplacebo internals would be an unstable
//     private ABI, so SW is the only supported integration point.
//   - DMA-BUF output is NOT produced by mpv: it stays in the caller's chain
//     (CPU RGBA frame -> Vulkan staging/upload -> md_vk_exporter GPU copy ->
//     DMA-BUF fd). mpv only decodes/plays and hands over CPU frames.
//   - Hardware decoding is delegated entirely to mpv's built-in decision via
//     the "hwdec=auto-copy" option (GPU decode, CPU-visible frames), covering
//     NVIDIA (nvdec-copy/cuda-copy), Intel and AMD (vaapi-copy/vulkan-copy);
//     mpv falls back to software decoding when no hwdec backend is usable.
//     No runtime probing/fallback branches live in this code (AGENTS.md).
//   - MUST set "vo=libmpv" explicitly, otherwise mpv opens its default VO
//     (crashes in headless sessions).
//   - The system render.h uses the newer API: mpv_render_context_update()
//     returns uint64_t flags; there is no MPV_EVENT_RENDER_UPDATE event.
// Decoded RGBA frames are handed to the viewer (GLFW/OpenGL, mirroring
// SceneViewer) via currentFrame(). Deliberately free of Qt Widgets and
// Qt Multimedia.
class VRVideoRendererEngine final {
public:
    // 回调在引擎渲染线程（mpv 线程）上同步调用：回调内不得调用引擎的
    // stop/析构（会 join 自身线程导致死锁），只应做线程安全的信号通知
    // （如 Qt QueuedConnection / 简单标志）。
    struct Callbacks {
        std::function<void(const std::string&)> playbackError;
        std::function<void()> videoDidEnd;
    };

    struct Frame {
        const std::uint8_t* rgb { nullptr };
        int width { 0 };
        int height { 0 };
        int stride { 0 };
        std::uint64_t serial { 0 };
    };

    explicit VRVideoRendererEngine(
        VRVideoEngineConfig config = VRDefaultVideoEngineConfig(),
        Callbacks callbacks = {});
    ~VRVideoRendererEngine();

    VRVideoRendererEngine(const VRVideoRendererEngine&) = delete;
    VRVideoRendererEngine& operator=(const VRVideoRendererEngine&) = delete;

    [[nodiscard]] static VRVideoEngineConfig defaultConfig() noexcept;

    [[nodiscard]] bool openWallpaper(const VRVideoManifest& manifest,
                                     std::string* error = nullptr) const;

    void play() const;
    void pause() const;
    void setVolume(float volume) const;
    void setMuted(bool muted) const;
    void setFillMode(VRVideoFillMode fillMode) const;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] float volume() const noexcept;
    [[nodiscard]] bool muted() const noexcept;
    [[nodiscard]] VRVideoFillMode fillMode() const noexcept;

    // Latest decoded frame (RGB24). The buffer stays valid until the next
    // decode step swaps it; callers should upload it before calling again.
    [[nodiscard]] bool currentFrame(Frame& out) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
