# 演示全屏与深色主题实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将现有演示模式升级为仅保留顶部连接/健康状态与中心可视化的全屏展示界面，并增加可持久化的灯泡按钮浅色/深色主题切换。

**Architecture:** `AppSettings` 持久化经过校验的外观设置；新的 `uitheme` 模块集中生成调色板、样式表和灯泡图标；`MainWindow` 只协调主题应用与监控页区域显隐。演示模式复用现有 QWidget，不搬移或复制媒体控件。

**Tech Stack:** Qt 6.5+ Widgets/Gui/Core、C++20、QSettings、Qt Test、CMake/Ninja。

## Global Constraints

- 在当前 `master` 工作区直接修改，不创建分支，不执行 Git 提交。
- 不回滚、覆盖或清理已有工作区改动。
- 不增加第三方依赖，灯泡图标使用 `QPainter` 生成。
- 演示模式只改变 UI 可见性和窗口状态，不停止任何后台线程或清空数据。
- 所有行为变化同步更新 `doc/Readme.md` 及其变更记录。

---

### Task 1: 外观设置持久化

**Files:**
- Modify: `src/app/appsettings.h`
- Modify: `src/app/appsettings.cpp`
- Test: `tests/app/test_app.cpp`

**Interfaces:**
- Produces: `AppearanceSettings { QString theme = "light"; }`
- Produces: `AppearanceSettings AppSettings::appearance() const`
- Produces: `void AppSettings::saveAppearance(const AppearanceSettings &settings)`

- [ ] **Step 1: 编写失败测试**

在 `AppTest` 增加 `persistsAndSanitizesAppearanceSettings()`，使用临时 INI 验证：

```cpp
AppSettings settings(path);
QCOMPARE(settings.appearance().theme, QStringLiteral("light"));
settings.saveAppearance({QStringLiteral("dark")});
QCOMPARE(AppSettings(path).appearance().theme, QStringLiteral("dark"));

QSettings raw(path, QSettings::IniFormat);
raw.setValue(QStringLiteral("appearance/theme"), QStringLiteral("unknown"));
raw.sync();
QCOMPARE(AppSettings(path).appearance().theme, QStringLiteral("light"));
```

并在保存深色后调用 `resetAll()`，确认恢复 `light`。

- [ ] **Step 2: 运行测试确认按预期失败**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_app --output-on-failure
```

Expected: 编译失败，提示 `AppearanceSettings` 或 `appearance()` 尚未定义。

- [ ] **Step 3: 实现最小设置接口**

在 `appsettings.h` 增加：

```cpp
struct AppearanceSettings {
    QString theme = QStringLiteral("light");
    bool operator==(const AppearanceSettings &) const = default;
};
```

在 `AppSettings` 增加读取和保存方法。读取时仅接受规范化后的 `light`/`dark`，其他值返回默认值；保存时同样校验并立即 `sync()`。

- [ ] **Step 4: 运行设置测试**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_app --output-on-failure
```

Expected: `test_app` 通过。

### Task 2: 集中式主题模块

**Files:**
- Create: `src/ui/uitheme.h`
- Create: `src/ui/uitheme.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Produces: `enum class UiTheme { Light, Dark }`
- Produces: `UiTheme uiThemeFromName(const QString &name)`
- Produces: `QString uiThemeName(UiTheme theme)`
- Produces: `QPalette paletteForUiTheme(UiTheme theme)`
- Produces: `QString styleSheetForUiTheme(UiTheme theme)`
- Produces: `QIcon bulbIconForUiTheme(UiTheme theme)`

- [ ] **Step 1: 编写失败测试**

增加 `uiThemeProvidesDistinctPalettesAndBulbIcons()`：

```cpp
QCOMPARE(uiThemeFromName(QStringLiteral("dark")), UiTheme::Dark);
QCOMPARE(uiThemeFromName(QStringLiteral("invalid")), UiTheme::Light);
QVERIFY(paletteForUiTheme(UiTheme::Light).color(QPalette::Window)
        != paletteForUiTheme(UiTheme::Dark).color(QPalette::Window));
QVERIFY(!styleSheetForUiTheme(UiTheme::Dark).isEmpty());
QVERIFY(!bulbIconForUiTheme(UiTheme::Light).isNull());
QVERIFY(!bulbIconForUiTheme(UiTheme::Dark).isNull());
```

- [ ] **Step 2: 运行测试确认按预期失败**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
```

Expected: 编译失败，提示 `ui/uitheme.h` 或主题接口不存在。

- [ ] **Step 3: 实现主题模块**

使用 Qt 调色板角色定义浅色与深色语义颜色；样式表覆盖主窗口、普通容器、分组框、标签页、输入控件、按钮、文本编辑器、滚动条和工具提示。给主题按钮预留对象名规则，避免覆盖热力图、视频和定位视图的业务绘制。

灯泡图标使用透明 `QPixmap` 和抗锯齿 `QPainter` 绘制灯泡轮廓、灯丝/光芒及底座；浅色主题对应“切换到深色”的亮灯泡，深色主题对应“切换到浅色”的轮廓灯泡。

- [ ] **Step 4: 运行主题模块测试**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: 新主题单元行为通过，原 UI 测试保持通过。

### Task 3: 主窗口主题切换与演示布局

**Files:**
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Consumes: Task 1 的 `AppearanceSettings`
- Consumes: Task 2 的 `UiTheme` 辅助函数
- Produces: 对象名 `themeToggleButton`、`statusBand`、`monitorLeftPanel`、`monitorCenterPanel`、`monitorDiagnosticsPanel`、`monitorSplitter`、`exportDiagnosticsButton`
- Produces: `void MainWindow::applyUiTheme(UiTheme theme, bool persist)`
- Produces: `void MainWindow::setPresentationMode(bool enabled)`

- [ ] **Step 1: 编写演示模式失败测试**

扩展现有 `mainWindowProvidesDashboardAndPresentationToggle()`：

```cpp
QTest::mouseClick(presentation, Qt::LeftButton);
QTRY_VERIFY(window.isFullScreen());
QVERIFY(statusBand->isVisible());
QVERIFY(center->isVisible());
QVERIFY(!left->isVisible());
QVERIFY(!right->isVisible());
QVERIFY(!errorLabel->isVisible());
QVERIFY(!exportButton->isVisible());
QVERIFY(!tabs->tabBar()->isVisible());
QVERIFY(!window.statusBar()->isVisible());

QTest::keyClick(&window, Qt::Key_Escape);
QTRY_VERIFY(!window.isFullScreen());
QVERIFY(left->isVisible());
QVERIFY(right->isVisible());
QVERIFY(errorLabel->isVisible());
QVERIFY(exportButton->isVisible());
QVERIFY(tabs->tabBar()->isVisible());
QVERIFY(window.statusBar()->isVisible());
```

测试同时记录进入前 `QSplitter::sizes()`，退出后验证比例恢复。

- [ ] **Step 2: 运行演示模式测试确认按预期失败**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: 因区域缺少对象名且进入全屏后左右区域仍可见而失败。

- [ ] **Step 3: 实现可逆演示布局**

将状态带、监控页 splitter 及左/中/右面板保存为成员并设置稳定对象名。进入时保存 splitter 比例和先前窗口状态，隐藏规定区域后调用 `showFullScreen()`；退出时恢复显隐、比例与普通/最大化状态。`togglePresentationMode()` 委托给幂等的 `setPresentationMode(bool)`。

重写：

```cpp
void MainWindow::keyPressEvent(QKeyEvent *event);
```

仅在 `m_presentationMode && event->key() == Qt::Key_Escape` 时退出并接受事件，其他情况调用 `QMainWindow::keyPressEvent(event)`。

- [ ] **Step 4: 编写主题按钮失败测试**

增加 `mainWindowTogglesAndPersistsTheme()`，验证：

```cpp
auto *themeButton = window.findChild<QToolButton *>(
    QStringLiteral("themeToggleButton"));
QVERIFY(themeButton != nullptr);
QVERIFY(themeButton->text().isEmpty());
QVERIFY(!themeButton->icon().isNull());
QCOMPARE(window.property("uiTheme").toString(), QStringLiteral("light"));
QTest::mouseClick(themeButton, Qt::LeftButton);
QCOMPARE(window.property("uiTheme").toString(), QStringLiteral("dark"));
QVERIFY(themeButton->toolTip().contains(QStringLiteral("浅色")));
```

测试结束前再次点击恢复浅色，避免全局应用主题污染其他用例。

- [ ] **Step 5: 运行主题按钮测试确认按预期失败**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: 找不到 `themeToggleButton` 或 `uiTheme` 属性。

- [ ] **Step 6: 实现主题按钮与应用流程**

在状态带最右侧创建固定尺寸、无文字、可聚焦且有无障碍名称的 `QToolButton`。`loadSettings()` 在构造阶段应用已保存主题；点击按钮切换主题并调用 `saveAppearance()`。`applyUiTheme()` 设置 `qApp` 调色板和样式表，更新窗口 `uiTheme` 属性、灯泡图标、提示文本和无障碍描述。

“恢复全部默认设置”调用现有 `resetAll()` 后通过 `loadSettings()` 恢复浅色主题。

- [ ] **Step 7: 运行完整 UI 测试**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: `test_ui` 通过。

### Task 4: 文档与完整验证

**Files:**
- Modify: `doc/Readme.md`
- Verify: all modified source and test files

- [ ] **Step 1: 更新功能说明**

在 UI 章节说明：

- 演示全屏只显示顶部连接/健康状态与中心视频、波形、阵列热力图、趋势图。
- 诊断、告警、日志、错误计数和导出诊断在演示全屏隐藏但继续后台记录。
- `Esc`、视频双击和退出按钮均可退出。
- 灯泡按钮切换浅色/深色主题，设置本地持久化。

- [ ] **Step 2: 添加变更记录**

在变更记录顶部增加 `2026-07-26` UI/配置变更，列出演示布局、主题持久化及验证命令。

- [ ] **Step 3: 执行完整构建与测试**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
git diff --check
```

Expected: 构建成功，全部 CTest/Python 测试和模拟设备自检通过，`git diff --check` 无空白错误。

- [ ] **Step 4: 审查工作区**

Run:

```powershell
git status --short
git diff --stat
```

确认仅包含本功能的源码、测试、规格、计划和文档改动，没有生成物、分支或提交。
