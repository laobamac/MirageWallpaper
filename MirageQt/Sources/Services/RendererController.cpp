#include "Services/RendererController.h"

#include "Services/LinuxSystemIntegration.h"
#include "Services/Paths.h"
#include "Services/DisplayBrokerService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

#include <functional>

namespace Mirage {
namespace {

QString number(double value) {
    return QString::number(value, 'f', 3);
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

} // namespace

RendererController::RendererController(GlobalSettingsService* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings) {
    qRegisterMetaType<Mirage::FillMode>();
}

RendererController::~RendererController() {
    stopAll();
}

bool RendererController::render(const Wallpaper& wallpaper, int screenIndex, const RenderOptions& options, QString* error) {
    if (!wallpaper.isValid()) {
        if (error) *error = QStringLiteral("壁纸无效或缺少预设依赖");
        return false;
    }

    const QString unsupported = LinuxSystemIntegration::wallpaperUnsupportedReason();
    if (!unsupported.isEmpty()) {
        if (error) *error = unsupported;
        return false;
    }

    if (wallpaper.kind() == WallpaperKind::Web) {
        if (error) *error = QStringLiteral("Linux WebRenderer 尚未实现。");
        emit rendererMessage(error ? *error : QString());
        return false;
    }

    const QString binary = binaryForKind(wallpaper.kind());
    if (binary.isEmpty()) {
        if (error) *error = QStringLiteral("找不到渲染器二进制");
        return false;
    }

    stop(screenIndex);

    auto* process = new QProcess(this);
    auto* running = new RunningProcess;
    running->process = process;
    running->wallpaper = wallpaper;
    running->screenIndex = screenIndex;
    const QList<QScreen*> screens = QGuiApplication::screens();
    QScreen* targetScreen = screens.isEmpty()
                                ? nullptr
                                : screens.at(qBound(0, screenIndex, screens.size() - 1));
    running->outputStableId = stableOutputId(targetScreen);
    if (running->outputStableId.isEmpty()) {
        delete running;
        process->deleteLater();
        if (error) *error = QStringLiteral("无法确定目标显示器标识，无法应用壁纸");
        return false;
    }

    QStringList args;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Wallpaper hosts display exclusively through the mirage-display protocol.
    args << "--display-output-id" << running->outputStableId
         << "--display-socket" << DisplayBrokerService::defaultSocketPath();

    switch (wallpaper.kind()) {
    case WallpaperKind::Scene: {
        args << Paths::assetsDir()
             << wallpaper.resolvedEntryPath()
             << "--fps" << QString::number(options.fps)
             << "--control-stdin";
        if (options.muted) args << "--muted";
        if (options.loadFromMemory) args << "--load-from-memory";
        const QString propsFile = writeUserPropertiesFile(options.userProperties, wallpaper);
        if (!propsFile.isEmpty()) {
            args << "--user-properties" << propsFile;
            running->tempFiles << propsFile;
        }
        break;
    }
    case WallpaperKind::Video:
        args << wallpaper.renderDirectory
             << "--volume" << number(options.volume)
             << "--fill" << fillModeKey(options.fillMode);
        if (options.muted) args << "--muted";
        if (options.loadFromMemory) args << "--load-from-memory";
        args << "--control-stdin";
        break;
    case WallpaperKind::Web:
    case WallpaperKind::Unsupported:
        delete running;
        process->deleteLater();
        return false;
    }

    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        const QString text = QString::fromUtf8(process->readAllStandardError()).trimmed();
        if (!text.isEmpty()) emit rendererMessage(text);
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
                if (m_running.value(screen) == running) m_running.remove(screen);
                for (const QString& temp : running->tempFiles) QFile::remove(temp);
                running->process->deleteLater();
                delete running;
                emit rendererExited(screen, abnormal);
            });

    process->setProgram(binary);
    process->setArguments(args);
    process->setProcessEnvironment(env);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->start();

    if (!process->waitForStarted(5000)) {
        const QString message = process->errorString();
        for (const QString& temp : running->tempFiles) QFile::remove(temp);
        delete running;
        process->deleteLater();
        if (error) *error = message;
        return false;
    }

    m_running.insert(screenIndex, running);
    return true;
}

void RendererController::stop(int screenIndex) {
    RunningProcess* running = m_running.take(screenIndex);
    if (!running) return;

    running->stopping = true;
    sendCommand(running, QJsonObject{{"cmd", "quit"}});
    running->process->closeWriteChannel();

    // Escalating shutdown: "quit" first, then terminate() at 1.5 s and
    // kill() at 3 s. Both timers no-op once the process has exited.
    QTimer::singleShot(1500, running->process, [process = running->process] {
        if (process->state() != QProcess::NotRunning) process->terminate();
    });
    QTimer::singleShot(3000, running->process, [process = running->process] {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
}

void RendererController::stopAll() {
    const QVector<int> screens = activeScreens();
    for (int screen : screens) stop(screen);
}

QVector<int> RendererController::activeScreens() const {
    QVector<int> screens;
    for (auto it = m_running.constBegin(); it != m_running.constEnd(); ++it) screens.push_back(it.key());
    std::sort(screens.begin(), screens.end());
    return screens;
}

QString RendererController::fillModeKey(FillMode mode) {
    switch (mode) {
    case FillMode::Cover: return QStringLiteral("cover");
    case FillMode::Contain: return QStringLiteral("contain");
    case FillMode::Stretch: return QStringLiteral("stretch");
    }
    return QStringLiteral("cover");
}

QString RendererController::stableOutputId(const QScreen* screen) {
    if (screen == nullptr) return {};
    const QString manufacturer = screen->manufacturer().trimmed();
    const QString model = screen->model().trimmed();
    const QString serial = screen->serialNumber().trimmed();
    const QString connector = screen->name().trimmed();
    const QString identity = serial.isEmpty()
                                 ? QStringList {manufacturer, model, connector}.join('|')
                                 : QStringList {manufacturer, model, serial}.join('|');
    return QStringLiteral("kde:") + identity;
}

void RendererController::setVolume(double volume, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        sendCommand(running, QJsonObject{{"cmd", "volume"}, {"value", volume}});
    });
}

void RendererController::setMuted(bool muted, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        sendCommand(running, QJsonObject{{"cmd", "muted"}, {"value", muted}});
    });
}

void RendererController::pause(int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        sendCommand(running, QJsonObject{{"cmd", "pause"}});
    });
}

void RendererController::resume(int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        sendCommand(running, QJsonObject{{"cmd", "resume"}});
    });
}

void RendererController::setFps(int fps, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        sendCommand(running, QJsonObject{{"cmd", "fps"}, {"value", fps}});
    });
}

void RendererController::setSpeed(double speed, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        sendCommand(running, QJsonObject{{"cmd", "speed"}, {"value", speed}});
    });
}

void RendererController::setFillMode(FillMode mode, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
        sendCommand(running, QJsonObject{{"cmd", "fillmode"}, {"value", fillModeKey(mode)}});
    });
}

void RendererController::setProperty(const QString& key, const ProjectProperty& property, int screenIndex) {
    forEachTarget(screenIndex, [&](RunningProcess* running) {
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
    return firstExecutable({
        siblingBinary("SceneWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/SceneRenderer/build/linux-clang-release/Tools/SceneWallpaper/SceneWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/SceneRenderer/build/release/Tools/SceneWallpaper/SceneWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/SceneRenderer/cmake-build-debug-clang-21/Tools/SceneWallpaper/SceneWallpaper"),
    });
}

QString RendererController::webWallpaperBinary() const {
    return firstExecutable({siblingBinary("WebWallpaper")});
}

QString RendererController::videoWallpaperBinary() const {
    return firstExecutable({
        siblingBinary("VideoWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/VideoRenderer/build/linux-clang-release/Tools/VideoWallpaper/VideoWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/VideoRenderer/build/debug/Tools/VideoWallpaper/VideoWallpaper"),
        QDir::cleanPath(Paths::repoRoot() + "/VideoRenderer/cmake-build-debug-clang-21/Tools/VideoWallpaper/VideoWallpaper"),
    });
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

    const QString path = QDir::temp().filePath(QStringLiteral("mirageqt_props_%1.json")
                                                   .arg(qHash(wallpaper.id())));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
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
    running->process->write(line);
}

void RendererController::forEachTarget(int screenIndex, const std::function<void(RunningProcess*)>& body) {
    if (screenIndex >= 0) {
        if (RunningProcess* running = m_running.value(screenIndex)) body(running);
        return;
    }
    for (RunningProcess* running : m_running) body(running);
}

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
        if (object.value(QStringLiteral("event")).toString() == QStringLiteral("video-did-end")) {
            emit videoDidEnd(running->screenIndex);
        }
    }
}

} // namespace Mirage
