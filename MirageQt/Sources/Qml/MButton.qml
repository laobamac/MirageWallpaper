// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Button {
    id: control
    property string symbol: ""
    property bool prominent: false
    property bool destructive: false
    property bool flatStyle: false
    property bool pill: false
    property string hint: ""
    implicitHeight: 26
    implicitWidth: Math.max(26, label.implicitWidth + 20)
    padding: 5
    leftPadding: 10
    rightPadding: 10
    font.pixelSize: 13
    hoverEnabled: true
    opacity: enabled ? 1 : 0.42
    contentItem: RowLayout {
        id: label
        spacing: 5
        Icon {
            visible: control.symbol !== ""
            name: control.symbol
            color: control.prominent ? "white" : (control.destructive ? Theme.red : Theme.text)
            Layout.preferredWidth: 15
            Layout.preferredHeight: 15
        }
        Text {
            visible: control.text !== ""
            text: control.text
            color: control.prominent ? "white" : (control.destructive ? Theme.red : Theme.text)
            font: control.font
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
    }
    background: Rectangle {
        radius: control.pill ? height / 2 : 5
        color: control.prominent ? (control.down ? Qt.darker(control.destructive ? Theme.red : Theme.accent, 1.18) : control.destructive ? Theme.red : Theme.accent) : control.down || control.hovered ? Theme.hover : control.flatStyle ? "transparent" : Theme.field
        border.width: control.prominent || control.flatStyle ? 0 : 1
        border.color: Theme.line
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: parent.radius + 3
            color: "transparent"
            border.color: Theme.accent
            border.width: 2
            visible: control.visualFocus
        }
    }
    ToolTip.visible: hovered && hint !== ""
    ToolTip.text: hint
    ToolTip.delay: 600
    Accessible.name: text || hint
}
