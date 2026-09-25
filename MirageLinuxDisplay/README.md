<h1 align="center">MirageLinuxDisplay</h1>

<p align="center">
  为 MirageWallpaper 打造的 Linux 桌面环境显示集成层。
</p>

<p align="center">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white">
  <img alt="C ABI" src="https://img.shields.io/badge/ABI-C11%20%7C%20C%2B%2B%20%7C%20FFI-blue">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Linux-green">
  <a href="https://elysia-best.github.io/MirageLinuxDisplay/"><img alt="Docs" src="https://img.shields.io/badge/docs-website-67cce9"></a>
</p>

> [!IMPORTANT]
> **MirageLinuxDisplay 当前仍处于早期阶段。** 如果遇到问题，请认真撰写 [GitHub Issue](https://github.com/elysia-best/MirageLinuxDisplay/issues/new/choose)，说明发行版与会话类型（X11 / Wayland）、复现步骤、预期结果、实际现象和相关日志。

MirageLinuxDisplay 是 [MirageWallpaper](https://github.com/laobamac/MirageWallpaper) 的 **Linux 桌面环境显示集成层**。项目定义了 `mirage-display-v1` Unix 域套接字协议，并提供稳定的 C ABI 消费库、渲染生产库与路由核心；KDE Plasma 等桌面环境集成通过该库接收来自 MirageWallpaper 的 DMA-BUF 帧，并把桌面输入回传给渲染器。

X11 与 Wayland 会话均受支持，但二者都通过桌面环境自有的集成点呈现，本项目**不创建也不管理**裸的 X11 桌面/根窗口。例如 KDE 适配器是一个 Plasma 壁纸插件，在 Plasma X11 与 Plasma Wayland 上以相同方式运行，工作区信息取自 Plasma/KWin 接口。

## 0.2.0 ABI

核心实现使用 C++20，但 `include/` 仍只导出可由 C11 和 C++ 使用的 C ABI。0.2.0 将跨语言 FD、超时和状态计数收紧为定宽类型，布尔结果使用 `uint8_t`，并为公开 DTO 固定八字节布局；使用 0.1.x 的下游必须重新编译。线上 `mirage-display-v1.3` 保持既有 FD 所有权和回调顺序，并在生产者输出配置中保留显示端上报的逻辑尺寸。

## 主要能力

- **协议**：`mirage-display-v1` 当前为 **v1.3**，`protocol/mirage_display_v1.xml` 是权威定义，覆盖握手、输出注册、逻辑桌面坐标、缓冲池绑定、帧与同步、配置、指针输入和错误处理。broker 只接受明确声明 v1.3 的端点，不提供旧次版本降级。
- **路由核心**（`mirage_display_broker.h`）：持有 `0600` 权限的 Unix 套接字，通过 `SO_PEERCRED` 校验同 UID 对端，按稳定输出标识匹配一个生产者与多个 DE 消费者，协商精确的格式/修饰符组合，在 `OUTPUT_CONFIG` 中下发消费者的 DRM render node，要求生产者确认同一 GPU 后才接受 DMA-BUF 池与帧；输出首次注册、几何更新和最后一个消费者断开时分别触发生命周期回调。像素数据全程留在 GPU 显存，绝不经过 broker 拷贝。
- **消费库**（`mirage_display.h`）：非阻塞握手、缓冲池生命周期、帧接收、显式同步、延迟解绑与指针/窗口状态上报，兼容 Qt、GObject 与 Rust FFI。
- **生产库**（`mirage_display_producer.h`）：渲染端会话、目标 GPU 绑定、缓冲出借、帧提交与同步对象管理。
- **GPU 助手**：EGL（`EGL_EXT_image_dma_buf_import`）与 Vulkan（external memory FD / DRM 修饰符）导入、同设备 relay/blit 回退，以及 DRM syncobj 扇出与释放。
- **KDE Plasma 适配器**：Qt Quick 显示项 + Plasma 壁纸包，OpenGL/EGL 与 Vulkan 双后端，支持多显示器、旋转、缩放、指针转发与窗口状态。

未实现部分（GNOME Shell 适配器、layer-shell 通用消费者）见 `docs/ARCHITECTURE.md` 中的规划。

## 架构总览

```text
SceneWallpaper / WebWallpaper / VideoWallpaper 生产者
                         |
                 DMA-BUF + 显式同步
                         |
                         v
             嵌入式 md_broker 路由核心（MirageQt 内）
                         |
          $XDG_RUNTIME_DIR/mirage-wallpaper/display-v1.sock
                         |
                         v
                 libmirage-display
             /            |             \
       KDE Plasma     GNOME Shell     layer-shell
       X11/Wayland     X11/Wayland    Wayland only
```

| 组件 | 技术 | 职责 |
| --- | --- | --- |
| 路由核心 | C++20、Linux `AF_UNIX` | 同 UID 校验、输出路由、格式协商、多消费者扇出 |
| 消费库 | C ABI（C11/C++/FFI） | 握手、缓冲池、帧接收、显式同步、指针/窗口状态 |
| 生产库 | C ABI（C11/C++/FFI） | 渲染端会话、缓冲出借、帧提交与同步对象管理 |
| GPU 助手 | EGL / Vulkan | DMA-BUF 导入、relay/blit 回退、syncobj 扇出 |
| KDE 适配器 | Qt Quick + Plasma 6 | 壁纸表面、双后端导入、工作区桥 |

broker 只转发协议报文与文件描述符。像素数据停留在 GPU 显存，从不经过 broker 拷贝；生产者必须使用 `OUTPUT_CONFIG` 指定的消费者 GPU 创建资源，并以 `PRODUCER_GPU_BOUND` 确认后才能出借缓冲。一个 broker 提供稳定的发现、多输出路由，以及在渲染器或桌面壳层重启后的自动恢复；稳定的输出标识取代了旧的屏幕索引约定。

## 目录结构

```text
include/       公开头文件（稳定 C ABI）
protocol/      mirage-display-v1 协议定义（XML）
src/           核心库：编解码、协议、broker、consumer、producer、同步
  common/      内部共享模块（工具、网络、出站队列、握手、DRM）
gpu/           EGL 与 Vulkan DMA-BUF 助手
adapters/kde/  Plasma 6 适配器（Qt Quick 显示项 + 壁纸包）
examples/      headless 消费者与 mock broker 示例
tests/         协议、会话与 GPU 单元测试
docs/          架构、协议与适配器文档
website/       Astro + Starlight 文档网站
.github/workflows/ 文档网站 GitHub Pages 部署
```

## 构建与测试

```sh
# 核心库 + C++20 测试
cmake -S . -B build -G Ninja -DMIRAGE_DISPLAY_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Vulkan 与 EGL 助手在检测到相应开发环境时自动构建；可用 `-DMIRAGE_DISPLAY_WITH_VULKAN=OFF` / `-DMIRAGE_DISPLAY_WITH_EGL=OFF` 关闭。

KDE Plasma 壁纸包单独由 `mirage-wallpaper-package` 目标构建。

```sh
cmake -S . -B build-kde-package -G Ninja \
  -DMIRAGE_DISPLAY_PLUGIN_QML=ON \
  -DMIRAGE_DISPLAY_WITH_EGL=ON \
  -DMIRAGE_DISPLAY_QML_URI=Mirage.DisplayEmbed
cmake --build build-kde-package --target mirage-wallpaper-package
```

输出 `build-kde-package/adapters/kde/mirage-wallpaper-0.2.0.zip` 后，用 KDE 包管理器安装到当前用户：

```sh
kpackagetool6 -t Plasma/Wallpaper \
  -i build-kde-package/adapters/kde/mirage-wallpaper-0.2.0.zip
```

## 文档

- 文档网站：https://elysia-best.github.io/MirageLinuxDisplay/
- `docs/ARCHITECTURE.md`：整体架构与实现状态
- `docs/protocol.md`：mirage-display-v1 线上协议
- `docs/adapters.md`：桌面环境适配器边界与职责
- `adapters/kde/README.md`：KDE Plasma 适配器说明

## 贡献

提交前请至少确认：

1. 核心库在 `-Wall -Wextra -Wpedantic -Werror` 下零警告构建通过；
2. `ctest` 全量测试通过；
3. 公开 C ABI（`include/`）未发生未记录的布局或语义变化；
4. 没有提交构建目录、`_CPack_*`、壁纸包 ZIP 或本地缓存。

## 许可证

KDE 壁纸包元数据（`adapters/kde/wallpaper/metadata.json`）声明 **GPL-3.0-or-later**。MirageLinuxDisplay 与 Valve、Steam 或 Wallpaper Engine 没有关联，也未获得其官方认可。
