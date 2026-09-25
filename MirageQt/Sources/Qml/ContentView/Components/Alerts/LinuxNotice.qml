import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../../GlobalComponents"

// Linux 未实现功能提示对话框：TODO 占位，向用户说明该功能尚未在 Qt 版实现。
MirageDialogWindow {
    id: root
    title: "Mirage"
    width: 380
    height: 220
    buttonFlags: 0
    contentDelegate: Component {
        ColumnLayout {
            spacing: 12
            FluText {
                text: "Mirage"
                font: FluTextStyle.Subtitle
            }
            FluText {
                Layout.fillWidth: true
                text: "TODO: 此功能尚未在 Linux Qt 版本实现。"
                wrapMode: Text.WordWrap
            }
            FluFilledButton {
                Layout.alignment: Qt.AlignRight
                text: "好"
                onClicked: root.close()
            }
        }
    }
}
