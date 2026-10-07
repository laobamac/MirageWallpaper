// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: library
    signal importRequested
    signal contextRequested
    spacing: 8
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        MField {
            id: searchField
            placeholderText: Theme.t("搜索")
            text: Store.search
            onTextEdited: Store.search = text
            implicitWidth: 160
            Accessible.name: placeholderText
        }
        MButton {
            text: Theme.t("筛选")
            symbol: "filter"
            prominent: true
            onClicked: Store.filtersVisible = !Store.filtersVisible
        }
        MButton {
            symbol: "refresh"
            hint: Theme.t("刷新壁纸库")
            enabled: !bridge.busy
            onClicked: bridge.refresh()
        }
        MButton {
            symbol: "grid"
            hint: Theme.t("视图")
            onClicked: viewMenu.popup()
        }
        Item {
            Layout.fillWidth: true
        }
        MButton {
            symbol: Store.descending ? "up" : "down"
            flatStyle: true
            onClicked: Store.descending = !Store.descending
        }
        MCombo {
            model: ["名称", "修改时间", "类型"]
            implicitWidth: 120
            currentIndex: Store.sort
            onActivated: Store.sort = currentIndex
        }
    }
    RowLayout {
        Layout.fillHeight: true
        Layout.fillWidth: true
        spacing: 10
        FilterSidebar {
            visible: Store.filtersVisible
            Layout.preferredWidth: 225
            Layout.fillHeight: true
        }
        Item {
            Layout.fillHeight: true
            Layout.fillWidth: true
            GridView {
                id: grid
                anchors.fill: parent
                anchors.bottomMargin: Store.pageCount > 1 ? 58 : 0
                clip: true
                cellWidth: width / Math.max(1, Math.floor(width / (Store.cardSize + 14)))
                cellHeight: cellWidth
                model: Store.paged
                delegate: WallpaperCard {
                    required property var modelData
                    width: grid.cellWidth - 14
                    height: grid.cellHeight - 12
                    wallpaper: modelData
                    onContextRequested: library.contextRequested()
                }
                ScrollBar.vertical: ScrollBar {}
            }
            MText {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 50
                visible: Store.filtered.length === 0
                text: Theme.t("没有找到匹配的壁纸。\n请调整或重置左侧筛选条件，或更换搜索关键词。\n也可以点击底部“导入壁纸”添加新壁纸。")
                font.pixelSize: 22
                color: Theme.secondary
                horizontalAlignment: Text.AlignHCenter
                lineHeight: 1.45
            }
            PageNavigator {
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                visible: Store.pageCount > 1
            }
            DropArea {
                anchors.fill: parent
                onDropped: drop => {
                    for (const url of drop.urls)
                        bridge.addSource(url);
                }
            }
        }
    }
    Menu {
        id: viewMenu
        MenuItem {
            text: Theme.t("小图标")
            onTriggered: Store.cardSize = 140
        }
        MenuItem {
            text: Theme.t("中图标")
            onTriggered: Store.cardSize = 170
        }
        MenuItem {
            text: Theme.t("大图标")
            onTriggered: Store.cardSize = 200
        }
        MenuSeparator {}
        Repeater {
            model: [25, 50, 100, 200]
            delegate: MenuItem {
                required property int modelData
                text: Theme.t("每页") + " " + modelData
                checkable: true
                checked: Store.pageSize === modelData
                onTriggered: {
                    Store.pageSize = modelData;
                    Store.page = 1;
                }
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+F"
        onActivated: searchField.forceActiveFocus()
    }
    Shortcut {
        sequence: "F5"
        onActivated: bridge.refresh()
    }
}
