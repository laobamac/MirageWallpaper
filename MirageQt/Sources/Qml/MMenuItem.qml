// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

MenuItem {
    id: control

    property string symbol: ""
    // MenuItem does not expose a shortcut QML property; callers provide the display text explicitly.
    property string shortcutText: ""
    property bool destructive: false

    // Menu entries use the same 13px text and compact corner language as the existing controls.
    implicitWidth: 250
    implicitHeight: 30
    padding: 4
    spacing: 7
    font.pixelSize: 13
    hoverEnabled: true
    opacity: enabled ? 1 : 0.42

    contentItem: RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 25
        anchors.rightMargin: control.subMenu ? control.arrow.width + control.spacing : 0
        spacing: 7

        Icon {
            visible: control.symbol !== "" || control.icon.name !== ""
            name: control.symbol !== "" ? control.symbol : control.icon.name
            color: control.enabled ? (control.highlighted ? "white" : control.destructive ? Theme.red : Theme.text) : Theme.secondary
            Layout.preferredWidth: 15
            Layout.preferredHeight: 15
        }

        MText {
            text: control.text
            color: control.enabled ? (control.highlighted ? "white" : control.destructive ? Theme.red : Theme.text) : Theme.secondary
            font: control.font
            wrapMode: Text.NoWrap
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        Text {
            text: control.shortcutText
            visible: text !== ""
            color: control.highlighted ? Qt.rgba(255, 255, 255, 0.78) : Theme.secondary
            font: control.font
            horizontalAlignment: Text.AlignRight
            Layout.preferredWidth: implicitWidth
        }
    }

    indicator: Rectangle {
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 14
        height: 14
        radius: 3
        visible: control.checkable && control.checked
        color: "transparent"
        border.width: 0

        Icon {
            anchors.centerIn: parent
            width: 12
            height: 12
            name: "check"
            color: "white"
            visible: control.checked
        }
    }

    arrow: Icon {
        x: control.mirrored ? control.leftPadding : control.width - width - control.rightPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 14
        height: 14
        name: "right"
        color: control.highlighted ? "white" : control.enabled ? Theme.secondary : Theme.line
        visible: !!control.subMenu
    }

    background: Rectangle {
        radius: 5
        color: control.down ? Qt.darker(Theme.accent, 1.08) : control.highlighted ? Theme.accent : "transparent"

        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: 7
            color: "transparent"
            border.width: control.visualFocus && !control.highlighted ? 2 : 0
            border.color: Theme.accent
        }
    }

    Accessible.name: control.text
}
