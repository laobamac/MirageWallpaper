#pragma once

#include <QJsonObject>
#include <QLabel>

namespace Mirage {

class VideoWallpaperWindow : public QLabel {
    Q_OBJECT

public:
    explicit VideoWallpaperWindow(QWidget* parent = nullptr);

public slots:
    void handleCommand(const QJsonObject& command);
};

} // namespace Mirage
