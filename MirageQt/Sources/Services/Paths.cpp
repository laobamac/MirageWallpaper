#include "Services/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace Mirage {
namespace {

QString ensureDir(const QString& path) {
    QDir().mkpath(path);
    return QDir::cleanPath(path);
}

QString homePath(const QString& suffix) {
    return QDir::cleanPath(QDir::homePath() + "/" + suffix);
}

} // namespace

void Paths::ensureBaseDirectories() {
    ensureDir(configDir());
    ensureDir(dataDir());
    ensureDir(cacheDir());
    ensureDir(importedDir());
    ensureDir(steamCMDDir());
    ensureDir(workshopCacheDir());
}

QString Paths::repoRoot() {
    return QDir::cleanPath(QStringLiteral(MIRAGEQT_REPO_ROOT));
}

QString Paths::configDir() {
    return ensureDir(homePath(".config/MirageQt"));
}

QString Paths::dataDir() {
    return ensureDir(homePath(".local/share/MirageQt"));
}

QString Paths::cacheDir() {
    return ensureDir(homePath(".cache/MirageQt"));
}

QString Paths::importedDir() {
    return ensureDir(dataDir() + "/Wallpapers");
}

QString Paths::steamCMDDir() {
    return ensureDir(dataDir() + "/steamcmd");
}

QString Paths::workshopCacheDir() {
    return ensureDir(cacheDir() + "/WorkshopCache");
}

QString Paths::assetsDir() {
    const QString appAssets = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/assets");
    if (QFileInfo::exists(appAssets)) return appAssets;

    const QString repoAssets = QDir::cleanPath(repoRoot() + "/assets");
    if (QFileInfo::exists(repoAssets)) return repoAssets;

    return appAssets;
}

QString Paths::autostartDesktopFile() {
    return homePath(".config/autostart/mirageqt.desktop");
}

QStringList Paths::defaultSteamWorkshopDirs() {
    return {
        homePath(".steam/steam/steamapps/workshop/content/431960"),
        homePath(".local/share/Steam/steamapps/workshop/content/431960"),
        homePath(".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/workshop/content/431960"),
    };
}

QStringList Paths::steamCMDContentDirs() {
    const QString root = steamCMDDir();
    return {
        QDir::cleanPath(root + "/steamapps/workshop/content/431960"),
        QDir::cleanPath(root + "/home/.steam/steam/steamapps/workshop/content/431960"),
        QDir::cleanPath(root + "/home/.local/share/Steam/steamapps/workshop/content/431960"),
    };
}

} // namespace Mirage
