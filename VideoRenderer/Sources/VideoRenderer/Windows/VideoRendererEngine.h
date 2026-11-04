#pragma once

#include "VideoRendererTypes.h"
#include <QJsonObject>
#include <QOpenGLWindow>
#include <QQueue>
#include <QTimer>
#include <functional>
#include <memory>

struct mpv_handle;
struct mpv_render_context;
class VRVideoManifest;

// libmpv 的事件/控制与 Qt GL 上下文均由 GUI 线程处理；解码和音频由 mpv 自身线程执行。
class VRVideoRendererEngine final : public QOpenGLWindow {
public:
    struct Callbacks {
        std::function<void(const QString&)> playbackError;
        std::function<void(const QJsonObject&)> event;
    };
    explicit VRVideoRendererEngine(VRVideoEngineConfig config, Callbacks callbacks);
    ~VRVideoRendererEngine() override;
    bool openWallpaper(const VRVideoManifest& manifest, QString* error = nullptr);
    void stop();
    void handleCommand(const QJsonObject& command);
    void setFps(double fps);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    struct MemoryVideo;
    static void wakeup(void* context);
    static void renderUpdate(void* context);
    static void* resolveGL(void* context, const char* name);
    void processEvents();
    void applyFill();
    void emitPositionAvailability();
    void property(const char* name, const char* value);
    void property(const char* name, double value);
    void property(const char* name, bool value);
    void emitEvent(const QJsonObject& event);
    void fail(const QString& error);
    void requestFrame();

    VRVideoEngineConfig m_config;
    Callbacks m_callbacks;
    mpv_handle* m_mpv = nullptr;
    mpv_render_context* m_render = nullptr;
    std::unique_ptr<MemoryVideo> m_memory;
    QString m_videoPath;
    struct SnapshotRequest { QString path; QString token; };
    QQueue<SnapshotRequest> m_snapshots;
    QTimer m_frameTimer;
    bool m_pendingFrame = false;
    bool m_loaded = false;
    bool m_firstPresented = false;
    bool m_presentedFrame = false;
    bool m_restarted = false;
    bool m_seekPending = false;
    double m_lastPosition = 0;
    double m_duration = 0;
    double m_fps = 60;
    int m_videoWidth = 0;
    int m_videoHeight = 0;
};
