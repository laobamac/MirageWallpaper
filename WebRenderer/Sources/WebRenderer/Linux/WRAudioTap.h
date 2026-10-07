#pragma once

#include <array>
#include <memory>

#include <QString>

// Linux system-output spectrum capture used by WebRendererEngine. This mirrors
// the macOS WRAudioTap contract while keeping the capture backend selected at
// build time, so WebViewer and WebWallpaper share one known audio protocol.
class WRAudioTap final {
public:
    WRAudioTap();
    ~WRAudioTap();

    WRAudioTap(const WRAudioTap&) = delete;
    WRAudioTap& operator=(const WRAudioTap&) = delete;

    // Opens the configured system-output monitor. On failure `error` describes
    // the backend failure; ownership of the string remains with the caller.
    bool start(QString* error);
    void stop();
    bool isRunning() const;

    // Copies the latest calibrated 64-bin stereo spectrum. Returns false until
    // the backend has received enough output samples for an FFT window.
    bool copySpectrum(std::array<float, 64>& left, std::array<float, 64>& right) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
