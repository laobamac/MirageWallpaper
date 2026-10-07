// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: panel
    signal loaded
    property string pendingDelete: ""
    Layout.fillWidth: true
    ScrollView {
        id: scroll
        Layout.fillWidth: true
        Layout.preferredHeight: 260
        clip: true
        ColumnLayout {
            width: scroll.availableWidth
            spacing: 6
            Repeater {
                model: Store.savedPlaylists
                Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 68
                    radius: 10
                    color: Theme.hover
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 12
                        Icon {
                            name: "list"
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                            color: Theme.accent
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            MText {
                                text: modelData.name
                                font.bold: true
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                maximumLineCount: 1
                            }
                            MText {
                                text: modelData.items.length + " · " + (modelData.updated ? new Date(modelData.updated).toLocaleString(Qt.locale(), Locale.ShortFormat) : "")
                                font.pixelSize: 11
                                color: Theme.secondary
                            }
                        }
                        MButton {
                            text: Theme.t("读取")
                            prominent: true
                            onClicked: {
                                Store.updatePlaylist(modelData.items, modelData.settings);
                                panel.loaded();
                            }
                        }
                        MButton {
                            symbol: "trash"
                            flatStyle: true
                            destructive: true
                            onClicked: {
                                panel.pendingDelete = modelData.name;
                                confirmation.open();
                            }
                        }
                    }
                }
            }
        }
    }
    Sheet {
        id: confirmation
        width: 380
        contentItem: ColumnLayout {
            spacing: 18
            MText {
                text: Theme.t("删除") + " “" + panel.pendingDelete + "”？"
                Layout.fillWidth: true
                font.pixelSize: 18
            }
            RowLayout {
                Item {
                    Layout.fillWidth: true
                }
                MButton {
                    text: Theme.t("取消")
                    onClicked: confirmation.close()
                }
                MButton {
                    text: Theme.t("删除")
                    prominent: true
                    destructive: true
                    onClicked: {
                        Store.savedPlaylists = Store.savedPlaylists.filter(p => p.name !== panel.pendingDelete);
                        bridge.save("savedPlaylists", JSON.stringify(Store.savedPlaylists));
                        confirmation.close();
                    }
                }
            }
        }
    }
}
