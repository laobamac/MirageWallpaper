#include "Services/PlaylistRotator.h"

#include "Services/PlaylistManager.h"
#include "Services/PlaylistTransitionOverlay.h"
#include "Services/RendererController.h"
#include "Services/WallpaperLibrary.h"

#include <algorithm>
#include <climits>
#include <QDateTime>
#include <QRandomGenerator>
#include <QtGlobal>

namespace Mirage {

PlaylistRotator::PlaylistRotator(int screen,
                                 PlaylistManager* manager,
                                 RendererController* renderer,
                                 QObject* parent)
    : QObject(parent)
    , m_screen(screen)
    , m_manager(manager)
    , m_renderer(renderer) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &PlaylistRotator::tick);
}

PlaylistRotator::~PlaylistRotator() {
    stop();
}

void PlaylistRotator::start(StartReason reason) {
    rebuild(reason);
}

void PlaylistRotator::rebuild(StartReason reason) {
    QMetaObject::invokeMethod(this, [this, reason] { rebuildOnMain(reason); }, Qt::QueuedConnection);
}

void PlaylistRotator::stop() {
    m_timer.stop();
    m_pendingVideoAdvance = false;
    if (m_observingVideoEnd && m_renderer) {
        disconnect(m_renderer, &RendererController::videoDidEnd, this, &PlaylistRotator::onVideoDidEnd);
        m_observingVideoEnd = false;
    }
}

void PlaylistRotator::rebuildOnMain(StartReason reason) {
    stop();
    if (!m_manager) return;

    const Playlist playlist = m_manager->current(m_screen);
    if (playlist.items.isEmpty()) return;

    if (reason == StartReason::AppLaunch) {
        applyLaunchAnchor(playlist);
    }

    if (playlist.settings.videoSequence && m_renderer) {
        connect(m_renderer, &RendererController::videoDidEnd, this, &PlaylistRotator::onVideoDidEnd,
                Qt::UniqueConnection);
        m_observingVideoEnd = true;
    }

    switch (playlist.settings.timing) {
    case PlaylistTiming::Never:
    case PlaylistTiming::Logon:
        return;
    case PlaylistTiming::Timer:
        scheduleTimer(playlist.settings.timerIntervalSeconds());
        break;
    case PlaylistTiming::Daytime:
        scheduleNextDaytimeAnchor(playlist.settings.daytimeAnchors);
        break;
    case PlaylistTiming::DayOfWeek:
        scheduleNextMidnight();
        applyDayOfWeek(playlist);
        break;
    }
}

void PlaylistRotator::applyLaunchAnchor(const Playlist& playlist) {
    if (m_didHandleLaunch) return;
    m_didHandleLaunch = true;
    const Wallpaper target = firstItemWallpaper(playlist);
    if (!target.isValid()) return;
    if (playlist.settings.introOnStartup || playlist.settings.alwaysBeginFirst) {
        apply(target);
    }
}

Wallpaper PlaylistRotator::firstItemWallpaper(const Playlist& playlist) const {
    if (!m_manager || playlist.items.isEmpty()) return {};
    return m_manager->resolveWallpaper(playlist.items.first().wallpaperID);
}

void PlaylistRotator::scheduleTimer(int seconds) {
    m_timer.setSingleShot(false);
    m_timer.start(qMax(seconds, 30) * 1000);
}

void PlaylistRotator::scheduleNextDaytimeAnchor(const QVector<int>& anchors) {
    m_timer.setSingleShot(true);
    const QDateTime now = QDateTime::currentDateTime();
    QVector<int> sorted = anchors;
    std::sort(sorted.begin(), sorted.end());
    QVector<int> valid;
    for (int hour : sorted) {
        if (hour >= 0 && hour <= 23) valid.push_back(hour);
    }
    if (valid.isEmpty()) return;

    const int currentHour = now.time().hour();
    QDateTime target;
    for (int hour : valid) {
        if (hour > currentHour) {
            target = QDateTime(now.date(), QTime(hour, 0, 0));
            break;
        }
    }
    if (!target.isValid()) {
        const QDate tomorrow = now.date().addDays(1);
        target = QDateTime(tomorrow, QTime(valid.first(), 0, 0));
    }
    const qint64 delayMs = qMax<qint64>(target.toMSecsSinceEpoch() - now.toMSecsSinceEpoch(), 30000);
    m_timer.start(static_cast<int>(qMin<qint64>(delayMs, INT_MAX)));
}

void PlaylistRotator::scheduleNextMidnight() {
    m_timer.setSingleShot(true);
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime next = QDateTime(now.date().addDays(1), QTime(0, 0, 5));
    const qint64 delayMs = qMax<qint64>(next.toMSecsSinceEpoch() - now.toMSecsSinceEpoch(), 30000);
    m_timer.start(static_cast<int>(qMin<qint64>(delayMs, INT_MAX)));
}

void PlaylistRotator::applyDayOfWeek(const Playlist& playlist) {
    // Qt: Monday=1 ... Sunday=7. Map to macOS Calendar weekday-1 (Sun=0 ... Sat=6).
    const int qtDay = QDate::currentDate().dayOfWeek();
    const int today = qtDay % 7; // Sunday -> 0, Monday -> 1, ... Saturday -> 6
    if (today < 0 || today >= qMin(7, playlist.items.size())) return;
    if (!m_manager) return;
    const Wallpaper wallpaper = m_manager->resolveWallpaper(playlist.items.at(today).wallpaperID);
    if (wallpaper.isValid()) apply(wallpaper);
}

void PlaylistRotator::onVideoDidEnd(int screen) {
    if (screen != m_screen || !m_pendingVideoAdvance) return;
    m_pendingVideoAdvance = false;
    advanceNow();
    if (!m_manager) return;
    const auto timing = m_manager->current(m_screen).settings.timing;
    if (timing == PlaylistTiming::Daytime || timing == PlaylistTiming::DayOfWeek) {
        rebuildOnMain(StartReason::ListChanged);
    }
}

void PlaylistRotator::tick() {
    if (!m_manager) return;
    const PlaylistSettings settings = m_manager->current(m_screen).settings;
    const Wallpaper current = m_manager->currentWallpaper(m_screen);
    if (settings.videoSequence && current.kind() == WallpaperKind::Video) {
        // Defer to the video-end signal; onVideoDidEnd rebuilds one-shot schedules.
        m_pendingVideoAdvance = true;
        return;
    }
    advanceNow();
    if (settings.timing == PlaylistTiming::Daytime || settings.timing == PlaylistTiming::DayOfWeek) {
        rebuildOnMain(StartReason::ListChanged);
    }
}

void PlaylistRotator::advanceNow() {
    if (!m_manager) return;
    const Playlist playlist = m_manager->current(m_screen);
    const Wallpaper next = pickNext(playlist);
    if (!next.isValid()) return;
    apply(next);
}

Wallpaper PlaylistRotator::pickNext(const Playlist& playlist) const {
    if (!m_manager || playlist.items.isEmpty()) return {};
    const QVector<Wallpaper> library = m_manager->resolvedItems(m_screen);
    // resolvedItems already filters missing IDs but preserves playlist order via manager helper.
    QHash<QString, Wallpaper> byID;
    QStringList ids;
    ids.reserve(playlist.items.size());
    for (const PlaylistItem& item : playlist.items) {
        ids.push_back(item.wallpaperID);
        if (const Wallpaper wallpaper = m_manager->resolveWallpaper(item.wallpaperID); wallpaper.isValid()) {
            byID.insert(item.wallpaperID, wallpaper);
        }
    }
    QVector<Wallpaper> resolved;
    for (const QString& id : ids) {
        if (byID.contains(id)) resolved.push_back(byID.value(id));
    }
    if (resolved.isEmpty()) return {};

    const QString currentID = m_manager->currentWallpaper(m_screen).isValid()
                                  ? m_manager->currentWallpaper(m_screen).id()
                                  : m_lastPlayedID;
    switch (playlist.settings.order) {
    case PlaylistOrder::Sorted: {
        if (!currentID.isEmpty()) {
            const int idx = ids.indexOf(currentID);
            if (idx >= 0) {
                const QString target = ids.at((idx + 1) % ids.size());
                return byID.value(target, resolved.first());
            }
        }
        return resolved.first();
    }
    case PlaylistOrder::Random: {
        if (resolved.size() == 1) return resolved.first();
        QVector<Wallpaper> pool = resolved;
        if (!currentID.isEmpty()) {
            pool.erase(std::remove_if(pool.begin(), pool.end(),
                                      [&](const Wallpaper& wallpaper) { return wallpaper.id() == currentID; }),
                       pool.end());
        }
        if (pool.isEmpty()) return resolved.first();
        const int index = QRandomGenerator::global()->bounded(pool.size());
        return pool.at(index);
    }
    }
    return resolved.first();
}

void PlaylistRotator::apply(const Wallpaper& wallpaper) {
    if (!m_manager || !wallpaper.isValid()) return;
    m_lastPlayedID = wallpaper.id();
    m_manager->setCurrentWallpaper(m_screen, wallpaper);
    const PlaylistSettings settings = m_manager->current(m_screen).settings;
    const double duration = settings.transition == PlaylistTransitionKind::Disabled
                                ? 0.0
                                : settings.transitionSeconds;
    const int screen = m_screen;
    PlaylistTransitionOverlay::instance().present(screen, duration, settings.transition, [this, wallpaper, screen] {
        if (m_manager) emit m_manager->applyWallpaperRequested(wallpaper, screen);
    });
}

} // namespace Mirage
