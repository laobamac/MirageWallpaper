#include "Services/PlaybackController.h"

#include "Services/GlobalSettingsService.h"
#include "Services/MirageController.h"
#include "Services/PlaylistManager.h"

#include <QGuiApplication>
#include <QScreen>

namespace Mirage {

namespace {

// 把 QML 提交的设置 map 应用到 GlobalSettings（缺失键保留原值）。
void updateSettingsFromMap(GlobalSettings& settings, const QVariantMap& values) {
    const auto stringValue = [&values](const QString& key, const QString& fallback) {
        return values.contains(key) ? values.value(key).toString() : fallback;
    };
    const auto boolValue = [&values](const QString& key, bool fallback) {
        return values.contains(key) ? values.value(key).toBool() : fallback;
    };
    const auto intValue = [&values](const QString& key, int fallback) {
        return values.contains(key) ? values.value(key).toInt() : fallback;
    };
    const auto doubleValue = [&values](const QString& key, double fallback) {
        return values.contains(key) ? values.value(key).toDouble() : fallback;
    };
    settings.otherApplicationFocused = stringValue(QStringLiteral("otherApplicationFocused"), settings.otherApplicationFocused);
    settings.otherApplicationFullscreen = stringValue(QStringLiteral("otherApplicationFullscreen"), settings.otherApplicationFullscreen);
    settings.otherApplicationPlayingAudio = stringValue(QStringLiteral("otherApplicationPlayingAudio"), settings.otherApplicationPlayingAudio);
    settings.displayAsleep = stringValue(QStringLiteral("displayAsleep"), settings.displayAsleep);
    settings.laptopOnBattery = stringValue(QStringLiteral("laptopOnBattery"), settings.laptopOnBattery);
    settings.antiAliasing = stringValue(QStringLiteral("antiAliasing"), settings.antiAliasing);
    settings.postProcessing = stringValue(QStringLiteral("postProcessing"), settings.postProcessing);
    settings.textureResolution = stringValue(QStringLiteral("textureResolution"), settings.textureResolution);
    settings.reflections = boolValue(QStringLiteral("reflections"), settings.reflections);
    settings.fps = intValue(QStringLiteral("fps"), settings.fps);
    settings.wallpaperLoadSource = stringValue(QStringLiteral("wallpaperLoadSource"), settings.wallpaperLoadSource);
    settings.autoStart = boolValue(QStringLiteral("autoStart"), settings.autoStart);
    settings.safeMode = boolValue(QStringLiteral("safeMode"), settings.safeMode);
    settings.language = stringValue(QStringLiteral("language"), settings.language);
    settings.appearance = stringValue(QStringLiteral("appearance"), settings.appearance);
    settings.audioOutput = boolValue(QStringLiteral("audioOutput"), settings.audioOutput);
    settings.reloadWhenChangingOutputDevice = boolValue(QStringLiteral("reloadWhenChangingOutputDevice"), settings.reloadWhenChangingOutputDevice);
    settings.masterVolume = doubleValue(QStringLiteral("masterVolume"), settings.masterVolume);
    settings.globalMuted = boolValue(QStringLiteral("globalMuted"), settings.globalMuted);
    settings.enableSpectrum = boolValue(QStringLiteral("enableSpectrum"), settings.enableSpectrum);
    settings.videoFramework = stringValue(QStringLiteral("videoFramework"), settings.videoFramework);
    settings.processPriority = stringValue(QStringLiteral("processPriority"), settings.processPriority);
    settings.pauseOnVRAMExhausted = boolValue(QStringLiteral("pauseOnVRAMExhausted"), settings.pauseOnVRAMExhausted);
    settings.restartAfterCrashing = boolValue(QStringLiteral("restartAfterCrashing"), settings.restartAfterCrashing);
    settings.logLevel = stringValue(QStringLiteral("logLevel"), settings.logLevel);
    settings.verboseLog = boolValue(QStringLiteral("verboseLog"), settings.verboseLog);
    settings.autoRefresh = boolValue(QStringLiteral("autoRefresh"), settings.autoRefresh);
    settings.steamAPIEndpoint = stringValue(QStringLiteral("steamAPIEndpoint"), settings.steamAPIEndpoint);
    settings.steamAPIKey = stringValue(QStringLiteral("steamAPIKey"), settings.steamAPIKey);
    settings.customWorkshopDirectory = stringValue(QStringLiteral("customWorkshopDirectory"), settings.customWorkshopDirectory);
    settings.customImportedDirectory = stringValue(QStringLiteral("customImportedDirectory"), settings.customImportedDirectory);
}

} // namespace

PlaybackController::PlaybackController(GlobalSettingsService* settings,
                                       RendererController* renderer,
                                       WallpaperRuntimeStore* runtimeStore,
                                       PlaylistManager* playlist,
                                       MirageController* owner,
                                       QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_renderer(renderer)
    , m_runtimeStore(runtimeStore)
    , m_playlist(playlist)
    , m_owner(owner) {
    // 属性下发防抖定时器：合并窗口 16ms（约 1 帧，对齐 macOS 的 1/60s
    // propertyCommandWorkItem）。singleShot + 每次 start() 重启语义与
    // macOS 的 cancel + asyncAfter 一致：拖动过程中持续重启，停止后
    // 16ms 内下发最后一次合并结果。
    m_propertyCommandTimer = new QTimer(this);
    m_propertyCommandTimer->setSingleShot(true);
    m_propertyCommandTimer->setInterval(16);
    connect(m_propertyCommandTimer, &QTimer::timeout,
            this, &PlaybackController::flushPropertyCommands);
}

RenderOptions PlaybackController::renderOptionsFor(const Wallpaper& item) const {
    const GlobalSettings& settings = m_settings->settings();
    const WallpaperRuntimeState runtime = m_runtimeStore->loadRuntime(item);
    RenderOptions options;
    options.fps = settings.fps;
    if (settings.textureResolution == QStringLiteral("highQuality"))
        options.renderScale = 1.0;
    else if (settings.textureResolution == QStringLiteral("highPerformance"))
        options.renderScale = 0.5;
    else
        options.renderScale = 0.75;
    if (settings.antiAliasing == QStringLiteral("none"))
        options.msaaSamples = 1;
    else if (settings.antiAliasing == QStringLiteral("msaa_x4"))
        options.msaaSamples = 4;
    else if (settings.antiAliasing == QStringLiteral("msaa_x8"))
        options.msaaSamples = 8;
    else
        options.msaaSamples = 2;
    options.volume = runtime.volume * settings.masterVolume;
    options.muted = runtime.muted || settings.globalMuted || runtime.volume <= 0.0;
    options.speed = runtime.speed;
    options.fillMode = runtime.fillMode;
    options.position = runtime.position;
    options.scriptStorage = runtime.scriptStorage;
    options.enableSpectrum = settings.enableSpectrum;
    options.loadFromMemory = settings.wallpaperLoadSource == QStringLiteral("memory");
    options.userProperties = m_runtimeStore->effectiveProperties(item, runtime);
    return options;
}

QVector<int> PlaybackController::enabledDisplayScreens() const {
    const QList<QScreen*> screens = QGuiApplication::screens();
    QVector<int> result;
    result.reserve(m_owner->m_displayModel.rowCount());
    for (int row = 0; row < m_owner->m_displayModel.rowCount(); ++row) {
        const QString stableId = m_owner->m_displayModel.stableIdAt(row);
        for (int screen = 0; screen < screens.size(); ++screen) {
            if (RendererController::stableOutputId(screens.at(screen)) != stableId) continue;
            result.append(screen);
            break;
        }
    }
    return result;
}

QVariantList PlaybackController::displays() const {
    QVariantList result;
    const QList<QScreen*> screens = QGuiApplication::screens();
    result.reserve(qMax(1, screens.size()));

    if (screens.isEmpty()) {
        result.append(QVariantMap{
            {QStringLiteral("index"), 0},
            {QStringLiteral("name"), QStringLiteral("显示器 1")},
            {QStringLiteral("width"), 0},
            {QStringLiteral("height"), 0},
            {QStringLiteral("primary"), true},
            {QStringLiteral("running"), m_renderer->isRunningOnScreen(0)},
            {QStringLiteral("wallpaperTitle"), QString()},
        });
        return result;
    }

    for (int index = 0; index < screens.size(); ++index) {
        const QScreen* screen = screens.at(index);
        const QString wallpaperId = m_renderer->wallpaperIdOnScreen(index);
        const Wallpaper runningWallpaper = m_owner->wallpaper(wallpaperId);
        const QRect geometry = screen->geometry();
        result.append(QVariantMap{
            {QStringLiteral("index"), index},
            {QStringLiteral("stableId"), RendererController::stableOutputId(screen)},
            {QStringLiteral("name"), screen->name().isEmpty()
                ? QStringLiteral("显示器 %1").arg(index + 1) : screen->name()},
            {QStringLiteral("width"), geometry.width()},
            {QStringLiteral("height"), geometry.height()},
            {QStringLiteral("primary"), screen == QGuiApplication::primaryScreen()},
            {QStringLiteral("running"), m_renderer->isRunningOnScreen(index)},
            {QStringLiteral("wallpaperTitle"), runningWallpaper.isValid()
                ? runningWallpaper.project.title : QString()},
        });
    }
    return result;
}

double PlaybackController::selectedVolume() const {
    const Wallpaper item = m_owner->wallpaper(m_owner->m_selectedWallpaperId);
    return item.isValid() ? m_runtimeStore->loadRuntime(item).volume : 1.0;
}

double PlaybackController::selectedSpeed() const {
    const Wallpaper item = m_owner->wallpaper(m_owner->m_selectedWallpaperId);
    return item.isValid() ? m_runtimeStore->loadRuntime(item).speed : 1.0;
}

QString PlaybackController::selectedFillMode() const {
    const Wallpaper item = m_owner->wallpaper(m_owner->m_selectedWallpaperId);
    if (!item.isValid()) return QStringLiteral("cover");
    return RendererController::fillModeKey(m_runtimeStore->loadRuntime(item).fillMode);
}

QPointF PlaybackController::selectedPosition() const {
    const Wallpaper item = m_owner->wallpaper(m_owner->m_selectedWallpaperId);
    return item.isValid() ? m_runtimeStore->loadRuntime(item).position : QPointF(0.5, 0.5);
}

PositionAvailability PlaybackController::selectedPositionAvailability() const {
    return m_renderer->positionAvailabilityForWallpaper(m_owner->m_selectedWallpaperId);
}

void PlaybackController::apply(const Wallpaper& item, bool allScreens) {
    if (!item.isValid()) return;
    m_owner->selectWallpaper(item.id());

    bool applied = false;
    QString error;
    const RenderOptions options = renderOptionsFor(item);
    const QVector<int> enabledScreens = enabledDisplayScreens();
    QVector<int> targets;
    if (allScreens) {
        targets = enabledScreens;
    } else if (!enabledScreens.isEmpty()) {
        const QList<QScreen*> screens = QGuiApplication::screens();
        const int primaryIndex = screens.indexOf(QGuiApplication::primaryScreen());
        targets.append(enabledScreens.contains(primaryIndex) ? primaryIndex : enabledScreens.first());
    } else {
        error = QStringLiteral("没有启用 Mirage 壁纸的显示器");
    }
    for (const int screen : targets) {
        QString screenError;
        if (m_renderer->render(item, screen, options, &screenError)) {
            applied = true;
            m_playlist->setCurrentWallpaper(screen, item);
            m_playlist->kickRotator(screen);
        }
        if (!screenError.isEmpty()) error = screenError;
    }
    if (applied) {
        if (m_paused) {
            m_paused = false;
            emit pausedChanged(false);
        }
        emit m_owner->playlistChanged();
        m_owner->setStatusMessage(allScreens
            ? QStringLiteral("已应用到所有显示器") : QStringLiteral("已应用壁纸"));
    } else if (!error.isEmpty()) {
        m_owner->setStatusMessage(error);
    }
}

void PlaybackController::applySelectedToScreen(int screen) {
    const Wallpaper item = m_owner->wallpaper(m_owner->m_selectedWallpaperId);
    if (!item.isValid()) return;

    if (!enabledDisplayScreens().contains(screen)) {
        m_owner->setStatusMessage(QStringLiteral("该显示器未启用 Mirage 壁纸"));
        return;
    }

    const int target = qBound(0, screen, m_owner->screenCount() - 1);
    QString error;
    if (m_renderer->render(item, target, renderOptionsFor(item), &error)) {
        if (m_paused) {
            m_paused = false;
            emit pausedChanged(false);
        }
        m_playlist->setCurrentWallpaper(target, item);
        m_playlist->kickRotator(target);
        emit m_owner->playlistChanged();
        m_owner->setStatusMessage(QStringLiteral("已应用到显示器 %1").arg(target + 1));
    } else if (!error.isEmpty()) {
        m_owner->setStatusMessage(error);
    }
}

void PlaybackController::playPlaylistItem(const Wallpaper& item) {
    if (!item.isValid()) return;
    if (!enabledDisplayScreens().contains(m_owner->m_playlistScreen)) {
        m_owner->setStatusMessage(QStringLiteral("当前显示器未启用 Mirage 壁纸"));
        return;
    }
    QString error;
    if (m_renderer->render(item, m_owner->m_playlistScreen, renderOptionsFor(item), &error)) {
        if (m_paused) {
            m_paused = false;
            emit pausedChanged(false);
        }
        m_playlist->setCurrentWallpaper(m_owner->m_playlistScreen, item);
        m_playlist->kickRotator(m_owner->m_playlistScreen);
        m_owner->selectWallpaper(item.id());
        m_owner->setStatusMessage(QStringLiteral("已应用壁纸"));
    } else if (!error.isEmpty()) {
        m_owner->setStatusMessage(error);
    }
}

void PlaybackController::stopWallpapers() {
    if (m_paused) {
        m_paused = false;
        emit pausedChanged(false);
    }
    // 渲染进程即将退出，丢弃未下发的属性命令，避免 flush 写向已停止的进程。
    m_pendingPropertyCommands.clear();
    m_propertyCommandTimer->stop();
    m_renderer->stopAll();
    m_owner->setStatusMessage(QStringLiteral("已停止动态壁纸"));
}

void PlaybackController::stopScreen(int screen) {
    if (screen < 0 || screen >= m_owner->screenCount()) return;
    // 与 stopWallpapers 一致：丢弃未下发命令，避免 flush 写向已停止的进程。
    m_pendingPropertyCommands.clear();
    m_propertyCommandTimer->stop();
    m_renderer->stop(screen);
    m_owner->setStatusMessage(QStringLiteral("已停止显示器 %1 的动态壁纸").arg(screen + 1));
}

void PlaybackController::pauseWallpapers() {
    if (m_paused) return;
    m_paused = true;
    emit pausedChanged(true);
    m_renderer->pause();
}

void PlaybackController::resumeWallpapers() {
    if (!m_paused) return;
    m_paused = false;
    emit pausedChanged(false);
    m_renderer->resume();
}

void PlaybackController::muteWallpapers() {
    m_muted = true;
    m_renderer->setMuted(true);
}

// WINDOW_STATE 位定义（mirage-display 协议）：0x1 遮盖、0x2 失焦、
// 0x4 最大化、0x8 全屏。
namespace {
constexpr quint32 kWindowFocusLost = 0x2u;
constexpr quint32 kWindowFullscreen = 0x8u;
} // namespace

PlaybackController::PlaybackAction PlaybackController::actionForRule(
    const QString& rule, PlaybackAction current) const {
    // 与 macOS 一致：多个来源的规则取优先级最高的动作
    // （keepRunning < mute < pause < stop）。
    PlaybackAction action = PlaybackAction::KeepRunning;
    if (rule == QStringLiteral("mute")) action = PlaybackAction::Mute;
    else if (rule == QStringLiteral("pause")) action = PlaybackAction::Pause;
    else if (rule == QStringLiteral("stop")) action = PlaybackAction::Stop;
    return static_cast<int>(action) > static_cast<int>(current) ? action : current;
}

// 把窗口动作应用到指定屏幕：stop 由应用终止渲染进程并在策略解除后重新
// 应用壁纸（对齐 macOS stoppedByPlaybackPolicy）；pause/mute 与会话级
// 手动状态合并后经 power/muted 命令下发，渲染器只服从最终状态。
void PlaybackController::applyWindowAction(int screen, PlaybackAction action) {
    if (action == PlaybackAction::Stop) {
        if (!m_windowStoppedScreens.contains(screen)) {
            m_windowStoppedScreens.insert(screen);
            m_renderer->stop(screen);
        }
        return;
    }

    if (m_windowStoppedScreens.remove(screen)) {
        const Wallpaper item = m_playlist->currentWallpaper(screen);
        if (item.isValid()) {
            QString error;
            m_renderer->render(item, screen, renderOptionsFor(item), &error);
        }
    }

    const bool windowPaused = action == PlaybackAction::Pause;
    const bool windowMuted = action == PlaybackAction::Mute;
    // 合并会话级暂停/静音（对齐 macOS isPaused/isMuted）。
    m_renderer->setPowerState(m_paused || windowPaused ? QStringLiteral("pause")
                                                       : QStringLiteral("run"),
                              screen);
    m_renderer->setMuted(m_muted || windowMuted, screen);
}

// 桌面窗口事实 → 该屏幕的播放动作。stableId 与 RendererController 的
// stableOutputId() 同源（"kde:" + 厂商|型号|序列号），据此定位屏幕。
void PlaybackController::handleWindowState(const QString& stableId, quint32 flags) {
    if (stableId.isEmpty()) return;
    int screen = -1;
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (int index = 0; index < screens.size(); ++index) {
        if (RendererController::stableOutputId(screens.at(index)) == stableId) {
            screen = index;
            break;
        }
    }
    if (screen < 0) return;

    m_lastWindowFlags.insert(screen, flags);
    const GlobalSettings& settings = m_settings->settings();
    PlaybackAction action = PlaybackAction::KeepRunning;
    if ((flags & kWindowFocusLost) != 0u) {
        action = actionForRule(settings.otherApplicationFocused, action);
    }
    if ((flags & kWindowFullscreen) != 0u) {
        action = actionForRule(settings.otherApplicationFullscreen, action);
    }
    applyWindowAction(screen, action);
}

void PlaybackController::setSelectedVolume(double volume) {
    const Wallpaper item = m_owner->wallpaper(m_owner->m_selectedWallpaperId);
    if (!item.isValid()) return;
    m_runtimeStore->setVolume(item, volume);
    const GlobalSettings& settings = m_settings->settings();
    const WallpaperRuntimeState runtime = m_runtimeStore->loadRuntime(item);
    m_renderer->setVolume(runtime.volume * settings.masterVolume);
    m_renderer->setMuted(runtime.muted || settings.globalMuted || runtime.volume <= 0.0);
    emit m_owner->selectedRuntimeChanged();
}

void PlaybackController::setSelectedSpeed(double speed) {
    const Wallpaper item = m_owner->wallpaper(m_owner->m_selectedWallpaperId);
    if (!item.isValid()) return;
    m_runtimeStore->setSpeed(item, speed);
    m_renderer->setSpeed(speed);
    emit m_owner->selectedRuntimeChanged();
}

void PlaybackController::setSelectedFillMode(const QString& mode) {
    const Wallpaper item = m_owner->wallpaper(m_owner->m_selectedWallpaperId);
    if (!item.isValid()) return;
    const FillMode fillMode = mode == QStringLiteral("contain")
        ? FillMode::Contain
        : mode == QStringLiteral("stretch") ? FillMode::Stretch : FillMode::Cover;
    m_runtimeStore->setFillMode(item, fillMode);
    m_renderer->setFillMode(fillMode);
    emit m_owner->selectedRuntimeChanged();
}

void PlaybackController::setSelectedPosition(const QPointF& position) {
    const Wallpaper item = m_owner->wallpaper(m_owner->m_selectedWallpaperId);
    if (!item.isValid() || item.kind() != WallpaperKind::Scene) return;
    m_runtimeStore->setPosition(item, position);
    for (const int screen : m_renderer->activeScreens()) {
        if (m_playlist->currentWallpaper(screen).id() == item.id()) {
            m_renderer->setPosition(position, screen);
        }
    }
    emit m_owner->selectedRuntimeChanged();
}

void PlaybackController::setSelectedProperty(const QString& key, const QVariant& value) {
    const Wallpaper item = m_owner->wallpaper(m_owner->m_selectedWallpaperId);
    if (!item.isValid()) return;
    const ProjectProperty property = m_runtimeStore->setProperty(item, key, value);
    if (property.type.isEmpty()) return;
    if (item.kind() != WallpaperKind::Scene && item.kind() != WallpaperKind::Web) return;
    // 属性实时下发防抖：滑块/开关拖动时 QML onMoved 每像素触发一次，若直接
    // sendCommand 会向渲染进程洪水式灌命令（场景壁纸每帧重绘，且 GUI 线程
    // 每像素全链路重建）。与 macOS propertyCommandWorkItem 对齐：命令进
    // pending 表（同 key 只留最新值），16ms 合并窗口到期后批量下发。
    m_pendingPropertyCommands.insert(key, {m_owner->m_selectedWallpaperId, property});
    m_propertyCommandTimer->start();
}

void PlaybackController::flushPropertyCommands() {
    if (m_pendingPropertyCommands.isEmpty()) return;
    const QHash<QString, PendingPropertyCommand> commands = m_pendingPropertyCommands;
    m_pendingPropertyCommands.clear();
    for (auto it = commands.constBegin(); it != commands.constEnd(); ++it) {
        // wallpaperId 守卫：合并窗口内若已切换选中壁纸，丢弃过期命令，
        // 对应 macOS work item 的 currentWallpaper.id 检查。
        if (it.value().wallpaperId != m_owner->m_selectedWallpaperId) continue;
        m_renderer->setProperty(it.key(), it.value().property);
    }
}

void PlaybackController::resetSelectedProperties() {
    const Wallpaper item = m_owner->wallpaper(m_owner->m_selectedWallpaperId);
    if (!item.isValid()) return;
    // 清空待发属性命令：拖动滑块期间点"重置"时，合并窗口内的旧编辑值
    // 若在重置（渲染进程按默认值重启）后才 flush，会覆盖重置结果。
    m_pendingPropertyCommands.clear();
    m_propertyCommandTimer->stop();
    m_runtimeStore->resetRuntime(item);
    bool rendered = false;
    for (int screen : m_renderer->activeScreens()) {
        if (m_playlist->currentWallpaper(screen).id() != item.id()) continue;
        QString error;
        if (m_renderer->render(item, screen, renderOptionsFor(item), &error))
            rendered = true;
    }
    // 重新渲染启动的是全新播放进程，暂停状态随之解除。
    if (rendered && m_paused) {
        m_paused = false;
        emit pausedChanged(false);
    }
}

void PlaybackController::previewFps(int fps) {
    m_renderer->setFps(qBound(10, fps, 120));
}

bool PlaybackController::applySettings(const QVariantMap& values) {
    const GlobalSettings before = m_settings->settings();
    GlobalSettings updated = m_settings->settings();
    updateSettingsFromMap(updated, values);
    QString error;
    if (m_settings->setSettings(updated, &error)) {
        const GlobalSettings& applied = m_settings->settings();
        if (before.fps != applied.fps) m_renderer->setFps(applied.fps);
        // 播放规则（其他应用获取焦点/全屏）变化时，按最新规则重算每屏的
        // 当前窗口动作（无需等待下一次窗口状态事件）。
        if (before.otherApplicationFocused != applied.otherApplicationFocused
            || before.otherApplicationFullscreen != applied.otherApplicationFullscreen) {
            for (auto it = m_lastWindowFlags.constBegin(); it != m_lastWindowFlags.constEnd(); ++it) {
                const quint32 flags = it.value();
                PlaybackAction action = PlaybackAction::KeepRunning;
                if ((flags & kWindowFocusLost) != 0u)
                    action = actionForRule(applied.otherApplicationFocused, action);
                if ((flags & kWindowFullscreen) != 0u)
                    action = actionForRule(applied.otherApplicationFullscreen, action);
                applyWindowAction(it.key(), action);
            }
        }
        return true;
    }
    m_owner->setStatusMessage(error.isEmpty() ? QStringLiteral("设置保存失败") : error);
    return false;
}

void PlaybackController::restoreStartupPlayback() {
    const QList<QScreen*> screens = QGuiApplication::screens();
    const QVector<int> enabledScreens = enabledDisplayScreens();
    const QHash<QString, QString> lastApplied = m_playlist->lastAppliedIDs();
    for (auto it = lastApplied.constBegin(); it != lastApplied.constEnd(); ++it) {
        int screen = -1;
        for (const int index : enabledScreens) {
            if (RendererController::stableOutputId(screens.at(index)) == it.key()) {
                screen = index;
                break;
            }
        }
        // 屏幕可能已被移除；稳定 ID 不匹配时不把壁纸错误恢复到另一块屏。
        if (screen < 0) continue;

        const Wallpaper item = m_playlist->resolveWallpaper(it.value());
        if (!item.isValid()) continue;
        QString error;
        if (m_renderer->render(item, screen, renderOptionsFor(item), &error)) {
            m_playlist->setCurrentWallpaper(screen, item);
        }
    }
}

} // namespace Mirage
