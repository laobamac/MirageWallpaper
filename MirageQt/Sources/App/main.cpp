#include "Services/DisplayBrokerService.h"
#include "Services/MirageController.h"
#include "Services/Paths.h"
#ifdef Q_OS_WIN
#include "Services/WindowsPlaybackStateMonitor.h"
#endif
#include "SelfTest.h"
#include "StatusBar.h"
#include "UiBridge.h"

#include <QApplication>
#include <QDebug>
#include <QCommandLineParser>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextStream>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <QIcon>
#include <QQmlError>
#include <QScreen>
#include <QQuickStyle>
#include <QUrl>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QTextStream>
#include <cstdio>

namespace {

// GUI 子系统没有控制台，Qt 与渲染器/Steam 服务的诊断信息默认无处可去。
// 统一落一份日志到缓存目录，便于用户反馈问题时定位（也覆盖服务进程的
// stderr 与渲染器崩溃前的最后一条诊断）。
void InstallFileLogger() {
    static QMutex mutex;
    static QFile* file = nullptr;
    static QString path;
    path = Mirage::Paths::cacheDir() + QStringLiteral("/mirageqt.log");
    file = new QFile(path);
    if (! file->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning().noquote() << "[Log] Cannot open log file:" << path;
        delete file;
        file = nullptr;
        return;
    }
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& context, const QString& message) {
        const char* level = "info";
        switch (type) {
        case QtDebugMsg: level = "debug"; break;
        case QtInfoMsg: level = "info"; break;
        case QtWarningMsg: level = "warning"; break;
        case QtCriticalMsg: level = "critical"; break;
        case QtFatalMsg: level = "fatal"; break;
        }
        const QString line = QStringLiteral("%1 [%2] %3%4\n")
                                 .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), QLatin1String(level),
                                      context.category ? QStringLiteral("%1: ").arg(QLatin1String(context.category))
                                                       : QString(),
                                      message);
        QMutexLocker lock(&mutex);
        if (file != nullptr) {
            file->write(line.toUtf8());
            file->flush();
        }
        std::fputs(line.toLocal8Bit().constData(), stderr);
    });
    qInfo().noquote() << "[Log] Writing diagnostics to" << path;
}

} // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--self-test") == 0) {
            QCoreApplication testApp(argc, argv);
            return Mirage::runSelfTests();
        }
    }
    // RichHTMLText 的 WebEngineView（远程富标签）需要 WebEngine 初始化；
    // 必须在 QCoreApplication/QGuiApplication 创建之前调用
    // （对齐 QtWebEngineQuick 的初始化要求，之后调用仅剩 deprecated 行为）。
    QtWebEngineQuick::initialize();
    // This allow us to read embedded resource files.
    qputenv("QML_XHR_ALLOW_FILE_READ", QByteArrayLiteral("1"));
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Mirage"));
    QCoreApplication::setApplicationName(QStringLiteral("MirageQtUI"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Mirage 壁纸管理器"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("self-test"), QStringLiteral("运行隔离的基础业务自测，不启动 Steam 或渲染器")});
    parser.addOption({QStringLiteral("run-seconds"), QStringLiteral("使用临时用户状态运行界面并自动退出，不启动 Steam 或恢复壁纸"), QStringLiteral("seconds")});
    parser.process(app);
    const bool boundedRun = parser.isSet(QStringLiteral("run-seconds"));
    std::unique_ptr<QTemporaryDir> testDirectory;
    int runMilliseconds = 0;
    if (boundedRun) {
        bool ok = false;
        const double seconds = parser.value(QStringLiteral("run-seconds")).toDouble(&ok);
        if (!ok || !std::isfinite(seconds) || seconds <= 0.0 || seconds * 1000.0 > std::numeric_limits<int>::max()) {
            QTextStream(stderr) << "--run-seconds 必须是有效的正数\n";
            return 2;
        }
        runMilliseconds = qMax(1, static_cast<int>(seconds * 1000.0));
        testDirectory = std::make_unique<QTemporaryDir>();
        if (!testDirectory->isValid()) {
            QTextStream(stderr) << "无法创建界面回归测试临时目录\n";
            return 1;
        }
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testDirectory->path());
        QStandardPaths::setTestModeEnabled(true);
        Mirage::Paths::setTestRoot(testDirectory->path());
        QTimer::singleShot(runMilliseconds, &app, &QCoreApplication::quit);
    }
    InstallFileLogger();
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    app.setWindowIcon(QIcon(QStringLiteral(
        ":/MirageQt/Resources/Assets.xcassets/AppIcon.appiconset/icon_1024.png")));

    // 检查系统报告的screen数量和输出信息
    const auto screens = QGuiApplication::screens();
    qInfo() << "[ScreenDebug] Total screens:" << screens.size();
    for (int i = 0; i < screens.size(); ++i) {
        QScreen* s = screens[i];
        qInfo() << "[ScreenDebug] Screen" << i << ":"
                << "name=" << s->name()
                << "manufacturer=" << s->manufacturer()
                << "model=" << s->model()
                << "serial=" << s->serialNumber();
    }

    Mirage::MirageController controller(nullptr, !boundedRun);
    UiBridge bridge(&app);
    Mirage::DisplayBrokerService displayBroker;
#ifdef Q_OS_WIN
    Mirage::WindowsPlaybackStateMonitor playbackStateMonitor;
    QObject::connect(&playbackStateMonitor, &Mirage::WindowsPlaybackStateMonitor::otherApplicationAudioChanged,
                     &controller, &Mirage::MirageController::handleOtherApplicationAudioState,
                     Qt::QueuedConnection);
    QObject::connect(&playbackStateMonitor, &Mirage::WindowsPlaybackStateMonitor::displayPowerChanged,
                     &controller, &Mirage::MirageController::handleDisplayPowerState,
                     Qt::QueuedConnection);
    QObject::connect(&playbackStateMonitor, &Mirage::WindowsPlaybackStateMonitor::batteryStateChanged,
                     &controller, &Mirage::MirageController::handleBatteryState,
                     Qt::QueuedConnection);
    playbackStateMonitor.start();
#endif
    QObject::connect(&displayBroker, &Mirage::DisplayBrokerService::outputAdded,
                     &controller, &Mirage::MirageController::handleOutputAdded,
                     Qt::QueuedConnection);
    QObject::connect(&displayBroker, &Mirage::DisplayBrokerService::outputUpdated,
                     &controller, &Mirage::MirageController::handleOutputUpdated,
                     Qt::QueuedConnection);
    QObject::connect(&displayBroker, &Mirage::DisplayBrokerService::outputRemoved,
                     &controller, &Mirage::MirageController::handleOutputRemoved,
                     Qt::QueuedConnection);
    // A producer needs a registered display consumer before the broker can
    // send OUTPUT_CONFIG.  KDE creates that consumer from its wallpaper QML,
    // which is later than the broker listener, so startup restoration waits
    // for the first copied output event and is kept one-shot per process.
    bool startupPlaybackStarted = false;
    QObject::connect(&displayBroker, &Mirage::DisplayBrokerService::outputAdded,
                     &controller, [&controller, &startupPlaybackStarted, boundedRun](const Mirage::DisplayOutputSnapshot&) {
                         if (boundedRun || startupPlaybackStarted) return;
                         startupPlaybackStarted = true;
                         controller.startPlayback();
                     }, Qt::QueuedConnection);
#ifdef Q_OS_WIN
    QObject::connect(&displayBroker, &Mirage::DisplayBrokerService::desktopShellAvailable,
                     &controller, &Mirage::MirageController::handleDesktopShellAvailable,
                     Qt::QueuedConnection);
#endif
    // 桌面窗口状态（焦点/全屏）由 broker 回调上报
    QObject::connect(&displayBroker, &Mirage::DisplayBrokerService::windowStateChanged,
                     &controller, &Mirage::MirageController::handleWindowState);
    QString brokerError;
    if (!displayBroker.start(&brokerError)) {
        qWarning().noquote() << brokerError;
    }
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("controller"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    const QUrl url(QStringLiteral("qrc:/MirageQt/Qml/Main.qml"));
    // The engine only emits structured diagnostics; forwarding them keeps
    // packaged QML failures visible when no development console is attached.
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [](const QList<QQmlError>& warnings) {
                         for (const QQmlError& warning : warnings)
                             qWarning().noquote() << warning.toString();
                     });
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
                     [url](QObject* object, const QUrl& objectUrl) {
                         if (!object && objectUrl == url) QCoreApplication::exit(-1);
                     }, Qt::QueuedConnection);
    engine.load(url);

    Mirage::StatusBar statusBar(&engine, &controller, &app);
    app.setQuitOnLastWindowClosed(!statusBar.start());
    return app.exec();
}
