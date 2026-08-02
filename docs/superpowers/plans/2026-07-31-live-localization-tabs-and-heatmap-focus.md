# Live Localization Tabs and Heatmap Focus Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将声源定位右侧控制区拆成“实时定位 / 视角校准”页签，新增可持久化的热力聚焦强度，并让四点校准表四行常态可见。

**Architecture:** 聚焦强度属于纯 UI 渲染设置，由 `AppSettings` 持久化并传给 `LiveLocalizationView`，只使热力图缓存失效。页面布局使用一个右侧 `QTabWidget`；实时定位页可以滚动，视角校准页采用固定布局和扩展表格，不改变定位线程或校准数据流。

**Tech Stack:** Qt 6 Widgets、Qt Test、C++20、CMake。

## Global Constraints

- 全程保留当前工作区改动，不创建分支、不暂存、不提交。
- `heatmapFocusExponent` 范围 `[0.5, 4.0]`，默认 `1.5`，稳定键为 `liveLocalization/heatmapFocusExponent`。
- 聚焦只改变颜色映射，不改变定位结果、空间网格、线程配置或 generation。
- 右侧只提供“实时定位”和“视角校准”两个页签。
- 四点校准表固定 4 行、7 列，四行常态可见且禁止垂直滚动。
- 保持现有 Qt/C++ 命名和四空格 Allman 风格，并更新 `doc/Readme.md` 变更记录。

---

### Task 1: Persist Heatmap Focus Setting

**Files:**
- Modify: `src/app/appsettings.h`
- Modify: `src/app/appsettings.cpp`
- Test: `tests/app/test_app.cpp`

**Interfaces:**
- Produces: `LiveLocalizationSettings::heatmapFocusExponent`, default `1.5`.
- Produces: QSettings key `liveLocalization/heatmapFocusExponent`.

- [ ] **Step 1: Write failing persistence tests**

Extend `persistsLiveLocalizationSettings()` with:

```cpp
expected.heatmapFocusExponent = 2.4;
```

Extend `invalidLiveLocalizationFieldsFallBackIndependently()` by writing `4.1` and asserting the default. Add a small table-driven test or repeated reloads for NaN and `0.4` so all invalid classes are covered without mutating production state.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app
build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug\test_app.exe persistsLiveLocalizationSettings invalidLiveLocalizationFieldsFallBackIndependently
```

Expected: compile failure because `heatmapFocusExponent` does not exist.

- [ ] **Step 3: Implement the setting**

Add the field after `heatmapOpacity`; sanitize non-finite and out-of-range values to defaults; load and save the stable key next to opacity.

- [ ] **Step 4: Run focused and full app tests**

Expected: all `test_app` cases pass.

---

### Task 2: Apply Display-Only Heatmap Focus

**Files:**
- Modify: `src/ui/livelocalizationview.h`
- Modify: `src/ui/livelocalizationview.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Consumes: finite focus exponent in `[0.5, 4.0]`.
- Produces: `void LiveLocalizationView::setHeatmapFocusExponent(double exponent)`.

- [ ] **Step 1: Write a failing rendering test**

Add `liveHeatmapFocusExponentSuppressesShoulders()` to the Qt Test slots. Render the same response at exponents `1.0` and `3.0`; sample a shoulder pixel with normalized response strictly between zero and one. Assert the focused image maps it to a lower heat value while the maximum pixel retains the same peak color.

- [ ] **Step 2: Run the focused UI test and verify RED**

Expected: compile failure because the setter is missing.

- [ ] **Step 3: Implement cache-safe focus mapping**

Clamp the setter to `[0.5, 4.0]`, ignore unchanged values, invalidate the heatmap cache and update. In `heatmapImage()`, apply:

```cpp
const double displayValue = std::pow(
    std::clamp(value, 0.0, 1.0),
    m_heatmapFocusExponent);
```

Apply it once, after base/refined likelihood selection and before `colorForValue`, so both paths use identical mapping.

- [ ] **Step 4: Run focused heatmap tests**

Expected: new focus test and existing refined peak, roll, correlation and cache tests pass.

---

### Task 3: Build Two-Tab Control Panel and Visible Calibration Table

**Files:**
- Modify: `src/ui/livelocalizationpage.h`
- Modify: `src/ui/livelocalizationpage.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Produces object names: `liveLocalizationControlTabs`, `liveLocalizationRealtimeTab`, `liveLocalizationCalibrationTab`, `liveHeatmapFocusSpin`.
- Consumes: `LiveLocalizationSettings::heatmapFocusExponent` and view setter from Task 2.

- [ ] **Step 1: Write failing layout and signal tests**

Extend `liveLocalizationPageProvidesCalibrationControls()` to assert:

```cpp
QCOMPARE(tabs->count(), 2);
QCOMPARE(tabs->tabText(0), QStringLiteral("实时定位"));
QCOMPARE(tabs->tabText(1), QStringLiteral("视角校准"));
QVERIFY(realtimeTab->isAncestorOf(focus));
QVERIFY(calibrationTab->isAncestorOf(samples));
QVERIFY(!samples->verticalScrollBar()->isVisible());
```

Also calculate the horizontal-header height plus four row heights and assert the table viewport can show that height. Change focus from `1.5` to `2.5` and assert it persists without emitting `configureWorkerRequested` or a new `spatialGridRequested`.

Add `liveCalibrationTableAdaptsToStyleChanges()`: construct the page first, then apply a larger table font and header padding, compress the window height, and assert the viewport still contains all four actual row heights with scroll maximum zero.

- [ ] **Step 2: Run the focused page test and verify RED**

Expected: missing page tabs and focus control assertions fail.

- [ ] **Step 3: Rebuild the right panel**

Replace the single outer scroll area with `QTabWidget`. Put the existing localization group in a scrollable realtime tab. Put camera and calibration groups directly in the calibration tab. Set the tab panel width to `520..680 px`; set all table columns to `QHeaderView::Stretch` and vertical scrollbar policy to `Qt::ScrollBarAlwaysOff`. Calculate the minimum table height from the actual header, four row heights and frame, then recalculate after `StyleChange`, `FontChange`, `ApplicationFontChange` and polish events.

- [ ] **Step 4: Wire focus restore and persistence**

Create `liveHeatmapFocusSpin` with range `0.5..4.0`, default-restored value and tooltip. On change, update settings, call the view setter and persist only.

- [ ] **Step 5: Run UI tests**

Expected: all `test_ui` cases pass, including 1366×768 layout and cached heatmap performance gates.

---

### Task 4: Documentation and Full Verification

**Files:**
- Modify: `doc/Readme.md`
- Verify: all changed files

**Interfaces:**
- Documents the two-tab layout, focus formula/range/default and calibration table behavior.

- [ ] **Step 1: Update maintained behavior documentation**

Update the realtime localization section and add a 2026-07-31 change-record row. State explicitly that focus is display-only and does not improve or change the numerical localization result.

- [ ] **Step 2: Run full verification**

Run Debug and Release builds, both CTest suites, Release UI cache/performance tests, Python 45-test suite, mock server self-test and `git diff --check`.

- [ ] **Step 3: Inspect final workspace**

Use `git status --short` and focused diffs. Confirm no build artifacts, staging, commits or unrelated edits were introduced.
