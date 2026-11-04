#include "WallpaperManifest.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

WRManifest WRManifest::loadFromDirectory(const QString& directory, QString* error) {
    QFile file(directory + QStringLiteral("/project.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = QStringLiteral("cannot open project.json");
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("project.json is not an object");
        return {};
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("type")).toString().compare(QStringLiteral("web"), Qt::CaseInsensitive) != 0) {
        if (error != nullptr) *error = QStringLiteral("project.json type is not web");
        return {};
    }
    WRManifest manifest;
    manifest.m_directory = QFileInfo(directory).canonicalFilePath();
    manifest.m_entryHtml = root.value(QStringLiteral("file")).toString(QStringLiteral("index.html"));
    const QString entry = QFileInfo(QDir(manifest.m_directory).filePath(manifest.m_entryHtml)).canonicalFilePath();
#ifdef Q_OS_WIN
    const auto sensitivity = Qt::CaseInsensitive;
#else
    const auto sensitivity = Qt::CaseSensitive;
#endif
    if (manifest.m_directory.isEmpty() || QDir::isAbsolutePath(manifest.m_entryHtml) ||
        !entry.startsWith(manifest.m_directory + QLatin1Char('/'), sensitivity) || !QFileInfo(entry).isFile()) {
        if (error != nullptr) *error = QStringLiteral("wallpaper entry must be a file inside the project directory");
        return {};
    }
    manifest.m_entryHtml = QDir::fromNativeSeparators(manifest.m_entryHtml);
    manifest.m_title = root.value(QStringLiteral("title")).toString(QStringLiteral("Wallpaper"));
    const QJsonObject general = root.value(QStringLiteral("general")).toObject();
    manifest.m_userProperties = general.value(QStringLiteral("properties")).toObject();
    return manifest;
}
