#pragma once

#include "ContentView/Components/FilterResultsWidget.h"
#include "Services/FavoritesManager.h"
#include "Services/WallpaperLibrary.h"

#include <QListWidget>
#include <QWidget>

namespace Mirage {

class WallpaperListWidget : public QWidget {
    Q_OBJECT

public:
    explicit WallpaperListWidget(WallpaperLibrary* library,
                                 FavoritesManager* favorites,
                                 QWidget* parent = nullptr);

    Wallpaper currentWallpaper() const;

public slots:
    void reload();
    void setSearchText(const QString& text);
    void setSortText(const QString& text);
    void setSortDescending(bool descending);
    void setFilterState(const Mirage::WallpaperFilterState& state);

signals:
    void wallpaperSelected(const Mirage::Wallpaper& wallpaper);
    void applyRequested(const Mirage::Wallpaper& wallpaper, bool allScreens);
    void importRequested();
    void favoriteToggled(const Mirage::Wallpaper& wallpaper);

private:
    void rebuildList();
    bool matchesFilter(const Wallpaper& wallpaper) const;

    WallpaperLibrary* m_library = nullptr;
    FavoritesManager* m_favorites = nullptr;
    QVector<Wallpaper> m_wallpapers;
    QListWidget* m_list = nullptr;
    QString m_searchText;
    QString m_sortText = QStringLiteral("名称");
    bool m_sortDescending = true;
    WallpaperFilterState m_filter;
};

} // namespace Mirage
