#pragma once

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
    QString ageRating;

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
    MostUpvoted,
    PlaytimeTrend,
    TotalPlaytime,
    AveragePlaytimeTrend,
    LifetimeAveragePlaytime,
    SessionsTrend,
    LifetimeSessions,
    LastUpdated,
};

enum class WorkshopAgeRating {
    Everyone,
    Questionable,
    Mature,
};

enum class WorkshopTrendPeriod {
    Day = 1,
    Week = 7,
    Month = 30,
    ThreeMonths = 90,
    SixMonths = 180,
    Year = 365,
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
    int trendDays = 7;
    int ageRatingMask = 1;
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

// 下载状态（对齐 SteamService/WorkshopDownloader 的状态机）。
// connecting/downloading → SteamService 正在下载；resolving → 已下载完成、
// 正在校验展开；completed 时 outputPath 指向项目目录。
enum class DownloadStateKind {
    Queued,
    Starting,
    Connecting,
    Downloading,
    Resolving,
    Completed,
    Failed,
    Cancelled,
};

struct DownloadState {
    DownloadStateKind kind = DownloadStateKind::Queued;
    double percent = -1.0;
    QString message;
    qint64 bytesReceived = 0;
    qint64 totalBytes = 0;
    double bytesPerSecond = 0.0;
    QString outputPath;
};

struct WorkshopDownloadTask {
    WorkshopItem workshopItem;
    DownloadState state;
    QDateTime startedAt;
    QDateTime completedAt;
    DownloadPurpose purpose = DownloadPurpose::Wallpaper;
};

// Steam Workshop 订阅记录（对齐 WorkshopModels.swift 的 WorkshopSubscription）。
struct WorkshopSubscription {
    QString publishedFileId;
    qint64 subscribedAt = 0;  // Unix 秒
    qint64 updatedAt = 0;     // Unix 秒
    QString contentHash;
    qint64 fileSize = 0;
};

struct WorkshopSubscriptionPage {
    int total = 0;
    int startIndex = 0;
    QVector<WorkshopSubscription> items;
};

// "下载全部已订阅壁纸"的确认计划（对齐 macOS WorkshopViewModel.swift 的
// SubscriptionDownloadPlan）：统计订阅总数与剩余未下载数，items 为本次将
// 实际加入下载队列的作品（已安装、队列中活跃、不支持类型均已排除）。
struct SubscriptionDownloadPlan {
    int subscriptionCount = 0;  // 已订阅壁纸总数
    int remainingCount = 0;     // 尚未下载（含队列中）的数量
    QVector<WorkshopItem> items; // 本次待下载列表
};

QString workshopSortLabel(WorkshopSortOrder order);
bool workshopSortUsesTrendPeriod(WorkshopSortOrder order);
QString workshopAgeRatingLabel(WorkshopAgeRating rating);
QString workshopAgeRatingTag(WorkshopAgeRating rating);
QString workshopTrendPeriodLabel(WorkshopTrendPeriod period);
QString workshopTypeLabel(WorkshopTypeFilter filter);
QVector<WorkshopTag> workshopTags();

// ---- 分辨率过滤（对齐 FilterResultsViewModel.swift 的 FR*Resolution OptionSet）----
// 订阅/已安装壁纸的分辨率过滤基于 Steam 创意工坊的 tag 匹配，而非本地视频测量。
// 六组选项，每组每个选项占一个 bit（bit 0 = 第一个选项），"all" = 全部 bit 置位。
enum WorkshopResolutionGroup {
    WorkshopResolutionWidescreen = 0,
    WorkshopResolutionUltraWidescreen,
    WorkshopResolutionDualscreen,
    WorkshopResolutionTriplescreen,
    WorkshopResolutionPortrait,
    WorkshopResolutionMisc,
    WorkshopResolutionGroupCount,
};

// 六组选项的展示标签（供 QML 渲染 checkbox 组，顺序与 bit 位对应）。
struct WorkshopResolutionOptions {
    QStringList widescreen;      // 7 选项
    QStringList ultraWidescreen; // 3 选项
    QStringList dualscreen;      // 4 选项
    QStringList triplescreen;    // 5 选项
    QStringList portrait;        // 5 选项
    QStringList misc;            // 2 选项
};

WorkshopResolutionOptions workshopResolutionOptions();
// 每组"全选"掩码（所有 bit 置位）。用于 QML 的"全选/清空"按钮禁用判断。
int workshopResolutionAllMask(int group);
// 判定 tags 是否命中给定的分辨率过滤（六组掩码，bit 0 为各组第一个选项）。
// 语义对齐 FRResolutionFilter.matches(tags:)：六组全选→true；
// tags 中无任何分辨率 tag→等价于仅选中 misc.otherResolution（bit 0）。
bool workshopResolutionMatches(const QStringList& tags,
                               int widescreen, int ultraWidescreen,
                               int dualscreen, int triplescreen,
                               int portrait, int misc);

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::WorkshopItem)
Q_DECLARE_METATYPE(Mirage::WorkshopQueryResult)
Q_DECLARE_METATYPE(Mirage::WorkshopDownloadTask)
