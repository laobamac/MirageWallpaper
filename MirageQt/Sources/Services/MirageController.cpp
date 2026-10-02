#include "Services/MirageController.h"

#include <QDir>
#include <QDesktopServices>
#include <QFileInfo>
#include <QClipboard>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace Mirage {
namespace {




WorkshopSortOrder workshopSortOrderFor(const QString& key) {
    if (key == QStringLiteral("recent")) return WorkshopSortOrder::MostRecent;
    if (key == QStringLiteral("subscribed")) return WorkshopSortOrder::MostSubscribed;
    if (key == QStringLiteral("rated")) return WorkshopSortOrder::TopRated;
    if (key == QStringLiteral("upvoted")) return WorkshopSortOrder::MostUpvoted;
    if (key == QStringLiteral("playtime")) return WorkshopSortOrder::PlaytimeTrend;
    if (key == QStringLiteral("total-playtime")) return WorkshopSortOrder::TotalPlaytime;
    if (key == QStringLiteral("average-playtime")) return WorkshopSortOrder::AveragePlaytimeTrend;
    if (key == QStringLiteral("lifetime-average")) return WorkshopSortOrder::LifetimeAveragePlaytime;
    if (key == QStringLiteral("sessions")) return WorkshopSortOrder::SessionsTrend;
    if (key == QStringLiteral("lifetime-sessions")) return WorkshopSortOrder::LifetimeSessions;
    if (key == QStringLiteral("updated")) return WorkshopSortOrder::LastUpdated;
    return WorkshopSortOrder::Trending;
}

WorkshopTypeFilter workshopTypeFilterFor(const QString& key) {
    if (key == QStringLiteral("scene")) return WorkshopTypeFilter::Scene;
    if (key == QStringLiteral("web")) return WorkshopTypeFilter::Web;
    if (key == QStringLiteral("video")) return WorkshopTypeFilter::Video;
    if (key == QStringLiteral("preset")) return WorkshopTypeFilter::Preset;
    return WorkshopTypeFilter::All;
}

std::optional<WorkshopAgeRating> workshopAgeRatingFor(const QString& key) {
    if (key == QStringLiteral("Everyone")) return WorkshopAgeRating::Everyone;
    if (key == QStringLiteral("Questionable")) return WorkshopAgeRating::Questionable;
    if (key == QStringLiteral("Mature")) return WorkshopAgeRating::Mature;
    return std::nullopt;
}

// 类型过滤 enum → QML key（与 OptionData.js 的 workshopTypeFilters 对齐）。
QString workshopTypeKey(WorkshopTypeFilter filter) {
    switch (filter) {
    case WorkshopTypeFilter::Scene: return QStringLiteral("scene");
    case WorkshopTypeFilter::Web: return QStringLiteral("web");
    case WorkshopTypeFilter::Video: return QStringLiteral("video");
    case WorkshopTypeFilter::Preset: return QStringLiteral("preset");
    case WorkshopTypeFilter::All: return QStringLiteral("all");
    }
    return QStringLiteral("all");
}


} // namespace

MirageController::MirageController(QObject* parent)
    : QObject(parent)
    , m_settings(this)
    , m_favorites(this)
    , m_library(&m_settings, this)
    , m_steamService(this)
    , m_steamAPI(&m_settings, this)
    , m_workshop(&m_steamAPI, &m_steamService, &m_library, this)
    , m_renderer(&m_settings, this)
    , m_runtimeStore(this)
    , m_playlist(&m_library, &m_renderer, this)
    , m_trusted(this)
    , m_steamSetup(&m_steamService, this)
    , m_playback(&m_settings, &m_renderer, &m_runtimeStore, &m_playlist, this)
    , m_displayModel(this) {
    m_firstLaunch = QSettings().value(QStringLiteral("IsFirstLaunch"), true).toBool();
    m_renderer.setWallpaperTrustChecker([this](const Wallpaper& item) {
        return m_trusted.isTrusted(item.id());
    });
    connect(&m_library, &WallpaperLibrary::libraryChanged, this, &MirageController::reloadWallpapers);
    connect(&m_favorites, &FavoritesManager::changed, this, [this] {
        // favorite 字段参与 wallpaperMap 序列化，必须先重建缓存再发信号，
        // 保证 QML 在 selectedWallpaperChanged / wallpapersChanged 后重求值
        // 读到最新收藏状态。
        refreshSelectedWallpaperCache();
        refreshWallpapersCache();
        emit wallpapersChanged();
        emit selectedWallpaperChanged();
    });
    connect(&m_playlist, &PlaylistManager::currentChanged, this, [this](int screen) {
        if (screen == m_playlistScreen) emit playlistChanged();
    });
    connect(&m_playlist, &PlaylistManager::savedChanged, this, &MirageController::playlistsSavedChanged);
    connect(&m_playback, &PlaybackController::pausedChanged,
            this, &MirageController::playbackPausedChanged);
    connect(&m_renderer, &RendererController::rendererMessage, this, &MirageController::setStatusMessage);
    connect(&m_renderer, &RendererController::rendererStateChanged,
            this, [this] { refreshDisplayStates(); emit displaysChanged(); });
    connect(&m_renderer, &RendererController::positionAvailabilityChanged,
            this, [this](int screen, bool, bool) {
                if (m_renderer.wallpaperIdOnScreen(screen) == m_selectedWallpaperId) {
                    emit selectedRuntimeChanged();
                }
            });
    connect(&m_renderer, &RendererController::scriptStorageChanged,
            this, [this](const QString& wallpaperId,
                         const QHash<QString, QString>& values) {
                const Wallpaper item = wallpaper(wallpaperId);
                if (item.isValid()) m_runtimeStore.setScriptStorage(item, values);
            });
    connect(&m_renderer, &RendererController::snapshotFinished,
            this, &MirageController::sceneSnapshotFinished);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen*) {
        emit displaysChanged();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) {
        emit displaysChanged();
    });
    connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, [this](QScreen*) {
        // KDE can change the primary output without adding/removing a screen;
        // refresh the model so the primary marker stays aligned with Plasma.
        emit displaysChanged();
    });
    connect(&m_settings, &GlobalSettingsService::settingsChanged, this, [this](const GlobalSettings&) {
        emit settingsChanged();
    });
    connect(&m_runtimeStore, &WallpaperRuntimeStore::runtimeChanged, this,
            [this](const QString& id, const WallpaperRuntimeState&) {
                if (id != m_selectedWallpaperId) return;
                // 属性 override 变化（滑块/开关/combo 编辑）会改变
                // effectiveProperties 结果，先重建缓存再发信号，避免
                // PropertyEditor 各属性绑定在重求值时各自触发一次重活。
                refreshSelectedPropertiesCache();
                emit selectedRuntimeChanged();
            });
    connect(&m_workshop, &WorkshopViewModel::browseChanged, this, [this] {
        emit workshopItemsChanged();
        emit workshopStateChanged();
    });
    connect(&m_workshop, &WorkshopViewModel::discoverChanged, this, &MirageController::discoverChanged);
    connect(&m_workshop, &WorkshopViewModel::filtersChanged, this, &MirageController::workshopStateChanged);
    connect(&m_workshop, &WorkshopViewModel::selectedItemChanged, this,
            [this](const WorkshopItem&, bool) { emit selectedWorkshopItemChanged(); });
    connect(&m_workshop, &WorkshopViewModel::downloadQueueChanged, this, [this] {
        emit workshopItemsChanged();
        emit discoverChanged();
        emit selectedWorkshopItemChanged();
        emit workshopStateChanged();
    });
    connect(&m_workshop, &WorkshopViewModel::installedStateChanged, this, [this] {
        emit workshopItemsChanged();
        emit discoverChanged();
        emit selectedWorkshopItemChanged();
    });
    connect(&m_workshop, &WorkshopViewModel::steamSetupChanged, this, &MirageController::workshopStateChanged);
    connect(&m_steamSetup, &SteamSetupViewModel::steamChanged, this, &MirageController::steamChanged);
    connect(&m_steamService, &SteamServiceManager::authenticationChanged, this,
            [this](bool, const QString&, const QString&) { emit workshopStateChanged(); });
    connect(&m_workshop, &WorkshopViewModel::subscriptionsChanged, this,
            &MirageController::subscriptionsChanged);
    connect(&m_workshop, &WorkshopViewModel::installedWallpaperRequested, this, [this](const Wallpaper& item) {
        selectWallpaper(item.id());
        emit installedWallpaperSelected();
        setStatusMessage(QStringLiteral("该壁纸已在本地库中"));
    });
    connect(&m_workshop, &WorkshopViewModel::presetDependencyRequested, this,
            [this](const QString&, const QString& title, const QString&, const WorkshopItem&) {
                setStatusMessage(QStringLiteral("预设“%1”需要下载基础壁纸").arg(title));
            });
    connect(&m_workshop, &WorkshopViewModel::workshopItemDownloaded, this, [this](const QString&) {
        reloadWallpapers();
        setStatusMessage(QStringLiteral("创意工坊下载完成"));
    });

    // 启动 Steam 服务进程；若保存过会话则自动恢复。
    m_steamService.start();
    if (m_steamService.hasSavedSession()) m_steamService.restoreSessionIfNeeded();
    reloadWallpapers();
}

void MirageController::startPlayback() {
    m_playback.restoreStartupPlayback();
    m_playlist.startRotators();
}

MirageController::~MirageController() {
    m_renderer.stopAll();
}

QVariantList MirageController::wallpapers() const {
    return m_wallpapersCache;
}

void MirageController::refreshWallpapersCache() {
    QVariantList result;
    result.reserve(m_allWallpapers.size());
    for (const Wallpaper& item : m_allWallpapers) result.append(wallpaperMap(item));
    m_wallpapersCache = result;
}

QVariantMap MirageController::selectedWallpaper() const {
    return m_selectedWallpaperCache;
}

QString MirageController::selectedWallpaperId() const {
    return m_selectedWallpaperId;
}

QVariantList MirageController::playlistItems() const {
    QVariantList result;
    for (const Wallpaper& item : m_playlist.resolvedItems(m_playlistScreen)) result.append(wallpaperMap(item));
    return result;
}

QVariantList MirageController::workshopItems() const {
    QVariantList result;
    result.reserve(m_workshop.items().size());
    for (const WorkshopItem& item : m_workshop.items()) result.append(workshopItemMap(item));
    return result;
}


QVariantMap MirageController::selectedWorkshopItem() const {
    return m_workshop.selectedItem() ? workshopItemMap(*m_workshop.selectedItem()) : QVariantMap{};
}

bool MirageController::workshopLoading() const { return m_workshop.isLoading(); }
bool MirageController::discoverLoading() const { return m_workshop.isDiscoverLoading(); }
QString MirageController::workshopError() const { return m_workshop.error(); }
int MirageController::workshopPage() const { return m_workshop.currentPage(); }
int MirageController::workshopPageCount() const { return m_workshop.totalPages(); }
int MirageController::activeDownloadCount() const { return m_workshop.activeDownloadCount(); }
QVariantList MirageController::downloadQueue() const {
    QVariantList result;
    for (const WorkshopDownloadTask& task : m_workshop.downloadQueue()) {
        const DownloadState& state = task.state;
        result.append(QVariantMap{
            {QStringLiteral("id"), task.workshopItem.publishedFileId},
            {QStringLiteral("title"), task.workshopItem.title},
            {QStringLiteral("preview"), task.workshopItem.previewImageUrl},
            {QStringLiteral("typeLabel"), task.workshopItem.displayTypeName()},
            {QStringLiteral("purpose"), task.purpose == DownloadPurpose::PresetDependency
                ? QStringLiteral("presetDependency") : QStringLiteral("wallpaper")},
            {QStringLiteral("state"), downloadStateKey(state.kind)},
            {QStringLiteral("progress"), state.percent},
            {QStringLiteral("message"), state.message},
            {QStringLiteral("size"), task.workshopItem.fileSize},
            {QStringLiteral("sizeLabel"), task.workshopItem.formattedFileSize()},
        });
    }
    return result;
}

bool MirageController::steamReady() const { return m_workshop.steamSetupState() == SteamSetupState::Ready; }
QString MirageController::steamSetupSummary() const { return m_workshop.steamSetupSummary(); }
QString MirageController::steamUsername() const { return m_steamService.accountName(); }
bool MirageController::steamLoggedIn() const { return m_steamService.isLoggedIn(); }
QString MirageController::steamLoginState() const { return m_steamSetup.loginState(); }
QString MirageController::steamLoginMessage() const { return m_steamSetup.loginMessage(); }
QString MirageController::steamGuardType() const { return m_steamSetup.guardType(); }
QString MirageController::steamQRCodeUrl() const { return m_steamSetup.qrChallengeUrl(); }
bool MirageController::steamSessionReusable() const { return m_steamSetup.hasSavedSession(); }
bool MirageController::steamServiceRunning() const { return m_steamService.isRunning(); }

QVariantList MirageController::subscriptions() const {
    QVariantList result;
    for (const WorkshopItem& item : m_workshop.subscriptions()) {
        result.append(workshopItemMap(item));
    }
    return result;
}
bool MirageController::subscriptionsLoading() const { return m_workshop.isSubscriptionsLoading(); }
int MirageController::subscriptionTotal() const { return m_workshop.subscriptionTotal(); }
int MirageController::subscriptionPage() const { return m_workshop.subscriptionCurrentPage(); }
int MirageController::subscriptionPageCount() const { return m_workshop.subscriptionPageCount(); }
bool MirageController::subscriptionDownloadPreparing() const {
    return m_workshop.isPreparingSubscriptionDownloads();
}

QVariantMap MirageController::subscriptionDownloadPlan() const {
    const std::optional<SubscriptionDownloadPlan>& plan = m_workshop.subscriptionDownloadPlan();
    if (!plan) return {};
    QVariantList items;
    items.reserve(plan->items.size());
    for (const WorkshopItem& item : plan->items) {
        items.append(QVariantMap{
            {QStringLiteral("id"), item.publishedFileId},
            {QStringLiteral("title"), item.title},
        });
    }
    return {
        {QStringLiteral("subscriptionCount"), plan->subscriptionCount},
        {QStringLiteral("remainingCount"), plan->remainingCount},
        {QStringLiteral("downloadCount"), plan->items.size()},
        {QStringLiteral("items"), items},
    };
}

QVariantMap MirageController::subscriptionFilters() const {
    return {
        {QStringLiteral("searchText"), m_workshop.subscriptionSearchText()},
        {QStringLiteral("typeFilter"), workshopTypeKey(m_workshop.subscriptionTypeFilter())},
        {QStringLiteral("ageRatingMask"), m_workshop.subscriptionAgeRatingMask()},
        {QStringLiteral("widescreen"), m_workshop.subscriptionWidescreenMask()},
        {QStringLiteral("ultraWidescreen"), m_workshop.subscriptionUltraWidescreenMask()},
        {QStringLiteral("dualscreen"), m_workshop.subscriptionDualscreenMask()},
        {QStringLiteral("triplescreen"), m_workshop.subscriptionTriplescreenMask()},
        {QStringLiteral("portrait"), m_workshop.subscriptionPortraitMask()},
        {QStringLiteral("misc"), m_workshop.subscriptionMiscMask()},
        {QStringLiteral("selectedTags"), QStringList(m_workshop.subscriptionSelectedTags().values())},
        {QStringLiteral("hasActiveFilters"), m_workshop.hasActiveSubscriptionFilters()},
    };
}

bool MirageController::firstLaunch() const {
    return m_firstLaunch;
}

QString MirageController::statusMessage() const {
    return m_statusMessage;
}


QVariantList MirageController::selectedProperties() const {
    return m_selectedPropertiesCache;
}

void MirageController::refreshSelectedPropertiesCache() {
    const Wallpaper item = wallpaper(m_selectedWallpaperId);
    if (!item.isValid()) {
        m_selectedPropertiesCache.clear();
        return;
    }

    const QHash<QString, ProjectProperty> properties = m_runtimeStore.effectiveProperties(item);
    QVector<QString> keys;
    keys.reserve(properties.size());
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        if (!it.value().presetOnly) keys.append(it.key());
    }
    std::sort(keys.begin(), keys.end(), [&properties](const QString& left, const QString& right) {
        const ProjectProperty leftProperty = properties.value(left);
        const ProjectProperty rightProperty = properties.value(right);
        const int leftOrder = leftProperty.order < 0 ? leftProperty.index : leftProperty.order;
        const int rightOrder = rightProperty.order < 0 ? rightProperty.index : rightProperty.order;
        if (leftOrder != rightOrder) return leftOrder < rightOrder;
        return left < right;
    });

    QVariantList result;
    result.reserve(keys.size());
    for (const QString& key : keys) result.append(propertyMap(key, properties.value(key)));
    m_selectedPropertiesCache = result;
}

int MirageController::playlistScreen() const {
    return m_playlistScreen;
}

int MirageController::screenCount() const {
    return qMax(1, QGuiApplication::screens().size());
}

QVariantList MirageController::displays() const {
    return m_playback.displays();
}

QVariantList MirageController::savedPlaylists() const {
    QVariantList result;
    const QVector<Playlist> playlists = m_playlist.saved();
    result.reserve(playlists.size());
    for (const Playlist& playlist : playlists) result.append(playlistMap(playlist));
    return result;
}


double MirageController::selectedVolume() const {
    return m_playback.selectedVolume();
}

double MirageController::selectedSpeed() const {
    return m_playback.selectedSpeed();
}

QString MirageController::selectedFillMode() const {
    return m_playback.selectedFillMode();
}

double MirageController::selectedPositionX() const {
    return m_playback.selectedPosition().x();
}

double MirageController::selectedPositionY() const {
    return m_playback.selectedPosition().y();
}

bool MirageController::positionAvailabilityKnown() const {
    return m_playback.selectedPositionAvailability().known;
}

bool MirageController::positionXAvailable() const {
    return m_playback.selectedPositionAvailability().x;
}

bool MirageController::positionYAvailable() const {
    return m_playback.selectedPositionAvailability().y;
}

void MirageController::reloadWallpapers() {
    m_allWallpapers = m_library.loadAll();
    refreshWallpapersCache();
    // 选中 id 有效时直接以现有 id 重建选中缓存；无效时下方重置 id 后再建。
    // 两个分支各重建一次，避免分支 2 先用旧（无效）id 建一次再被覆盖。
    if (wallpaper(m_selectedWallpaperId).isValid()) {
        refreshSelectedWallpaperCache();
        refreshSelectedPropertiesCache();
        emit wallpapersChanged();
        emit selectedWallpaperChanged();
        return;
    }
    m_selectedWallpaperId = m_allWallpapers.isEmpty() ? QString() : m_allWallpapers.first().id();
    refreshSelectedWallpaperCache();
    refreshSelectedPropertiesCache();
    emit wallpapersChanged();
    emit selectedWallpaperChanged();
    emit selectedRuntimeChanged();
}

void MirageController::selectWallpaper(const QString& id) {
    if (m_selectedWallpaperId == id || !wallpaper(id).isValid()) return;
    m_selectedWallpaperId = id;
    refreshSelectedWallpaperCache();
    refreshSelectedPropertiesCache();
    emit selectedWallpaperChanged();
    emit selectedRuntimeChanged();
}

bool MirageController::isWallpaperTrusted(const QString& id) const {
    const Wallpaper item = wallpaper(id);
    if (!item.isValid() || item.kind() != WallpaperKind::Web) return false;
    return m_trusted.isTrusted(id);
}

void MirageController::trustWallpaper(const QString& id, bool persist) {
    const Wallpaper item = wallpaper(id);
    if (!item.isValid() || item.kind() != WallpaperKind::Web) return;
    m_trusted.trust(id, persist);
}

void MirageController::applySelected(bool allScreens) {
    if (!m_wallpaperAssignmentDisplayId.isEmpty()) {
        applySelectedToDisplay(m_wallpaperAssignmentDisplayId);
        cancelWallpaperAssignment();
        return;
    }
    m_playback.apply(wallpaper(m_selectedWallpaperId), allScreens);
}

void MirageController::applyWallpaper(const QString& id, bool allScreens) {
    selectWallpaper(id);
    if (!m_wallpaperAssignmentDisplayId.isEmpty()) {
        applySelectedToDisplay(m_wallpaperAssignmentDisplayId);
        cancelWallpaperAssignment();
        return;
    }
    m_playback.apply(wallpaper(id), allScreens);
}

void MirageController::toggleSelectedFavorite() {
    if (!m_selectedWallpaperId.isEmpty()) m_favorites.toggle(m_selectedWallpaperId);
}

void MirageController::updateSelectedMetadata(const QString& title, const QVariantList& tags) {
    const Wallpaper item = wallpaper(m_selectedWallpaperId);
    if (!item.isValid()) return;

    QStringList normalizedTags;
    QSet<QString> seen;
    for (const QVariant& value : tags) {
        const QString tag = value.toString().trimmed();
        const QString key = tag.toCaseFolded();
        if (tag.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        normalizedTags.append(tag);
    }
    normalizedTags.sort(Qt::CaseInsensitive);

    QString error;
    if (!m_library.updateMetadata(item, title, normalizedTags, true, true, &error)) {
        setStatusMessage(error);
        return;
    }
    setStatusMessage(QStringLiteral("壁纸信息已更新"));
}

void MirageController::deleteSelectedWallpaper() {
    const Wallpaper item = wallpaper(m_selectedWallpaperId);
    if (!item.isValid()) return;

    QString error;
    if (!m_library.removeImportedWallpaper(item, &error)) {
        setStatusMessage(error);
        return;
    }
    m_favorites.setFavorite(item.id(), false);
    m_trusted.clear(item.id());
    setStatusMessage(QStringLiteral("已删除导入壁纸"));
}

void MirageController::importWallpaperPath(const QString& path) {
    if (path.isEmpty()) return;

    QString error;
    const QString source = path.endsWith(QStringLiteral("/project.json"))
        ? QFileInfo(path).absolutePath()
        : path;
    const QString imported = m_library.importAny(source, &error);
    if (imported.isEmpty()) {
        setStatusMessage(error);
        return;
    }
    reloadWallpapers();
    setStatusMessage(QStringLiteral("已导入 %1").arg(imported));
}

void MirageController::stopWallpapers() {
    m_playback.stopWallpapers();
}

void MirageController::applySelectedToScreen(int screen) {
    m_playback.applySelectedToScreen(screen);
}

void MirageController::stopScreen(int screen) {
    m_playback.stopScreen(screen);
}

void MirageController::applySelectedToDisplay(const QString& displayId) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (int index = 0; index < screens.size(); ++index) {
        if (RendererController::stableOutputId(screens.at(index)) == displayId) {
            m_playback.applySelectedToScreen(index);
            return;
        }
    }
}

void MirageController::applyWallpaperToDisplay(const QString& wallpaperId,
                                               const QString& displayId) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (int index = 0; index < screens.size(); ++index) {
        if (RendererController::stableOutputId(screens.at(index)) == displayId) {
            selectWallpaper(wallpaperId);
            m_playback.applySelectedToScreen(index);
            return;
        }
    }
}

void MirageController::removeWallpaperFromDisplay(const QString& displayId) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (int index = 0; index < screens.size(); ++index) {
        if (RendererController::stableOutputId(screens.at(index)) == displayId) {
            m_playback.stopScreen(index);
            return;
        }
    }
}

void MirageController::beginWallpaperAssignment(const QString& displayId) {
    if (m_wallpaperAssignmentDisplayId == displayId) return;
    m_wallpaperAssignmentDisplayId = displayId;
    emit wallpaperAssignmentChanged();
}

void MirageController::cancelWallpaperAssignment() {
    if (m_wallpaperAssignmentDisplayId.isEmpty()) return;
    m_wallpaperAssignmentDisplayId.clear();
    emit wallpaperAssignmentChanged();
}

void MirageController::addSelectedToPlaylist() {
    const Wallpaper item = wallpaper(m_selectedWallpaperId);
    if (!item.isValid()) return;
    m_playlist.add(item, m_playlistScreen);
    emit playlistChanged();
    setStatusMessage(QStringLiteral("已加入播放列表"));
}

void MirageController::playPlaylistItem(const QString& id) {
    const Wallpaper item = m_playlist.resolveWallpaper(id);
    if (!item.isValid()) return;
    m_playback.playPlaylistItem(item);
}

void MirageController::removePlaylistItem(const QString& id) {
    m_playlist.remove(id, m_playlistScreen);
}

void MirageController::clearPlaylist() {
    m_playlist.clear(m_playlistScreen);
}

void MirageController::trimPlaylistItems(int limit) {
    m_playlist.trimItems(qMax(0, limit), m_playlistScreen);
}

void MirageController::movePlaylistItem(int source, int destination) {
    m_playlist.move(source, destination, m_playlistScreen);
}

void MirageController::savePlaylist(const QString& name) {
    if (m_playlist.saveAs(name, m_playlistScreen).name.isEmpty()) return;
    setStatusMessage(QStringLiteral("播放列表已保存"));
}

void MirageController::loadSavedPlaylist(const QString& id) {
    for (const Playlist& playlist : m_playlist.saved()) {
        if (playlist.id.toString(QUuid::WithoutBraces) != id) continue;
        m_playlist.loadSaved(playlist, m_playlistScreen);
        setStatusMessage(QStringLiteral("已载入播放列表"));
        return;
    }
}

void MirageController::deleteSavedPlaylist(const QString& id) {
    m_playlist.deleteSaved(QUuid(id));
}

void MirageController::updatePlaylistSettings(const QVariantMap& values) {
    m_playlist.updateSettings(m_playlistScreen, [&values](PlaylistSettings& settings) {
        updatePlaylistSettingsFromMap(settings, values);
    });
}

void MirageController::setSelectedProperty(const QString& key, const QVariant& value) {
    m_playback.setSelectedProperty(key, value);
}

void MirageController::resetSelectedProperties() {
    m_playback.resetSelectedProperties();
}

QString MirageController::requestSceneSnapshot(int screenIndex, const QString& path) {
    return m_renderer.requestSnapshot(screenIndex, path);
}

void MirageController::completeFirstLaunch(bool hideUntilNextUpdate) {
    m_firstLaunch = false;
    QSettings().setValue(QStringLiteral("IsFirstLaunch"), !hideUntilNextUpdate);
    emit firstLaunchChanged();
}

void MirageController::loadDiscover() {
    m_workshop.loadDiscover();
}

void MirageController::refreshDiscover() {
    m_workshop.refreshDiscover();
}

void MirageController::setDiscoverTrendDays(int days) {
    m_workshop.setDiscoverTrendDays(days);
}

void MirageController::setWorkshopSearchText(const QString& text) {
    m_workshop.setSearchText(text);
}

void MirageController::submitWorkshopSearch() {
    m_workshop.submitSearch();
}

void MirageController::setWorkshopSortOrder(const QString& key) {
    m_workshop.setSortOrder(workshopSortOrderFor(key));
}

void MirageController::setWorkshopTypeFilter(const QString& key) {
    m_workshop.setTypeFilter(workshopTypeFilterFor(key));
}

void MirageController::setWorkshopAgeRatingEnabled(const QString& key, bool enabled) {
    const std::optional<WorkshopAgeRating> rating = workshopAgeRatingFor(key);
    if (rating) m_workshop.setAgeRatingEnabled(*rating, enabled);
}

void MirageController::toggleWorkshopTag(const QString& tag) {
    m_workshop.toggleTag(tag);
}

void MirageController::clearWorkshopFilters() {
    m_workshop.clearFilters();
}

void MirageController::goToWorkshopPage(int page) {
    m_workshop.goToPage(page);
}

void MirageController::selectWorkshopItem(const QString& id) {
    const std::optional<WorkshopItem> item = workshopItem(id);
    if (item) m_workshop.selectWorkshopItem(*item);
}

void MirageController::downloadWorkshopItem(const QString& id) {
    const std::optional<WorkshopItem> item = workshopItem(id);
    if (item) m_workshop.downloadItem(*item);
}

void MirageController::downloadWorkshopItemById(const QString& id) {
    m_workshop.downloadItemById(id);
}

void MirageController::requestWorkshopPresetDependency(const QString& id) {
    m_workshop.requestPresetDependency(id);
}

void MirageController::cancelWorkshopDownload(const QString& id) {
    m_workshop.cancelDownload(id);
}

void MirageController::retryWorkshopDownload(const QString& id) {
    m_workshop.retryDownload(id);
}

void MirageController::clearCompletedDownloads() {
    m_workshop.clearCompletedDownloads();
}

void MirageController::loginSteamQR() {
    m_steamSetup.loginWithQR();
}

void MirageController::loginSteam(const QString& username, const QString& password) {
    m_steamSetup.login(username, password);
}

void MirageController::submitSteamGuardCode(const QString& code) {
    m_steamSetup.submitGuardCode(code);
}

void MirageController::useSavedSteamSession() {
    m_steamSetup.useSavedSession();
}

void MirageController::cancelSteamLogin() {
    m_steamSetup.cancelLogin();
}

void MirageController::cancelPendingSteamWork() {
    m_steamSetup.cancelPendingWork();
}

void MirageController::logoutSteam() {
    m_steamSetup.logout();
}

void MirageController::copyTextToClipboard(const QString& text) {
    QGuiApplication::clipboard()->setText(text);
}

void MirageController::loadSubscriptions() {
    m_workshop.loadSubscriptions(0);
}

void MirageController::goToSubscriptionPage(int page) {
    m_workshop.goToSubscriptionPage(page);
}

void MirageController::setSubscriptionSearchText(const QString& text) {
    m_workshop.setSubscriptionSearchText(text);
}

void MirageController::setSubscriptionTypeFilter(const QString& key) {
    m_workshop.setSubscriptionTypeFilter(workshopTypeFilterFor(key));
}

void MirageController::setSubscriptionAgeRatingEnabled(const QString& key, bool enabled) {
    const std::optional<WorkshopAgeRating> rating = workshopAgeRatingFor(key);
    if (rating) m_workshop.setSubscriptionAgeRatingEnabled(*rating, enabled);
}

void MirageController::setSubscriptionResolutionOption(int group, int bit, bool enabled) {
    m_workshop.setSubscriptionResolutionOption(group, bit, enabled);
}

void MirageController::selectAllSubscriptionResolutions() {
    m_workshop.selectAllSubscriptionResolutions();
}

void MirageController::clearSubscriptionResolutions() {
    m_workshop.clearSubscriptionResolutions();
}

void MirageController::selectAllSubscriptionTags() {
    m_workshop.selectAllSubscriptionTags();
}

void MirageController::clearSubscriptionTags() {
    m_workshop.clearSubscriptionTags();
}

void MirageController::toggleSubscriptionTag(const QString& tag) {
    m_workshop.toggleSubscriptionTag(tag);
}

void MirageController::clearSubscriptionFilters() {
    m_workshop.clearSubscriptionFilters();
}

void MirageController::downloadAllSubscriptions() {
    m_workshop.downloadAllSubscriptions();
}

void MirageController::confirmSubscriptionDownloads() {
    m_workshop.confirmSubscriptionDownloads();
}

void MirageController::dismissSubscriptionDownloadPlan() {
    m_workshop.dismissSubscriptionDownloadPlan();
}

void MirageController::subscribeWorkshopItem(const QString& id) {
    m_workshop.subscribe(id);
}

void MirageController::unsubscribeWorkshopItem(const QString& id) {
    m_workshop.unsubscribe(id);
}

void MirageController::revealWorkshopDownload(const QString& id) {
    const QStringList directories = m_library.workshopItemDirectories(id);
    for (const QString& directory : directories) {
        if (QFileInfo::exists(directory + QStringLiteral("/project.json"))) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
            return;
        }
    }
}

void MirageController::pauseWallpapers() {
    m_playback.pauseWallpapers();
}

void MirageController::resumeWallpapers() {
    m_playback.resumeWallpapers();
}

void MirageController::muteWallpapers() {
    m_playback.muteWallpapers();
}

void MirageController::handleWindowState(const QString& stableId, quint32 flags) {
    m_playback.handleWindowState(stableId, flags);
}

void MirageController::handleOutputAdded(const DisplayOutputSnapshot& output) {
    m_displayModel.addOutput(output);
    refreshDisplayStates();
    emit displaysChanged();
}

void MirageController::handleOutputUpdated(const DisplayOutputSnapshot& output) {
    m_displayModel.updateOutput(output);
    emit displaysChanged();
}

void MirageController::handleOutputRemoved(const QString& stableId) {
    m_displayModel.removeOutput(stableId);
    emit displaysChanged();
}

void MirageController::refreshDisplayStates() {
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (int row = 0; row < m_displayModel.rowCount(); ++row) {
        const QString stableId = m_displayModel.stableIdAt(row);
        for (int screen = 0; screen < screens.size(); ++screen) {
            if (RendererController::stableOutputId(screens.at(screen)) != stableId) continue;
            const Wallpaper current = m_playlist.currentWallpaper(screen);
            const bool running = m_renderer.isRunningOnScreen(screen);
            m_displayModel.setWallpaperState(stableId, running, running,
                                              current.id(), current.project.title,
                                              current.isValid() ? QUrl::fromLocalFile(current.previewPath()) : QUrl());
            break;
        }
    }
}

bool MirageController::applySettings(const QVariantMap& values) {
    return m_playback.applySettings(values);
}

void MirageController::previewFps(int fps) {
    m_playback.previewFps(fps);
}

void MirageController::setPlaylistScreen(int screen) {
    const int selected = qBound(0, screen, screenCount() - 1);
    if (selected == m_playlistScreen) return;
    m_playlistScreen = selected;
    m_playlist.ensureScreen(m_playlistScreen);
    emit playlistChanged();
}

bool MirageController::showOnStart() const {
    return QSettings().value(QStringLiteral("DisplaySettings/ShowOnStart"), true).toBool();
}

void MirageController::setShowOnStart(bool enabled) {
    if (showOnStart() == enabled) return;
    QSettings().setValue(QStringLiteral("DisplaySettings/ShowOnStart"), enabled);
    emit showOnStartChanged();
}

void MirageController::setSelectedVolume(double volume) {
    m_playback.setSelectedVolume(volume);
}

void MirageController::setSelectedSpeed(double speed) {
    m_playback.setSelectedSpeed(speed);
}

void MirageController::setSelectedFillMode(const QString& mode) {
    m_playback.setSelectedFillMode(mode);
}

void MirageController::setSelectedPositionX(double x) {
    const QPointF current = m_playback.selectedPosition();
    m_playback.setSelectedPosition(QPointF(x, current.y()));
}

void MirageController::setSelectedPositionY(double y) {
    const QPointF current = m_playback.selectedPosition();
    m_playback.setSelectedPosition(QPointF(current.x(), y));
}

Wallpaper MirageController::wallpaper(const QString& id) const {
    for (const Wallpaper& item : m_allWallpapers) {
        if (item.id() == id) return item;
    }
    return {};
}

void MirageController::refreshSelectedWallpaperCache() {
    m_selectedWallpaperCache = wallpaperMap(wallpaper(m_selectedWallpaperId));
}

std::optional<WorkshopItem> MirageController::workshopItem(const QString& id) const {
    const auto find = [&id](const QVector<WorkshopItem>& items) -> std::optional<WorkshopItem> {
        for (const WorkshopItem& item : items) {
            if (item.publishedFileId == id) return item;
        }
        return std::nullopt;
    };
    if (const std::optional<WorkshopItem> item = find(m_workshop.items())) return item;
    for (const DiscoverSectionDefinition& definition : kDiscoverSections) {
        if (const std::optional<WorkshopItem> item = find(m_workshop.discoverItems(definition.collection))) return item;
    }
    // 已订阅项也纳入查找：订阅卡片点击（selectWorkshopItem）与双击下载
    // （downloadWorkshopItem）复用与创意工坊浏览完全相同的查找/选中链路；
    // 可点击的订阅卡片一定来自当前页（GridView model = subscriptions()）。
    if (const std::optional<WorkshopItem> item = find(m_workshop.subscriptions())) return item;
    return std::nullopt;
}





void MirageController::setStatusMessage(const QString& message) {
    if (message.isEmpty()) return;
    m_statusMessage = message;
    emit statusMessageChanged();
}

} // namespace Mirage
