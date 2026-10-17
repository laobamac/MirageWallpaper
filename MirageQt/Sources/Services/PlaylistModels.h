#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QUuid>
#include <QVector>

namespace Mirage {

enum class PlaylistOrder {
    Sorted,
    Random,
};

enum class PlaylistTiming {
    Timer,
    Logon,
    Daytime,
    DayOfWeek,
    Never,
};

enum class PlaylistTransitionKind {
    Disabled,
    Enabled,
    Random,
};

struct PlaylistSettings {
    PlaylistOrder order = PlaylistOrder::Sorted;
    PlaylistTiming timing = PlaylistTiming::Timer;
    int timerHours = 0;
    int timerMinutes = 30;
    bool updateOnPause = false;
    PlaylistTransitionKind transition = PlaylistTransitionKind::Enabled;
    double transitionSeconds = 1.0;
    bool alwaysBeginFirst = false;
    bool introOnStartup = false;
    bool videoSequence = false;
    QVector<int> daytimeAnchors{8, 12, 18, 22};
    QVector<int> dayOfWeekOrder{0, 1, 2, 3, 4, 5, 6};

    static PlaylistSettings defaults();
    int timerIntervalSeconds() const;

    QJsonObject toJson() const;
    static PlaylistSettings fromJson(const QJsonObject& object);
    bool operator==(const PlaylistSettings&) const = default;
};

struct PlaylistItem {
    QString wallpaperID;
    QDateTime addedAt = QDateTime::currentDateTimeUtc();

    QString id() const { return wallpaperID; }

    QJsonObject toJson() const;
    static PlaylistItem fromJson(const QJsonObject& object);
    bool operator==(const PlaylistItem&) const = default;
};

struct Playlist {
    QUuid id = QUuid::createUuid();
    QString name;
    QVector<PlaylistItem> items;
    PlaylistSettings settings = PlaylistSettings::defaults();
    QDateTime updatedAt = QDateTime::currentDateTimeUtc();

    void touch();

    QJsonObject toJson() const;
    static Playlist fromJson(const QJsonObject& object);
    bool operator==(const Playlist&) const = default;
};

QString playlistOrderKey(PlaylistOrder order);
PlaylistOrder playlistOrderFromKey(const QString& key);
QString playlistOrderDisplayName(PlaylistOrder order);

QString playlistTimingKey(PlaylistTiming timing);
PlaylistTiming playlistTimingFromKey(const QString& key);
QString playlistTimingDisplayName(PlaylistTiming timing);

QString playlistTransitionKey(PlaylistTransitionKind kind);
PlaylistTransitionKind playlistTransitionFromKey(const QString& key);
QString playlistTransitionDisplayName(PlaylistTransitionKind kind);

} // namespace Mirage
