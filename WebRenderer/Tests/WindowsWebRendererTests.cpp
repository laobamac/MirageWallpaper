#include "WallpaperManifest.h"
#include "WebRendererEngine.h"
#include "WRAudioTap.h"
#include <windows.h>
#include <mmsystem.h>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineUrlScheme>
#include <QWebEngineView>
#include <cstdio>
#include <functional>

static bool waitUntil(const std::function<bool()>& condition, int timeout = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return condition();
}

static QVariant evaluate(WebRendererEngine& engine, const QString& source) {
    struct Result { QVariant value; bool finished = false; };
    auto result = std::make_shared<Result>();
    engine.view()->page()->runJavaScript(source, [result](const QVariant& value) {
        result->value = value;
        result->finished = true;
    });
    if (!waitUntil([&] { return result->finished; })) std::fprintf(stderr, "JavaScript callback timed out\n");
    return result->value;
}

static bool writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

#define REQUIRE(expression) do { if (!(expression)) { std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #expression); return 1; } } while (false)

int main(int argc, char** argv) {
    QWebEngineUrlScheme scheme("mirage-wallpaper");
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::HostAndPort);
    scheme.setDefaultPort(443);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalScheme |
                    QWebEngineUrlScheme::LocalAccessAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);
    QApplication app(argc, argv);
    QTemporaryDir project;
    REQUIRE(project.isValid());
    REQUIRE(writeFile(project.filePath("project.json"), R"JSON({"type":"web","file":"index.html","general":{"properties":{"color":{"value":"#112233"}}}})JSON"));
    REQUIRE(writeFile(project.filePath("index.html"), R"HTML(<!doctype html><html><head><style>html,body{margin:0;width:100%;height:100%;background:#112233}</style></head><body><script>
window.observed={};window.motion=0;window.buttons=0;window.axes=0;window.audio=[];
window.wallpaperPropertyListener={applyUserProperties:function(values){Object.keys(values).forEach(function(key){observed[key]=values[key];});if(values.color)document.body.style.background=values.color.value;},setPaused:function(value){window.hostPaused=value;}};
wallpaperRegisterAudioListener(function(data){window.audio=data;});
document.addEventListener('mousemove',function(){window.motion++;});
document.addEventListener('mousedown',function(){window.buttons++;});
document.addEventListener('wheel',function(){window.axes++;});
</script></body></html>)HTML"));
    QString error;
    if (qEnvironmentVariableIsSet("WR_TEST_WASAPI")) {
        WRAudioTap tap;
        if (!tap.start(&error)) { std::fprintf(stderr, "%s\n", qPrintable(error)); return 1; }
        // 低电平半秒 PCM 验证真实默认输出→WASAPI→FFT，不更改系统音量。
        QByteArray wave;
        auto integer = [&wave](quint32 value, int bytes) {
            for (int index = 0; index < bytes; ++index) wave.append(static_cast<char>((value >> (8 * index)) & 0xff));
        };
        const int frames = 24000;
        wave.append("RIFF"); integer(36 + frames * 4, 4); wave.append("WAVEfmt ");
        integer(16, 4); integer(1, 2); integer(2, 2); integer(48000, 4);
        integer(192000, 4); integer(4, 2); integer(16, 2); wave.append("data"); integer(frames * 4, 4);
        for (int index = 0; index < frames; ++index) {
            const auto sample = static_cast<qint16>(327.0 * std::sin(2.0 * 3.141592653589793 * 1000.0 * index / 48000.0));
            integer(static_cast<quint16>(sample), 2); integer(static_cast<quint16>(sample), 2);
        }
        REQUIRE(PlaySoundW(reinterpret_cast<LPCWSTR>(wave.constData()), nullptr, SND_MEMORY | SND_SYNC | SND_NODEFAULT));
        std::array<float, 64> left{}, right{};
        REQUIRE(tap.copySpectrum(left, right));
        const float peak = std::max(*std::max_element(left.begin(), left.end()), *std::max_element(right.begin(), right.end()));
        std::fprintf(stderr, "WASAPI real loopback spectrum peak=%f\n", peak);
        REQUIRE(peak > 0.01f);
        tap.stop();
        REQUIRE(!tap.isRunning());
    }
    const WRManifest manifest = WRManifest::loadFromDirectory(project.path(), &error);
    REQUIRE(!manifest.workshopDirectory().isEmpty());
    WebRendererEngine::Config config;
    config.enableAudioSpectrum = false;
    config.loadFromMemory = true;
    WebRendererEngine engine(config);
    engine.view()->resize(320, 200);
    engine.view()->show();
    bool ready = false;
    bool presented = false;
    QString failure;
    QObject::connect(&engine, &WebRendererEngine::contentReady, [&] { ready = true; });
    QObject::connect(&engine, &WebRendererEngine::firstFramePresented, [&] { presented = true; });
    QObject::connect(&engine, &WebRendererEngine::failed, [&](const QString& message) { failure = message; });
    engine.openWallpaper(manifest);
    engine.setVolume(0.35f);
    engine.setMuted(true);
    engine.applyUserProperty("color", QJsonObject{{"value", "#20c040"}});
    engine.startAudioSpectrum();
    REQUIRE(waitUntil([&] { return ready || !failure.isEmpty(); }));
    REQUIRE(failure.isEmpty());
    REQUIRE(waitUntil([&] { return presented; }));
    REQUIRE(evaluate(engine, "window.__mirage_volume").toDouble() > 0.34);
    REQUIRE(evaluate(engine, "window.__mirage_volume").toDouble() < 0.36);
    REQUIRE(evaluate(engine, "window.__mirage_muted").toBool());
    REQUIRE(evaluate(engine, "observed.color.value").toString() == "#20c040");
    engine.sendPointerEnter(0.5f, 0.5f);
    engine.sendPointerMotion(0.5f, 0.5f);
    engine.sendPointerButton(0.5f, 0.5f, Qt::LeftButton, true);
    engine.sendPointerButton(0.5f, 0.5f, Qt::LeftButton, false);
    engine.sendPointerAxis(0.5f, 0.5f, 0, 1, false);
    REQUIRE(waitUntil([&] { return evaluate(engine, "motion>0&&buttons>0&&axes>0").toBool(); }));
    bool demand = false;
    QObject::connect(&engine, &WebRendererEngine::audioSpectrumDemandChanged, [&](bool needed) { demand = needed; });
    engine.setPaused(true);
    engine.setPaused(false);
    REQUIRE(waitUntil([&] { return demand; }));
    REQUIRE(evaluate(engine, "window.__wr_audio_listener_count").toInt() == 1);
    std::array<float, 128> spectrum{};
    spectrum.fill(0.25f);
    REQUIRE(waitUntil([&] { engine.pushAudioSpectrum(spectrum); return evaluate(engine, "audio.length==128&&audio[0]==0.25&&audio[127]==0.25").toBool(); }));
    engine.setPaused(true);
    REQUIRE(evaluate(engine, "window.__mirage_paused&&hostPaused&&document.hidden").toBool());
    engine.setPaused(false);
    REQUIRE(!evaluate(engine, "window.__mirage_paused||document.hidden").toBool());
    const QString snapshot = project.filePath("snapshot.png");
    bool saved = false;
    QObject::connect(&engine, &WebRendererEngine::snapshotFinished, [&](bool ok) { saved = ok; });
    engine.takeSnapshotToPath(snapshot);
    REQUIRE(waitUntil([&] { return saved; }));
    const QImage image(snapshot);
    REQUIRE(!image.isNull());
    REQUIRE(image.pixelColor(image.width()/2, image.height()/2) == QColor("#20c040"));
    bool ended = false;
    QObject::connect(&engine, &WebRendererEngine::videoDidEnd, [&] { ended = true; });
    evaluate(engine, "var video=document.createElement('video');document.body.appendChild(video);video.dispatchEvent(new Event('ended'));");
    REQUIRE(waitUntil([&] { return ended; }));
    // Windows 路径、目录穿越和项目外文件不能成为受控资源入口。
    REQUIRE(writeFile(project.filePath("project.json"), R"JSON({"type":"web","file":"../outside.html"})JSON"));
    REQUIRE(WRManifest::loadFromDirectory(project.path(), &error).workshopDirectory().isEmpty());
    REQUIRE(writeFile(project.filePath("project.json"), R"JSON({"type":"web","file":"index.html"})JSON"));

    if (argc > 1) {
        QProcess process;
        process.start(QString::fromLocal8Bit(argv[1]), {project.path(), "--windowed", "--control-stdin", "--external-spectrum", "--load-from-memory"});
        REQUIRE(process.waitForStarted(15000));
        QByteArray output;
        REQUIRE(waitUntil([&] {
            output += process.readAllStandardOutput();
            return output.contains("first-frame-presented");
        }));
        REQUIRE(process.write("{\"cmd\":\"setProperty\",\"key\":\"color\",") > 0);
        REQUIRE(process.write("\"value\":\"#20c040\"}\r\n") > 0);
        const QByteArray snapshotCommand = QJsonDocument(QJsonObject{{"cmd", "snapshot"}, {"path", snapshot}, {"token", "quote\"token"}}).toJson(QJsonDocument::Compact) + '\n';
        REQUIRE(process.write(snapshotCommand) == snapshotCommand.size());
        REQUIRE(waitUntil([&] {
            output += process.readAllStandardOutput();
            return output.contains("snapshot-done");
        }));
        for (const auto& line : output.split('\n')) {
            if (line.trimmed().isEmpty()) continue;
            REQUIRE(QJsonDocument::fromJson(line).isObject());
        }
        process.closeWriteChannel();
        REQUIRE(process.waitForFinished(15000));
        REQUIRE(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0);
        // 管道仍打开时显式退出也须取消 ReadFile，不能卡住析构。
        process.start(QString::fromLocal8Bit(argv[1]), {project.path(), "--windowed", "--control-stdin", "--no-spectrum"});
        REQUIRE(process.waitForStarted(15000));
        REQUIRE(process.write("{\"cmd\":\"quit\"}\n") > 0);
        REQUIRE(process.waitForBytesWritten(5000));
        if (!process.waitForFinished(15000)) {
            std::fprintf(stderr, "quit stdout: %s\nquit stderr: %s\n",
                process.readAllStandardOutput().constData(), process.readAllStandardError().constData());
            return 1;
        }
        REQUIRE(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0);
    }
    std::fprintf(stderr, "Windows WebRenderer integration passed\n");
    return 0;
}
