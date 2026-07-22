---
title: project.json 结构
description: Wallpaper Engine 壁纸清单文件 project.json 的字段与含义。
---

`project.json` 是每个壁纸包根目录的清单文件，描述壁纸的类型、入口文件、预览图、元数据和可自定义属性。Mirage 读取它来识别和渲染壁纸。

## 顶层字段

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `type` | string | 壁纸类型：`scene`、`web`、`video`、`application` 等 |
| `file` | string | 入口文件：场景的 `scene.json` / `scene.pkg`、网页的 HTML、视频的视频文件名 |
| `preview` | string | 预览图文件名（如 `preview.jpg` / `preview.gif`） |
| `title` | string | 壁纸标题 |
| `description` | string？ | 作品描述 |
| `author` | string？ | 作者（很多作品没有此字段） |
| `tags` | string[]？ | 标签，如 `Anime`、`Nature` |
| `contentrating` | string？ | 内容分级：`Everyone`、`Questionable`、`Mature` |
| `approved` | bool？ | 是否为「广受好评」 |
| `visibility` | string？ | 可见性 |
| `version` | int？ | 版本号 |
| `workshopid` | int 或 string？ | Steam 创意工坊物品 ID |
| `workshopurl` | string？ | 创意工坊链接 |
| `general` | object？ | 通用配置，含用户属性 `properties` |
| `dependency` | int 或 string？ | 预设依赖的底层作品 ID（预设专用） |
| `preset` | object？ | 预设的属性取值（预设专用） |

:::note
`contentrating` 的三档 `Everyone` / `Questionable` / `Mature` 对应筛选面板中的「所有人」/「轻度裸露」/「成人」。`workshopid` 在不同作品里可能是数字或字符串，Mirage 两种都能解析。
:::

## 作者的推断

许多 `project.json` 并没有 `author` 字段。此时 Mirage 会尝试从标题的 `[名字]`、`【名字】` 括号，或描述中的 `作者：X`、`by X`、`author: X` 等文本里推断作者名。

## 用户属性 general.properties

`general.properties` 描述作品暴露给用户的可调项。Mirage 会据此在侧栏生成控件，你的改动保存为 `propertyOverrides`。每条属性大致包含：

| 字段 | 说明 |
| --- | --- |
| `type` | 属性类型（见下表） |
| `value` | 当前值（布尔、数字或字符串） |
| `text` | 显示标签（可能是 `ui_` 本地化键） |
| `order` / `index` | 排序 |
| `options` | 组合框选项（`label` + `value` + 可选 `condition`） |
| `min` / `max` / `step` / `fraction` | 滑块范围与精度 |
| `condition` | 显示条件 |
| `mode` | 附加模式 |

支持的属性类型（`type`）包括：

```text
bool        布尔开关
slider      数值滑块
color       颜色
combo       下拉组合框（配合 options）
textinput   文本输入
text        纯文本说明
group       分组
file        文件
directory   目录
scenetexture 场景纹理
usershortcut 快捷方式
```

属性标签可能是 `ui_` 前缀的本地化键，Mirage 会用与当前语言匹配的 Wallpaper Engine 官方 `ui_*` 词表解析；解析不到时按键名做人性化处理。壁纸的标题、描述等元数据本身不做翻译。

## 最小示例

由视频转换生成的壁纸包会写入一份最小 `project.json`：

```json
{
  "type": "video",
  "file": "我的视频.mp4",
  "preview": "preview.jpg",
  "title": "我的视频"
}
```

一个带用户属性的场景作品可能类似：

```json
{
  "type": "scene",
  "file": "scene.json",
  "preview": "preview.jpg",
  "title": "Aurora",
  "tags": ["Abstract", "Nature"],
  "contentrating": "Everyone",
  "general": {
    "properties": {
      "brightness": {
        "type": "slider",
        "text": "ui_browse_properties_brightness",
        "value": 1.0,
        "min": 0.0,
        "max": 2.0,
        "step": 0.01,
        "order": 0
      },
      "showtext": {
        "type": "bool",
        "text": "Show text",
        "value": true,
        "order": 1
      }
    }
  }
}
```

关于如何在界面中调整这些属性，见[播放控制与自定义属性](/wallpapers/playback/)。
