---
title: 适配器边界
description: mirage-display-v1 只传输帧、输出元数据、同步对象与输入事件，不放置窗口。
---

`mirage-display-v1` 只传输帧、输出元数据、同步对象与输入事件；它不放置窗口，也不暴露任何 X11 或 Wayland 对象。显示消费者必须运行在桌面环境自有的集成点内。

这条边界是强制的：

```text
MirageWallpaper 渲染器
  -> mirage-display-v1 生产者
  -> MirageQt 内嵌 broker
  -> libmirage-display 消费者
  -> DE 壁纸/背景 API
  -> 合成器或 X 服务器（由 DE 选择）
```

最后一跳属于桌面环境。消费者不得用 Xlib/XCB 桌面窗口或私有 Wayland toplevel 来替代它。

## 适配器的职责

| 层 | 职责 |
|---|---|
| 壁纸宿主 | 拥有每屏壁纸表面及其生命周期 |
| 显示项 | 导入 DMA-BUF 帧、绘制，并观察指针事件 |
| 工作区桥 | 上报遮盖、活跃、最大化与全屏窗口事实 |
| `libmirage-display` | 协议握手、缓冲池、帧、同步 FD 与输入消息 |

## 输出标识与几何

- 输出标识必须**稳定**：派生规则要能跨重连与重启保持一致（如 Plasma/Qt 暴露的 `QScreen` 名称、厂商、型号与序列号），取代旧的屏幕索引约定。
- 几何与设备像素比来自 DE 的屏幕对象，而不是裸 X11 查询；v1.2 还要求上报逻辑桌面 `x/y` 坐标，允许负值。
- 窗口与工作区状态来自 DE 的任务模型或工作区接口。

## 相关

- [KDE Plasma 适配器](/adapters/kde/)
- [规划中的适配器](/adapters/planned/)
- [架构总览](/guides/architecture/)
