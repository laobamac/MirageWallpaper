#include "ControlChannel.h"
#include "VideoManifest.h"
#include "ProtocolVideoRenderer.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

struct WallpaperArgs {
    const char* workshop = nullptr;
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
        if (std::strcmp(argument, "--volume") == 0) {
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

} // namespace

int main(int argc, char** argv) {
    WallpaperArgs args;
    if (!parseArgs(argc, argv, args)) return 1;

    // Linux displays through the mirage-display protocol only; there is no
    // native X11 window host anymore.
    if (args.displayOutputId == nullptr || args.displaySocket == nullptr ||
        *args.displaySocket == '\0') {
        std::fprintf(stderr,
                     "VideoWallpaper: --display-output-id and --display-socket are required\n");
        return 1;
    }

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
