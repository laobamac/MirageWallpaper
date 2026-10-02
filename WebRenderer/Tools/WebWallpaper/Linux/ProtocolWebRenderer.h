#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <Qt>

#include <functional>
#include <memory>

// Bridges QtWebEngine frames to the Linux desktop protocol. The producer and
// Vulkan objects are owned by the worker instance; submitted sync FDs are
// consumed by md_producer_submit_frame on every path.
class ProtocolWebRenderer final : public QObject {
    Q_OBJECT
public:
    struct Config {
        QString socketPath;
        QString outputId;
        // Pointer callbacks run on the producer I/O thread. Coordinates are
        // normalized against the configured physical output exactly as in
        // SceneWallpaper. Linux BTN_* codes and axis sources are mapped here,
        // keeping mirage-display wire values out of WebRendererEngine.
        std::function<void(float, float)> pointerEnter;
        std::function<void()> pointerLeave;
        std::function<void(float, float)> pointerMotion;
        std::function<void(float, float, Qt::MouseButton, bool)> pointerButton;
        std::function<void(float, float, float, float, bool)> pointerAxis;
        std::function<void(uint32_t, uint32_t)> outputSizeChanged;
    };

    explicit ProtocolWebRenderer(Config config, QObject* parent = nullptr);
    ~ProtocolWebRenderer() override;

    bool start(QString* error);
    void stop();
    void submitFrame(const QImage& image);

signals:
    void failed(const QString& message);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
