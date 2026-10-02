// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls

Popup {
    id: sheet
    parent: Overlay.overlay
    modal: true
    focus: true
    anchors.centerIn: parent
    padding: 20
    width: 520
    height: Math.min(implicitHeight, parent ? parent.height - 40 : 700)
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        color: Theme.panel
        radius: 12
        border.color: Theme.line
        border.width: 1
    }
    Overlay.modal: Rectangle {
        color: Theme.dark ? "#70000000" : "#35000000"
    }
    enter: Transition {
        ParallelAnimation {
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1
                duration: 130
            }
            NumberAnimation {
                property: "scale"
                from: 0.97
                to: 1
                duration: 130
            }
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "opacity"
            to: 0
            duration: 100
        }
    }
}
