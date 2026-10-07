// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: online
    property int section: 1
    property string query: ""
    property int sortIndex: 0
    signal loginRequested
    signal settingsRequested
    signal downloadRequested
    signal itemRequested(var item)
    spacing: 8
    Rectangle {
        visible: online.section === 2 && !/^[0-9a-fA-F]{32}$/.test(Store.preferences.apiKey)
        Layout.fillWidth: true
        implicitHeight: 58
        radius: 8
        color: Theme.dark ? "#3f3525" : "#fff1da"
        border.color: "#b3ff9f0a"
        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            Icon {
                name: "warning"
                color: Theme.orange
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                MText {
                    text: Theme.t("请设置您自己的 Steam Web API Key")
                    font.bold: true
                }
                MText {
                    text: Theme.t("设置专属 Key 可提高创意工坊浏览稳定性")
                    font.pixelSize: 11
                    color: Theme.secondary
                }
            }
            MButton {
                text: Theme.t("设置")
                onClicked: online.settingsRequested()
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        MButton {
            visible: online.section !== 1
            text: Theme.t("筛选")
            symbol: "filter"
            prominent: true
            onClicked: Store.filtersVisible = !Store.filtersVisible
        }
        MField {
            id: queryField
            Layout.preferredWidth: online.section === 1 ? 250 : 200
            placeholderText: Theme.t(online.section === 1 ? "查找壁纸" : online.section === 3 ? "搜索已订阅壁纸…" : "搜索作品、作者或作品 ID…")
            text: online.query
            onTextEdited: online.query = text
            onAccepted: Store.request("search", {
                section: online.section,
                query: text
            })
        }
        MButton {
            visible: online.section === 1
            text: Theme.t("查找")
            symbol: "search"
            flatStyle: true
            enabled: queryField.text.trim() !== ""
            onClicked: Store.request("search", {
                section: online.section,
                query: online.query
            })
        }
        MCombo {
            visible: online.section === 2
            implicitWidth: 130
            model: ["最热门", "最新", "最多订阅", "最高评分"]
            currentIndex: online.sortIndex
            onActivated: online.sortIndex = currentIndex
        }
        Item {
            Layout.fillWidth: true
        }
        MButton {
            visible: online.section === 3
            text: Theme.t("下载全部")
            symbol: "download"
            enabled: Store.subscriptions.length > 0
            onClicked: online.downloadRequested()
        }
        MButton {
            symbol: "refresh"
            hint: Theme.t("刷新")
            enabled: !Store.networkBusy
            onClicked: Store.request("refresh", {
                section: online.section
            })
        }
        MButton {
            visible: online.section !== 1
            symbol: "download"
            hint: Theme.t("下载")
            onClicked: online.downloadRequested()
        }
        MButton {
            visible: online.section !== 1
            text: Theme.t("登录 Steam")
            symbol: "person"
            onClicked: online.loginRequested()
        }
    }
    Rectangle {
        height: 1
        Layout.fillWidth: true
        color: Theme.line
    }
    RowLayout {
        Layout.fillHeight: true
        Layout.fillWidth: true
        spacing: 10
        FilterSidebar {
            visible: Store.filtersVisible && online.section !== 1
            Layout.preferredWidth: 225
            Layout.fillHeight: true
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            BusyIndicator {
                anchors.centerIn: parent
                running: Store.networkBusy
                visible: running
            }
            EmptyState {
                anchors.fill: parent
                visible: !Store.networkBusy && (online.section === 1 ? Store.discoverRows.length === 0 : online.section === 2 ? Store.workshopItems.length === 0 : Store.subscriptions.length === 0)
                symbol: online.section === 1 ? "search" : online.section === 3 ? "check" : "cloud"
                title: Theme.t(online.section === 3 ? "登录以查看已订阅壁纸" : "创意工坊服务尚未连接")
                subtitle: Theme.t(online.section === 3 ? "登录 Steam 后，可以浏览订阅、管理收藏并下载壁纸。" : "连接服务后可浏览发现、搜索结果和壁纸详情")
                action: Theme.t(online.section === 3 ? "登录 Steam" : "设置")
                onTriggered: {
                    if (online.section === 3)
                        online.loginRequested();
                    else
                        online.settingsRequested();
                }
            }
            GridView {
                id: grid
                anchors.fill: parent
                visible: online.section !== 1
                clip: true
                cellWidth: width / Math.max(1, Math.floor(width / 180))
                cellHeight: cellWidth
                model: online.section === 2 ? Store.workshopItems : Store.subscriptions
                delegate: WallpaperCard {
                    required property var modelData
                    width: grid.cellWidth - 14
                    workshop: true
                    wallpaper: modelData
                    onContextRequested: online.itemRequested(modelData)
                    TapHandler {
                        onTapped: online.itemRequested(modelData)
                    }
                }
                ScrollBar.vertical: ScrollBar {}
            }
            ScrollView {
                id: discover
                anchors.fill: parent
                visible: online.section === 1 && Store.discoverRows.length > 0
                clip: true
                ColumnLayout {
                    width: discover.availableWidth
                    spacing: 34
                    Repeater {
                        model: Store.discoverRows
                        ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 14
                            RowLayout {
                                MText {
                                    text: modelData.title
                                    font.pixelSize: 22
                                    font.bold: true
                                    Layout.fillWidth: true
                                }
                                MButton {
                                    text: Theme.t("查看全部")
                                    flatStyle: true
                                    onClicked: Store.request("discoverBrowse", {
                                        id: modelData.id
                                    })
                                }
                            }
                            ListView {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 200
                                orientation: ListView.Horizontal
                                spacing: 14
                                clip: true
                                model: modelData.items
                                delegate: WallpaperCard {
                                    required property var modelData
                                    width: 164
                                    workshop: true
                                    wallpaper: modelData
                                    onContextRequested: online.itemRequested(modelData)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
