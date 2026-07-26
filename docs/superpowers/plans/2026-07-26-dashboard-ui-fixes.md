# Dashboard UI Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复音视频统计、浅色录制通道、演示全屏诊断布局和退出全屏窗口跳变。

**Architecture:** 保持现有 `MainWindow` 页面结构，仅为录制通道和演示摘要增加稳定对象标识及主题规则。演示摘要复用协议统计数据源，窗口恢复改为保存并原子恢复 `Qt::WindowStates`。

**Tech Stack:** Qt 6 Widgets、C++20、Qt Test、CMake/Ninja

## Global Constraints

- 在当前 `master` 工作区未提交改动上继续开发，不回滚或覆盖已有改动。
- 不创建分支，不提交。
- 演示全屏仅新增协议诊断摘要，告警和日志保持隐藏。
- 所有生产代码修改必须先有对应失败测试。

---

### Task 1: 音视频统计与录制通道可读性

**Files:**
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `src/ui/mainwindow.cpp`
- Modify: `src/ui/uitheme.cpp`

**Interfaces:**
- Consumes: `styleSheetForUiTheme(UiTheme::Light)`
- Produces: `audioLevelIndicator` 不可见；`recordChannel_*` 在浅色主题下保持可读

- [x] **Step 1: Write the failing tests**

在 `DashboardTest` 中新增测试，查找 `audioLevelIndicator` 并断言它不在统计布局中可见；切换浅色主题后分别抓取选中和未选中的 `recordChannel_*`，断言中心区域文字像素与背景颜色不同。

- [x] **Step 2: Run tests to verify they fail**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: FAIL，现有电平条可见，浅色录制按钮缺少专用未选中配色。

- [x] **Step 3: Write minimal implementation**

为音频电平控件设置稳定对象名并从可见布局移除；为录制通道按钮设置 `recordingChannelButton=true` 动态属性，并在 `uitheme.cpp` 中定义普通、悬停、选中和禁用状态的配色。

- [x] **Step 4: Run tests to verify they pass**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: PASS。

### Task 2: 演示全屏紧凑诊断卡片

**Files:**
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`

**Interfaces:**
- Consumes: `m_protocolStatsLabel->text()`
- Produces: `presentationDiagnosticsPanel`、`presentationProtocolStatsLabel`

- [x] **Step 1: Write the failing test**

扩展全屏测试：普通模式摘要卡片隐藏；进入全屏后摘要卡片可见且完整右栏隐藏；协议统计更新后两个标签文本一致；退出后摘要卡片再次隐藏。

- [x] **Step 2: Run test to verify it fails**

Run:

```powershell
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: FAIL，找不到 `presentationDiagnosticsPanel`。

- [x] **Step 3: Write minimal implementation**

在监控页分栏中增加仅用于演示模式的紧凑面板，包含标题和独立协议统计标签。更新统计时同步两个标签；`setPresentationMode()` 在完整右栏和紧凑面板之间切换。

- [x] **Step 4: Run test to verify it passes**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: PASS。

### Task 3: 原子恢复窗口状态

**Files:**
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`

**Interfaces:**
- Produces: 保存的 `Qt::WindowStates m_windowStateBeforePresentation`

- [x] **Step 1: Write the failing test**

在最大化恢复测试中监听 `QWindow::windowStateChanged`，退出全屏后断言状态序列直接到达 `Qt::WindowMaximized`，不存在中间 `Qt::WindowNoState`。

- [x] **Step 2: Run test to verify it fails**

Run:

```powershell
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: FAIL，旧实现使用 `showMaximized()` 的恢复路径不能保证单一状态转换。

- [x] **Step 3: Write minimal implementation**

进入前保存 `windowState()`，通过 `setWindowState(Qt::WindowFullScreen)` 进入；退出时通过一次 `setWindowState(m_windowStateBeforePresentation)` 恢复，不调用 `showNormal()` 或 `showMaximized()`。

- [x] **Step 4: Run test to verify it passes**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: PASS。

### Task 4: 文档与完整验证

**Files:**
- Modify: `doc/Readme.md`

**Interfaces:**
- Produces: 2026-07-26 UI 修复变更记录

- [x] **Step 1: Update maintained documentation**

补充音视频统计、浅色录制按钮、演示诊断摘要和原子窗口恢复说明。

- [x] **Step 2: Run full verification**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
git diff --check
```

Expected: 构建成功，全部测试与自检通过，`git diff --check` 无错误。
