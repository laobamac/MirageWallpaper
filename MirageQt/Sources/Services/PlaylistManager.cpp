#include "Services/PlaylistManager.h"

#include "Services/Paths.h"
#include "Services/PlaylistRotator.h"
#include "Services/RendererController.h"
#include "Services/WallpaperLibrary.h"

#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QSaveFile>

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

QString PlaylistManager::screenKey(int screen) const {
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screen >= 0 && screen < screens.size()) {
        const QString stableId = RendererController::stableOutputId(screens.at(screen));
        if (!stableId.isEmpty()) return stableId;
    }
    // The no-screen case is only an in-memory compatibility state. It is never
    // emitted by a KDE adapter and cannot be used to identify a real output.
    return QStringLiteral("index:%1").arg(screen);
}

int PlaylistManager::screenIndexForKey(const QString& key) const {
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (int index = 0; index < screens.size(); ++index) {
        if (RendererController::stableOutputId(screens.at(index)) == key) return index;
    }
    return -1;
}

Playlist PlaylistManager::current(int screen) const {
    return m_currents.value(screenKey(screen), defaultPlaylist());
}

QVector<Playlist> PlaylistManager::saved() const {
    return m_saved;
}

void PlaylistManager::ensureScreen(int screen) {
    const QString key = screenKey(screen);
    if (m_currents.contains(key)) return;
    m_currents.insert(key, defaultPlaylist());
    scheduleSave();
    rebuildRotator(screen, false);
}

void PlaylistManager::startRotators() {
    stopAllRotators();
    const auto keys = m_currents.keys();
    for (const QString& key : keys) {
        const int screen = screenIndexForKey(key);
        if (screen >= 0) rebuildRotator(screen, true);
    }
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
    const QString key = screenKey(screen);
    if (!m_currents.contains(key)) return {};
    Playlist current = m_currents.value(key);
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

    m_currents[key] = current;
    scheduleSave();
    emit currentChanged(screen);
    emit savedChanged();
    return current;
}

void PlaylistManager::loadSaved(const Playlist& playlist, int screen) {
    const QString key = screenKey(screen);
    Playlist target = playlist;
    target.touch();
    m_currents[key] = target;
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
    const Playlist playlist = current(screen);
    resolved.reserve(playlist.items.size());
    for (const PlaylistItem& item : playlist.items) {
        const Wallpaper wallpaper = resolveWallpaper(item.wallpaperID);
        if (wallpaper.isValid()) resolved.push_back(wallpaper);
    }
    return resolved;
}

Wallpaper PlaylistManager::resolveWallpaper(const QString& id) const {
    if (id.isEmpty()) return {};
    for (const Wallpaper& wallpaper : m_library->loadAll()) {
        if (wallpaper.id() == id) return wallpaper;
    }
    return {};
}

void PlaylistManager::setCurrentWallpaper(int screen, const Wallpaper& wallpaper) {
    m_currentWallpapers.insert(screen, wallpaper);
    if (wallpaper.isValid()) {
        m_lastAppliedIDs.insert(screenKey(screen), wallpaper.id());
        scheduleSave();
    }
}

Wallpaper PlaylistManager::currentWallpaper(int screen) const {
    return m_currentWallpapers.value(screen);
}

QHash<QString, QString> PlaylistManager::lastAppliedIDs() const {
    return m_lastAppliedIDs;
}

void PlaylistManager::load() {
    QFile file(m_storagePath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_currents = {{screenKey(0), defaultPlaylist()}};
        return;
    }

    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        m_currents = {{screenKey(0), defaultPlaylist()}};
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonObject currents = root.value(QStringLiteral("currents")).toObject();
    bool migrated = false;
    for (auto it = currents.begin(); it != currents.end(); ++it) {
        bool ok = false;
        const int screen = it.key().toInt(&ok);
        const QString key = ok ? screenKey(screen) : it.key();
        if (key.isEmpty()) continue;
        migrated = migrated || ok;
        m_currents.insert(key, Playlist::fromJson(it.value().toObject()));
    }
    if (m_currents.isEmpty()) m_currents.insert(screenKey(0), defaultPlaylist());

    const QJsonArray saved = root.value(QStringLiteral("saved")).toArray();
    m_saved.reserve(saved.size());
    for (const auto& value : saved) {
        m_saved.push_back(Playlist::fromJson(value.toObject()));
    }

    const QJsonObject lastApplied = root.value(QStringLiteral("lastApplied")).toObject();
    for (auto it = lastApplied.begin(); it != lastApplied.end(); ++it) {
        bool ok = false;
        const int screen = it.key().toInt(&ok);
        const QString key = ok ? screenKey(screen) : it.key();
        if (key.isEmpty()) continue;
        const QString id = it.value().toString();
        if (!id.isEmpty()) {
            m_lastAppliedIDs.insert(key, id);
            migrated = migrated || ok;
        }
    }

    if (migrated) {
        const QString backupPath = m_storagePath + QStringLiteral(".pre-stable-id");
        if (!QFileInfo::exists(backupPath) && !QFile::copy(m_storagePath, backupPath)) {
            qWarning() << "[Playlist] Cannot preserve legacy playlist file:" << backupPath;
        }
        saveNow();
    }
}

    // Persistence is debounced so bursts of edits (dragging items, changing
    // settings) write once after the UI settles instead of on every mutation.
void PlaylistManager::scheduleSave() {
    m_saveTimer.start();
}

void PlaylistManager::saveNow() {
    if (!QDir().mkpath(Paths::dataDir())) {
        qWarning() << "[Playlist] Cannot create playlist directory:" << Paths::dataDir();
        return;
    }
    QJsonObject currents;
    for (auto it = m_currents.constBegin(); it != m_currents.constEnd(); ++it) {
        currents.insert(it.key(), it.value().toJson());
    }
    QJsonArray saved;
    for (const Playlist& playlist : m_saved) saved.push_back(playlist.toJson());

    QJsonObject lastApplied;
    for (auto it = m_lastAppliedIDs.constBegin(); it != m_lastAppliedIDs.constEnd(); ++it) {
        lastApplied.insert(it.key(), it.value());
    }

    const QJsonObject root{
        {QStringLiteral("currents"), currents},
        {QStringLiteral("saved"), saved},
        {QStringLiteral("lastApplied"), lastApplied},
    };

    // QSaveFile writes a complete replacement beside the old file and renames
    // it only after commit. This prevents a crash or full disk during startup
    // migration from leaving playlists.json truncated and unrecoverable.
    QSaveFile file(m_storagePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[Playlist] Cannot write playlist file:" << m_storagePath;
        return;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        qWarning() << "[Playlist] Cannot complete playlist write:" << m_storagePath;
        return;
    }
    if (!file.commit()) {
        qWarning() << "[Playlist] Cannot commit playlist file:" << m_storagePath;
    }
}

void PlaylistManager::mutateCurrent(int screen, const std::function<void(Playlist&)>& transform) {
    const QString key = screenKey(screen);
    Playlist playlist = m_currents.value(key, defaultPlaylist());
    transform(playlist);
    playlist.touch();
    m_currents[key] = playlist;
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
