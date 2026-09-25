import QtQuick
import "../ContentViewLogic.js" as ContentViewLogic
import "../OptionData.js" as OptionData

// ContentView 的视图状态层：对应 macOS ContentView/ViewModels/ContentViewModel.swift。
// 持有窗口级状态（tab 选择、筛选开关、搜索词、排序、分页、草稿、信任流程），
// 由 ContentView 实例化并通过 property alias 转发，组件经 host 访问时
// 语义不变；筛选/分页的派生数据在此计算，纯逻辑在 ContentViewLogic.js。
QtObject {
    id: viewModel

    // --- 视图偏好 ---
    property int currentTab: 0
    property bool filtersVisible: false
    property bool playlistExpanded: false
    property string searchText: ""
    property int explorerIconSize: 170
    // 已安装、创意工坊和已订阅共用每页 50 项的协议；该值只读，
    // 窗口尺寸和图标尺寸只能改变列数，不能改变分页边界。
    readonly property int wallpaperItemsPerPage: 50

    // --- 筛选状态（仅显示开关） ---
    property bool approvedOnly: false
    property bool favoritesOnly: false
    property bool mobileOnly: false
    property bool audioOnly: false
    property bool customizableOnly: false

    // --- 分辨率筛选状态（已安装壁纸，对齐 macOS FR*Resolution OptionSet） ---
    // 六组掩码默认全选（bit 位与 OptionData.resolutionGroups.options 顺序对应，
    // 与订阅侧 C++ 默认 m_subscription* 掩码一致：7/3/4/5/5/2 个选项全置位）。
    property int widescreenMask: 0x7F
    property int ultraWidescreenMask: 0x07
    property int dualscreenMask: 0x0F
    property int triplescreenMask: 0x1F
    property int portraitMask: 0x1F
    property int miscMask: 0x03

    // --- 筛选选项数据（来自 OptionData.js 常量） ---
    property var typeFilters: OptionData.typeFilters
    property var ratingFilters: OptionData.ratingFilters
    property var sourceFilters: OptionData.sourceFilters
    property var tagFilters: OptionData.tagFilters
    property var enabledTypes: typeFilters.map(function (filter) {
        return filter.key;
    })
    property var enabledRatings: ratingFilters.map(function (filter) {
        return filter.key;
    })
    property var enabledSources: sourceFilters.map(function (filter) {
        return filter.key;
    })
    property var enabledTags: tagFilters.map(function (filter) {
        return filter.key;
    })

    // --- 排序与分页（已安装壁纸） ---
    property string sortMode: "name"
    property bool sortDescending: false
    property var installedSortOptions: OptionData.installedSortOptions
    property var rawWallpapers: mirage.wallpapers
    property var filteredWallpapers: ContentViewLogic.filterWallpapers(rawWallpapers, {
        searchText: searchText,
        approvedOnly: approvedOnly,
        favoritesOnly: favoritesOnly,
        mobileOnly: mobileOnly,
        audioOnly: audioOnly,
        customizableOnly: customizableOnly,
        enabledTypes: enabledTypes,
        enabledRatings: enabledRatings,
        enabledSources: enabledSources,
        enabledTags: enabledTags,
        tagFilters: tagFilters,
        widescreenMask: widescreenMask,
        ultraWidescreenMask: ultraWidescreenMask,
        dualscreenMask: dualscreenMask,
        triplescreenMask: triplescreenMask,
        portraitMask: portraitMask,
        miscMask: miscMask,
        sortMode: sortMode,
        sortDescending: sortDescending
    })
    property int wallpaperCurrentPage: 1
    property int wallpaperPageCount: Math.max(1, Math.ceil(filteredWallpapers.length / wallpaperItemsPerPage))
    property var pagedWallpapers: filteredWallpapers.slice((wallpaperCurrentPage - 1) * wallpaperItemsPerPage, wallpaperCurrentPage * wallpaperItemsPerPage)

    // --- 创意工坊筛选状态 ---
    property string workshopSearchText: ""
    property string workshopSortKey: "trending"
    property string workshopType: "all"
    property var workshopRatings: ["Everyone"]
    property var workshopSelectedTags: []
    property int discoverTrendDays: 7

    // --- 设置与播放列表草稿 ---
    property int settingsPage: 0
    property var settingsDraft: mirage.settings
    property bool settingsDirty: false
    property var playlistSettingsDraft: ({})
    property string playlistSaveName: ""

    // --- 元数据编辑草稿 ---
    property string metadataTitle: ""
    property string metadataTags: ""

    // --- 网页壁纸信任流程（pending 请求） ---
    property var pendingTrustAction: null
    property string pendingTrustWallpaperId: ""
}
