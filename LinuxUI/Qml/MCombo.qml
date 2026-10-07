// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls

ComboBox {
    id: control
    implicitWidth: 170
    implicitHeight: 26
    font.pixelSize: 13
    property bool translateModel: true
    padding: 5
    leftPadding: 8
    rightPadding: 24
    opacity: enabled ? 1 : 0.42
    contentItem: MText {
        text: control.translateModel ? Theme.t(control.displayText) : control.displayText
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Icon {
        name: "down"
        width: 12
        height: 12
        x: control.width - 19
        y: 7
        color: control.enabled ? Theme.text : Theme.secondary
    }
    background: Rectangle {
        radius: 5
        color: Theme.field
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.accent : Theme.line
    }
    delegate: ItemDelegate {
        width: control.width
        text: control.translateModel ? Theme.t(modelData) : modelData
        highlighted: control.highlightedIndex === index
        contentItem: MText {
            text: parent.text
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: parent.highlighted ? Theme.hover : Theme.field
        }
    }
    popup: Popup {
        y: control.height + 3
        width: Math.max(control.width, 180)
        padding: 4
        background: Rectangle {
            radius: 7
            color: Theme.field
            border.color: Theme.line
        }
        contentItem: ListView {
            implicitHeight: Math.min(contentHeight, 320)
            model: control.popup.visible ? control.delegateModel : null
            clip: true
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }
}
