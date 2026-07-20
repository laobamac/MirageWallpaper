#pragma once

#include <QWidget>

namespace Mirage {

class DesktopWindowX11 {
public:
    static bool attachToDesktop(QWidget* widget, int screenIndex);
    static void makeNormalToolWindow(QWidget* widget);
};

} // namespace Mirage
