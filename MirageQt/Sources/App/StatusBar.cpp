#include "StatusBar.h"

#include "Services/MirageController.h"

#include <QAction>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QIcon>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QUrl>
#include <QWindow>

namespace Mirage {

StatusBar::StatusBar(QQmlApplicationEngine* engine,
                     MirageController* controller,
                     QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_controller(controller)
    , m_tray(QIcon(QStringLiteral(":/appicon.png")), this) {
    m_tray.setToolTip(QStringLiteral("Mirage"));
}

bool StatusBar::start() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return false;

    auto* openAction = m_menu.addAction(QStringLiteral("打开 Mirage"));
    connect(openAction, &QAction::triggered, this, &StatusBar::showMainWindow);

    auto* importAction = m_menu.addAction(QStringLiteral("导入壁纸…"));
    connect(importAction, &QAction::triggered, this, [this] {
        invokeMainWindowAction("openImportPanel");
    });

    m_menu.addSeparator();

    auto* settingsAction = m_menu.addAction(QStringLiteral("设置…"));
    connect(settingsAction, &QAction::triggered, this, [this] {
        invokeMainWindowAction("openSettingsPanel");
    });

    auto* updateAction = m_menu.addAction(QStringLiteral("检查更新…（TODO：Linux 更新服务）"));
    updateAction->setEnabled(false);

    m_menu.addSeparator();

    auto* projectAction = m_menu.addAction(QStringLiteral("项目主页"));
    connect(projectAction, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/laobamac/MirageWallpaper")));
    });

    m_menu.addSeparator();
    m_menu.addAction(QStringLiteral("静音"), m_controller, &MirageController::muteWallpapers);
    m_pauseAction = m_menu.addAction(QStringLiteral("暂停"));
    connect(m_pauseAction, &QAction::triggered, this, [this] {
        if (m_paused)
            m_controller->resumeWallpapers();
        else
            m_controller->pauseWallpapers();
    });
    // 菜单文字跟随 PlaybackController 的实际暂停状态：QML 界面应用新
    // 壁纸/停止壁纸等路径也会通过 playbackPausedChanged 同步。
    connect(m_controller, &MirageController::playbackPausedChanged,
            this, &StatusBar::setPaused);

    auto* allScreensAction = m_menu.addAction(QStringLiteral("覆盖到所有显示器"));
    connect(allScreensAction, &QAction::triggered, this, [this] {
        m_controller->applySelected(true);
    });

    m_menu.addAction(QStringLiteral("停止壁纸"), m_controller, &MirageController::stopWallpapers);

    m_menu.addSeparator();
    auto* quitAction = m_menu.addAction(QStringLiteral("退出 Mirage"));
    connect(quitAction, &QAction::triggered, QCoreApplication::instance(), &QCoreApplication::quit);

    m_tray.setContextMenu(&m_menu);
    connect(&m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    showMainWindow();
            });
    m_tray.show();
    // 平台托盘可用性以 isSystemTrayAvailable() 为准：SNI/StatusNotifier
    // 托盘经 DBus 异步激活，show() 后立即查 isVisible() 可能误报 false，
    // 导致 main() 误设 quitOnLastWindowClosed=true、关闭窗口即退出。
    return QSystemTrayIcon::isSystemTrayAvailable();
}

QWindow* StatusBar::mainWindow() const {
    if (m_engine == nullptr || m_engine->rootObjects().isEmpty()) return nullptr;
    return qobject_cast<QWindow*>(m_engine->rootObjects().constFirst());
}

void StatusBar::showMainWindow() {
    QWindow* window = mainWindow();
    if (window == nullptr) {
        qWarning() << "StatusBar: main window not available, cannot restore";
        return;
    }

    // Wayland 的 surface 创建/销毁是异步的。每种状态只执行一次对应的显示
    // 操作，避免 setVisibility(Windowed) 后紧接 show() 造成两次 expose，令
    // Qt Quick 渲染线程继续使用关闭前已经失效的 EGL/Vulkan surface。
    const QWindow::Visibility visibility = window->visibility();
    if (visibility == QWindow::Minimized)
        window->showNormal();
    else if (visibility == QWindow::Hidden)
        window->show();
    window->raise();
    window->requestActivate();
}

void StatusBar::invokeMainWindowAction(const char* action) {
    QWindow* window = mainWindow();
    if (window == nullptr) return;

    showMainWindow();
    QMetaObject::invokeMethod(window, action, Qt::QueuedConnection);
}

void StatusBar::setPaused(bool paused) {
    m_paused = paused;
    if (m_pauseAction)
        m_pauseAction->setText(paused ? QStringLiteral("恢复播放") : QStringLiteral("暂停"));
}

} // namespace Mirage
