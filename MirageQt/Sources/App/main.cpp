#include "Services/DisplayBrokerService.h"
#include "Services/MirageController.h"
#include "StatusBar.h"

#include "FluentUI.h"

#include <QApplication>
#include <QDebug>
#include <QIcon>
#include <QScreen>
#include <QUrl>
#include <QtQml/qqmlextensionplugin.h>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

Q_IMPORT_QML_PLUGIN(FluentUIPlugin)

int main(int argc, char** argv) {
    // RichHTMLText 的 WebEngineView（远程富标签）需要 WebEngine 初始化；
    // 必须在 QCoreApplication/QGuiApplication 创建之前调用
    // （对齐 QtWebEngineQuick 的初始化要求，之后调用仅剩 deprecated 行为）。
    QtWebEngineQuick::initialize();
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("MirageQt"));
    QCoreApplication::setOrganizationName(QStringLiteral("Mirage"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    app.setWindowIcon(QIcon(QStringLiteral(":/appicon.png")));

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
    // 桌面窗口事实（焦点/全屏）由 broker 宿主回调上报，应用据此按播放规则
    // 驱动渲染器（对齐 macOS 的应用侧播放策略）。
    QObject::connect(&displayBroker, &Mirage::DisplayBrokerService::windowStateChanged,
                     &controller, &Mirage::MirageController::handleWindowState);
    QString brokerError;
    if (!displayBroker.start(&brokerError)) {
        qWarning().noquote() << brokerError;
    }
    QQmlApplicationEngine engine;
    FluentUI::registerTypes(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("mirage"), &controller);
    // QTP0001 places the MirageQt module under Qt's canonical resource import
    // prefix.  Loading this exact URL keeps the executable entry point aligned
    // with the module resource layout instead of relying on a legacy prefix.
    const QUrl url(QStringLiteral("qrc:/qt/qml/MirageQt/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
                     [url](QObject* object, const QUrl& objectUrl) {
                         if (!object && objectUrl == url) QCoreApplication::exit(-1);
                     }, Qt::QueuedConnection);
    engine.load(url);

    Mirage::StatusBar statusBar(&engine, &controller, &app);
    app.setQuitOnLastWindowClosed(!statusBar.start());
    return app.exec();
}
