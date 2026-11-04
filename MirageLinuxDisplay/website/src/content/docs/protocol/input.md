---
title: 指针输入与窗口状态
description: 指针 enter/leave/motion/button/axis 与 WINDOW_STATE 的报文格式与坐标约定。
---

坐标为输出物理像素、左上角原点；时间戳使用单调微秒时钟；修饰符在已知时使用 Linux 输入修饰符位，否则为零。

## 指针移动

```text
f32 x
f32 y
u64 timestamp_us
u32 modifiers
```

## 指针按键

```text
f32 x
f32 y
u32 button
u32 state
u64 timestamp_us
u32 modifiers
```

按键取 Linux `BTN_LEFT`、`BTN_RIGHT`、`BTN_MIDDLE`、`BTN_SIDE`、`BTN_EXTRA` 码；`state` 为 0 表示释放、1 表示按下。

## 指针滚轮

```text
f32 x
f32 y
f32 delta_x
f32 delta_y
u32 source
u64 timestamp_us
u32 modifiers
```

增量以逻辑滚轮刻度为单位；来源取 wheel=0、finger=1、continuous=2。拖拽通过移动事件与按键状态重建。

## 进入与离开

`POINTER_ENTER` 携带 `x`、`y` 与 `timestamp_us`；`POINTER_LEAVE` 只携带 `timestamp_us`。适配器应保证 enter/leave 与桌面环境的指针焦点语义一致。

## 窗口状态

`WINDOW_STATE { u32 flags }` 携带位标志，由适配器从 DE 的工作区/任务模型计算，绝不来自裸 X11 查询。KDE 适配器从 `org.kde.taskmanager`（Plasma/KWin 工作区数据）读取活跃窗口的焦点、最大化与全屏事实，并以活跃窗口的 frame geometry 与壁纸区域求交判定遮盖。位定义：

| 位 | 值 | 含义 |
|---|---|---|
| covered | `0x1` | 活跃窗口的 frame geometry 与壁纸区域相交（桌面至少部分被覆盖） |
| focusLost | `0x2` | 存在活跃的普通窗口（桌面失去焦点） |
| maximized | `0x4` | 活跃窗口处于最大化 |
| fullscreen | `0x8` | 本屏幕存在非最小化的全屏或最大化窗口，或几何上占满屏幕的窗口（壁纸被完全覆盖；不依赖焦点。Linux 下最大化视同全屏） |

broker 按 route 缓存最近一次 `WINDOW_STATE`，转发为 producer 侧 `PRODUCER_WINDOW_STATE { u32 flags }`；producer 在建立/重建路由时（`OUTPUT_CONFIG` 之后）会收到缓存值的补发，因此先于 producer 连接的显示端上报的状态不会丢失。渲染器可据此暂停/恢复播放（如失焦或全屏覆盖时暂停）。
