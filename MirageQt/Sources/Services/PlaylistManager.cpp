#include "Services/PlaylistManager.h"

#include "Services/Paths.h"
#include "Services/PlaylistRotator.h"
#include "Services/RendererController.h"
#include "Services/WallpaperLibrary.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace Mirage {

PlaylistManager::PlaylistManager(WallpaperLibrary* library,
                                 RendererController* renderer,
                                 QObject* parent)
    : QObject(parent)
    , m_library(library)
    , m_renderer(renderer) {
    m_storagePath = Paths::dataDir() + QStringLiteral("/playlists.json");
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(400);
    connect(&m_saveTimer, &QTimer::timeout, this, &PlaylistManager::saveNow);
    load();
}

PlaylistManager::~PlaylistManager() {
    stopAllRotators();
    if (m_saveTimer.isActive()) saveNow();
}

Playlist PlaylistManager::current(int screen) const {
    return m_currents.value(screen, defaultPlaylist());
}

QVector<Playlist> PlaylistManager::saved() const {
    return m_saved;
}

void PlaylistManager::ensureScreen(int screen) {
    if (m_currents.contains(screen)) return;
    m_currents.insert(screen, defaultPlaylist());
    scheduleSave();
    rebuildRotator(screen, false);
}

void PlaylistManager::startRotators() {
    stopAllRotators();
    const auto screens = m_currents.keys();
    for (int screen : screens) rebuildRotator(screen, true);
}

void PlaylistManager::kickRotator(int screen) {
    if (PlaylistRotator* rotator = m_rotators.value(screen)) {
        rotator->rebuild(PlaylistRotator::StartReason::SettingsChanged);
    } else {
        rebuildRotator(screen, false);
    }
}

void PlaylistManager::stopAllRotators() {
    for (PlaylistRotator* rotator : std::as_const(m_rotators)) {
        rotator->stop();
        rotator->deleteLater();
    }
    m_rotators.clear();
}

void PlaylistManager::add(const Wallpaper& wallpaper, int screen) {
    if (!wallpaper.isValid()) return;
    mutateCurrent(screen, [&](Playlist& playlist) {
        for (const PlaylistItem& item : playlist.items) {
            if (item.wallpaperID == wallpaper.id()) return;
        }
        PlaylistItem item;
        item.wallpaperID = wallpaper.id();
        item.addedAt = QDateTime::currentDateTimeUtc();
        playlist.items.push_back(item);
    });
}

void PlaylistManager::remove(const QString& itemID, int screen) {
    mutateCurrent(screen, [&](Playlist& playlist) {
        playlist.items.erase(std::remove_if(playlist.items.begin(),
                                            playlist.items.end(),
                                            [&](const PlaylistItem& item) {
                                                return item.wallpaperID == itemID;
                                            }),
                             playlist.items.end());
    });
}

void PlaylistManager::move(int source, int destination, int screen) {
    mutateCurrent(screen, [&](Playlist& playlist) {
        if (source < 0 || source >= playlist.items.size()) return;
        const int clamped = qBound(0, destination, playlist.items.size());
        const PlaylistItem item = playlist.items.takeAt(source);
        const int insertion = clamped > source ? clamped - 1 : clamped;
        playlist.items.insert(qMin(insertion, playlist.items.size()), item);
    });
}

void PlaylistManager::clear(int screen) {
    mutateCurrent(screen, [](Playlist& playlist) { playlist.items.clear(); });
}

void PlaylistManager::trimItems(int limit, int screen) {
    mutateCurrent(screen, [limit](Playlist& playlist) {
        if (playlist.items.size() > limit) {
            playlist.items = playlist.items.mid(0, limit);
        }
    });
}

void PlaylistManager::updateSettings(int screen, const std::function<void(PlaylistSettings&)>& transform) {
    mutateCurrent(screen, [&](Playlist& playlist) { transform(playlist.settings); });
}

void PlaylistManager::resetSettings(int screen) {
    mutateCurrent(screen, [](Playlist& playlist) { playlist.settings = PlaylistSettings::defaults(); });
}

Playlist PlaylistManager::saveAs(const QString& name, int screen) {
    if (!m_currents.contains(screen)) return {};
    Playlist current = m_currents.value(screen);
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return {};
    current.name = trimmed;
    current.touch();

    bool replaced = false;
    for (int i = 0; i < m_saved.size(); ++i) {
        if (m_saved.at(i).name == trimmed) {
            Playlist replacement = current;
            replacement.id = m_saved.at(i).id;
            m_saved[i] = replacement;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        Playlist copy = current;
        copy.id = QUuid::createUuid();
        m_saved.push_back(copy);
    }

    m_currents[screen] = current;
    scheduleSave();
    emit currentChanged(screen);
    emit savedChanged();
    return current;
}

void PlaylistManager::loadSaved(const Playlist& playlist, int screen) {
    Playlist target = playlist;
    target.touch();
    m_currents[screen] = target;
    scheduleSave();
    emit currentChanged(screen);
    if (PlaylistRotator* rotator = m_rotators.value(screen)) {
        rotator->rebuild(PlaylistRotator::StartReason::ListChanged);
    } else {
        rebuildRotator(screen, false);
    }
}

void PlaylistManager::deleteSaved(const QUuid& id) {
    m_saved.erase(std::remove_if(m_saved.begin(),
                                 m_saved.end(),
                                 [&](const Playlist& playlist) { return playlist.id == id; }),
                  m_saved.end());
    scheduleSave();
    emit savedChanged();
}

QVector<Wallpaper> PlaylistManager::resolvedItems(int screen) const {
    QVector<Wallpaper> resolved;
    if (!m_library) return resolved;
    const Playlist playlist = current(screen);
    resolved.reserve(playlist.items.size());
    for (const PlaylistItem& item : playlist.items) {
        const Wallpaper wallpaper = resolveWallpaper(item.wallpaperID);
        if (wallpaper.isValid()) resolved.push_back(wallpaper);
    }
    return resolved;
}

Wallpaper PlaylistManager::resolveWallpaper(const QString& id) const {
    if (!m_library || id.isEmpty()) return {};
    for (const Wallpaper& wallpaper : m_library->loadAll()) {
        if (wallpaper.id() == id) return wallpaper;
    }
    return {};
}

void PlaylistManager::setCurrentWallpaper(int screen, const Wallpaper& wallpaper) {
    m_currentWallpapers.insert(screen, wallpaper);
}

Wallpaper PlaylistManager::currentWallpaper(int screen) const {
    return m_currentWallpapers.value(screen);
}

void PlaylistManager::load() {
    QFile file(m_storagePath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_currents = {{0, defaultPlaylist()}};
        return;
    }

    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        m_currents = {{0, defaultPlaylist()}};
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonObject currents = root.value(QStringLiteral("currents")).toObject();
    for (auto it = currents.begin(); it != currents.end(); ++it) {
        bool ok = false;
        const int screen = it.key().toInt(&ok);
        if (!ok) continue;
        m_currents.insert(screen, Playlist::fromJson(it.value().toObject()));
    }
    if (!m_currents.contains(0)) m_currents.insert(0, defaultPlaylist());

    const QJsonArray saved = root.value(QStringLiteral("saved")).toArray();
    m_saved.reserve(saved.size());
    for (const auto& value : saved) {
        m_saved.push_back(Playlist::fromJson(value.toObject()));
    }
}

    // Persistence is debounced so bursts of edits (dragging items, changing
    // settings) write once after the UI settles instead of on every mutation.
void PlaylistManager::scheduleSave() {
    m_saveTimer.start();
}

void PlaylistManager::saveNow() {
    QDir().mkpath(Paths::dataDir());
    QJsonObject currents;
    for (auto it = m_currents.constBegin(); it != m_currents.constEnd(); ++it) {
        currents.insert(QString::number(it.key()), it.value().toJson());
    }
    QJsonArray saved;
    for (const Playlist& playlist : m_saved) saved.push_back(playlist.toJson());

    const QJsonObject root{
        {QStringLiteral("currents"), currents},
        {QStringLiteral("saved"), saved},
    };

    QFile file(m_storagePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }
}

void PlaylistManager::mutateCurrent(int screen, const std::function<void(Playlist&)>& transform) {
    Playlist playlist = m_currents.value(screen, defaultPlaylist());
    transform(playlist);
    playlist.touch();
    m_currents[screen] = playlist;
    scheduleSave();
    emit currentChanged(screen);
    if (PlaylistRotator* rotator = m_rotators.value(screen)) {
        rotator->rebuild(PlaylistRotator::StartReason::ListChanged);
    } else {
        rebuildRotator(screen, false);
    }
}

Playlist PlaylistManager::defaultPlaylist() const {
    Playlist playlist;
    playlist.name = QStringLiteral("默认播放列表");
    return playlist;
}

void PlaylistManager::rebuildRotator(int screen, bool appLaunch) {
    if (PlaylistRotator* existing = m_rotators.take(screen)) {
        existing->stop();
        existing->deleteLater();
    }
    auto* rotator = new PlaylistRotator(screen, this, m_renderer, this);
    m_rotators.insert(screen, rotator);
    rotator->start(appLaunch ? PlaylistRotator::StartReason::AppLaunch
                             : PlaylistRotator::StartReason::ListChanged);
}

} // namespace Mirage
