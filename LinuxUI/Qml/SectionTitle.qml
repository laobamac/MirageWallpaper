// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Layouts

RowLayout {
    property string text: ""
    spacing: 6
    MText {
        text: parent.text
        font.weight: Font.DemiBold
    }
    Rectangle {
        Layout.fillWidth: true
        height: 1
        color: Theme.line
    }
}
