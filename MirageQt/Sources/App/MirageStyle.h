#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace Mirage {

void applyMirageStyle(QApplication& app, const QString& appearance = QStringLiteral("followSystem"));

// Theme border color of the currently active appearance (used by custom-drawn
// widgets such as the rounded combo-box popup panel).
QColor currentBorderColor();

} // namespace Mirage
