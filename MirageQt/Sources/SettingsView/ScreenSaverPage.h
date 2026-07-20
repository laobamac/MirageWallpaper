#pragma once

#include <QWidget>

namespace Mirage {

class ScreenSaverPage final : public QWidget {
    Q_OBJECT

public:
    explicit ScreenSaverPage(QWidget* parent = nullptr);
};

} // namespace Mirage
