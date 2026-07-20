#include "Services/WallpaperLibrary.h"

#include "Services/Paths.h"
#include "Services/ThumbnailGenerator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

namespace Mirage {
namespace {

bool copyRecursively(const QString& srcPath, const QString& dstPath, QString* error) {
    const QFileInfo srcInfo(srcPath);
    if (!srcInfo.exists()) {
        if (error) *error = QStringLiteral("源文件不存在");
        return false;
    }

    if (srcInfo.isDir()) {
        QDir().mkpath(dstPath);
        const QDir srcDir(srcPath);
        const QFileInfoList entries = srcDir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden);
        for (const QFileInfo& entry : entries) {
            if (!copyRecursively(entry.absoluteFilePath(), QDir(dstPath).filePath(entry.fileName()), error)) {
                return false;
            }
        }
        return true;
    }

    QFile::remove(dstPath);
    if (!QFile::copy(srcPath, dstPath)) {
        if (error) *error = QStringLiteral("复制失败: %1").arg(srcPath);
        return false;
    }
    QFile::setPermissions(dstPath, srcInfo.permissions());
    return true;
}

QString canonicalOrClean(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
}

QString dedupeKey(const Wallpaper& wallpaper) {
    if (!wallpaper.project.workshopId.isEmpty()) return QStringLiteral("workshop:") + wallpaper.project.workshopId;
    const QString leaf = QFileInfo(wallpaper.wallpaperDirectory).fileName();
    bool numeric = false;
    leaf.toULongLong(&numeric);
    if (numeric) return QStringLiteral("workshop:") + leaf;
    return QStringLiteral("path:") + canonicalOrClean(wallpaper.wallpaperDirectory);
}

} // namespace

WallpaperLibrary::WallpaperLibrary(GlobalSettingsService* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings) {
}

QString WallpaperLibrary::importedDirectory() const {
    const QString custom = m_settings ? m_settings->settings().customImportedDirectory.trimmed() : QString();
    return custom.isEmpty() ? Paths::importedDir() : QDir::cleanPath(custom);
}

QStringList WallpaperLibrary::sourceDirectories() const {
    QStringList dirs;

    const QString customWorkshop = m_settings ? m_settings->settings().customWorkshopDirectory.trimmed() : QString();
    if (!customWorkshop.isEmpty()) dirs << QDir::cleanPath(customWorkshop);

    dirs << importedDirectory();

    for (const QString& dir : Paths::defaultSteamWorkshopDirs()) {
        if (!dirs.contains(dir)) dirs << dir;
    }
    for (const QString& dir : Paths::steamCMDContentDirs()) {
        if (!dirs.contains(dir)) dirs << dir;
    }
    return dirs;
}

QVector<Wallpaper> WallpaperLibrary::loadAll() const {
    QVector<Wallpaper> result;
    QSet<QString> seen;

    for (const QString& source : sourceDirectories()) {
        const QDir dir(source);
        if (!dir.exists()) continue;

        const QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QFileInfo& entry : entries) {
            if (!QFileInfo::exists(entry.absoluteFilePath() + "/project.json")) continue;
            Wallpaper wallpaper = loadWallpaper(entry.absoluteFilePath());
            const QString key = dedupeKey(wallpaper);
            if (seen.contains(key)) continue;
            seen.insert(key);
            result.push_back(wallpaper);
        }
    }
    return result;
}

QStringList WallpaperLibrary::workshopItemDirectories(const QString& workshopId) const {
    QStringList dirs;
    if (workshopId.isEmpty()) return dirs;
    for (const QString& source : sourceDirectories()) {
        const QString candidate = QDir::cleanPath(source + "/" + workshopId);
        if (QFileInfo::exists(candidate + "/project.json")) dirs << candidate;
    }
    return dirs;
}

QString WallpaperLibrary::workshopItemDirectory(const QString& workshopId) const {
    const QStringList dirs = workshopItemDirectories(workshopId);
    return dirs.isEmpty() ? QString() : dirs.first();
}

Wallpaper WallpaperLibrary::loadWallpaper(const QString& directory) const {
    return loadWallpaper(directory, {});
}

QString WallpaperLibrary::importWallpaperFolder(const QString& directory, QString* error) const {
    const QFileInfo info(directory);
    if (!info.isDir() || !QFileInfo::exists(directory + "/project.json")) {
        if (error) *error = QStringLiteral("文件夹内没有 project.json");
        return {};
    }

    const QString destination = uniqueDestination(info.fileName());
    if (!copyRecursively(directory, destination, error)) return {};
    return destination;
}

QString WallpaperLibrary::importVideoFile(const QString& filePath, QString* error) const {
    const QFileInfo info(filePath);
    const QString ext = info.suffix().toLower();
    if (!(ext == "mp4" || ext == "mov" || ext == "m4v" || ext == "webm" || ext == "mkv")) {
        if (error) *error = QStringLiteral("不支持的壁纸类型");
        return {};
    }

    const QString destination = uniqueDestination(info.completeBaseName());
    QDir().mkpath(destination);

    const QString mediaName = info.fileName();
    const QString mediaDestination = QDir(destination).filePath(mediaName);
    QFile::remove(mediaDestination);
    if (!QFile::copy(filePath, mediaDestination)) {
        if (error) *error = QStringLiteral("复制视频失败");
        return {};
    }

    const QString previewName = QStringLiteral("preview.jpg");
    const QString previewPath = QDir(destination).filePath(previewName);
    if (!ThumbnailGenerator::generateForVideo(filePath, previewPath)) {
        ThumbnailGenerator::writePlaceholder(previewPath, info.completeBaseName());
    }

    Project project;
    project.file = mediaName;
    project.preview = previewName;
    project.title = info.completeBaseName();
    project.type = QStringLiteral("video");

    QFile projectFile(QDir(destination).filePath(QStringLiteral("project.json")));
    if (!projectFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("写入 project.json 失败");
        return {};
    }
    projectFile.write(QJsonDocument(project.toJson()).toJson(QJsonDocument::Indented));
    return destination;
}

QString WallpaperLibrary::importAny(const QString& path, QString* error) const {
    const QFileInfo info(path);
    if (info.isDir()) return importWallpaperFolder(path, error);
    return importVideoFile(path, error);
}

bool WallpaperLibrary::updateStoredMetadata(const Wallpaper& wallpaper, const QString& title, const QStringList& tags) const {
    QFile file(wallpaper.wallpaperDirectory + "/project.json");
    if (!file.open(QIODevice::ReadOnly)) return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return false;

    QJsonObject object = doc.object();
    if (!title.isNull()) object["title"] = title;
    if (!tags.isEmpty()) {
        QJsonArray array;
        for (const QString& tag : tags) array.push_back(tag);
        object["tags"] = array;
    }

    QSaveFile out(wallpaper.wallpaperDirectory + "/project.json");
    if (!out.open(QIODevice::WriteOnly)) return false;
    out.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return out.commit();
}

Wallpaper WallpaperLibrary::loadWallpaper(const QString& directory, QSet<QString> visited) const {
    QFile file(QDir(directory).filePath(QStringLiteral("project.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        Wallpaper wallpaper;
        wallpaper.wallpaperDirectory = QDir::cleanPath(directory);
        wallpaper.renderDirectory = wallpaper.wallpaperDirectory;
        wallpaper.project = Project::invalid();
        return wallpaper;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        Wallpaper wallpaper;
        wallpaper.wallpaperDirectory = QDir::cleanPath(directory);
        wallpaper.renderDirectory = wallpaper.wallpaperDirectory;
        wallpaper.project = Project::invalid();
        return wallpaper;
    }

    const Project project = Project::fromJson(doc.object());
    Wallpaper wallpaper;
    wallpaper.wallpaperDirectory = QDir::cleanPath(directory);
    wallpaper.renderDirectory = wallpaper.wallpaperDirectory;
    wallpaper.project = project;

    if (!project.isWorkshopPreset()) return wallpaper;

    const QString dependency = project.dependency;
    wallpaper.presetDependency = dependency;

    const QString thisLeaf = QFileInfo(directory).fileName();
    if (dependency == thisLeaf || visited.contains(dependency)) {
        wallpaper.presetStatus = PresetStatus::CircularDependency;
        return wallpaper;
    }

    const QStringList dependencyDirs = workshopItemDirectories(dependency);
    if (dependencyDirs.isEmpty()) {
        wallpaper.presetStatus = PresetStatus::MissingDependency;
        return wallpaper;
    }

    bool sawCircular = false;
    QSet<QString> nextVisited = visited;
    nextVisited.insert(thisLeaf);

    for (const QString& dependencyDir : dependencyDirs) {
        Wallpaper base = loadWallpaper(dependencyDir, nextVisited);
        if (!base.isValid() || !QFileInfo::exists(base.resolvedEntryPath())) {
            sawCircular = sawCircular || base.presetStatus == PresetStatus::CircularDependency;
            continue;
        }

        Project effectiveProject = base.project.applyingPreset(project);
        if (effectiveProject.workshopId.isEmpty()) effectiveProject.workshopId = thisLeaf;

        wallpaper.project = effectiveProject;
        wallpaper.renderDirectory = base.renderDirectory;
        wallpaper.assetOverlayDirectories = QStringList{wallpaper.wallpaperDirectory} + base.assetOverlayDirectories;
        wallpaper.presetStatus = PresetStatus::Resolved;
        return wallpaper;
    }

    wallpaper.presetStatus = sawCircular ? PresetStatus::CircularDependency : PresetStatus::InvalidDependency;
    return wallpaper;
}

QString WallpaperLibrary::uniqueDestination(const QString& baseName) const {
    QDir().mkpath(importedDirectory());
    QString cleanName = baseName;
    cleanName.replace(QRegularExpression(QStringLiteral("[/\\\\:]")), QStringLiteral("_"));
    if (cleanName.trimmed().isEmpty()) cleanName = QStringLiteral("Wallpaper");

    QString destination = QDir(importedDirectory()).filePath(cleanName);
    int counter = 1;
    while (QFileInfo::exists(destination)) {
        destination = QDir(importedDirectory()).filePath(QStringLiteral("%1_%2").arg(cleanName).arg(counter++));
    }
    return destination;
}

} // namespace Mirage
