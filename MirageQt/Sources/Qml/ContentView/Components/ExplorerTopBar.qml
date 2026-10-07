import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../GlobalComponents"

RowLayout {
    id: topBar
    property int currentTab: 0
    property string searchText: ""
    property int explorerIconSize: 170
    property string sortMode: "name"
    property bool sortDescending: false
    property var host
    property alias searchBox: searchInput
    signal filterRequested
    signal refreshRequested
    signal sortChanged(string key)
    signal iconSizeChanged(int size)
    Layout.fillWidth: true
    spacing: 8

    FluTextBox {
        id: searchInput
        Layout.preferredWidth: topBar.currentTab === 0 ? 160 : 260
        placeholderText: topBar.currentTab === 0 ? "搜索已安装的壁纸" : "搜索创意工坊"
        iconSource: FluentIcons.Search
        text: topBar.searchText
        onTextChanged: {
            if (topBar.host)
                topBar.host.searchText = text;
        }
    }
    FluFilledButton {
        text: "筛选"
        onClicked: topBar.filterRequested()
    }
    FluIconButton {
        text: "刷新"
        iconSource: FluentIcons.Refresh
        onClicked: topBar.refreshRequested()
    }
    // 已安装、创意工坊和已订阅共用该图标尺寸偏好；每页数量
    // 由固定 50 项的数据协议决定，不在视图菜单中暴露。
    // （对齐 macOS 的共享视图菜单）。
    WallpaperGridViewMenu {
        visible: topBar.currentTab === 0
        explorerIconSize: topBar.explorerIconSize
        onIconSizeChanged: size => topBar.iconSizeChanged(size)
    }
    Item {
        Layout.fillWidth: true
    }
    FluIconButton {
        visible: topBar.currentTab === 0
        text: topBar.sortDescending ? "降序" : "升序"
        iconSource: FluentIcons.Sort
        onClicked: topBar.sortChanged("direction")
    }
    FluComboBox {
        Layout.preferredWidth: 120
        model: topBar.host ? topBar.host.installedSortOptions.map(function (option) {
            return option.label;
        }) : []
        onActivated: topBar.sortChanged(currentText)
    }
}
