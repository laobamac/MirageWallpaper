#pragma once

#include <QWidget>

namespace Mirage {

class ContentView : public QWidget {
    Q_OBJECT

public:
    explicit ContentView(QWidget* parent = nullptr);
};

} // namespace Mirage
