import QtQuick
import QtQuick.Layouts
import FluentUI

FluFrame {
    id: banner
    property bool showsActions: true
    property string groupNumber: "2160040437"
    Layout.fillWidth: true
    implicitHeight: bannerContent.implicitHeight + 16

    TextEdit {
        id: groupNumberClipboard
        visible: false
        text: banner.groupNumber
        selectByMouse: false
    }

    RowLayout {
        id: bannerContent
        width: parent.width - 16
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 8
        spacing: 10
        FluIcon {
            iconSource: FluentIcons.Message
            iconSize: 18
            iconColor: "#f59e0b"
        }
        ColumnLayout {
            Layout.fillWidth: true
            FluText {
                text: "Mirage 仍处于早期阶段"
                font: FluTextStyle.BodyStrong
            }
            FluText {
                Layout.fillWidth: true
                text: "遇到问题请认真撰写 Issue，或加入 QQ 交流群 " + banner.groupNumber + " 反馈。"
                wrapMode: Text.WordWrap
            }
        }
        FluButton {
            visible: banner.showsActions
            text: "支持 Mirage"
            onClicked: Qt.openUrlExternally("https://github.com/laobamac/MirageWallpaper")
        }
        FluButton {
            visible: banner.showsActions
            text: "提交 Issue"
            onClicked: Qt.openUrlExternally("https://github.com/laobamac/MirageWallpaper/issues/new/choose")
        }
        FluFilledButton {
            visible: banner.showsActions
            text: "复制群号"
            onClicked: {
                groupNumberClipboard.selectAll();
                groupNumberClipboard.copy();
            }
        }
    }
}
