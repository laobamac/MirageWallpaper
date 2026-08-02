#include "ControlChannel.h"
#include "VideoManifest.h"
#include "VideoRendererEngine.h"
#include "X11DesktopWindow.h"
#if defined(VIDEORENDERER_MIRAGE_DISPLAY)
#include "ProtocolVideoRenderer.h"
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

struct WallpaperArgs {
    const char* workshop = nullptr;
    int screen = 0;
    float volume = 1.0f;
    bool muted = false;
    int runSeconds = 0;
    VRVideoFillMode fillMode = VRVideoFillModeCover;
    bool controlStdin = false;
    bool loadFromMemory = false;
    const char* displayOutputId = nullptr;
    const char* displaySocket = nullptr;
};

void printUsage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s <wallpaper-dir> [options]\n\n"
                 "Options:\n"
                 "  --screen N             screen index to cover (default 0 = main)\n"
                 "  --volume 0..1          audio volume (default 1.0)\n"
                 "  --muted                start muted\n"
                 "  --fill MODE            cover | contain | stretch (default cover)\n"
                 "  --control-stdin        accept live JSON control commands on stdin\n"
                 "  --load-from-memory     keep the video bytes in memory\n"
                 "  --display-output-id ID mirage-display stable output id (protocol mode)\n"
                 "  --display-socket PATH  mirage-display broker socket (protocol mode)\n"
                 "  --run-seconds N        exit after N seconds (test helper)\n"
                 "  -h, --help             show this help\n",
                 argv0);
}

const char* takeValue(int& index, int argc, char** argv, const char* option) {
    if (index + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", option);
        return nullptr;
    }
    return argv[++index];
}

bool parseArgs(int argc, char** argv, WallpaperArgs& out) {
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (std::strcmp(argument, "-h") == 0 || std::strcmp(argument, "--help") == 0) {
            printUsage(argv[0]);
            std::exit(0);
        }
        if (std::strcmp(argument, "--screen") == 0) {
            const char* value = takeValue(index, argc, argv, argument);
            if (!value) return false;
            out.screen = std::atoi(value);
        } else if (std::strcmp(argument, "--volume") == 0) {
            const char* value = takeValue(index, argc, argv, argument);
            if (!value) return false;
            out.volume = std::strtof(value, nullptr);
        } else if (std::strcmp(argument, "--muted") == 0) {
            out.muted = true;
        } else if (std::strcmp(argument, "--fill") == 0) {
            const char* value = takeValue(index, argc, argv, argument);
            if (!value || !VRParseVideoFillMode(value, out.fillMode)) return false;
        } else if (std::strcmp(argument, "--control-stdin") == 0) {
            out.controlStdin = true;
        } else if (std::strcmp(argument, "--load-from-memory") == 0) {
            out.loadFromMemory = true;
        } else if (std::strcmp(argument, "--display-output-id") == 0) {
            const char* value = takeValue(index, argc, argv, argument);
            if (!value || *value == '\0') return false;
            out.displayOutputId = value;
        } else if (std::strcmp(argument, "--display-socket") == 0) {
            const char* value = takeValue(index, argc, argv, argument);
            if (!value || *value == '\0') return false;
            out.displaySocket = value;
        } else if (std::strcmp(argument, "--run-seconds") == 0) {
            const char* value = takeValue(index, argc, argv, argument);
            if (!value) return false;
            out.runSeconds = std::atoi(value);
        } else if (argument[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", argument);
            return false;
        } else if (!out.workshop) {
            out.workshop = argument;
        } else {
            std::fprintf(stderr, "unexpected positional argument: %s\n", argument);
            return false;
        }
    }

    if (!out.workshop) {
        printUsage(argv[0]);
        return false;
    }
    if (out.screen < 0) out.screen = 0;
    out.volume = VRClampVideoVolume(out.volume);
    if (out.runSeconds < 0) out.runSeconds = 0;
    return true;
}

std::mutex& stdoutMutex() {
    static std::mutex mutex;
    return mutex;
}

void emitStdoutEvent(const char* eventName) {
    std::lock_guard lock(stdoutMutex());
    std::fprintf(stdout, "{\"event\":\"%s\"}\n", eventName);
    std::fflush(stdout);
}

#if defined(VIDEORENDERER_MIRAGE_DISPLAY)
int runProtocol(QCoreApplication& app, const VRVideoManifest& manifest,
                const WallpaperArgs& args) {
    VRProtocolVideoRenderer::Config config;
    config.socketPath = QString::fromLocal8Bit(args.displaySocket);
    config.outputId = QString::fromLocal8Bit(args.displayOutputId);
    config.videoPath = manifest.videoUrl().toLocalFile();
    config.fillMode = args.fillMode;
    config.volume = args.volume;
    config.muted = args.muted;
    config.loadFromMemory = args.loadFromMemory;
    config.errorCallback = [&app](const QString& message) {
        std::fprintf(stderr, "VideoWallpaper: %s\n", message.toLocal8Bit().constData());
        QMetaObject::invokeMethod(&app,
                                  [] { QCoreApplication::exit(3); },
                                  Qt::QueuedConnection);
    };
    config.videoDidEndCallback = [] { emitStdoutEvent("video-did-end"); };
    config.firstFrameCallback = [] { emitStdoutEvent("first-frame-presented"); };

    VRProtocolVideoRenderer renderer(config);
    QString startError;
    if (!renderer.start(&startError)) {
        std::fprintf(stderr, "VideoWallpaper: %s\n",
                     startError.toLocal8Bit().constData());
        return 3;
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
                     [&renderer] { renderer.stop(); });

    VRControlChannel control(
        [&renderer](const QJsonObject& command) {
            const QString name = command.value(QStringLiteral("cmd")).toString();
            const QJsonValue value = command.value(QStringLiteral("value"));
            if (name == QStringLiteral("pause")) {
                renderer.pause();
            } else if (name == QStringLiteral("resume") || name == QStringLiteral("play")) {
                renderer.play();
            } else if (name == QStringLiteral("volume") && value.isDouble()) {
                renderer.setVolume(static_cast<float>(value.toDouble()));
            } else if (name == QStringLiteral("muted") && value.isBool()) {
                renderer.setMuted(value.toBool());
            } else if (name == QStringLiteral("fillmode") && value.isString()) {
                VRVideoFillMode mode;
                if (VRParseVideoFillMode(value.toString().toStdString(), mode)) {
                    renderer.setFillMode(mode);
                }
            }
        },
        [&app] { app.quit(); },
        &app);
    if (args.controlStdin) {
        QString controlError;
        if (!control.start(&controlError)) {
            std::fprintf(stderr, "VideoWallpaper: %s\n",
                         controlError.toLocal8Bit().constData());
            return 1;
        }
    }

    if (args.runSeconds > 0) {
        QTimer::singleShot(args.runSeconds * 1000, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
#endif

int runX11(QApplication& app, const VRVideoManifest& manifest,
           const WallpaperArgs& args) {
    QWidget window;
    window.setWindowTitle(manifest.title());
    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    VRVideoEngineConfig config = VRVideoRendererEngine::defaultConfig();
    config.fillMode = args.fillMode;
    config.initialVolume = args.volume;
    config.muted = args.muted;
    config.autoplay = true;
    config.loadFromMemory = args.loadFromMemory;
    VRVideoRendererEngine engine(config, &window);
    layout->addWidget(&engine);

    QString openError;
    if (!engine.openWallpaper(manifest, &openError)) {
        std::fprintf(stderr, "VideoWallpaper: %s\n", openError.toLocal8Bit().constData());
        return 3;
    }
    QObject::connect(&engine, &VRVideoRendererEngine::playbackError, &app,
                     [&app](const QString& message) {
                         std::fprintf(stderr, "VideoWallpaper: %s\n",
                                      message.toLocal8Bit().constData());
                         app.exit(3);
                     });
    QObject::connect(&engine, &VRVideoRendererEngine::videoDidEnd, &app,
                     [] { emitStdoutEvent("video-did-end"); });

    QString windowError;
    if (!VRConfigureX11DesktopWindow(&window, args.screen, &windowError)) {
        std::fprintf(stderr, "VideoWallpaper: %s\n", windowError.toLocal8Bit().constData());
        return 1;
    }
    window.show();

    VRControlChannel control(
        [&engine](const QJsonObject& command) {
            const QString name = command.value(QStringLiteral("cmd")).toString();
            const QJsonValue value = command.value(QStringLiteral("value"));
            if (name == QStringLiteral("pause")) {
                engine.pause();
            } else if (name == QStringLiteral("resume") || name == QStringLiteral("play")) {
                engine.play();
            } else if (name == QStringLiteral("volume") && value.isDouble()) {
                engine.setVolume(static_cast<float>(value.toDouble()));
            } else if (name == QStringLiteral("muted") && value.isBool()) {
                engine.setMuted(value.toBool());
            } else if (name == QStringLiteral("fillmode") && value.isString()) {
                VRVideoFillMode mode;
                if (VRParseVideoFillMode(value.toString().toStdString(), mode)) engine.setFillMode(mode);
            }
        },
        [&app] { app.quit(); },
        &app);
    if (args.controlStdin) {
        QString controlError;
        if (!control.start(&controlError)) {
            std::fprintf(stderr, "VideoWallpaper: %s\n",
                         controlError.toLocal8Bit().constData());
            return 1;
        }
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &engine, &VRVideoRendererEngine::pause);
    if (args.runSeconds > 0) {
        QTimer::singleShot(args.runSeconds * 1000, &app, &QCoreApplication::quit);
    }
    return app.exec();
}

} // namespace

int main(int argc, char** argv) {
    WallpaperArgs args;
    if (!parseArgs(argc, argv, args)) return 1;

    const bool protocolMode = args.displayOutputId != nullptr;
#if !defined(VIDEORENDERER_MIRAGE_DISPLAY)
    if (protocolMode) {
        std::fprintf(stderr,
                     "VideoWallpaper: this build has no mirage-display support\n");
        return 1;
    }
#endif
    if (protocolMode && (args.displaySocket == nullptr || *args.displaySocket == '\0')) {
        std::fprintf(stderr, "--display-socket is required with --display-output-id\n");
        return 1;
    }

    if (!protocolMode) {
        if (qEnvironmentVariable("XDG_SESSION_TYPE").compare(
                QStringLiteral("wayland"), Qt::CaseInsensitive) == 0) {
            std::fprintf(stderr,
                         "VideoWallpaper: native Wayland sessions are not supported; use X11.\n");
            return 1;
        }
        if (!qEnvironmentVariableIsSet("QT_MEDIA_BACKEND")) {
            qputenv("QT_MEDIA_BACKEND", "ffmpeg");
        }
    }

#if defined(VIDEORENDERER_MIRAGE_DISPLAY)
    if (protocolMode) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("VideoWallpaper"));

        QString manifestError;
        const auto manifest = VRVideoManifest::loadFromDirectory(
            QString::fromLocal8Bit(args.workshop), &manifestError);
        if (!manifest) {
            std::fprintf(stderr, "VideoWallpaper: %s\n",
                         manifestError.toLocal8Bit().constData());
            return 2;
        }
        return runProtocol(app, *manifest, args);
    }
#endif

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("VideoWallpaper"));

    QString manifestError;
    const auto manifest = VRVideoManifest::loadFromDirectory(
        QString::fromLocal8Bit(args.workshop), &manifestError);
    if (!manifest) {
        std::fprintf(stderr, "VideoWallpaper: %s\n",
                     manifestError.toLocal8Bit().constData());
        return 2;
    }
    return runX11(app, *manifest, args);
}
