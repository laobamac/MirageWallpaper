import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../../GlobalComponents"
import "../../ContentViewLogic.js" as ContentViewLogic

// 编辑壁纸信息对话框：编辑选中壁纸的名称与标签。
// 草稿状态（metadataTitle/metadataTags）由 host（窗口）持有，
// 打开时从选中壁纸预填，保存时把标签文本解析为列表提交。
MirageDialogWindow {
    id: root
    required property var host

    title: "编辑壁纸信息"
    width: 460
    height: 300
    negativeText: "取消"
    positiveText: "保存"
    buttonFlags: FluContentDialogType.NegativeButton | FluContentDialogType.PositiveButton
    onOpened: {
        root.host.metadataTitle = mirage.selectedWallpaper.title || "";
        root.host.metadataTags = (mirage.selectedWallpaper.tags || []).join(", ");
    }
    onPositiveClicked: mirage.updateSelectedMetadata(root.host.metadataTitle, ContentViewLogic.metadataTagList(root.host.metadataTags))
    contentDelegate: Component {
        ColumnLayout {
            width: parent.width
            spacing: 8
            FluText {
                text: "名称"
            }
            FluTextBox {
                Layout.fillWidth: true
                text: root.host.metadataTitle
                placeholderText: "壁纸名称"
                onTextChanged: root.host.metadataTitle = text
            }
            FluText {
                text: "标签"
            }
            FluTextBox {
                Layout.fillWidth: true
                text: root.host.metadataTags
                placeholderText: "用逗号、分号或换行分隔标签"
                onTextChanged: root.host.metadataTags = text
            }
        }
    }
}
