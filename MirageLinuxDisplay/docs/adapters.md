# 桌面环境适配器

## 边界

`mirage-display-v1` 只传输帧、输出元数据、同步对象与输入事件；它不放置
窗口，也不暴露 X11 或 Wayland 对象。显示消费者必须运行在桌面环境自有的
集成点内。

这条边界是强制的：

```text
MirageWallpaper 渲染器
  -> mirage-display-v1 生产者
  -> MirageQt 内嵌 broker
  -> libmirage-display 消费者
  -> DE 壁纸/背景 API
  -> 合成器或 X 服务器（由 DE 选择）
```

最后一跳属于桌面环境。消费者不得用 Xlib/XCB 桌面窗口或私有 Wayland
toplevel 替代它。

## KDE Plasma

KDE 适配器是 Plasma 6 的 `Plasma/Wallpaper` 包，后端为 Qt Quick 模块；
同一个包在 Plasma X11 与 Plasma Wayland 的 `plasmashell` 中运行，不分化
出裸 X11 宿主。

职责划分：

| 层 | 职责 |
|---|---|
| Plasma `WallpaperItem` | 拥有每屏壁纸表面及其生命周期 |
| Qt Quick 显示项 | 导入 DMA-BUF 帧、绘制，并观察指针事件 |
| Plasma/KWin 工作区桥 | 上报遮盖、活跃、最大化与全屏窗口事实 |
| `libmirage-display` | 协议握手、缓冲池、帧、同步 FD 与输入消息 |

输出标识由 Plasma/Qt 暴露的 `QScreen` 名称、厂商、型号与序列号派生；
几何与设备像素比来自壁纸项的 `Screen` 对象；窗口与工作区状态来自 Plasma
任务模型或 KWin 工作区接口（如 `KWinWorkspaceWrapper`），绝不直接查询
X11 窗口。

Qt Quick 显示项同时支持 **OpenGL/EGL** 与 **Vulkan** 两条导入路径：
- EGL 路径使用 `EGL_EXT_image_dma_buf_import` 与原生 fence 同步；
- Vulkan 路径使用 external memory FD / DRM 修饰符导入，并以同设备
  relay/blit 回退到宿主可采样图像。

壁纸配置提供 Automatic、OpenGL、Vulkan 三档后端偏好。指定后端时，显示项
会在场景图初始化阶段严格校验 plasmashell 的实际 Qt Quick graphics API；
不匹配则停止连接并报告错误。图形设备由 plasmashell 启动时创建，切换后端
需要调整其启动环境并重启 plasmashell，插件不会运行时切换设备。

指针观察必须让 Qt 事件过滤器返回 `false`，使 Plasma 继续接收桌面点击、
右键菜单、拖放与滚轮事件；渲染器由移动事件加按键状态重建拖拽。

## GNOME Shell

GNOME 适配器规划为一个 Shell 扩展加原生纹理导入助手：将纹理插入
Shell 自有的背景 actor，在观察舞台事件时返回
`Clutter.EVENT_PROPAGATE`，并从 Shell/Mutter 接口读取窗口与工作区状态。
若某 GNOME X11 版本暴露所需 Shell API，则沿用同一扩展路径，不引入
根窗口渲染器。仓库中尚无该实现。

## 通用合成器

仅当合成器没有桌面壳层壁纸 API 时，才允许通用适配器。Wayland
layer-shell 是首选回退；合成器自有 API 若能提供更好的输出标识、输入
观察或壁纸生命周期语义，则优先于 layer-shell。仓库中尚无该实现。

刻意不提供通用 X11 回退：X11 支持由桌面环境适配器承担，因为只有 DE 能
可靠维持桌面堆叠、活动、虚拟桌面、壳层输入与重启行为。
