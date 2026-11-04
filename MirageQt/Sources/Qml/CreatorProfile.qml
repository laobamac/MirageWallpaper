// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: profile
    property var author: ({})
    property var items: Store.decoratedLibrary.filter(item => item.author === author.author)
    property int gridSize: 140
    signal back
    signal selected(var wallpaper)
    color: Theme.panel
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 14
        RowLayout {
            MButton {
                symbol: "left"
                text: Theme.t("返回")
                flatStyle: true
                onClicked: profile.back()
            }
            Item {
                Layout.fillWidth: true
            }
            MButton {
                symbol: "link"
                enabled: !!profile.author.steamId
                onClicked: Qt.openUrlExternally("https://steamcommunity.com/profiles/" + profile.author.steamId)
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Icon {
                name: "person"
                Layout.preferredWidth: 48
                Layout.preferredHeight: 48
                color: Theme.secondary
            }
            ColumnLayout {
                Layout.fillWidth: true
                MText {
                    text: profile.author.author || Theme.t("佚名作者")
                    font.pixelSize: 20
                    font.bold: true
                    Layout.fillWidth: true
                }
                MText {
                    text: "Steam ID: " + (profile.author.steamId || "—")
                    font.pixelSize: 11
                    color: Theme.secondary
                    Layout.fillWidth: true
                }
            }
        }
        SectionTitle {
            text: Theme.t("作品")
            Layout.fillWidth: true
        }
        RowLayout {
            Layout.fillWidth: true
            MCombo {
                model: ["小图标", "中图标", "大图标"]
                currentIndex: 1
                onActivated: profile.gridSize = [110, 140, 180][currentIndex]
            }
            Item {
                Layout.fillWidth: true
            }
            MText {
                text: String(profile.items.length)
                color: Theme.secondary
            }
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            GridView {
                id: grid
                anchors.fill: parent
                clip: true
                cellWidth: width / Math.max(1, Math.floor(width / profile.gridSize))
                cellHeight: cellWidth
                model: profile.items
                delegate: WallpaperCard {
                    required property var modelData
                    width: grid.cellWidth - 10
                    wallpaper: modelData
                    onContextRequested: profile.selected(modelData)
                }
                ScrollBar.vertical: ScrollBar {}
            }
            EmptyState {
                anchors.fill: parent
                visible: profile.items.length === 0
                symbol: "cloud"
                title: Theme.t("创意工坊服务尚未连接")
            }
        }
    }
}
