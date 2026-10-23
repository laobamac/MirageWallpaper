// RendererController — owns the lifecycle of the Scene/Video/Web wallpaper
// processes and drives them over the line-oriented JSON control protocol on
// stdin (volume, speed, fill mode, user properties, pause/resume, ...).

#pragma once

#include "Services/GlobalSettingsService.h"
#include "Services/WEProject.h"

#include <QDBusContext>
#include <QJsonArray>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QProcess>

#include <functional>

class QScreen;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Mirage {

enum class FillMode {
    Cover,
    Contain,
    Stretch,
};

struct RenderOptions {
    int fps = 30;
    double renderScale = 0.75;
    int msaaSamples = 2;
    double volume = 1.0;
    bool muted = false;
    double speed = 1.0;
    FillMode fillMode = FillMode::Cover;
    QPointF position {0.5, 0.5};
    bool enableSpectrum = true;
    bool loadFromMemory = false;
    QHash<QString, QString> scriptStorage;
    QHash<QString, ProjectProperty> userProperties;
};

// Per-process crop capability reported by SceneRenderer after it knows the
// authored scene and output aspect ratios.
struct PositionAvailability {
    bool known = false;
    bool x = false;
    bool y = false;
};

class RendererController : public QObject, protected QDBusContext {
    Q_OBJECT

public:
    explicit RendererController(GlobalSettingsService* settings, QObject* parent = nullptr);
    ~RendererController() override;

    void setWallpaperTrustChecker(const std::function<bool(const Wallpaper&)>& checker);

    bool render(const Wallpaper& wallpaper, int screenIndex, const RenderOptions& options, QString* error = nullptr);
    void stop(int screenIndex);
    void stopAll();
    QVector<int> activeScreens() const;
    bool isRunningOnScreen(int screenIndex) const;
    QString wallpaperIdOnScreen(int screenIndex) const;
    PositionAvailability positionAvailabilityForWallpaper(const QString& wallpaperId) const;

    // Starts one asynchronous PNG capture for an active Scene renderer. The
    // returned UUID is echoed by snapshotFinished exactly once on the GUI
    // thread. An empty return means the request was rejected synchronously.
    QString requestSnapshot(int screenIndex, const QString& path);

    static QString fillModeKey(FillMode mode);
    static QString stableOutputId(const QScreen* screen);

public slots:
    void setVolume(double volume, int screenIndex = -1);
    void setMuted(bool muted, int screenIndex = -1);
    void pause(int screenIndex = -1);
    void resume(int screenIndex = -1);
    void setFps(int fps, int screenIndex = -1);
    void setSpeed(double speed, int screenIndex = -1);
    void setFillMode(Mirage::FillMode mode, int screenIndex = -1);
    void setPosition(const QPointF& position, int screenIndex = -1);
    void setProperty(const QString& key, const Mirage::ProjectProperty& property, int screenIndex = -1);
    // 应用侧播放策略的唯一权威指令（与 macOS 一致）：state 为
    // "run"/"throttle"/"pause"，渲染器只服从最终状态。
    void setPowerState(const QString& state, int screenIndex = -1);

signals:
    void rendererExited(int screenIndex, bool abnormal);
    void rendererMessage(const QString& message);
    void rendererStateChanged();
    void firstFramePresented(int screenIndex);
    void positionAvailabilityChanged(int screenIndex, bool x, bool y);
    void scriptStorageChanged(const QString& wallpaperId,
                              const QHash<QString, QString>& values);
    void snapshotFinished(const QString& token, int screenIndex, const QString& path,
                          bool success, const QString& error);
    void videoDidEnd(int screenIndex);

private:
    struct RunningProcess {
        QProcess* process = nullptr;
        Wallpaper wallpaper;
        int screenIndex = 0;
        QString outputStableId;
        bool stopping = false;
        QStringList tempFiles;
        QByteArray stdoutBuffer;
        PositionAvailability positionAvailability;
        // Renderer-reported demand is retained for diagnostics only. Linux
        // spectrum samples remain sourced inside SceneRenderer.
        bool audioDemanded = false;
    };

    struct SnapshotRequest {
        int screenIndex = 0;
        QString path;
        QPointer<QProcess> process;
    };

    // Strongly typed projection of org.mpris.MediaPlayer2.Player. DBus metadata
    // arrives as a standard a{sv} dictionary, but no dynamic payload escapes
    // this boundary.
    struct MprisPlayer {
        QString service;
        QString owner;
        QString playbackStatus;
        QString title;
        QString artist;
        QString album;
        QString albumArtist;
        QString artUrl;
        QString trackId;
        qint64 positionUs = 0;
        qint64 durationUs = 0;
        quint64 updatedOrder = 0;
    };

    // 同一输出只能有一个 broker producer。切换期间保留最后一次经过
    // 预检的请求，待旧进程 finished 关闭 producer socket 后再启动；broker
    // 会在读取新 producer 注册前处理该挂断。值类型不借用调用方内存，且
    // 仅在 GUI 线程访问。
    struct PendingRender {
        Wallpaper wallpaper;
        RenderOptions options;
    };

    QString binaryForKind(WallpaperKind kind) const;
    QString sceneWallpaperBinary() const;
    QString webWallpaperBinary() const;
    QString videoWallpaperBinary() const;
    QString writeUserPropertiesFile(const QHash<QString, ProjectProperty>& props, const Wallpaper& wallpaper) const;
    QJsonObject propertyCommand(const QString& key, const ProjectProperty& property) const;
    void sendCommand(RunningProcess* running, const QJsonObject& command);
    void forEachTarget(int screenIndex, const std::function<void(RunningProcess*)>& body);
    void consumeStdout(RunningProcess* running, const QByteArray& chunk);
    void completeSnapshot(const QString& token, QProcess* source, bool success,
                          const QString& error);
    void updateMprisMonitoring();
    void refreshMprisPlayer(const QString& service);
    void publishMprisStatus();

private slots:
    // QtDBus invokes this on the GUI thread. The signal's a{sv} payload has the
    // fixed MPRIS PropertiesChanged schema and is re-read through GetAll before
    // it updates renderer state.
    void onMprisPropertiesChanged(const QString& interfaceName,
                                  const QVariantMap& changedProperties,
                                  const QStringList& invalidatedProperties);

private:

    GlobalSettingsService* m_settings = nullptr;
    std::function<bool(const Wallpaper&)> m_wallpaperTrustChecker;
    QHash<int, RunningProcess*> m_running;
    QHash<QString, SnapshotRequest> m_snapshotRequests;
    QHash<QString, MprisPlayer> m_mprisPlayers;
    QNetworkAccessManager* m_networkAccess = nullptr;
    QNetworkReply* m_artworkReply = nullptr;
    QTimer* m_mprisPositionTimer = nullptr;
    bool m_mprisMonitoring = false;
    quint64 m_mprisUpdateOrder = 0;
    QString m_selectedMprisService;
    QString m_mediaIdentity;
    QString m_artworkSource;
    QString m_artworkPath;
    QString m_previousArtworkPath;
    QJsonArray m_artworkPalette;
    // 按屏幕合并切换请求。显式 stop/stopAll 会清空对应项，防止退出或停止后
    // 的 finished 回调重新启动 renderer。
    QHash<int, PendingRender> m_pendingRenders;
};

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::FillMode)
