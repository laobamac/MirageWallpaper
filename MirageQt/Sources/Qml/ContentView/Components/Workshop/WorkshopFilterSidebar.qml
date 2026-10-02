import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI

// 创意工坊/发现页的筛选侧栏：根为 FluScrollablePage（与已安装页 FilterResults 一致），
// 内部 ColumnLayout 用 Layout.fillWidth 严格占满视口，内容超长时在栏内滚动，
// 不会被内容隐式宽度撑破而侵入右侧的壁纸列表。
FluScrollablePage {
    id: root
    required property var host

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 10

    FluText {
        text: "创意工坊筛选"
        font: FluTextStyle.Subtitle
    }
    FluFilledButton {
        Layout.fillWidth: true
        text: "重置筛选"
        onClicked: root.host.resetWorkshopFilters()
    }
    FluText {
        text: "类型"
        font: FluTextStyle.BodyStrong
    }
    Repeater {
        model: root.host.workshopTypeFilters
        delegate: FluCheckBox {
            required property var modelData
            text: modelData.label
            checked: root.host.workshopType === modelData.key
            clickListener: function () {
                if (checked) {
                    root.host.workshopType = modelData.key;
                    mirage.setWorkshopTypeFilter(modelData.key);
                }
            }
        }
    }
    FluDivider { Layout.fillWidth: true }
    FluText {
        text: "分级"
        font: FluTextStyle.BodyStrong
    }
    Repeater {
        model: root.host.ratingFilters
        delegate: FluCheckBox {
            required property var modelData
            text: modelData.label
            checked: root.host.isEnabled(root.host.workshopRatings, modelData.key)
            clickListener: function () {
                root.host.setWorkshopRating(modelData.key, checked);
            }
        }
    }
    FluDivider { Layout.fillWidth: true }
    FluText {
        text: "标签"
        font: FluTextStyle.BodyStrong
    }
    RowLayout {
        FluTextButton {
            text: "全选"
            onClicked: root.host.selectAllWorkshopTags()
        }
        FluTextButton {
            text: "清空"
            onClicked: root.host.clearWorkshopTags()
        }
    }
    Repeater {
        model: root.host.workshopTagFilters
        delegate: FluCheckBox {
            required property var modelData
            text: modelData.label
            checked: root.host.isEnabled(root.host.workshopSelectedTags, modelData.key)
            clickListener: function () {
                root.host.toggleWorkshopTag(modelData.key);
            }
        }
    }
    }
}
