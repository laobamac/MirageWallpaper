// PlaylistManager — per-screen playlists: current/saved state, debounced JSON
// persistence, and the owning factory for the per-screen PlaylistRotator.

#pragma once

#include "Services/PlaylistModels.h"
#include "Services/WEProject.h"

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <functional>

namespace Mirage {

class PlaylistRotator;
class RendererController;
class WallpaperLibrary;

class PlaylistManager : public QObject {
    Q_OBJECT

public:
    explicit PlaylistManager(WallpaperLibrary* library,
                             RendererController* renderer,
                             QObject* parent = nullptr);
    ~PlaylistManager() override;

    Playlist current(int screen) const;
    QVector<Playlist> saved() const;

    void ensureScreen(int screen);
    void startRotators();
    void kickRotator(int screen);
    void stopAllRotators();

    void add(const Wallpaper& wallpaper, int screen);
    void remove(const QString& itemID, int screen);
    void move(int source, int destination, int screen);
    void clear(int screen);
    void trimItems(int limit, int screen);
    void updateSettings(int screen, const std::function<void(PlaylistSettings&)>& transform);
    void resetSettings(int screen);

    Playlist saveAs(const QString& name, int screen);
    void loadSaved(const Playlist& playlist, int screen);
    void deleteSaved(const QUuid& id);

    QVector<Wallpaper> resolvedItems(int screen) const;
    Wallpaper resolveWallpaper(const QString& id) const;
    void setCurrentWallpaper(int screen, const Wallpaper& wallpaper);
    Wallpaper currentWallpaper(int screen) const;
    // Persisted "last applied wallpaper" per screen, used to resume playback
    // on app launch (mirrors the macOS app's CurrentWallpaper restore).
    QHash<int, QString> lastAppliedIDs() const;

signals:
    void currentChanged(int screen);
    void savedChanged();
    void applyWallpaperRequested(const Mirage::Wallpaper& wallpaper, int screen);

private:
    friend class PlaylistRotator;

    struct Persisted {
        QHash<int, Playlist> currents;
        QVector<Playlist> saved;
    };

    void load();
    void scheduleSave();
    void saveNow();
    void mutateCurrent(int screen, const std::function<void(Playlist&)>& transform);
    Playlist defaultPlaylist() const;
    void rebuildRotator(int screen, bool appLaunch);

    WallpaperLibrary* m_library = nullptr;
    RendererController* m_renderer = nullptr;
    QHash<int, Playlist> m_currents;
    QVector<Playlist> m_saved;
    QHash<int, Wallpaper> m_currentWallpapers;
    QHash<int, QString> m_lastAppliedIDs;
    QHash<int, PlaylistRotator*> m_rotators;
    QTimer m_saveTimer;
    QString m_storagePath;
};

} // namespace Mirage
