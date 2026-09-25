#include "ControlChannel.h"
#include "WallpaperManifest.h"
#include "WebRendererEngine.h"
#include "ProtocolWebRenderer.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QJsonArray>
#include <QMetaObject>
#include <QJsonObject>
#include <QTimer>
#include <QWebEngineView>
#include <QWebEngineUrlScheme>

#include <cstdio>
#include <array>
#include <memory>
#include <utility>

int main(int argc, char** argv) {
    // Register before QApplication creates the Chromium profile; the renderer
    // uses this controlled origin for overlay and in-memory wallpaper assets.
    // HostAndPort schemes require a fixed default port; without it Qt rejects
    // registration and every wallpaper navigation resolves to a blank page.
    QWebEngineUrlScheme wallpaperScheme(QByteArrayLiteral("mirage-wallpaper"));
    wallpaperScheme.setSyntax(QWebEngineUrlScheme::Syntax::HostAndPort);
    wallpaperScheme.setDefaultPort(443);
    wallpaperScheme.setFlags(QWebEngineUrlScheme::SecureScheme
                             | QWebEngineUrlScheme::LocalScheme
                             | QWebEngineUrlScheme::LocalAccessAllowed);
    QWebEngineUrlScheme::registerScheme(wallpaperScheme);
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("display-output-id"), QStringLiteral("mirage-display output id"), QStringLiteral("id")});
    parser.addOption({QStringLiteral("display-socket"), QStringLiteral("mirage-display broker socket"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("fps"), QStringLiteral("target frame rate"), QStringLiteral("fps"), QStringLiteral("60")});
    parser.addOption({QStringLiteral("volume"), QStringLiteral("master volume"), QStringLiteral("volume"), QStringLiteral("1.0")});
    parser.addOption({QStringLiteral("muted"), QStringLiteral("start muted")});
    parser.addOption({QStringLiteral("no-spectrum"), QStringLiteral("disable system audio spectrum capture")});
    parser.addOption({QStringLiteral("external-spectrum"),
                      QStringLiteral("receive 128-bin spectrum frames from the control channel")});
    parser.addOption({QStringLiteral("asset-overlay"),
                      QStringLiteral("preset asset directory searched before the wallpaper directory"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("load-from-memory"),
                      QStringLiteral("cache wallpaper resources for this process")});
    parser.addOption({QStringLiteral("control-stdin"), QStringLiteral("accept JSON commands")});
    parser.addPositionalArgument(QStringLiteral("wallpaper-dir"), QStringLiteral("Web wallpaper directory"));
    parser.process(app);
    if (parser.positionalArguments().size() != 1) return 1;
    if (!parser.isSet(QStringLiteral("display-output-id")) || !parser.isSet(QStringLiteral("display-socket"))) {
        std::fprintf(stderr, "WebWallpaper: --display-output-id and --display-socket are required\n");
        return 1;
    }

    QString error;
    const WRManifest manifest = WRManifest::loadFromDirectory(parser.positionalArguments().constFirst(), &error);
    if (manifest.workshopDirectory().isEmpty()) {
        std::fprintf(stderr, "WebWallpaper: %s\n", qPrintable(error));
        return 2;
    }
    WebRendererEngine::Config config;
    config.frameRate = parser.value(QStringLiteral("fps")).toInt();
    config.initialVolume = parser.value(QStringLiteral("volume")).toFloat();
    // These settings are part of the renderer contract even though loading is
    // performed by WebRendererEngine, not by the display-producer process.
    config.assetOverlayDirectories = parser.values(QStringLiteral("asset-overlay"));
    config.loadFromMemory = parser.isSet(QStringLiteral("load-from-memory"));
    // External frames are already analysed by the parent process. Keeping the
    // engine's request loop active while disabling local capture prevents two
    // independent monitor streams and preserves the macOS control protocol.
    const bool externalSpectrum = parser.isSet(QStringLiteral("external-spectrum"));
    config.enableAudioSpectrum = !parser.isSet(QStringLiteral("no-spectrum")) && !externalSpectrum;
    std::unique_ptr<WebRendererEngine> engine;
    ProtocolWebRenderer::Config protocolConfig {
        .socketPath = parser.value(QStringLiteral("display-socket")),
        .outputId = parser.value(QStringLiteral("display-output-id")),
        .pointerEnter = [&engine](float x, float y) {
            const bool queued = QMetaObject::invokeMethod(
                engine.get(), [&, x, y] { engine->sendPointerEnter(x, y); },
                Qt::QueuedConnection);
            if (!queued) std::fprintf(stderr, "WebWallpaper: cannot queue pointer enter\n");
        },
        .pointerLeave = [&engine] {
            const bool queued = QMetaObject::invokeMethod(
                engine.get(), [&] { engine->sendPointerLeave(); }, Qt::QueuedConnection);
            if (!queued) std::fprintf(stderr, "WebWallpaper: cannot queue pointer leave\n");
        },
        .pointerMotion = [&engine](float x, float y) {
            const bool queued = QMetaObject::invokeMethod(
                engine.get(), [&, x, y] {
                    engine->sendPointerMotion(x, y);
                }, Qt::QueuedConnection);
            if (!queued) std::fprintf(stderr, "WebWallpaper: cannot queue pointer motion\n");
        },
        .pointerButton = [&engine](float x, float y, Qt::MouseButton button, bool pressed) {
            const bool queued = QMetaObject::invokeMethod(
                engine.get(), [&, x, y, button, pressed] {
                    engine->sendPointerButton(x, y, button, pressed);
                }, Qt::QueuedConnection);
            if (!queued) std::fprintf(stderr, "WebWallpaper: cannot queue pointer button\n");
        },
        .pointerAxis = [&engine](float x, float y, float dx, float dy, bool pixelBased) {
            const bool queued = QMetaObject::invokeMethod(
                engine.get(), [&, x, y, dx, dy, pixelBased] {
                    engine->sendPointerAxis(x, y, dx, dy, pixelBased);
                }, Qt::QueuedConnection);
            if (!queued) std::fprintf(stderr, "WebWallpaper: cannot queue pointer axis\n");
        },
        .outputSizeChanged = [&engine](uint32_t width, uint32_t height) {
            const bool queued = QMetaObject::invokeMethod(
                engine.get(), [&, width, height] {
                    engine->view()->resize(static_cast<int>(width),
                                           static_cast<int>(height));
                }, Qt::QueuedConnection);
            if (!queued) std::fprintf(stderr, "WebWallpaper: cannot queue output resize\n");
        },
    };
    ProtocolWebRenderer protocol(std::move(protocolConfig), &app);
    engine = std::make_unique<WebRendererEngine>(config);
    engine->view()->setAttribute(Qt::WA_DontShowOnScreen, true);
    engine->view()->resize(1920, 1080);
    QObject::connect(engine.get(), &WebRendererEngine::frameReady, &protocol,
                     &ProtocolWebRenderer::submitFrame, Qt::QueuedConnection);
    QString protocolError;
    if (!protocol.start(&protocolError)) {
        std::fprintf(stderr, "WebWallpaper: %s\n", qPrintable(protocolError));
        return 3;
    }
    // Initialize the DMA-BUF producer before activating WebEngine so Qt and the
    // producer create their Vulkan devices in a fixed order. show() activates
    // the off-screen backing store without mapping an extra desktop window.
    engine->view()->show();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &protocol, &ProtocolWebRenderer::stop);
    engine->openWallpaper(manifest);
    engine->setMuted(parser.isSet(QStringLiteral("muted")));
    engine->startAudioSpectrum();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, engine.get(),
                     [&engine] { engine->stopAudioSpectrum(); });
    QObject::connect(engine.get(), &WebRendererEngine::audioSpectrumDemandChanged,
                     [](bool needed) {
        std::fprintf(stdout, "{\"event\":\"audio-demand\",\"needed\":%s}\n",
                     needed ? "true" : "false");
        std::fflush(stdout);
    });
    QObject::connect(engine.get(), &WebRendererEngine::contentReady, [] {
        std::fprintf(stdout, "{\"event\":\"content-ready\"}\n");
        std::fflush(stdout);
    });
    QObject::connect(engine.get(), &WebRendererEngine::failed, [](const QString& message) {
        std::fprintf(stderr, "WebWallpaper: %s\n", qPrintable(message));
    });
    ControlChannel channel([&engine](const QJsonObject& command) {
        const QString name = command.value(QStringLiteral("cmd")).toString();
        if (name == QStringLiteral("pause")) engine->setPaused(true);
        else if (name == QStringLiteral("resume") || name == QStringLiteral("play")) engine->setPaused(false);
        else if (name == QStringLiteral("muted")) {
            const QJsonValue value = command.value(QStringLiteral("value"));
            if (!value.isBool()) {
                std::fprintf(stderr, "WebWallpaper: muted requires a boolean value\n");
                return;
            }
            engine->setMuted(value.toBool());
        } else if (name == QStringLiteral("volume")) {
            const QJsonValue value = command.value(QStringLiteral("value"));
            if (!value.isDouble()) {
                std::fprintf(stderr, "WebWallpaper: volume requires a numeric value\n");
                return;
            }
            engine->setVolume(static_cast<float>(value.toDouble()));
        } else if (name == QStringLiteral("fps")) {
            const QJsonValue value = command.value(QStringLiteral("value"));
            if (!value.isDouble()) {
                std::fprintf(stderr, "WebWallpaper: fps requires a numeric value\n");
                return;
            }
            engine->setFrameRate(value.toInt());
        } else if (name == QStringLiteral("setProperty")) {
            const QJsonValue key = command.value(QStringLiteral("key"));
            const QJsonValue value = command.value(QStringLiteral("value"));
            if (!key.isString() || key.toString().isEmpty() || value.isUndefined()) {
                std::fprintf(stderr, "WebWallpaper: setProperty requires key and value\n");
                return;
            }
            // Wallpaper Engine listeners require every live value to use the
            // same {key: {value: ...}} shape as the initial property snapshot.
            engine->applyUserProperty(key.toString(), QJsonObject{{QStringLiteral("value"), value}});
        } else if (name == QStringLiteral("setProperties")) {
            const QJsonValue values = command.value(QStringLiteral("values"));
            if (!values.isObject()) {
                std::fprintf(stderr, "WebWallpaper: setProperties requires a values object\n");
                return;
            }
            engine->applyUserProperties(values.toObject());
        } else if (name == QStringLiteral("playbackState")) {
            const QJsonValue volume = command.value(QStringLiteral("volume"));
            const QJsonValue muted = command.value(QStringLiteral("muted"));
            const QJsonValue state = command.value(QStringLiteral("state"));
            if (!volume.isDouble() || !muted.isBool() || !state.isString()) {
                std::fprintf(stderr, "WebWallpaper: playbackState requires volume, muted, and state\n");
                return;
            }
            const QString playbackState = state.toString();
            if (playbackState == QStringLiteral("pause")) {
                engine->setVolume(static_cast<float>(volume.toDouble()));
                engine->setMuted(muted.toBool());
                engine->setPaused(true);
            } else if (playbackState == QStringLiteral("run") || playbackState == QStringLiteral("throttle")) {
                const QJsonValue fps = command.value(QStringLiteral("fps"));
                if (!fps.isUndefined() && !fps.isDouble()) {
                    std::fprintf(stderr, "WebWallpaper: playbackState fps must be numeric\n");
                    return;
                }
                engine->setVolume(static_cast<float>(volume.toDouble()));
                engine->setMuted(muted.toBool());
                if (!fps.isUndefined()) engine->setFrameRate(fps.toInt());
                engine->setPaused(false);
            } else {
                std::fprintf(stderr, "WebWallpaper: unsupported playback state\n");
            }
        } else if (name == QStringLiteral("power")) {
            const QJsonValue state = command.value(QStringLiteral("state"));
            if (!state.isString()) {
                std::fprintf(stderr, "WebWallpaper: power requires a state\n");
                return;
            }
            const QString powerState = state.toString();
            if (powerState == QStringLiteral("pause")) {
                engine->setPaused(true);
            } else if (powerState == QStringLiteral("run") || powerState == QStringLiteral("throttle")) {
                const QJsonValue fps = command.value(QStringLiteral("fps"));
                if (!fps.isUndefined() && !fps.isDouble()) {
                    std::fprintf(stderr, "WebWallpaper: power fps must be numeric\n");
                    return;
                }
                if (!fps.isUndefined()) engine->setFrameRate(fps.toInt());
                engine->setPaused(false);
            } else {
                std::fprintf(stderr, "WebWallpaper: unsupported power state\n");
            }
        }
        else if (name == QStringLiteral("audioSpectrum")) {
            const QJsonArray data = command.value(QStringLiteral("data")).toArray();
            if (data.size() != 128) {
                std::fprintf(stderr, "WebWallpaper: audioSpectrum requires exactly 128 samples\n");
                return;
            }
            std::array<float, 128> spectrum {};
            for (qsizetype index = 0; index < data.size(); ++index) {
                const QJsonValue sample = data.at(index);
                if (!sample.isDouble()) {
                    std::fprintf(stderr, "WebWallpaper: audioSpectrum sample is not numeric\n");
                    return;
                }
                spectrum[static_cast<std::size_t>(index)] = static_cast<float>(sample.toDouble());
            }
            engine->pushAudioSpectrum(spectrum);
        }
        else if (name == QStringLiteral("snapshot")) engine->takeSnapshotToPath(command.value(QStringLiteral("path")).toString());
    }, [&app] { app.quit(); }, &app);
    if (parser.isSet(QStringLiteral("control-stdin"))) channel.start();
    return app.exec();
}
