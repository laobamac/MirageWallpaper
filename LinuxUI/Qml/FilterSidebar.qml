// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: sidebar
    clip: true
    property var groups: [
        {
            key: "show",
            title: "仅显示",
            options: ["广受好评", "我的收藏", "音频响应", "可自定义"]
        },
        {
            key: "type",
            title: "类型",
            options: ["场景", "视频", "网页", "应用程序", "预设"]
        },
        {
            key: "rating",
            title: "分级",
            options: ["所有人", "轻度裸露", "成人"]
        },
        {
            key: "resolution",
            title: "分辨率",
            options: []
        },
        {
            key: "source",
            title: "来源",
            options: ["创意工坊", "我的壁纸"]
        },
        {
            key: "tags",
            title: "标签",
            options: FilterData.tags
        }
    ]
    ColumnLayout {
        width: sidebar.availableWidth - 14
        spacing: 18
        MButton {
            text: Theme.t("重置筛选")
            symbol: "refresh"
            prominent: true
            Layout.fillWidth: true
            implicitHeight: 32
            onClicked: Store.filterGroups = ({})
        }
        Repeater {
            model: sidebar.groups
            ColumnLayout {
                id: group
                required property var modelData
                property bool expanded: true
                Layout.fillWidth: true
                spacing: 6
                MButton {
                    Layout.fillWidth: true
                    flatStyle: true
                    text: Theme.t(group.modelData.title)
                    symbol: group.expanded ? "down" : "right"
                    onClicked: group.expanded = !group.expanded
                }
                ColumnLayout {
                    visible: group.expanded
                    Layout.fillWidth: true
                    Layout.leftMargin: 10
                    spacing: 5
                    RowLayout {
                        visible: group.modelData.key === "tags" || group.modelData.key === "resolution"
                        MButton {
                            text: Theme.t("全选")
                            flatStyle: true
                            onClicked: {
                                let f = Object.assign({}, Store.filterGroups);
                                delete f[group.modelData.key];
                                Store.filterGroups = f;
                            }
                        }
                        MButton {
                            text: Theme.t("清空")
                            flatStyle: true
                            onClicked: {
                                let f = Object.assign({}, Store.filterGroups);
                                f[group.modelData.key] = [];
                                Store.filterGroups = f;
                            }
                        }
                    }
                    Repeater {
                        model: group.modelData.options
                        MCheck {
                            required property string modelData
                            required property int index
                            Layout.fillWidth: true
                            text: Theme.t(modelData)
                            checked: Store.filterGroups[group.modelData.key] === undefined ? group.modelData.key !== "show" : Store.filterGroups[group.modelData.key].includes(index)
                            onClicked: {
                                let f = Object.assign({}, Store.filterGroups);
                                let values = f[group.modelData.key] === undefined ? (group.modelData.key === "show" ? [] : group.modelData.options.map((x, i) => i)) : f[group.modelData.key].slice();
                                if (checked)
                                    values.push(index);
                                else
                                    values = values.filter(x => x !== index);
                                f[group.modelData.key] = values;
                                Store.filterGroups = f;
                            }
                        }
                    }
                    Repeater {
                        model: group.modelData.key === "resolution" ? FilterData.resolutions : []
                        ColumnLayout {
                            id: resolutionGroup
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            spacing: 5
                            MText {
                                text: Theme.t(parent.modelData.title)
                                color: Theme.secondary
                                font.pixelSize: 12
                                Layout.topMargin: 8
                            }
                            Repeater {
                                model: parent.modelData.options
                                MCheck {
                                    required property string modelData
                                    required property int index
                                    text: Theme.t(modelData)
                                    Layout.fillWidth: true
                                    property string tag: FilterData.resolutionTags[resolutionGroup.index][index]
                                    checked: Store.filterGroups.resolution === undefined || Store.filterGroups.resolution.includes(tag)
                                    onClicked: {
                                        let f = Object.assign({}, Store.filterGroups);
                                        let all = FilterData.resolutionTags.flat();
                                        let a = f.resolution === undefined ? all : f.resolution.slice();
                                        if (checked)
                                            a.push(tag);
                                        else
                                            a = a.filter(x => x !== tag);
                                        f.resolution = a;
                                        Store.filterGroups = f;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
