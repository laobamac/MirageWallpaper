#include "Services/RendererController.h"

#include "Services/LinuxSystemIntegration.h"
#include "Services/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

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
    case PropertyKind::Color:
    case PropertyKind::SceneTexture:
    case PropertyKind::File:
    case PropertyKind::Combo:
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

    if (wallpaper.kind() == WallpaperKind::Web || wallpaper.kind() == WallpaperKind::Video) {
        if (error) {
            *error = wallpaper.kind() == WallpaperKind::Web
                ? QStringLiteral("Linux WebWallpaperQt 渲染器尚未实现，当前仅保留占位调用。")
                : QStringLiteral("Linux VideoWallpaperQt 渲染器尚未实现，当前仅保留占位调用。");
        }
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

    QStringList args;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    switch (wallpaper.kind()) {
    case WallpaperKind::Scene: {
        args << Paths::assetsDir()
             << wallpaper.resolvedEntryPath()
             << "--fps" << QString::number(options.fps)
             << "--screen" << QString::number(screenIndex)
             << "--control-stdin";
        if (options.muted) args << "--muted";
        const QString propsFile = writeUserPropertiesFile(options.userProperties, wallpaper);
        if (!propsFile.isEmpty()) {
            args << "--user-properties" << propsFile;
            running->tempFiles << propsFile;
        }
        break;
    }
    case WallpaperKind::Web:
    case WallpaperKind::Video:
    case WallpaperKind::Unsupported:
        delete running;
        process->deleteLater();
        return false;
    }

    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        const QString text = QString::fromUtf8(process->readAllStandardError()).trimmed();
        if (!text.isEmpty()) emit rendererMessage(text);
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

bool RendererController::isRendering(int screenIndex) const {
    const RunningProcess* running = m_running.value(screenIndex);
    return running && running->process->state() != QProcess::NotRunning;
}

QVector<int> RendererController::activeScreens() const {
    QVector<int> screens;
    for (auto it = m_running.constBegin(); it != m_running.constEnd(); ++it) screens.push_back(it.key());
    std::sort(screens.begin(), screens.end());
    return screens;
}

QSet<qint64> RendererController::processIds() const {
    QSet<qint64> ids;
    for (RunningProcess* running : m_running) {
        if (running->process->state() != QProcess::NotRunning) ids.insert(running->process->processId());
    }
    return ids;
}

QString RendererController::fillModeKey(FillMode mode) {
    switch (mode) {
    case FillMode::Cover: return QStringLiteral("cover");
    case FillMode::Contain: return QStringLiteral("contain");
    case FillMode::Stretch: return QStringLiteral("stretch");
    }
    return QStringLiteral("cover");
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
    return firstExecutable({siblingBinary("WebWallpaperQt")});
}

QString RendererController::videoWallpaperBinary() const {
    return firstExecutable({siblingBinary("VideoWallpaperQt")});
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

} // namespace Mirage
