#pragma once

#include "Services/FavoritesManager.h"
#include "Services/WallpaperLibrary.h"

#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
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
    void setTypeFilterText(const QString& text);
    void setSortText(const QString& text);

signals:
    void wallpaperSelected(const Mirage::Wallpaper& wallpaper);
    void applyRequested(const Mirage::Wallpaper& wallpaper, bool allScreens);
    void importRequested();
    void favoriteToggled(const Mirage::Wallpaper& wallpaper);

private:
    void rebuildList();
    bool matchesFilter(const Wallpaper& wallpaper) const;
    QString itemText(const Wallpaper& wallpaper) const;

    WallpaperLibrary* m_library = nullptr;
    FavoritesManager* m_favorites = nullptr;
    QVector<Wallpaper> m_wallpapers;

    QLineEdit* m_search = nullptr;
    QComboBox* m_typeFilter = nullptr;
    QComboBox* m_sort = nullptr;
    QListWidget* m_list = nullptr;
    QPushButton* m_apply = nullptr;
    QPushButton* m_applyAll = nullptr;
    QPushButton* m_favorite = nullptr;
    QString m_externalSearch;
    QString m_externalTypeFilter = QStringLiteral("全部");
    QString m_externalSort = QStringLiteral("名称");
};

} // namespace Mirage
