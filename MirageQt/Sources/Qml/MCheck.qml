// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls

CheckBox {
    id: control
    implicitHeight: 24
    spacing: 7
    padding: 0
    font.pixelSize: 13
    opacity: enabled ? 1 : 0.42
    indicator: Rectangle {
        x: 0
        y: (control.height - height) / 2
        width: 14
        height: 14
        radius: 3
        color: control.checked ? Theme.accent : Theme.field
        border.width: 1
        border.color: control.checked ? Theme.accent : Theme.line
        Icon {
            anchors.centerIn: parent
            width: 12
            height: 12
            name: "check"
            color: "white"
            visible: control.checked
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            color: "transparent"
            radius: 5
            border.width: control.visualFocus ? 2 : 0
            border.color: Theme.accent
        }
    }
    contentItem: MText {
        text: control.text
        leftPadding: 21
        verticalAlignment: Text.AlignVCenter
    }
}
