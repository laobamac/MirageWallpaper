// VRProtocolVideoRenderer — Linux mirage-display wallpaper host: decodes the
// video with FFmpeg, uploads frames through Vulkan, and exports each frame as
// DMA-BUF to the desktop environment's display adapter over a producer
// connection. Audio goes to PulseAudio; control comes from the stdin channel.

#pragma once

#include "VideoRendererTypes.h"

#include <QString>

#include <functional>
#include <memory>

class VRProtocolVideoRenderer final {
public:
    struct Config {
        QString socketPath;
        QString outputId;
        QString videoPath;
        VRVideoFillMode fillMode = VRVideoFillModeCover;
        float volume = 1.0f;
        bool muted = false;
        bool loadFromMemory = false;
        std::function<void(const QString&)> errorCallback;
        std::function<void()> videoDidEndCallback;
        std::function<void()> firstFrameCallback;
    };

    explicit VRProtocolVideoRenderer(Config config);
    ~VRProtocolVideoRenderer();

    VRProtocolVideoRenderer(const VRProtocolVideoRenderer&) = delete;
    VRProtocolVideoRenderer& operator=(const VRProtocolVideoRenderer&) = delete;

    bool start(QString* error = nullptr);
    void stop();

    void play();
    void pause();
    void setVolume(float volume);
    void setMuted(bool muted);
    void setFillMode(VRVideoFillMode fillMode);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
