#pragma once

#include <QWidget>

namespace Mirage {

class AboutUsView final : public QWidget {
    Q_OBJECT

public:
    explicit AboutUsView(QWidget* parent = nullptr);
};

} // namespace Mirage
