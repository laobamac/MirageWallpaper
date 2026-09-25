import QtQuick
import QtQuick.Layouts
import FluentUI

FluScrollablePage {
    id: page
    required property var host

    function modeKeys(key) {
        if (key === "otherApplicationFocused" || key === "otherApplicationPlayingAudio")
            return ["keepRunning", "mute", "pause"];
        if (key === "displayAsleep" || key === "laptopOnBattery")
            return ["keepRunning", "pause", "stop"];
        return ["keepRunning", "mute", "pause", "stop"];
    }

    function modeLabels(keys) {
        var result = [];
        for (var i = 0; i < keys.length; ++i) {
            for (var j = 0; j < host.playbackModes.length; ++j) {
                if (host.playbackModes[j].key === keys[i]) {
                    result.push(host.playbackModes[j].label);
                    break;
                }
            }
        }
        return result;
    }

    function setQuality(antiAliasing, postProcessing, textureResolution, fps, reflections) {
        host.setSetting("antiAliasing", antiAliasing);
        host.setSetting("postProcessing", postProcessing);
        host.setSetting("textureResolution", textureResolution);
        host.setSetting("fps", fps);
        host.setSetting("reflections", reflections);
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12

        FluText {
            text: "播放规则"
            font: FluTextStyle.BodyStrong
        }

    Repeater {
        model: host.playbackOptions
        delegate: RowLayout {
            required property var modelData
            property var keys: page.modeKeys(modelData.key)
            Layout.fillWidth: true

            FluText {
                Layout.fillWidth: true
                text: modelData.label
            }

            FluComboBox {
                Layout.preferredWidth: 180
                model: page.modeLabels(keys)
                currentIndex: Math.max(0, keys.indexOf(host.settingsDraft[modelData.key]))
                onActivated: host.setSetting(modelData.key, keys[currentIndex])
            }
        }
    }

    FluDivider {
        Layout.fillWidth: true
    }

    FluText {
        text: "渲染质量"
        font: FluTextStyle.BodyStrong
    }

    RowLayout {
        Layout.fillWidth: true
        Repeater {
            model: ["低", "中", "高", "极致"]
            delegate: FluButton {
                required property string modelData
                Layout.fillWidth: true
                text: modelData
                onClicked: {
                    if (modelData === "低")
                        page.setQuality("none", "disabled", "highPerformance", 10, false);
                    else if (modelData === "中")
                        page.setQuality("none", "enabled", "automatic", 15, true);
                    else if (modelData === "高")
                        page.setQuality("msaa_x2", "enabled", "automatic", 25, true);
                    else
                        page.setQuality("msaa_x2", "ultra", "highQuality", 30, true);
                }
            }
        }
    }

    FluDivider {
        Layout.fillWidth: true
    }

    FluText {
        text: "渲染"
        font: FluTextStyle.BodyStrong
    }

    RowLayout {
        Layout.fillWidth: true
        FluText {
            text: "帧率"
        }
        FluSlider {
            Layout.fillWidth: true
            from: 10
            to: 120
            stepSize: 1
            value: Number(host.settingsDraft.fps || 30)
            onMoved: host.setSetting("fps", Math.round(value))
        }
        FluText {
            text: String(Math.round(host.settingsDraft.fps || 30))
            Layout.preferredWidth: 32
        }
    }

    RowLayout {
        Layout.fillWidth: true
        FluText {
            text: "抗锯齿"
            Layout.fillWidth: true
        }
        FluComboBox {
            Layout.preferredWidth: 180
            model: ["关闭", "MSAA 2x", "MSAA 4x", "MSAA 8x"]
            currentIndex: ["none", "msaa_x2", "msaa_x4", "msaa_x8"].indexOf(host.settingsDraft.antiAliasing)
            onActivated: host.setSetting("antiAliasing", ["none", "msaa_x2", "msaa_x4", "msaa_x8"][currentIndex])
        }
    }

    RowLayout {
        Layout.fillWidth: true
        FluText {
            text: "渲染分辨率"
            Layout.fillWidth: true
        }
        FluComboBox {
            Layout.preferredWidth: 180
            model: ["原生（最高画质）", "75%（自动）", "50%（高性能）"]
            currentIndex: ["highQuality", "automatic", "highPerformance"].indexOf(host.settingsDraft.textureResolution)
            onActivated: host.setSetting("textureResolution", ["highQuality", "automatic", "highPerformance"][currentIndex])
        }
    }

    RowLayout {
        Layout.fillWidth: true
        FluText {
            text: "壁纸加载方式"
            Layout.fillWidth: true
        }
        FluComboBox {
            Layout.preferredWidth: 240
            model: ["从磁盘加载（较低内存占用）", "从内存加载（减少磁盘读取）"]
            currentIndex: host.settingsDraft.wallpaperLoadSource === "memory" ? 1 : 0
            onActivated: host.setSetting("wallpaperLoadSource", currentIndex === 1 ? "memory" : "disk")
        }
    }

    FluToggleSwitch {
        text: "启用音频频谱（场景与网页壁纸）"
        checked: !!host.settingsDraft.enableSpectrum
        clickListener: function () {
            host.setSetting("enableSpectrum", !checked);
        }
    }

        FluText {
            Layout.fillWidth: true
            text: "抗锯齿、渲染分辨率和壁纸加载方式在切换壁纸后生效；帧率会立即应用。"
            font: FluTextStyle.Caption
            color: FluTheme.fontSecondaryColor
            wrapMode: Text.WordWrap
        }
    }
}
