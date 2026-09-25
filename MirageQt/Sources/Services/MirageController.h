#pragma once

#include "Services/FavoritesManager.h"
#include "Services/DisplayOutputModel.h"
#include "Services/GlobalSettingsService.h"
#include "Services/PlaybackController.h"
#include "Services/PlaylistManager.h"
#include "Services/PlaylistModels.h"
#include "Services/RendererController.h"
#include "Services/SteamServiceManager.h"
#include "Services/SteamSetupViewModel.h"
#include "Services/SteamWebAPI.h"
#include "Services/TrustedWallpaperService.h"
#include "Services/WallpaperLibrary.h"
#include "Services/WallpaperRuntimeStore.h"
#include "Services/WorkshopModels.h"
#include "Services/WorkshopViewModel.h"

#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <optional>

namespace Mirage {

// 序列化辅助（定义在 MirageControllerMapping.cpp）：
// MirageController.cpp 的 downloadQueue()/updatePlaylistSettings()/workshopItem()
// 与 Mapping 的序列化成员函数共享同一份实现。
QString downloadStateKey(DownloadStateKind kind);
bool isActiveDownload(DownloadStateKind kind);
void updatePlaylistSettingsFromMap(PlaylistSettings& settings, const QVariantMap& values);
// 发现页区块定义：集合 + 展示标题（inline 定义使所有使用方共享同一数组）。
struct DiscoverSectionDefinition {
    DiscoverCollection collection;
    const char* title;
};
inline const DiscoverSectionDefinition kDiscoverSections[] = {
    {DiscoverCollection::Trending, "本周最热"},
    {DiscoverCollection::MostUpvoted, "最多投票"},
    {DiscoverCollection::MostRecent, "最新上架"},
    {DiscoverCollection::MostSubscribed, "订阅最多"},
    {DiscoverCollection::TopRated, "评分最高"},
    {DiscoverCollection::LastUpdated, "最近更新"},
    {DiscoverCollection::PlaytimeTrend, "本周播放时长最多"},
    {DiscoverCollection::AveragePlaytimeTrend, "本周平均播放时长最长"},
    {DiscoverCollection::SessionsTrend, "本周播放次数最多"},
    {DiscoverCollection::TotalPlaytime, "总播放时长最多"},
    {DiscoverCollection::LifetimeAveragePlaytime, "终身平均播放时长"},
    {DiscoverCollection::LifetimeSessions, "总播放次数最多"},
    {DiscoverCollection::Anime, "动漫精选"},
    {DiscoverCollection::Nature, "自然风光"},
    {DiscoverCollection::Abstract, "抽象艺术"},
    {DiscoverCollection::Landscape, "风景壁纸"},
};

class MirageController : public QObject {
    Q_OBJECT

    friend class PlaybackController;

    Q_PROPERTY(QVariantList wallpapers READ wallpapers NOTIFY wallpapersChanged)
    Q_PROPERTY(QVariantMap selectedWallpaper READ selectedWallpaper NOTIFY selectedWallpaperChanged)
    Q_PROPERTY(QString selectedWallpaperId READ selectedWallpaperId WRITE selectWallpaper NOTIFY selectedWallpaperChanged)
    Q_PROPERTY(QVariantList playlistItems READ playlistItems NOTIFY playlistChanged)
    Q_PROPERTY(QVariantList workshopItems READ workshopItems NOTIFY workshopItemsChanged)
    Q_PROPERTY(QVariantList discoverSections READ discoverSections NOTIFY discoverChanged)
    Q_PROPERTY(QVariantMap selectedWorkshopItem READ selectedWorkshopItem NOTIFY selectedWorkshopItemChanged)
    Q_PROPERTY(bool workshopLoading READ workshopLoading NOTIFY workshopStateChanged)
    Q_PROPERTY(bool discoverLoading READ discoverLoading NOTIFY discoverChanged)
    Q_PROPERTY(QString workshopError READ workshopError NOTIFY workshopStateChanged)
    Q_PROPERTY(int workshopPage READ workshopPage NOTIFY workshopStateChanged)
    Q_PROPERTY(int workshopPageCount READ workshopPageCount NOTIFY workshopStateChanged)
    Q_PROPERTY(int activeDownloadCount READ activeDownloadCount NOTIFY workshopStateChanged)
    Q_PROPERTY(QVariantList downloadQueue READ downloadQueue NOTIFY workshopStateChanged)
    Q_PROPERTY(bool steamReady READ steamReady NOTIFY workshopStateChanged)
    Q_PROPERTY(QString steamSetupSummary READ steamSetupSummary NOTIFY workshopStateChanged)
    Q_PROPERTY(QString steamUsername READ steamUsername NOTIFY steamChanged)
    Q_PROPERTY(bool steamLoggedIn READ steamLoggedIn NOTIFY steamChanged)
    Q_PROPERTY(QString steamLoginState READ steamLoginState NOTIFY steamChanged)
    Q_PROPERTY(QString steamLoginMessage READ steamLoginMessage NOTIFY steamChanged)
    Q_PROPERTY(QString steamGuardType READ steamGuardType NOTIFY steamChanged)
    Q_PROPERTY(QString steamQRCodeUrl READ steamQRCodeUrl NOTIFY steamChanged)
    Q_PROPERTY(bool steamSessionReusable READ steamSessionReusable NOTIFY steamChanged)
    Q_PROPERTY(bool steamServiceRunning READ steamServiceRunning NOTIFY steamChanged)
    Q_PROPERTY(QVariantList subscriptions READ subscriptions NOTIFY subscriptionsChanged)
    Q_PROPERTY(bool subscriptionsLoading READ subscriptionsLoading NOTIFY subscriptionsChanged)
    Q_PROPERTY(int subscriptionTotal READ subscriptionTotal NOTIFY subscriptionsChanged)
    Q_PROPERTY(int subscriptionPage READ subscriptionPage NOTIFY subscriptionsChanged)
    Q_PROPERTY(int subscriptionPageCount READ subscriptionPageCount NOTIFY subscriptionsChanged)
    Q_PROPERTY(QVariantMap subscriptionFilters READ subscriptionFilters NOTIFY subscriptionsChanged)
    Q_PROPERTY(bool subscriptionDownloadPreparing READ subscriptionDownloadPreparing NOTIFY subscriptionsChanged)
    Q_PROPERTY(QVariantMap subscriptionDownloadPlan READ subscriptionDownloadPlan NOTIFY subscriptionsChanged)
    Q_PROPERTY(bool firstLaunch READ firstLaunch NOTIFY firstLaunchChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList selectedProperties READ selectedProperties NOTIFY selectedRuntimeChanged)
    Q_PROPERTY(int playlistScreen READ playlistScreen WRITE setPlaylistScreen NOTIFY playlistChanged)
    Q_PROPERTY(int screenCount READ screenCount NOTIFY displaysChanged)
    Q_PROPERTY(QVariantList displays READ displays NOTIFY displaysChanged)
    Q_PROPERTY(Mirage::DisplayOutputModel* displayModel READ displayModel CONSTANT)
    Q_PROPERTY(QVariantList savedPlaylists READ savedPlaylists NOTIFY playlistsSavedChanged)
    Q_PROPERTY(QVariantMap playlistSettings READ playlistSettings NOTIFY playlistChanged)
    Q_PROPERTY(double selectedVolume READ selectedVolume WRITE setSelectedVolume NOTIFY selectedRuntimeChanged)
    Q_PROPERTY(double selectedSpeed READ selectedSpeed WRITE setSelectedSpeed NOTIFY selectedRuntimeChanged)
    Q_PROPERTY(QString selectedFillMode READ selectedFillMode WRITE setSelectedFillMode NOTIFY selectedRuntimeChanged)
    Q_PROPERTY(double selectedPositionX READ selectedPositionX WRITE setSelectedPositionX NOTIFY selectedRuntimeChanged)
    Q_PROPERTY(double selectedPositionY READ selectedPositionY WRITE setSelectedPositionY NOTIFY selectedRuntimeChanged)
    Q_PROPERTY(bool positionAvailabilityKnown READ positionAvailabilityKnown NOTIFY selectedRuntimeChanged)
    Q_PROPERTY(bool positionXAvailable READ positionXAvailable NOTIFY selectedRuntimeChanged)
    Q_PROPERTY(bool positionYAvailable READ positionYAvailable NOTIFY selectedRuntimeChanged)
    Q_PROPERTY(QString wallpaperAssignmentDisplayId READ wallpaperAssignmentDisplayId NOTIFY wallpaperAssignmentChanged)
    Q_PROPERTY(bool showOnStart READ showOnStart WRITE setShowOnStart NOTIFY showOnStartChanged)

public:
    explicit MirageController(QObject* parent = nullptr);
    ~MirageController() override;

    /* Starts persisted playback after DisplayBrokerService has successfully
     * bound its socket. Renderer producers must never race broker startup. */
    void startPlayback();

    QVariantList wallpapers() const;
    QVariantMap selectedWallpaper() const;
    QString selectedWallpaperId() const;
    QVariantList playlistItems() const;
    QVariantList workshopItems() const;
    QVariantList discoverSections() const;
    QVariantMap selectedWorkshopItem() const;
    bool workshopLoading() const;
    bool discoverLoading() const;
    QString workshopError() const;
    int workshopPage() const;
    int workshopPageCount() const;
    int activeDownloadCount() const;
    QVariantList downloadQueue() const;
    bool steamReady() const;
    QString steamSetupSummary() const;
    QString steamUsername() const;
    bool steamLoggedIn() const;
    QString steamLoginState() const;
    QString steamLoginMessage() const;
    QString steamGuardType() const;
    QString steamQRCodeUrl() const;
    bool steamSessionReusable() const;
    bool steamServiceRunning() const;
    QVariantList subscriptions() const;
    bool subscriptionsLoading() const;
    int subscriptionTotal() const;
    int subscriptionPage() const;
    int subscriptionPageCount() const;
    QVariantMap subscriptionFilters() const;
    bool subscriptionDownloadPreparing() const;
    QVariantMap subscriptionDownloadPlan() const;
    bool firstLaunch() const;
    QString statusMessage() const;
    QVariantMap settings() const;
    QVariantList selectedProperties() const;
    int playlistScreen() const;
    int screenCount() const;
    QVariantList displays() const;
    DisplayOutputModel* displayModel() { return &m_displayModel; }
    QVariantList savedPlaylists() const;
    QVariantMap playlistSettings() const;
    double selectedVolume() const;
    double selectedSpeed() const;
    QString selectedFillMode() const;
    double selectedPositionX() const;
    double selectedPositionY() const;
    bool positionAvailabilityKnown() const;
    bool positionXAvailable() const;
    bool positionYAvailable() const;
    QString wallpaperAssignmentDisplayId() const { return m_wallpaperAssignmentDisplayId; }
    bool showOnStart() const;

    Q_INVOKABLE void reloadWallpapers();
    Q_INVOKABLE void selectWallpaper(const QString& id);
    Q_INVOKABLE bool isWallpaperTrusted(const QString& id) const;
    Q_INVOKABLE void trustWallpaper(const QString& id, bool persist);
    Q_INVOKABLE void applySelected(bool allScreens = false);
    Q_INVOKABLE void applyWallpaper(const QString& id, bool allScreens = false);
    Q_INVOKABLE void toggleSelectedFavorite();
    Q_INVOKABLE void updateSelectedMetadata(const QString& title, const QVariantList& tags);
    Q_INVOKABLE void deleteSelectedWallpaper();
    Q_INVOKABLE void importWallpaperPath(const QString& path);
    Q_INVOKABLE void stopWallpapers();
    Q_INVOKABLE void applySelectedToScreen(int screen);
    Q_INVOKABLE void stopScreen(int screen);
    Q_INVOKABLE void applySelectedToDisplay(const QString& displayId);
    Q_INVOKABLE void applyWallpaperToDisplay(const QString& wallpaperId, const QString& displayId);
    Q_INVOKABLE void removeWallpaperFromDisplay(const QString& displayId);
    Q_INVOKABLE void beginWallpaperAssignment(const QString& displayId);
    Q_INVOKABLE void cancelWallpaperAssignment();
    Q_INVOKABLE void addSelectedToPlaylist();
    Q_INVOKABLE void playPlaylistItem(const QString& id);
    Q_INVOKABLE void removePlaylistItem(const QString& id);
    Q_INVOKABLE void clearPlaylist();
    Q_INVOKABLE void trimPlaylistItems(int limit);
    Q_INVOKABLE void movePlaylistItem(int source, int destination);
    Q_INVOKABLE void savePlaylist(const QString& name);
    Q_INVOKABLE void loadSavedPlaylist(const QString& id);
    Q_INVOKABLE void deleteSavedPlaylist(const QString& id);
    Q_INVOKABLE void updatePlaylistSettings(const QVariantMap& values);
    Q_INVOKABLE void setSelectedProperty(const QString& key, const QVariant& value);
    Q_INVOKABLE void resetSelectedProperties();
    // GUI-thread QML API. Requests a PNG from the active Scene renderer on
    // `screenIndex`; `path` is a non-empty caller-owned local filesystem path.
    // Returns a UUID token, or an empty string when no matching renderer exists.
    // Completion is asynchronous through sceneSnapshotFinished and never runs
    // on a renderer or DBus worker thread.
    Q_INVOKABLE QString requestSceneSnapshot(int screenIndex, const QString& path);
    Q_INVOKABLE void completeFirstLaunch(bool hideUntilNextUpdate);
    Q_INVOKABLE void loadDiscover();
    Q_INVOKABLE void refreshDiscover();
    Q_INVOKABLE void setDiscoverTrendDays(int days);
    Q_INVOKABLE void setWorkshopSearchText(const QString& text);
    Q_INVOKABLE void submitWorkshopSearch();
    Q_INVOKABLE void setWorkshopSortOrder(const QString& key);
    Q_INVOKABLE void setWorkshopTypeFilter(const QString& key);
    Q_INVOKABLE void setWorkshopAgeRatingEnabled(const QString& key, bool enabled);
    Q_INVOKABLE void toggleWorkshopTag(const QString& tag);
    Q_INVOKABLE void clearWorkshopFilters();
    // QML 在 GUI 线程传入目标页码；WorkshopViewModel 将其限制到
    // [1, workshopPageCount] 并加载该页。返回 void，同页调用不会发起
    // 网络请求，加载失败通过 workshopError/workshopStateChanged 向 QML 报告。
    Q_INVOKABLE void goToWorkshopPage(int page);
    Q_INVOKABLE void selectWorkshopItem(const QString& id);
    Q_INVOKABLE void downloadWorkshopItem(const QString& id);
    Q_INVOKABLE void downloadWorkshopItemById(const QString& id);
    Q_INVOKABLE void requestWorkshopPresetDependency(const QString& id);
    Q_INVOKABLE void cancelWorkshopDownload(const QString& id);
    Q_INVOKABLE void retryWorkshopDownload(const QString& id);
    Q_INVOKABLE void clearCompletedDownloads();
    Q_INVOKABLE void loginSteamQR();
    Q_INVOKABLE void loginSteam(const QString& username, const QString& password);
    Q_INVOKABLE void submitSteamGuardCode(const QString& code);
    Q_INVOKABLE void useSavedSteamSession();
    Q_INVOKABLE void cancelSteamLogin();
    Q_INVOKABLE void cancelPendingSteamWork();
    Q_INVOKABLE void logoutSteam();
    Q_INVOKABLE void copyTextToClipboard(const QString& text);
    Q_INVOKABLE void loadSubscriptions();
    Q_INVOKABLE void goToSubscriptionPage(int page);
    Q_INVOKABLE void subscribeWorkshopItem(const QString& id);
    Q_INVOKABLE void unsubscribeWorkshopItem(const QString& id);
    Q_INVOKABLE void setSubscriptionSearchText(const QString& text);
    Q_INVOKABLE void setSubscriptionTypeFilter(const QString& key);
    Q_INVOKABLE void setSubscriptionAgeRatingEnabled(const QString& key, bool enabled);
    Q_INVOKABLE void setSubscriptionResolutionOption(int group, int bit, bool enabled);
    Q_INVOKABLE void selectAllSubscriptionResolutions();
    Q_INVOKABLE void clearSubscriptionResolutions();
    Q_INVOKABLE void selectAllSubscriptionTags();
    Q_INVOKABLE void clearSubscriptionTags();
    Q_INVOKABLE void toggleSubscriptionTag(const QString& tag);
    Q_INVOKABLE void clearSubscriptionFilters();
    Q_INVOKABLE void downloadAllSubscriptions();
    Q_INVOKABLE void confirmSubscriptionDownloads();
    Q_INVOKABLE void dismissSubscriptionDownloadPlan();
    Q_INVOKABLE void revealWorkshopDownload(const QString& id);
    Q_INVOKABLE void pauseWallpapers();
    Q_INVOKABLE void resumeWallpapers();
    Q_INVOKABLE void muteWallpapers();
    Q_INVOKABLE void previewFps(int fps);
    Q_INVOKABLE bool applySettings(const QVariantMap& values);

public slots:
    void setPlaylistScreen(int screen);
    void setSelectedVolume(double volume);
    void setSelectedSpeed(double speed);
    void setSelectedFillMode(const QString& mode);
    void setSelectedPositionX(double x);
    void setSelectedPositionY(double y);
    // 桌面窗口事实（由 main.cpp 从 DisplayBrokerService 接入），转发给
    // PlaybackController 按播放规则驱动渲染器。
    void handleWindowState(const QString& stableId, quint32 flags);
    void handleOutputAdded(const Mirage::DisplayOutputSnapshot& output);
    void handleOutputUpdated(const Mirage::DisplayOutputSnapshot& output);
    void handleOutputRemoved(const QString& stableId);
    void setShowOnStart(bool enabled);

signals:
    void wallpapersChanged();
    void selectedWallpaperChanged();
    void playlistChanged();
    void workshopItemsChanged();
    void discoverChanged();
    void selectedWorkshopItemChanged();
    void installedWallpaperSelected();
    void workshopStateChanged();
    void firstLaunchChanged();
    void statusMessageChanged();
    void settingsChanged();
    void selectedRuntimeChanged();
    void playlistsSavedChanged();
    void displaysChanged();
    void steamChanged();
    void subscriptionsChanged();
    void wallpaperAssignmentChanged();
    void showOnStartChanged();
    void playbackPausedChanged(bool paused);
    // QML completion for requestSceneSnapshot. `path` remains owned by the
    // caller; success means the renderer closed a complete PNG at that path.
    void sceneSnapshotFinished(const QString& token, int screenIndex, const QString& path,
                               bool success, const QString& error);

private:
    Wallpaper wallpaper(const QString& id) const;
    QVariantMap wallpaperMap(const Wallpaper& wallpaper) const;
    std::optional<WorkshopItem> workshopItem(const QString& id) const;
    QVariantMap workshopItemMap(const WorkshopItem& item) const;
    QVariantMap playlistMap(const Playlist& playlist) const;
    QVariantMap propertyMap(const QString& key, const ProjectProperty& property) const;
    void setStatusMessage(const QString& message);

    // 重建 m_selectedWallpaperCache。必须在任何 emit selectedWallpaperChanged
    // 之前调用：QML 侧有约 13 处独立绑定引用 mirage.selectedWallpaper，每次
    // 求值都会调用 getter；若 getter 直接走 wallpaperMap()（含 3 次文件 stat
    // 与 O(N) 线性扫描），一次选中变化就会在 GUI 线程重复执行该重活导致卡顿。
    // 缓存后 getter 仅返回 QVariantMap（隐式共享，拷贝 O(1)）。
    // 线程假设：mirage 仅由 GUI 线程创建与访问，QML 绑定在 GUI 线程求值，
    // 无需加锁。
    void refreshSelectedWallpaperCache();
    // 重建 m_selectedPropertiesCache。必须在任何 emit selectedRuntimeChanged
    // 之前调用：PropertyEditor 中每个属性 delegate 的 visible 绑定与每个
    // combo 的 optionItems 绑定都会各自调用一次 selectedProperties() getter
    // （数量随属性数线性放大），而 getter 原实现每次执行 effectiveProperties()
    // ——对 workshop preset 壁纸含读盘解析 project.json 与多次文件 stat。
    // 重建本身仍执行该重活，但由 emit 前的一次集中调用替代 QML 侧的
    // 多次重复调用；重复解析的残余成本由 WallpaperRuntimeStore 层缓存兜底。
    void refreshSelectedPropertiesCache();
    // 重建 m_wallpapersCache。必须在任何 emit wallpapersChanged 之前调用：
    // 一次 wallpapersChanged（收藏切换/库刷新）后 QML 可能有多个独立绑定
    // 引用 mirage.wallpapers，每个绑定各调用一次 getter；若 getter 每次都
    // 对全部壁纸重建 QVariantMap（每壁纸 3 次文件 stat），收藏切换会重复
    // 执行该全量重活。重建本身无法避免单次全量序列化（favorite/size 字段
    // 变化必须重算），缓存只消除信号后的重复调用。
    void refreshWallpapersCache();
    void refreshDisplayStates();

    GlobalSettingsService m_settings;
    FavoritesManager m_favorites;
    WallpaperLibrary m_library;
    SteamServiceManager m_steamService;
    SteamWebAPI m_steamAPI;
    WorkshopViewModel m_workshop;
    RendererController m_renderer;
    WallpaperRuntimeStore m_runtimeStore;
    PlaylistManager m_playlist;
    TrustedWallpaperService m_trusted;
    SteamSetupViewModel m_steamSetup;
    PlaybackController m_playback;
    DisplayOutputModel m_displayModel;
    QVector<Wallpaper> m_allWallpapers;
    QString m_selectedWallpaperId;
    // 选中壁纸的序列化缓存（mutable：getter 为 const）。失效时机见
    // refreshSelectedWallpaperCache() 注释；此处仅存储值，无所有权语义。
    mutable QVariantMap m_selectedWallpaperCache;
    // 选中壁纸属性列表缓存（mutable：getter 为 const）。失效时机见
    // refreshSelectedPropertiesCache() 注释。
    mutable QVariantList m_selectedPropertiesCache;
    // 全量壁纸列表序列化缓存（mutable：getter 为 const）。失效时机见
    // refreshWallpapersCache() 注释。
    mutable QVariantList m_wallpapersCache;
    int m_playlistScreen = 0;
    bool m_firstLaunch = true;
    QString m_statusMessage;
    QString m_wallpaperAssignmentDisplayId;
};

} // namespace Mirage
