import QtQuick
import QtQuick.Layouts
import FluentUI
import ".."
import "../../OptionData.js" as OptionData

// 已订阅壁纸的筛选侧栏：对齐 macOS Components/Workshop/SubscribedWorkshopFilterSidebar.swift。
// 过滤状态（类型/分级/分辨率掩码/标签）全部由后端 WorkshopViewModel 持有，
// 通过 mirage.subscriptionFilters（NOTIFY subscriptionsChanged）读取、
// 通过 mirage.setSubscription* 写回，组件不保存自己的状态。
// 分辨率分组数据（六组标题与选项）来自 OptionData.resolutionGroups，
// 与 C++ workshopResolutionOptions() 的 bit 位一一对应。
FluScrollablePage {
    id: root
    required property var host

    // 后端过滤状态快照；subscriptionsChanged 时由 QML 绑定自动更新。
    property var filters: mirage.subscriptionFilters

    function resolutionAllSelected() {
        return OptionData.resolutionGroups.every(function (group) {
            var allMask = (1 << group.options.length) - 1;
            return (Number(root.filters[group.maskKey] || 0) & allMask) === allMask;
        });
    }

    function resolutionNoneSelected() {
        return OptionData.resolutionGroups.every(function (group) {
            return Number(root.filters[group.maskKey] || 0) === 0;
        });
    }

    function tagsAllSelected() {
        return OptionData.workshopTagFilters.every(function (tag) {
            return (root.filters.selectedTags || []).indexOf(tag.key) >= 0;
        });
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 10

        // 重置筛选：与上游 clearSubscriptionFilters 对应，无激活过滤时禁用。
        FluFilledButton {
            Layout.fillWidth: true
            text: qsTr("重置筛选")
            enabled: Boolean(root.filters.hasActiveFilters)
            onClicked: mirage.clearSubscriptionFilters()
        }

        FilterSection {
            Layout.fillWidth: true
            title: qsTr("类型")
            // 单选：与上游 Toggle 一致，点击已选中的项不产生取消效果。
            Repeater {
                model: OptionData.workshopTypeFilters
                delegate: FluCheckBox {
                    required property var modelData
                    width: parent.width
                    text: modelData.label
                    checked: String(root.filters.typeFilter) === modelData.key
                    clickListener: function () {
                        if (checked)
                            mirage.setSubscriptionTypeFilter(modelData.key);
                    }
                }
            }
        }

        FilterSection {
            Layout.fillWidth: true
            title: qsTr("分级")
            // 多选：mask 的 bit 与 ratingFilters 顺序（Everyone/Questionable/Mature）对应。
            Repeater {
                model: OptionData.ratingFilters
                delegate: FluCheckBox {
                    required property var modelData
                    // 声明 required modelData 后 Qt6 Repeater 不再注入 context 属性 index，
                    // 需显式声明 required index（否则绑定求值报 ReferenceError）。
                    required property int index
                    width: parent.width
                    text: modelData.label
                    checked: (Number(root.filters.ageRatingMask || 0) & (1 << index)) !== 0
                    clickListener: function () {
                        mirage.setSubscriptionAgeRatingEnabled(modelData.key, checked);
                    }
                }
            }
        }

        FilterSection {
            Layout.fillWidth: true
            title: qsTr("分辨率")
            RowLayout {
                width: parent.width
                FluTextButton {
                    text: qsTr("全选")
                    enabled: !root.resolutionAllSelected()
                    onClicked: mirage.selectAllSubscriptionResolutions()
                }
                Item {
                    Layout.fillWidth: true
                }
                FluTextButton {
                    text: qsTr("清空")
                    enabled: !root.resolutionNoneSelected()
                    onClicked: mirage.clearSubscriptionResolutions()
                }
            }
            // 六组分辨率（其他/宽屏/超宽屏/双/三/纵向），对齐 SubscribedResolutionFilterGroup。
            Repeater {
                model: OptionData.resolutionGroups
                delegate: Column {
                    required property var modelData
                    // 显式转发组信息，避免内层 Repeater 的 modelData 遮蔽后依赖 parent 链取值。
                    property string maskKey: modelData.maskKey
                    property int resolutionGroup: modelData.group
                    width: parent.width
                    spacing: 4
                    FluText {
                        text: modelData.title
                        font: FluTextStyle.Caption
                        color: FluTheme.fontSecondaryColor
                    }
                    Repeater {
                        model: modelData.options
                        delegate: FluCheckBox {
                            required property string modelData
                            // 同外层：required modelData 使 index 不再自动注入，需显式声明。
                            required property int index
                            width: parent.width
                            text: modelData
                            checked: (Number(root.filters[parent.maskKey] || 0)
                                      & (1 << index)) !== 0
                            clickListener: function () {
                                mirage.setSubscriptionResolutionOption(
                                    parent.resolutionGroup, index, checked);
                            }
                        }
                    }
                }
            }
        }

        FilterSection {
            Layout.fillWidth: true
            title: qsTr("标签")
            RowLayout {
                width: parent.width
                FluTextButton {
                    text: qsTr("全选")
                    enabled: !root.tagsAllSelected()
                    onClicked: mirage.selectAllSubscriptionTags()
                }
                Item {
                    Layout.fillWidth: true
                }
                FluTextButton {
                    text: qsTr("清空")
                    enabled: (root.filters.selectedTags || []).length > 0
                    onClicked: mirage.clearSubscriptionTags()
                }
            }
            // 多选：与浏览页一致，勾选即 toggle（对齐 applySubscriptionTagFilter）。
            Repeater {
                model: OptionData.workshopTagFilters
                delegate: FluCheckBox {
                    required property var modelData
                    width: parent.width
                    text: modelData.label
                    checked: (root.filters.selectedTags || []).indexOf(modelData.key) >= 0
                    clickListener: function () {
                        mirage.toggleSubscriptionTag(modelData.key);
                    }
                }
            }
        }
    }
}
