# Independent Presentation Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 使用独立全屏演示窗口消除主窗口状态闪跳，并提供尺寸稳定、可手动拖动的视频/诊断分隔条。

**Architecture:** 主窗口不再进入原生全屏；演示期间把顶部状态区和监控中心区临时迁移到独立顶层窗口。视频与诊断卡片由水平 `QSplitter` 承载，诊断文本不参与宽度尺寸提示，拖动比例在运行期保存。

**Tech Stack:** Qt 6 Widgets、C++20、Qt Test、CMake/Ninja

## Global Constraints

- 在当前 `master` 未提交改动上继续开发，不回滚或覆盖已有改动。
- 不创建分支，不提交。
- 不改变媒体、协议、告警和日志后台行为。
- 生产代码修改前必须先有失败测试。

---

### Task 1: 可拖动且稳定的视频诊断分隔条

**Files:**
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`

**Interfaces:**
- Produces: `presentationVideoSplitter`
- Produces: 运行期保存的 `QList<int> m_presentationVideoSplitterSizes`

- [x] **Step 1: Write the failing tests**

扩展演示布局测试，查找名为 `presentationVideoSplitter` 的水平 `QSplitter`，验证视频和诊断卡片是其直接子控件；手动设置分隔尺寸后反复改变诊断文本并处理事件，尺寸必须保持不变；退出再进入后恢复手动比例。

- [x] **Step 2: Run tests to verify they fail**

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: FAIL，当前不存在 `presentationVideoSplitter`。

- [x] **Step 3: Implement the splitter**

用水平 `QSplitter` 替换 `videoRow` 的 `QHBoxLayout`，设置不可折叠、可见分隔条和视频优先伸展；诊断标签使用 `QSizePolicy::Ignored`；进入、退出演示模式时分别恢复和保存尺寸。

- [x] **Step 4: Run tests to verify they pass**

运行 Task 1 Step 2 的命令，Expected: PASS。

### Task 2: 独立演示全屏窗口

**Files:**
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`

**Interfaces:**
- Produces: `presentationWindow`
- Produces: `bool MainWindow::eventFilter(QObject *, QEvent *)`

- [x] **Step 1: Write the failing tests**

调整演示模式测试：点击后 `presentationWindow` 全屏而 `MainWindow` 状态和几何不变；退出后主窗口 `QWindow::windowStateChanged` 信号数为零；状态区和监控中心区返回原父布局。分别验证按钮、视频双击和 `Esc`。

- [x] **Step 2: Run tests to verify they fail**

运行 UI 测试，Expected: FAIL，当前主窗口自身进入全屏且没有独立演示窗口。

- [x] **Step 3: Implement the independent window**

创建带 `Qt::Window | Qt::FramelessWindowHint` 的独立窗口和垂直布局。进入时迁移 `statusBand`、`monitorCenterPanel` 并调用独立窗口 `showFullScreen()`；退出时用最后一帧全屏遮罩覆盖迁移过程，恢复两个控件的父布局并同步重绘主窗口后再隐藏独立窗口。事件过滤器处理 `Esc` 和关闭请求。

- [x] **Step 4: Run tests to verify they pass**

运行 UI 测试，Expected: PASS。

### Task 3: 文档与完整验证

**Files:**
- Modify: `doc/Readme.md`

- [x] **Step 1: Update documentation**

说明独立演示窗口、可拖动分隔条、运行期比例恢复和无主窗口状态跳变。

- [x] **Step 2: Run full verification**

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
git diff --check
```

Expected: 构建成功，全部测试、自检和差异检查通过。
