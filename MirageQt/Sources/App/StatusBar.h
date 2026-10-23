#pragma once

#include <QObject>
#include <QMenu>
#include <QSystemTrayIcon>

class QAction;
class QQmlApplicationEngine;
class QWindow;

namespace Mirage {

class MirageController;

// Linux equivalent of MenuBars/StatusBar.swift. The platform tray remains in
// C++ because QSystemTrayIcon is a Widgets API rather than a QML control.
class StatusBar final : public QObject {
    Q_OBJECT

public:
    StatusBar(QQmlApplicationEngine* engine, MirageController* controller, QObject* parent = nullptr);

    bool start();

private:
    QWindow* mainWindow() const;
    void showMainWindow();
    void invokeMainWindowAction(const char* action);
    void setPaused(bool paused);

    QQmlApplicationEngine* m_engine = nullptr;
    MirageController* m_controller = nullptr;
    QMenu m_menu;
    QSystemTrayIcon m_tray;
    QAction* m_pauseAction = nullptr;
    bool m_paused = false;
};

} // namespace Mirage
