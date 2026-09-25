#include "Services/WorkshopViewModel.h"

#include "Services/Paths.h"
#include "Services/SteamServiceManager.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTimer>

namespace Mirage {
namespace {

// 三个方形网格的固定分页协议。Steam QueryFiles 也必须使用
// 同一容量，保证服务端页码、总页数与本地订阅切片边界一致。
constexpr int kItemsPerPage = 50;

bool isActive(const DownloadState& state) {
    return state.kind == DownloadStateKind::Starting ||
           state.kind == DownloadStateKind::Connecting ||
           state.kind == DownloadStateKind::Downloading ||
           state.kind == DownloadStateKind::Resolving;
}

InstalledWorkshopState scanInstalledWorkshopState(const QStringList& sources) {
    InstalledWorkshopState state;
    QHash<QString, Project> projects;

    for (const QString& source : sources) {
        const QDir directory(source);
        if (!directory.exists()) continue;
        const QFileInfoList entries = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QFileInfo& entry : entries) {
            QFile file(entry.filePath() + "/project.json");
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
            if (!document.isObject()) continue;

            const Project project = Project::fromJson(document.object());
            const QString id = project.workshopId.isEmpty() ? entry.fileName() : project.workshopId;
            if (id.isEmpty() || projects.contains(id)) continue;
            projects.insert(id, project);
            state.installedIds.insert(id);
        }
    }

    for (auto it = projects.cbegin(); it != projects.cend(); ++it) {
        const Project& project = it.value();
        if (project.isWorkshopPreset() &&
            (project.dependency.isEmpty() || !state.installedIds.contains(project.dependency))) {
            state.presetsNeedingDependency.insert(it.key());
        }
    }
    return state;
}

} // namespace

WorkshopViewModel::WorkshopViewModel(SteamWebAPI* api,
                                     SteamServiceManager* service,
                                     WallpaperLibrary* library,
                                     QObject* parent)
    : QObject(parent)
    , m_api(api)
    , m_steamService(service)
    , m_library(library) {
    qRegisterMetaType<Mirage::WorkshopDownloadTask>();
    qRegisterMetaType<Mirage::WorkshopSortOrder>();
    qRegisterMetaType<Mirage::WorkshopTypeFilter>();

    m_searchDebounce.setSingleShot(true);
    m_searchDebounce.setInterval(500);
    m_ageRatingMask = QSettings().value(QStringLiteral("Workshop/AgeRatingMask"), 1).toInt() & 0x7;
    connect(&m_searchDebounce, &QTimer::timeout, this, &WorkshopViewModel::search);
    connect(m_api, &SteamWebAPI::queryFinished, this, &WorkshopViewModel::handleQueryFinished);
    connect(m_api, &SteamWebAPI::detailsFinished, this, &WorkshopViewModel::handleDetailsFinished);
    connect(m_steamService, &SteamServiceManager::downloadStateChanged, this,
            &WorkshopViewModel::handleDownloadState);
    connect(m_steamService, &SteamServiceManager::authenticationChanged, this, [this] {
        refreshSteamSetupState();
        processDownloadQueue();
    });
    connect(m_steamService, &SteamServiceManager::serviceStateChanged, this, [this] {
        refreshSteamSetupState();
    });
    connect(m_library, &WallpaperLibrary::libraryChanged, this, &WorkshopViewModel::refreshInstalledState);
    connect(&m_installedStateWatcher, &QFutureWatcher<InstalledWorkshopState>::finished, this, [this] {
        const InstalledWorkshopState state = m_installedStateWatcher.result();
        const bool changed = m_installedWorkshopIds != state.installedIds ||
                             m_presetsNeedingDependency != state.presetsNeedingDependency;
        m_installedWorkshopIds = state.installedIds;
        m_presetsNeedingDependency = state.presetsNeedingDependency;
        if (changed) emit installedStateChanged();
        if (m_installedStateRefreshPending) {
            m_installedStateRefreshPending = false;
            refreshInstalledState();
        }
    });
    refreshSteamSetupState();
    refreshInstalledState();
}

const QVector<WorkshopItem>& WorkshopViewModel::items() const { return m_items; }

const QVector<WorkshopItem>& WorkshopViewModel::discoverItems(DiscoverCollection collection) const {
    static const QVector<WorkshopItem> empty;
    const auto it = m_discoverItems.constFind(collection);
    return it == m_discoverItems.cend() ? empty : it.value();
}

const QVector<WorkshopItem>& WorkshopViewModel::bannerItems() const { return m_bannerItems; }
const QVector<WorkshopDownloadTask>& WorkshopViewModel::downloadQueue() const { return m_downloadQueue; }
const std::optional<WorkshopItem>& WorkshopViewModel::selectedItem() const { return m_selectedItem; }
QString WorkshopViewModel::searchText() const { return m_searchText; }
const QSet<QString>& WorkshopViewModel::selectedTags() const { return m_selectedTags; }
WorkshopSortOrder WorkshopViewModel::sortOrder() const { return m_sortOrder; }
WorkshopTypeFilter WorkshopViewModel::typeFilter() const { return m_typeFilter; }
int WorkshopViewModel::ageRatingMask() const { return m_ageRatingMask; }
int WorkshopViewModel::trendDays() const { return m_trendDays; }
int WorkshopViewModel::discoverTrendDays() const { return m_discoverTrendDays; }
int WorkshopViewModel::currentPage() const { return m_currentPage; }
int WorkshopViewModel::totalPages() const { return qMax(1, (m_totalItems + kItemsPerPage - 1) / kItemsPerPage); }
bool WorkshopViewModel::isLoading() const { return m_isLoading; }
bool WorkshopViewModel::isDiscoverLoading() const { return m_isDiscoverLoading; }
QString WorkshopViewModel::error() const { return m_error; }
SteamSetupState WorkshopViewModel::steamSetupState() const { return m_steamSetupState; }

QString WorkshopViewModel::steamSetupSummary() const {
    switch (m_steamSetupState) {
    case SteamSetupState::NeedsLogin: {
        // 服务不可用（二进制缺失/未连接）与未登录统一视为需要 Steam 会话。
        if (! m_steamService->isRunning()) {
            return QStringLiteral("需要 Steam 服务（SteamService 未运行）");
        }
        return QStringLiteral("需要有效的 Steam 会话");
    }
    case SteamSetupState::Ready: return QStringLiteral("已登录 %1").arg(m_steamService->accountName());
    }
    return {};
}

int WorkshopViewModel::activeDownloadCount() const {
    int count = 0;
    for (const WorkshopDownloadTask& task : m_downloadQueue) {
        if (isActive(task.state)) ++count;
    }
    return count;
}

std::optional<Wallpaper> WorkshopViewModel::installedItem(const QString& workshopId) const {
    const QStringList directories = m_library->workshopItemDirectories(workshopId);
    std::optional<Wallpaper> first;
    std::optional<Wallpaper> preset;
    for (const QString& directory : directories) {
        const Wallpaper wallpaper = m_library->loadWallpaper(directory);
        if (!first) first = wallpaper;
        if (wallpaper.isValid()) return wallpaper;
        if (wallpaper.isPreset() && !preset) preset = wallpaper;
    }
    return preset ? preset : first;
}

bool WorkshopViewModel::isItemDownloaded(const QString& workshopId) const {
    return m_installedWorkshopIds.contains(workshopId);
}

bool WorkshopViewModel::presetNeedsDependency(const QString& workshopId) const {
    return m_presetsNeedingDependency.contains(workshopId);
}

std::optional<DownloadState> WorkshopViewModel::downloadStateFor(const QString& workshopId) const {
    for (const WorkshopDownloadTask& task : m_downloadQueue) {
        if (task.workshopItem.publishedFileId == workshopId) return task.state;
    }
    return std::nullopt;
}

void WorkshopViewModel::setSearchText(const QString& text) {
    if (m_searchText == text) return;
    m_searchText = text;
    m_currentPage = 1;
    emit filtersChanged();
    m_searchDebounce.start();
}

void WorkshopViewModel::submitSearch() {
    m_searchDebounce.stop();
    m_currentPage = 1;
    search();
}

void WorkshopViewModel::setSortOrder(WorkshopSortOrder order) {
    if (m_sortOrder == order) return;
    m_sortOrder = order;
    m_currentPage = 1;
    emit filtersChanged();
    search();
}

void WorkshopViewModel::setTrendDays(int days) {
    const int bounded = qBound(1, days, 365);
    if (m_trendDays == bounded) return;
    m_trendDays = bounded;
    m_currentPage = 1;
    emit filtersChanged();
    if (workshopSortUsesTrendPeriod(m_sortOrder)) search();
}

void WorkshopViewModel::setDiscoverTrendDays(int days) {
    const int bounded = qBound(1, days, 365);
    if (m_discoverTrendDays == bounded) return;
    m_discoverTrendDays = bounded;
    refreshDiscover();
}

void WorkshopViewModel::setTypeFilter(WorkshopTypeFilter filter) {
    if (m_typeFilter == filter) return;
    m_typeFilter = filter;
    m_currentPage = 1;
    emit filtersChanged();
    search();
}

void WorkshopViewModel::setAgeRatingEnabled(WorkshopAgeRating rating, bool enabled) {
    const int bit = 1 << static_cast<int>(rating);
    const int updated = enabled ? (m_ageRatingMask | bit) : (m_ageRatingMask & ~bit);
    if (updated == m_ageRatingMask) return;
    m_ageRatingMask = updated;
    QSettings().setValue(QStringLiteral("Workshop/AgeRatingMask"), m_ageRatingMask);
    m_currentPage = 1;
    emit filtersChanged();
    search();
}

void WorkshopViewModel::toggleTag(const QString& tag) {
    if (m_selectedTags.contains(tag)) m_selectedTags.remove(tag);
    else m_selectedTags.insert(tag);
    m_currentPage = 1;
    emit filtersChanged();
    search();
}

void WorkshopViewModel::selectAllTags() {
    m_selectedTags.clear();
    for (const WorkshopTag& tag : workshopTags()) m_selectedTags.insert(tag.value);
    m_currentPage = 1;
    emit filtersChanged();
    search();
}

void WorkshopViewModel::clearTags() {
    if (m_selectedTags.isEmpty()) return;
    m_selectedTags.clear();
    m_currentPage = 1;
    emit filtersChanged();
    search();
}

void WorkshopViewModel::clearFilters() {
    m_searchDebounce.stop();
    m_searchText.clear();
    m_selectedTags.clear();
    m_sortOrder = WorkshopSortOrder::Trending;
    m_typeFilter = WorkshopTypeFilter::All;
    m_ageRatingMask = 1;
    m_trendDays = 7;
    QSettings().setValue(QStringLiteral("Workshop/AgeRatingMask"), m_ageRatingMask);
    m_currentPage = 1;
    emit filtersChanged();
    search();
}

void WorkshopViewModel::goToPage(int page) {
    // 所有页码按钮和可编辑页码框都经过此唯一入口，使边界约束
    // 与搜索请求时机保持一致，不需要为前后翻页维护平行接口。
    const int target = qBound(1, page, totalPages());
    if (target == m_currentPage) return;
    m_currentPage = target;
    emit filtersChanged();
    search();
}

void WorkshopViewModel::search() {
    WorkshopQuery query;
    query.searchText = m_searchText;
    query.tags = m_selectedTags.values();
    query.sortOrder = m_sortOrder;
    query.typeFilter = m_typeFilter;
    query.trendDays = m_trendDays;
    query.ageRatingMask = m_ageRatingMask;
    query.page = m_currentPage;
    query.perPage = kItemsPerPage;

    if (m_searchRequestId != 0) m_api->cancelQuery(m_searchRequestId);
    m_isLoading = true;
    m_error.clear();
    emit browseChanged();
    m_searchRequestId = m_api->queryFiles(query);
}

void WorkshopViewModel::loadDiscover() {
    if (m_isDiscoverLoading) return;
    m_isDiscoverLoading = true;
    m_discoverRequests.clear();
    m_pendingDiscoverItems.clear();
    emit discoverChanged();

    issueDiscoverRequest(DiscoverCollection::Trending, WorkshopSortOrder::Trending, {}, 15, m_discoverTrendDays);
    issueDiscoverRequest(DiscoverCollection::MostUpvoted, WorkshopSortOrder::MostUpvoted, {}, 10);
    issueDiscoverRequest(DiscoverCollection::MostRecent, WorkshopSortOrder::MostRecent, {}, 10);
    issueDiscoverRequest(DiscoverCollection::MostSubscribed, WorkshopSortOrder::MostSubscribed, {}, 10);
    issueDiscoverRequest(DiscoverCollection::TopRated, WorkshopSortOrder::TopRated, {}, 10);
    issueDiscoverRequest(DiscoverCollection::LastUpdated, WorkshopSortOrder::LastUpdated, {}, 10);
    issueDiscoverRequest(DiscoverCollection::PlaytimeTrend, WorkshopSortOrder::PlaytimeTrend, {}, 10, m_discoverTrendDays);
    issueDiscoverRequest(DiscoverCollection::AveragePlaytimeTrend, WorkshopSortOrder::AveragePlaytimeTrend, {}, 10, m_discoverTrendDays);
    issueDiscoverRequest(DiscoverCollection::SessionsTrend, WorkshopSortOrder::SessionsTrend, {}, 10, m_discoverTrendDays);
    issueDiscoverRequest(DiscoverCollection::TotalPlaytime, WorkshopSortOrder::TotalPlaytime, {}, 10);
    issueDiscoverRequest(DiscoverCollection::LifetimeAveragePlaytime, WorkshopSortOrder::LifetimeAveragePlaytime, {}, 10);
    issueDiscoverRequest(DiscoverCollection::LifetimeSessions, WorkshopSortOrder::LifetimeSessions, {}, 10);
    issueDiscoverRequest(DiscoverCollection::Anime, WorkshopSortOrder::Trending, QStringLiteral("Anime"), 10);
    issueDiscoverRequest(DiscoverCollection::Nature, WorkshopSortOrder::Trending, QStringLiteral("Nature"), 10);
    issueDiscoverRequest(DiscoverCollection::Abstract, WorkshopSortOrder::Trending, QStringLiteral("Abstract"), 10);
    issueDiscoverRequest(DiscoverCollection::Landscape, WorkshopSortOrder::Trending, QStringLiteral("Landscape"), 10);
}

void WorkshopViewModel::reloadOnlineContent() {
    cancelDiscoverRequests();
    m_discoverItems.clear();
    m_pendingDiscoverItems.clear();
    m_bannerItems.clear();
    m_isDiscoverLoading = false;
    emit discoverChanged();
    loadDiscover();
    search();
}

void WorkshopViewModel::refreshDiscover() {
    cancelDiscoverRequests();
    m_pendingDiscoverItems.clear();
    m_isDiscoverLoading = false;
    loadDiscover();
}

void WorkshopViewModel::issueDiscoverRequest(DiscoverCollection collection,
                                             WorkshopSortOrder order,
                                             const QString& tag,
                                             int count,
                                             int trendDays) {
    WorkshopQuery query;
    query.sortOrder = order;
    query.perPage = count;
    query.trendDays = trendDays;
    query.ageRatingMask = m_ageRatingMask;
    if (!tag.isEmpty()) query.tags = {tag};
    const quint64 requestId = m_api->queryFiles(query);
    m_discoverRequests.insert(requestId, collection);
}

void WorkshopViewModel::handleQueryFinished(quint64 requestId, const WorkshopQueryResult& result) {
    if (requestId == m_searchRequestId) {
        m_isLoading = false;
        m_error = result.error;
        if (result.error.isEmpty()) {
            m_items = result.items;
            m_totalItems = result.total;
        } else {
            m_items.clear();
            m_totalItems = 0;
        }
        emit browseChanged();
        return;
    }

    const auto it = m_discoverRequests.find(requestId);
    if (it == m_discoverRequests.end()) return;
    const DiscoverCollection collection = it.value();
    m_discoverRequests.erase(it);
    if (result.error.isEmpty()) m_pendingDiscoverItems[collection] = result.items;
    if (!m_discoverRequests.isEmpty()) return;

    m_discoverItems = std::move(m_pendingDiscoverItems);
    m_bannerItems = m_discoverItems.value(DiscoverCollection::Trending).mid(
        0, qMin(5, m_discoverItems.value(DiscoverCollection::Trending).size()));
    m_isDiscoverLoading = false;
    emit discoverChanged();
}

void WorkshopViewModel::checkSteamSetup() {
    // SteamKit 服务无需安装步骤；仅刷新登录状态。
    refreshSteamSetupState();
}

void WorkshopViewModel::refreshSteamSetupState() {
    if (! m_steamService->isRunning()) {
        m_steamSetupState = SteamSetupState::NeedsLogin;
    } else if (! m_steamService->isLoggedIn()) {
        m_steamSetupState = SteamSetupState::NeedsLogin;
    } else {
        m_steamSetupState = SteamSetupState::Ready;
    }
    emit steamSetupChanged();
}

void WorkshopViewModel::logout() {
    m_steamService->logout();
    refreshSteamSetupState();
}

void WorkshopViewModel::setSelectedItem(const std::optional<WorkshopItem>& item) {
    m_selectedItem = item;
    emit selectedItemChanged(item.value_or(WorkshopItem()), item.has_value());
}

void WorkshopViewModel::selectWorkshopItem(const WorkshopItem& item) {
    const auto installed = installedItem(item.publishedFileId);
    if (installed && installed->isValid()) {
        setSelectedItem(std::nullopt);
        emit installedWallpaperRequested(*installed);
        return;
    }

    setSelectedItem(item);
    if (installed && installed->isPreset()) requestPresetDependency(item.publishedFileId);
}

void WorkshopViewModel::requestPresetDependency(const QString& workshopId) {
    const auto preset = installedItem(workshopId);
    if (!preset || !preset->isPreset() || preset->presetDependency.isEmpty()) return;

    const QString dependencyId = preset->presetDependency;
    const auto dependency = installedItem(dependencyId);
    if (dependency && dependency->isValid()) {
        const auto refreshed = installedItem(workshopId);
        if (refreshed && refreshed->isValid()) emit installedWallpaperRequested(*refreshed);
        return;
    }

    PendingDependency pending;
    pending.presetId = workshopId;
    pending.presetTitle = preset->project.title;
    pending.dependencyId = dependencyId;
    const quint64 requestId = m_api->getFileDetails({dependencyId});
    m_dependencyRequests.insert(requestId, pending);
}

void WorkshopViewModel::handleDetailsFinished(quint64 requestId,
                                              const QVector<WorkshopItem>& items,
                                              const QString&) {
    const auto it = m_dependencyRequests.find(requestId);
    if (it != m_dependencyRequests.end()) {
        const PendingDependency pending = it.value();
        m_dependencyRequests.erase(it);

        WorkshopItem dependency = WorkshopItem::dependencyPlaceholder(pending.dependencyId);
        for (const WorkshopItem& item : items) {
            if (item.publishedFileId == pending.dependencyId) {
                dependency = item;
                break;
            }
        }
        emit presetDependencyRequested(pending.presetId,
                                       pending.presetTitle,
                                       pending.dependencyId,
                                       dependency);
        return;
    }

    // 订阅详情请求：累积本批结果并继续请求下一批。
    if (m_subscriptionDetailRequests.remove(requestId)) {
        m_pendingSubscriptionDetails.append(items);
        requestNextSubscriptionDetailBatch();
        return;
    }
}

void WorkshopViewModel::downloadItem(const WorkshopItem& item, DownloadPurpose purpose) {    for (int i = 0; i < m_downloadQueue.size(); ++i) {
        if (m_downloadQueue.at(i).workshopItem.publishedFileId != item.publishedFileId) continue;
        const DownloadStateKind kind = m_downloadQueue.at(i).state.kind;
        if (kind != DownloadStateKind::Failed && kind != DownloadStateKind::Completed && kind != DownloadStateKind::Cancelled) return;
        m_downloadQueue.removeAt(i);
        break;
    }

    WorkshopDownloadTask task;
    task.workshopItem = item;
    task.state.kind = DownloadStateKind::Queued;
    task.state.message = QStringLiteral("等待 Steam 服务按顺序下载…");
    task.purpose = purpose;
    m_downloadQueue.push_back(task);
    emit downloadQueueChanged();
    processDownloadQueue();
}

void WorkshopViewModel::downloadItemById(const QString& workshopId) {
    WorkshopItem item;
    item.publishedFileId = workshopId;
    item.title = QStringLiteral("创意工坊 #%1").arg(workshopId);
    downloadItem(item);
}

void WorkshopViewModel::processDownloadQueue() {
    if (m_steamSetupState != SteamSetupState::Ready) return;
    for (const WorkshopDownloadTask& task : m_downloadQueue) {
        if (isActive(task.state)) return;
    }

    for (WorkshopDownloadTask& task : m_downloadQueue) {
        if (task.state.kind != DownloadStateKind::Queued) continue;
        task.state.kind = DownloadStateKind::Starting;
        task.state.message = QStringLiteral("正在连接 Steam 服务…");
        task.startedAt = QDateTime::currentDateTime();
        emit downloadQueueChanged();
        // taskId 用 workshopId 生成，方便队列按壁纸 ID 匹配进度事件。
        m_steamService->downloadItem(task.workshopItem.publishedFileId,
                                     task.workshopItem.publishedFileId,
                                     Paths::importedDir());
        return;
    }
}

void WorkshopViewModel::handleDownloadState(const QString& workshopId, DownloadStateKind kind,
                                            qint64 receivedBytes, qint64 totalBytes,
                                            double bytesPerSecond, const QString& outputPath,
                                            const QString& message) {
    DownloadState state;
    state.kind = kind;
    state.message = message;
    state.bytesReceived = receivedBytes;
    state.totalBytes = totalBytes;
    state.bytesPerSecond = bytesPerSecond;
    state.outputPath = outputPath;
    if (totalBytes > 0) {
        state.percent = static_cast<double>(receivedBytes) * 100.0 / static_cast<double>(totalBytes);
    }
    for (int i = 0; i < m_downloadQueue.size(); ++i) {
        WorkshopDownloadTask& task = m_downloadQueue[i];
        if (task.workshopItem.publishedFileId != workshopId) continue;
        if (state.kind == DownloadStateKind::Cancelled) {
            m_downloadQueue.removeAt(i);
            emit downloadQueueChanged();
            processDownloadQueue();
            return;
        }

        task.state = state;
        if (state.kind == DownloadStateKind::Completed) {
            task.completedAt = QDateTime::currentDateTime();
            emit workshopItemDownloaded(workshopId);
            QTimer::singleShot(500, this, [this, workshopId] {
                refreshInstalledState();
                handleCompletedDownload(workshopId);
            });
        }
        emit downloadQueueChanged();
        if (state.kind == DownloadStateKind::Completed || state.kind == DownloadStateKind::Failed) {
            processDownloadQueue();
        }
        return;
    }
}

// ---- 订阅管理（对齐上游 WorkshopViewModel.swift）----

const QVector<WorkshopItem>& WorkshopViewModel::subscriptions() const { return m_subscriptionItems; }
int WorkshopViewModel::subscriptionTotal() const { return m_subscriptionTotal; }
int WorkshopViewModel::subscriptionCurrentPage() const {
    const int pageCount = subscriptionPageCount();
    return qMin(pageCount, m_subscriptionStartIndex / kItemsPerPage + 1);
}
int WorkshopViewModel::subscriptionPageCount() const {
    return qMax(1, (m_subscriptionTotal + kItemsPerPage - 1) / kItemsPerPage);
}
bool WorkshopViewModel::isSubscriptionsLoading() const { return m_subscriptionsLoading; }

QString WorkshopViewModel::subscriptionSearchText() const { return m_subscriptionSearchText; }
WorkshopTypeFilter WorkshopViewModel::subscriptionTypeFilter() const { return m_subscriptionTypeFilter; }
int WorkshopViewModel::subscriptionAgeRatingMask() const { return m_subscriptionAgeRatingMask; }
int WorkshopViewModel::subscriptionWidescreenMask() const { return m_subscriptionWidescreen; }
int WorkshopViewModel::subscriptionUltraWidescreenMask() const { return m_subscriptionUltraWidescreen; }
int WorkshopViewModel::subscriptionDualscreenMask() const { return m_subscriptionDualscreen; }
int WorkshopViewModel::subscriptionTriplescreenMask() const { return m_subscriptionTriplescreen; }
int WorkshopViewModel::subscriptionPortraitMask() const { return m_subscriptionPortrait; }
int WorkshopViewModel::subscriptionMiscMask() const { return m_subscriptionMisc; }
const QSet<QString>& WorkshopViewModel::subscriptionSelectedTags() const { return m_subscriptionSelectedTags; }
bool WorkshopViewModel::hasActiveSubscriptionFilters() const {
    const bool tagsActive = !m_subscriptionSelectedTags.isEmpty() &&
                            m_subscriptionSelectedTags.size() < workshopTags().size();
    return !m_subscriptionSearchText.trimmed().isEmpty() ||
           m_subscriptionTypeFilter != WorkshopTypeFilter::All ||
           (m_subscriptionAgeRatingMask != 0 && m_subscriptionAgeRatingMask != 0x7) ||
           tagsActive ||
           m_subscriptionWidescreen != 0x7F ||
           m_subscriptionUltraWidescreen != 0x07 ||
           m_subscriptionDualscreen != 0x0F ||
           m_subscriptionTriplescreen != 0x1F ||
           m_subscriptionPortrait != 0x1F ||
           m_subscriptionMisc != 0x03;
}

void WorkshopViewModel::loadSubscriptions(int startIndex) {
    if (m_subscriptionsLoading) return;
    if (!m_steamService->isLoggedIn()) {
        // 未登录时清空订阅态（对齐 refreshSubscriptions 的 guard 分支）。
        m_pendingSubscriptionIds.clear();
        m_subscriptionCatalogItems.clear();
        m_subscriptionItems.clear();
        m_subscriptionTotal = 0;
        m_subscriptionStartIndex = 0;
        emit subscriptionsChanged();
        return;
    }
    m_subscriptionsLoading = true;
    m_requestedSubscriptionStart = qMax(0, startIndex);
    m_pendingSubscriptionIds.clear();
    m_seenSubscriptionIds.clear();
    emit subscriptionsChanged();
    fetchSubscriptionPage(0, m_requestedSubscriptionStart);
}

void WorkshopViewModel::fetchSubscriptionPage(int serviceStart, int requestedStart) {
    // 服务端订阅记录是分页的，逐页拉取并去重累积 publishedFileId，
    // 直到拉完（page.items 为空或 nextStart >= page.total）为止。
    m_steamService->fetchSubscriptions(serviceStart,
        [this, requestedStart](bool ok, const SteamServiceManager::SubscriptionPage& page, const QString&) {
            if (!ok) {
                m_subscriptionsLoading = false;
                m_pendingSubscriptionIds.clear();
                m_seenSubscriptionIds.clear();
                emit subscriptionsChanged();
                return;
            }
            for (const auto& item : page.items) {
                if (m_seenSubscriptionIds.contains(item.publishedFileId)) continue;
                m_seenSubscriptionIds.insert(item.publishedFileId);
                m_pendingSubscriptionIds.append(item.publishedFileId);
            }
            const int nextStart = page.startIndex + page.items.size();
            if (page.items.isEmpty() || nextStart >= page.total) {
                startSubscriptionDetailsLoad();
            } else {
                fetchSubscriptionPage(nextStart, requestedStart);
            }
        });
}

void WorkshopViewModel::startSubscriptionDetailsLoad() {
    m_pendingSubscriptionDetailBatches.clear();
    m_pendingSubscriptionDetails.clear();
    m_subscriptionDetailRequests.clear();
    // getFileDetails 每批 100 个（对齐 upstream loadSubscriptionItems 的 stride 100）。
    for (int start = 0; start < m_pendingSubscriptionIds.size(); start += 100) {
        m_pendingSubscriptionDetailBatches.append(m_pendingSubscriptionIds.mid(start, 100));
    }
    requestNextSubscriptionDetailBatch();
}

void WorkshopViewModel::requestNextSubscriptionDetailBatch() {
    if (m_pendingSubscriptionDetailBatches.isEmpty()) {
        finishSubscriptionLoad();
        return;
    }
    const QStringList batch = m_pendingSubscriptionDetailBatches.takeFirst();
    const quint64 requestId = m_api->getFileDetails(batch);
    m_subscriptionDetailRequests.insert(requestId);
}

void WorkshopViewModel::finishSubscriptionLoad() {
    // 按订阅顺序重建详情列表（对齐 loadSubscriptionItems 的 byID 映射 + records.map），
    // 缺详情时用占位项（对齐 unavailableSubscription）。
    QHash<QString, WorkshopItem> byId;
    for (const WorkshopItem& item : m_pendingSubscriptionDetails) {
        if (!byId.contains(item.publishedFileId)) byId.insert(item.publishedFileId, item);
    }
    m_subscriptionCatalogItems.clear();
    m_subscriptionCatalogItems.reserve(m_pendingSubscriptionIds.size());
    for (const QString& id : m_pendingSubscriptionIds) {
        const auto it = byId.constFind(id);
        if (it != byId.constEnd()) {
            m_subscriptionCatalogItems.append(it.value());
        } else {
            m_subscriptionCatalogItems.append(WorkshopItem::dependencyPlaceholder(id));
        }
    }
    m_pendingSubscriptionIds.clear();
    m_seenSubscriptionIds.clear();
    m_pendingSubscriptionDetails.clear();
    m_pendingSubscriptionDetailBatches.clear();
    m_subscriptionsLoading = false;
    rebuildSubscriptionPage(m_requestedSubscriptionStart);
    // "下载全部"在目录为空时发起：订阅加载完成后延续生成确认计划。
    if (m_buildPlanAfterSubscriptionLoad) {
        buildSubscriptionDownloadPlan();
        return;
    }
    emit subscriptionsChanged();
}

void WorkshopViewModel::rebuildSubscriptionPage(int startIndex) {
    QVector<WorkshopItem> filtered;
    filtered.reserve(m_subscriptionCatalogItems.size());
    for (const WorkshopItem& item : m_subscriptionCatalogItems) {
        if (matchesSubscriptionFilters(item)) filtered.append(item);
    }
    const int maximumStart = filtered.isEmpty() ? 0 : (filtered.size() - 1) / kItemsPerPage * kItemsPerPage;
    const int clampedStart = qBound(0, startIndex, maximumStart);
    m_subscriptionTotal = filtered.size();
    m_subscriptionStartIndex = clampedStart;
    m_subscriptionItems = filtered.mid(clampedStart, kItemsPerPage);
    emit subscriptionsChanged();
}

bool WorkshopViewModel::matchesSubscriptionFilters(const WorkshopItem& item) const {
    const QString query = m_subscriptionSearchText.trimmed();
    if (!query.isEmpty()) {
        const QStringList searchable = QStringList{item.title, item.description,
                                                   item.creatorSteamId, item.publishedFileId} + item.tags;
        bool matched = false;
        for (const QString& value : searchable) {
            if (value.contains(query, Qt::CaseInsensitive)) { matched = true; break; }
        }
        if (!matched) return false;
    }

    switch (m_subscriptionTypeFilter) {
    case WorkshopTypeFilter::All: break;
    case WorkshopTypeFilter::Scene: if (item.kind() != WallpaperKind::Scene) return false; break;
    case WorkshopTypeFilter::Web: if (item.kind() != WallpaperKind::Web) return false; break;
    case WorkshopTypeFilter::Video: if (item.kind() != WallpaperKind::Video) return false; break;
    case WorkshopTypeFilter::Preset: if (!item.isPreset()) return false; break;
    }

    // 分级过滤：非空且非全选时，排除未选中的分级（对齐 subscriptionAgeRatingFilter 语义）。
    if (m_subscriptionAgeRatingMask != 0 && m_subscriptionAgeRatingMask != 0x7) {
        int bit = 0;
        if (item.ageRating == QLatin1String("Questionable")) bit = 1;
        else if (item.ageRating == QLatin1String("Mature")) bit = 2;
        if ((m_subscriptionAgeRatingMask & (1 << bit)) == 0) return false;
    }

    // 标签过滤：要求 item 同时命中所有已选标签（全选时等价于不过滤）。
    const int selectableCount = workshopTags().size();
    if (!m_subscriptionSelectedTags.isEmpty() && m_subscriptionSelectedTags.size() < selectableCount) {
        QSet<QString> itemTags;
        for (const QString& tag : item.tags) itemTags.insert(tag.toLower());
        for (const QString& selected : m_subscriptionSelectedTags) {
            if (!itemTags.contains(selected.toLower())) return false;
        }
    }

    return workshopResolutionMatches(item.tags,
                                     m_subscriptionWidescreen,
                                     m_subscriptionUltraWidescreen,
                                     m_subscriptionDualscreen,
                                     m_subscriptionTriplescreen,
                                     m_subscriptionPortrait,
                                     m_subscriptionMisc);
}

void WorkshopViewModel::goToSubscriptionPage(int page) {
    const int target = qBound(1, page, subscriptionPageCount());
    if (m_subscriptionsLoading || target == subscriptionCurrentPage()) return;
    rebuildSubscriptionPage((target - 1) * kItemsPerPage);
}

void WorkshopViewModel::setSubscriptionSearchText(const QString& text) {
    if (m_subscriptionSearchText == text) return;
    m_subscriptionSearchText = text;
    rebuildSubscriptionPage(0);
}

void WorkshopViewModel::setSubscriptionTypeFilter(WorkshopTypeFilter filter) {
    if (m_subscriptionTypeFilter == filter) return;
    m_subscriptionTypeFilter = filter;
    rebuildSubscriptionPage(0);
}

void WorkshopViewModel::setSubscriptionAgeRatingEnabled(WorkshopAgeRating rating, bool enabled) {
    const int bit = 1 << static_cast<int>(rating);
    const int updated = enabled ? (m_subscriptionAgeRatingMask | bit) : (m_subscriptionAgeRatingMask & ~bit);
    if (updated == m_subscriptionAgeRatingMask) return;
    m_subscriptionAgeRatingMask = updated;
    rebuildSubscriptionPage(0);
}

void WorkshopViewModel::setSubscriptionResolutionOption(int group, int bit, bool enabled) {
    if (group < 0 || group >= WorkshopResolutionGroupCount) return;
    int* mask = nullptr;
    switch (group) {
    case WorkshopResolutionWidescreen: mask = &m_subscriptionWidescreen; break;
    case WorkshopResolutionUltraWidescreen: mask = &m_subscriptionUltraWidescreen; break;
    case WorkshopResolutionDualscreen: mask = &m_subscriptionDualscreen; break;
    case WorkshopResolutionTriplescreen: mask = &m_subscriptionTriplescreen; break;
    case WorkshopResolutionPortrait: mask = &m_subscriptionPortrait; break;
    case WorkshopResolutionMisc: mask = &m_subscriptionMisc; break;
    }
    if (!mask) return;
    const int updated = enabled ? (*mask | (1 << bit)) : (*mask & ~(1 << bit));
    if (updated == *mask) return;
    *mask = updated;
    rebuildSubscriptionPage(0);
}

void WorkshopViewModel::selectAllSubscriptionResolutions() {
    m_subscriptionWidescreen = 0x7F;
    m_subscriptionUltraWidescreen = 0x07;
    m_subscriptionDualscreen = 0x0F;
    m_subscriptionTriplescreen = 0x1F;
    m_subscriptionPortrait = 0x1F;
    m_subscriptionMisc = 0x03;
    rebuildSubscriptionPage(0);
}

void WorkshopViewModel::clearSubscriptionResolutions() {
    m_subscriptionWidescreen = 0;
    m_subscriptionUltraWidescreen = 0;
    m_subscriptionDualscreen = 0;
    m_subscriptionTriplescreen = 0;
    m_subscriptionPortrait = 0;
    m_subscriptionMisc = 0;
    rebuildSubscriptionPage(0);
}

void WorkshopViewModel::selectAllSubscriptionTags() {
    for (const WorkshopTag& tag : workshopTags()) m_subscriptionSelectedTags.insert(tag.value);
    rebuildSubscriptionPage(0);
}

void WorkshopViewModel::clearSubscriptionTags() {
    m_subscriptionSelectedTags.clear();
    rebuildSubscriptionPage(0);
}

void WorkshopViewModel::toggleSubscriptionTag(const QString& tag) {
    if (m_subscriptionSelectedTags.contains(tag)) m_subscriptionSelectedTags.remove(tag);
    else m_subscriptionSelectedTags.insert(tag);
    rebuildSubscriptionPage(0);
}

void WorkshopViewModel::clearSubscriptionFilters() {
    m_subscriptionSearchText.clear();
    m_subscriptionSelectedTags.clear();
    m_subscriptionTypeFilter = WorkshopTypeFilter::All;
    m_subscriptionAgeRatingMask = 0x7;
    m_subscriptionWidescreen = 0x7F;
    m_subscriptionUltraWidescreen = 0x07;
    m_subscriptionDualscreen = 0x0F;
    m_subscriptionTriplescreen = 0x1F;
    m_subscriptionPortrait = 0x1F;
    m_subscriptionMisc = 0x03;
    rebuildSubscriptionPage(0);
}

bool WorkshopViewModel::isPreparingSubscriptionDownloads() const {
    return m_isPreparingSubscriptionDownloads;
}

const std::optional<SubscriptionDownloadPlan>& WorkshopViewModel::subscriptionDownloadPlan() const {
    return m_subscriptionDownloadPlan;
}

void WorkshopViewModel::downloadAllSubscriptions() {
    // 对齐 macOS downloadAllSubscriptions：未登录或正在准备时直接返回。
    if (!m_steamService->isLoggedIn() || m_isPreparingSubscriptionDownloads) return;
    m_isPreparingSubscriptionDownloads = true;
    m_subscriptionDownloadPlan.reset();
    emit subscriptionsChanged();
    if (!m_subscriptionCatalogItems.isEmpty()) {
        buildSubscriptionDownloadPlan();
        return;
    }
    // 目录尚未加载（如直接点击"下载全部"）：先拉取订阅，
    // loadSubscriptions 完成后由 finishSubscriptionLoad 延续生成计划。
    m_buildPlanAfterSubscriptionLoad = true;
    loadSubscriptions(0);
}

void WorkshopViewModel::buildSubscriptionDownloadPlan() {
    m_isPreparingSubscriptionDownloads = false;
    m_buildPlanAfterSubscriptionLoad = false;

    // 过滤：去重、排除不支持类型与已安装项（对齐 macOS 的 remaining 计算）。
    QVector<WorkshopItem> remaining;
    QSet<QString> seen;
    remaining.reserve(m_subscriptionCatalogItems.size());
    for (const WorkshopItem& item : m_subscriptionCatalogItems) {
        if (seen.contains(item.publishedFileId)) continue;
        seen.insert(item.publishedFileId);
        if (item.kind() == WallpaperKind::Unsupported) continue;
        if (isItemDownloaded(item.publishedFileId)) continue;
        remaining.append(item);
    }
    // 排除下载队列中活跃的任务（starting/connecting/downloading/resolving），
    // 避免重复入队；已下载任务仍在队列时也由 isItemDownloaded 覆盖。
    QSet<QString> active;
    for (const WorkshopDownloadTask& task : m_downloadQueue) {
        if (isActive(task.state)) active.insert(task.workshopItem.publishedFileId);
    }
    QVector<WorkshopItem> pending;
    for (const WorkshopItem& item : remaining) {
        if (!active.contains(item.publishedFileId)) pending.append(item);
    }

    SubscriptionDownloadPlan plan;
    plan.subscriptionCount = seen.size();
    plan.remainingCount = remaining.size();
    plan.items = pending;
    m_subscriptionDownloadPlan = plan;
    emit subscriptionsChanged();
}

void WorkshopViewModel::confirmSubscriptionDownloads() {
    // 对齐 macOS confirmSubscriptionDownloads：取走计划后逐项入队下载。
    if (!m_subscriptionDownloadPlan) return;
    const QVector<WorkshopItem> items = m_subscriptionDownloadPlan->items;
    m_subscriptionDownloadPlan.reset();
    emit subscriptionsChanged();
    for (const WorkshopItem& item : items) {
        downloadItem(item, DownloadPurpose::Wallpaper);
    }
}

void WorkshopViewModel::dismissSubscriptionDownloadPlan() {
    if (!m_subscriptionDownloadPlan) return;
    m_subscriptionDownloadPlan.reset();
    emit subscriptionsChanged();
}

void WorkshopViewModel::subscribe(const QString& workshopId) {
    m_steamService->subscribe(workshopId,
        [this](bool ok, const QString&, const QString&, const QJsonObject&) {
            if (ok) loadSubscriptions(0);
        });
}

void WorkshopViewModel::unsubscribe(const QString& workshopId) {
    m_steamService->unsubscribe(workshopId,
        [this](bool ok, const QString&, const QString&, const QJsonObject&) {
            if (ok) loadSubscriptions(0);
        });
}

void WorkshopViewModel::cancelDownload(const QString& workshopId) {
    for (int i = 0; i < m_downloadQueue.size(); ++i) {
        WorkshopDownloadTask& task = m_downloadQueue[i];
        if (task.workshopItem.publishedFileId != workshopId) continue;
        if (task.state.kind == DownloadStateKind::Queued) {
            m_downloadQueue.removeAt(i);
            emit downloadQueueChanged();
            processDownloadQueue();
        } else {
            m_steamService->cancelDownload(workshopId);
        }
        return;
    }
}

void WorkshopViewModel::retryDownload(const QString& workshopId) {
    for (WorkshopDownloadTask& task : m_downloadQueue) {
        if (task.workshopItem.publishedFileId != workshopId || task.state.kind != DownloadStateKind::Failed) continue;
        task.state = DownloadState();
        task.state.kind = DownloadStateKind::Queued;
        task.state.message = QStringLiteral("等待 Steam 服务按顺序下载…");
        emit downloadQueueChanged();
        processDownloadQueue();
        return;
    }
}

void WorkshopViewModel::clearCompletedDownloads() {
    for (int i = m_downloadQueue.size() - 1; i >= 0; --i) {
        const DownloadStateKind kind = m_downloadQueue.at(i).state.kind;
        if (kind == DownloadStateKind::Completed || kind == DownloadStateKind::Failed || kind == DownloadStateKind::Cancelled) {
            m_downloadQueue.removeAt(i);
        }
    }
    emit downloadQueueChanged();
}

void WorkshopViewModel::confirmPresetDependencyDownload(const QString& presetId,
                                                        const QString& dependencyId,
                                                        const WorkshopItem& dependencyItem) {
    const auto preset = installedItem(presetId);
    if (preset && preset->isValid()) {
        emit installedWallpaperRequested(*preset);
        return;
    }

    m_pendingPresetId = presetId;
    m_pendingDependencyId = dependencyId;
    downloadItem(dependencyItem, DownloadPurpose::PresetDependency);
    if (m_steamSetupState != SteamSetupState::Ready) emit steamSetupRequested();
}

void WorkshopViewModel::handleCompletedDownload(const QString& workshopId) {
    if (!m_pendingDependencyId.isEmpty() && workshopId == m_pendingDependencyId) {
        const auto preset = installedItem(m_pendingPresetId);
        if (preset && preset->isValid()) {
            m_pendingPresetId.clear();
            m_pendingDependencyId.clear();
            emit installedWallpaperRequested(*preset);
            return;
        }
    }

    const auto wallpaper = installedItem(workshopId);
    if (!wallpaper) return;
    if (wallpaper->isPreset() && !wallpaper->isValid()) {
        requestPresetDependency(workshopId);
    } else if (wallpaper->isValid()) {
        emit installedWallpaperRequested(*wallpaper);
    }
}

void WorkshopViewModel::refreshInstalledState() {
    if (m_installedStateWatcher.isRunning()) {
        m_installedStateRefreshPending = true;
        return;
    }
    const QStringList sources = m_library->sourceDirectories();
    m_installedStateWatcher.setFuture(QtConcurrent::run(scanInstalledWorkshopState, sources));
}

void WorkshopViewModel::cancelDiscoverRequests() {
    for (auto it = m_discoverRequests.cbegin(); it != m_discoverRequests.cend(); ++it) {
        m_api->cancelQuery(it.key());
    }
    m_discoverRequests.clear();
}

void WorkshopViewModel::navigateToWorkshopWithTag(const QString& tag) {
    m_searchDebounce.stop();
    m_selectedTags = {tag};
    m_searchText.clear();
    m_typeFilter = WorkshopTypeFilter::All;
    m_sortOrder = WorkshopSortOrder::Trending;
    m_currentPage = 1;
    emit filtersChanged();
    emit navigateToWorkshopRequested();
    search();
}

void WorkshopViewModel::navigateToWorkshopWithSort(WorkshopSortOrder order) {
    m_searchDebounce.stop();
    m_selectedTags.clear();
    m_searchText.clear();
    m_typeFilter = WorkshopTypeFilter::All;
    m_sortOrder = order;
    m_currentPage = 1;
    emit filtersChanged();
    emit navigateToWorkshopRequested();
    search();
}

} // namespace Mirage
