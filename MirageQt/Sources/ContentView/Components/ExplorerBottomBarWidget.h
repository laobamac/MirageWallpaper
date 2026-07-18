#pragma once

#include <QWidget>

namespace Mirage {

class ExplorerBottomBarWidget : public QWidget {
    Q_OBJECT

public:
    explicit ExplorerBottomBarWidget(QWidget* parent = nullptr);

signals:
    void importRequested();
};

} // namespace Mirage
