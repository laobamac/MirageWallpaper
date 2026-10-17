#pragma once

#include "Services/RendererController.h"
#include "Services/WallpaperRuntimeStore.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <QVariantList>
#include <QVariantMap>

namespace Mirage {

class GlobalSettingsService;
class MirageController;
class PlaylistManager;

// 渲染与播放控制：对应 macOS 版 RendererController.swift 的播放编排部分。
// 负责把壁纸渲染到显示器（单屏/全屏）、播放列表当前项联动、运行时属性
// （音量/速度/填充）应用、启动恢复与显示器状态序列化。
// 依赖 owner（MirageController）提供壁纸查找与选中状态。
class PlaybackController : public QObject {
    Q_OBJECT
public:
    Q_SIGNAL void pausedChanged(bool paused);

    PlaybackController(GlobalSettingsService* settings,
                       RendererController* renderer,
                       WallpaperRuntimeStore* runtimeStore,
                       PlaylistManager* playlist,
                       MirageController* owner,
                       QObject* parent = nullptr);

    QVariantList displays() const;
    double selectedVolume() const;
    double selectedSpeed() const;
    QString selectedFillMode() const;
    QPointF selectedPosition() const;
    PositionAvailability selectedPositionAvailability() const;

    void apply(const Wallpaper& item, bool allScreens);
    void applySelectedToScreen(int screen);
    void playPlaylistItem(const Wallpaper& item);
    void stopWallpapers();
    void stopScreen(int screen);
    void pauseWallpapers();
    void resumeWallpapers();
    void muteWallpapers();
    void setSelectedVolume(double volume);
    void setSelectedSpeed(double speed);
    void setSelectedFillMode(const QString& mode);
    void setSelectedPosition(const QPointF& position);
    void setSelectedProperty(const QString& key, const QVariant& value);
    void resetSelectedProperties();
    void previewFps(int fps);
    bool applySettings(const QVariantMap& values);
    void restoreStartupPlayback();

    // 桌面窗口事实（来自 mirage-display broker 宿主回调）：按"播放规则"
    // 设置计算该屏幕的最终动作并下发 power/muted 命令（对齐 macOS 的
    // applyPlaybackPolicy）。
    void handleWindowState(const QString& stableId, quint32 flags);

private:
    // 属性实时下发防抖的待发命令：key → 命令（含所属壁纸 id 与属性值）。
    // 合并窗口（16ms）内同 key 只保留最新值，窗口结束批量下发。
    struct PendingPropertyCommand {
        QString wallpaperId;
        ProjectProperty property;
    };

    // 播放规则动作，优先级 rank：stop > pause > mute > keepRunning。
    enum class PlaybackAction { KeepRunning, Mute, Pause, Stop };

    RenderOptions renderOptionsFor(const Wallpaper& item) const;
    /* Returns QGuiApplication screen indexes that have a registered
     * mirage-display consumer. Renderer producers are meaningful only for
     * these outputs; the full Qt screen list also contains disabled outputs. */
    QVector<int> enabledDisplayScreens() const;
    // 把待发属性命令批量下发给渲染进程（合并窗口到期时由定时器调用）。
    void flushPropertyCommands();
    PlaybackAction actionForRule(const QString& rule, PlaybackAction current) const;
    void applyWindowAction(int screen, PlaybackAction action);

    GlobalSettingsService* m_settings;
    RendererController* m_renderer;
    WallpaperRuntimeStore* m_runtimeStore;
    PlaylistManager* m_playlist;
    MirageController* m_owner;
    bool m_paused = false;
    // 会话级静音（状态栏"静音"动作）；与窗口动作 mute 合并后下发。
    bool m_muted = false;
    // 每屏最近一次窗口标志，设置变化时据此重算动作。
    QHash<int, quint32> m_lastWindowFlags;
    // 因窗口 stop 策略被停止的屏幕；策略解除后需要重新应用壁纸。
    QSet<int> m_windowStoppedScreens;
    QHash<QString, PendingPropertyCommand> m_pendingPropertyCommands;
    QTimer* m_propertyCommandTimer = nullptr;
};

} // namespace Mirage
