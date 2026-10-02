import QtQuick
import FluentUI

// 创意工坊图片（对齐 macOS GlobalComponents/WorkshopImage.swift）：
// 灰色占位底 + 按容器尺寸异步加载（sourceSize 取容器 2x，对齐 downsample
// 的缩放解码）+ 加载指示 + 失败图标；GIF 动画由 AnimatedImage 原生多帧
// 播放支持（对齐 WorkshopAnimatedImage，play 控制播放/暂停）。
Item {
    id: root

    property url imageUrl
    // 与 Image.fillMode 一致：PreserveAspectCrop = fill，PreserveAspectFit = fit。
    property int contentMode: Image.PreserveAspectCrop
    // 对齐 macOS isAnimating：false 时暂停 GIF 动画。
    property bool isAnimating: true

    // 裁剪容器：AnimatedImage 的动画路径不受 fillMode 控制（Qt 已知行为，
    // 动画帧按原始尺寸绘制，anchors.fill 左右双锚点还会触发宽度自动扩展
    // 覆盖裁剪，clip 也无效），因此显式给 AnimatedImage width/height 并
    // 在外层容器 clip，保证 GIF/静态图都按 PreserveAspectCrop 比例裁剪
    // 而非被拉伸（对齐 macOS WorkshopImage 的 .fill 语义）。
    // 使用矩形 Item 保持图片直角；clip 仅用于裁剪 PreserveAspectCrop 内容。
    Item {
        id: cropBox
        anchors.fill: parent
        clip: true

        // AnimatedImage 继承 Image 的异步加载/状态/fillMode/sourceSize API，
        // 非动画图显示单帧、GIF 多帧播放；play 控制动画启停。
        // 尺寸用显式 width/height（不用 anchors.fill 双锚点），fillMode 才
        // 在动画路径下按比例缩放。
        AnimatedImage {
            id: img
            width: cropBox.width
            height: cropBox.height
            source: root.imageUrl
            asynchronous: true
            visible: status === Image.Ready
            fillMode: root.contentMode
            playing: root.isAnimating
            // 按容器尺寸缩放解码（对齐 macOS downsample 的 maxPixel 逻辑）：
            // 2x 对齐高分屏，避免全尺寸大图占内存。
            sourceSize.width: root.width > 0 ? Math.round(root.width * 2) : 0
            sourceSize.height: root.height > 0 ? Math.round(root.height * 2) : 0
        }
    }

    // 加载中指示（对齐 ProgressView().controlSize(.small)）。
    FluProgressRing {
        anchors.centerIn: parent
        visible: img.status === Image.Loading || img.status === Image.Null
        indeterminate: true
        width: 18
        height: 18
    }

    // 失败占位图标（对齐 Image(systemName: "photo")）。
    FluIcon {
        anchors.centerIn: parent
        iconSource: FluentIcons.Photo
        iconSize: 20
        iconColor: FluTheme.fontTertiaryColor
        visible: img.status === Image.Error
    }
}
