# Contextual Presentation Routing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让主窗口右上角“演示全屏”按钮根据当前页进入音视频融合或降噪处理各自的全屏窗口。

**Architecture:** 保留两个现有演示窗口，在 `MainWindow` 增加页签感知的按钮分派；`NoiseReductionPage` 只公开打开自身演示窗口的窄接口。既有页面内部全屏按钮、视频双击和退出逻辑保持不变。

**Tech Stack:** C++20、Qt 6 Widgets、Qt Test、CMake

## Global Constraints

- 在当前工作区增量修改，不回滚或覆盖已有改动。
- 不创建分支，不提交。
- 只改变右上角全局按钮的路由，其他页面和视频双击行为保持不变。
- 生产代码前必须先运行新增测试并确认因错误窗口被打开而失败。

---

### Task 1: 按当前页分派演示全屏

**Files:**
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `src/ui/noisereductionpage.h`
- Modify: `src/ui/noisereductionpage.cpp`
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`
- Modify: `doc/Readme.md`

**Interfaces:**
- Produces: `NoiseReductionPage::showPresentation()`。
- Produces: `MainWindow::toggleCurrentPagePresentation()`。
- Consumes: `QTabWidget::currentWidget()`、现有 `MainWindow::togglePresentationMode()` 和 `NoiseReductionPage::setPresentationMode(bool)`。

- [ ] **Step 1: 写入失败的窗口路由测试**

  在 `DashboardTest` 增加
  `presentationButtonRoutesToCurrentPageFullscreen()`。构造真实
  `MainWindow`，分别选中索引 0 和 `NoiseReductionPage`，点击
  `presentationModeButton`，断言只显示当前页面对应的演示窗口。

- [ ] **Step 2: 验证 RED**

  Run:
  `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui`

  Run:
  `build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug\test_ui.exe presentationButtonRoutesToCurrentPageFullscreen`

  Expected: 在降噪页点击后，`presentationWindow` 可见而
  `noiseReductionPresentationWindow` 不可见，测试失败。

- [ ] **Step 3: 实现最小路由**

  `NoiseReductionPage::showPresentation()` 调用
  `setPresentationMode(true)`。`MainWindow::toggleCurrentPagePresentation()`
  在当前页等于 `m_noiseReductionPage` 时先关闭主演示模式，再调用
  `showPresentation()`；其他页面调用现有
  `togglePresentationMode()`。右上角按钮连接到新分派入口。

- [ ] **Step 4: 验证 GREEN 和完整 UI 回归**

  Run:
  `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui`

  Run:
  `ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure`

  Expected: `test_ui` PASS。

- [ ] **Step 5: 更新文档并执行完整验证**

  在 `doc/Readme.md` 记录右上角按钮按当前页面路由。

  Run:
  `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug`

  Run:
  `ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure`

  Run:
  `python -m unittest discover -s tests/tools -p "test_*.py" -v`

  Run:
  `python tools/mock_device_server.py --self-test`

  Run: `git diff --check`

  Expected: 全部构建、测试、自检和差异检查通过。

## Self-Review

- 覆盖了两个指定页面及窗口互斥要求。
- 新接口名称在测试、头文件和实现计划中一致。
- 未改变其他页面或视频双击行为。
- 没有占位实现或未定义依赖。

