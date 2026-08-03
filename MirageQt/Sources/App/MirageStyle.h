#pragma once

#include <QString>

class QApplication;

namespace Mirage {

void applyMirageStyle(QApplication& app, const QString& appearance = QStringLiteral("followSystem"));

} // namespace Mirage
