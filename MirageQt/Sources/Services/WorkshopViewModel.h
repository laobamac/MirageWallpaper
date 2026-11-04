#pragma once

#include "Services/SteamWebAPI.h"
#include "Services/WallpaperLibrary.h"

#include <QObject>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>
#include <QTimer>

#include <optional>

namespace Mirage {

class SteamServiceManager;

enum class DiscoverCollection {
    Trending,
    MostRecent,
    MostSubscribed,
    TopRated,
    MostUpvoted,
    LastUpdated,
    PlaytimeTrend,
    AveragePlaytimeTrend,
    SessionsTrend,
    TotalPlaytime,
    LifetimeAveragePlaytime,
    LifetimeSessions,
    Anime,
    Nature,
    Abstract,
    Landscape,
};

enum class SteamSetupState {
    NeedsLogin,  // Steam 未登录或服务不可用
    Ready,       // 已登录，可下载
};

struct InstalledWorkshopState {
    QSet<QString> installedIds;
    QSet<QString> presetsNeedingDependency;
};

class WorkshopViewModel : public QObject {
    Q_OBJECT

public:
    explicit WorkshopViewModel(SteamWebAPI* api,
                               SteamServiceManager* service,
                               WallpaperLibrary* library,
                               QObject* parent = nullptr);

    const QVector<WorkshopItem>& items() const;
    const QVector<WorkshopItem>& discoverItems(DiscoverCollection collection) const;
    const QVector<WorkshopItem>& bannerItems() const;
    const QVector<WorkshopDownloadTask>& downloadQueue() const;
    const std::optional<WorkshopItem>& selectedItem() const;

    QString searchText() const;
    const QSet<QString>& selectedTags() const;
    WorkshopSortOrder sortOrder() const;
    WorkshopTypeFilter typeFilter() const;
    int ageRatingMask() const;
    int trendDays() const;
    int discoverTrendDays() const;
    int currentPage() const;
    int totalPages() const;
    bool isLoading() const;
    bool isDiscoverLoading() const;
    QString error() const;
    SteamSetupState steamSetupState() const;
    QString steamSetupSummary() const;
    int activeDownloadCount() const;

    bool isItemDownloaded(const QString& workshopId) const;
    bool presetNeedsDependency(const QString& workshopId) const;
    std::optional<Wallpaper> installedItem(const QString& workshopId) const;
    std::optional<DownloadState> downloadStateFor(const QString& workshopId) const;

    // 订阅管理（对齐上游 WorkshopViewModel.swift 的 subscriptionItems / 过滤）。
    const QVector<WorkshopItem>& subscriptions() const;
    int subscriptionTotal() const;
    int subscriptionCurrentPage() const;
    int subscriptionPageCount() const;
    bool isSubscriptionsLoading() const;
    // 订阅过滤状态（供 QML 渲染 checkbox 与重置按钮）。
    QString subscriptionSearchText() const;
    WorkshopTypeFilter subscriptionTypeFilter() const;
    int subscriptionAgeRatingMask() const;
    int subscriptionWidescreenMask() const;
    int subscriptionUltraWidescreenMask() const;
    int subscriptionDualscreenMask() const;
    int subscriptionTriplescreenMask() const;
    int subscriptionPortraitMask() const;
    int subscriptionMiscMask() const;
    const QSet<QString>& subscriptionSelectedTags() const;
    bool hasActiveSubscriptionFilters() const;
    // "下载全部已订阅壁纸"状态（对齐 macOS subscriptionDownloadPlan /
    // isPreparingSubscriptionDownloads）。
    bool isPreparingSubscriptionDownloads() const;
    const std::optional<SubscriptionDownloadPlan>& subscriptionDownloadPlan() const;

public slots:
    void setSearchText(const QString& text);
    void submitSearch();
    void setSortOrder(Mirage::WorkshopSortOrder order);
    void setTrendDays(int days);
    void setDiscoverTrendDays(int days);
    void setTypeFilter(Mirage::WorkshopTypeFilter filter);
    void setAgeRatingEnabled(Mirage::WorkshopAgeRating rating, bool enabled);
    void toggleTag(const QString& tag);
    void selectAllTags();
    void clearTags();
    void clearFilters();
    // 将创意工坊浏览切换到指定页。page 会按当前总页数限制在
    // 有效范围内；目标页与当前页相同时不发起网络请求。本对象及
    // SteamWebAPI 均属于 GUI 线程，调用方必须在 GUI 线程调用。
    void goToPage(int page);
    void search();
    void loadDiscover();
    void refreshDiscover();
    void reloadOnlineContent();
    void checkSteamSetup();
    void logout();

    // 订阅管理。
    void loadSubscriptions(int startIndex = 0);
    void goToSubscriptionPage(int page);
    void subscribe(const QString& workshopId);
    void unsubscribe(const QString& workshopId);

    // 订阅过滤（对齐 SubscribedWorkshopFilterSidebar 的 setter）。
    void setSubscriptionSearchText(const QString& text);
    void setSubscriptionTypeFilter(Mirage::WorkshopTypeFilter filter);
    void setSubscriptionAgeRatingEnabled(Mirage::WorkshopAgeRating rating, bool enabled);
    void setSubscriptionResolutionOption(int group, int bit, bool enabled);
    void selectAllSubscriptionResolutions();
    void clearSubscriptionResolutions();
    void selectAllSubscriptionTags();
    void clearSubscriptionTags();
    void toggleSubscriptionTag(const QString& tag);
    void clearSubscriptionFilters();
    // "下载全部已订阅壁纸"：生成确认计划 → confirm 后逐项入队下载
    // （对齐 macOS downloadAllSubscriptions / confirmSubscriptionDownloads）。
    void downloadAllSubscriptions();
    void confirmSubscriptionDownloads();
    void dismissSubscriptionDownloadPlan();

    void selectWorkshopItem(const Mirage::WorkshopItem& item);
    void downloadItem(const Mirage::WorkshopItem& item,
                      Mirage::DownloadPurpose purpose = Mirage::DownloadPurpose::Wallpaper);
    // 仅凭 workshopId 下载（订阅列表等无完整详情场景）。
    void downloadItemById(const QString& workshopId);
    void cancelDownload(const QString& workshopId);
    void retryDownload(const QString& workshopId);
    void clearCompletedDownloads();
    void requestPresetDependency(const QString& workshopId);
    void confirmPresetDependencyDownload(const QString& presetId,
                                         const QString& dependencyId,
                                         const Mirage::WorkshopItem& dependencyItem);

    void navigateToWorkshopWithTag(const QString& tag);
    void navigateToWorkshopWithSort(Mirage::WorkshopSortOrder order);

signals:
    void browseChanged();
    void discoverChanged();
    void filtersChanged();
    void selectedItemChanged(const Mirage::WorkshopItem& item, bool hasSelection);
    void installedWallpaperRequested(const Mirage::Wallpaper& wallpaper);
    void presetDependencyRequested(const QString& presetId,
                                   const QString& presetTitle,
                                   const QString& dependencyId,
                                   const Mirage::WorkshopItem& dependencyItem);
    void downloadQueueChanged();
    void installedStateChanged();
    void steamSetupChanged();
    void steamSetupRequested();
    void navigateToWorkshopRequested();
    void workshopItemDownloaded(const QString& workshopId);
    void subscriptionsChanged();

private:
    struct PendingDependency {
        QString presetId;
        QString presetTitle;
        QString dependencyId;
    };

    void handleQueryFinished(quint64 requestId, const WorkshopQueryResult& result);
    void handleDetailsFinished(quint64 requestId,
                               const QVector<WorkshopItem>& items,
                               const QString& error);
    // SteamServiceManager::downloadStateChanged 信号（taskId=workshopId）。
    void handleDownloadState(const QString& taskId, DownloadStateKind kind,
                             qint64 receivedBytes, qint64 totalBytes, double bytesPerSecond,
                             const QString& outputPath, const QString& message);
    void refreshSteamSetupState();
    void processDownloadQueue();
    void handleCompletedDownload(const QString& workshopId);
    void refreshInstalledState();
    void cancelDiscoverRequests();
    void setSelectedItem(const std::optional<WorkshopItem>& item);
    void issueDiscoverRequest(DiscoverCollection collection,
                              WorkshopSortOrder order,
                              const QString& tag,
                              int count,
                              int trendDays = 7);
    // 订阅全量加载：分页拉取订阅记录 → 批量加载详情 → 客户端过滤分页。
    void fetchSubscriptionPage(int serviceStart, int requestedStart);
    void startSubscriptionDetailsLoad();
    void requestNextSubscriptionDetailBatch();
    void finishSubscriptionLoad();
    void rebuildSubscriptionPage(int startIndex);
    bool matchesSubscriptionFilters(const WorkshopItem& item) const;
    // "下载全部"：用已加载的订阅目录生成确认计划（排除已安装/队列活跃/不支持）。
    void buildSubscriptionDownloadPlan();

    SteamWebAPI* m_api = nullptr;
    SteamServiceManager* m_steamService = nullptr;
    WallpaperLibrary* m_library = nullptr;

    QVector<WorkshopItem> m_items;
    QHash<DiscoverCollection, QVector<WorkshopItem>> m_discoverItems;
    QHash<DiscoverCollection, QVector<WorkshopItem>> m_pendingDiscoverItems;
    QVector<WorkshopItem> m_bannerItems;
    QVector<WorkshopDownloadTask> m_downloadQueue;
    std::optional<WorkshopItem> m_selectedItem;

    QString m_searchText;
    QSet<QString> m_selectedTags;
    WorkshopSortOrder m_sortOrder = WorkshopSortOrder::Trending;
    WorkshopTypeFilter m_typeFilter = WorkshopTypeFilter::All;
    int m_ageRatingMask = 1;
    int m_trendDays = 7;
    int m_discoverTrendDays = 7;
    int m_currentPage = 1;
    int m_totalItems = 0;
    bool m_isLoading = false;
    bool m_isDiscoverLoading = false;
    QString m_error;
    SteamSetupState m_steamSetupState = SteamSetupState::NeedsLogin;

    // 订阅状态：全量详情（catalog）+ 当前页（过滤后，对齐 upstream subscriptionCatalogItems/subscriptionItems）。
    QVector<WorkshopItem> m_subscriptionCatalogItems;
    QVector<WorkshopItem> m_subscriptionItems;
    int m_subscriptionTotal = 0;
    int m_subscriptionStartIndex = 0;
    bool m_subscriptionsLoading = false;

    // 订阅过滤状态（对齐 upstream subscription* 属性；默认全选）。
    QString m_subscriptionSearchText;
    WorkshopTypeFilter m_subscriptionTypeFilter = WorkshopTypeFilter::All;
    int m_subscriptionAgeRatingMask = 0x7;  // 默认 .all（Everyone|Questionable|Mature）
    int m_subscriptionWidescreen = 0x7F;
    int m_subscriptionUltraWidescreen = 0x07;
    int m_subscriptionDualscreen = 0x0F;
    int m_subscriptionTriplescreen = 0x1F;
    int m_subscriptionPortrait = 0x1F;
    int m_subscriptionMisc = 0x03;
    QSet<QString> m_subscriptionSelectedTags;

    // "下载全部已订阅壁纸"：准备中标志 + 待确认计划 + 目录为空时
    // 等待 loadSubscriptions 完成后再生成计划的延续标志。
    bool m_isPreparingSubscriptionDownloads = false;
    std::optional<SubscriptionDownloadPlan> m_subscriptionDownloadPlan;
    bool m_buildPlanAfterSubscriptionLoad = false;

    // 订阅加载中间状态：按订阅顺序累积 id + 分批详情请求（避免与依赖详情请求混淆）。
    QStringList m_pendingSubscriptionIds;
    QSet<QString> m_seenSubscriptionIds;
    int m_requestedSubscriptionStart = 0;
    QVector<QStringList> m_pendingSubscriptionDetailBatches;
    QVector<WorkshopItem> m_pendingSubscriptionDetails;
    QSet<quint64> m_subscriptionDetailRequests;

    QTimer m_searchDebounce;
    QFutureWatcher<InstalledWorkshopState> m_installedStateWatcher;
    QSet<QString> m_installedWorkshopIds;
    QSet<QString> m_presetsNeedingDependency;
    bool m_installedStateRefreshPending = false;
    quint64 m_searchRequestId = 0;
    QHash<quint64, DiscoverCollection> m_discoverRequests;
    QHash<quint64, PendingDependency> m_dependencyRequests;
    QString m_pendingPresetId;
    QString m_pendingDependencyId;
};

} // namespace Mirage

Q_DECLARE_METATYPE(Mirage::WorkshopSortOrder)
Q_DECLARE_METATYPE(Mirage::WorkshopTypeFilter)
