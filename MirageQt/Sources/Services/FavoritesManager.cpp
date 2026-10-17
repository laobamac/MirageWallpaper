#include "Services/FavoritesManager.h"

#include "Services/Paths.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace Mirage {

FavoritesManager::FavoritesManager(QObject* parent)
    : QObject(parent) {
    load();
}

bool FavoritesManager::contains(const QString& wallpaperId) const {
    return m_favorites.contains(wallpaperId);
}

QSet<QString> FavoritesManager::favorites() const {
    return m_favorites;
}

void FavoritesManager::setFavorite(const QString& wallpaperId, bool favorite) {
    if (wallpaperId.isEmpty()) return;
    const bool wasFavorite = m_favorites.contains(wallpaperId);
    if (favorite == wasFavorite) return;
    if (favorite) {
        m_favorites.insert(wallpaperId);
    } else {
        m_favorites.remove(wallpaperId);
    }
    save();
    emit changed();
}

void FavoritesManager::toggle(const QString& wallpaperId) {
    setFavorite(wallpaperId, !contains(wallpaperId));
}

void FavoritesManager::load() {
    QFile file(Paths::configDir() + "/favorites.json");
    if (!file.open(QIODevice::ReadOnly)) return;
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) return;
    for (const auto& item : doc.array()) {
        const QString id = item.toString();
        if (!id.isEmpty()) m_favorites.insert(id);
    }
}

void FavoritesManager::save() const {
    QDir().mkpath(Paths::configDir());
    QJsonArray array;
    const auto values = m_favorites.values();
    for (const QString& id : values) array.push_back(id);
    QFile file(Paths::configDir() + "/favorites.json");
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    }
}

} // namespace Mirage
