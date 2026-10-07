import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import FluentUI
import "ContentViewLogic.js" as ContentViewLogic
import "OptionData.js" as OptionData
import "Components"
import "Components/Discover"
import "Components/Workshop"
import "Components/Playlist"
import "Components/ContextMenus"
import "Components/Alerts"
import "ViewModels"
import "../SettingsView"
import "../SteamSetup"
import "../GlobalComponents"

FluWindow {
    id: window
    title: "Mirage"
    // Matches MainWindowController's initial and minimum content size.
    width: 1000
    height: 640
    minimumWidth: 1000
    minimumHeight: 640
    // The status bar restores this root window after it is closed. FluentUI's
    // default destroys the window on close, leaving the tray with no target.
    autoDestroy: false
    // Wayland destroys the native surface asynchronously when a window is
    // hidden. Reject the close first and defer hiding until close-event
    // delivery has finished, so Qt Quick cannot expose the window again with
    // the stale EGL/Vulkan surface retained by the closing path.
    closeListener: function(event) {
        event.accepted = false;
        Qt.callLater(function() {
            window.hide();
        });
    }
    fitsAppBarWindows: true
    appBar: FluAppBar {
        id: appBar
        // 与 FluAppBar 默认高度一致，窗口按钮组贴顶，标签栏 30px 正好填满。
        height: 30
        titleVisible: false
        icon: ""
        showDark: true
        showStayTop: true
    TopTabBar {
            id: appBarTabs
            height: 30
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.right: appBar.layoutStandardbuttons.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            currentIndex: window.currentTab
            onSelected: index => window.currentTab = index
            onMobileRequested: linuxNotice.open()
            onDisplayRequested: displaySettingsSheet.open()
            onSettingsRequested: settingsSheet.open()
        }
        Component.onCompleted: {
            appBarTabs.registerHitTest(window);
        }
    }

    property bool displayStartupDialogShown: false
    Connections {
        target: mirage.displayModel
        function onRowsInserted() {
            if (!window.displayStartupDialogShown && mirage.showOnStart && mirage.displayModel.count > 0) {
                window.displayStartupDialogShown = true
                displaySettingsSheet.open()
            }
        }
    }

    ContentViewModel {
        id: contentViewModel
    }
    property alias currentTab: contentViewModel.currentTab
    property alias filtersVisible: contentViewModel.filtersVisible
    property alias playlistExpanded: contentViewModel.playlistExpanded
    property alias searchText: contentViewModel.searchText
    property alias approvedOnly: contentViewModel.approvedOnly
    property alias favoritesOnly: contentViewModel.favoritesOnly
    property alias mobileOnly: contentViewModel.mobileOnly
    property alias audioOnly: contentViewModel.audioOnly
    property alias customizableOnly: contentViewModel.customizableOnly
    property alias typeFilters: contentViewModel.typeFilters
    property alias ratingFilters: contentViewModel.ratingFilters
    property alias sourceFilters: contentViewModel.sourceFilters
    property alias tagFilters: contentViewModel.tagFilters
    property alias enabledTypes: contentViewModel.enabledTypes
    property alias enabledRatings: contentViewModel.enabledRatings
    property alias enabledSources: contentViewModel.enabledSources
    property alias enabledTags: contentViewModel.enabledTags
    property alias widescreenMask: contentViewModel.widescreenMask
    property alias ultraWidescreenMask: contentViewModel.ultraWidescreenMask
    property alias dualscreenMask: contentViewModel.dualscreenMask
    property alias triplescreenMask: contentViewModel.triplescreenMask
    property alias portraitMask: contentViewModel.portraitMask
    property alias miscMask: contentViewModel.miscMask
    property alias sortMode: contentViewModel.sortMode
    property alias sortDescending: contentViewModel.sortDescending
    property alias installedSortOptions: contentViewModel.installedSortOptions
    property alias rawWallpapers: contentViewModel.rawWallpapers
    property alias filteredWallpapers: contentViewModel.filteredWallpapers
    property alias explorerIconSize: contentViewModel.explorerIconSize
    property alias wallpaperCurrentPage: contentViewModel.wallpaperCurrentPage
    property alias wallpaperPageCount: contentViewModel.wallpaperPageCount
    property alias pagedWallpapers: contentViewModel.pagedWallpapers
    property alias workshopSearchText: contentViewModel.workshopSearchText
    property alias workshopSortKey: contentViewModel.workshopSortKey
    property alias workshopType: contentViewModel.workshopType
    property alias workshopRatings: contentViewModel.workshopRatings
    property alias workshopSelectedTags: contentViewModel.workshopSelectedTags
    property alias discoverTrendDays: contentViewModel.discoverTrendDays
    property alias settingsPage: contentViewModel.settingsPage
    property alias settingsDraft: contentViewModel.settingsDraft
    property alias settingsDirty: contentViewModel.settingsDirty
    property alias playlistSettingsDraft: contentViewModel.playlistSettingsDraft
    property alias playlistSaveName: contentViewModel.playlistSaveName
    property alias metadataTitle: contentViewModel.metadataTitle
    property alias metadataTags: contentViewModel.metadataTags
    property alias pendingTrustAction: contentViewModel.pendingTrustAction
    property alias pendingTrustWallpaperId: contentViewModel.pendingTrustWallpaperId

    function openImportPanel() {
        importFileDialog.open();
    }

    function openImportFolderPanel() {
        importFolderDialog.open();
    }

    function openPlaylistOpenSheet() {
        playlistOpenSheet.open();
    }

    function openPlaylistSaveSheet() {
        playlistSaveSheet.open();
    }

    function openPlaylistSettingsSheet() {
        playlistSettingsSheet.open();
    }

    function openPlaylistDeleteConfirmation(playlistId, playlistName) {
        playlistDeleteConfirmation.playlistId = playlistId;
        playlistDeleteConfirmation.playlistName = playlistName;
        playlistDeleteConfirmation.open();
    }

    function openSettingsPanel() {
        settingsSheet.open();
    }

    function openSteamSetup() {
        steamSetupController.open();
    }

    function showLinuxNotice() {
        linuxNotice.open();
    }

    function openWallpaperContextMenu(wallpaper) {
        wallpaperContextMenu.wallpaper = wallpaper;
        wallpaperContextMenu.popup();
    }

    function openPropertyPicker(propertyKey, directory) {
        if (directory) {
            propertyFolderDialog.propertyKey = propertyKey;
            propertyFolderDialog.open();
        } else {
            propertyFileDialog.propertyKey = propertyKey;
            propertyFileDialog.open();
        }
    }
    property var playlistOrderOptions: OptionData.playlistOrderOptions
    property var playlistTimingOptions: OptionData.playlistTimingOptions
    property var playlistTransitionOptions: OptionData.playlistTransitionOptions
    property var playbackOptions: OptionData.playbackOptions
    property var playbackModes: OptionData.playbackModes
    property var weekdayLabels: OptionData.weekdayLabels
    property var workshopSortOptions: OptionData.workshopSortOptions
    property var workshopTypeFilters: OptionData.workshopTypeFilters
    property var workshopTagFilters: OptionData.workshopTagFilters

    function isEnabled(filters, key) {
        return ContentViewLogic.isEnabled(filters, key);
    }

    function setEnabled(propertyName, key, enabled) {
        window[propertyName] = ContentViewLogic.setEnabled(window[propertyName], key, enabled);
    }

    function resetFilters() {
        approvedOnly = false;
        favoritesOnly = false;
        mobileOnly = false;
        audioOnly = false;
        customizableOnly = false;
        enabledTypes = typeFilters.map(function (filter) {
            return filter.key;
        });
        enabledRatings = ratingFilters.map(function (filter) {
            return filter.key;
        });
        enabledSources = sourceFilters.map(function (filter) {
            return filter.key;
        });
        enabledTags = tagFilters.map(function (filter) {
            return filter.key;
        });
        selectAllResolutions();
    }

    // 分辨率筛选（已安装壁纸）：掩码以 bit 位表示，maskKey 来自
    // OptionData.resolutionGroups，属性名为 maskKey + "Mask"。
    // 与订阅侧 SubscribedWorkshopFilterSidebar 的分辨率逻辑同构。
    function setResolutionOption(maskKey, index, enabled) {
        var mask = window[maskKey + "Mask"];
        window[maskKey + "Mask"] = enabled ? (mask | (1 << index)) : (mask & ~(1 << index));
    }

    function selectAllResolutions() {
        OptionData.resolutionGroups.forEach(function (group) {
            window[group.maskKey + "Mask"] = (1 << group.options.length) - 1;
        });
    }

    function clearResolutions() {
        OptionData.resolutionGroups.forEach(function (group) {
            window[group.maskKey + "Mask"] = 0;
        });
    }

    function propertyLabel(propertyData) {
        return ContentViewLogic.propertyLabel(propertyData);
    }

    function propertyOptionItems(options) {
        return ContentViewLogic.propertyOptionItems(options, mirage.selectedProperties);
    }

    Connections {
        target: mirage
        // 属性集变化（选中不同壁纸、编辑属性值）时使 ContentViewLogic 的条件
        // 判定缓存失效；语义与 macOS WEConditionEvaluator 的 updateContext
        // 清缓存一致。selectedRuntimeChanged 也会因音量/速度等变化触发，此时
        // 属性判定并未改变，清空缓存只是多一次重建，无害。
        function onSelectedRuntimeChanged() { ContentViewLogic.invalidatePropertyConditionCache(); }
        function onSelectedWallpaperChanged() { ContentViewLogic.invalidatePropertyConditionCache(); }
    }

    function resetDetailScroll() {
        Qt.callLater(function () {
            if (detailScroll.contentItem)
                detailScroll.contentItem.contentY = 0;
        });
    }

    function workshopSortIndex() {
        return ContentViewLogic.findOptionIndex(workshopSortOptions, workshopSortKey);
    }

    function setWorkshopRating(key, enabled) {
        setEnabled("workshopRatings", key, enabled);
        mirage.setWorkshopAgeRatingEnabled(key, enabled);
    }

    function toggleWorkshopTag(key) {
        setEnabled("workshopSelectedTags", key, !isEnabled(workshopSelectedTags, key));
        mirage.toggleWorkshopTag(key);
    }

    function selectAllWorkshopTags() {
        workshopTagFilters.forEach(function(filter) {
            if (!isEnabled(workshopSelectedTags, filter.key))
                toggleWorkshopTag(filter.key);
        });
    }

    function clearWorkshopTags() {
        workshopSelectedTags.slice().forEach(function(tag) {
            toggleWorkshopTag(tag);
        });
    }

    function filterDiscoverItems(items) {
        return ContentViewLogic.filterDiscoverItems(items, {
            workshopType: workshopType,
            workshopRatings: workshopRatings,
            workshopSelectedTags: workshopSelectedTags
        });
    }

    function resetWorkshopFilters() {
        workshopSearchText = "";
        workshopSortKey = "trending";
        workshopType = "all";
        workshopRatings = ["Everyone"];
        workshopSelectedTags = [];
        mirage.clearWorkshopFilters();
    }

    function resetSettingsDraft() {
        var next = {};
        for (var name in mirage.settings)
            next[name] = mirage.settings[name];
        settingsDraft = next;
        settingsDirty = false;
    }

    function cancelSettingsDraft() {
        mirage.previewFps(mirage.settings.fps);
        resetSettingsDraft();
    }

    function setSetting(key, value) {
        if (settingsDraft[key] === value)
            return;
        var next = {};
        for (var name in settingsDraft)
            next[name] = settingsDraft[name];
        next[key] = value;
        settingsDraft = next;
        settingsDirty = true;
        if (key === "fps")
            mirage.previewFps(value);
    }

    function playlistOptionIndex(options, value) {
        return ContentViewLogic.findOptionIndex(options, value);
    }

    function resetPlaylistSettingsDraft() {
        var next = {};
        var source = mirage.playlistSettings || {};
        for (var key in source) {
            next[key] = Array.isArray(source[key]) ? source[key].slice() : source[key];
        }
        playlistSettingsDraft = next;
    }

    function setPlaylistSetting(key, value) {
        var next = {};
        for (var name in playlistSettingsDraft) {
            next[name] = Array.isArray(playlistSettingsDraft[name]) ? playlistSettingsDraft[name].slice() : playlistSettingsDraft[name];
        }
        next[key] = value;
        playlistSettingsDraft = next;
    }

    function screenLabels() {
        return ContentViewLogic.screenLabels(mirage.screenCount);
    }

    function propertyConditionVisible(condition) {
        return ContentViewLogic.propertyConditionVisible(condition, mirage.selectedProperties);
    }

    function propertyOptionIndex(options, value) {
        return ContentViewLogic.findOptionIndex(options, value, "value");
    }

    function propertyBoolValue(value) {
        return ContentViewLogic.propertyBoolValue(value);
    }

    function propertyColor(value) {
        return ContentViewLogic.propertyColor(value);
    }

    function propertyColorValue(color) {
        return ContentViewLogic.propertyColorValue(color);
    }

    function propertyPathLabel(value) {
        return ContentViewLogic.propertyPathLabel(value);
    }

    function needsWallpaperTrust(wallpaper) {
        return wallpaper && wallpaper.type === "web" && !mirage.isWallpaperTrusted(wallpaper.id);
    }

    function runWithWallpaperTrust(wallpaper, action) {
        if (!wallpaper || !wallpaper.id)
            return;
        if (!needsWallpaperTrust(wallpaper)) {
            action();
            return;
        }
        pendingTrustWallpaperId = wallpaper.id;
        pendingTrustAction = action;
        wallpaperTrustSheet.open();
    }

    function confirmWallpaperTrust() {
        var action = pendingTrustAction;
        var wallpaperId = pendingTrustWallpaperId;
        pendingTrustAction = null;
        pendingTrustWallpaperId = "";
        mirage.trustWallpaper(wallpaperId, wallpaperTrustSheet.rememberWallpaper);
        if (action)
            action();
    }

    function cancelWallpaperTrust() {
        pendingTrustAction = null;
        pendingTrustWallpaperId = "";
    }

    function addWallpaperToPlaylist(id, screen) {
        var previousScreen = mirage.playlistScreen;
        mirage.selectWallpaper(id);
        mirage.playlistScreen = screen;
        mirage.addSelectedToPlaylist();
        mirage.playlistScreen = previousScreen;
    }

    // SharedBrowseControls 传入经 IntValidator 约束的数字页码；此处
    // 仍按当前页数限制边界，以覆盖筛选使总页数同步缩小的情况。
    function setWallpaperPage(page) {
        wallpaperCurrentPage = Math.max(1, Math.min(wallpaperPageCount, Math.floor(Number(page))));
    }

    onFilteredWallpapersChanged: wallpaperCurrentPage = 1
    onWallpaperPageCountChanged: {
        if (wallpaperCurrentPage > wallpaperPageCount)
            wallpaperCurrentPage = wallpaperPageCount;
    }

    onCurrentTabChanged: {
        if (currentTab === 1 && !mirage.discoverLoading && mirage.discoverSections.length === 0) {
            mirage.loadDiscover();
        } else if (currentTab === 2 && !mirage.workshopLoading && mirage.workshopItems.length === 0) {
            mirage.submitWorkshopSearch();
        } else if (currentTab === 3 && mirage.steamLoggedIn
                   && !mirage.subscriptionsLoading && mirage.subscriptions.length === 0) {
            // 已订阅 tab：首次进入且无缓存内容时加载（对齐 macOS loadedSections 惰性加载）。
            mirage.loadSubscriptions();
        }
        if (currentTab !== 1) {
            explorerTopBar.searchBox.text = currentTab === 0 ? searchText : workshopSearchText;
        }
        resetDetailScroll();
    }

    Connections {
        target: mirage
        function onSettingsChanged() {
            if (!settingsDirty)
                window.resetSettingsDraft();
        }
        function onInstalledWallpaperSelected() {
            window.currentTab = 0;
        }
        function onSelectedWallpaperChanged() {
            window.resetDetailScroll();
        }
        function onSelectedWorkshopItemChanged() {
            window.resetDetailScroll();
        }
    }

    ExplorerItemMenu {
        id: wallpaperContextMenu
        host: window
        onUnavailableFeatureRequested: linuxNotice.open()
        onDeleteRequested: wallpaperDeleteConfirmation.open()
    }

    FluSplitLayout {
        id: mainSplit
        anchors.fill: parent
        orientation: Qt.Horizontal

        Item {
            SplitView.minimumWidth: 620
            SplitView.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                anchors.bottomMargin: 16
                anchors.topMargin: appBar.height

                ProjectFeedbackBanner {
                    Layout.fillWidth: true
                    Layout.preferredHeight: implicitHeight
                }

                ExplorerTopBar {
                    id: explorerTopBar
                    Layout.fillWidth: true
                    visible: window.currentTab === 0
                    host: window
                    currentTab: window.currentTab
                    searchText: window.currentTab === 0 ? window.searchText : window.workshopSearchText
                    explorerIconSize: window.explorerIconSize
                    sortMode: window.sortMode
                    sortDescending: window.sortDescending
                    onFilterRequested: window.filtersVisible = !window.filtersVisible
                    onRefreshRequested: {
                        if (window.currentTab === 0)
                            mirage.reloadWallpapers();
                        else
                            mirage.submitWorkshopSearch();
                    }
                    onIconSizeChanged: size => window.explorerIconSize = size
                    onSortChanged: key => {
                        if (key === "direction") {
                            window.sortDescending = !window.sortDescending;
                        } else {
                            var sortIndex = ContentViewLogic.findOptionIndex(window.installedSortOptions, key, "label");
                            window.sortMode = window.installedSortOptions[sortIndex].key;
                        }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: window.currentTab

                    RowLayout {
                        spacing: 10

                        // FluPage（FluScrollablePage 基类）onCompleted 会强制
                        // visible=true，永久破坏外部 visible 绑定，导致侧栏常驻
                        // 无法收起（同 SettingsView.qml 的页面可见性问题）。
                        // 故用普通 Item 承载显隐，FluScrollablePage 内部
                        // anchors.fill 即可，Item 的 visible 绑定不受影响。
                        Item {
                            Layout.fillHeight: true
                            Layout.preferredWidth: 225
                            visible: window.filtersVisible

                            FilterResults {
                                anchors.fill: parent
                                host: window
                            }
                        }

                        WallpaperExplorer {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            host: window
                        }
                    }

                    RowLayout {
                        spacing: 10
                        // 同 tab 0：FluPage 基类 onCompleted 强制 visible=true，
                        // 需用 Item 承载显隐，否则侧栏无法收起。
                        Item {
                            Layout.fillHeight: true
                            Layout.preferredWidth: 225
                            visible: window.filtersVisible

                            WorkshopFilterSidebar {
                                anchors.fill: parent
                                host: window
                            }
                        }
                        DiscoverView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            host: window
                        }
                    }

                    RowLayout {
                        spacing: 10
                        // 同 tab 0：FluPage 基类 onCompleted 强制 visible=true，
                        // 需用 Item 承载显隐，否则侧栏无法收起。
                        Item {
                            Layout.fillHeight: true
                            Layout.preferredWidth: 225
                            visible: window.filtersVisible

                            WorkshopFilterSidebar {
                                anchors.fill: parent
                                host: window
                            }
                        }
                        WorkshopView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            host: window
                        }
                    }

                    // 已订阅独立 tab（对齐 macOS MainSection.subscriptions）。
                    RowLayout {
                        spacing: 10
                        Item {
                            Layout.fillHeight: true
                            Layout.preferredWidth: 225
                            visible: window.filtersVisible

                            SubscribedWorkshopFilterSidebar {
                                anchors.fill: parent
                                host: window
                            }
                        }
                        SubscribedWorkshopView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            host: window
                        }
                    }
                }

                ExplorerBottomBar {
                    host: window
                }
            }
        }

        FluScrollablePage {
            id: detailScroll
            SplitView.minimumWidth: 288
            SplitView.preferredWidth: 320
            SplitView.maximumWidth: 340
            SplitView.fillHeight: true

            clip: true

            // 滚动模型：内容高度取当前可见内容的自然高度（implicitHeight），
            // 与 macOS ScrollView + VStack 一致，超高时可滚动。
            // 不要用 columnHeight 强制内容高度 = 视口高度：FluScrollablePage
            // 的 contentHeight 即 container.height，被强制等于视口后滚动范围
            // 恒为 0，滚动条形同虚设（"内容超高但滚不动"的根因）。
            // 也不能用 fillHeight + anchors.fill：Item 高度由容器给、容器
            // 高度由内容 implicitHeight 给、内容又 anchors.fill 依赖 Item
            // 尺寸，形成循环依赖，内容高度会塌缩为 0（此前内容不可见的
            // 根因，也是当初加 columnHeight: parent.height 的原因）。
            // 子项高度自取自身 implicitHeight、Item 高度绑定可见子项的
            // implicitHeight，打破循环。
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: (window.currentTab === 0 ? wallpaperPreview : workshopDetail).implicitHeight
                Layout.topMargin: appBar.height + 16
                Layout.bottomMargin: 16
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                WallpaperPreview {
                    id: wallpaperPreview
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: implicitHeight
                    visible: window.currentTab === 0
                    host: window
                    onMetadataEditRequested: wallpaperMetadataSheet.open()
                    onDeleteRequested: wallpaperDeleteConfirmation.open()
                }

                WorkshopItemDetail {
                    id: workshopDetail
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: implicitHeight
                    visible: window.currentTab !== 0
                    host: window
                }
            }
        }
    }

    FirstLaunchView {
        id: firstLaunchSheet
        host: window
    }

    SettingsView {
        id: settingsSheet
        host: window
    }
    WallpaperMetadataSheet {
        id: wallpaperMetadataSheet
        host: window
    }

    DeleteWallpaperDialog {
        id: wallpaperDeleteConfirmation
    }

    UnsafeWallpaper {
        id: wallpaperTrustSheet
        host: window
    }

    DisplaySettings {
        id: displaySettingsSheet
        host: window
    }

    PlaylistOpenSheet {
        id: playlistOpenSheet
        host: window
    }
    PlaylistDeleteConfirmation {
        id: playlistDeleteConfirmation
    }

    PlaylistSaveSheet {
        id: playlistSaveSheet
        host: window
    }
    PlaylistSettingsSheet {
        id: playlistSettingsSheet
        host: window
    }
    FileDialog {
        id: propertyFileDialog
        property string propertyKey: ""
        title: "选择文件"
        fileMode: FileDialog.OpenFile
        onAccepted: mirage.setSelectedProperty(propertyKey, ContentViewLogic.selectedFilePath(selectedFile))
    }

    FolderDialog {
        id: propertyFolderDialog
        property string propertyKey: ""
        title: "选择目录"
        onAccepted: mirage.setSelectedProperty(propertyKey, ContentViewLogic.selectedFilePath(selectedFolder))
    }

    FileDialog {
        id: importFileDialog
        title: "导入壁纸视频"
        fileMode: FileDialog.OpenFile
        nameFilters: ["视频 (*.mp4 *.mov *.m4v *.webm *.mkv)", "所有文件 (*)"]
        onAccepted: mirage.importWallpaperPath(ContentViewLogic.selectedFilePath(selectedFile))
    }

    FolderDialog {
        id: importFolderDialog
        title: "导入壁纸文件夹"
        onAccepted: mirage.importWallpaperPath(ContentViewLogic.selectedFilePath(selectedFolder))
    }

    SteamSetupView {
        id: steamSetupWindow
        autoVisible: false
    }

    SteamSetupWindowController {
        id: steamSetupController
        window: steamSetupWindow
    }

    LinuxNotice {
        id: linuxNotice
    }

    Connections {
        target: mirage
        function onStatusMessageChanged() {
            if (mirage.statusMessage.length > 0)
                window.showSuccess(mirage.statusMessage);
        }
    }
}
