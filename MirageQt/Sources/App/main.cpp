#include "Services/DisplayBrokerService.h"
#include "Services/MirageController.h"
#include "StatusBar.h"
#include "UiBridge.h"

#include <QApplication>
#include <QDebug>
#include <QIcon>
#include <QQmlError>
#include <QScreen>
#include <QQuickStyle>
#include <QUrl>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

int main(int argc, char** argv) {
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

    Mirage::MirageController controller;
    UiBridge bridge(&app);
    Mirage::DisplayBrokerService displayBroker;
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
                     &controller, [&controller, &startupPlaybackStarted](const Mirage::DisplayOutputSnapshot&) {
                         if (startupPlaybackStarted) return;
                         startupPlaybackStarted = true;
                         controller.startPlayback();
                     }, Qt::QueuedConnection);
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
