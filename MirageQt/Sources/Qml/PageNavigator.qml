// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    implicitWidth: row.implicitWidth + 20
    implicitHeight: 42
    radius: 8
    color: Theme.panel
    border.color: Theme.line
    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 6
        MButton {
            symbol: "left"
            enabled: Store.page > 1
            flatStyle: true
            onClicked: Store.page--
        }
        Repeater {
            model: Math.min(Store.pageCount, 7)
            MButton {
                required property int index
                property int pageNumber: Store.pageCount <= 7 ? index + 1 : Math.max(1, Math.min(Store.page - 3, Store.pageCount - 6)) + index
                text: String(pageNumber)
                prominent: Store.page === pageNumber
                flatStyle: true
                onClicked: Store.page = pageNumber
            }
        }
        MButton {
            symbol: "right"
            enabled: Store.page < Store.pageCount
            flatStyle: true
            onClicked: Store.page++
        }
        Rectangle {
            width: 1
            height: 20
            color: Theme.line
        }
        MField {
            implicitWidth: 48
            text: String(Store.page)
            horizontalAlignment: Text.AlignHCenter
            validator: IntValidator {
                bottom: 1
                top: Store.pageCount
            }
            onAccepted: Store.page = Math.max(1, Math.min(Store.pageCount, Number(text)))
        }
        MText {
            text: "/ " + Store.pageCount
            color: Theme.secondary
            font.pixelSize: 12
        }
    }
}
