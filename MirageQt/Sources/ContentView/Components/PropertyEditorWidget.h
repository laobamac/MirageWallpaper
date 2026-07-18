#pragma once

#include "Services/WEProject.h"

#include <QFormLayout>
#include <QHash>
#include <QWidget>

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
    QWidget* editorFor(const QString& key, ProjectProperty property);
    void clear();

    QFormLayout* m_form = nullptr;
    Wallpaper m_wallpaper;
};

} // namespace Mirage
