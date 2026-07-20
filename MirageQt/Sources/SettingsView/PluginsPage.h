#pragma once

#include <QWidget>

namespace Mirage {

class PluginsPage final : public QWidget {
    Q_OBJECT

public:
    explicit PluginsPage(QWidget* parent = nullptr);
};

} // namespace Mirage
