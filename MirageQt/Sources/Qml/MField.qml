// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls

TextField {
    id: control
    implicitHeight: 26
    implicitWidth: 160
    color: Theme.text
    placeholderTextColor: Theme.secondary
    font.pixelSize: 13
    selectByMouse: true
    leftPadding: 8
    rightPadding: 8
    background: Rectangle {
        radius: 5
        color: Theme.field
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.accent : Theme.line
    }
}
