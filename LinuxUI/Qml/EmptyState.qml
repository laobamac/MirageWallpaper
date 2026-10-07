// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Layouts

Item {
    property string symbol: "image"
    property string title: ""
    property string subtitle: ""
    property string action: ""
    signal triggered
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 450)
        spacing: 12
        Icon {
            name: parent.parent.symbol
            color: Theme.secondary
            Layout.preferredWidth: 42
            Layout.preferredHeight: 42
            Layout.alignment: Qt.AlignHCenter
            opacity: 0.6
        }
        MText {
            text: parent.parent.title
            font.pixelSize: 17
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            color: Theme.secondary
        }
        MText {
            text: parent.parent.subtitle
            font.pixelSize: 12
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            color: Theme.secondary
        }
        MButton {
            text: parent.parent.action
            visible: text !== ""
            prominent: true
            Layout.alignment: Qt.AlignHCenter
            onClicked: parent.parent.triggered()
        }
    }
}
