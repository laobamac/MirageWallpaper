import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../../GlobalComponents"

MirageDialogWindow {
    id: root
    required property var host
    title: "播放列表设置"
    width: 520
    height: 620
    negativeText: "取消"
    positiveText: "保存"
    buttonFlags: FluContentDialogType.NegativeButton | FluContentDialogType.PositiveButton
    onOpened: root.host.resetPlaylistSettingsDraft()
    onPositiveClicked: mirage.updatePlaylistSettings(root.host.playlistSettingsDraft)
    contentDelegate: Component {
            ColumnLayout {
                width: parent.width
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    FluText {
                        text: "播放顺序"
                        font: FluTextStyle.BodyStrong
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    FluTextButton {
                        text: "重置"
                        onClicked: root.host.resetPlaylistSettingsDraft()
                    }
                }
                FluComboBox {
                    Layout.fillWidth: true
                    model: root.host.playlistOrderOptions.map(function (option) {
                        return option.label;
                    })
                    currentIndex: root.host.playlistOptionIndex(root.host.playlistOrderOptions, root.host.playlistSettingsDraft.order)
                    onActivated: root.host.setPlaylistSetting("order", root.host.playlistOrderOptions[currentIndex].key)
                }
                FluDivider {
                    Layout.fillWidth: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    FluText {
                        text: "更换壁纸"
                        font: FluTextStyle.BodyStrong
                    }
                    FluComboBox {
                        Layout.fillWidth: true
                        model: root.host.playlistTimingOptions.map(function (option) {
                            return option.label;
                        })
                        currentIndex: root.host.playlistOptionIndex(root.host.playlistTimingOptions, root.host.playlistSettingsDraft.timing)
                        onActivated: root.host.setPlaylistSetting("timing", root.host.playlistTimingOptions[currentIndex].key)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.host.playlistSettingsDraft.timing === "timer"
                    FluText {
                        text: "间隔"
                    }
                    FluSpinBox {
                        from: 0
                        to: 24
                        value: Number(root.host.playlistSettingsDraft.timerHours || 0)
                        onValueModified: root.host.setPlaylistSetting("timerHours", value)
                    }
                    FluText {
                        text: "小时"
                    }
                    FluSpinBox {
                        from: 0
                        to: 59
                        value: Number(root.host.playlistSettingsDraft.timerMinutes || 0)
                        onValueModified: root.host.setPlaylistSetting("timerMinutes", value)
                    }
                    FluText {
                        text: "分钟"
                    }
                }
                FluText {
                    Layout.fillWidth: true
                    visible: root.host.playlistSettingsDraft.timing === "logon"
                    text: "仅在 Mirage 启动时切换到列表中的壁纸。"
                    wrapMode: Text.WordWrap
                }
                FluText {
                    Layout.fillWidth: true
                    visible: root.host.playlistSettingsDraft.timing === "never"
                    text: "壁纸不会自动更换，仅手动点选切换。"
                    wrapMode: Text.WordWrap
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.host.playlistSettingsDraft.timing === "daytime"
                    spacing: 6
                    FluText {
                        text: "选择每天切换壁纸的时刻"
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 6
                        Repeater {
                            model: 24
                            delegate: FluToggleButton {
                                required property int index
                                text: (index < 10 ? "0" : "") + index
                                checked: (root.host.playlistSettingsDraft.daytimeAnchors || []).indexOf(index) !== -1
                                clickListener: function () {
                                    var anchors = (root.host.playlistSettingsDraft.daytimeAnchors || []).slice();
                                    var anchorIndex = anchors.indexOf(index);
                                    if (anchorIndex === -1)
                                        anchors.push(index);
                                    else
                                        anchors.splice(anchorIndex, 1);
                                    anchors.sort(function (left, right) {
                                        return left - right;
                                    });
                                    root.host.setPlaylistSetting("daytimeAnchors", anchors);
                                }
                            }
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.host.playlistSettingsDraft.timing === "dayOfWeek"
                    spacing: 6
                    FluText {
                        text: "列表中的前 7 张壁纸依次对应星期日至星期六。"
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: mirage.playlistItems.length > 7
                        FluText {
                            Layout.fillWidth: true
                            text: "星期播放列表最多支持 7 张壁纸。"
                            wrapMode: Text.WordWrap
                        }
                        FluButton {
                            text: "移除多余壁纸"
                            onClicked: mirage.trimPlaylistItems(7)
                        }
                    }
                    Repeater {
                        model: Math.min(7, mirage.playlistItems.length)
                        delegate: FluText {
                            required property int index
                            text: root.host.weekdayLabels[index] + "：" + mirage.playlistItems[index].title
                        }
                    }
                }
                FluDivider {
                    Layout.fillWidth: true
                }

                FluText {
                    text: "显示壁纸过渡"
                    font: FluTextStyle.BodyStrong
                }
                FluComboBox {
                    Layout.fillWidth: true
                    model: root.host.playlistTransitionOptions.map(function (option) {
                        return option.label;
                    })
                    currentIndex: root.host.playlistOptionIndex(root.host.playlistTransitionOptions, root.host.playlistSettingsDraft.transition)
                    onActivated: root.host.setPlaylistSetting("transition", root.host.playlistTransitionOptions[currentIndex].key)
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.host.playlistSettingsDraft.transition !== "disabled"
                    FluText {
                        text: "过渡时间"
                    }
                    FluSlider {
                        Layout.fillWidth: true
                        from: 0.2
                        to: 5
                        stepSize: 0.1
                        value: Number(root.host.playlistSettingsDraft.transitionSeconds || 1)
                        onMoved: root.host.setPlaylistSetting("transitionSeconds", value)
                    }
                    FluText {
                        text: Number(root.host.playlistSettingsDraft.transitionSeconds || 1).toFixed(1) + " 秒"
                    }
                }
                FluDivider {
                    Layout.fillWidth: true
                }

                FluText {
                    text: "选项"
                    font: FluTextStyle.BodyStrong
                }
                FluToggleSwitch {
                    text: "总是从第一张壁纸开始"
                    checked: !!root.host.playlistSettingsDraft.alwaysBeginFirst
                    clickListener: function () {
                        root.host.setPlaylistSetting("alwaysBeginFirst", !checked);
                    }
                }
                FluToggleSwitch {
                    text: "第一张壁纸仅在启动时播放"
                    checked: !!root.host.playlistSettingsDraft.introOnStartup
                    clickListener: function () {
                        root.host.setPlaylistSetting("introOnStartup", !checked);
                    }
                }
                FluToggleSwitch {
                    text: "在视频结束时更换壁纸"
                    checked: !!root.host.playlistSettingsDraft.videoSequence
                    clickListener: function () {
                        root.host.setPlaylistSetting("videoSequence", !checked);
                    }
                }
                FluToggleSwitch {
                    text: "允许壁纸在暂停时更换"
                    checked: !!root.host.playlistSettingsDraft.updateOnPause
                    clickListener: function () {
                        root.host.setPlaylistSetting("updateOnPause", !checked);
                    }
                }
            }
    }
}
