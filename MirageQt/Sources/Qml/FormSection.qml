// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: section
    property string title: ""
    property string symbol: ""
    default property alias rows: body.data
    Layout.fillWidth: true
    spacing: 8
    RowLayout {
        spacing: 7
        Layout.leftMargin: 10
        visible: section.title !== ""
        Icon {
            name: section.symbol
            visible: section.symbol !== ""
            width: 15
            height: 15
            color: Theme.secondary
        }
        MText {
            text: Theme.t(section.title)
            font.weight: Font.DemiBold
            color: Theme.secondary
        }
    }
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: body.implicitHeight + 24
        radius: 9
        color: Theme.field
        border.color: Theme.line
        ColumnLayout {
            id: body
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            spacing: 13
        }
    }
}
