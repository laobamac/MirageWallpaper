// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Sheet {
    id: sheet
    width: 520
    property var draft: ({})
    function set(key, value) {
        let d = Object.assign({}, draft);
        d[key] = value;
        draft = d;
    }
    onOpened: draft = JSON.parse(JSON.stringify(Store.playlistSettings))
    contentItem: ColumnLayout {
        spacing: 18
        RowLayout {
            MText {
                text: Theme.t("播放列表设置")
                font.pixelSize: 20
                font.bold: true
                Layout.fillWidth: true
            }
            MButton {
                text: Theme.t("重置")
                onClicked: sheet.draft = ({
                        order: 0,
                        timing: 0,
                        hours: 0,
                        minutes: 30,
                        transition: 0,
                        duration: 1,
                        anchors: [],
                        first: false,
                        intro: false,
                        videoEnd: false,
                        paused: false
                    })
            }
        }
        Rectangle {
            height: 1
            Layout.fillWidth: true
            color: Theme.line
        }
        ScrollView {
            id: scroll
            Layout.preferredHeight: 380
            Layout.fillWidth: true
            clip: true
            ColumnLayout {
                width: scroll.availableWidth
                spacing: 20
                SectionTitle {
                    text: Theme.t("播放顺序")
                    Layout.fillWidth: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    Repeater {
                        model: ["随机", "顺序"]
                        MButton {
                            required property string modelData
                            required property int index
                            text: Theme.t(modelData)
                            Layout.fillWidth: true
                            prominent: sheet.draft.order === index
                            onClicked: sheet.set("order", index)
                        }
                    }
                }
                RowLayout {
                    MText {
                        text: Theme.t("更换壁纸")
                        font.bold: true
                        Layout.fillWidth: true
                    }
                    MCombo {
                        model: ["定时", "时间", "星期", "启动时", "从不"]
                        currentIndex: sheet.draft.timing || 0
                        onActivated: sheet.set("timing", currentIndex)
                    }
                }
                RowLayout {
                    visible: sheet.draft.timing === 0
                    SpinBox {
                        from: 0
                        to: 24
                        value: sheet.draft.hours || 0
                        editable: true
                        onValueModified: sheet.set("hours", value)
                    }
                    MText {
                        text: Theme.t("小时")
                    }
                    SpinBox {
                        from: 0
                        to: 59
                        value: sheet.draft.minutes || 0
                        editable: true
                        onValueModified: sheet.set("minutes", value)
                    }
                    MText {
                        text: Theme.t("分钟")
                    }
                }
                ColumnLayout {
                    visible: sheet.draft.timing === 1
                    Layout.fillWidth: true
                    MText {
                        text: Theme.t("选择每天切换壁纸的时刻")
                        color: Theme.secondary
                    }
                    GridLayout {
                        columns: 8
                        rowSpacing: 6
                        columnSpacing: 6
                        Repeater {
                            model: 24
                            MButton {
                                required property int index
                                text: String(index).padStart(2, "0")
                                implicitWidth: 44
                                prominent: (sheet.draft.anchors || []).includes(index)
                                onClicked: {
                                    let a = (sheet.draft.anchors || []).slice();
                                    if (a.includes(index))
                                        a = a.filter(x => x !== index);
                                    else
                                        a.push(index);
                                    sheet.set("anchors", a);
                                }
                            }
                        }
                    }
                }
                MText {
                    visible: sheet.draft.timing === 2
                    Layout.fillWidth: true
                    color: Theme.secondary
                    text: Theme.t("列表中的前 7 张壁纸将分别对应星期日至星期六。")
                }
                MText {
                    visible: sheet.draft.timing === 3
                    Layout.fillWidth: true
                    color: Theme.secondary
                    text: Theme.t("仅在 Mirage 启动时切换到列表中的壁纸。")
                }
                MText {
                    visible: sheet.draft.timing === 4
                    Layout.fillWidth: true
                    color: Theme.secondary
                    text: Theme.t("壁纸不会自动更换，仅手动点选切换。")
                }
                SectionTitle {
                    text: Theme.t("显示壁纸过渡")
                    Layout.fillWidth: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    Repeater {
                        model: ["禁用", "淡入淡出"]
                        MButton {
                            required property string modelData
                            required property int index
                            text: Theme.t(modelData)
                            Layout.fillWidth: true
                            prominent: sheet.draft.transition === index
                            onClicked: sheet.set("transition", index)
                        }
                    }
                }
                RowLayout {
                    visible: sheet.draft.transition !== 0
                    MText {
                        text: Theme.t("过渡时间")
                    }
                    MSlider {
                        from: 0.2
                        to: 5
                        stepSize: 0.1
                        value: sheet.draft.duration || 1
                        onMoved: sheet.set("duration", value)
                    }
                    MText {
                        text: Number(sheet.draft.duration || 1).toFixed(1) + "s"
                    }
                }
                SectionTitle {
                    text: Theme.t("选项")
                    Layout.fillWidth: true
                }
                Repeater {
                    model: [
                        {
                            key: "first",
                            label: "总是从第一张壁纸开始"
                        },
                        {
                            key: "intro",
                            label: "第一张壁纸仅在启动时播放"
                        },
                        {
                            key: "videoEnd",
                            label: "在视频结束时更换壁纸"
                        },
                        {
                            key: "paused",
                            label: "允许壁纸在暂停时更换"
                        }
                    ]
                    MCheck {
                        required property var modelData
                        text: Theme.t(modelData.label)
                        checked: Boolean(sheet.draft[modelData.key])
                        Layout.fillWidth: true
                        onClicked: sheet.set(modelData.key, checked)
                    }
                }
            }
        }
        Rectangle {
            height: 1
            Layout.fillWidth: true
            color: Theme.line
        }
        RowLayout {
            Item {
                Layout.fillWidth: true
            }
            MButton {
                text: Theme.t("取消")
                onClicked: sheet.close()
            }
            MButton {
                text: Theme.t("好")
                implicitWidth: 80
                prominent: true
                onClicked: {
                    Store.updatePlaylist(Store.playlist, sheet.draft);
                    sheet.close();
                }
            }
        }
    }
}
