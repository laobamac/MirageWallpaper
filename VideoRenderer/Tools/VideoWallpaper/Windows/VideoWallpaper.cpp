#include "ControlChannel.h"
#include "DesktopHost.h"
#include "VideoManifest.h"
#include "VideoRendererEngine.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QMetaObject>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTimer>
#include <QtGui/qscreen_platform.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
void emitEvent(const QJsonObject& event) {
    const QByteArray bytes = QJsonDocument(event).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
HMONITOR nativeMonitor(QScreen* screen) {
    const auto* native = screen ? screen->nativeInterface<QNativeInterface::QWindowsScreen>() : nullptr;
    return native ? native->handle() : nullptr;
}
QString deviceName(QScreen* screen) {
    return QString::fromStdWString(Mirage::Windows::screenDevice(nativeMonitor(screen)));
}
bool monitorBounds(QScreen* screen, RECT& bounds) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const HMONITOR monitor = nativeMonitor(screen);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
    bounds = info.rcMonitor;
    return true;
}
}

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName(QStringLiteral("VideoWallpaper"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Windows libmpv GPU desktop video host"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("wallpaper"), QStringLiteral("Video wallpaper directory or video file"));
    parser.addOptions({
        {{QStringLiteral("screen")}, QStringLiteral("Zero-based screen index"), QStringLiteral("N"), QStringLiteral("0")},
        {{QStringLiteral("output-id"), QStringLiteral("display-output-id")}, QStringLiteral("Windows display device name"), QStringLiteral("ID")},
        {{QStringLiteral("volume")}, QStringLiteral("Audio volume 0..1"), QStringLiteral("V"), QStringLiteral("1")},
        {{QStringLiteral("speed")}, QStringLiteral("Initial playback speed"), QStringLiteral("S"), QStringLiteral("1")},
        {{QStringLiteral("muted")}, QStringLiteral("Start muted")},
        {{QStringLiteral("fill")}, QStringLiteral("cover | contain | stretch"), QStringLiteral("MODE"), QStringLiteral("cover")},
        {{QStringLiteral("position-x")}, QStringLiteral("Horizontal crop anchor 0..1"), QStringLiteral("X"), QStringLiteral("0.5")},
        {{QStringLiteral("position-y")}, QStringLiteral("Vertical crop anchor 0..1"), QStringLiteral("Y"), QStringLiteral("0.5")},
        {{QStringLiteral("fps")}, QStringLiteral("Maximum presentation rate"), QStringLiteral("FPS"), QStringLiteral("60")},
        {{QStringLiteral("control-stdin")}, QStringLiteral("Accept newline-delimited JSON on stdin")},
        {{QStringLiteral("load-from-memory")}, QStringLiteral("Keep video bytes in memory")},
        {{QStringLiteral("run-seconds")}, QStringLiteral("Exit after N seconds (test helper)"), QStringLiteral("N"), QStringLiteral("0")},
        {{QStringLiteral("windowed")}, QStringLiteral("Use a visible window instead of the desktop (test helper)")},
    });
    if (!parser.parse(app.arguments())) { std::fprintf(stderr, "%s\n", parser.errorText().toUtf8().constData()); return 1; }
    if (parser.isSet(QStringLiteral("help"))) { std::fputs(parser.helpText().toUtf8().constData(), stdout); return 0; }
    if (parser.positionalArguments().size() != 1) { std::fputs(parser.helpText().toUtf8().constData(), stderr); return 1; }
    const auto numeric = [&parser](const QString& name, double& result) {
        bool ok = false;
        result = parser.value(name).toDouble(&ok);
        return ok && std::isfinite(result);
    };
    double screenIndex, volume, x, y, fps, seconds, speed;
    if (!numeric(QStringLiteral("screen"), screenIndex) || !numeric(QStringLiteral("volume"), volume)
        || !numeric(QStringLiteral("position-x"), x) || !numeric(QStringLiteral("position-y"), y)
        || !numeric(QStringLiteral("fps"), fps) || !numeric(QStringLiteral("run-seconds"), seconds)
        || !numeric(QStringLiteral("speed"), speed)
        || fps <= 0 || seconds < 0 || seconds > 86400 || speed <= 0 || speed > 100
        || std::floor(screenIndex) != screenIndex
        || screenIndex < 0 || (!parser.isSet(QStringLiteral("output-id")) && screenIndex >= app.screens().size())) {
        std::fprintf(stderr, "VideoWallpaper: invalid numeric option or screen index\n"); return 1;
    }
    QScreen* screen = nullptr;
    if (parser.isSet(QStringLiteral("output-id"))) {
        const QString id = parser.value(QStringLiteral("output-id"));
        for (QScreen* candidate : app.screens()) {
            if (candidate->name().compare(id, Qt::CaseInsensitive) == 0
                || deviceName(candidate).compare(id, Qt::CaseInsensitive) == 0) { screen = candidate; break; }
        }
        if (!screen) { std::fprintf(stderr, "VideoWallpaper: unknown output-id\n"); return 1; }
    } else screen = app.screens().at(static_cast<int>(screenIndex));
    VRVideoEngineConfig config = VRDefaultVideoEngineConfig();
    if (!VRParseVideoFillMode(parser.value(QStringLiteral("fill")).toStdString(), config.fillMode)) {
        std::fprintf(stderr, "VideoWallpaper: invalid fill mode\n"); return 1;
    }
    config.initialVolume = VRClampVideoVolume(static_cast<float>(volume));
    config.initialSpeed = std::clamp(speed, 0.05, 100.0);
    config.positionX = std::clamp(x, 0.0, 1.0);
    config.positionY = std::clamp(y, 0.0, 1.0);
    config.muted = parser.isSet(QStringLiteral("muted"));
    config.loadFromMemory = parser.isSet(QStringLiteral("load-from-memory"));
    std::shared_ptr<VRVideoManifest> manifest;
    try { manifest = VRVideoManifest::loadFromPath(parser.positionalArguments().front().toStdString()); }
    catch (const VideoRendererManifestError& error) { std::fprintf(stderr, "VideoWallpaper: %s\n", error.what()); return 2; }
    VRVideoRendererEngine renderer(config, {
        [&app](const QString& error) {
            std::fprintf(stderr, "VideoWallpaper: %s\n", error.toUtf8().constData());
            QMetaObject::invokeMethod(&app, [] { QCoreApplication::exit(3); }, Qt::QueuedConnection);
        }, emitEvent});
    renderer.setScreen(screen);
    renderer.setGeometry(screen->geometry());
    renderer.setFps(fps);
    QString error;
    if (!renderer.openWallpaper(*manifest, &error)) { std::fprintf(stderr, "VideoWallpaper: %s\n", error.toUtf8().constData()); return 3; }
    renderer.show();
    const bool windowed = parser.isSet(QStringLiteral("windowed"));
    const QString outputId = deviceName(screen);
    if (outputId.isEmpty()) {
        std::fprintf(stderr, "VideoWallpaper: cannot resolve native Windows monitor\n"); return 3;
    }
    std::fprintf(stderr, "VideoWallpaper: output %s maps to %s\n", screen->name().toUtf8().constData(), outputId.toUtf8().constData());
    DWORD hostError = ERROR_SUCCESS;
    auto synchronizeOutput = [&]() -> bool {
        QScreen* target = nullptr;
        for (QScreen* candidate : app.screens()) {
            if (deviceName(candidate).compare(outputId, Qt::CaseInsensitive) == 0) { target = candidate; break; }
        }
        if (!target) return false;
        RECT bounds{};
        if (!windowed && !monitorBounds(target, bounds)) return false;
        if (!IsWindow(reinterpret_cast<HWND>(renderer.winId()))) {
            // QOpenGLWindow 的 GL context 独立于平台 surface，保留 libmpv 与播放位置，只重建 HWND。
            renderer.destroy();
            renderer.create();
        }
        if (renderer.screen() != target) renderer.setScreen(target);
        renderer.setGeometry(target->geometry());
        if (!windowed && !Mirage::Windows::attachToDesktop(reinterpret_cast<HWND>(renderer.winId()), bounds, &hostError)) return false;
        renderer.show();
        renderer.update();
        return true;
    };
    if (!synchronizeOutput()) {
        std::fprintf(stderr, "VideoWallpaper: cannot attach desktop host (%lu)\n", hostError); return 3;
    }
    auto watchScreen = [&](QScreen* target) {
        QObject::connect(target, &QScreen::geometryChanged, &app, [&](const QRect&) { synchronizeOutput(); });
        QObject::connect(target, &QScreen::physicalDotsPerInchChanged, &app, [&](qreal) { synchronizeOutput(); });
    };
    for (QScreen* target : app.screens()) watchScreen(target);
    QObject::connect(&app, &QGuiApplication::screenAdded, &app, [&](QScreen* target) { watchScreen(target); synchronizeOutput(); });
    QObject::connect(&app, &QGuiApplication::screenRemoved, &app, [&](QScreen*) { synchronizeOutput(); });
    QObject::connect(&renderer, &QWindow::screenChanged, &app, [&](QScreen*) {
        QTimer::singleShot(0, &app, [&] { synchronizeOutput(); });
    });
    QTimer desktopMonitor;
    if (!windowed) {
        QObject::connect(&desktopMonitor, &QTimer::timeout, &app, [&] {
            const HWND window = reinterpret_cast<HWND>(renderer.winId());
            const HWND worker = Mirage::Windows::desktopWorker();
            if (worker && (!IsWindow(window) || GetParent(window) != worker)) synchronizeOutput();
        });
        desktopMonitor.start(1000);
    }
    VRControlChannel control([&renderer](const QJsonObject& command) { renderer.handleCommand(command); }, [&app] { app.quit(); });
    if (parser.isSet(QStringLiteral("control-stdin")) && !control.start(&error)) {
        std::fprintf(stderr, "VideoWallpaper: %s\n", error.toUtf8().constData()); return 1;
    }
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] { control.stop(); renderer.stop(); });
    if (seconds > 0) QTimer::singleShot(static_cast<int>(seconds * 1000), &app, &QCoreApplication::quit);
    return app.exec();
}
