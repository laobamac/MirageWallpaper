// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls.Basic

Menu {
    id: control

    // The custom delegate keeps every menu entry on the same compact scale as MButton and MCombo.
    implicitWidth: 260
    padding: 6
    font.pixelSize: 13
    delegate: MMenuItem { }
    palette.window: Theme.field
    palette.windowText: Theme.text
    palette.mid: Theme.line
    palette.highlight: Theme.accent
    palette.highlightedText: "white"

    contentItem: ListView {
        implicitHeight: Math.min(contentHeight, 400)
        model: control.contentModel
        interactive: contentHeight > control.height
        clip: true
        currentIndex: control.currentIndex
        boundsBehavior: Flickable.StopAtBounds
        ScrollIndicator.vertical: ScrollIndicator { }
    }

    // The popup surface uses Theme.field so it remains distinct from both the window and panel surfaces.
    background: Rectangle {
        implicitWidth: 260
        implicitHeight: 40
        radius: 9
        color: Qt.rgba(Theme.field.r, Theme.field.g, Theme.field.b, 0.97)
        border.width: 1
        border.color: Theme.line
    }
}
