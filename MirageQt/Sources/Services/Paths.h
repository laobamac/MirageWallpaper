#pragma once

#include <QString>
#include <QStringList>

namespace Mirage {

class Paths {
public:
    static void ensureBaseDirectories();
    static void setTestRoot(const QString& directory);
    static bool isTestMode();

    static QString repoRoot();
    static QString configDir();
    static QString dataDir();
    static QString cacheDir();
    static QString importedDir();
    static QString steamCMDDir();
    static QString workshopCacheDir();
    static QString assetsDir();
    static QString autostartDesktopFile();

    static QStringList defaultSteamWorkshopDirs();
    static QStringList steamCMDContentDirs();
    static QStringList legacySteamCMDContentDirs();
};

} // namespace Mirage
