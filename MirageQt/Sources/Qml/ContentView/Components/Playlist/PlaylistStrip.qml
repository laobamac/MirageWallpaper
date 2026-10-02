import QtQuick
import QtQuick.Layouts
import FluentUI

// 播放列表横向条：对应 macOS Playlist/PlaylistStrip.swift。
// 展示当前屏幕的播放列表，支持拖拽排序、双击播放、单项移除；
// 双击播放走 host 的信任检查流程。
ListView {
    id: strip
    required property var host
    orientation: ListView.Horizontal
    spacing: 8
    clip: true
    model: mirage.playlistItems
    delegate: FluFrame {
        required property var modelData
        required property int index
        width: 112
        height: parent.height
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 4
            Image {
                Layout.fillWidth: true
                Layout.fillHeight: true
                source: modelData.preview
                fillMode: Image.PreserveAspectCrop
            }
            FluText {
                Layout.fillWidth: true
                text: modelData.title
                elide: Text.ElideRight
            }
        }
        FluIconButton {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 2
            z: 1
            text: "从播放列表移除"
            iconSource: FluentIcons.Delete
            onClicked: mirage.removePlaylistItem(modelData.id)
        }
        MouseArea {
            anchors.fill: parent
            anchors.rightMargin: 30
            drag.target: parent
            drag.axis: Drag.XAxis
            onDoubleClicked: strip.host.runWithWallpaperTrust(modelData, function () {
                mirage.playPlaylistItem(modelData.id);
            })
            onReleased: {
                var target = Math.round((parent.x + parent.width / 2) / (parent.width + strip.spacing));
                target = Math.max(0, Math.min(strip.count - 1, target));
                if (target !== index) {
                    mirage.movePlaylistItem(index, target > index ? target + 1 : target);
                }
            }
        }
    }
}
