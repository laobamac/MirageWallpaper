#pragma once

#include "ContentView/Components/PropertyEditorWidget.h"
#include "Services/FavoritesManager.h"
#include "Services/RendererController.h"
#include "Services/WallpaperLibrary.h"

#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QWidget>

class QComboBox;

namespace Mirage {

class WallpaperPreviewWidget : public QWidget {
    Q_OBJECT

public:
    explicit WallpaperPreviewWidget(FavoritesManager* favorites, QWidget* parent = nullptr);

public slots:
    void setWallpaper(const Mirage::Wallpaper& wallpaper);

signals:
    void favoriteRequested(const Mirage::Wallpaper& wallpaper);
    void volumeChanged(double volume);
    void speedChanged(double speed);
    void fillModeChanged(Mirage::FillMode mode);
    void propertyChanged(const QString& key, const Mirage::ProjectProperty& property);
    void applyAllRequested(const Mirage::Wallpaper& wallpaper);
    void stopRequested();
    void closeRequested();

private:
    QWidget* sectionHeader(const QString& title);
    void updateTags();

    FavoritesManager* m_favorites = nullptr;
    Wallpaper m_wallpaper;
    QLabel* m_preview = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_author = nullptr;
    QLabel* m_typeSize = nullptr;
    QLabel* m_dependency = nullptr;
    QWidget* m_tags = nullptr;
    QSlider* m_volume = nullptr;
    QSlider* m_speed = nullptr;
    QLabel* m_volumeValue = nullptr;
    QLabel* m_speedValue = nullptr;
    QWidget* m_speedRow = nullptr;
    QWidget* m_fillRow = nullptr;
    QComboBox* m_fill = nullptr;
    PropertyEditorWidget* m_properties = nullptr;
    QPushButton* m_favorite = nullptr;
};

} // namespace Mirage
