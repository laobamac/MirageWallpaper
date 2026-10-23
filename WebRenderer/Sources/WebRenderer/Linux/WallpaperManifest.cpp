#include "WallpaperManifest.h"

#include <QFile>
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
    manifest.m_directory = directory;
    manifest.m_entryHtml = root.value(QStringLiteral("file")).toString(QStringLiteral("index.html"));
    manifest.m_title = root.value(QStringLiteral("title")).toString(QStringLiteral("Wallpaper"));
    const QJsonObject general = root.value(QStringLiteral("general")).toObject();
    manifest.m_userProperties = general.value(QStringLiteral("properties")).toObject();
    return manifest;
}
