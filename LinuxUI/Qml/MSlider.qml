// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls

Slider {
    id: control
    implicitWidth: 150
    implicitHeight: 24
    background: Rectangle {
        x: control.leftPadding
        y: (control.height - height) / 2
        width: control.availableWidth
        height: 4
        radius: 2
        color: Theme.line
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: Theme.accent
        }
    }
    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: (control.height - height) / 2
        width: 16
        height: 16
        radius: 8
        color: control.pressed ? "#eeeeee" : "#ffffff"
        border.color: control.visualFocus ? Theme.accent : "#c0c0c5"
        border.width: 1
    }
}
