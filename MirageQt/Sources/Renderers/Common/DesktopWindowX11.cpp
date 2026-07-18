#include "Renderers/Common/DesktopWindowX11.h"

#include <QGuiApplication>
#include <QScreen>

namespace Mirage {

bool DesktopWindowX11::attachToDesktop(QWidget* widget, int screenIndex) {
    if (!widget) return false;
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screenIndex < 0 || screenIndex >= screens.size()) screenIndex = 0;
    if (!screens.isEmpty()) widget->setGeometry(screens.at(screenIndex)->geometry());
    widget->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnBottomHint);
    widget->setAttribute(Qt::WA_ShowWithoutActivating);
    return true;
}

void DesktopWindowX11::makeNormalToolWindow(QWidget* widget) {
    if (!widget) return;
    widget->setWindowFlags(Qt::Window);
}

} // namespace Mirage
