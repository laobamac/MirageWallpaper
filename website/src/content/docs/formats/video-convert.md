---
title: 视频转换为壁纸
description: 把 mp4、mov、m4v 视频包装成可直接播放的本地视频壁纸包。
---

Mirage 可以把普通视频文件转换成动态壁纸。你只需在[导入](/MirageWallpaper/formats/import/)时选择视频，Mirage 就会自动生成一个视频壁纸包。

## 支持的格式

- `.mp4`
- `.mov`
- `.m4v`

其它扩展名会被拒绝，并提示不支持的类型。

## 转换做了什么

选择一个视频后，Mirage 会在导入目录下创建一个新的壁纸包，并完成以下工作：

1. **复制视频文件**到新建的壁纸目录。转换过程**不会重新编码**视频，因此速度快、画质无损，但也意味着播放兼容性取决于该视频本身的编码。
2. **生成预览图**：从视频第 1 秒附近抽取一帧，保存为 `preview.jpg`（JPEG，质量约 0.85）。
3. **写入 `project.json`**：自动生成一份最小化的项目描述，`type` 为 `video`，`file` 指向复制进来的视频，`preview` 指向刚生成的缩略图，`title` 取视频文件名。

生成的壁纸包结构大致如下：

```text
我的视频/
├── project.json      # type: "video", file, preview, title
├── 我的视频.mp4       # 复制进来的原始视频
└── preview.jpg        # 自动生成的缩略图
```

## 播放

转换后的视频壁纸由 VideoWallpaper（AVFoundation）渲染，支持[音量、速度、填充模式](/MirageWallpaper/wallpapers/playback/)等控制。

## 提示

- 由于不重新编码，请尽量使用 macOS / AVFoundation 能良好播放的编码（如 H.264 / HEVC 的 mp4、mov）。若某个视频在系统「快速查看」中能正常播放，通常也能作为壁纸播放。
- 想进一步压缩体积或转码，可在导入前用外部工具处理（项目构建依赖中包含 FFmpeg，主要用于渲染器侧的视频处理，见[系统与构建要求](/MirageWallpaper/guides/requirements/)）。
- 转换是复制操作，原视频不受影响，但会占用额外磁盘空间。
