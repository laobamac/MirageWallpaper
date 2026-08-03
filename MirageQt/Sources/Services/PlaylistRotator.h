// PlaylistRotator — advances one screen's playlist on its schedule (timer,
// daytime anchors, day of week) or when a video wallpaper ends, then asks
// PlaylistManager to apply the next wallpaper.

#pragma once

#include "Services/PlaylistModels.h"
#include "Services/WEProject.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

namespace Mirage {

class PlaylistManager;
class RendererController;

class PlaylistRotator : public QObject {
    Q_OBJECT

public:
    enum class StartReason {
        AppLaunch,
        ListChanged,
        SettingsChanged,
        ManualAdvance,
    };

    PlaylistRotator(int screen,
                    PlaylistManager* manager,
                    RendererController* renderer,
                    QObject* parent = nullptr);
    ~PlaylistRotator() override;

    void start(StartReason reason);
    void rebuild(StartReason reason);
    void stop();

private slots:
    void onVideoDidEnd(int screen);
    void tick();

private:
    void rebuildOnMain(StartReason reason);
    void applyLaunchAnchor(const Playlist& playlist);
    Wallpaper firstItemWallpaper(const Playlist& playlist) const;
    void scheduleTimer(int seconds);
    void scheduleNextDaytimeAnchor(const QVector<int>& anchors);
    void scheduleNextMidnight();
    void applyDayOfWeek(const Playlist& playlist);
    void advanceNow();
    Wallpaper pickNext(const Playlist& playlist) const;
    void apply(const Wallpaper& wallpaper);

    int m_screen = 0;
    QPointer<PlaylistManager> m_manager;
    QPointer<RendererController> m_renderer;
    QTimer m_timer;
    QString m_lastPlayedID;
    bool m_didHandleLaunch = false;
    bool m_pendingVideoAdvance = false;
    bool m_observingVideoEnd = false;
};

} // namespace Mirage
