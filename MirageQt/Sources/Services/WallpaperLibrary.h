#pragma once

#include "Services/GlobalSettingsService.h"
#include "Services/WEProject.h"

#include <QObject>
#include <QStringList>
#include <QVector>

namespace Mirage {

class WallpaperLibrary : public QObject {
    Q_OBJECT

public:
    explicit WallpaperLibrary(GlobalSettingsService* settings, QObject* parent = nullptr);

    QString importedDirectory() const;
    QStringList sourceDirectories() const;
    QVector<Wallpaper> loadAll() const;
    QStringList workshopItemDirectories(const QString& workshopId) const;
    QString workshopItemDirectory(const QString& workshopId) const;

    Wallpaper loadWallpaper(const QString& directory) const;
    QString importWallpaperFolder(const QString& directory, QString* error = nullptr) const;
    QString importVideoFile(const QString& filePath, QString* error = nullptr) const;
    QString importAny(const QString& path, QString* error = nullptr) const;

    bool updateStoredMetadata(const Wallpaper& wallpaper, const QString& title, const QStringList& tags) const;

private:
    Wallpaper loadWallpaper(const QString& directory, QSet<QString> visited) const;
    QString uniqueDestination(const QString& baseName) const;

    GlobalSettingsService* m_settings = nullptr;
};

} // namespace Mirage
