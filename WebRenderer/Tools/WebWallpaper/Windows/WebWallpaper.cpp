#include "ControlChannel.h"
#include "WallpaperManifest.h"
#include "WebRendererEngine.h"
#include "WRDesktopHost.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

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
    // WorkerW 和转场遮罩不能被 Chromium 当作停止合成的普通遮挡窗口。
    QByteArray chromiumFlags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    chromiumFlags += " --disable-features=CalculateNativeWinOcclusion --disable-backgrounding-occluded-windows --disable-renderer-backgrounding";
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", chromiumFlags);
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("screen"), QStringLiteral("目标屏幕索引"), QStringLiteral("index"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("output-id"), QStringLiteral("Windows 屏幕设备名"), QStringLiteral("device")});
    parser.addOption({QStringLiteral("windowed"), QStringLiteral("普通测试窗口，不嵌入桌面")});
    parser.addOption({QStringLiteral("run-seconds"), QStringLiteral("验证模式自动退出秒数"), QStringLiteral("seconds")});
    parser.addOption({QStringLiteral("no-audio"), QStringLiteral("禁用网页音频播放")});
    parser.addOption({QStringLiteral("properties"), QStringLiteral("初始用户属性 JSON 文件"), QStringLiteral("path")});
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
    bool validScreen = false;
    const int screenIndex = parser.value(QStringLiteral("screen")).toInt(&validScreen);
    if (!validScreen || screenIndex < 0) {
        std::fprintf(stderr, "WebWallpaper: --screen must be a nonnegative integer\n");
        return 1;
    }

    if (parser.isSet(QStringLiteral("run-seconds"))) {
        bool validDuration = false;
        const int seconds = parser.value(QStringLiteral("run-seconds")).toInt(&validDuration);
        if (!validDuration || seconds <= 0 || seconds > 86400) {
            std::fprintf(stderr, "WebWallpaper: --run-seconds must be between 1 and 86400\n");
            return 1;
        }
        QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
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
    config.enableAudioPlayback = !parser.isSet(QStringLiteral("no-audio"));
    auto engine = std::make_unique<WebRendererEngine>(config);
    engine->view()->resize(1280, 720);
    // Windows 直接托管 Chromium 的 Qt 窗口，常规渲染不读回 GPU 帧。
    WRDesktopHost host(engine.get());
    if (!parser.isSet(QStringLiteral("windowed"))) {
        // DesktopHost 的实现由各 Windows 渲染器共享。
        if (!host.attach(screenIndex, parser.value(QStringLiteral("output-id")), &error)) {
            std::fprintf(stderr, "WebWallpaper: %s\n", qPrintable(error));
            return 3;
        }
    } else {
        engine->view()->show();
    }
    engine->openWallpaper(manifest);
    if (parser.isSet(QStringLiteral("properties"))) {
        QFile properties(parser.value(QStringLiteral("properties")));
        if (!properties.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "WebWallpaper: cannot open initial properties\n");
            return 2;
        }
        const auto document = QJsonDocument::fromJson(properties.readAll());
        if (!document.isObject()) {
            std::fprintf(stderr, "WebWallpaper: initial properties must be a JSON object\n");
            return 2;
        }
        engine->applyUserProperties(document.object());
    }
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
    QObject::connect(engine.get(), &WebRendererEngine::firstFramePresented, [] {
        std::fprintf(stdout, "{\"event\":\"first-frame-presented\"}\n");
        std::fflush(stdout);
    });
    QObject::connect(engine.get(), &WebRendererEngine::videoDidEnd, [] {
        std::fprintf(stdout, "{\"event\":\"video-did-end\"}\n");
        std::fflush(stdout);
    });
    QStringList snapshotTokens;
    QObject::connect(engine.get(), &WebRendererEngine::snapshotFinished, [&snapshotTokens](bool ok) {
        const QString token = snapshotTokens.isEmpty() ? QString() : snapshotTokens.takeFirst();
        const QJsonObject result{{QStringLiteral("event"), QStringLiteral("snapshot-done")},
            {QStringLiteral("token"), token}, {QStringLiteral("ok"), ok}};
        const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
        std::fprintf(stdout, "%s\n", json.constData());
        std::fflush(stdout);
    });
    ControlChannel channel([&engine, &app, &snapshotTokens](const QJsonObject& command) {
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
        else if (name == QStringLiteral("snapshot")) {
            const auto path = command.value(QStringLiteral("path"));
            const auto token = command.value(QStringLiteral("token"));
            if (!path.isString() || path.toString().isEmpty() || !token.isString()) {
                std::fprintf(stderr, "WebWallpaper: snapshot requires path and token strings\n");
                return;
            }
            snapshotTokens.append(token.toString());
            engine->takeSnapshotToPath(path.toString());
        } else if (name == QStringLiteral("quit") || name == QStringLiteral("stop")) {
            if (qEnvironmentVariableIsSet("WR_DEBUG")) std::fprintf(stderr, "WebWallpaper: received quit\n");
            app.quit();
        } else if (name == QStringLiteral("pointerMotion") || name == QStringLiteral("pointerEnter") ||
                   name == QStringLiteral("pointerButton") || name == QStringLiteral("pointerAxis")) {
            const auto x = command.value(QStringLiteral("x"));
            const auto y = command.value(QStringLiteral("y"));
            if (!x.isDouble() || !y.isDouble()) {
                std::fprintf(stderr, "WebWallpaper: pointer command requires numeric x and y\n");
                return;
            }
            const float px = qBound(0.0f, static_cast<float>(x.toDouble()), 1.0f);
            const float py = qBound(0.0f, static_cast<float>(y.toDouble()), 1.0f);
            if (name == QStringLiteral("pointerMotion")) engine->sendPointerMotion(px, py);
            else if (name == QStringLiteral("pointerEnter")) engine->sendPointerEnter(px, py);
            else if (name == QStringLiteral("pointerButton")) {
                const auto pressed = command.value(QStringLiteral("pressed"));
                const auto button = command.value(QStringLiteral("button"));
                if (!pressed.isBool() || !button.isDouble() || button.toInt() < 1 || button.toInt() > 3) return;
                const Qt::MouseButton buttons[] = {Qt::LeftButton, Qt::RightButton, Qt::MiddleButton};
                engine->sendPointerButton(px, py, buttons[button.toInt() - 1], pressed.toBool());
            } else {
                const auto dx = command.value(QStringLiteral("deltaX"));
                const auto dy = command.value(QStringLiteral("deltaY"));
                if (!dx.isDouble() || !dy.isDouble()) return;
                engine->sendPointerAxis(px, py, static_cast<float>(dx.toDouble()),
                    static_cast<float>(dy.toDouble()), command.value(QStringLiteral("pixelBased")).toBool());
            }
        } else if (name == QStringLiteral("pointerLeave")) {
            engine->sendPointerLeave();
        }
    }, [&app] { app.quit(); }, &app);
    if (parser.isSet(QStringLiteral("control-stdin"))) channel.start();
    return app.exec();
}
