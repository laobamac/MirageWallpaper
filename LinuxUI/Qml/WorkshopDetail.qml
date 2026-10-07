// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: detail
    property var item: ({})
    signal authorRequested(var author)
    color: Theme.panel
    ScrollView {
        id: scroll
        anchors.fill: parent
        clip: true
        ColumnLayout {
            width: scroll.availableWidth
            spacing: 16
            ColumnLayout {
                Layout.margins: 16
                Layout.fillWidth: true
                spacing: 14
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: width
                    radius: 6
                    color: Theme.hover
                    Image {
                        anchors.fill: parent
                        source: detail.item.preview || ""
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                    }
                    Icon {
                        anchors.centerIn: parent
                        width: 50
                        height: 50
                        name: "cloud"
                        visible: !detail.item.preview
                        color: Theme.secondary
                    }
                }
                MText {
                    text: detail.item.title || Theme.t("选择壁纸以查看详情")
                    font.pixelSize: 18
                    font.bold: true
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                }
                MButton {
                    text: detail.item.author || Theme.t("佚名作者")
                    symbol: "person"
                    flatStyle: true
                    Layout.alignment: Qt.AlignHCenter
                    enabled: !!detail.item.author
                    onClicked: detail.authorRequested(detail.item)
                }
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    Repeater {
                        model: 5
                        Icon {
                            name: "star"
                            width: 12
                            height: 12
                            color: Theme.secondary
                        }
                    }
                    MButton {
                        symbol: "heart"
                        flatStyle: true
                        enabled: Store.steamConnected
                    }
                }
                MButton {
                    text: Theme.t("订阅并下载")
                    symbol: "download"
                    prominent: true
                    Layout.fillWidth: true
                    enabled: Store.steamConnected && !!detail.item.id
                    onClicked: Store.request("subscribe", {
                        id: detail.item.id
                    })
                }
                MButton {
                    text: Theme.t("在创意工坊中打开")
                    symbol: "link"
                    Layout.fillWidth: true
                    enabled: /^\d+$/.test(String(detail.item.id || ""))
                    onClicked: Qt.openUrlExternally("https://steamcommunity.com/sharedfiles/filedetails/?id=" + detail.item.id)
                }
                SectionTitle {
                    text: Theme.t("详情")
                    Layout.fillWidth: true
                }
                MText {
                    text: detail.item.description || ""
                    Layout.fillWidth: true
                }
                SectionTitle {
                    text: Theme.t("标签")
                    Layout.fillWidth: true
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 5
                    Repeater {
                        model: detail.item.tags || []
                        MButton {
                            required property string modelData
                            text: Theme.p(modelData)
                            flatStyle: true
                            onClicked: Store.request("tag", {
                                tag: modelData
                            })
                        }
                    }
                }
                SectionTitle {
                    text: Theme.t("评论")
                    Layout.fillWidth: true
                }
                MText {
                    text: Theme.t("登录 Steam 后查看评论")
                    color: Theme.secondary
                    Layout.fillWidth: true
                }
                MField {
                    Layout.fillWidth: true
                    placeholderText: Theme.t("发表评论…")
                    enabled: Store.steamConnected
                }
            }
        }
    }
}
