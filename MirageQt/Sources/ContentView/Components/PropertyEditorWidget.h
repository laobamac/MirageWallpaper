#pragma once

#include "Services/WEProject.h"

#include <QWidget>

class QVBoxLayout;

namespace Mirage {

class PropertyEditorWidget : public QWidget {
    Q_OBJECT

public:
    explicit PropertyEditorWidget(QWidget* parent = nullptr);

public slots:
    void setWallpaper(const Mirage::Wallpaper& wallpaper);

signals:
    void propertyChanged(const QString& key, const Mirage::ProjectProperty& property);

private:
    QWidget* widgetFor(const QString& key, ProjectProperty property);
    void clear();

    QVBoxLayout* m_layout = nullptr;
    Wallpaper m_wallpaper;
};

} // namespace Mirage
