#include "Services/RendererController.h"

#include "Services/LinuxSystemIntegration.h"
#include "Services/Paths.h"
#include "Services/DisplayBrokerService.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QCryptographicHash>
#ifndef Q_OS_WIN
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>
#endif
#include "Services/WindowsProcessJob.h"
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QScreen>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUuid>

#include <functional>
#include <algorithm>
#include <optional>
#include <array>
#include <cmath>
#ifdef Q_OS_WIN
#include <atomic>
#include <chrono>
#include <mutex>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/base.h>
#endif

namespace Mirage {
#ifdef Q_OS_WIN
struct WindowsMediaSessionState {
    using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;

    std::atomic<quint64> generation{0};
    std::atomic<quint64> managerRequestGeneration{0};
    std::atomic<quint64> refreshGeneration{0};
    std::atomic_bool active{false};
    std::mutex managerMutex;
    Manager manager{nullptr};
    QJsonObject lastStatus;
};
#endif
namespace {

#ifdef Q_OS_WIN
bool windowsMediaGenerationIsActive(
    const std::shared_ptr<WindowsMediaSessionState>& state, quint64 generation) {
    return state && state->active.load() && state->generation.load() == generation;
}

QString windowsMediaString(const winrt::hstring& value) {
    const std::string utf8 = winrt::to_string(value);
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

QJsonObject idleWindowsMediaStatus() {
    return QJsonObject{
        {QStringLiteral("state"), 0},
        {QStringLiteral("title"), QString()},
        {QStringLiteral("artist"), QString()},
        {QStringLiteral("album"), QString()},
        {QStringLiteral("albumArtist"), QString()},
        {QStringLiteral("artURL"), QString()},
        {QStringLiteral("previousArtURL"), QString()},
        {QStringLiteral("position"), 0.0},
        {QStringLiteral("duration"), 0.0},
    };
}
#endif

QString number(double value) {
    return QString::number(value, 'f', 3);
}

void writeRendererDiagnostic(int screenIndex, const QString& text) {
    QTextStream stream(stdout);
    stream << "[Renderer " << screenIndex + 1 << "] " << text << Qt::endl;
}

QString siblingBinary(const QString& name) {
    const QString candidate = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/" + name);
    return QFileInfo(candidate).isExecutable() ? candidate : QString();
}

QString firstExecutable(const QStringList& candidates) {
    for (const QString& candidate : candidates) {
        if (QFileInfo(candidate).isExecutable()) return QDir::cleanPath(candidate);
    }
    return {};
}

QJsonValue propertyWireValue(const ProjectProperty& property) {
    switch (property.propertyKind()) {
    case PropertyKind::Bool:
        return property.boolValue();
    case PropertyKind::Slider:
        return property.doubleValue();
    case PropertyKind::Combo:
        return variantToJsonValue(property.value);
    case PropertyKind::Color:
    case PropertyKind::SceneTexture:
    case PropertyKind::File:
    case PropertyKind::TextInput:
    case PropertyKind::Text:
    case PropertyKind::Group:
    case PropertyKind::Directory:
    case PropertyKind::UserShortcut:
    case PropertyKind::Unknown:
        return property.stringValue();
    }
    return property.stringValue();
}

std::optional<QString> MprisString(const QVariantMap& values, const QString& key) {
    const auto value = values.constFind(key);
    if (value == values.constEnd()) return std::nullopt;
    if (value->metaType().id() != QMetaType::QString) {
        qWarning() << "[MPRIS] Property has non-string type:" << key << value->metaType().name();
        return std::nullopt;
    }
    return value->toString();
}

std::optional<QStringList> MprisStringList(const QVariantMap& values, const QString& key) {
    const auto value = values.constFind(key);
    if (value == values.constEnd()) return std::nullopt;
    if (value->metaType().id() != QMetaType::QStringList) {
        qWarning() << "[MPRIS] Property has non-string-list type:" << key
                   << value->metaType().name();
        return std::nullopt;
    }
    return value->toStringList();
}

std::optional<qint64> MprisInt64(const QVariantMap& values, const QString& key) {
    const auto value = values.constFind(key);
    if (value == values.constEnd()) return std::nullopt;
    if (value->metaType().id() != QMetaType::LongLong) {
        qWarning() << "[MPRIS] Property has non-int64 type:" << key << value->metaType().name();
        return std::nullopt;
    }
    return value->toLongLong();
}

QJsonArray MprisPalette(const QImage& source) {
    if (source.isNull()) return QJsonArray();
    const QImage image = source.scaled(32, 32, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_RGBA8888);
    QHash<int, std::array<double, 5>> buckets;
    for (int y = 0; y < image.height(); ++y) {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const uchar* pixel = row + x * 4;
            if (pixel[3] <= 96U) continue;
            const double red = static_cast<double>(pixel[0]) / 255.0;
            const double green = static_cast<double>(pixel[1]) / 255.0;
            const double blue = static_cast<double>(pixel[2]) / 255.0;
            const int key = (static_cast<int>(red * 15.0) << 8) |
                            (static_cast<int>(green * 15.0) << 4) |
                            static_cast<int>(blue * 15.0);
            std::array<double, 5>& bucket = buckets[key];
            bucket[0] += 1.0;
            bucket[1] += red;
            bucket[2] += green;
            bucket[3] += blue;
            bucket[4] += 0.35 + std::max({red, green, blue}) - std::min({red, green, blue});
        }
    }
    QList<std::array<double, 5>> ranked = buckets.values();
    std::sort(ranked.begin(), ranked.end(), [](const std::array<double, 5>& lhs,
                                                const std::array<double, 5>& rhs) {
        return lhs[4] > rhs[4];
    });
    QList<std::array<double, 3>> selected;
    for (const std::array<double, 5>& bucket : ranked) {
        const std::array<double, 3> color {
            bucket[1] / bucket[0], bucket[2] / bucket[0], bucket[3] / bucket[0],
        };
        bool distinct = true;
        for (const std::array<double, 3>& existing : selected) {
            const double red = existing[0] - color[0];
            const double green = existing[1] - color[1];
            const double blue = existing[2] - color[2];
            if (std::sqrt(red * red + green * green + blue * blue) <= 0.16) {
                distinct = false;
                break;
            }
        }
        if (distinct) selected.append(color);
        if (selected.size() == 3) break;
    }
    if (selected.isEmpty()) return QJsonArray();
    while (selected.size() < 3) {
        const double factor = selected.size() == 1 ? 1.25 : 0.65;
        const std::array<double, 3>& primary = selected.first();
        selected.append({std::clamp(primary[0] * factor, 0.0, 1.0),
                         std::clamp(primary[1] * factor, 0.0, 1.0),
                         std::clamp(primary[2] * factor, 0.0, 1.0)});
    }
    const std::array<double, 3>& primary = selected.first();
    const double luminance = primary[0] * 0.2126 + primary[1] * 0.7152 + primary[2] * 0.0722;
    const std::array<double, 3> text = luminance > 0.5
                                           ? std::array<double, 3>{0.0, 0.0, 0.0}
                                           : std::array<double, 3>{1.0, 1.0, 1.0};
    const std::array<double, 3> contrast = luminance > 0.35
                                               ? std::array<double, 3>{0.0, 0.0, 0.0}
                                               : std::array<double, 3>{1.0, 1.0, 1.0};
    selected.append(text);
    selected.append(contrast);
    QJsonArray palette;
    for (const std::array<double, 3>& color : selected) {
        palette.append(QJsonArray{color[0], color[1], color[2]});
    }
    return palette;
}

} // namespace

RendererController::RendererController(GlobalSettingsService* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings) {
    qRegisterMetaType<Mirage::FillMode>();
#ifdef Q_OS_WIN
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        m_windowsMediaApartmentReady = true;
    } catch (const winrt::hresult_error& error) {
        qWarning() << "[Media] Cannot initialize Windows Runtime apartment:"
                   << QString::fromStdString(winrt::to_string(error.message()));
    }
#else
    m_networkAccess = new QNetworkAccessManager(this);
    m_mprisPositionTimer = new QTimer(this);
    m_mprisPositionTimer->setInterval(1000);
    connect(m_mprisPositionTimer, &QTimer::timeout, this, [this] {
        if (!m_mprisMonitoring || m_selectedMprisService.isEmpty()) return;
        auto* properties = new QDBusInterface(
            m_selectedMprisService, QStringLiteral("/org/mpris/MediaPlayer2"),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QDBusConnection::sessionBus(), this);
        QDBusPendingCall call = properties->asyncCall(
            QStringLiteral("Get"), QStringLiteral("org.mpris.MediaPlayer2.Player"),
            QStringLiteral("Position"));
        auto* watcher = new QDBusPendingCallWatcher(call, properties);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, properties](QDBusPendingCallWatcher* completed) {
            const QDBusPendingReply<QDBusVariant> reply = *completed;
            const QString service = properties->service();
            completed->deleteLater();
            properties->deleteLater();
            if (reply.isError() || !m_mprisPlayers.contains(service)) {
                if (reply.isError()) {
                    qWarning() << "[MPRIS] Position query failed:" << service
                               << reply.error().message();
                }
                return;
            }
            const QVariant value = reply.value().variant();
            if (value.metaType().id() != QMetaType::LongLong) {
                qWarning() << "[MPRIS] Position has non-int64 type:" << value.metaType().name();
                return;
            }
            m_mprisPlayers[service].positionUs = value.toLongLong();
            publishMprisStatus();
        });
    });

    QDBusConnectionInterface* bus = QDBusConnection::sessionBus().interface();
    if (bus != nullptr) {
        connect(bus, &QDBusConnectionInterface::serviceRegistered, this,
                [this](const QString& service) {
            if (m_mprisMonitoring && service.startsWith(QStringLiteral("org.mpris.MediaPlayer2."))) {
                refreshMprisPlayer(service);
            }
        });
        connect(bus, &QDBusConnectionInterface::serviceUnregistered, this,
                [this](const QString& service) {
            if (!service.startsWith(QStringLiteral("org.mpris.MediaPlayer2."))) return;
            m_mprisPlayers.remove(service);
            publishMprisStatus();
        });
    }
    const bool connected = QDBusConnection::sessionBus().connect(
        QString(), QStringLiteral("/org/mpris/MediaPlayer2"),
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("PropertiesChanged"), this,
        SLOT(onMprisPropertiesChanged(QString,QVariantMap,QStringList)));
    if (!connected) qWarning() << "[MPRIS] Cannot subscribe to PropertiesChanged";
#endif
#ifdef Q_OS_WIN
    m_windowsMediaState = std::make_shared<WindowsMediaSessionState>();
    m_windowsMediaTimer = new QTimer(this);
    m_windowsMediaTimer->setInterval(1000);
    connect(m_windowsMediaTimer, &QTimer::timeout,
            this, &RendererController::refreshWindowsMediaStatus);
#endif
}

RendererController::~RendererController() {
    shutdown();
#ifdef Q_OS_WIN
    if (m_windowsMediaApartmentReady) {
        winrt::uninit_apartment();
        m_windowsMediaApartmentReady = false;
    }
#endif
}

void RendererController::shutdown() {
#ifdef Q_OS_WIN
    m_shuttingDown = true;
#endif
    stopAll();
    QList<QPointer<QProcess>> processes;
    for (const RunningProcess* running : m_running) processes.append(running->process);
    for (const QPointer<QProcess>& process : processes) {
        if (!process || process->state() == QProcess::NotRunning) continue;
        if (!process->waitForFinished(1500)) {
            process->kill();
            process->waitForFinished(3000);
        }
    }
    updateMprisMonitoring();
}

void RendererController::reconcileScreens() {
#ifdef Q_OS_WIN
    const QList<QScreen*> screens = QGuiApplication::screens();
    const auto previous = m_running;
    const auto pending = m_pendingRenders;
    m_running.clear();
    m_pendingRenders.clear();
    int retiredIndex = -1;
    for (auto it = previous.constBegin(); it != previous.constEnd(); ++it) {
        RunningProcess* running = it.value();
        int nextIndex = -1;
        if (it.key() >= 0) {
            for (int index = 0; index < screens.size(); ++index) {
                if (stableOutputId(screens.at(index)) == running->outputStableId) {
                    nextIndex = index;
                    break;
                }
            }
        }
        if (nextIndex >= 0) {
            running->screenIndex = nextIndex;
            m_running.insert(nextIndex, running);
            if (pending.contains(it.key())) m_pendingRenders.insert(nextIndex, pending.value(it.key()));
            for (SnapshotRequest& request : m_snapshotRequests)
                if (request.process == running->process) request.screenIndex = nextIndex;
        } else {
            cancelRecovery(running->outputStableId);
            // 断开的 producer 保留负索引直至 finished，仍受 shutdown 与 Job 管理。
            running->screenIndex = retiredIndex--;
            m_running.insert(running->screenIndex, running);
            if (!running->stopping) {
                running->stopping = true;
                sendCommand(running, QJsonObject{{"cmd", "quit"}});
                running->process->closeWriteChannel();
                QTimer::singleShot(200, running->process, [process = running->process] {
                    if (process->state() != QProcess::NotRunning) process->kill();
                });
            }
        }
    }
    updateMprisMonitoring();
    emit rendererStateChanged();
#endif
}

void RendererController::setWallpaperTrustChecker(const std::function<bool(const Wallpaper&)>& checker) {
    m_wallpaperTrustChecker = checker;
}

#ifdef Q_OS_WIN
void RendererController::cancelRecovery(const QString& outputStableId) {
    if (!outputStableId.isEmpty()) m_recoveryStates.remove(outputStableId);
}

void RendererController::cancelRecoveryForScreen(int screenIndex) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screenIndex < 0 || screenIndex >= screens.size()) return;
    cancelRecovery(stableOutputId(screens.at(screenIndex)));
}

void RendererController::scheduleRecovery(const Wallpaper& wallpaper, int screenIndex,
                                          const QString& outputStableId,
                                          const RenderOptions& options) {
    if (m_shuttingDown || outputStableId.isEmpty() || !wallpaper.isValid() ||
        m_pendingRenders.contains(screenIndex) || m_settings == nullptr ||
        !m_settings->settings().restartAfterCrashing) return;

    auto stateIt = m_recoveryStates.find(outputStableId);
    if (stateIt == m_recoveryStates.end()) {
        RendererRecoveryState state;
        state.wallpaper = wallpaper;
        state.options = options;
        state.generation = ++m_recoveryGeneration;
        stateIt = m_recoveryStates.insert(outputStableId, state);
    } else {
        stateIt->wallpaper = wallpaper;
        stateIt->options = options;
    }

    static constexpr std::array<int, 3> delaysMs {500, 1500, 4000};
    if (stateIt->attempts >= static_cast<int>(delaysMs.size())) {
        writeRendererDiagnostic(screenIndex, QStringLiteral("渲染器重启次数已达上限"));
        return;
    }

    const int attempt = ++stateIt->attempts;
    const quint64 generation = stateIt->generation;
    const int delayMs = delaysMs.at(static_cast<std::size_t>(attempt - 1));
    writeRendererDiagnostic(screenIndex,
                            QStringLiteral("渲染器异常退出，将在 %1 ms 后恢复（第 %2/3 次）")
                                .arg(delayMs).arg(attempt));
    QTimer::singleShot(delayMs, this, [this, outputStableId, generation, attempt] {
        const auto current = m_recoveryStates.constFind(outputStableId);
        if (m_shuttingDown || m_settings == nullptr ||
            !m_settings->settings().restartAfterCrashing ||
            current == m_recoveryStates.constEnd() || current->generation != generation ||
            current->attempts != attempt) return;
        recoverOutput(outputStableId, generation, false);
    });
}

void RendererController::recoverShellOutputs() {
    if (m_shuttingDown) return;
    const QStringList outputIds = m_recoveryStates.keys();
    for (const QString& outputId : outputIds) {
        const auto state = m_recoveryStates.constFind(outputId);
        if (state != m_recoveryStates.constEnd())
            recoverOutput(outputId, state->generation, true);
    }
}

void RendererController::recoverOutput(const QString& outputStableId, quint64 generation,
                                       bool shellRecovery) {
    const auto state = m_recoveryStates.constFind(outputStableId);
    if (m_shuttingDown || state == m_recoveryStates.constEnd() ||
        state->generation != generation || !state->wallpaper.isValid()) return;
    if (!shellRecovery && (m_settings == nullptr || !m_settings->settings().restartAfterCrashing)) return;

    int currentScreen = -1;
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (int index = 0; index < screens.size(); ++index) {
        if (stableOutputId(screens.at(index)) == outputStableId) {
            currentScreen = index;
            break;
        }
    }
    if (currentScreen < 0 || m_pendingRenders.contains(currentScreen)) return;
    for (const RunningProcess* running : m_running) {
        if (running && running->outputStableId == outputStableId) return;
    }

    const Wallpaper wallpaper = state->wallpaper;
    const RenderOptions options = state->options;
    writeRendererDiagnostic(currentScreen, shellRecovery
        ? QStringLiteral("Windows 桌面壳已恢复，正在恢复渲染器")
        : QStringLiteral("正在恢复异常退出的渲染器"));
    QString launchError;
    if (!renderInternal(wallpaper, currentScreen, options, &launchError) && !launchError.isEmpty())
        emit rendererMessage(launchError);
}
#endif

bool RendererController::render(const Wallpaper& wallpaper, int screenIndex, const RenderOptions& options, QString* error) {
#ifdef Q_OS_WIN
    cancelRecoveryForScreen(screenIndex);
#endif
    return renderInternal(wallpaper, screenIndex, options, error);
}

bool RendererController::renderInternal(const Wallpaper& wallpaper, int screenIndex,
                                        const RenderOptions& options, QString* error) {
    if (!wallpaper.isValid()) {
        qWarning() << "[Render] Wallpaper is invalid";
        if (error) *error = QStringLiteral("壁纸无效或缺少预设依赖");
        return false;
    }

    qWarning() << "[Render] Called with wallpaper kind:" << static_cast<int>(wallpaper.kind())
               << "screenIndex:" << screenIndex;

    if (wallpaper.kind() == WallpaperKind::Web
        && m_wallpaperTrustChecker
        && !m_wallpaperTrustChecker(wallpaper)) {
        if (error) *error = QStringLiteral("网页壁纸需要用户确认后才可运行");
        emit rendererMessage(error ? *error : QString());
        return false;
    }

    const QString unsupported = LinuxSystemIntegration::wallpaperUnsupportedReason();
    if (!unsupported.isEmpty()) {
        qWarning() << "[Render] Wallpaper unsupported:" << unsupported;
        if (error) *error = unsupported;
        return false;
    }

    const QString binary = binaryForKind(wallpaper.kind());
    if (binary.isEmpty()) {
        qWarning() << "[Render] Binary not found for kind:" << static_cast<int>(wallpaper.kind());
        if (error) *error = QStringLiteral("找不到渲染器二进制");
        return false;
    }

    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screenIndex < 0 || screenIndex >= screens.size()) {
        if (error) *error = QStringLiteral("目标显示器已断开或索引无效");
        return false;
    }
    QScreen* targetScreen = screens.at(screenIndex);
    const QString outputStableId = stableOutputId(targetScreen);
    if (outputStableId.isEmpty()) {
        qWarning() << "[Render] Cannot determine output stable ID for screen" << screenIndex;
        if (error) *error = QStringLiteral("无法确定目标显示器标识，无法应用壁纸");
        return false;
    }
    qWarning() << "[Render] Output stable ID:" << outputStableId;

    qWarning() << "[Render] Checking m_running for screenIndex:" << screenIndex
               << "contains:" << m_running.contains(screenIndex);
    if (m_running.contains(screenIndex)) {
        qWarning() << "[Render] Found existing process, will terminate";
    }

    if (RunningProcess* const running = m_running.value(screenIndex)) {
        // Broker 在旧 producer 断连前拒绝同一输出的新 producer。因此切换时
        // 覆盖保存最新请求，并立即强制终止旧进程以加速 broker 清理。
        qWarning() << "[Switch] Detected existing renderer on screen" << screenIndex << "PID" << running->process->processId();
        m_pendingRenders.insert(screenIndex, PendingRender{wallpaper, options});
        if (!running->stopping) {
            running->stopping = true;
            updateMprisMonitoring();
#ifdef Q_OS_WIN
            sendCommand(running, QJsonObject{{"cmd", "quit"}});
            running->process->closeWriteChannel();
#else
            qWarning() << "[Switch] Sending SIGTERM to PID" << running->process->processId();
            // 立即发送 SIGTERM，不再尝试优雅退出
            running->process->terminate();
#endif
            // 如果 200ms 后仍未退出，发送 SIGKILL
            QTimer::singleShot(200, running->process, [process = running->process] {
                if (process->state() != QProcess::NotRunning) {
                    qWarning() << "[Switch] Process still running after 200ms, sending SIGKILL to" << process->processId();
                    process->kill();
                }
            });
            emit rendererStateChanged();
        }
        return true;
    }

    auto* process = new QProcess(this);
    if (!configureProcessJob(process, error)) {
        process->deleteLater();
        return false;
    }
    auto* running = new RunningProcess;
    running->process = process;
    running->wallpaper = wallpaper;
    running->desiredOptions = options;
    running->screenIndex = screenIndex;
    running->outputStableId = outputStableId;

    QStringList args;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_WIN
    args << "--screen" << QString::number(screenIndex)
         << "--output-id" << running->outputStableId;
#else
    // Wallpaper hosts display exclusively through the mirage-display protocol.
    args << "--display-output-id" << running->outputStableId
         << "--display-socket" << DisplayBrokerService::defaultSocketPath();
#endif

    switch (wallpaper.kind()) {
    case WallpaperKind::Scene: {
        args << Paths::assetsDir()
             << wallpaper.resolvedEntryPath()
             << "--fps" << QString::number(options.fps)
             << "--render-scale" << number(options.renderScale)
             << "--msaa" << QString::number(options.msaaSamples)
             << "--fill" << fillModeKey(options.fillMode)
             << "--position-x" << number(options.position.x())
             << "--position-y" << number(options.position.y())
             << "--control-stdin";
        if (options.muted) args << "--muted";
        if (options.loadFromMemory) args << "--load-from-memory";
        if (!options.enableSpectrum) args << "--no-spectrum";
        const QString propsFile = writeUserPropertiesFile(options.userProperties, wallpaper);
        // A non-empty property set must reach SceneWallpaper atomically. An
        // empty path in this case means serialization failed, not that the
        // renderer may start with authored defaults.
        if (!options.userProperties.isEmpty() && propsFile.isEmpty()) {
            if (error) *error = QStringLiteral("无法写入场景用户属性临时文件");
            delete running;
            process->deleteLater();
            return false;
        }
        if (!propsFile.isEmpty()) {
            args << "--user-properties" << propsFile;
            running->tempFiles << propsFile;
        }
        QJsonObject storage;
        for (auto it = options.scriptStorage.constBegin(); it != options.scriptStorage.constEnd(); ++it) {
            storage.insert(it.key(), it.value());
        }
        const QJsonObject runtime {
            {QStringLiteral("speed"), options.speed},
            {QStringLiteral("scriptStorage"), storage},
        };
        const QString runtimePath = QDir::temp().filePath(
            QStringLiteral("mirageqt_runtime_%1.json").arg(
                QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QFile runtimeFile(runtimePath);
        if (!runtimeFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = runtimeFile.errorString();
            // Properties were registered before the runtime snapshot because
            // SceneWallpaper consumes both files as one startup transaction.
            for (const QString& temp : running->tempFiles) {
                if (QFileInfo::exists(temp) && !QFile::remove(temp)) {
                    qWarning() << "[Render] Cannot remove temporary file" << temp;
                }
            }
            delete running;
            process->deleteLater();
            return false;
        }
        const QByteArray runtimeData = QJsonDocument(runtime).toJson(QJsonDocument::Compact);
        if (runtimeFile.write(runtimeData) != runtimeData.size()) {
            if (error) *error = runtimeFile.errorString();
            runtimeFile.close();
            const bool removed = QFile::remove(runtimePath);
            if (!removed) qWarning() << "[Render] Cannot remove incomplete runtime file" << runtimePath;
            for (const QString& temp : running->tempFiles) {
                if (QFileInfo::exists(temp) && !QFile::remove(temp)) {
                    qWarning() << "[Render] Cannot remove temporary file" << temp;
                }
            }
            delete running;
            process->deleteLater();
            return false;
        }
        runtimeFile.close();
        if (runtimeFile.error() != QFileDevice::NoError) {
            if (error) *error = runtimeFile.errorString();
            const bool removed = QFile::remove(runtimePath);
            if (!removed) qWarning() << "[Render] Cannot remove failed runtime file" << runtimePath;
            for (const QString& temp : running->tempFiles) {
                if (QFileInfo::exists(temp) && !QFile::remove(temp)) {
                    qWarning() << "[Render] Cannot remove temporary file" << temp;
                }
            }
            delete running;
            process->deleteLater();
            return false;
        }
        args << "--runtime" << runtimePath;
        running->tempFiles << runtimePath;
        break;
    }
    case WallpaperKind::Video:
        args << wallpaper.renderDirectory
             << "--volume" << number(options.volume)
             << "--fill" << fillModeKey(options.fillMode);
#ifdef Q_OS_WIN
        args << "--fps" << QString::number(options.fps)
             << "--speed" << number(options.speed)
             << "--position-x" << number(options.position.x())
             << "--position-y" << number(options.position.y());
#endif
        if (options.muted) args << "--muted";
        if (options.loadFromMemory) args << "--load-from-memory";
        args << "--control-stdin";
            env.insert("LC_NUMERIC", "C");  // From mpv: Non-C locale detected. This is not supported.
                                                             // Call 'setlocale(LC_NUMERIC, "C");' in your code.
        break;
    case WallpaperKind::Web:
        // WebWallpaper resolves preset files in this declared order, so the
        // preset directory can replace base-project assets without changing
        // project property paths. The base render directory remains the
        // positional wallpaper root required by its manifest loader.
        args << wallpaper.renderDirectory;
        for (const QString& overlay : wallpaper.assetOverlayDirectories) {
            args << "--asset-overlay" << overlay;
        }
        args << "--fps" << QString::number(options.fps)
             << "--volume" << number(options.volume)
             << "--control-stdin";
        if (options.muted) args << "--muted";
        if (options.loadFromMemory) args << "--load-from-memory";
        if (!options.enableSpectrum) args << "--no-spectrum";
        break;
    case WallpaperKind::Unsupported:
        delete running;
        process->deleteLater();
        return false;
    }

    connect(process, &QProcess::readyReadStandardError, this, [running, process] {
        const QString text = QString::fromUtf8(process->readAllStandardError()).trimmed();
        if (!text.isEmpty()) writeRendererDiagnostic(running->screenIndex, text);
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, running, process] {
        consumeStdout(running, process->readAllStandardOutput());
    });

    connect(process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this, running](int exitCode, QProcess::ExitStatus exitStatus) {
                const int screen = running->screenIndex;
                const bool abnormal = !running->stopping &&
                                      (exitStatus != QProcess::NormalExit || exitCode != 0);
                const bool wasCurrent = m_running.value(screen) == running;
                const Wallpaper exitedWallpaper = running->wallpaper;
                const RenderOptions exitedOptions = running->desiredOptions;
                const QString exitedOutputId = running->outputStableId;
                if (wasCurrent) m_running.remove(screen);
                const bool launchPending = wasCurrent && running->stopping &&
                                           m_pendingRenders.contains(screen);
                PendingRender pending;
                if (launchPending) pending = m_pendingRenders.take(screen);
                const QStringList snapshotTokens = m_snapshotRequests.keys();
                for (const QString& token : snapshotTokens) {
                    if (m_snapshotRequests.value(token).process == running->process) {
                        completeSnapshot(token, running->process, false,
                                         QStringLiteral("渲染器已退出"));
                    }
                }
                for (const QString& temp : running->tempFiles) {
                    if (QFileInfo::exists(temp) && !QFile::remove(temp)) {
                        qWarning() << "[Render] Cannot remove temporary file" << temp;
                    }
                }
                running->process->deleteLater();
                delete running;
                if (wasCurrent) emit rendererStateChanged();
                updateMprisMonitoring();
#ifdef Q_OS_WIN
                if (wasCurrent && abnormal) {
                    scheduleRecovery(exitedWallpaper, screen, exitedOutputId, exitedOptions);
                }
#endif
                if (launchPending) {
#ifdef Q_OS_WIN
                    QString launchError;
                    if (!render(pending.wallpaper, screen, pending.options, &launchError) && !launchError.isEmpty())
                        emit rendererMessage(launchError);
#else
                    // 给 broker 100ms 时间完成旧 producer 清理，避免新 producer
                    // 连接时遇到 MD_ERR_STATE 或重复注册错误。
                    qWarning() << "[Switch] Old process finished, waiting 100ms before launching new renderer";
                    QTimer::singleShot(100, this, [this, pending, screen]() {
                        qWarning() << "[Switch] Launching new renderer after cleanup delay";
                        QString launchError;
                        if (!render(pending.wallpaper, screen, pending.options, &launchError)
                            && !launchError.isEmpty()) {
                            emit rendererMessage(launchError);
                        }
                    });
#endif
                }
                emit rendererExited(screen, abnormal);
            });

    process->setProgram(binary);
    process->setArguments(args);
    process->setProcessEnvironment(env);
    process->setProcessChannelMode(QProcess::SeparateChannels);

    // 在 start() 之前插入 m_running，避免竞态：如果进程快速退出，
    // finished 信号处理时 wasCurrent 判断才能正确工作。
    m_running.insert(screenIndex, running);
    qWarning() << "[Render] Inserted into m_running at screenIndex:" << screenIndex << "before start";

    process->start();

    if (!process->waitForStarted(5000)) {
        const QString message = process->errorString();
        for (const QString& temp : running->tempFiles) {
            if (QFileInfo::exists(temp) && !QFile::remove(temp)) {
                qWarning() << "[Render] Cannot remove temporary file" << temp;
            }
        }
        m_running.remove(screenIndex);
        delete running;
        process->deleteLater();
        if (error) *error = message;
        return false;
    }

    qWarning() << "[Render] Process started successfully, PID:" << process->processId();

#ifdef Q_OS_WIN
    auto recovery = m_recoveryStates.find(outputStableId);
    if (recovery == m_recoveryStates.end()) {
        RendererRecoveryState state;
        state.wallpaper = wallpaper;
        state.options = options;
        state.generation = ++m_recoveryGeneration;
        m_recoveryStates.insert(outputStableId, state);
    } else {
        recovery->wallpaper = wallpaper;
        recovery->options = options;
    }
    // Scene config files carry authored properties and script storage, while
    // live volume and final playback policy travel on the control channel.
    // Keep both aligned when a failed scene is relaunched after shell recovery.
    if (wallpaper.kind() == WallpaperKind::Scene) {
        sendCommand(running, QJsonObject{{QStringLiteral("cmd"), QStringLiteral("volume")},
                                         {QStringLiteral("value"), options.volume}});
        if (options.powerState != QStringLiteral("run")) {
            sendCommand(running, QJsonObject{{QStringLiteral("cmd"), QStringLiteral("power")},
                                             {QStringLiteral("state"), options.powerState},
                                             {QStringLiteral("fps"), options.fps}});
        }
    }
#endif

    if (wallpaper.kind() == WallpaperKind::Web) {
        // QtWebEngine navigation is asynchronous. WebWallpaper retains this
        // full snapshot until the page installs its Wallpaper Engine listener,
        // preventing startup properties from being lost before live edits.
        QJsonObject values;
        for (auto it = options.userProperties.constBegin();
             it != options.userProperties.constEnd(); ++it) {
            values.insert(it.key(), QJsonObject{{"value", propertyWireValue(it.value())}});
        }
        sendCommand(running, QJsonObject{
            {"cmd", "setProperties"},
            {"generation", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"values", values},
        });
    }

    emit rendererStateChanged();
    updateMprisMonitoring();
    return true;
}

void RendererController::stop(int screenIndex) {
#ifdef Q_OS_WIN
    cancelRecoveryForScreen(screenIndex);
#endif
    // 显式停止优先于异步切换：移除待请求，禁止 finished 回调重新启动壁纸。
    m_pendingRenders.remove(screenIndex);
    RunningProcess* running = m_running.value(screenIndex);
    if (!running || running->stopping) return;

    running->stopping = true;
    sendCommand(running, QJsonObject{{"cmd", "quit"}});
    running->process->closeWriteChannel();
    updateMprisMonitoring();

    // Escalating shutdown: "quit" first, then terminate() at 1.5 s and
    // kill() at 3 s. Both timers no-op once the process has exited.
    QTimer::singleShot(1500, running->process, [process = running->process] {
        if (process->state() != QProcess::NotRunning) process->terminate();
    });
    QTimer::singleShot(3000, running->process, [process = running->process] {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
    emit rendererStateChanged();
}

void RendererController::stopOutput(const QString& stableOutputId) {
    if (stableOutputId.isEmpty()) return;
#ifdef Q_OS_WIN
    cancelRecovery(stableOutputId);
#endif
    for (auto it = m_running.constBegin(); it != m_running.constEnd(); ++it) {
        if (it.value() && it.value()->outputStableId == stableOutputId) {
            stop(it.key());
            return;
        }
    }
}

void RendererController::stopAll() {
#ifdef Q_OS_WIN
    m_recoveryStates.clear();
#endif
    // 应用退出和“停止全部”均不得保留延迟启动意图。
    m_pendingRenders.clear();
    const QVector<int> screens = activeScreens();
    for (int screen : screens) stop(screen);
}

QVector<int> RendererController::activeScreens() const {
    QVector<int> screens;
    for (auto it = m_running.constBegin(); it != m_running.constEnd(); ++it) {
        if (!it.value()->stopping) screens.push_back(it.key());
    }
    std::sort(screens.begin(), screens.end());
    return screens;
}

bool RendererController::isRunningOnScreen(int screenIndex) const {
    const RunningProcess* const running = m_running.value(screenIndex);
    return running != nullptr && !running->stopping;
}

QString RendererController::wallpaperIdOnScreen(int screenIndex) const {
    const RunningProcess* running = m_running.value(screenIndex);
    return running != nullptr && !running->stopping ? running->wallpaper.id() : QString();
}

PositionAvailability RendererController::positionAvailabilityForWallpaper(
    const QString& wallpaperId) const {
    for (const RunningProcess* running : m_running) {
        if (!running->stopping && running->wallpaper.id() == wallpaperId &&
            running->positionAvailability.known) {
            return running->positionAvailability;
        }
    }
    return PositionAvailability();
}

QString RendererController::requestSnapshot(int screenIndex, const QString& path) {
    RunningProcess* running = m_running.value(screenIndex);
    if (path.isEmpty() || running == nullptr || running->stopping ||
        running->wallpaper.kind() != WallpaperKind::Scene) {
        emit snapshotFinished(QString(), screenIndex, path, false,
                              QStringLiteral("目标显示器没有活动的场景壁纸"));
        return QString();
    }
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_snapshotRequests.insert(token, SnapshotRequest{screenIndex, path, running->process});
    sendCommand(running, QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("snapshot")},
        {QStringLiteral("path"), path},
        {QStringLiteral("token"), token},
    });
    QTimer::singleShot(8000, this, [this, token, process = QPointer<QProcess>(running->process)] {
        if (!m_snapshotRequests.contains(token)) return;
        completeSnapshot(token, process.data(), false, QStringLiteral("截图请求超时"));
    });
    return token;
}

QString RendererController::fillModeKey(FillMode mode) {
    switch (mode) {
    case FillMode::Cover: return QStringLiteral("cover");
    case FillMode::Contain: return QStringLiteral("contain");
    case FillMode::Stretch: return QStringLiteral("stretch");
    }
    return QStringLiteral("cover");
}

void RendererController::setPowerState(const QString& state, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        if (state == QStringLiteral("run") || state == QStringLiteral("throttle") ||
            state == QStringLiteral("pause")) {
            running->desiredOptions.powerState = state;
        }
        sendCommand(running, QJsonObject{{QStringLiteral("cmd"), QStringLiteral("power")},
                                         {QStringLiteral("state"), state},
                                         {QStringLiteral("fps"), running->desiredOptions.fps}});
    });
}

QString RendererController::stableOutputId(const QScreen* screen) {
    if (screen == nullptr) return {};
#ifdef Q_OS_WIN
    const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (native && GetMonitorInfoW(native->handle(), &monitor))
        return QString::fromWCharArray(monitor.szDevice);
    qWarning() << "[Display] Cannot obtain native Windows device for screen" << screen->name();
    return {};
#else
    const QString manufacturer = screen->manufacturer().trimmed();
    const QString model = screen->model().trimmed();
    const QString serial = screen->serialNumber().trimmed();
    const QString connector = screen->name().trimmed();
    const QString identity = serial.isEmpty()
                                 ? QStringList {manufacturer, model, connector}.join('|')
                                 : QStringList {manufacturer, model, serial}.join('|');
    return QStringLiteral("kde:") + identity;
#endif
}

void RendererController::setVolume(double volume, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        running->desiredOptions.volume = volume;
        sendCommand(running, QJsonObject{{"cmd", "volume"}, {"value", volume}});
    });
}

void RendererController::setMuted(bool muted, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        running->desiredOptions.muted = muted;
        sendCommand(running, QJsonObject{{"cmd", "muted"}, {"value", muted}});
    });
}

void RendererController::pause(int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        running->desiredOptions.powerState = QStringLiteral("pause");
        sendCommand(running, QJsonObject{{"cmd", "pause"}});
    });
}

void RendererController::resume(int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        running->desiredOptions.powerState = QStringLiteral("run");
        sendCommand(running, QJsonObject{{"cmd", "resume"}});
    });
}

void RendererController::setFps(int fps, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        running->desiredOptions.fps = fps;
        sendCommand(running, QJsonObject{{"cmd", "fps"}, {"value", fps}});
    });
}

void RendererController::setSpeed(double speed, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        running->desiredOptions.speed = speed;
        sendCommand(running, QJsonObject{{"cmd", "speed"}, {"value", speed}});
    });
}

void RendererController::setFillMode(FillMode mode, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        running->desiredOptions.fillMode = mode;
        sendCommand(running, QJsonObject{{"cmd", "fillmode"}, {"value", fillModeKey(mode)}});
    });
}

void RendererController::setPosition(const QPointF& position, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        if (running->wallpaper.kind() != WallpaperKind::Scene) return;
        running->desiredOptions.position = position;
        sendCommand(running, QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("position")},
            {QStringLiteral("x"), position.x()},
            {QStringLiteral("y"), position.y()},
        });
    });
}

void RendererController::setProperty(const QString& key, const ProjectProperty& property, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        running->desiredOptions.userProperties.insert(key, property);
        sendCommand(running, propertyCommand(key, property));
    });
}

QString RendererController::binaryForKind(WallpaperKind kind) const {
    switch (kind) {
    case WallpaperKind::Scene: return sceneWallpaperBinary();
    case WallpaperKind::Web: return webWallpaperBinary();
    case WallpaperKind::Video: return videoWallpaperBinary();
    case WallpaperKind::Unsupported: return {};
    }
    return {};
}

QString RendererController::sceneWallpaperBinary() const {
#ifdef Q_OS_WIN
    return firstExecutable({
        siblingBinary("SceneWallpaper.exe"),
        QDir::cleanPath(QStringLiteral(MIRAGEQT_RUNTIME_DIR) + "/SceneWallpaper.exe"),
        QDir::cleanPath(Paths::repoRoot() + "/SceneRenderer/build/windows/Tools/SceneWallpaper/SceneWallpaper.exe"),
        QDir::cleanPath(Paths::repoRoot() + "/SceneRenderer/build/windows-release/Tools/SceneWallpaper/SceneWallpaper.exe"),
        QStandardPaths::findExecutable(QStringLiteral("SceneWallpaper.exe")),
    });
#else
    return firstExecutable({
        QDir::cleanPath(QStringLiteral(MIRAGEQT_RUNTIME_DIR) + "/SceneWallpaper"),
        siblingBinary("SceneWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/SceneRenderer/build/linux-clang-release/Tools/SceneWallpaper/SceneWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/SceneRenderer/build/release/Tools/SceneWallpaper/SceneWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/SceneRenderer/cmake-build-debug-clang-21/Tools/SceneWallpaper/SceneWallpaper"),
    });
#endif
}

QString RendererController::webWallpaperBinary() const {
#ifdef Q_OS_WIN
    return firstExecutable({
        siblingBinary("WebWallpaper.exe"),
        QDir::cleanPath(QStringLiteral(MIRAGEQT_RUNTIME_DIR) + "/WebWallpaper.exe"),
        QDir::cleanPath(Paths::repoRoot() + "/WebRenderer/build/windows/Tools/WebWallpaper/WebWallpaper.exe"),
        QDir::cleanPath(Paths::repoRoot() + "/WebRenderer/build/windows-release/Tools/WebWallpaper/WebWallpaper.exe"),
        QStandardPaths::findExecutable(QStringLiteral("WebWallpaper.exe")),
    });
#else
    return firstExecutable({
        QDir::cleanPath(QStringLiteral(MIRAGEQT_RUNTIME_DIR) + "/WebWallpaper"),
        siblingBinary("WebWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/WebRenderer/build/linux-release/Tools/WebWallpaper/WebWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/WebRenderer/build/linux-debug/Tools/WebWallpaper/WebWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/WebRenderer/build/linux/Tools/WebWallpaper/WebWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/WebRenderer/build/release/Tools/WebWallpaper/WebWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/WebRenderer/build/debug/Tools/WebWallpaper/WebWallpaper"),
    });
#endif
}

QString RendererController::videoWallpaperBinary() const {
#ifdef Q_OS_WIN
    return firstExecutable({
        siblingBinary("VideoWallpaper.exe"),
        QDir::cleanPath(QStringLiteral(MIRAGEQT_RUNTIME_DIR) + "/VideoWallpaper.exe"),
        QDir::cleanPath(Paths::repoRoot() + "/VideoRenderer/build/windows/Tools/VideoWallpaper/VideoWallpaper.exe"),
        QDir::cleanPath(Paths::repoRoot() + "/VideoRenderer/build/windows-release/Tools/VideoWallpaper/VideoWallpaper.exe"),
        QStandardPaths::findExecutable(QStringLiteral("VideoWallpaper.exe")),
    });
#else
    return firstExecutable({
        QDir::cleanPath(QStringLiteral(MIRAGEQT_RUNTIME_DIR) + "/VideoWallpaper"),
        siblingBinary("VideoWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/VideoRenderer/build/linux-clang-release/Tools/VideoWallpaper/VideoWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/VideoRenderer/build/debug/Tools/VideoWallpaper/VideoWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/VideoRenderer/cmake-build-debug-clang-21/Tools/VideoWallpaper/VideoWallpaper"),
    });
#endif
}

QString RendererController::writeUserPropertiesFile(const QHash<QString, ProjectProperty>& props, const Wallpaper& wallpaper) const {
    if (props.isEmpty()) return {};

    QJsonObject object;
    for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
        const auto kind = it.value().propertyKind();
        if (kind == PropertyKind::Color) {
            object.insert(it.key(), QJsonObject{{"type", "color"}, {"value", it.value().stringValue()}});
        } else if (kind == PropertyKind::SceneTexture || kind == PropertyKind::File) {
            object.insert(it.key(), QJsonObject{{"type", "scenetexture"}, {"value", it.value().stringValue()}});
        } else {
            object.insert(it.key(), propertyWireValue(it.value()));
        }
    }

    Q_UNUSED(wallpaper)
    const QString path = QDir::temp().filePath(QStringLiteral("mirageqt_props_%1.json")
                                                   .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size()) {
        qWarning() << "[Render] Cannot write user-properties file" << path << file.errorString();
        file.close();
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            qWarning() << "[Render] Cannot remove incomplete user-properties file" << path;
        }
        return {};
    }
    file.close();
    if (file.error() != QFileDevice::NoError) {
        qWarning() << "[Render] Cannot close user-properties file" << path << file.errorString();
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            qWarning() << "[Render] Cannot remove failed user-properties file" << path;
        }
        return {};
    }
    return path;
}

QJsonObject RendererController::propertyCommand(const QString& key, const ProjectProperty& property) const {
    QJsonObject object{{"cmd", "setProperty"}, {"key", key}};
    const auto kind = property.propertyKind();
    if (kind == PropertyKind::Color) {
        object.insert("type", "color");
        object.insert("value", property.stringValue());
    } else if (kind == PropertyKind::SceneTexture || kind == PropertyKind::File) {
        object.insert("type", "scenetexture");
        object.insert("value", property.stringValue());
    } else {
        object.insert("value", propertyWireValue(property));
    }
    return object;
}

void RendererController::sendCommand(RunningProcess* running, const QJsonObject& command) {
    if (!running || running->process->state() == QProcess::NotRunning) return;
    QByteArray line = QJsonDocument(command).toJson(QJsonDocument::Compact);
    line.push_back('\n');
    const qint64 accepted = running->process->write(line);
    if (accepted != line.size()) {
        writeRendererDiagnostic(running->screenIndex,
                                QStringLiteral("控制命令写入失败：%1")
                                    .arg(running->process->errorString()));
    }
}

void RendererController::forEachTarget(int screenIndex, const std::function<void(RunningProcess*)>& body) {
    const auto apply = [this, &body](RunningProcess* running) {
        if (!running) return;
        body(running);
#ifdef Q_OS_WIN
        auto recovery = m_recoveryStates.find(running->outputStableId);
        if (recovery != m_recoveryStates.end()) {
            recovery->wallpaper = running->wallpaper;
            recovery->options = running->desiredOptions;
        }
#endif
    };
    if (screenIndex >= 0) {
        apply(m_running.value(screenIndex));
        return;
    }
    for (RunningProcess* running : m_running) apply(running);
}

#ifdef Q_OS_WIN
void RendererController::updateMprisMonitoring() {
    bool needed = false;
    for (const RunningProcess* running : m_running) {
        if (!running->stopping && running->wallpaper.kind() == WallpaperKind::Scene) {
            needed = true;
            break;
        }
    }
    if (needed == m_mprisMonitoring) {
        if (needed) {
            publishWindowsMediaStatus(m_windowsMediaState->lastStatus);
            refreshWindowsMediaStatus();
        }
        return;
    }

    m_mprisMonitoring = needed;
    const std::shared_ptr<WindowsMediaSessionState> state = m_windowsMediaState;
    if (!needed) {
        m_windowsMediaTimer->stop();
        state->active.store(false);
        state->generation.store(++m_windowsMediaGeneration);
        state->managerRequestGeneration.store(0);
        state->refreshGeneration.store(0);
        {
            std::lock_guard lock(state->managerMutex);
            state->manager = nullptr;
        }
        state->lastStatus = idleWindowsMediaStatus();
        return;
    }

    const quint64 generation = ++m_windowsMediaGeneration;
    state->generation.store(generation);
    state->active.store(true);
    state->managerRequestGeneration.store(0);
    state->refreshGeneration.store(0);
    {
        std::lock_guard lock(state->managerMutex);
        state->manager = nullptr;
    }
    state->lastStatus = idleWindowsMediaStatus();
    if (!m_windowsMediaApartmentReady) {
        state->active.store(false);
        publishWindowsMediaStatus(state->lastStatus);
        return;
    }
    m_windowsMediaTimer->start();
    publishWindowsMediaStatus(state->lastStatus);
    refreshWindowsMediaStatus();
}

void RendererController::publishWindowsMediaStatus(const QJsonObject& status) {
    if (!m_mprisMonitoring) return;
    m_windowsMediaState->lastStatus = status;
    forEachTarget(-1, [this, &status](RunningProcess* running) {
        if (running->stopping || running->wallpaper.kind() != WallpaperKind::Scene) return;
        sendCommand(running, QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("mediaStatus")},
            {QStringLiteral("data"), status},
        });
    });
}

void RendererController::refreshWindowsMediaStatus() {
    if (!m_mprisMonitoring || !m_windowsMediaApartmentReady) return;
    const std::shared_ptr<WindowsMediaSessionState> state = m_windowsMediaState;
    const quint64 generation = state->generation.load();
    if (!windowsMediaGenerationIsActive(state, generation)) return;

    WindowsMediaSessionState::Manager manager{nullptr};
    {
        std::lock_guard lock(state->managerMutex);
        manager = state->manager;
    }
    const QPointer<RendererController> owner(this);
    const std::weak_ptr<WindowsMediaSessionState> weakState(state);
    if (!manager) {
        const quint64 previousRequest = state->managerRequestGeneration.exchange(generation);
        if (previousRequest == generation) return;
        try {
            auto request = WindowsMediaSessionState::Manager::RequestAsync();
            request.Completed(
                [weakState, owner, generation](
                    const winrt::Windows::Foundation::IAsyncOperation<
                        WindowsMediaSessionState::Manager>& completed,
                    winrt::Windows::Foundation::AsyncStatus status) {
                const std::shared_ptr<WindowsMediaSessionState> state = weakState.lock();
                if (!state) return;
                quint64 expected = generation;
                state->managerRequestGeneration.compare_exchange_strong(expected, 0);
                if (!windowsMediaGenerationIsActive(state, generation)) return;
                if (status != winrt::Windows::Foundation::AsyncStatus::Completed) {
                    qWarning() << "[Media] GSMTC session manager request failed";
                    return;
                }
                try {
                    auto manager = completed.GetResults();
                    if (!manager || !windowsMediaGenerationIsActive(state, generation)) return;
                    {
                        std::lock_guard lock(state->managerMutex);
                        if (!windowsMediaGenerationIsActive(state, generation)) return;
                        state->manager = manager;
                    }
                    if (!owner) return;
                    QMetaObject::invokeMethod(owner.data(), [owner, weakState, generation] {
                        const std::shared_ptr<WindowsMediaSessionState> current = weakState.lock();
                        if (!owner || !windowsMediaGenerationIsActive(current, generation)) return;
                        owner->refreshWindowsMediaStatus();
                    }, Qt::QueuedConnection);
                } catch (const winrt::hresult_error& error) {
                    qWarning() << "[Media] GSMTC session manager result failed:"
                               << QString::fromStdString(winrt::to_string(error.message()));
                }
            });
        } catch (const winrt::hresult_error& error) {
            quint64 expected = generation;
            state->managerRequestGeneration.compare_exchange_strong(expected, 0);
            qWarning() << "[Media] GSMTC session manager request failed:"
                       << QString::fromStdString(winrt::to_string(error.message()));
        }
        return;
    }

    const quint64 previousRefresh = state->refreshGeneration.exchange(generation);
    if (previousRefresh == generation) return;
    try {
        const auto session = manager.GetCurrentSession();
        if (!session) {
            quint64 expected = generation;
            state->refreshGeneration.compare_exchange_strong(expected, 0);
            publishWindowsMediaStatus(idleWindowsMediaStatus());
            return;
        }

        using MediaProperties =
            winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties;
        auto mediaRequest = session.TryGetMediaPropertiesAsync();
        mediaRequest.Completed(
            [weakState, owner, generation, session](
                const winrt::Windows::Foundation::IAsyncOperation<MediaProperties>& completed,
                winrt::Windows::Foundation::AsyncStatus status) {
            const std::shared_ptr<WindowsMediaSessionState> state = weakState.lock();
            if (!state) return;
            quint64 expected = generation;
            state->refreshGeneration.compare_exchange_strong(expected, 0);
            if (!windowsMediaGenerationIsActive(state, generation)) return;
            if (status != winrt::Windows::Foundation::AsyncStatus::Completed) {
                qWarning() << "[Media] GSMTC media metadata query failed";
                return;
            }

            QJsonObject data;
            try {
                const auto properties = completed.GetResults();
                if (!properties || properties.Title().empty()) {
                    data = idleWindowsMediaStatus();
                } else {
                    const auto playbackStatus = session.GetPlaybackInfo().PlaybackStatus();
                    const auto timeline = session.GetTimelineProperties();
                    const double position = std::max(
                        0.0, std::chrono::duration<double>(timeline.Position()).count());
                    const double duration = std::max(
                        0.0, std::chrono::duration<double>(
                                 timeline.EndTime() - timeline.StartTime()).count());
                    data = QJsonObject{
                        {QStringLiteral("state"), playbackStatus ==
                            winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing
                                ? 1 : 2},
                        {QStringLiteral("title"), windowsMediaString(properties.Title())},
                        {QStringLiteral("artist"), windowsMediaString(properties.Artist())},
                        {QStringLiteral("album"), windowsMediaString(properties.AlbumTitle())},
                        {QStringLiteral("albumArtist"), windowsMediaString(properties.AlbumArtist())},
                        {QStringLiteral("artURL"), QString()},
                        {QStringLiteral("previousArtURL"), QString()},
                        {QStringLiteral("position"), position},
                        {QStringLiteral("duration"), duration},
                    };
                }
            } catch (const winrt::hresult_error& error) {
                qWarning() << "[Media] GSMTC metadata conversion failed:"
                           << QString::fromStdString(winrt::to_string(error.message()));
                return;
            }

            if (!owner) return;
            QMetaObject::invokeMethod(owner.data(), [owner, weakState, generation, data] {
                const std::shared_ptr<WindowsMediaSessionState> current = weakState.lock();
                if (!owner || !windowsMediaGenerationIsActive(current, generation)) return;
                owner->publishWindowsMediaStatus(data);
            }, Qt::QueuedConnection);
        });
    } catch (const winrt::hresult_error& error) {
        quint64 expected = generation;
        state->refreshGeneration.compare_exchange_strong(expected, 0);
        qWarning() << "[Media] GSMTC session query failed:"
                   << QString::fromStdString(winrt::to_string(error.message()));
    }
}
#else
void RendererController::updateMprisMonitoring() {
    bool needed = false;
    for (const RunningProcess* running : m_running) {
        if (!running->stopping && running->wallpaper.kind() == WallpaperKind::Scene) {
            needed = true;
            break;
        }
    }
    if (needed == m_mprisMonitoring) {
        if (needed) publishMprisStatus();
        return;
    }

    m_mprisMonitoring = needed;
    if (!needed) {
        m_mprisPositionTimer->stop();
        if (m_artworkReply != nullptr) {
            m_artworkReply->abort();
            m_artworkReply = nullptr;
        }
        m_mprisPlayers.clear();
        m_selectedMprisService.clear();
        m_mediaIdentity.clear();
        m_artworkSource.clear();
        m_artworkPath.clear();
        m_previousArtworkPath.clear();
        m_artworkPalette = QJsonArray();
        return;
    }

    QDBusConnectionInterface* bus = QDBusConnection::sessionBus().interface();
    if (bus == nullptr) {
        qWarning() << "[MPRIS] Session bus interface is unavailable";
        return;
    }
    const QDBusReply<QStringList> names = bus->registeredServiceNames();
    if (!names.isValid()) {
        qWarning() << "[MPRIS] Cannot enumerate players:" << names.error().message();
        return;
    }
    for (const QString& service : names.value()) {
        if (service.startsWith(QStringLiteral("org.mpris.MediaPlayer2."))) {
            refreshMprisPlayer(service);
        }
    }
    m_mprisPositionTimer->start();
    publishMprisStatus();
}

void RendererController::refreshMprisPlayer(const QString& service) {
    if (!m_mprisMonitoring || !service.startsWith(QStringLiteral("org.mpris.MediaPlayer2."))) {
        return;
    }
    auto* properties = new QDBusInterface(
        service, QStringLiteral("/org/mpris/MediaPlayer2"),
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QDBusConnection::sessionBus(), this);
    QDBusPendingCall call = properties->asyncCall(
        QStringLiteral("GetAll"), QStringLiteral("org.mpris.MediaPlayer2.Player"));
    auto* watcher = new QDBusPendingCallWatcher(call, properties);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, properties, service](QDBusPendingCallWatcher* completed) {
        const QDBusPendingReply<QVariantMap> reply = *completed;
        completed->deleteLater();
        properties->deleteLater();
        if (!m_mprisMonitoring) return;
        if (reply.isError()) {
            qWarning() << "[MPRIS] GetAll failed:" << service << reply.error().message();
            return;
        }
        QDBusConnectionInterface* bus = QDBusConnection::sessionBus().interface();
        if (bus == nullptr) {
            qWarning() << "[MPRIS] Session bus disappeared while reading" << service;
            return;
        }
        const QDBusReply<QString> ownerReply = bus->serviceOwner(service);
        if (!ownerReply.isValid()) {
            qWarning() << "[MPRIS] Cannot resolve service owner:" << service
                       << ownerReply.error().message();
            return;
        }

        const QVariantMap propertiesMap = reply.value();
        MprisPlayer player;
        player.service = service;
        player.owner = ownerReply.value();
        const std::optional<QString> playbackStatus =
            MprisString(propertiesMap, QStringLiteral("PlaybackStatus"));
        const std::optional<qint64> position =
            MprisInt64(propertiesMap, QStringLiteral("Position"));
        // PlaybackStatus and Position are required Player properties. A
        // malformed implementation is excluded instead of publishing an
        // invented stopped state or timestamp.
        if (!playbackStatus.has_value() || !position.has_value()) {
            qWarning() << "[MPRIS] Player omitted required properties:" << service;
            return;
        }
        player.playbackStatus = *playbackStatus;
        player.positionUs = *position;
        player.updatedOrder = ++m_mprisUpdateOrder;

        const auto metadataValue = propertiesMap.constFind(QStringLiteral("Metadata"));
        if (metadataValue != propertiesMap.constEnd()) {
            const QVariantMap metadata = qdbus_cast<QVariantMap>(*metadataValue);
            if (const std::optional<QString> title =
                    MprisString(metadata, QStringLiteral("xesam:title")); title.has_value()) {
                player.title = *title;
            }
            if (const std::optional<QStringList> artist =
                    MprisStringList(metadata, QStringLiteral("xesam:artist")); artist.has_value()) {
                player.artist = artist->join(QStringLiteral(", "));
            }
            if (const std::optional<QString> album =
                    MprisString(metadata, QStringLiteral("xesam:album")); album.has_value()) {
                player.album = *album;
            }
            if (const std::optional<QStringList> albumArtist =
                    MprisStringList(metadata, QStringLiteral("xesam:albumArtist"));
                albumArtist.has_value()) {
                player.albumArtist = albumArtist->join(QStringLiteral(", "));
            }
            if (const std::optional<QString> artUrl =
                    MprisString(metadata, QStringLiteral("mpris:artUrl")); artUrl.has_value()) {
                player.artUrl = *artUrl;
            }
            const auto trackId = metadata.constFind(QStringLiteral("mpris:trackid"));
            if (trackId != metadata.constEnd()) {
                if (trackId->metaType().id() != qMetaTypeId<QDBusObjectPath>()) {
                    qWarning() << "[MPRIS] Property has non-object-path type: mpris:trackid"
                               << trackId->metaType().name();
                } else {
                    player.trackId = trackId->value<QDBusObjectPath>().path();
                }
            }
            if (const std::optional<qint64> duration =
                    MprisInt64(metadata, QStringLiteral("mpris:length")); duration.has_value()) {
                player.durationUs = *duration;
            }
        }
        m_mprisPlayers.insert(service, player);
        publishMprisStatus();
    });
}

void RendererController::onMprisPropertiesChanged(
    const QString& interfaceName, const QVariantMap& changedProperties,
    const QStringList& invalidatedProperties) {
    if (!m_mprisMonitoring ||
        interfaceName != QStringLiteral("org.mpris.MediaPlayer2.Player") ||
        (changedProperties.isEmpty() && invalidatedProperties.isEmpty())) {
        return;
    }
    const QString owner = message().service();
    for (const MprisPlayer& player : m_mprisPlayers) {
        if (player.owner == owner) refreshMprisPlayer(player.service);
    }
}

void RendererController::publishMprisStatus() {
    if (!m_mprisMonitoring) return;

    const MprisPlayer* selected = nullptr;
    for (const MprisPlayer& player : m_mprisPlayers) {
        if (player.title.isEmpty()) continue;
        if (selected == nullptr) {
            selected = &player;
            continue;
        }
        const bool playing = player.playbackStatus == QStringLiteral("Playing");
        const bool selectedPlaying = selected->playbackStatus == QStringLiteral("Playing");
        if ((playing && !selectedPlaying) ||
            (playing == selectedPlaying && player.updatedOrder > selected->updatedOrder)) {
            selected = &player;
        }
    }

    QJsonObject data;
    if (selected == nullptr) {
        m_selectedMprisService.clear();
        const QString previous = m_artworkPath;
        m_mediaIdentity.clear();
        m_artworkSource.clear();
        m_artworkPath.clear();
        m_previousArtworkPath.clear();
        m_artworkPalette = QJsonArray();
        data = QJsonObject{
            {QStringLiteral("state"), 0},
            {QStringLiteral("title"), QString()},
            {QStringLiteral("artist"), QString()},
            {QStringLiteral("album"), QString()},
            {QStringLiteral("albumArtist"), QString()},
            {QStringLiteral("position"), 0.0},
            {QStringLiteral("duration"), 0.0},
            {QStringLiteral("artURL"), QString()},
            {QStringLiteral("previousArtURL"), previous},
        };
    } else {
        m_selectedMprisService = selected->service;
        const QString identity = QStringList{
            selected->service, selected->trackId, selected->title,
            selected->artist, selected->album, selected->albumArtist,
        }.join(QChar(0x1f));
        if (identity != m_mediaIdentity) {
            if (!m_artworkPath.isEmpty()) m_previousArtworkPath = m_artworkPath;
            m_artworkPath.clear();
            m_artworkPalette = QJsonArray();
            m_artworkSource.clear();
            m_mediaIdentity = identity;
            if (m_artworkReply != nullptr) {
                m_artworkReply->abort();
                m_artworkReply = nullptr;
            }
        }

        if (!selected->artUrl.isEmpty() && selected->artUrl != m_artworkSource) {
            const QUrl artworkUrl(selected->artUrl);
            const QString scheme = artworkUrl.scheme().toLower();
            m_artworkSource = selected->artUrl;
            if (artworkUrl.isValid() && scheme == QStringLiteral("file")) {
                const QString path = artworkUrl.toLocalFile();
                const QImage image(path);
                const QJsonArray palette = MprisPalette(image);
                if (!image.isNull() && palette.size() == 5) {
                    if (!m_artworkPath.isEmpty() && m_artworkPath != path) {
                        m_previousArtworkPath = m_artworkPath;
                    }
                    m_artworkPath = path;
                    m_artworkPalette = palette;
                } else {
                    qWarning() << "[MPRIS] Cannot decode local artwork:" << path;
                }
            } else if (artworkUrl.isValid() &&
                       (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))) {
                QNetworkRequest request(artworkUrl);
                request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                     QNetworkRequest::NoLessSafeRedirectPolicy);
                QNetworkReply* reply = m_networkAccess->get(request);
                m_artworkReply = reply;
                const QString requestedIdentity = m_mediaIdentity;
                const QString requestedSource = m_artworkSource;
                connect(reply, &QNetworkReply::finished, this,
                        [this, reply, requestedIdentity, requestedSource] {
                    if (m_artworkReply == reply) m_artworkReply = nullptr;
                    const QNetworkReply::NetworkError networkError = reply->error();
                    const QByteArray data = reply->readAll();
                    const QString contentType = reply->header(
                        QNetworkRequest::ContentTypeHeader).toString().section(';', 0, 0).toLower();
                    reply->deleteLater();
                    if (requestedIdentity != m_mediaIdentity || requestedSource != m_artworkSource) {
                        return;
                    }
                    if (networkError != QNetworkReply::NoError) {
                        qWarning() << "[MPRIS] Artwork download failed:" << requestedSource
                                   << networkError;
                        return;
                    }
                    QString extension;
                    if (contentType == QStringLiteral("image/png")) extension = QStringLiteral("png");
                    else if (contentType == QStringLiteral("image/jpeg")) extension = QStringLiteral("jpg");
                    else {
                        qWarning() << "[MPRIS] Unsupported artwork content type:" << contentType;
                        return;
                    }
                    QImage image;
                    if (!image.loadFromData(data)) {
                        qWarning() << "[MPRIS] Cannot decode downloaded artwork:" << requestedSource;
                        return;
                    }
                    const QJsonArray palette = MprisPalette(image);
                    if (palette.size() != 5) {
                        qWarning() << "[MPRIS] Cannot derive artwork palette:" << requestedSource;
                        return;
                    }
                    const QString directory = QDir(
                        QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                                                  .filePath(QStringLiteral("NowPlaying"));
                    if (!QDir().mkpath(directory)) {
                        qWarning() << "[MPRIS] Cannot create artwork cache:" << directory;
                        return;
                    }
                    const QString digest = QString::fromLatin1(
                        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
                    const QString path = QDir(directory).filePath(digest + QLatin1Char('.') + extension);
                    if (!QFileInfo::exists(path)) {
                        QSaveFile file(path);
                        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() ||
                            !file.commit()) {
                            qWarning() << "[MPRIS] Cannot persist artwork:" << path
                                       << file.errorString();
                            return;
                        }
                    }
                    if (!m_artworkPath.isEmpty() && m_artworkPath != path) {
                        m_previousArtworkPath = m_artworkPath;
                    }
                    m_artworkPath = path;
                    m_artworkPalette = palette;
                    publishMprisStatus();
                });
            } else {
                qWarning() << "[MPRIS] Rejected artwork URL:" << selected->artUrl;
            }
        }

        data = QJsonObject{
            {QStringLiteral("state"), selected->playbackStatus == QStringLiteral("Playing") ? 1 : 2},
            {QStringLiteral("title"), selected->title},
            {QStringLiteral("artist"), selected->artist},
            {QStringLiteral("album"), selected->album},
            {QStringLiteral("albumArtist"), selected->albumArtist},
            {QStringLiteral("position"), static_cast<double>(selected->positionUs) / 1000000.0},
            {QStringLiteral("duration"), static_cast<double>(selected->durationUs) / 1000000.0},
            {QStringLiteral("artURL"), m_artworkPath},
            {QStringLiteral("previousArtURL"), m_previousArtworkPath},
        };
        if (m_artworkPalette.size() == 5) {
            data.insert(QStringLiteral("primaryColor"), m_artworkPalette.at(0));
            data.insert(QStringLiteral("secondaryColor"), m_artworkPalette.at(1));
            data.insert(QStringLiteral("tertiaryColor"), m_artworkPalette.at(2));
            data.insert(QStringLiteral("textColor"), m_artworkPalette.at(3));
            data.insert(QStringLiteral("highContrastColor"), m_artworkPalette.at(4));
        }
    }

    forEachTarget(-1, [this, &data](RunningProcess* running) {
        if (running->wallpaper.kind() != WallpaperKind::Scene) return;
        sendCommand(running, QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("mediaStatus")},
            {QStringLiteral("data"), data},
        });
    });
}
#endif

void RendererController::consumeStdout(RunningProcess* running, const QByteArray& chunk) {
    if (!running || chunk.isEmpty()) return;
    running->stdoutBuffer.append(chunk);
    while (true) {
        const int newline = running->stdoutBuffer.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = running->stdoutBuffer.left(newline).trimmed();
        running->stdoutBuffer.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        const auto doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) continue;
        const QJsonObject object = doc.object();
        const QString event = object.value(QStringLiteral("event")).toString();
        if (event == QStringLiteral("video-did-end")) {
            emit videoDidEnd(running->screenIndex);
        } else if (event == QStringLiteral("first-frame-presented")) {
            if (running->stopping) continue;
#ifdef Q_OS_WIN
            auto recovery = m_recoveryStates.find(running->outputStableId);
            if (recovery != m_recoveryStates.end()) recovery->attempts = 0;
#endif
            emit firstFramePresented(running->screenIndex);
            if (running->wallpaper.kind() == WallpaperKind::Scene) {
                const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
                sendCommand(running, QJsonObject{
                    {QStringLiteral("cmd"), QStringLiteral("exportScriptStorage")},
                    {QStringLiteral("token"), token},
                });
            }
        } else if (event == QStringLiteral("capability")) {
            if (object.value(QStringLiteral("supported")).isBool() &&
                !object.value(QStringLiteral("supported")).toBool()) {
                const QString command = object.value(QStringLiteral("command")).toString();
                writeRendererDiagnostic(running->screenIndex,
                    QStringLiteral("渲染器不支持控制命令：%1").arg(command));
            }
        } else if (event == QStringLiteral("renderer-error")) {
            emit rendererMessage(QStringLiteral("场景渲染器发生不可恢复的 Vulkan 错误"));
            running->process->terminate();
        } else if (event == QStringLiteral("position-availability")) {
            const QJsonValue x = object.value(QStringLiteral("x"));
            const QJsonValue y = object.value(QStringLiteral("y"));
            if (!x.isBool() || !y.isBool()) {
                writeRendererDiagnostic(running->screenIndex,
                                        QStringLiteral("无效的 position-availability 事件"));
                continue;
            }
            running->positionAvailability = PositionAvailability{true, x.toBool(), y.toBool()};
            emit positionAvailabilityChanged(running->screenIndex, x.toBool(), y.toBool());
        } else if (event == QStringLiteral("audio-demand")) {
            const QJsonValue needed = object.value(QStringLiteral("needed"));
            if (!needed.isBool()) {
                writeRendererDiagnostic(running->screenIndex,
                                        QStringLiteral("无效的 audio-demand 事件"));
                continue;
            }
            running->audioDemanded = needed.toBool();
        } else if (event == QStringLiteral("script-storage")) {
            const QJsonValue valuesValue = object.value(QStringLiteral("values"));
            if (!valuesValue.isObject()) {
                writeRendererDiagnostic(running->screenIndex,
                                        QStringLiteral("无效的 script-storage 事件"));
                continue;
            }
            const QJsonObject valuesObject = valuesValue.toObject();
            if (valuesObject.size() > 1024) {
                writeRendererDiagnostic(running->screenIndex,
                                        QStringLiteral("脚本存储超过 1024 项限制"));
                continue;
            }
            QHash<QString, QString> values;
            bool valid = true;
            for (auto it = valuesObject.constBegin(); it != valuesObject.constEnd(); ++it) {
                if (!it.value().isString()) {
                    valid = false;
                    break;
                }
                values.insert(it.key(), it.value().toString());
            }
            if (!valid) {
                writeRendererDiagnostic(running->screenIndex,
                                        QStringLiteral("脚本存储包含非字符串值"));
                continue;
            }
            running->desiredOptions.scriptStorage = values;
#ifdef Q_OS_WIN
            auto recovery = m_recoveryStates.find(running->outputStableId);
            if (recovery != m_recoveryStates.end()) recovery->options.scriptStorage = values;
#endif
            emit scriptStorageChanged(running->wallpaper.id(), values);
        } else if (event == QStringLiteral("snapshot-done")) {
            const QJsonValue tokenValue = object.value(QStringLiteral("token"));
            const QJsonValue okValue = object.value(QStringLiteral("ok"));
            if (!tokenValue.isString() || !okValue.isBool()) {
                writeRendererDiagnostic(running->screenIndex,
                                        QStringLiteral("无效的 snapshot-done 事件"));
                continue;
            }
            completeSnapshot(tokenValue.toString(), running->process, okValue.toBool(),
                             okValue.toBool() ? QString() : QStringLiteral("渲染器截图失败"));
        } else if (event == QStringLiteral("open-shortcut")) {
            const QJsonValue targetValue = object.value(QStringLiteral("value"));
            if (!targetValue.isString()) {
                writeRendererDiagnostic(running->screenIndex,
                                        QStringLiteral("无效的 open-shortcut 事件"));
                continue;
            }
            const QString target = targetValue.toString();
            const QFileInfo localFile(target);
            bool opened = false;
            if (localFile.exists()) {
                opened = QDesktopServices::openUrl(QUrl::fromLocalFile(localFile.absoluteFilePath()));
            } else {
                const QUrl url(target);
                const QString scheme = url.scheme().toLower();
                if (url.isValid() && (scheme == QStringLiteral("http") ||
                                      scheme == QStringLiteral("https") ||
                                      scheme == QStringLiteral("mailto"))) {
                    opened = QDesktopServices::openUrl(url);
                }
            }
            if (!opened) {
                writeRendererDiagnostic(running->screenIndex,
                                        QStringLiteral("已拒绝或无法打开用户快捷方式"));
            }
        }
    }
}

void RendererController::completeSnapshot(const QString& token, QProcess* source, bool success,
                                          const QString& error) {
    const auto request = m_snapshotRequests.constFind(token);
    if (request == m_snapshotRequests.constEnd() || request->process != source) return;
    const SnapshotRequest completed = request.value();
    m_snapshotRequests.erase(request);
    emit snapshotFinished(token, completed.screenIndex, completed.path, success, error);
}

} // namespace Mirage
