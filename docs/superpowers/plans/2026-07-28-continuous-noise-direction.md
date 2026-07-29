# 降噪方向连续角度 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让降噪页面的 A/B1/B2 标记和可编辑参数使用连续实际角度，仅在只读接口码显示和控制帧发送时量化。

**Architecture:** `NoiseReductionSettings` 以三组连续角度作为唯一方向状态，`DirectionControlMapper` 在控制边界生成六路接口码。页面使用小数角度输入和只读码标签，视频控件直接投影连续角度；配置层优先读取新角度字段，并兼容迁移已有接口码字段。

**Tech Stack:** C++20、Qt 6 Core/Widgets/Test、QSettings、现有相机投影与控制协议。

## Global Constraints

- 不创建分支，不提交代码，直接基于当前 `master` 未提交改动继续开发。
- 不回滚或覆盖当前工作区已有降噪功能。
- 视频点击、标记位置、实际角度输入和持久化均使用未量化连续角度。
- 只读接口码显示和发送控制帧时使用线性映射、四舍五入和范围夹紧。
- 设备控制协议仍为方位 `-4..4`、俯仰 `-3..3` 的六路 `qint16`。
- 每项生产行为先写失败测试并确认 RED，再写最小实现。
- 更新 `doc/Readme.md` 和 2026-07-28 变更记录。

---

### Task 1: 连续方向模型与控制边界量化

**Files:**
- Modify: `src/media/noisereductiontypes.h`
- Modify: `src/control/directioncontrolmapper.h`
- Modify: `src/control/directioncontrolmapper.cpp`
- Modify: `tests/media/test_media.cpp`

**Interfaces:**
- Produces: `ContinuousDirectionTarget { double azimuthDegrees; double elevationDegrees; }`
- Produces: `DirectionControlMapper::controlValues(const ContinuousDirectionTarget &, const ContinuousDirectionTarget &, const ContinuousDirectionTarget &, const DirectionControlMapping &)`
- Consumes: `DirectionControlMapping`

- [x] **Step 1: 写连续角度仅在控制边界量化的失败测试**

在 `MediaTest` 增加：

```cpp
void MediaTest::quantizesContinuousNoiseDirectionsOnlyAtControlBoundary()
{
    DirectionControlMapping mapping;
    const ContinuousDirectionTarget a {11.2, -7.4};
    const ContinuousDirectionTarget b1 {-38.8, 12.6};
    const ContinuousDirectionTarget b2 {74.9, -33.3};

    const ControlValues values =
        DirectionControlMapper::controlValues(a, b1, b2, mapping);
    QCOMPARE(values.channelValue(0), qint16(0));
    QCOMPARE(values.channelValue(1), qint16(0));
    QCOMPARE(values.channelValue(2), qint16(-2));
    QCOMPARE(values.channelValue(3), qint16(1));
    QCOMPARE(values.channelValue(4), qint16(3));
    QCOMPARE(values.channelValue(5), qint16(-2));
    QCOMPARE(a.azimuthDegrees, 11.2);
    QCOMPARE(a.elevationDegrees, -7.4);
}
```

再增加连续角度夹紧测试：输入超出映射范围时，返回夹紧后的连续方向，但不得先量化再反算。

- [x] **Step 2: 运行媒体测试确认 RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media
```

Expected: 编译失败，缺少 `ContinuousDirectionTarget` 和新的 `controlValues` 重载。

- [x] **Step 3: 实现连续方向类型和控制边界量化**

在 `noisereductiontypes.h` 增加：

```cpp
struct ContinuousDirectionTarget
{
    double azimuthDegrees = 0.0;
    double elevationDegrees = 0.0;
    bool operator==(const ContinuousDirectionTarget &) const = default;
};
```

将 `NoiseReductionSettings::enhancedA/suppressionB1/suppressionB2` 改为该类型。增加：

```cpp
static ContinuousDirectionTarget clampDirection(
    const ContinuousDirectionTarget &target,
    const DirectionControlMapping &mapping);

static ControlValues controlValues(
    const ContinuousDirectionTarget &enhancedA,
    const ContinuousDirectionTarget &suppressionB1,
    const ContinuousDirectionTarget &suppressionB2,
    const DirectionControlMapping &mapping);
```

`controlValues()` 分别调用 `azimuthCode()` 和 `elevationCode()`；不得调用 `azimuthDegrees()` 或 `elevationDegrees()` 回写输入。

- [x] **Step 4: 更新已有映射测试并确认 GREEN**

将旧 `DirectionTarget` 控制测试改为连续角度输入，保留角度↔码基础映射测试。

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_media --output-on-failure
```

Expected: `test_media` 通过。

---

### Task 2: 连续角度持久化与旧接口码迁移

**Files:**
- Modify: `src/app/appsettings.cpp`
- Modify: `tests/app/test_app.cpp`

**Interfaces:**
- Consumes: `NoiseReductionSettings` 的三组 `ContinuousDirectionTarget`
- Produces: 新键 `enhancedAAzimuthDegrees`、`enhancedAElevationDegrees`、`suppressionB1AzimuthDegrees` 等六个连续角度字段
- Migrates: 旧键 `enhancedAAzimuthCode`、`enhancedAElevationCode` 等六个接口码字段

- [x] **Step 1: 写连续角度往返和旧配置迁移失败测试**

连续角度往返断言使用非量化网格值：

```cpp
stored.enhancedA = {11.2, -7.4};
stored.suppressionB1 = {-38.8, 12.6};
stored.suppressionB2 = {74.9, -33.3};
settings.saveNoiseReduction(stored);
const NoiseReductionSettings loaded = settings.noiseReduction();
QCOMPARE(loaded.enhancedA, stored.enhancedA);
QCOMPARE(loaded.suppressionB1, stored.suppressionB1);
QCOMPARE(loaded.suppressionB2, stored.suppressionB2);
```

迁移测试直接写入旧 `*Code` 键和映射范围，断言加载角度等于旧码反算值。再写新 `*Degrees` 键，断言新字段优先于旧字段。

- [x] **Step 2: 运行配置测试确认 RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app
```

Expected: 编译失败或往返断言失败，因为配置层仍保存接口码。

- [x] **Step 3: 实现连续字段读取、保存和迁移**

读取顺序：

1. 先读取并验证 `DirectionControlMapping`。
2. 对每个方向，如果两个新 `*Degrees` 字段均存在且为有限数，则读取连续角度。
3. 否则读取旧 `*Code` 字段并使用映射反算。
4. 调用 `DirectionControlMapper::clampDirection()` 逐方向夹紧。

保存时只写新连续角度字段；保留旧字段不影响加载优先级，不主动删除用户本地数据。

- [x] **Step 4: 运行配置与媒体测试确认 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app test_media
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_app|test_media" --output-on-failure
```

Expected: `test_app`、`test_media` 通过。

---

### Task 3: 连续位置视频标记与页面输入

**Files:**
- Modify: `src/ui/noisereductionvideowidget.h`
- Modify: `src/ui/noisereductionvideowidget.cpp`
- Modify: `src/ui/noisereductionpage.h`
- Modify: `src/ui/noisereductionpage.cpp`
- Modify: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Produces: `NoiseReductionVideoWidget::setDirectionTargets(const ContinuousDirectionTarget &, ...)`
- Produces: 六个可编辑 `QDoubleSpinBox` 实际角度控件
- Produces: 六个只读接口码 `QLabel`
- Consumes: `DirectionControlMapper::controlValues(...)` 和 `clampDirection(...)`

- [x] **Step 1: 写任意点击不吸附和小数输入失败测试**

修改视频点击测试，点击视频宽度的 `0.63` 位置；从当前相机参数手工确定预期连续方位角，并断言：

```cpp
const double clickedAzimuth = page.currentSettings().suppressionB1.azimuthDegrees;
QVERIFY(std::abs(clickedAzimuth - expectedAzimuth) < 0.05);
const double quantizedAngle = DirectionControlMapper::azimuthDegrees(
    page.currentControlValues().channelValue(2), mapping);
QVERIFY(std::abs(clickedAzimuth - quantizedAngle) > 0.5);
```

为避免暴露整份可变状态，只增加只读 `NoiseReductionSettings currentSettings() const`。

新增手动输入测试：把 B2 方位设为 `17.3°`、俯仰设为 `-8.6°`，断言输入值保持不变、只读码为 `1/-1`、视频控件收到连续方向。

- [x] **Step 2: 运行 UI 测试确认 RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
```

Expected: 编译失败，因为页面仍使用整数输入和量化方向状态。

- [x] **Step 3: 视频控件改为直接投影连续角度**

`drawTarget()` 直接构造：

```cpp
SoundSourceDirection direction;
direction.azimuthDegrees = target.azimuthDegrees;
direction.elevationDegrees = target.elevationDegrees;
```

接口码只用于标签：

```cpp
const qint16 azimuthCode =
    DirectionControlMapper::azimuthCode(target.azimuthDegrees, m_mapping);
const qint16 elevationCode =
    DirectionControlMapper::elevationCode(target.elevationDegrees, m_mapping);
```

- [x] **Step 4: 页面改为小数角度输入和只读码**

将 `m_directionSpins` 改为六个 `QDoubleSpinBox *`，对象名改为：

```text
noiseDirectionAAzimuthDegreesSpin
noiseDirectionAElevationDegreesSpin
noiseDirectionB1AzimuthDegreesSpin
noiseDirectionB1ElevationDegreesSpin
noiseDirectionB2AzimuthDegreesSpin
noiseDirectionB2ElevationDegreesSpin
```

增加只读标签：

```text
noiseDirectionAAzimuthCodeLabel
noiseDirectionAElevationCodeLabel
noiseDirectionB1AzimuthCodeLabel
noiseDirectionB1ElevationCodeLabel
noiseDirectionB2AzimuthCodeLabel
noiseDirectionB2ElevationCodeLabel
```

角度输入使用一位小数和 `0.1°` 步长。`refreshDirectionViews()` 同步输入范围、六个码标签及普通/全屏标记。

`handleVideoPoint()` 保存反投影连续角度并夹紧，使用 `QSignalBlocker` 更新所选两个输入，然后调用 `requestControlSend()`；不得把接口码反写为角度。

- [x] **Step 5: 运行 UI 测试确认 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: `test_ui` 通过，任意点击位置不吸附。

---

### Task 4: 文档与完整回归

**Files:**
- Modify: `doc/Readme.md`
- Modify: `docs/superpowers/plans/2026-07-28-continuous-noise-direction.md`

**Interfaces:**
- Consumes: Tasks 1-3 的最终行为
- Produces: 连续角度、只读量化码和旧配置迁移说明

- [x] **Step 1: 更新维护文档**

将实时阵列降噪章节改为：

- 点击位置和实际角度使用连续值，标记不吸附到接口网格。
- 六个实际角度可编辑，六个接口码只读。
- 接口码仅显示和发送时量化。
- 旧接口码配置自动迁移。

在 2026-07-28 变更记录追加本次细节修正。

- [x] **Step 2: 完整构建与全量验证**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
git diff --check
git status --short --branch
```

Expected: 构建成功；全部 C++、Python 测试和模拟设备自检通过；差异仅包含当前降噪功能及本次连续角度修正。
