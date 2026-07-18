#pragma once

#include <QJsonObject>
#include <QLabel>

namespace Mirage {

class WebWallpaperWindow : public QLabel {
    Q_OBJECT

public:
    explicit WebWallpaperWindow(QWidget* parent = nullptr);

public slots:
    void handleCommand(const QJsonObject& command);
};

} // namespace Mirage
