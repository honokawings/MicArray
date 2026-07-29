# 实时降噪处理页面实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增紧邻音视频融合页的实时降噪页面，以增益前原始 CH0–CH3 音频完成单点/两点频域维纳抑制、监听、双波形/频谱对比、视频点击方向控制和独立全屏显示。

**Architecture:** `MainWindow` 在原始帧进入 `AudioProcessor` 前将其有界旁路给独立 `NoiseReductionThread`；纯算法类负责解码、方向量化和 STFT 维纳计算，页面只处理设置、绘图、视频点击和控制请求。全屏窗口使用独立镜像控件，不改变主窗口状态，也不迁移普通页面控件。

**Tech Stack:** C++20、Qt 6 Core/Gui/Widgets/Multimedia/Test、现有 `Radix2Fft`、Qt Test、CMake。

## Global Constraints

- 不创建分支，不提交代码。
- 保留并基于当前工作区已有改动开发，不回滚或覆盖无关内容。
- 降噪输入必须来自移位增益和 AGC 之前的原始 48 通道 payload。
- CH0、CH1、CH2、CH3 分别表示第一阵元、增强 A、抑制 B1、抑制 B2；CH4–CH47 不参与降噪。
- 方位接口 0/2/4 为 `-4…4`，俯仰接口 1/3/5 为 `-3…3`。
- 实际角度到接口值使用已确认的线性量化、四舍五入和夹紧规则。
- 进入降噪页自动关闭原通道播放，离开后不自动恢复。
- 降噪旁路过载不得阻塞现有播放、录音、定位、视频或网络链路。
- 每个生产行为变更必须先有失败测试。
- 更新 `doc/Readme.md` 及其变更记录。

---

### Task 1: 降噪领域类型和方向控制映射

**Files:**
- Create: `src/media/noisereductiontypes.h`
- Create: `src/control/directioncontrolmapper.h`
- Create: `src/control/directioncontrolmapper.cpp`
- Modify: `tests/media/test_media.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `NoiseReductionMode`、`NoiseSignalSource`、`DirectionTarget`、`DirectionControlMapping`、`NoiseReductionSettings`。
- Produces: `DirectionControlMapper::azimuthCode()`、`elevationCode()`、`azimuthDegrees()`、`elevationDegrees()`、`controlValues()`。

- [x] **Step 1: 写方向映射失败测试**

在 `tests/media/test_media.cpp` 增加：

```cpp
void MediaTest::mapsNoiseReductionDirectionsToSixControlInterfaces()
{
    DirectionControlMapping mapping;
    QCOMPARE(DirectionControlMapper::azimuthCode(-90.0, mapping), qint16(-4));
    QCOMPARE(DirectionControlMapper::azimuthCode(0.0, mapping), qint16(0));
    QCOMPARE(DirectionControlMapper::azimuthCode(90.0, mapping), qint16(4));
    QCOMPARE(DirectionControlMapper::elevationCode(-45.0, mapping), qint16(-3));
    QCOMPARE(DirectionControlMapper::elevationCode(45.0, mapping), qint16(3));

    DirectionTarget a {1, -1};
    DirectionTarget b1 {-2, 2};
    DirectionTarget b2 {4, -3};
    const ControlValues values =
        DirectionControlMapper::controlValues(a, b1, b2);
    QCOMPARE(values.channelValue(0), qint16(1));
    QCOMPARE(values.channelValue(1), qint16(-1));
    QCOMPARE(values.channelValue(2), qint16(-2));
    QCOMPARE(values.channelValue(3), qint16(2));
    QCOMPARE(values.channelValue(4), qint16(4));
    QCOMPARE(values.channelValue(5), qint16(-3));
}
```

另加范围夹紧、半步四舍五入、非法映射拒绝测试。

- [x] **Step 2: 运行测试确认 RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media
```

Expected: 编译失败，缺少 `DirectionControlMapping` 和 `DirectionControlMapper`。

- [x] **Step 3: 实现最小领域类型和映射**

核心接口：

```cpp
enum class NoiseReductionMode { SinglePoint, TwoPoint };
enum class NoiseSignalSource {
    FirstElement,
    EnhancedA,
    SuppressionB1,
    SuppressionB2,
    Denoised,
};

struct DirectionTarget {
    qint16 azimuthCode = 0;
    qint16 elevationCode = 0;
    bool operator==(const DirectionTarget &) const = default;
};

struct DirectionControlMapping {
    double azimuthMinimumDegrees = -90.0;
    double azimuthMaximumDegrees = 90.0;
    double elevationMinimumDegrees = -45.0;
    double elevationMaximumDegrees = 45.0;
    bool isValid() const;
};
```

`DirectionControlMapper` 使用显式 `std::clamp` 和 `std::lround`，接口值反向换算使用同一线性关系。

- [x] **Step 4: 运行媒体测试确认 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_media --output-on-failure
```

Expected: `test_media` 通过。

- [x] **Step 5: 检查本任务差异**

Run:

```powershell
git diff --check -- CMakeLists.txt src/media/noisereductiontypes.h src/control/directioncontrolmapper.h src/control/directioncontrolmapper.cpp tests/media/test_media.cpp
```

Expected: 退出码 0。

---

### Task 2: 原始 CH0–CH3 解码和频域维纳算法

**Files:**
- Create: `src/media/noisereductionframedecoder.h`
- Create: `src/media/noisereductionframedecoder.cpp`
- Create: `src/media/wienernoisereducer.h`
- Create: `src/media/wienernoisereducer.cpp`
- Modify: `tests/media/test_media.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `NoiseReductionInputFrame NoiseReductionFrameDecoder::decode(const QByteArray &) const`。
- Produces: `QVector<float> WienerNoiseReducer::process(const QVector<float> &enhancedA, const QVector<float> &suppressionB1, const QVector<float> &suppressionB2)`。
- Consumes: `NoiseReductionMode`。

- [x] **Step 1: 写 CH0–CH3 解码失败测试**

构造完整 38400 字节 payload，每个通道写不同的 24-bit 量级常数。断言：

```cpp
const NoiseReductionInputFrame decoded = decoder.decode(payload);
QVERIFY(decoded.valid);
QCOMPARE(decoded.channels[0].at(0), 100.0f / 32768.0f);
QCOMPARE(decoded.channels[3].at(0), 400.0f / 32768.0f);
```

分别改变 CH4–CH47 并断言四个输出完全相同；错误长度必须返回 `valid=false`。

- [x] **Step 2: 写维纳算法失败测试**

生成 48 kHz 合成信号：

- 目标 A 含 1000 Hz 有效信号；
- B1 含 3000 Hz 干扰，A 中混入同相 3000 Hz；
- B2 含 5000 Hz 干扰，A 中混入同相 5000 Hz。

断言单点模式降低 3000 Hz，双点模式同时降低 3000 Hz 和 5000 Hz，静音和相同参考不产生非有限值。

- [x] **Step 3: 运行测试确认 RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media
```

Expected: 编译失败，缺少解码器和维纳算法类型。

- [x] **Step 4: 实现解码器**

`NoiseReductionFrameDecoder`：

```cpp
struct NoiseReductionInputFrame {
    bool valid = false;
    QString error;
    std::array<QVector<float>, 4> channels;
};
```

只遍历每个采样点的前四个通道，读取 `qFromLittleEndian<qint32>`，右移 8 位并除以 32768.0，最后夹紧到 `[-1, 1]`。

- [x] **Step 5: 实现 STFT 维纳算法**

实现 1024 点 FFT、512 点步长、平方根 Hann 和归一化重叠相加。单点模式计算一个参考权重；两点模式逐频点求解正则化 2×2 复数矩阵。模式改变调用 `reset()`。

算法接口包含：

```cpp
void setMode(NoiseReductionMode mode);
void reset();
QVector<float> process(const QVector<float> &enhancedA,
                       const QVector<float> &suppressionB1,
                       const QVector<float> &suppressionB2);
```

- [x] **Step 6: 运行媒体测试确认 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_media --output-on-failure
```

Expected: 解码、单点、双点和数值稳定性测试通过。

- [x] **Step 7: 检查本任务差异**

Run:

```powershell
git diff --check -- CMakeLists.txt src/media/noisereductionframedecoder.h src/media/noisereductionframedecoder.cpp src/media/wienernoisereducer.h src/media/wienernoisereducer.cpp tests/media/test_media.cpp
```

Expected: 退出码 0。

---

### Task 3: 独立降噪工作线程、监听和绘图数据

**Files:**
- Create: `src/media/noisereductionworker.h`
- Create: `src/media/noisereductionworker.cpp`
- Modify: `src/media/noisereductiontypes.h`
- Modify: `tests/media/test_media.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `configure(const NoiseReductionSettings &, int sampleRate)`。
- Consumes: `processRawFrame(const QByteArray &, quint32, quint32)`。
- Produces: `frameConsumed()`、`plotFrameReady(const NoiseReductionPlotFrame &)`、`warningRaised(const QString &)`。

- [x] **Step 1: 写工作对象失败测试**

覆盖：

- 功能关闭时消费帧但不输出绘图；
- 开启后从真实 payload 产生五个信号源；
- 单点/两点模式切换；
- 选中两个绘图源后返回对应标签和有限功率谱；
- 监听增益换算及 PCM16 饱和辅助函数；
- shutdown 后不再处理。

- [x] **Step 2: 运行测试确认 RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media
```

Expected: 编译失败，缺少 `NoiseReductionWorker`。

- [x] **Step 3: 实现工作对象**

工作对象保存五个滚动样本缓存，按不高于 20 FPS 生成：

```cpp
struct NoiseReductionPlotSeries {
    NoiseSignalSource source = NoiseSignalSource::EnhancedA;
    QVector<float> waveform;
    QVector<float> spectrumDbfs;
};

struct NoiseReductionPlotFrame {
    NoiseReductionPlotSeries first;
    NoiseReductionPlotSeries second;
    int sampleRate = 48000;
    qint64 timestampMicroseconds = 0;
};
```

功率谱使用最近 2048 点 Hann 窗和现有 `Radix2Fft`，输出 1025 个 `-120…0 dBFS` 值。

`QAudioSink` 只在工作线程槽函数中创建和销毁；测试通过注入或关闭监听避免依赖真实音频设备。

- [x] **Step 4: 运行媒体测试确认 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_media --output-on-failure
```

Expected: `test_media` 通过。

- [x] **Step 5: 检查本任务差异**

Run:

```powershell
git diff --check -- CMakeLists.txt src/media/noisereductiontypes.h src/media/noisereductionworker.h src/media/noisereductionworker.cpp tests/media/test_media.cpp
```

Expected: 退出码 0。

---

### Task 4: 降噪设置持久化

**Files:**
- Modify: `src/app/appsettings.h`
- Modify: `src/app/appsettings.cpp`
- Modify: `tests/app/test_app.cpp`

**Interfaces:**
- Produces: `NoiseReductionSettings AppSettings::noiseReduction() const`。
- Produces: `void AppSettings::saveNoiseReduction(const NoiseReductionSettings &)`。

- [x] **Step 1: 写设置失败测试**

创建临时 INI，保存非默认设置并重新加载，断言模式、五种源、增益、三组方向、映射和相机参数全部一致。再写入非法枚举、非有限数、越界接口值和非法视角，断言逐字段回退。

- [x] **Step 2: 运行测试确认 RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app
```

Expected: 编译失败，缺少设置接口。

- [x] **Step 3: 实现读取、校验和保存**

使用稳定字符串保存枚举：

```text
singlePoint / twoPoint
firstElement / enhancedA / suppressionB1 / suppressionB2 / denoised
```

接口值分别夹紧到 `-4…4` 和 `-3…3`。非法角度映射和相机字段逐项回退默认。保存后调用 `sync()`。

- [x] **Step 4: 运行设置测试确认 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_app --output-on-failure
```

Expected: `test_app` 通过。

- [x] **Step 5: 检查本任务差异**

Run:

```powershell
git diff --check -- src/app/appsettings.h src/app/appsettings.cpp tests/app/test_app.cpp
```

Expected: 退出码 0。

---

### Task 5: 视频标记、波形和功率谱控件

**Files:**
- Create: `src/ui/powerspectrumwidget.h`
- Create: `src/ui/powerspectrumwidget.cpp`
- Create: `src/ui/noisereductionvideowidget.h`
- Create: `src/ui/noisereductionvideowidget.cpp`
- Modify: `src/ui/audiowaveformwidget.h`
- Modify: `src/ui/audiowaveformwidget.cpp`
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `PowerSpectrumWidget::setSpectrum(...)`。
- Produces: `NoiseReductionVideoWidget::videoPointClicked(const QPointF &)`。
- Produces: `NoiseReductionVideoWidget::setDirectionTargets(...)`。
- Produces: `AudioWaveformWidget::setTitle(const QString &)` 和浮点波形设置接口。

- [x] **Step 1: 写控件失败测试**

断言：

- 功率谱控件保存源名称、采样率和频点数；
- 视频黑边点击不发信号，有效区域点击发归一化坐标；
- 标记隐藏后状态正确；
- 波形标题和浮点样本可更新。

- [x] **Step 2: 运行 UI 测试确认 RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
```

Expected: 编译失败，缺少新控件接口。

- [x] **Step 3: 实现绘图和视频控件**

`PowerSpectrumWidget` 使用 `QPainterPath` 绘制 dBFS 网格和曲线；`NoiseReductionVideoWidget` 使用 `projectDirection()` 绘制三个量化方向标记，并在有效视频区域单击时发信号。

- [x] **Step 4: 运行 UI 测试确认 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

Expected: `test_ui` 通过。

- [x] **Step 5: 检查本任务差异**

Run:

```powershell
git diff --check -- CMakeLists.txt src/ui/powerspectrumwidget.h src/ui/powerspectrumwidget.cpp src/ui/noisereductionvideowidget.h src/ui/noisereductionvideowidget.cpp src/ui/audiowaveformwidget.h src/ui/audiowaveformwidget.cpp tests/ui/test_dashboard.cpp
```

Expected: 退出码 0。

---

### Task 6: 降噪页面、全屏和主窗口集成

**Files:**
- Create: `src/ui/noisereductionpage.h`
- Create: `src/ui/noisereductionpage.cpp`
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `tests/media/test_media.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `setActive(bool)`、`setConnected(bool)`、`setControlLocked(bool)`。
- Consumes: `submitRawAudioFrame(...)` 和 `setVideoFrame(...)`。
- Produces: `controlSendRequested(const ControlValues &)`。
- Produces: `pendingAudioFrames()` 和 `droppedAudioFrames()` 可观察状态。

- [x] **Step 1: 写页面结构和默认值失败测试**

断言新页紧邻音视频融合，默认源为增强 A/降噪输出，包含启用、模式、监听、目标、映射、相机、两个波形、两个频谱和全屏按钮。

- [x] **Step 2: 写视频点击和控制失败测试**

设置已知相机及映射，选择 B1，点击视频右侧，断言：

- 仅 B1 值改变；
- 输出仍包含 A、B1、B2 全部 6 路；
- 未连接和控制锁定不会报告发送成功；
- 隐藏标记状态持久。

- [x] **Step 3: 写原始旁路和页面切换失败测试**

使用 `MainWindow` 可观察属性或页面计数断言：

- 原始帧在增益配置非零时仍以原值送入降噪页；
- 待处理上限为 32；
- 进入降噪页关闭原通道播放；
- 离开后不恢复。

- [x] **Step 4: 写独立全屏失败测试**

断言：

- `noiseReductionPresentationWindow` 是独立顶层全屏窗口；
- 主窗口状态和几何不变；
- 全屏内容只有视频区域和两个功率谱；
- Esc、视频双击和 close 均退出；
- 普通页面控件的父对象不改变。

- [x] **Step 5: 运行 UI 测试确认 RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
```

Expected: 编译失败或断言失败，因为页面尚不存在。

- [x] **Step 6: 实现页面和线程生命周期**

构造时创建工作对象并移入 `NoiseReductionThread`。页面使用 32 帧有界待处理计数；设置变化通过 queued signal 发往 worker。析构时阻塞 shutdown 后退出线程。

- [x] **Step 7: 实现主窗口集成**

`MainWindow::enqueueAudioFrame()` 顺序固定为：

```cpp
m_noiseReductionPage->submitRawAudioFrame(
    frame.payload, frame.timestampSeconds, frame.timestampMicros);
// 随后才进入现有 AudioProcessor 队列
```

控制发送由 `MainWindow` 统一调用 `DeviceClient::sendControl()`，成功后同步现有控制 UI 和撤销状态。

- [x] **Step 8: 实现全屏镜像窗口**

创建独立无边框窗口及专用视频/频谱控件。普通和全屏控件读取同一缓存，不 reparent，不改变主窗口状态。

- [x] **Step 9: 运行 UI 和媒体测试确认 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui test_media
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_ui|test_media" --output-on-failure
```

Expected: `test_ui` 和 `test_media` 通过。

- [x] **Step 10: 检查本任务差异**

Run:

```powershell
git diff --check -- CMakeLists.txt src/ui/noisereductionpage.h src/ui/noisereductionpage.cpp src/ui/mainwindow.h src/ui/mainwindow.cpp tests/ui/test_dashboard.cpp tests/media/test_media.cpp
```

Expected: 退出码 0。

---

### Task 7: 文档、完整构建和回归验证

**Files:**
- Modify: `doc/Readme.md`
- Modify: `docs/superpowers/plans/2026-07-28-noise-reduction-page.md`

**Interfaces:**
- Consumes: Task 1–6 的最终行为。
- Produces: 可维护的功能说明、线程边界、控制映射和验证记录。

- [x] **Step 1: 更新维护文档**

在 `doc/Readme.md` 增加：

- 降噪页面位置和用途；
- CH0–CH3 语义；
- 增益前原始音频旁路；
- 单点/两点维纳模式；
- 监听和双源波形/频谱；
- 视频点击方向映射；
- 独立全屏内容；
- 线程、队列和错误处理；
- 2026-07-28 变更记录。

- [x] **Step 2: 完整构建**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
```

Expected: 退出码 0。

- [x] **Step 3: 运行全量 C++ 测试**

Run:

```powershell
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
```

Expected: 全部测试通过。

- [x] **Step 4: 运行 Python 工具测试**

Run:

```powershell
python -m unittest discover -s tests/tools -p "test_*.py" -v
```

Expected: 全部测试通过。

- [x] **Step 5: 运行模拟设备自检**

Run:

```powershell
python tools/mock_device_server.py --self-test
```

Expected: 自检通过。

- [x] **Step 6: 检查最终差异**

Run:

```powershell
git diff --check
git status --short --branch
```

Expected: `git diff --check` 退出码 0；状态只包含本功能和此前保留的用户改动。

- [x] **Step 7: 对照设计逐项验收**

逐条核对设计文档第 14 节，确认页面位置、原始旁路、两种模式、五源监听、双源对比、方向控制、持久化、标记隐藏、全屏限制和全部验证结果都有实现或测试证据。
