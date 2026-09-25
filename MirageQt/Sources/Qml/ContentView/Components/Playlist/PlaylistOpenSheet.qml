import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../../GlobalComponents"

MirageDialogWindow {
    id: root
    required property var host
    title: "打开播放列表"
    width: 460
    height: 360
    buttonFlags: FluContentDialogType.NeutralButton
    neutralText: "完成"
    contentDelegate: Component {
            ColumnLayout {
                width: parent.width
                spacing: 10

                FluText {
                    Layout.fillWidth: true
                    visible: mirage.savedPlaylists.length === 0
                    text: "您尚未创建任何播放列表。"
                    horizontalAlignment: Text.AlignHCenter
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(280, contentHeight)
                    visible: mirage.savedPlaylists.length > 0
                    clip: true
                    spacing: 6
                    model: mirage.savedPlaylists
                    delegate: Item {
                        required property var modelData
                        width: ListView.view.width
                        height: 58

                        RowLayout {
                            anchors.fill: parent
                            spacing: 8
                            FluIcon {
                                iconSource: FluentIcons.List
                                iconSize: 20
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                FluText {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    elide: Text.ElideRight
                                    font: FluTextStyle.BodyStrong
                                }
                                FluText {
                                    Layout.fillWidth: true
                                    text: modelData.itemCount + " 项 · " + modelData.updatedAt
                                    elide: Text.ElideRight
                                }
                            }
                            FluFilledButton {
                                text: "读取"
                                onClicked: {
                                    mirage.loadSavedPlaylist(modelData.id);
                                    root.close();
                                }
                            }
                            FluIconButton {
                                text: "删除播放列表"
                                iconSource: FluentIcons.Delete
                                onClicked: {
                                    root.host.openPlaylistDeleteConfirmation(modelData.id, modelData.name);
                                }
                            }
                        }
                    }
                }
            }
    }
}
