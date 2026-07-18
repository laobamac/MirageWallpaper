#include "Renderers/WebWallpaperQt/WebWallpaperWindow.h"

#include <QApplication>

namespace Mirage {

WebWallpaperWindow::WebWallpaperWindow(QWidget* parent)
    : QLabel(parent) {
    setText(QStringLiteral("WebWallpaperQt placeholder\nLinux web renderer is not implemented yet."));
    setAlignment(Qt::AlignCenter);
    setMinimumSize(640, 360);
    setStyleSheet(QStringLiteral("QLabel { background: #20242a; color: #f1f3f5; font-size: 18px; }"));
}

void WebWallpaperWindow::handleCommand(const QJsonObject& command) {
    const QString cmd = command.value("cmd").toString();
    if (cmd == "quit") qApp->quit();
}

} // namespace Mirage
