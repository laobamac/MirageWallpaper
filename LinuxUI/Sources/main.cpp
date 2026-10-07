// Copyright © 2026 王孝慈. All rights reserved.
#include "UiBridge.h"
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <cstdio>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QWindow>
#include <QDesktopServices>
int main(int argc, char** argv) {
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == "--condition-worker") {
        QCoreApplication worker(argc, argv);
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly)) return 1;
        const auto request = QJsonDocument::fromJson(input.readAll()).object();
        QJSEngine engine;
        const auto values = request.value("values").toObject();
        for (auto it = values.begin(); it != values.end(); ++it) engine.globalObject().setProperty(it.key(), engine.toScriptValue(it.value().toVariant()));
        QJsonObject verdicts;
        for (const auto& expression : request.value("expressions").toArray()) {
            const QJSValue result = engine.evaluate(expression.toString());
            verdicts[expression.toString()] = result.isError() || result.isUndefined() || result.isNull() ? true : result.toBool();
        }
        const QByteArray response = QJsonDocument(verdicts).toJson(QJsonDocument::Compact);
        return std::fwrite(response.constData(), 1, static_cast<size_t>(response.size()), stdout) == static_cast<size_t>(response.size()) ? 0 : 1;
    }
    QApplication app(argc, argv);
    app.setOrganizationName("Mirage");
    app.setApplicationName("MirageLinuxUI");
    app.setApplicationDisplayName("Mirage");
    app.setApplicationVersion("1.0.0");
    QQuickStyle::setStyle("Basic");
    UiBridge bridge;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("bridge", &bridge);
    engine.load(QUrl("qrc:/Qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    app.setWindowIcon(QIcon(":/art/AppIcon.png"));
    auto* window = qobject_cast<QWindow*>(engine.rootObjects().first());
    QMenu menu;
    QSystemTrayIcon tray(app.windowIcon());
    tray.setToolTip("Mirage");
    const auto showWindow = [window]() { window->show(); window->raise(); window->requestActivate(); };
    const auto rebuildMenu = [&]() {
        menu.clear();
        menu.addAction(bridge.text("打开 Mirage"), &app, showWindow);
        menu.addAction(bridge.text("导入壁纸…"), &app, [&]() { showWindow(); QMetaObject::invokeMethod(window, "openImport"); });
        menu.addSeparator();
        menu.addAction(bridge.text("设置"), &app, [&]() { showWindow(); QMetaObject::invokeMethod(window, "openSettings"); });
        menu.addAction(bridge.text("检查更新…"))->setEnabled(false);
        menu.addAction(bridge.text("项目主页"), &app, []() { QDesktopServices::openUrl(QUrl("https://github.com/laobamac/MirageWallpaper")); });
        menu.addSeparator();
        for (const QString& key : {QStringLiteral("静音"), QStringLiteral("暂停"), QStringLiteral("上一张"), QStringLiteral("下一张"), QStringLiteral("覆盖到所有显示器"), QStringLiteral("停止壁纸")}) menu.addAction(bridge.text(key))->setEnabled(false);
        menu.addSeparator();
        menu.addAction(bridge.text("退出 Mirage"), &app, &QApplication::quit);
    };
    rebuildMenu();
    QObject::connect(&bridge, &UiBridge::languageChanged, &app, rebuildMenu);
    tray.setContextMenu(&menu);
    QObject::connect(&tray, &QSystemTrayIcon::activated, &app, [&](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) showWindow();
    });
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        app.setQuitOnLastWindowClosed(false);
        tray.show();
    }
    return app.exec();
}
