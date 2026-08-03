<!--
Vendored dependency — do not edit this tree directly.

Origin:   https://github.com/laobamac/MirageLinuxDisplay (sibling repo)
Pinned:   c0ff66dbef67d8ef219326788fcb518c79791344
Sync:     git -C <MirageLinuxDisplay> archive HEAD | tar -x -C Vendors/MirageLinuxDisplay
          then review `git diff --stat Vendors/MirageLinuxDisplay`.
Consumers: MirageQt, SceneRenderer (SceneWallpaper), VideoRenderer (VideoWallpaper)
          build this library via add_subdirectory and include its public headers
          through target_include_directories(Vendors/MirageLinuxDisplay/include).
-->

# MirageLinuxDisplay

为 MirageWallpaper 打造的 Linux 桌面环境显示集成层。项目定义了
`mirage-display-v1` Unix 域套接字协议，并提供稳定的 C 消费库、渲染生产库与
路由核心；KDE Plasma 等桌面环境集成通过该库接收来自 MirageWallpaper 的
DMA-BUF 帧，并将桌面输入回传给渲染器。

X11 与 Wayland 会话均受支持，但二者都通过桌面环境自有的集成点呈现，本项目
**不创建也不管理**裸的 X11 桌面/根窗口。例如 KDE 适配器是一个 Plasma 壁纸
插件，在 Plasma X11 与 Plasma Wayland 上以相同方式运行，工作区信息取自
Plasma/KWin 接口。

## 已实现能力

- **协议**：`mirage-display-v1` 已冻结，`protocol/mirage_display_v1.xml`
  为权威定义，覆盖握手、输出注册、缓冲池绑定、帧与同步、配置、指针输入与
  错误处理。
- **路由核心**（`mirage_display_broker.h`）：持有 `0600` 权限的 Unix
  套接字，通过 `SO_PEERCRED` 校验同 UID 对端，按稳定的输出标识匹配
  一个生产者与多个 DE 消费者，协商精确的格式/修饰符组合，并转发
  DMA-BUF 与同步描述符——像素数据全程留在 GPU 显存，绝不经过 broker 拷贝。
- **消费库**（`mirage_display.h`）：非阻塞握手、缓冲池生命周期、帧接收、
  显式同步、延迟解绑与指针/窗口状态上报，兼容 Qt、GObject 与 Rust FFI。
- **生产库**（`mirage_display_producer.h`）：渲染端会话、缓冲出借、帧提交
  与同步对象管理。
- **GPU 助手**：EGL（`EGL_EXT_image_dma_buf_import`）与 Vulkan
  （external memory FD / DRM 修饰符）导入、同设备 relay/blit 回退，以及
  DRM syncobj 扇出与释放。
- **KDE Plasma 适配器**：Qt Quick 显示项 + Plasma 壁纸包，OpenGL/EGL 与
  Vulkan 双后端，支持多显示器、旋转、缩放、指针转发与窗口状态。

未实现部分（GNOME Shell 适配器、layer-shell 通用消费者）见
`docs/ARCHITECTURE.md` 中的规划。

## 目录结构

```text
include/       公开头文件（稳定 ABI）
protocol/      mirage-display-v1 协议定义（XML）
src/           核心库：编解码、协议、broker、consumer、producer、同步
  common/      内部共享模块（工具、网络、出站队列、握手、DRM）
gpu/           EGL 与 Vulkan DMA-BUF 助手
adapters/kde/  Plasma 6 适配器（Qt Quick 显示项 + 壁纸包）
tests/         协议、会话与 GPU 单元测试
examples/      headless 消费者与 mock broker 示例
docs/          架构、协议与适配器文档
```

## 构建与测试

```sh
# 核心库 + 测试
cmake -S . -B build -G Ninja -DMIRAGE_DISPLAY_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Vulkan 与 EGL 助手在检测到相应开发环境时自动构建；可用
`-DMIRAGE_DISPLAY_WITH_VULKAN=OFF` / `-DMIRAGE_DISPLAY_WITH_EGL=OFF` 关闭。

```sh
# KDE Plasma 适配器（Qt Quick + 壁纸包）
cmake -S . -B build-kde -G Ninja \
  -DMIRAGE_DISPLAY_PLUGIN_QML=ON \
  -DMIRAGE_DISPLAY_WITH_EGL=ON
cmake --build build-kde
```

安装（含 Plasma 壁纸包）：

```sh
cmake --install build-kde --prefix /usr
```

## 文档

- `docs/ARCHITECTURE.md`：整体架构与实现状态
- `docs/protocol.md`：mirage-display-v1 线上协议
- `docs/adapters.md`：桌面环境适配器边界与职责
- `adapters/kde/README.md`：KDE Plasma 适配器说明
