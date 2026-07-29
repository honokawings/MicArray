# Noise Reduction Processing Options Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为实时降噪页面增加默认固定参数维纳、独立双点抑制系数、可选带通后处理、监听开关，以及安全的方向和视角参数交互。

**Architecture:** 复用 `WienerNoiseReducer` 的 STFT/OLA 框架，在频域输出阶段选择固定或自适应算法；新增独立 `BandPassFilter` 作为工作线程中的降噪后处理。`NoiseReductionSettings` 作为 UI、工作线程和 `AppSettings` 之间的唯一配置契约。

**Tech Stack:** C++20、Qt 6 Widgets、Qt Multimedia、Qt Test、CMake

## Global Constraints

- 在当前 `master` 工作区现有未提交改动上增量开发，不创建分支或 worktree。
- 不回滚、覆盖或提交现有改动，也不创建 Git commit。
- 固定参数维纳为默认算法；单点系数默认 `3.0`，双点 B1/B2 系数分别默认 `5.0`。
- 带通默认关闭；人声预设 `100–8000 Hz`，音乐预设 `40–16000 Hz`。
- 降噪页面实际角度映射默认 `-90°～90°`，水平和垂直视场角显示及保存默认 `180°`，投影内部限制 `179°`。
- 所有生产代码改动遵循 RED-GREEN-REFACTOR。

---

### Task 1: 配置契约与持久化

**Files:**
- Modify: `src/media/noisereductiontypes.h`
- Modify: `src/app/appsettings.cpp`
- Test: `tests/app/test_app.cpp`

**Interfaces:**
- Produces: `NoiseReductionAlgorithm`、`BandPassPreset`、`BandPassFilterSettings` 以及扩展后的 `NoiseReductionSettings`。
- Consumes: 现有 `NoiseReductionMode`、`DirectionControlMapping`、`CameraViewSettings`。

- [ ] **Step 1: 写入失败测试**

  扩展 `persistsNoiseReductionSettings()`，保存并恢复固定/自适应算法、三个独立系数、监听开关、带通开关/预设/上下限。扩展非法字段测试，分别注入非有限系数、负系数、非法频率和未知枚举，断言仅对应字段回退。新增默认值断言：固定算法、`3/5/5`、监听开、带通关、映射俯仰 `-90/90`、视场角 `180/180`。

- [ ] **Step 2: 验证测试按预期失败**

  Run: `cmake --build build --target test_app`

  Expected: 编译失败，提示新增枚举或设置字段不存在。

- [ ] **Step 3: 实现最小配置模型**

  在 `noisereductiontypes.h` 定义：

  ```cpp
  enum class NoiseReductionAlgorithm {
      FixedParameterWiener,
      AdaptiveWiener,
  };

  enum class BandPassPreset {
      Voice,
      Music,
      Custom,
  };

  struct BandPassFilterSettings {
      bool enabled = false;
      BandPassPreset preset = BandPassPreset::Voice;
      double lowCutoffHz = 100.0;
      double highCutoffHz = 8000.0;
      bool operator==(const BandPassFilterSettings &) const = default;
  };
  ```

  将 `algorithm`、`singleSuppressionCoefficient`、`dualB1SuppressionCoefficient`、`dualB2SuppressionCoefficient`、`monitorEnabled` 和 `bandPass` 加入 `NoiseReductionSettings`，并设置已确认默认值。为降噪设置单独初始化 `camera {0, 0, 180, 180, 0}`，不修改全局 `CameraViewSettings` 默认值。

- [ ] **Step 4: 实现键值转换、逐字段清洗、保存和载入**

  `AppSettings` 使用稳定字符串键 `fixed/adaptive` 和 `voice/music/custom`。系数合法范围设为 `[0, 100]`；视场角在降噪配置中允许 `(1, 180]`，其他页面继续使用原有 `<179` 校验。上下限的最终 Nyquist 校验由工作线程按采样率执行，设置层接受正数且 `low < high`。

- [ ] **Step 5: 验证配置测试通过**

  Run: `cmake --build build --target test_app`

  Run: `ctest --test-dir build -R test_app --output-on-failure`

  Expected: `test_app` PASS。

### Task 2: 固定参数维纳算法

**Files:**
- Modify: `src/media/wienernoisereducer.h`
- Modify: `src/media/wienernoisereducer.cpp`
- Test: `tests/media/test_media.cpp`

**Interfaces:**
- Consumes: `NoiseReductionAlgorithm`、`NoiseReductionMode` 和三个抑制系数。
- Produces: `setAlgorithm(...)`、`setSuppressionCoefficients(double, double, double)`，以及与现有 `process(...)` 相同的时域输出。

- [ ] **Step 1: 写入失败测试**

  新增确定性正弦输入测试。分别把单点系数设为 `0` 和较大值，断言系数为零时输出接近增强信号、较大值时参考频率能量下降；双点模式给 B1/B2 不同频率并分别改变两个系数，断言只增强对应参考频率的抑制。静音和无参考输入必须有限且无 NaN。

- [ ] **Step 2: 验证测试按预期失败**

  Run: `cmake --build build --target test_media`

  Expected: 编译失败，提示算法和系数配置接口不存在。

- [ ] **Step 3: 实现固定增益分支**

  共用窗口、FFT、IFFT 与 OLA，在固定算法分支直接计算：

  ```cpp
  const double enhancedPower = std::norm(enhanced);
  const double denominator = enhancedPower
      + coefficientB1 * std::norm(suppressionB1)
      + coefficientB2 * std::norm(suppressionB2);
  const double gain = denominator > std::numeric_limits<double>::epsilon()
      ? enhancedPower / denominator : 0.0;
  return gain * enhanced;
  ```

  单点模式将 B2 系数置零并使用单点系数。只在自适应算法分支更新谱统计量。算法、模式或系数改变时调用 `reset()`。

- [ ] **Step 4: 验证算法测试通过且自适应回归不变**

  Run: `cmake --build build --target test_media`

  Run: `ctest --test-dir build -R test_media --output-on-failure`

  Expected: 新增固定算法测试和现有自适应测试均 PASS。

### Task 3: 四阶带通后处理

**Files:**
- Create: `src/media/bandpassfilter.h`
- Create: `src/media/bandpassfilter.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/media/test_media.cpp`

**Interfaces:**
- Produces: `BandPassFilter::configure(int sampleRate, double lowCutoffHz, double highCutoffHz)`、`reset()`、`process(const QVector<float> &)`.
- Consumes: 归一化浮点单声道音频。

- [ ] **Step 1: 写入失败测试**

  对 48 kHz 下混合的 40 Hz、1 kHz、12 kHz 信号运行人声带通，丢弃起始瞬态后以相关幅度断言 1 kHz 基本保留、40 Hz 和 12 kHz 明显衰减。增加空输入、非法截止频率和重配置状态重置测试。

- [ ] **Step 2: 验证测试按预期失败**

  Run: `cmake --build build --target test_media`

  Expected: 编译失败，提示 `BandPassFilter` 不存在。

- [ ] **Step 3: 实现级联 Butterworth 滤波**

  使用两个 Direct Form II Transposed 二阶节。按 RBJ 二阶高通/低通公式，`Q = 1 / sqrt(2)`，先高通后低通；每次配置校验 `0 < low < high < sampleRate/2` 并重置四个延迟状态。输出执行有限值和 `[-1, 1]` 保护。

- [ ] **Step 4: 加入构建并验证测试**

  Run: `cmake --build build --target test_media`

  Run: `ctest --test-dir build -R test_media --output-on-failure`

  Expected: `test_media` PASS。

### Task 4: 工作线程流水线与监听开关

**Files:**
- Modify: `src/media/noisereductionworker.h`
- Modify: `src/media/noisereductionworker.cpp`
- Test: `tests/media/test_media.cpp`

**Interfaces:**
- Consumes: 完整 `NoiseReductionSettings` 和 `BandPassFilter`。
- Produces: 维纳后、可选带通后的 `NoiseSignalSource::Denoised` 历史/频谱/监听数据。

- [ ] **Step 1: 写入失败测试**

  给工作线程输入同时包含通带与阻带的可控帧，比较带通关闭和开启后的 `Denoised` 频谱，断言开启后阻带峰值下降。为监听开关提取可观察的 `monitorOutputEnabled()` 状态，断言关闭时不会尝试创建输出设备且仍发布 `plotFrameReady`。

- [ ] **Step 2: 验证测试按预期失败**

  Run: `cmake --build build --target test_media`

  Expected: 测试因后处理或监听状态接口缺失而失败。

- [ ] **Step 3: 实现配置和处理顺序**

  `configure()` 将算法、模式和三个系数下发给降噪器；根据采样率把预设/手动频率限制在 Nyquist 以下并配置带通。`processRawFrame()` 先取得维纳结果，再按开关带通，最后写入 Denoised 历史、图表和监听。关闭监听时立即 `closeAudioOutput()`；`writeMonitorSamples()` 首行检查 `monitorEnabled`。

- [ ] **Step 4: 验证线程测试通过**

  Run: `cmake --build build --target test_media`

  Run: `ctest --test-dir build -R test_media --output-on-failure`

  Expected: `test_media` PASS。

### Task 5: 页面控件和防滚轮误操作

**Files:**
- Modify: `src/ui/noisereductionpage.h`
- Modify: `src/ui/noisereductionpage.cpp`
- Modify: `src/ui/noisereductionvideowidget.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Consumes/Produces: `NoiseReductionSettings`。
- Produces: 对象名稳定的算法、系数、带通和监听控件；安全投影相机副本。

- [ ] **Step 1: 写入失败测试**

  扩展页面默认控件测试，断言固定算法、`3/5/5`、监听开启、带通关闭、俯仰映射 `-90/90` 和视场角 `180/180`。模拟对方向、映射和相机 `QDoubleSpinBox` 发送 `QWheelEvent`，断言值不变；对抑制系数发送滚轮，断言仍可变化。

- [ ] **Step 2: 验证测试按预期失败**

  Run: `cmake --build build --target test_dashboard`

  Expected: 新控件查找失败或默认值不匹配。

- [ ] **Step 3: 构建并连接新控件**

  增加算法、单点系数、双点 B1/B2 系数、监听、带通、预设和上下限控件。预设切换写入确认值；用户编辑截止频率时切换为 `Custom`。通过 `restoreSettings()` 和 `settingsFromUi()` 保持 UI、线程配置与持久化一致。

- [ ] **Step 4: 屏蔽指定控件滚轮并安全投影**

  给方向、映射和相机数值控件安装页面事件过滤器；收到 `QEvent::Wheel` 时接受并返回 `true`。`NoiseReductionVideoWidget::setCameraView()` 保存用于投影的副本，并仅把水平/垂直 FOV 限制到 `179.0`。

- [ ] **Step 5: 验证 UI 测试通过**

  Run: `cmake --build build --target test_dashboard`

  Run: `ctest --test-dir build -R test_dashboard --output-on-failure`

  Expected: `test_dashboard` PASS。

### Task 6: 文档与完整回归

**Files:**
- Modify: `doc/Readme.md`

**Interfaces:**
- Consumes: 最终行为与实际测试命令。
- Produces: 维护文档中的降噪说明和 2026-07-29 变更记录。

- [ ] **Step 1: 更新功能说明和变更记录**

  记录固定/自适应算法、三个独立系数、四阶带通预设、监听关闭、默认角度/FOV 和防滚轮行为。

- [ ] **Step 2: 执行完整构建和测试**

  Run: `cmake --build build`

  Run: `ctest --test-dir build --output-on-failure`

  Run: `python -m unittest discover -s tests/tools -p "test_*.py" -v`

  Run: `python tools/mock_device_server.py --self-test`

  Run: `git diff --check`

  Expected: 构建成功，全部 C++/Python 测试和模拟服务端自检通过，diff 无空白错误。

## Self-Review

- 需求覆盖：算法选择、默认固定算法、三个独立系数、带通开关及两组预设、监听关闭、防滚轮、默认映射和 180° 安全投影均有对应任务。
- 占位符扫描：计划不包含未定义实现项；每个生产改动前均有具体失败测试和预期失败原因。
- 类型一致性：设置类型由 Task 1 定义，Task 2–5 统一消费；带通接口只由 Task 3 定义并由 Task 4 使用。
- 用户约束：计划不创建分支、worktree 或提交，不触碰现有改动之外的回滚操作。

