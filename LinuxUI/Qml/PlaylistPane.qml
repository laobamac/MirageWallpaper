// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: pane
    property bool collapsed: Boolean(bridge.load("playlistCollapsed", false))
    property string selected: ""
    signal importRequested
    signal dialogRequested(string kind)
    spacing: 8
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        MButton {
            symbol: pane.collapsed ? "up" : "down"
            flatStyle: true
            onClicked: {
                pane.collapsed = !pane.collapsed;
                bridge.save("playlistCollapsed", pane.collapsed);
            }
        }
        MText {
            text: Theme.t("播放列表") + (Store.playlist.length ? " (" + Store.playlist.length + ")" : "")
            font.pixelSize: 20
            font.bold: true
        }
        MCombo {
            visible: bridge.screens.length > 1
            implicitWidth: 170
            model: bridge.screens.map((s, i) => Theme.t("显示器") + " " + (i + 1) + " · " + s.name)
            translateModel: false
            currentIndex: Store.display
            onActivated: Store.display = currentIndex
        }
        Item {
            Layout.fillWidth: true
        }
        MButton {
            text: Theme.t("载入")
            symbol: "folder"
            enabled: Store.savedPlaylists.length > 0
            onClicked: pane.dialogRequested("playlistOpen")
        }
        MButton {
            text: Theme.t("保存")
            symbol: "download"
            enabled: Store.playlist.length > 0
            onClicked: pane.dialogRequested("playlistSave")
        }
        MButton {
            text: Theme.t("配置")
            symbol: "settings"
            onClicked: pane.dialogRequested("playlistSettings")
        }
        MButton {
            text: Theme.t("添加壁纸")
            symbol: "plus"
            prominent: true
            enabled: !!Store.selected.id
            onClicked: Store.addToPlaylist(Store.selectedId)
        }
    }
    Rectangle {
        visible: !pane.collapsed
        Layout.fillWidth: true
        Layout.preferredHeight: 96
        color: Theme.dark ? "#222224" : "#e4e4e7"
        radius: 6
        ListView {
            id: list
            anchors.fill: parent
            anchors.margins: 8
            orientation: ListView.Horizontal
            spacing: 10
            clip: true
            model: Store.playlist
            delegate: Item {
                required property string modelData
                required property int index
                width: 80
                height: 80
                WallpaperCard {
                    anchors.fill: parent
                    wallpaper: Store.find(modelData)
                    showTitle: false
                    selected: pane.selected === modelData
                    TapHandler {
                        onTapped: pane.selected = modelData
                    }
                }
                DragHandler {
                    id: drag
                    target: null
                    property real lastOffset: 0
                    onActiveTranslationChanged: if (active)
                        lastOffset = activeTranslation.x
                    xAxis.enabled: true
                    yAxis.enabled: false
                    onActiveChanged: if (!active) {
                        Store.movePlaylist(index, Math.max(0, Math.min(Store.playlist.length - 1, index + Math.round(lastOffset / 90))));
                        lastOffset = 0;
                    }
                }
            }
            ScrollBar.horizontal: ScrollBar {}
        }
        MText {
            anchors.centerIn: parent
            visible: Store.playlist.length === 0
            text: Theme.t("将壁纸添加到播放列表")
            color: Theme.secondary
        }
        DropArea {
            anchors.fill: parent
            onDropped: Store.addToPlaylist(Store.selectedId)
        }
    }
    RowLayout {
        visible: !pane.collapsed
        Layout.fillWidth: true
        MButton {
            text: Theme.t("导入壁纸")
            symbol: "upload"
            prominent: true
            implicitWidth: 180
            onClicked: pane.importRequested()
        }
        Item {
            Layout.fillWidth: true
        }
        MButton {
            text: Theme.t("移除壁纸")
            symbol: "minus"
            enabled: pane.selected !== ""
            onClicked: {
                Store.removeFromPlaylist(pane.selected);
                pane.selected = "";
            }
        }
        MButton {
            text: Theme.t("清理")
            symbol: "trash"
            destructive: true
            enabled: Store.playlist.length > 0
            onClicked: pane.dialogRequested("playlistClear")
        }
    }
}
