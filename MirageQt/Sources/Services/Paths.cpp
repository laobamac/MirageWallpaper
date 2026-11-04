#include "Services/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#ifdef Q_OS_WIN
#include <QSettings>
#include <QRegularExpression>
#include <QFile>
#endif

namespace Mirage {
namespace {
QString testRoot;

QString ensureDir(const QString& path) {
    QDir().mkpath(path);
    return QDir::cleanPath(path);
}

QString homePath(const QString& suffix) {
    return QDir::cleanPath(QDir::homePath() + "/" + suffix);
}

} // namespace

void Paths::setTestRoot(const QString& directory) {
    testRoot = directory;
}

bool Paths::isTestMode() {
    return !testRoot.isEmpty();
}

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
    if (!testRoot.isEmpty()) return ensureDir(testRoot + "/config");
#ifdef Q_OS_WIN
    return ensureDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
#else
    return ensureDir(homePath(".config/MirageQtUI"));
#endif
}

QString Paths::dataDir() {
    if (!testRoot.isEmpty()) return ensureDir(testRoot + "/data");
#ifdef Q_OS_WIN
    return ensureDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
#else
    return ensureDir(homePath(".local/share/MirageQtUI"));
#endif
}

QString Paths::cacheDir() {
    if (!testRoot.isEmpty()) return ensureDir(testRoot + "/cache");
#ifdef Q_OS_WIN
    return ensureDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
#else
    return ensureDir(homePath(".cache/MirageQtUI"));
#endif
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
#ifdef Q_OS_WIN
    if (QFileInfo::exists(appAssets)) return appAssets;
#endif
    // Packaged builds install renderer assets under the FHS data directory;
    // this fixed path is compiled from CMake so Debian and RPM layouts agree.
    const QString installedAssets = QDir::cleanPath(QStringLiteral(MIRAGEQT_DATA_DIR) + "/assets");
    if (QFileInfo::exists(installedAssets)) return installedAssets;

    if (QFileInfo::exists(appAssets)) return appAssets;

    const QString repoAssets = QDir::cleanPath(repoRoot() + "/assets");
    if (QFileInfo::exists(repoAssets)) return repoAssets;

    return appAssets;
}

QString Paths::autostartDesktopFile() {
    if (!testRoot.isEmpty()) return testRoot + "/autostart/mirageqt-ui.desktop";
    return homePath(".config/autostart/mirageqt-ui.desktop");
}

QStringList Paths::defaultSteamWorkshopDirs() {
    if (!testRoot.isEmpty()) return {};
#ifdef Q_OS_WIN
    QSettings steam(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"), QSettings::NativeFormat);
    QStringList roots;
    const QString steamRoot = QDir::fromNativeSeparators(steam.value(QStringLiteral("SteamPath")).toString());
    if (!steamRoot.isEmpty()) roots.append(steamRoot);
    const QString programFiles = qEnvironmentVariable("ProgramFiles(x86)");
    if (!programFiles.isEmpty()) roots.append(QDir(programFiles).filePath(QStringLiteral("Steam")));
    const QStringList installs = roots;
    for (const QString& root : installs) {
        QFile libraries(QDir(root).filePath(QStringLiteral("steamapps/libraryfolders.vdf")));
        if (!libraries.open(QIODevice::ReadOnly)) continue;
        const QString content = QString::fromUtf8(libraries.readAll());
        const QRegularExpression pathPattern(QStringLiteral("\\\"path\\\"\\s*\\\"([^\\\"]+)\\\""));
        auto matches = pathPattern.globalMatch(content);
        while (matches.hasNext()) {
            QString path = matches.next().captured(1);
            path.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
            roots.append(QDir::fromNativeSeparators(path));
        }
    }
    QStringList result;
    for (const QString& root : roots)
        result.append(QDir(root).filePath(QStringLiteral("steamapps/workshop/content/431960")));
    result.removeDuplicates();
    return result;
#else
    return {
        homePath(".steam/steam/steamapps/workshop/content/431960"),
        homePath(".local/share/Steam/steamapps/workshop/content/431960"),
        homePath(".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/workshop/content/431960"),
    };
#endif
}

QStringList Paths::steamCMDContentDirs() {
    const QString root = steamCMDDir();
    return {
        QDir::cleanPath(root + "/steamapps/workshop/content/431960"),
        QDir::cleanPath(root + "/home/Steam/steamapps/workshop/content/431960"),
        QDir::cleanPath(root + "/home/.steam/steam/steamapps/workshop/content/431960"),
        QDir::cleanPath(root + "/home/.local/share/Steam/steamapps/workshop/content/431960"),
    };
}

QStringList Paths::legacySteamCMDContentDirs() {
    if (!testRoot.isEmpty()) return {};
    const QString root = homePath(".local/share/MirageQt/steamcmd");
    return {
        QDir::cleanPath(root + "/steamapps/workshop/content/431960"),
        QDir::cleanPath(root + "/home/Steam/steamapps/workshop/content/431960"),
        QDir::cleanPath(root + "/home/.steam/steam/steamapps/workshop/content/431960"),
        QDir::cleanPath(root + "/home/.local/share/Steam/steamapps/workshop/content/431960"),
    };
}

} // namespace Mirage
