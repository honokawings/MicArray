# 48 通道软件波束形成降噪 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为降噪页面增加不改设备协议的 48 通道软件波束形成模式，并可选择复用实时声源定位的视频校准参数。

**Architecture:** 新建独立的流式分数延时波束形成器，将 CH0–CH47 合成为连续角度 A/B1/B2，再适配为现有四路降噪输入，使维纳、后处理、监听、绘图和录制保持原结构。输入模式及校准复用存入 `NoiseReductionSettings`；视频控件在角度线性投影和定位针孔投影之间成对切换绘制/反投影。

**Tech Stack:** C++20、Qt 6 Widgets/Multimedia/Test、CMake、Qt `QSettings`

## Global Constraints

- 不修改设备控制协议，不发送 FPGA 模式切换命令，不使用控制帧保留字段。
- 默认输入模式必须保持当前 CH1/CH2/CH3 FPGA 波束形成行为。
- 直接处理模式读取设备既有音频帧的 CH0–CH47，设备端原始阵元输出由外部配置保证。
- 软件波束形成必须使用 `double` 连续实际角度，不使用量化接口码。
- 仅在“直接处理 + 复用校准”时使用实时定位针孔投影，其他路径保持当前角度线性广角投影。
- 不创建分支，不提交，不覆盖工作区已有改动。

---

## File Map

- Create `src/media/fractionaldelaybeamformer.h/.cpp`: 48 路帧解码、流式历史、三方向分数延时求和。
- Modify `src/media/noisereductiontypes.h`: 输入模式和校准复用设置。
- Modify `src/media/noisereductionworker.h/.cpp`: 按模式选择旧解码器或软件波束形成器。
- Modify `src/app/appsettings.cpp`: 新设置键的读取、清洗和保存。
- Modify `src/ui/noisereductionvideowidget.h/.cpp`: 可切换且正反一致的投影模型。
- Modify `src/ui/noisereductionpage.h/.cpp`: 输入模式 UI、校准复用 UI 和最新相机参数同步。
- Modify `CMakeLists.txt`: 注册新媒体源文件。
- Modify `tests/media/test_media.cpp`, `tests/app/test_app.cpp`, `tests/ui/test_dashboard.cpp`: 分层回归。
- Modify `doc/Readme.md`: 用户行为、设备前置条件和变更记录。

### Task 1: 设置模型与持久化

**Files:**
- Modify: `src/media/noisereductiontypes.h`
- Modify: `src/app/appsettings.cpp:980-1100,1436-1530`
- Test: `tests/app/test_app.cpp:800-1040`

**Interfaces:**
- Produces: `enum class NoiseReductionInputMode { FpgaBeamformedChannels, Raw48SoftwareBeamforming };`
- Produces: `NoiseReductionSettings::inputMode` and `NoiseReductionSettings::reuseLocalizationVideoCalibration`.

- [ ] **Step 1: Write failing settings tests**

在默认值测试中断言 FPGA 模式和不复用；在往返测试中写入直接处理与复用；在非法值测试中写入 `noiseReduction/inputMode=invalid` 并断言回退：

```cpp
QCOMPARE(actual.inputMode,
         NoiseReductionInputMode::FpgaBeamformedChannels);
QVERIFY(!actual.reuseLocalizationVideoCalibration);

expected.inputMode = NoiseReductionInputMode::Raw48SoftwareBeamforming;
expected.reuseLocalizationVideoCalibration = true;
settings.saveNoiseReduction(expected);
QCOMPARE(settings.noiseReduction(), expected);
```

- [ ] **Step 2: Run the targeted test and verify RED**

Run: `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app && ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_app --output-on-failure`

Expected: 编译失败，提示 `NoiseReductionInputMode` 或成员不存在。

- [ ] **Step 3: Add the enum, members, metatype, and string mapping**

```cpp
enum class NoiseReductionInputMode {
    FpgaBeamformedChannels,
    Raw48SoftwareBeamforming,
};

struct NoiseReductionSettings {
    NoiseReductionInputMode inputMode =
        NoiseReductionInputMode::FpgaBeamformedChannels;
    bool reuseLocalizationVideoCalibration = false;
    // existing fields
};
```

在 `AppSettings` 中以字符串 `fpga-beamformed` / `raw-48-software` 保存，未知字符串回退 `fpga-beamformed`；布尔键名为 `reuseLocalizationVideoCalibration`。

- [ ] **Step 4: Run test_app and verify GREEN**

Run: 上一步命令。

Expected: `test_app` PASS。

### Task 2: 分数延时波束形成器

**Files:**
- Create: `src/media/fractionaldelaybeamformer.h`
- Create: `src/media/fractionaldelaybeamformer.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/media/test_media.cpp`

**Interfaces:**
- Consumes: `ContinuousDirectionTarget`, `createDefaultArrayGeometry()`, existing 48×200 packed payload format.
- Produces:

```cpp
struct BeamformedDirectionFrame {
    QVector<float> firstElement;
    QVector<float> enhancedA;
    QVector<float> suppressionB1;
    QVector<float> suppressionB2;
    bool valid = false;
    QString error;
};

class FractionalDelayBeamformer {
public:
    void configure(int sampleRate,
                   const ContinuousDirectionTarget &enhancedA,
                   const ContinuousDirectionTarget &suppressionB1,
                   const ContinuousDirectionTarget &suppressionB2);
    BeamformedDirectionFrame process(const QByteArray &payload);
    void reset();
};
```

- [ ] **Step 1: Add deterministic synthetic plane-wave test helpers**

构造 48 通道小端 32 位容器，使用默认阵列几何和三次插值生成 17.3°/20.7° 的连续正弦平面波；连续生成足够帧以越过共同启动延迟。

```cpp
QByteArray planeWavePayload(const ContinuousDirectionTarget &source,
                            double frequencyHz,
                            int frameIndex,
                            int sampleRate);
double rms(const QVector<float> &samples, int skipSamples);
```

- [ ] **Step 2: Write failing beamformer tests**

覆盖：同相正前方幅度保持；匹配方向 RMS 大于偏离 45° 方向；17.3° 与 20.7° 即使量化码相同也产生不同输出；每路 200 样本；跨帧连续；错误尺寸返回 `valid=false`。

```cpp
QVERIFY(matchedRms > offAxisRms * 1.5);
QCOMPARE(frame.enhancedA.size(), NoiseReductionFrameDecoder::SamplesPerFrame);
QCOMPARE(frame.enhancedA.size(), frame.suppressionB1.size());
QVERIFY(!invalid.valid);
```

- [ ] **Step 3: Run test_media and verify RED**

Run: `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media && ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_media --output-on-failure`

Expected: 编译失败，新类不存在。

- [ ] **Step 4: Implement fixed geometry and coefficient precomputation**

每个方向、阵元存储整数延时和四个 Lagrange 系数。统一参考延时取所有阵元最大绝对传播延时向上取整再加 2 个保护样本；延时为因果历史索引。方向变化重算系数，不清空历史。

```cpp
double relativeSamples =
    dot(position, directionVector) * sampleRate / 343.0;
double causalSamples = commonReferenceSamples + relativeSamples;
int integerDelay = static_cast<int>(std::floor(causalSamples));
double fraction = causalSamples - integerDelay;
```

- [ ] **Step 5: Implement streaming decode and three-beam synthesis**

逐采样先写入全部 48 路历史，再读取当前及历史样本执行四点插值；每路阵元求和除以 48。CH0 同步复制为 `firstElement`。任何非有限角度先夹紧到 [-90°, 90°]。

- [ ] **Step 6: Run test_media and verify GREEN**

Run: Task 2 Step 3 命令。

Expected: `test_media` PASS。

### Task 3: 接入降噪 Worker

**Files:**
- Modify: `src/media/noisereductionworker.h`
- Modify: `src/media/noisereductionworker.cpp`
- Test: `tests/media/test_media.cpp`

**Interfaces:**
- Consumes: `FractionalDelayBeamformer::configure/process/reset` and `NoiseReductionSettings::inputMode`.
- Produces: 现有 `NoiseReductionInputFrame` 逻辑四路，后续接口不变。

- [ ] **Step 1: Write worker mode tests**

配置直接处理模式并提交合成平面波，使用 `plotFrameReady`/`recordingFrameReady` 验证 A 与 B 来自软件波束输出；配置兼容模式后验证现有 CH1–CH3 固定样本行为未变。再验证仅修改角度不会丢失跨帧历史，切换模式会清空启动状态。

- [ ] **Step 2: Run test_media and verify RED**

Run: Task 2 Step 3 命令。

Expected: 直接处理断言失败，Worker 仍读取 CH1–CH3。

- [ ] **Step 3: Select decoder by input mode**

```cpp
NoiseReductionInputFrame input;
if (m_settings.inputMode
    == NoiseReductionInputMode::Raw48SoftwareBeamforming) {
    const auto beams = m_beamformer.process(payload);
    if (!beams.valid) {
        emit warningRaised(beams.error);
        emit frameConsumed();
        return;
    }
    input.channels = {beams.firstElement, beams.enhancedA,
                      beams.suppressionB1, beams.suppressionB2};
    input.valid = true;
} else {
    input = m_decoder.decode(payload);
}
```

`configure()` 中采样率或 `inputMode` 改变时调用 `resetProcessing()` 和 `m_beamformer.reset()`；连续方向改变时仅调用 `m_beamformer.configure(...)`。

- [ ] **Step 4: Run test_media and verify GREEN**

Run: Task 2 Step 3 命令。

Expected: `test_media` PASS，兼容模式回归不变。

### Task 4: 双投影视频控件

**Files:**
- Modify: `src/ui/noisereductionvideowidget.h`
- Modify: `src/ui/noisereductionvideowidget.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Produces: `void setProjectionMode(NoiseDirectionProjectionMode mode);`
- Produces: `enum class NoiseDirectionProjectionMode { AngularWideAngle, CalibratedPinhole };`

- [ ] **Step 1: Write projection symmetry tests**

默认模式断言 180° 水平视场的 -90°/0°/+90° 分别位于左/中/右；针孔模式设置定位相机参数后，点击由 `unprojectVideoPoint()` 得到的方向并断言标记经 `projectDirection()` 回到同一点。

- [ ] **Step 2: Run test_dashboard and verify RED**

Run: `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_dashboard && ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_dashboard --output-on-failure`

Expected: 编译失败，投影模式接口不存在。

- [ ] **Step 3: Implement paired projection selection**

```cpp
const ProjectionResult projected =
    m_projectionMode == NoiseDirectionProjectionMode::CalibratedPinhole
        ? projectDirection(direction, m_camera)
        : projectDirectionAngular(direction, m_camera);

const ScanDirection direction =
    m_projectionMode == NoiseDirectionProjectionMode::CalibratedPinhole
        ? unprojectVideoPoint(normalizedPoint, m_camera)
        : unprojectVideoPointAngular(normalizedPoint, m_camera);
```

将反投影封装在控件公开方法或信号数据中，确保页面不会再固定调用某一种模型。

- [ ] **Step 4: Run test_dashboard and verify GREEN**

Run: Task 4 Step 2 命令。

Expected: `test_dashboard` PASS。

### Task 5: 降噪页面输入模式与校准复用

**Files:**
- Modify: `src/ui/noisereductionpage.h`
- Modify: `src/ui/noisereductionpage.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Consumes: settings input mode/reuse flag and video projection mode.
- Produces: `noiseInputModeCombo`, `noiseReuseLocalizationCalibrationCheck`, and unchanged `configureWorkerRequested(settings, sampleRate)`.

- [ ] **Step 1: Write UI behavior tests**

按 objectName 查找控件，断言默认 FPGA 模式、复用框禁用；切到直接模式后复用框启用；勾选后保存设置并确认 Worker 配置携带连续的 17.3°/20.7°；用 `QSignalSpy` 确认切换输入模式没有触发 `controlSendRequested`。

```cpp
QCOMPARE(controlSpy.count(), 0);
QCOMPARE(configured.inputMode,
         NoiseReductionInputMode::Raw48SoftwareBeamforming);
QCOMPARE(configured.enhancedA.azimuthDegrees, 17.3);
```

- [ ] **Step 2: Run test_dashboard and verify RED**

Run: Task 4 Step 2 命令。

Expected: 找不到新控件或设置未更新。

- [ ] **Step 3: Add input controls and settings synchronization**

在方向页签顶部加入处理来源下拉框、校准复选框和设备前置条件说明。`settingsFromUi()`、`restoreSettings()` 和可用性刷新覆盖新字段。切换模式只调用 `applySettingsFromUi()`，不调用 `requestControlSend()`。

- [ ] **Step 4: Apply the latest localization camera**

当直接处理且复用校准时读取 `m_settingsStore->liveLocalization().camera`，同时设置普通与全屏视频；否则使用 `m_settings.camera`。页面 `setActive(true)`、切换复选框及刷新方向视图时执行同步。

- [ ] **Step 5: Keep angle control semantics explicit**

角度框保持 `double`、0.1° 步进；Worker 配置始终收到实际角度。接口码标签继续展示量化值；直接处理模式下发送方向按钮禁用并提示软件方向不需要发送，兼容模式恢复原行为。

- [ ] **Step 6: Run test_dashboard and verify GREEN**

Run: Task 4 Step 2 命令。

Expected: `test_dashboard` PASS，原有拖动和全屏测试也通过。

### Task 6: 文档与完整验证

**Files:**
- Modify: `doc/Readme.md`
- Inspect: all changed files

- [x] **Step 1: Update maintained behavior documentation**

在降噪章节说明两种输入模式、CH0–CH47 设备前置条件、三路软件波束形成、连续实际角度、校准复用触发条件、不发送 FPGA 命令；在变更记录追加 2026-08-02 条目。

- [x] **Step 2: Build the full application**

Run: `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug`

Expected: exit 0。

- [x] **Step 3: Run all C++ tests**

Run: `ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure`

Expected: 6/6 PASS。

- [x] **Step 4: Run Python tests and mock-device self-test**

Run: `python -m unittest discover -s tests/tools -p "test_*.py" -v`

Expected: all PASS。

Run: `python tools/mock_device_server.py --self-test`

Expected: self-test PASS；控制协议行为未变化。

- [x] **Step 5: Inspect diff quality**

Run: `git diff --check`

Expected: no output, exit 0。

Run: `git status --short`

Expected: 只包含本功能源代码、测试、设计、计划和 `doc/Readme.md`，不包含生成文件。
