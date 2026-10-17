#include "Services/PlaylistModels.h"

#include <QJsonArray>
#include <algorithm>

namespace Mirage {
namespace {

QString dateToString(const QDateTime& date) {
    return date.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime dateFromString(const QString& raw) {
    QDateTime date = QDateTime::fromString(raw, Qt::ISODateWithMs);
    if (!date.isValid()) date = QDateTime::fromString(raw, Qt::ISODate);
    if (!date.isValid()) return QDateTime::currentDateTimeUtc();
    return date.toUTC();
}

QVector<int> intsFromJson(const QJsonArray& array, const QVector<int>& fallback) {
    QVector<int> values;
    values.reserve(array.size());
    for (const auto& item : array) {
        if (item.isDouble()) values.push_back(item.toInt());
    }
    return values.isEmpty() ? fallback : values;
}

} // namespace

PlaylistSettings PlaylistSettings::defaults() {
    return {};
}

int PlaylistSettings::timerIntervalSeconds() const {
    const int total = qMax(timerHours, 0) * 3600 + qMax(timerMinutes, 0) * 60;
    return qMax(total, 30);
}

QJsonObject PlaylistSettings::toJson() const {
    QJsonArray anchors;
    for (int hour : daytimeAnchors) anchors.push_back(hour);
    QJsonArray weekOrder;
    for (int day : dayOfWeekOrder) weekOrder.push_back(day);
    return {
        {QStringLiteral("order"), playlistOrderKey(order)},
        {QStringLiteral("timing"), playlistTimingKey(timing)},
        {QStringLiteral("timerHours"), timerHours},
        {QStringLiteral("timerMinutes"), timerMinutes},
        {QStringLiteral("updateOnPause"), updateOnPause},
        {QStringLiteral("transition"), playlistTransitionKey(transition)},
        {QStringLiteral("transitionSeconds"), transitionSeconds},
        {QStringLiteral("alwaysBeginFirst"), alwaysBeginFirst},
        {QStringLiteral("introOnStartup"), introOnStartup},
        {QStringLiteral("videoSequence"), videoSequence},
        {QStringLiteral("daytimeAnchors"), anchors},
        {QStringLiteral("dayOfWeekOrder"), weekOrder},
    };
}

PlaylistSettings PlaylistSettings::fromJson(const QJsonObject& object) {
    PlaylistSettings settings;
    settings.order = playlistOrderFromKey(object.value(QStringLiteral("order")).toString());
    settings.timing = playlistTimingFromKey(object.value(QStringLiteral("timing")).toString());
    settings.timerHours = object.value(QStringLiteral("timerHours")).toInt(0);
    settings.timerMinutes = object.value(QStringLiteral("timerMinutes")).toInt(30);
    settings.updateOnPause = object.value(QStringLiteral("updateOnPause")).toBool(false);
    settings.transition = playlistTransitionFromKey(object.value(QStringLiteral("transition")).toString());
    settings.transitionSeconds = object.value(QStringLiteral("transitionSeconds")).toDouble(1.0);
    settings.alwaysBeginFirst = object.value(QStringLiteral("alwaysBeginFirst")).toBool(false);
    settings.introOnStartup = object.value(QStringLiteral("introOnStartup")).toBool(false);
    settings.videoSequence = object.value(QStringLiteral("videoSequence")).toBool(false);
    settings.daytimeAnchors = intsFromJson(object.value(QStringLiteral("daytimeAnchors")).toArray(),
                                           {8, 12, 18, 22});
    settings.dayOfWeekOrder = intsFromJson(object.value(QStringLiteral("dayOfWeekOrder")).toArray(),
                                           {0, 1, 2, 3, 4, 5, 6});
    return settings;
}

QJsonObject PlaylistItem::toJson() const {
    return {
        {QStringLiteral("wallpaperID"), wallpaperID},
        {QStringLiteral("addedAt"), dateToString(addedAt)},
    };
}

PlaylistItem PlaylistItem::fromJson(const QJsonObject& object) {
    PlaylistItem item;
    item.wallpaperID = object.value(QStringLiteral("wallpaperID")).toString();
    item.addedAt = dateFromString(object.value(QStringLiteral("addedAt")).toString());
    return item;
}

void Playlist::touch() {
    updatedAt = QDateTime::currentDateTimeUtc();
}

QJsonObject Playlist::toJson() const {
    QJsonArray itemsJson;
    for (const PlaylistItem& item : items) itemsJson.push_back(item.toJson());
    return {
        {QStringLiteral("id"), id.toString(QUuid::WithoutBraces)},
        {QStringLiteral("name"), name},
        {QStringLiteral("items"), itemsJson},
        {QStringLiteral("settings"), settings.toJson()},
        {QStringLiteral("updatedAt"), dateToString(updatedAt)},
    };
}

Playlist Playlist::fromJson(const QJsonObject& object) {
    Playlist playlist;
    const QUuid parsed = QUuid::fromString(object.value(QStringLiteral("id")).toString());
    playlist.id = parsed.isNull() ? QUuid::createUuid() : parsed;
    playlist.name = object.value(QStringLiteral("name")).toString();
    playlist.settings = PlaylistSettings::fromJson(object.value(QStringLiteral("settings")).toObject());
    playlist.updatedAt = dateFromString(object.value(QStringLiteral("updatedAt")).toString());
    const QJsonArray itemsJson = object.value(QStringLiteral("items")).toArray();
    playlist.items.reserve(itemsJson.size());
    for (const auto& value : itemsJson) {
        const PlaylistItem item = PlaylistItem::fromJson(value.toObject());
        if (!item.wallpaperID.isEmpty()) playlist.items.push_back(item);
    }
    return playlist;
}

QString playlistOrderKey(PlaylistOrder order) {
    switch (order) {
    case PlaylistOrder::Random: return QStringLiteral("random");
    case PlaylistOrder::Sorted: return QStringLiteral("sorted");
    }
    return QStringLiteral("sorted");
}

PlaylistOrder playlistOrderFromKey(const QString& key) {
    if (key == QStringLiteral("random")) return PlaylistOrder::Random;
    return PlaylistOrder::Sorted;
}

QString playlistOrderDisplayName(PlaylistOrder order) {
    switch (order) {
    case PlaylistOrder::Random: return QStringLiteral("随机");
    case PlaylistOrder::Sorted: return QStringLiteral("有序");
    }
    return QStringLiteral("有序");
}

QString playlistTimingKey(PlaylistTiming timing) {
    switch (timing) {
    case PlaylistTiming::Timer: return QStringLiteral("timer");
    case PlaylistTiming::Logon: return QStringLiteral("logon");
    case PlaylistTiming::Daytime: return QStringLiteral("daytime");
    case PlaylistTiming::DayOfWeek: return QStringLiteral("dayOfWeek");
    case PlaylistTiming::Never: return QStringLiteral("never");
    }
    return QStringLiteral("timer");
}

PlaylistTiming playlistTimingFromKey(const QString& key) {
    if (key == QStringLiteral("logon")) return PlaylistTiming::Logon;
    if (key == QStringLiteral("daytime")) return PlaylistTiming::Daytime;
    if (key == QStringLiteral("dayOfWeek")) return PlaylistTiming::DayOfWeek;
    if (key == QStringLiteral("never")) return PlaylistTiming::Never;
    return PlaylistTiming::Timer;
}

QString playlistTimingDisplayName(PlaylistTiming timing) {
    switch (timing) {
    case PlaylistTiming::Timer: return QStringLiteral("计时器上");
    case PlaylistTiming::Logon: return QStringLiteral("登录时");
    case PlaylistTiming::Daytime: return QStringLiteral("当日时间");
    case PlaylistTiming::DayOfWeek: return QStringLiteral("星期");
    case PlaylistTiming::Never: return QStringLiteral("从不");
    }
    return QStringLiteral("计时器上");
}

QString playlistTransitionKey(PlaylistTransitionKind kind) {
    switch (kind) {
    case PlaylistTransitionKind::Disabled: return QStringLiteral("disabled");
    case PlaylistTransitionKind::Enabled: return QStringLiteral("enabled");
    case PlaylistTransitionKind::Random: return QStringLiteral("random");
    }
    return QStringLiteral("enabled");
}

PlaylistTransitionKind playlistTransitionFromKey(const QString& key) {
    if (key == QStringLiteral("disabled")) return PlaylistTransitionKind::Disabled;
    if (key == QStringLiteral("random")) return PlaylistTransitionKind::Random;
    return PlaylistTransitionKind::Enabled;
}

QString playlistTransitionDisplayName(PlaylistTransitionKind kind) {
    switch (kind) {
    case PlaylistTransitionKind::Disabled: return QStringLiteral("禁用全部");
    case PlaylistTransitionKind::Enabled: return QStringLiteral("启用全部");
    case PlaylistTransitionKind::Random: return QStringLiteral("随机");
    }
    return QStringLiteral("启用全部");
}

} // namespace Mirage
