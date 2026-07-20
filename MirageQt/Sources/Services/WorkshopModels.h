#pragma once

#include "Services/SteamCMDManager.h"
#include "Services/WEProject.h"

#include <QDateTime>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace Mirage {

struct WorkshopItem {
    QString publishedFileId;
    QString title;
    QString description;
    QUrl previewImageUrl;
    QStringList tags;
    int subscriptions = 0;
    int favorited = 0;
    int views = 0;
    qint64 fileSize = 0;
    QDateTime timeCreated;
    QDateTime timeUpdated;
    QString creatorSteamId;
    QString wallpaperType = QStringLiteral("scene");

    WallpaperKind kind() const;
    bool isPreset() const;
    QString displayTypeName() const;
    QString formattedFileSize() const;
    QString formattedSubscriptions() const;
    QString formattedFavorited() const;
    QString formattedViews() const;

    static WorkshopItem dependencyPlaceholder(const QString& id);
};

enum class WorkshopSortOrder {
    Trending,
    MostRecent,
    MostSubscribed,
    TopRated,
};

enum class WorkshopTypeFilter {
    All,
    Scene,
    Web,
    Video,
    Preset,
};

struct WorkshopTag {
    QString value;
    QString displayName;
    QString iconName;
};

struct WorkshopQuery {
    QString searchText;
    QStringList tags;
    WorkshopSortOrder sortOrder = WorkshopSortOrder::Trending;
    WorkshopTypeFilter typeFilter = WorkshopTypeFilter::All;
    int page = 1;
    int perPage = 30;
};

struct WorkshopQueryResult {
    QVector<WorkshopItem> items;
    int total = 0;
    QString error;
};

enum class DownloadPurpose {
    Wallpaper,
    PresetDependency,
};

struct WorkshopDownloadTask {
    WorkshopItem workshopItem;
    DownloadState state;
    QDateTime startedAt;
    QDateTime completedAt;
    DownloadPurpose purpose = DownloadPurpose::Wallpaper;
};

QString workshopSortLabel(WorkshopSortOrder order);
QString workshopTypeLabel(WorkshopTypeFilter filter);
QVector<WorkshopTag> workshopTags();

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::WorkshopItem)
Q_DECLARE_METATYPE(Mirage::WorkshopQueryResult)
Q_DECLARE_METATYPE(Mirage::WorkshopDownloadTask)
