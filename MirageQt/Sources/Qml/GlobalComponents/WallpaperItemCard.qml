import QtQuick
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects
import FluentUI

// 壁纸卡片公共基类：已安装与创意工坊使用同一张正方形封面卡。
// macOS 版本将作品信息叠在封面下沿，而非把图片和文字拆成两块；这里由
// 裁剪容器、向上淡出的黑色渐变和 contentData 共同保证该视觉协议。子类只
// 提供领域信息和角标，不能改变封面的圆角、渐变或命中范围。
Item {
    id: root

    required property var host
    required property var itemData
    property bool selected: false
    property double hoverScale: 1.03
    // 子类的角标铺在封面上，例如工坊下载状态。内容区保持唯一的 default
    // property，避免子类信息意外落到封面外。
    property Component overlay
    default property alias contentData: contentHost.data

    signal clicked(var mouse)
    signal doubleClicked(var mouse)

    // 悬停时只放大封面容器内的内容，外层卡片尺寸和矩形裁剪保持固定。
    // 若放大根对象，绘制范围会越过矩形边界，阴影或相邻 delegate 会覆盖角落。
    z: mouseArea.containsMouse ? 10 : 0

    DropShadow {
        anchors.fill: cover
        source: cover
        horizontalOffset: 0
        verticalOffset: 3
        radius: 10
        samples: 21
        color: Qt.rgba(0, 0, 0, 0.28)
        visible: !root.selected
    }


    // 固定尺寸的矩形边界约束内部内容；内部内容可以放大，但超出的像素
    // 会在这里被裁掉，因此 hover 不会覆盖相邻卡片。
    Item {
        id: cover
        anchors.fill: parent
        clip: true

        Item {
            id: coverContent
            anchors.fill: parent
            scale: mouseArea.containsMouse ? root.hoverScale : 1.0
            transformOrigin: Item.Center
            Behavior on scale {
                NumberAnimation {
                    duration: 150
                    easing.type: Easing.OutCubic
                }
            }

            WorkshopImage {
                anchors.fill: parent
                // Both installed and workshop item maps define preview; direct
                // access preserves that documented protocol without a dynamic
                // field-name bridge.
                imageUrl: root.itemData.preview
                contentMode: Image.PreserveAspectCrop
                // 列表缩略图只显示 GIF 首帧；真实播放仍在详情预览页完成。
                isAnimating: false
            }

            // 从透明到近黑色的垂直渐变为浅色封面提供稳定的文字对比度。渐变只
            // 覆盖下半部，让封面的主要画面保持可见，和 macOS 原卡片一致。
            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.0) }
                    GradientStop { position: 0.42; color: Qt.rgba(0, 0, 0, 0.0) }
                    GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.90) }
                }
            }

            Loader {
                anchors.fill: parent
                sourceComponent: root.overlay
            }

            // 具体卡片提供标题、统计及类别。此区域固定贴在封面底边，因而卡片在
            // 所有网格尺寸下仍保留正方形比例，而不会被内容撑高。
            ColumnLayout {
                id: contentHost
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                anchors.bottomMargin: 6
                anchors.topMargin: 2
                spacing: 2
            }
        }
    }

    FluFocusRectangle {
        anchors.fill: cover
        visible: root.selected
        radius: 0
    }

    // 统一命中区保留左右键与双击语义，事件由子类连接到各自的控制器调用。
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: function(mouse) { root.clicked(mouse); }
        onDoubleClicked: function(mouse) { root.doubleClicked(mouse); }
    }
}
