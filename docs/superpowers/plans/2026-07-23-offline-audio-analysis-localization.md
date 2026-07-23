# Offline Audio Analysis and Sound Localization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增可流式读取录制 WAV、选择单通道播放、连续估计一个主声源方向并把方向投影到自定义照片的“音频分析”页面。

**Architecture:** Qt 控件留在 UI 线程，`AudioAnalysisWorker` 在专用线程中持有 WAV 读取器、离线 `QAudioSink` 和定位器。定位器通过 `ISoundSourceLocator` 与源码级注册表解耦，默认实现使用 GCC-PHAT、鲁棒 TDOA 拟合和正前方半球约束；照片投影由无 UI 数学模块和独立绘制控件完成。

**Tech Stack:** C++20、Qt 6.5+ Core/Gui/Widgets/Multimedia/Test、Qt Test、CMake/Ninja，不新增第三方运行时依赖。

## Global Constraints

- 保持 Qt 最低版本 6.5、C++20、MinGW/Ninja 和现有 Qt Widgets 架构兼容。
- 不回滚或覆盖工作区已有的 `doc/Readme.md` 状态项与 `doc/SessionHandoff-2026-07-22.md`。
- 所有新增类型位于 `MicArray` 命名空间，文件名使用小写，四空格缩进和 Allman 大括号。
- QWidget 只在 UI 线程访问；文件、`QAudioSink` 和定位计算只在分析线程访问。
- 首版只跟踪正前方半球内的一个远场主声源；接口保留多结果能力。
- 阵列固定为 6×8、行列间距 0.05 m、CH0 左上、CH47 右下。
- 进入分析页只关闭实时通道播放，不改变连接、视频、控制、增益或录音。
- 每个行为变更遵循 RED-GREEN-REFACTOR，并更新 `doc/Readme.md` 及变更记录。

---

## File Map

新增分析核心文件：

- `src/analysis/localizationtypes.h`：阵列、算法配置、音频窗、方向结果和相机参数值类型。
- `src/analysis/arraygeometry.h/.cpp`：6×8 阵列坐标和角度/单位向量转换。
- `src/analysis/isoundsourcelocator.h`：稳定的源码级算法接口。
- `src/analysis/soundsourcelocatorregistry.h/.cpp`：算法工厂登记、枚举和按 ID 创建。
- `src/analysis/wavefilereader.h/.cpp`：RIFF/WAVE 解析、随机多通道读取和单通道 PCM16 提取。
- `src/analysis/radix2fft.h/.cpp`：无第三方依赖的复数 radix-2 FFT。
- `src/analysis/gccphatlocator.h/.cpp`：默认 GCC-PHAT/TDOA 定位器。
- `src/analysis/directionprojection.h/.cpp`：针孔相机方向投影和视场外边缘交点。
- `src/analysis/audioanalysisworker.h/.cpp`：分析线程中的加载、播放、seek 和定位调度。

新增 UI 文件：

- `src/ui/directionphotowidget.h/.cpp`：静态照片、准星、文字和视场外箭头绘制。
- `src/ui/audioanalysispage.h/.cpp`：页面布局、分析线程生命周期、设置和 UI 状态。

新增测试：

- `tests/analysis/test_audioanalysis.cpp`：几何、注册表、WAV、投影、GCC-PHAT 和 worker 测试。

修改文件：

- `CMakeLists.txt`：加入新源文件和 `test_analysis`。
- `src/app/appsettings.h/.cpp`、`tests/app/test_app.cpp`：分析配置持久化和校验。
- `src/ui/mainwindow.h/.cpp`、`tests/ui/test_dashboard.cpp`：新增标签和切页隔离行为。
- `doc/Readme.md`：模块、线程、UI、测试和变更记录。

---

### Task 1: 定位值类型、阵列几何与算法注册表

**Files:**
- Create: `src/analysis/localizationtypes.h`
- Create: `src/analysis/arraygeometry.h`
- Create: `src/analysis/arraygeometry.cpp`
- Create: `src/analysis/isoundsourcelocator.h`
- Create: `src/analysis/soundsourcelocatorregistry.h`
- Create: `src/analysis/soundsourcelocatorregistry.cpp`
- Create: `tests/analysis/test_audioanalysis.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `QVector<MicrophonePosition> createDefaultArrayGeometry()`。
- Produces: `QVector3D directionFromAngles(double azimuthDegrees, double elevationDegrees)` 和 `SoundSourceDirection anglesFromDirection(const QVector3D &direction)`。
- Produces: `ISoundSourceLocator::configure()`、`ISoundSourceLocator::locate()`。
- Produces: `SoundSourceLocatorRegistry::registerFactory()`、`descriptors()`、`create()`。

- [ ] **Step 1: 写几何与注册表失败测试**

创建 `tests/analysis/test_audioanalysis.cpp`：

```cpp
#include "analysis/arraygeometry.h"
#include "analysis/soundsourcelocatorregistry.h"

#include <QtTest>

using namespace MicArray;

class FixedLocator final : public ISoundSourceLocator
{
public:
    LocatorDescriptor descriptor() const override
    {
        return {QStringLiteral("fixed"), QStringLiteral("固定结果"), QStringLiteral("测试定位器")};
    }

    bool configure(const LocatorConfiguration &, QString *) override { return true; }
    QVector<SoundSourceDirection> locate(const MultiChannelAudioWindow &) override
    {
        return {{10.0, 5.0, 1.0, 0.0, 1, 0}};
    }
};

class AudioAnalysisTest : public QObject
{
    Q_OBJECT

private slots:
    void createsCenteredSixByEightGeometry();
    void convertsAnglesToDirectionRoundTrip();
    void registersAndCreatesLocatorByStableId();
};

void AudioAnalysisTest::createsCenteredSixByEightGeometry()
{
    const auto microphones = createDefaultArrayGeometry();
    QCOMPARE(microphones.size(), 48);
    QCOMPARE(microphones.at(0), MicrophonePosition({-0.175, 0.125, 0.0}));
    QCOMPARE(microphones.at(7), MicrophonePosition({0.175, 0.125, 0.0}));
    QCOMPARE(microphones.at(40), MicrophonePosition({-0.175, -0.125, 0.0}));
    QCOMPARE(microphones.at(47), MicrophonePosition({0.175, -0.125, 0.0}));
}

void AudioAnalysisTest::convertsAnglesToDirectionRoundTrip()
{
    const QVector3D direction = directionFromAngles(30.0, 20.0);
    const SoundSourceDirection result = anglesFromDirection(direction);
    QVERIFY(std::abs(result.azimuthDegrees - 30.0) < 1.0e-6);
    QVERIFY(std::abs(result.elevationDegrees - 20.0) < 1.0e-6);
    QVERIFY(direction.z() > 0.0f);
}

void AudioAnalysisTest::registersAndCreatesLocatorByStableId()
{
    SoundSourceLocatorRegistry registry;
    QString error;
    QVERIFY(registry.registerFactory([] { return std::make_unique<FixedLocator>(); }, &error));
    QVERIFY(!registry.registerFactory([] { return std::make_unique<FixedLocator>(); }, &error));
    QCOMPARE(registry.descriptors().size(), 1);
    QVERIFY(registry.create(QStringLiteral("fixed")) != nullptr);
    QVERIFY(registry.create(QStringLiteral("missing")) == nullptr);
}

QTEST_MAIN(AudioAnalysisTest)
#include "test_audioanalysis.moc"
```

在 `CMakeLists.txt` 新增 `test_analysis` 目标，并且只把本任务已经创建的分析源文件加入 `MICARRAY_SOURCES/HEADERS`；后续任务各自在文件创建后再追加，不能提前引用不存在的文件。

- [ ] **Step 2: 运行测试确认 RED**

运行：

```powershell
cmake -S . -B build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
```

预期：编译失败，提示 `analysis/arraygeometry.h` 或相关类型不存在。

- [ ] **Step 3: 实现公共类型与接口**

`localizationtypes.h` 定义并注册以下类型：

```cpp
struct MicrophonePosition
{
    double xMeters = 0.0;
    double yMeters = 0.0;
    double zMeters = 0.0;
    bool operator==(const MicrophonePosition &) const = default;
};

struct LocatorDescriptor
{
    QString id;
    QString displayName;
    QString description;
};

struct LocatorConfiguration
{
    QVector<MicrophonePosition> microphones;
    int sampleRate = 48000;
    int windowSamples = 2048;
    double soundSpeedMetersPerSecond = 343.0;
    double confidenceThreshold = 0.45;
    double smoothingFactor = 0.35;
};

struct MultiChannelAudioWindow
{
    QVector<QVector<float>> channels;
    qint64 centerSample = 0;
    int sampleRate = 48000;
};

struct SoundSourceDirection
{
    double azimuthDegrees = 0.0;
    double elevationDegrees = 0.0;
    double confidence = 0.0;
    double residualSamples = 0.0;
    int validPairCount = 0;
    qint64 centerSample = 0;
};

struct CameraViewSettings
{
    double centerAzimuthDegrees = 0.0;
    double centerElevationDegrees = 0.0;
    double horizontalFovDegrees = 90.0;
    double verticalFovDegrees = 60.0;
    double rollDegrees = 0.0;
    bool isValid() const;
    bool operator==(const CameraViewSettings &) const = default;
};
```

同时为 `SoundSourceDirection`、`QVector<SoundSourceDirection>` 等跨线程信号参数使用 `Q_DECLARE_METATYPE`；Task 2 添加 `WaveFileInfo` 后也注册该类型，页面创建 worker 前调用 `qRegisterMetaType`。

`arraygeometry.cpp` 使用 `row=channel/8`、`column=channel%8` 和设计规格中的公式创建几何；角度转换使用：

```cpp
const double azimuth = qDegreesToRadians(azimuthDegrees);
const double elevation = qDegreesToRadians(elevationDegrees);
return QVector3D(std::sin(azimuth) * std::cos(elevation),
                 std::sin(elevation),
                 std::cos(azimuth) * std::cos(elevation));
```

`isoundsourcelocator.h` 实现规格中的纯虚接口。`SoundSourceLocatorRegistry` 在注册时创建一次实例读取 descriptor，拒绝空工厂、空 ID、空名称和重复 ID，并把工厂保存到 `QHash<QString, LocatorFactory>`；`descriptors()` 按显示名称稳定排序。

- [ ] **Step 4: 运行测试确认 GREEN**

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_analysis --output-on-failure
```

预期：`test_analysis` 通过。

- [ ] **Step 5: 提交**

```powershell
git add CMakeLists.txt src/analysis/localizationtypes.h src/analysis/arraygeometry.h src/analysis/arraygeometry.cpp src/analysis/isoundsourcelocator.h src/analysis/soundsourcelocatorregistry.h src/analysis/soundsourcelocatorregistry.cpp tests/analysis/test_audioanalysis.cpp
git commit -m "建立声源定位接口与阵列几何"
```

---

### Task 2: 流式 WAV 读取与单通道提取

**Files:**
- Create: `src/analysis/wavefilereader.h`
- Create: `src/analysis/wavefilereader.cpp`
- Modify: `tests/analysis/test_audioanalysis.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `bool WaveFileReader::open(const QString &, QString *)`。
- Produces: `WaveFileInfo WaveFileReader::info() const`。
- Produces: `QVector<QVector<float>> readChannels(qint64 startSample, int sampleCount, QString *)`。
- Produces: `QByteArray readMonoPcm16(int channel, qint64 startSample, int sampleCount, QString *)`。

- [ ] **Step 1: 写 WAV 读取失败测试**

在测试类增加 `readsRecordedExtensibleWave()`、`rejectsTruncatedWaveFrame()`、`skipsUnknownPaddedChunks()`、`readsStandardIntegerPcm()` 和 `rejectsInvalidWaveMetadata()`。使用现有 `MultiChannelWavWriter` 生成真实格式：

```cpp
void AudioAnalysisTest::readsRecordedExtensibleWave()
{
    QTemporaryDir directory;
    RecordingSettings settings;
    settings.outputDirectory = directory.path();
    settings.baseName = QStringLiteral("analysis");
    settings.sampleRate = 48000;
    for (int channel = 0; channel < 48; ++channel) {
        settings.channels.append(channel);
    }

    QByteArray payload(AudioFrameDecoder::FramePayloadBytes, char(0));
    for (int sample = 0; sample < AudioFrameDecoder::SamplesPerFrame; ++sample) {
        for (int channel = 0; channel < 48; ++channel) {
            qToLittleEndian<qint32>((channel + 1) << 8,
                reinterpret_cast<uchar *>(payload.data() + (sample * 48 + channel) * 4));
        }
    }
    MultiChannelWavWriter writer;
    QString error;
    QVERIFY(writer.open(settings, &error));
    QVERIFY(writer.appendFrame(payload, &error));
    QVERIFY(writer.close(QStringLiteral("test"), &error));

    WaveFileReader reader;
    QVERIFY2(reader.open(directory.filePath(QStringLiteral("analysis.wav")), &error), qPrintable(error));
    QCOMPARE(reader.info().channelCount, 48);
    QCOMPARE(reader.info().sampleRate, 48000);
    QCOMPARE(reader.info().containerBits, 32);
    QCOMPARE(reader.info().validBits, 24);
    const QByteArray pcm = reader.readMonoPcm16(17, 0, 4, &error);
    QCOMPARE(pcm.size(), 8);
    QCOMPARE(qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(pcm.constData())), qint16(18));
    const auto channels = reader.readChannels(0, 2, &error);
    QCOMPARE(channels.size(), 48);
    QCOMPARE(channels.at(47).size(), 2);
}
```

截断测试复制合法文件、删除最后 1 字节，预期 `open()` 返回 false 且错误包含“完整采样帧”。其余失败测试在实现前手工构造字节流，覆盖未知 `JUNK` chunk、奇数填充、标准 PCM16/24/32、非 PCM extensible GUID、非法 blockAlign 和越界通道。

- [ ] **Step 2: 运行测试确认 RED**

运行 `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis`。

预期：编译失败，提示 `WaveFileReader` 未定义。

- [ ] **Step 3: 实现 RIFF 解析与读取**

`WaveFileInfo` 包含路径、通道数、采样率、容器位数、有效位数、blockAlign、总样本帧和时长。`open()` 必须：

1. 验证 `RIFF` 和 `WAVE`。
2. 遍历 chunk，使用小端读取长度并为奇数长度跳过 1 字节填充。
3. 解析 PCM 或 extensible PCM GUID `01000000-0000-0010-8000-00aa00389b71`。
4. 验证通道 `1..48`、采样率 `8000..192000`、容器 `16/24/32`、有效位和 `blockAlign==channels*containerBytes`。
5. 保存 `data` 起点和长度，并拒绝 `dataLength % blockAlign != 0`。

样本解码按容器读取有符号小端整数并除以 `2^(validBits-1)` 得到 `[-1,1]`；当有效位小于容器位时沿用本项目的低位对齐数据约定，先符号扩展容器再按有效位归一化。PCM16 输出使用 `std::lround(value*32767.0)` 并饱和。

边界读取允许文件首尾补零，但无效通道、负 sampleCount、文件未打开和实际 I/O 短读必须返回错误。

- [ ] **Step 4: 运行 chunk 顺序与格式边界测试并保持 GREEN**

运行 Step 1 已写好的未知 chunk、填充、标准 PCM 和非法元数据测试：

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_analysis --output-on-failure
```

预期：全部通过。

- [ ] **Step 5: 提交**

```powershell
git add CMakeLists.txt src/analysis/wavefilereader.h src/analysis/wavefilereader.cpp tests/analysis/test_audioanalysis.cpp
git commit -m "支持流式读取多通道 WAV"
```

---

### Task 3: 照片方向投影数学模块

**Files:**
- Create: `src/analysis/directionprojection.h`
- Create: `src/analysis/directionprojection.cpp`
- Modify: `tests/analysis/test_audioanalysis.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ProjectionResult projectDirection(const SoundSourceDirection &, const CameraViewSettings &)`。
- `ProjectionResult` 包含 `normalizedPoint`、`edgePoint`、`inFront`、`inView`。

- [ ] **Step 1: 写中心、边缘和视场外失败测试**

```cpp
void AudioAnalysisTest::projectsDirectionIntoConfiguredCameraView()
{
    CameraViewSettings camera;
    ProjectionResult center = projectDirection({0.0, 0.0, 1.0}, camera);
    QVERIFY(center.inFront);
    QVERIFY(center.inView);
    QVERIFY(QLineF(center.normalizedPoint, QPointF(0.5, 0.5)).length() < 1.0e-6);

    ProjectionResult rightEdge = projectDirection({45.0, 0.0, 1.0}, camera);
    QVERIFY(rightEdge.inView);
    QVERIFY(std::abs(rightEdge.normalizedPoint.x() - 1.0) < 1.0e-6);

    ProjectionResult outside = projectDirection({70.0, 0.0, 1.0}, camera);
    QVERIFY(!outside.inView);
    QVERIFY(outside.edgePoint.x() <= 1.0 && outside.edgePoint.x() >= 0.0);
    QVERIFY(outside.edgePoint.y() <= 1.0 && outside.edgePoint.y() >= 0.0);
}
```

增加相机中心方位、俯仰、滚转和背后方向测试。

- [ ] **Step 2: 运行测试确认 RED**

预期：编译失败，提示 `directionprojection.h` 不存在。

- [ ] **Step 3: 实现针孔投影**

`CameraViewSettings` 默认 `{0,0,90,60,0}` 并提供 `isValid()`。实现时：

1. 用 `directionFromAngles()` 得到阵列坐标单位向量。
2. 按相机中心 yaw、pitch、roll 的逆旋转转换为 `(xc,yc,zc)`。
3. `zc<=0` 标记为背后。
4. 使用规格公式计算 `u/v`。
5. 当视场外时，以 `(0.5,0.5)` 为起点，对方向向量 `(u-0.5,v-0.5)` 求与单位矩形最先相交的正比例点。

所有角度先验证有限值和合法范围；非法相机参数返回 `valid=false`。

- [ ] **Step 4: 运行测试确认 GREEN**

运行 `ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_analysis --output-on-failure`，预期通过。

- [ ] **Step 5: 提交**

```powershell
git add CMakeLists.txt src/analysis/directionprojection.h src/analysis/directionprojection.cpp tests/analysis/test_audioanalysis.cpp
git commit -m "实现声源方向照片投影"
```

---

### Task 4: GCC-PHAT 默认定位器

**Files:**
- Create: `src/analysis/radix2fft.h`
- Create: `src/analysis/radix2fft.cpp`
- Create: `src/analysis/gccphatlocator.h`
- Create: `src/analysis/gccphatlocator.cpp`
- Modify: `src/analysis/soundsourcelocatorregistry.h`
- Modify: `src/analysis/soundsourcelocatorregistry.cpp`
- Modify: `tests/analysis/test_audioanalysis.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `Radix2Fft::transform(QVector<std::complex<double>> &, bool inverse)`。
- Produces: `GccPhatLocator`，稳定 ID `gcc-phat-tdoa`。
- Produces: `void registerBuiltInSoundSourceLocators(SoundSourceLocatorRegistry *)`。

- [ ] **Step 1: 写 FFT 与合成方向失败测试**

先测试 FFT 往返误差小于 `1e-9`。再用固定种子宽带噪声生成平面波窗口：

```cpp
MultiChannelAudioWindow syntheticPlaneWave(double azimuth, double elevation)
{
    constexpr int margin = 128;
    constexpr int samples = 2048;
    QRandomGenerator random(0x5848);
    QVector<float> source(samples + margin * 2 + 2);
    for (float &value : source) {
        value = static_cast<float>(random.generateDouble() * 2.0 - 1.0);
    }
    const QVector3D direction = directionFromAngles(azimuth, elevation);
    MultiChannelAudioWindow window;
    window.sampleRate = 48000;
    window.centerSample = 10000;
    for (const auto &microphone : createDefaultArrayGeometry()) {
        const double delay = (microphone.xMeters * direction.x()
                              + microphone.yMeters * direction.y())
            * window.sampleRate / 343.0;
        QVector<float> channel(samples);
        for (int n = 0; n < samples; ++n) {
            const double index = margin + n - delay;
            const int left = static_cast<int>(std::floor(index));
            const double fraction = index - left;
            channel[n] = static_cast<float>(source.at(left) * (1.0 - fraction)
                                             + source.at(left + 1) * fraction);
        }
        window.channels.append(channel);
    }
    return window;
}
```

测试 `(0,0)`、`(30,15)`、`(-35,-20)` 的方位/俯仰误差均小于 `5°`，置信度大于 `0.45`；静音返回空列表。

- [ ] **Step 2: 运行测试确认 RED**

预期：编译失败，提示 `GccPhatLocator` 和 `Radix2Fft` 不存在。

- [ ] **Step 3: 实现 radix-2 FFT**

实现迭代位反转 Cooley-Tukey：输入长度必须为非零 2 的幂；inverse 使用正角度并在结束除以 N。非法长度返回 false，不修改调用者可观测的有效结果。测试正变换后逆变换恢复原复数序列。

- [ ] **Step 4: 实现 GCC-PHAT、TDOA 和鲁棒拟合**

`configure()` 验证 48 个有限坐标、采样率、2 的幂窗长、声速、门限和平滑因子。`locate()`：

1. 去均值、Hann 窗、能量门限。
2. 对每通道做 `2*windowSamples` 点 FFT。
3. 构造覆盖每个通道的水平/垂直阵元对，使用 1 格和 4 格基线；避免同坐标轴重复对。
4. 对频谱 `Xi*conj(Xj)` 除以幅度加 `1e-12`，逆 FFT 得相关序列。
5. 最大搜索延迟为 `ceil(distance/c*sampleRate)+1`，在正负延迟中搜索并做三点抛物线插值。
6. 峰权重使用 `clamp((peak-secondPeak)/(abs(peak)+1e-12),0,1)` 乘通道能量权重。
7. 建立 `A*[ux,uy]^T=b`，其中 `b=delaySamples*c/sampleRate`。
8. 先加权最小二乘，再进行 3 次 Huber 重加权；解 2×2 正规方程时行列式小于 `1e-12` 返回空。
9. `ux²+uy²` 允许 `1e-3` 容差，轻微超出时归一化，明显超出返回空；取正 `uz`。
10. 置信度综合有效对比例、平均峰权重、内点比例、RMS 残差和能量，任何分量非有限则返回空。
11. 对单位向量执行指数平滑并重新归一化；`configure()` 和空结果连续超过 500 ms 时重置。

把结果转换为方位/俯仰并填充中心样本、残差和有效对数量。所有容器按窗复用以减少每 50 ms 分配。

- [ ] **Step 5: 注册内置算法并验证 GREEN**

`registerBuiltInSoundSourceLocators()` 注册 `GccPhatLocator`。运行：

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_analysis --output-on-failure
```

预期：FFT、三组方向、静音和注册测试全部通过。

- [ ] **Step 6: 提交**

```powershell
git add CMakeLists.txt src/analysis/radix2fft.h src/analysis/radix2fft.cpp src/analysis/gccphatlocator.h src/analysis/gccphatlocator.cpp src/analysis/soundsourcelocatorregistry.h src/analysis/soundsourcelocatorregistry.cpp tests/analysis/test_audioanalysis.cpp
git commit -m "实现 GCC-PHAT 声源定位"
```

---

### Task 5: 音频分析配置持久化

**Files:**
- Modify: `src/app/appsettings.h`
- Modify: `src/app/appsettings.cpp`
- Modify: `tests/app/test_app.cpp`

**Interfaces:**
- Produces: `AudioAnalysisSettings AppSettings::audioAnalysis() const`。
- Produces: `void AppSettings::saveAudioAnalysis(const AudioAnalysisSettings &)`。

- [ ] **Step 1: 写设置失败测试**

```cpp
void AppTest::persistsAndValidatesAudioAnalysisSettings()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    AudioAnalysisSettings expected;
    expected.lastWavePath = QStringLiteral("D:/capture.wav");
    expected.lastPhotoPath = QStringLiteral("D:/view.png");
    expected.outputChannel = 17;
    expected.volume = 0.6;
    expected.locatorId = QStringLiteral("gcc-phat-tdoa");
    expected.windowSamples = 4096;
    expected.updateIntervalMs = 100;
    expected.soundSpeedMetersPerSecond = 344.0;
    expected.camera.centerAzimuthDegrees = 10.0;
    {
        AppSettings settings(path);
        settings.saveAudioAnalysis(expected);
    }
    AppSettings reloaded(path);
    QCOMPARE(reloaded.audioAnalysis(), expected);
}
```

再直接用 `QSettings` 写入 NaN、非法窗长、通道 48、视场 180 和空算法 ID，验证逐字段回退默认值。

- [ ] **Step 2: 运行 `test_app` 确认 RED**

预期：编译失败，提示 `AudioAnalysisSettings` 不存在。

- [ ] **Step 3: 实现设置结构、读取校验和保存归一化**

结构字段与规格一致，提供默认值和 `operator==`。使用 `audioAnalysis/` 组；路径允许为空，通道 `0..47`、音量 `[0,1]`、窗长为 `512..8192` 的 2 的幂、周期 `20..1000 ms`、声速 `300..380 m/s`、平滑和门限 `[0,1]`，相机参数使用 `CameraViewSettings::isValid()`。无效字段只回退自身默认值，不丢弃其他合法字段。

- [ ] **Step 4: 运行测试确认 GREEN**

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_app --output-on-failure
```

- [ ] **Step 5: 提交**

```powershell
git add src/app/appsettings.h src/app/appsettings.cpp tests/app/test_app.cpp
git commit -m "持久化音频分析配置"
```

---

### Task 6: 分析线程、播放状态与定位调度

**Files:**
- Create: `src/analysis/audioanalysisworker.h`
- Create: `src/analysis/audioanalysisworker.cpp`
- Modify: `tests/analysis/test_audioanalysis.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `WaveFileReader`、`SoundSourceLocatorRegistry`、`AudioAnalysisSettings`。
- Produces slots: `loadFile()`、`play()`、`pause()`、`stop()`、`seek()`、`setOutputChannel()`、`setVolume()`、`setLocator()`、`configureAnalysis()`、`analyzeAt()`、`shutdown()`。
- Produces signals: `fileLoaded()`、`stateChanged()`、`positionChanged()`、`waveformReady()`、`directionsReady()`、`analysisStatusChanged()`、`errorOccurred()`、`shutdownFinished()`。

- [ ] **Step 1: 写无声卡依赖的 worker 失败测试**

生成 48 通道测试 WAV，直接在测试线程调用 worker 的加载和 `analyzeAt()`：

```cpp
void AudioAnalysisTest::workerLoadsAndAnalyzesPausedFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = createSyntheticPlaneWaveFile(directory.path(), 25.0, 10.0);
    AudioAnalysisWorker worker;
    QSignalSpy loaded(&worker, &AudioAnalysisWorker::fileLoaded);
    QSignalSpy located(&worker, &AudioAnalysisWorker::directionsReady);
    worker.loadFile(path, 1);
    QCOMPARE(loaded.count(), 1);
    worker.analyzeAt(2048, 2);
    QCOMPARE(located.count(), 1);
    const auto directions = qvariant_cast<QVector<SoundSourceDirection>>(
        located.first().at(1));
    QVERIFY(!directions.isEmpty());
    QVERIFY(std::abs(directions.first().azimuthDegrees - 25.0) < 5.0);
}
```

增加“第二个 load 请求使第一个结果过期”和“少于 48 通道只返回不可定位状态”测试。不要在自动化测试调用 `play()`，避免依赖 CI 声卡。

- [ ] **Step 2: 运行测试确认 RED**

预期：编译失败，提示 worker 不存在。

- [ ] **Step 3: 实现 worker 状态和离线分析**

worker 构造时登记内置定位器并创建默认算法。每个公开 slot 首先断言 `QThread::currentThread()==thread()`。`loadFile(path, requestId)` 只有成功后才替换当前 reader；失败保留旧文件。`analyzeAt(centerSample, requestId)` 读取以中心为基准的窗口，调用当前定位器，用 `QElapsedTimer` 记录耗时并发出带 requestId 的结果。

维护 `m_generation`；加载、seek、算法或关键参数变化时递增。计算结束时 generation 不一致就丢弃结果。分析忙时只保存最新 pending 中心位置，不建立无界队列。

- [ ] **Step 4: 实现 `QAudioSink` 播放供数**

`play()` 在 worker 线程创建单声道 Int16 `QAudioFormat`，检查默认输出设备支持后创建 `QAudioSink`。使用 10 ms `QTimer` 查询 `bytesFree()`，从 `WaveFileReader::readMonoPcm16()` 读取不超过空闲容量的完整样本并写入 sink 的 `QIODevice`。播放位置为 `playStartSample + processedUSecs*sampleRate/1'000'000`。

`pause()` 调用 sink suspend，`play()` 从暂停调用 resume；`stop()` 停止并回到样本 0；`seek()` 停止旧 sink、更新起点并按原播放状态重启。EOF 停止并把位置设置为总样本数。每 50 ms（可配置）以实际位置触发定位，处理落后时覆盖 pending 位置。

`shutdown()` 停止两个定时器和 sink、关闭 reader、清除定位器并发 `shutdownFinished()`。所有错误发中文 `errorOccurred()`，不得抛到事件循环。

- [ ] **Step 5: 运行测试和线程生命周期检查**

增加把 worker 移入真实 `QThread`、queued 调用 `loadFile/analyzeAt/shutdown` 并等待 `shutdownFinished` 的测试。运行 `test_analysis`，预期全部通过且无 `QThread: Destroyed while thread is still running`。

- [ ] **Step 6: 提交**

```powershell
git add CMakeLists.txt src/analysis/audioanalysisworker.h src/analysis/audioanalysisworker.cpp tests/analysis/test_audioanalysis.cpp
git commit -m "实现离线音频分析工作线程"
```

---

### Task 7: 照片控件与音频分析页面

**Files:**
- Create: `src/ui/directionphotowidget.h`
- Create: `src/ui/directionphotowidget.cpp`
- Create: `src/ui/audioanalysispage.h`
- Create: `src/ui/audioanalysispage.cpp`
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `DirectionPhotoWidget::setImage()`、`setCameraView()`、`setDirections()`、`setStatusText()`。
- Produces: `AudioAnalysisPage::setActive(bool)` 和 `settingsChanged(const AudioAnalysisSettings &)`。

- [ ] **Step 1: 写页面结构与照片绘制失败测试**

在 `DashboardTest` 增加：

```cpp
void DashboardTest::audioAnalysisPageProvidesPlaybackLocalizationAndPhotoControls()
{
    AppSettings settings;
    AudioAnalysisPage page(&settings);
    page.show();
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("analysisOpenWaveButton")) != nullptr);
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("analysisPlayButton")) != nullptr);
    QVERIFY(page.findChild<QComboBox *>(QStringLiteral("analysisOutputChannelCombo")) != nullptr);
    QVERIFY(page.findChild<QComboBox *>(QStringLiteral("analysisLocatorCombo")) != nullptr);
    QVERIFY(page.findChild<DirectionPhotoWidget *>(QStringLiteral("directionPhotoWidget")) != nullptr);
    QVERIFY(page.findChild<QDoubleSpinBox *>(QStringLiteral("cameraHorizontalFovSpin")) != nullptr);
}
```

为照片控件增加 800×600 控件、400×400 图片的 `imageRect()` 测试，预期左右留白各 100 px；方向中心映射到控件中心。

- [ ] **Step 2: 运行 `test_ui` 确认 RED**

预期：编译失败，提示新页面/控件不存在。

- [ ] **Step 3: 实现 `DirectionPhotoWidget`**

`paintEvent()`：

1. 黑色背景并用 `KeepAspectRatio` 计算 `imageRect`。
2. 绘制图片。
3. 对每个达到门限的方向调用 `projectDirection()`。
4. 视场内绘制圆形十字准星和角度/置信度；颜色按置信度红→黄→绿线性插值。
5. 视场外在 `edgePoint` 绘制朝外三角箭头和“视场外”。
6. 无图片、低置信度或无结果时居中绘制状态文本。

公开只读 `imageRect()` 和 `markerPoints()` 用于 UI 布局诊断和测试，它们返回真实绘制状态，不是测试专用后门。

- [ ] **Step 4: 实现 `AudioAnalysisPage`**

按规格构建顶部文件区、左播放区、中照片区、右参数区和底部诊断区，所有关键控件设置稳定 objectName。创建 `QThread`（objectName `MicArrayAudioAnalysisThread`）和 worker，连接 `finished -> deleteLater`，启动线程。

文件/照片按钮使用 `QFileDialog`；工作请求全部 queued。进度滑条使用毫秒或样本的 64-bit 映射辅助函数，避免长文件溢出。算法下拉使用 descriptor ID 作为 data。更新参数时验证并保存 `AudioAnalysisSettings`，同时 queued 配置 worker。

`setActive(false)` 发 stop；析构发 shutdown，等待 `shutdownFinished` 后 quit，并最多等待 3000 ms。超时通过页面错误信号上报。

- [ ] **Step 5: 运行 UI 测试确认 GREEN**

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

预期：页面结构、照片矩形和现有 UI 测试通过。

- [ ] **Step 6: 提交**

```powershell
git add CMakeLists.txt src/ui/directionphotowidget.h src/ui/directionphotowidget.cpp src/ui/audioanalysispage.h src/ui/audioanalysispage.cpp tests/ui/test_dashboard.cpp
git commit -m "新增音频分析页面与照片标记"
```

---

### Task 8: 主窗口切页集成与实时播放隔离

**Files:**
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`
- Modify: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Consumes: `AudioAnalysisPage::setActive(bool)`。
- Produces behavior: 进入分析页把实时 `playbackEnabled` 设为 false；离开时停止离线播放，不恢复实时播放。

- [ ] **Step 1: 写切页行为失败测试**

```cpp
void DashboardTest::enteringAudioAnalysisDisablesOnlyRealtimePlayback()
{
    MainWindow window;
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    auto *analysis = window.findChild<AudioAnalysisPage *>(QStringLiteral("audioAnalysisPage"));
    auto *playback = window.findChild<QCheckBox *>(QStringLiteral("audioPlaybackCheck"));
    auto *recordStart = window.findChild<QPushButton *>(QStringLiteral("startRecordingButton"));
    QVERIFY(tabs && analysis && playback && recordStart);
    playback->setChecked(true);
    const bool recordingEnabledBefore = recordStart->isEnabled();
    tabs->setCurrentWidget(analysis);
    QVERIFY(!playback->isChecked());
    QCOMPARE(recordStart->isEnabled(), recordingEnabledBefore);
    tabs->setCurrentIndex(0);
    QVERIFY(!playback->isChecked());
}
```

另验证标签文字为“音频分析”，且切页不改变 `DeviceClient` 的状态属性、录制选择和增益控件值。

- [ ] **Step 2: 运行 `test_ui` 确认 RED**

预期：找不到 `audioAnalysisPage` 或 `audioPlaybackCheck` objectName。

- [ ] **Step 3: 集成页面与 currentChanged**

`MainWindow` 持有 `AudioAnalysisPage *m_audioAnalysisPage`，构建标签时加入页面。为现有播放复选框设置 objectName。连接：

```cpp
connect(m_mainTabs, &QTabWidget::currentChanged, this, [this](int index) {
    const bool analysisActive = m_mainTabs->widget(index) == m_audioAnalysisPage;
    if (analysisActive && m_audioPlaybackCheck->isChecked()) {
        m_audioPlaybackCheck->setChecked(false);
    }
    m_audioAnalysisPage->setActive(analysisActive);
});
```

复选框取消会沿现有 `updateAudioSettingsFromUi()` 把 `playbackEnabled=false` queued 投递到实时 `AudioProcessor`。不要调用断开、清队列、停止录音或修改其他设置。`saveSettings()` 保持关闭后的 false 状态。

- [ ] **Step 4: 运行 UI 和媒体回归测试**

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui test_media
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_ui|test_media" --output-on-failure
```

预期：全部通过。

- [ ] **Step 5: 提交**

```powershell
git add src/ui/mainwindow.h src/ui/mainwindow.cpp tests/ui/test_dashboard.cpp
git commit -m "集成音频分析与播放隔离"
```

---

### Task 9: 文档、完整验证与人工联调清单

**Files:**
- Modify: `doc/Readme.md`

**Interfaces:**
- Documents: WAV 支持范围、分析线程、算法接口、坐标系、照片视角、切页行为和验证方式。

- [ ] **Step 1: 更新维护文档**

在 `doc/Readme.md`：

- 核心模块增加 `src/analysis/` 及各职责。
- 线程边界增加 `AudioAnalysisWorker`，明确 QWidget 仍在 UI 线程。
- UI 页面增加“音频分析”控件和交互。
- 记录 WAV PCM/extensible 支持范围与 48 通道定位限制。
- 写明 6×8、5 cm、坐标系、正前方半球和远场单主声源假设。
- 写明 `ISoundSourceLocator` 源码级扩展流程。
- 写明照片中心方位/俯仰、水平/垂直 FOV 和滚转。
- 在 2026-07-23 变更记录加入本功能和实际验证命令。

- [ ] **Step 2: 运行针对性构建和全部 CTest**

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
```

预期：`test_protocol`、`test_device`、`test_media`、`test_app`、`test_ui`、`test_analysis` 全部通过。

- [ ] **Step 3: 运行 Python 与模拟服务端回归**

```powershell
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
git diff --check
```

预期：Python 测试和 self-test 通过，`git diff --check` 无错误；仅允许现有 CRLF 转换警告。

- [ ] **Step 4: 执行无声卡 UI 冒烟检查**

启动 `build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug\MicArray.exe`，检查：

1. 音频分析页在 1366×768 下可操作。
2. 打开 48 通道 WAV 后摘要、时长和 CH0～CH47 可用。
3. 打开图片并修改视角参数时画面保持比例。
4. 进入分析页关闭实时播放，设备连接、视频和录音控件不变。
5. 非 48 通道 WAV 能选通道但定位显示禁用原因。

- [ ] **Step 5: 记录真实声卡/阵列人工验收限制**

若当前环境没有真实声卡或已知位置的 48 通道录音，在交付说明明确列出尚未执行项，不把自动合成测试宣称为现场精度验收。现场验收应记录正前、左右、上下位置的角度误差、置信度、有效阵元对和处理耗时。

- [ ] **Step 6: 提交文档与最终修订**

```powershell
git add doc/Readme.md
git commit -m "完善音频分析功能文档"
```

---

## Plan Completion Criteria

- `test_analysis` 覆盖几何、注册表、WAV、FFT、GCC-PHAT、投影和 worker。
- `test_app` 覆盖分析配置持久化与逐字段回退。
- `test_ui` 覆盖页面结构、照片绘制区域、切页关闭实时播放及其他状态不受影响。
- 48 通道 WAV 能流式播放任一通道并按实际播放位置刷新定位。
- 少于 48 通道时仍可播放但定位明确禁用。
- 自定义定位器只需实现接口并注册工厂，无需修改页面。
- 全部 CTest、Python 测试、模拟服务端自检和 `git diff --check` 通过。
- `doc/Readme.md` 与实际实现和验证结果一致。
