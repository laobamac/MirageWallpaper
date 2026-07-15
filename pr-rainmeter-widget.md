# PR: 新增雨滴（Rainmeter）桌面小组件支持

## 概述

为 Mirage Wallpaper 新增对 Rainmeter `.rmskin` 格式桌面小组件的完整支持。用户可以导入、浏览、筛选、应用和管理 Rainmeter 皮肤包，将小组件渲染为 macOS 桌面上可拖动的浮动窗口。

---

## 新增功能

### 用户面向

- **导入 .rmskin 包**：支持拖放或点击按钮导入 Rainmeter 皮肤包（ZIP 格式，自动解压到 `~/Library/Application Support/Mirage/Rmskins/`）
- **小组件标签页**：在"已安装"和"发现"之间新增"小组件"标签，图标为 `square.grid.2x2.fill`
- **网格浏览**：以卡片网格展示已安装主题，每张卡片含预览图、名称、LoadType、版本和作者
- **筛选与搜索**：左侧边栏支持按 LoadType / Version 筛选；顶部搜索框支持全文检索
- **预览与元数据**：右侧面板展示大预览图、完整元信息（作者、加载类型、版本、最低 Rainmeter 版本）
- **多主题并行**：支持同时应用多个小组件主题，每个运行在独立进程中，互不干扰
- **浮动桌面窗口**：小组件渲染为无边框、可拖动的桌面层级窗口，位于普通应用之下、桌面之上，跨所有 Space
- **会话持久化**：退出时记住已启用的小组件，下次启动自动恢复

### 渲染能力

- 解析 Rainmeter skin `.ini`：Section、Measure、Meter、MeterStyle、变量、Bang 命令
- Measure 实现：CPU、内存、时间/日期、正常运行时间、脚本、注册表等
- Meter 实现：String（含 InlineSetting）、Image、Bar、Histogram、Roundline、Shape（矩形/椭圆/路径/合并）
- 数学表达式求值：`#VAR#` 变量替换、`[MeasureName]` 动态值、嵌套括号、条件表达式
- 布局解析：Rainmeter Layout 的 `Rainmeter.ini`（Active、WindowX/WindowY、Anchor）、坐标百分比 / 像素
- 公式容错：对 `#WORKAREAWIDTH#` 等无法解析的 Rainmeter 内置变量，自动使用右上角级联布局

---

## 文件变更清单

### 新增文件

#### Swift 端（Mirage 主应用）

| 文件 | 说明 |
|------|------|
| `Services/RmskinTheme.swift` | 数据模型：解析 `RMSKIN.ini`，提取元数据和预览图 |
| `Services/RmskinLibrary.swift` | 文件管理：安装/扫描/删除主题，解压 `.rmskin` 归档 |
| `Services/RmskinController.swift` | 进程管理：为每个主题启动独立 `RmskinWallpaper` 子进程，通过 stdin JSON 控制 |
| `ContentView/ViewModels/RmskinViewModel.swift` | ViewModel：主题列表、筛选/搜索、多控制器生命周期、会话持久化、拖放导入 |
| `ContentView/Components/Widget/RmskinItem.swift` | 小组件卡片视图（预览图、名称、标签、已应用绿色勾标、右键菜单） |
| `ContentView/Components/Widget/WidgetExplorer.swift` | 网格浏览视图（`LazyVGrid` + 拖放导入） |
| `ContentView/Components/Widget/WidgetFilterSidebar.swift` | 左侧筛选边栏（LoadType / Version checkbox 筛选） |
| `ContentView/Components/Widget/WidgetPreview.swift` | 右侧详情面板（大预览图、元数据、应用/停止按钮） |

#### C++/ObjC 端（RmskinRenderer 渲染引擎）

整个 `RmskinRenderer/` 目录，使用纯 Apple 框架，零第三方依赖：

| 文件 | 说明 |
|------|------|
| `RMSkin.h/mm` | 单个 Rainmeter 皮肤运行时：加载 ini、Measure/Meter 树、tick、绘制、Bang 执行 |
| `RMLayout.h/mm` | 布局解析器：`RMSKIN.ini` + `Rainmeter.ini`，标记未解析公式 |
| `RMConfigParser.h/mm` | 皮肤 `.ini` 解析，构建 Measure/Meter 对象树 |
| `RMMeasure.h/mm` | Measure 实现（CPU/内存/时间/脚本等数据源） |
| `RMMeter.h/mm` | Meter 渲染（String/Image/Bar/Histogram/Shape 等） |
| `RMMathParser.h/mm` | 公式/变量解析（`#VAR#`、`[Measure]`、嵌套表达式） |
| `RMIniFile.h/mm` | INI 文件读取，兼容 Rainmeter 方言 |
| `RMFontManager.h/mm` | 字体管理（Rainmeter FontFace/FontWeight → macOS 系统字体） |
| `RMBangs.h/mm` | Bang 命令执行（!SetVariable/!Refresh/!ToggleConfig 等） |
| `RMSkinView.h/mm` | 翻转坐标系 NSView（`isFlipped=YES`），定时器驱动 tick + 重绘，拖拽/点击/滚轮转发 |
| `RmskinWallpaper.mm` | 桌面 Host 入口：多窗口创建、stdin JSON 控制协议 |
| `ControlChannel.h/m` | 逐行 JSON 控制通道（stdin 读取 + 主线程回调） |
| `RmskinViewer.mm` | 调试查看器工具 |
| `CMakeLists.txt` (x3) | CMake 构建配置，C++20/ObjC++20，macOS 14.0 target |
| `scripts/build.sh` | 构建脚本（Ninja + CMake presets） |

### 修改文件

| 文件 | 变更 |
|------|------|
| `AppDelegate.swift` | 新增 `rmskinViewModel` 实例；`applicationWillTerminate` 中调用 `stopAll()` |
| `ContentView/ContentView.swift` | 新增 `case 3` 标签页路由（Widget 视图） |
| `ContentView/Components/TopTabBar.swift` | 新增第 4 个按钮"小组件" |
| `Mirage/scripts/bundle_renderers.sh` | 将 `RmskinWallpaper` 打包到 `Resources/Renderers/` |
| `.github/workflows/build-macos.yml` | 新增 RmskinRenderer 构建步骤 |
| `scripts/build_all.sh` | 新增 RmskinRenderer 构建 |
| `README.md` | 功能列表、渲染架构表、仓库结构更新 |

---

## Bug 修复（随本 PR 一同提交）

### 1. SMAppService "Operation not permitted"

**文件**: `Services/GlobalSettingsService.swift`

**原因**: `didAddToLoginItem` 无条件调用 `register()`/`unregister()`，当已处于目标状态时触发权限错误。

**修复**: 添加 `appService.status` 检查，仅在状态不一致时操作；失败时回退 UI 设置。

### 2. "No symbol named '' found in system symbol set"

**文件**: `ContentView/ViewModels/FilterResultsViewModel.swift`

**原因**: `FRShowOnly.allOptions` 中"音频响应"和"可自定义"的 SF Symbol 名称为空字符串。

**修复**: 替换为有效符号 `"waveform.path.ecg"` 和 `"slider.horizontal.3"`。

### 3. NSHostingView Update Constraints 无限循环

**文件**: `ContentView/MainWindow.swift`

**原因**: 窗口初始尺寸 (480×300) 小于 SwiftUI 声明的 `minWidth: 1000, minHeight: 640`，导致约束循环。

**修复**: 
- 窗口初始尺寸改为 `1029×669`
- 设置 `contentMinSize = NSSize(width: 1000, height: 640)`
- 使用 `autoresizingMask` 替代 Auto Layout 约束

---

## 架构设计

```
┌──────────────────────────────────────────────────────────┐
│  Mirage.app (SwiftUI / AppKit)                           │
│                                                          │
│  ┌──────────────┐  ┌───────────────┐  ┌──────────────┐  │
│  │ ContentView  │  │RmskinViewModel│  │RmskinLibrary │  │
│  │  Tab: 小组件  │──│   (状态管理)   │──│  (文件管理)   │  │
│  └──────────────┘  └───────┬───────┘  └──────────────┘  │
│                            │                              │
│                    ┌───────┴───────┐                     │
│                    │RmskinController│ (每个主题一个)      │
│                    └───────┬───────┘                     │
│                            │ stdin JSON                   │
└────────────────────────────┼─────────────────────────────┘
                             │
     ┌───────────────────────┼───────────────────────┐
     ▼                       ▼                       ▼
┌─────────┐           ┌─────────┐           ┌─────────┐
│Rmskin   │           │Rmskin   │           │Rmskin   │
│Wallpaper│           │Wallpaper│           │Wallpaper│
│(进程 1) │           │(进程 2) │           │(进程 3) │
│         │           │         │           │         │
│ RMSkin  │           │ RMSkin  │           │ RMSkin  │
│ View(s) │           │ View(s) │           │ View(s) │
└─────────┘           └─────────┘           └─────────┘
  浮动窗口              浮动窗口              浮动窗口
```

### 关键设计决策

| 决策 | 理由 |
|------|------|
| **多进程架构** | 每个主题独立进程，崩溃不影响其他主题或主应用 |
| **纯 Apple 框架** | 零 Homebrew 依赖，降低构建复杂度和分发体积 |
| **桌面层级窗口** | `kCGNormalWindowLevel-1`，位于普通应用之下、桌面之上；`canJoinAllSpaces` + `stationary` 跨所有 Space |
| **坐标系转换** | Rainmeter 左上角原点 ↔ AppKit 左下角原点，通过 `isFlipped=YES` + 坐标转换处理 |
| **公式容错** | 对 `#WORKAREAWIDTH#` 等无法解析的公式，使用右上角级联布局降级 |
| **会话持久化** | `appliedThemeIDs` 持久化到 `UserDefaults`，启动时自动恢复，退出时不丢失 |
| **进程控制协议** | stdin 逐行 JSON，与现有 `RendererController` 模式一致 |

---

## 小组件生命周期

```
用户拖放 .rmskin
      │
      ▼
  importThemes(urls:)
      │
      ├── unzip .rmskin → 搜索 RMSKIN.ini → 复制到 Rmskins/ 目录
      │
      ▼
  refresh() → loadAll()  →  卡片网格展示
      │
      ▼
  用户点击"应用"
      │
      ▼
  apply(theme)
      │
      ├── 创建 RmskinController → 启动 RmskinWallpaper 子进程
      ├── 解析 RMSKIN.ini → 加载 Layout Rainmeter.ini
      ├── 为每个 Active skin 创建浮动窗口 + RMSkinView
      ├── 计时器驱动 tick() → 更新 Measure → 重绘 Meter
      ├── appliedThemeIDs 写入 UserDefaults
      │
      ▼
  用户点击"停止" / 应用其他主题
      │
      ├── stop() → stdin 发送 {"cmd":"quit"} → 进程退出 → 清理窗口
      └── appliedThemeIDs 移除并保存
      
App 退出 → stopAll() → 停止所有进程（保留 appliedThemeIDs）
      │
      ▼
下次启动 → init() 从 UserDefaults 恢复 → refresh() → restoreActiveThemesIfNeeded()
```

---

## 构建与测试

### 构建 RmskinRenderer

```bash
cd RmskinRenderer
scripts/build.sh              # release 构建
scripts/build.sh debug        # debug 构建
```

### 调试单个主题

```bash
RmskinRenderer/build/release/Tools/RmskinViewer/RmskinViewer "Quanto_1.31 Release.rmskin"
```

### 打包到 App Bundle

```bash
Mirage/scripts/bundle_renderers.sh
```

### 测试要点

1. **导入**：拖放 `.rmskin` 文件或目录 → 验证自动解压和卡片展示
2. **筛选**：按 LoadType / Version 切换 → 验证列表正确过滤
3. **搜索**：输入作者名或主题名 → 验证实时过滤
4. **应用/停止**：点击应用 → 验证浮动窗口出现在桌面，可拖动、可点击交互
5. **多主题并行**：同时启用 2+ 主题 → 验证各自独立运行
6. **会话恢复**：启用小组件 → 退出 App → 重新启动 → 验证小组件自动恢复
7. **公式容错**：使用含 `#WORKAREAWIDTH#` 公式的主题 → 验证右上角级联布局
8. **删除**：右键删除已安装主题 → 先停止再删除文件
