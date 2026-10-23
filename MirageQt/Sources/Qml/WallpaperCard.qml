// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes

Item {
    id: card
    required property var wallpaper
    property bool selected: Store.selectedId === wallpaper.id
    property bool showTitle: true
    property bool workshop: false
    signal contextRequested(real x, real y)
    implicitHeight: width
    Rectangle {
        width: parent.width
        height: width
        radius: 10
        color: Theme.hover
        clip: true
        Image {
            anchors.fill: parent
            source: card.wallpaper.preview || ""
            fillMode: card.workshop ? Image.PreserveAspectCrop : Image.PreserveAspectFit
            asynchronous: true
            visible: !animation.visible
        }
        AnimatedImage {
            id: animation
            anchors.fill: parent
            source: String(card.wallpaper.preview || "").toLowerCase().endsWith(".gif") ? card.wallpaper.preview : ""
            fillMode: card.workshop ? Image.PreserveAspectCrop : Image.PreserveAspectFit
            visible: source.toString() !== ""
            playing: visible && (hover.hovered || card.selected || Store.preferences.animated === 1)
            asynchronous: true
        }
        Icon {
            anchors.centerIn: parent
            width: 32
            height: 32
            name: card.wallpaper.type === "video" ? "play" : "image"
            color: Theme.secondary
            visible: !card.wallpaper.preview
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: card.showTitle
            height: card.workshop ? 60 : 38
            color: hover.hovered ? "#66000000" : "#33000000"
            Column {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 3
                Text {
                    width: parent.width
                    text: card.wallpaper.title
                    color: hover.hovered ? "#e6e6e6" : "#b3b3b3"
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
                Text {
                    visible: card.workshop
                    width: parent.width
                    text: Theme.t(({
                            scene: "场景",
                            video: "视频",
                            web: "网页",
                            preset: "预设"
                        })[card.wallpaper.type] || "应用程序")
                    color: "white"
                    font.pixelSize: 10
                    horizontalAlignment: Text.AlignRight
                }
            }
        }
        Icon {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 7
            name: "heart"
            color: "#ff4f70"
            visible: Store.favorites.includes(card.wallpaper.id)
        }
    }
    Repeater {
        model: 4
        Shape {
            required property int index
            width: 10
            height: 10
            x: index === 1 || index === 2 ? card.width - 10 : 0
            y: index >= 2 ? card.width - 10 : 0
            rotation: index * 90
            ShapePath {
                strokeWidth: 0
                fillColor: Theme.window
                PathSvg {
                    path: "M0 0 H10 A10 10 0 0 0 0 10 Z"
                }
            }
        }
    }
    Rectangle {
        width: parent.width
        height: width
        radius: 10
        color: "transparent"
        border.width: card.selected ? 3 : hover.hovered ? 1 : 0
        border.color: Theme.accent
    }
    HoverHandler {
        id: hover
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: Store.selectedId = card.wallpaper.id
        onDoubleTapped: Store.apply()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: (eventPoint, button) => {
            Store.selectedId = card.wallpaper.id;
            card.contextRequested(eventPoint.position.x, eventPoint.position.y);
        }
    }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 900
    ToolTip.text: wallpaper.title
    Accessible.name: wallpaper.title
    Accessible.role: Accessible.ListItem
}
