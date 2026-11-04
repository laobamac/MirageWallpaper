---
title: 测试与验证矩阵
description: ctest 测试套件与协议、GPU、桌面环境的验证覆盖。
---

测试目标由 `tests/CMakeLists.txt` 注册，使用 `-Wall -Wextra -Wpedantic -Werror` 编译，并通过 C 编译单元（`c_abi_core`、`c_abi_egl`、`c_abi_vulkan`）持续验证公开头文件对 C11 客户端可用。

## 测试列表

| ctest 名称 | 覆盖 |
|---|---|
| `test_codec` | 报文编解码、magic/头校验、`SCM_RIGHTS` FD 计数 |
| `test_protocol` | 各消息编解码、UTF-8 校验、golden 向量与解析边界 |
| `test_session` | 消费/生产会话、握手状态机、断连清理 |
| `broker` | 路由核心：同 UID 校验、输出路由、格式协商、扇出、代际管理 |
| `test_producer` | 生产端：缓冲出借、帧提交、退役 |
| `test_sync` | DRM syncobj 扇出、sync_file 桥接 |
| `test_vulkan` | Vulkan 导入、relay/blit、导出（检测到 Vulkan 时构建） |
| `test_egl` | EGL 导入与 fence 同步（检测到 EGL 时构建） |

```sh
cmake -S . -B build -G Ninja -DMIRAGE_DISPLAY_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## 验证矩阵

协议层：golden 向量；短包、超长包、尾随字节的拒绝；FD 数组缺失、超量、截断；未知可选/必选 opcode；旧代际丢帧；握手、绑定、帧、解绑各阶段断连；FD 计数回归。

GPU：Intel/AMD/NVIDIA 驱动、同卡与 PRIME 跨卡、线性与修饰符布局、1x/分数/混合缩放、尺寸变化、旋转、热插拔与挂起恢复。

桌面环境：Plasma Wayland 与 Plasma X11 全链路；GNOME 与 layer-shell 按[规划](/adapters/planned/)推进。

## 相关

- [构建与测试](/guides/build/)
- [示例](/dev/examples/)
