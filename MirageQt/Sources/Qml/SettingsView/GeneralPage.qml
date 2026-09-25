import QtQuick
import QtQuick.Layouts
import FluentUI

FluScrollablePage {
    required property var host

    ColumnLayout {
        Layout.fillWidth: true

                        spacing: 12
                        FluText {
                            text: "应用"
                            font: FluTextStyle.BodyStrong
                        }
                        FluToggleSwitch {
                            text: "登录后自动启动"
                            checked: !!host.settingsDraft.autoStart
                            clickListener: function () {
                                host.setSetting("autoStart", !checked);
                            }
                        }
                        FluToggleSwitch {
                            text: "安全模式"
                            checked: !!host.settingsDraft.safeMode
                            clickListener: function () {
                                host.setSetting("safeMode", !checked);
                            }
                        }
                        FluDivider {
                            Layout.fillWidth: true
                        }
                        FluText {
                            text: "软件更新"
                            font: FluTextStyle.BodyStrong
                        }
                        FluToggleSwitch {
                            text: "自动检查并下载更新（TODO：Linux 更新服务）"
                            enabled: false
                        }
                        FluToggleSwitch {
                            text: "接收测试版更新（TODO：Linux 更新服务）"
                            enabled: false
                        }
                        FluText {
                            Layout.fillWidth: true
                            text: "TODO：Linux 版本暂未提供 Sparkle 更新服务，仍可通过发行版包管理器更新。"
                            wrapMode: Text.WordWrap
                            color: FluTheme.fontSecondaryColor
                        }
                        FluDivider {
                            Layout.fillWidth: true
                        }
                        FluText {
                            text: "语言"
                            font: FluTextStyle.BodyStrong
                        }
                        FluComboBox {
                            Layout.fillWidth: true
                            model: ["跟随系统", "简体中文", "繁體中文", "English"]
                            currentIndex: ["followSystem", "zh_CN", "zh_TW", "en_US"].indexOf(host.settingsDraft.language)
                            onActivated: host.setSetting("language", ["followSystem", "zh_CN", "zh_TW", "en_US"][currentIndex])
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            FluText {
                                text: "外观"
                            }
                            FluComboBox {
                                Layout.fillWidth: true
                                model: ["跟随系统", "浅色", "深色"]
                                currentIndex: ["followSystem", "light", "dark"].indexOf(host.settingsDraft.appearance)
                                onActivated: host.setSetting("appearance", ["followSystem", "light", "dark"][currentIndex])
                            }
                        }
                        FluDivider {
                            Layout.fillWidth: true
                        }
                        FluText {
                            text: "音频"
                            font: FluTextStyle.BodyStrong
                        }
                        FluToggleSwitch {
                            text: "启用音频输出"
                            checked: !!host.settingsDraft.audioOutput
                            clickListener: function () {
                                host.setSetting("audioOutput", !checked);
                            }
                        }
                        FluToggleSwitch {
                            text: "切换音频输出设备时重新加载"
                            checked: !!host.settingsDraft.reloadWhenChangingOutputDevice
                            clickListener: function () {
                                host.setSetting("reloadWhenChangingOutputDevice", !checked);
                            }
                        }
                        FluToggleSwitch {
                            text: "全局静音"
                            checked: !!host.settingsDraft.globalMuted
                            clickListener: function () {
                                host.setSetting("globalMuted", !checked);
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            FluText {
                                text: "主音量"
                            }
                            FluSlider {
                                Layout.fillWidth: true
                                from: 0
                                to: 1
                                stepSize: 0.01
                                value: Number(host.settingsDraft.masterVolume || 0)
                                onMoved: host.setSetting("masterVolume", value)
                            }
                            FluText {
                                text: Math.round(Number(host.settingsDraft.masterVolume || 0) * 100) + "%"
                            }
                        }
                        FluDivider {
                            Layout.fillWidth: true
                        }
                        FluText {
                            text: "壁纸库"
                            font: FluTextStyle.BodyStrong
                        }
                        FluToggleSwitch {
                            text: "自动刷新壁纸库"
                            checked: !!host.settingsDraft.autoRefresh
                            clickListener: function () {
                                host.setSetting("autoRefresh", !checked);
                            }
                        }
                    }
    }
