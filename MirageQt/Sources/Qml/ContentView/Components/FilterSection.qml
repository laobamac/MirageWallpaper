import QtQuick
import FluentUI

// 可折叠筛选分组：标题行（箭头旋转 + 文字，点击展开/收起）+ 内容容器。
// 对应 macOS FilterResults.swift 的 FilterSection。
Column {
    id: root

    property string title
    property bool expanded: true
    default property alias content: contentItem.data

    spacing: 4

    Item {
        id: header
        width: root.width
        height: headerContent.implicitHeight

        Row {
            id: headerContent
            width: parent.width
            spacing: 4

            FluIcon {
                iconSource: FluentIcons.ChevronDownMed
                iconSize: 12
                anchors.verticalCenter: parent.verticalCenter
                rotation: root.expanded ? 0 : -90
                Behavior on rotation {
                    NumberAnimation {
                        duration: 150
                    }
                }
            }
            FluText {
                text: root.title
                font: FluTextStyle.BodyStrong
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        MouseArea {
            anchors.fill: parent
            onClicked: root.expanded = !root.expanded
        }
    }

    Column {
        id: contentItem
        width: root.width
        visible: root.expanded
        spacing: 2
    }
}
