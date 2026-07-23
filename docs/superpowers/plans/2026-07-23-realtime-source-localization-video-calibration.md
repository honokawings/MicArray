# 实时声源定位与视频四点校准 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增独立“声源定位”页面，使用实时 48 通道音频在实时视频上显示主声源标记或真实 SRP-PHAT 热力图，并支持四点自动拟合及手动调整视频视角参数。

**Architecture:** 增益后的实时音频旁路进入专用 `LiveLocalizationWorker` 线程，工作线程累积连续窗口并复用源码级定位器；内置 GCC-PHAT 额外实现可选空间响应接口。UI 复用统一的视频绘制矩形逻辑，通过物理 `CameraViewSettings` 完成标记、热力图和四点拟合，不改变实时播放或其他设备功能。

**Tech Stack:** Qt 6.5+ Core/Gui/Widgets/Multimedia/Test、C++20、CMake/Ninja、Qt Test。

## Global Constraints

- 阵列固定为 6×8、间距 0.05 m，`+X` 向右、`+Y` 向上、`+Z` 向前。
- 使用增益和 AGC 后的 48×200、32-bit 小端实时 payload。
- 定位与热力图计算不得在 UI、网络、协议、视频或现有音频处理线程执行。
- 进入或离开声源定位页面不得改变实时通道播放、连接、录音、增益或控制状态。
- 热力图必须来自真实 SRP-PHAT 空间响应，不得用单点渐变光斑替代。
- 四点校准拟合 `CameraViewSettings` 五个物理参数，不使用通用单应矩阵。
- 校准数据使用归一化视频坐标持久化，不因分辨率或宽高比变化提示重新校准。
- 所有行为变化必须更新 `doc/Readme.md` 和变更记录。

---

## File Map

**Create**

- `src/analysis/ispatialresponseprovider.h`：可选空间响应接口。
- `src/analysis/cameraviewcalibrator.h/.cpp`：四点物理视角拟合。
- `src/analysis/livelocalizationworker.h/.cpp`：实时音频窗口、调度、定位和校准采样。
- `src/ui/livelocalizationview.h/.cpp`：视频、热力图、标记和点击绘制。
- `src/ui/livelocalizationpage.h/.cpp`：页面、线程和设置装配。

**Modify**

- `src/analysis/localizationtypes.h`：空间响应、实时结果和校准数据类型。
- `src/analysis/directionprojection.h/.cpp`：视频点到方向的反投影。
- `src/analysis/gccphatlocator.h/.cpp`：复用 PHAT 相关数据并实现空间响应。
- `src/app/appsettings.h/.cpp`：`LiveLocalizationSettings`。
- `src/ui/videopreviewwidget.h/.cpp`：统一视频帧和实际绘制矩形。
- `src/ui/mainwindow.h/.cpp`：页面和实时音视频旁路。
- `tests/analysis/test_audioanalysis.cpp`、`tests/app/test_app.cpp`、`tests/ui/test_dashboard.cpp`。
- `CMakeLists.txt`、`doc/Readme.md`。

---

### Task 1: 空间响应、实时设置与持久化类型

**Files:**
- Modify: `src/analysis/localizationtypes.h`
- Modify: `src/app/appsettings.h`
- Modify: `src/app/appsettings.cpp`
- Modify: `tests/app/test_app.cpp`

**Interfaces:**
- Produces: `ScanDirection`、`SpatialResponseGrid`、`SpatialResponseResult`、`LiveLocalizationResult`、`VideoCalibrationSample`、`LiveLocalizationSettings`。

- [ ] **Step 1: 写入失败的设置持久化测试**

```cpp
void AppTest::persistsLiveLocalizationSettings()
{
    QTemporaryDir directory;
    LiveLocalizationSettings expected;
    expected.enabled = true;
    expected.displayMode = QStringLiteral("heatmap");
    expected.heatmapOpacity = 0.55;
    expected.updateIntervalMs = 100;
    expected.camera.centerAzimuthDegrees = 12.0;
    expected.camera.horizontalFovDegrees = 82.0;
    expected.calibrationSamples = {
        {{-20.0, 10.0}, {0.1, 0.2}, 0.8, 1.2},
        {{20.0, 10.0}, {0.9, 0.2}, 0.8, 1.1},
        {{20.0, -10.0}, {0.9, 0.8}, 0.9, 1.0},
        {{-20.0, -10.0}, {0.1, 0.8}, 0.9, 1.0},
    };
    {
        AppSettings settings(directory.filePath("settings.ini"));
        settings.saveLiveLocalization(expected);
    }
    AppSettings reloaded(directory.filePath("settings.ini"));
    QCOMPARE(reloaded.liveLocalization(), expected);
}
```

再增加逐字段非法值回退测试：非法模式、透明度、周期、非 2 次幂窗长、非法相机参数和错误数量校准点各自回退，合法字段仍保留。

- [ ] **Step 2: 运行测试确认 RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app
```

Expected: 编译失败，缺少 `LiveLocalizationSettings` 和访问器。

- [ ] **Step 3: 添加精确公共类型**

```cpp
struct ScanDirection {
    double azimuthDegrees = 0.0;
    double elevationDegrees = 0.0;
    bool operator==(const ScanDirection &) const = default;
};

struct SpatialResponseGrid {
    int columns = 0;
    int rows = 0;
    QVector<ScanDirection> directions;
};

struct SpatialResponseResult {
    QVector<float> values;
    double reliability = 0.0;
};

struct LiveLocalizationResult {
    QVector<SoundSourceDirection> directions;
    qint64 timestampMicroseconds = 0;
    qint64 elapsedMilliseconds = 0;
};

struct LiveSpatialResponse {
    int columns = 0;
    int rows = 0;
    QVector<float> normalizedValues;
    qint64 timestampMicroseconds = 0;
    quint64 generation = 0;
    double reliability = 0.0;
};

struct VideoCalibrationSample {
    ScanDirection direction;
    QPointF normalizedVideoPoint;
    double confidence = 0.0;
    double angularDeviationDegrees = 0.0;
    bool operator==(const VideoCalibrationSample &) const = default;
};

struct LiveLocalizationSettings {
    bool enabled = true;
    QString locatorId = QStringLiteral("gcc-phat-tdoa");
    QString displayMode = QStringLiteral("marker");
    double heatmapOpacity = 0.45;
    int windowSamples = 2048;
    int updateIntervalMs = 100;
    double soundSpeedMetersPerSecond = 343.0;
    double smoothingFactor = 0.35;
    double confidenceThreshold = 0.45;
    CameraViewSettings camera;
    QVector<VideoCalibrationSample> calibrationSamples;
    double calibrationRmsPixels = 0.0;
    int calibrationVersion = 1;
    bool operator==(const LiveLocalizationSettings &) const = default;
};
```

在 `AppSettings` 增加 `liveLocalization()` 和 `saveLiveLocalization()`；校准点使用固定索引键保存，不接受非 0/4 数量。保存前执行与读取相同的清洗函数并 `sync()`。

- [ ] **Step 4: 运行 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_app --output-on-failure
```

Expected: `test_app` PASS。

- [ ] **Step 5: 提交**

```powershell
git add src/analysis/localizationtypes.h src/app/appsettings.h src/app/appsettings.cpp tests/app/test_app.cpp
git commit -m "持久化实时声源定位配置"
```

---

### Task 2: 视频反投影与四点物理视角拟合

**Files:**
- Create: `src/analysis/cameraviewcalibrator.h`
- Create: `src/analysis/cameraviewcalibrator.cpp`
- Modify: `src/analysis/directionprojection.h`
- Modify: `src/analysis/directionprojection.cpp`
- Modify: `tests/analysis/test_audioanalysis.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ScanDirection unprojectVideoPoint(QPointF, CameraViewSettings)`。
- Produces: `CameraViewFitResult fitCameraView(QVector<VideoCalibrationSample>, CameraViewSettings, QSize)`。

- [ ] **Step 1: 写入投影往返和拟合测试**

测试使用已知相机参数生成四个方向的投影点，再从偏离初值拟合：

```cpp
CameraViewSettings expected;
expected.centerAzimuthDegrees = 14.0;
expected.centerElevationDegrees = -6.0;
expected.horizontalFovDegrees = 84.0;
expected.verticalFovDegrees = 58.0;
expected.rollDegrees = 4.0;

QVector<VideoCalibrationSample> samples;
for (ScanDirection direction : {
         ScanDirection{-25.0, 12.0}, {35.0, 10.0},
         {30.0, -20.0}, {-22.0, -18.0}}) {
    SoundSourceDirection source;
    source.azimuthDegrees = direction.azimuthDegrees;
    source.elevationDegrees = direction.elevationDegrees;
    const ProjectionResult projected = projectDirection(source, expected);
    QVERIFY(projected.inView);
    samples.append({direction, projected.normalizedPoint, 0.9, 1.0});
}
const CameraViewFitResult fitted = fitCameraView(samples, CameraViewSettings{}, {1280, 720});
QVERIFY(fitted.valid);
QVERIFY(fitted.rmsPixels < 1.0);
```

增加重复方向、重复像素、少于四点和非有限值拒绝测试。

- [ ] **Step 2: 运行 RED**

Run: `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis`

Expected: 缺少拟合接口。

- [ ] **Step 3: 实现反投影和受限多起点阻尼最小二乘**

`unprojectVideoPoint()` 对针孔投影执行逆变换；拟合函数：

- 参数向量顺序固定为中心方位、中心俯仰、水平 FOV、垂直 FOV、滚转。
- 目标为四个预测归一化点与观测点的加权平方误差。
- 使用有限差分 Jacobian、`5×5` 法方程和阻尼项。
- 每轮限制参数合法范围，最多 100 轮。
- 初值包括当前设置及中心角/FOV 的确定性扰动。
- 返回每点像素残差、RMS 像素误差、收敛状态和中文错误。
- RMS 阈值默认 25 px，任一点阈值 50 px。

- [ ] **Step 4: 运行 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_analysis --output-on-failure
```

- [ ] **Step 5: 提交**

```powershell
git add CMakeLists.txt src/analysis/directionprojection.* src/analysis/cameraviewcalibrator.* tests/analysis/test_audioanalysis.cpp
git commit -m "实现视频四点视角参数拟合"
```

---

### Task 3: 可扩展 SRP-PHAT 空间响应接口

**Files:**
- Create: `src/analysis/ispatialresponseprovider.h`
- Modify: `src/analysis/gccphatlocator.h`
- Modify: `src/analysis/gccphatlocator.cpp`
- Modify: `tests/analysis/test_audioanalysis.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ISpatialResponseProvider::evaluateSpatialResponse(window, grid)`。
- `GccPhatLocator` 同时实现 `ISoundSourceLocator` 和 `ISpatialResponseProvider`。

- [ ] **Step 1: 写入真实空间响应峰值测试**

用现有 `syntheticPlaneWave(25, 10)` 生成窗口，构造包含目标和干扰方向的网格：

```cpp
SpatialResponseGrid grid;
grid.columns = 5;
grid.rows = 3;
for (double elevation : {-20.0, 10.0, 30.0})
    for (double azimuth : {-50.0, -10.0, 25.0, 50.0, 70.0})
        grid.directions.append({azimuth, elevation});
const SpatialResponseResult response =
    provider->evaluateSpatialResponse(window, grid);
QCOMPARE(response.values.size(), 15);
QCOMPARE(std::distance(response.values.cbegin(),
                       std::max_element(response.values.cbegin(), response.values.cend())),
         7);
QVERIFY(response.reliability > 0.4);
```

增加静音返回低可靠度、非法网格返回空值、所有结果有限非负测试。

- [ ] **Step 2: 运行 RED**

Expected: 缺少 `ISpatialResponseProvider`。

- [ ] **Step 3: 提取 PHAT 相关缓存并实现 SRP**

在 `GccPhatLocator` 内新增每阵元对相关序列缓存。`locate()` 和 `evaluateSpatialResponse()` 对同一 `centerSample` 复用 FFT/相关结果。每个扫描方向：

```text
predictedSamples =
  (pair.dx * ux + pair.dy * uy)
  / soundSpeed * sampleRate
response += pairQuality * interpolatedCorrelation(predictedSamples)
```

输出非负响应值；可靠度综合信号能量、有效阵元对比例和峰值突出度。不得在此层执行视频颜色映射。

- [ ] **Step 4: 运行 GREEN 与定位回归**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_analysis --output-on-failure
```

- [ ] **Step 5: 提交**

```powershell
git add CMakeLists.txt src/analysis/ispatialresponseprovider.h src/analysis/gccphatlocator.* tests/analysis/test_audioanalysis.cpp
git commit -m "增加SRP-PHAT空间响应接口"
```

---

### Task 4: 实时定位工作线程

**Files:**
- Create: `src/analysis/livelocalizationworker.h`
- Create: `src/analysis/livelocalizationworker.cpp`
- Modify: `tests/analysis/test_audioanalysis.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: 增益后 38400 字节 payload、`LiveLocalizationSettings`、`SpatialResponseGrid`。
- Produces: `localizationReady`、`spatialResponseReady`、`calibrationSampleReady`、`statusChanged`。

- [ ] **Step 1: 写入实时累积、停用、最新窗口和稳定采样测试**

测试向 worker 连续发送从合成平面波编码的 200 样本 payload，确认不足 2048 样本不输出，第 11 帧后输出方向；`setActive(false)` 后不输出且重新激活需重新积累。增加专用 `QThread` 中 `shutdownFinished` 测试。

- [ ] **Step 2: 运行 RED**

Expected: 缺少 `livelocalizationworker.h`。

- [ ] **Step 3: 实现工作线程**

核心 slots：

```cpp
void configure(const LiveLocalizationSettings &settings);
void setActive(bool active);
void setSpatialGrid(const SpatialResponseGrid &grid, quint64 generation);
void pushAdjustedFrame(const QByteArray &payload,
                       quint32 timestampSeconds,
                       quint32 timestampMicros);
void captureStableDirection(quint64 requestId);
void shutdown();
```

使用 `QVector<QVector<float>>` 的固定容量通道环形缓冲或等价连续结构。每到更新周期以最新 `windowSamples` 构造 `MultiChannelAudioWindow`；不使用阻塞 sleep。保存最近 1 秒可靠方向，通过单位向量平均和角距离标准差生成校准采样，拒绝少于 5 个有效结果、平均置信度不足或标准差大于 5°。

热力图结果执行基线扣除、最大值归一化和时间平滑，网格 generation 不匹配时丢弃。

- [ ] **Step 4: 运行 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_analysis --output-on-failure
```

- [ ] **Step 5: 提交**

```powershell
git add CMakeLists.txt src/analysis/livelocalizationworker.* tests/analysis/test_audioanalysis.cpp
git commit -m "实现实时声源定位工作线程"
```

---

### Task 5: 可点击实时视频与定位叠加控件

**Files:**
- Modify: `src/ui/videopreviewwidget.h`
- Modify: `src/ui/videopreviewwidget.cpp`
- Create: `src/ui/livelocalizationview.h`
- Create: `src/ui/livelocalizationview.cpp`
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `VideoPreviewWidget::setFrame()`、`videoRect()`、`normalizedVideoPoint()`。
- Produces: `LiveLocalizationView::calibrationPointClicked(QPointF)`。

- [ ] **Step 1: 写入视频矩形、黑边点击和叠加测试**

在 640×480 控件放入 1600×900 图像，验证 `videoRect()==QRectF(0,60,640,360)`；点击中心发出 `(0.5,0.5)`，点击顶部黑边不发信号。验证 marker point 与 `projectDirection` 一致，空间响应图尺寸非法时不绘制。

- [ ] **Step 2: 运行 RED**

Expected: 缺少新 API 和控件。

- [ ] **Step 3: 实现统一视频绘制与叠加**

`VideoPreviewWidget` 改为自己保存和绘制 `QImage`，不再由 `MainWindow` 预缩放 pixmap。`LiveLocalizationView` 继承视频基础控件，绘制顺序：

1. 视频。
2. 双线性缩放后的半透明热力图。
3. 主峰准星和状态文字。
4. 校准点击点、预测点和误差连线。

校准模式下左键只在 `videoRect()` 内发出归一化点。

- [ ] **Step 4: 运行 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

- [ ] **Step 5: 提交**

```powershell
git add CMakeLists.txt src/ui/videopreviewwidget.* src/ui/livelocalizationview.* tests/ui/test_dashboard.cpp
git commit -m "实现实时视频声源叠加控件"
```

---

### Task 6: 声源定位页面与四点校准交互

**Files:**
- Create: `src/ui/livelocalizationpage.h`
- Create: `src/ui/livelocalizationpage.cpp`
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `setActive(bool)`、`setVideoFrame(const QImage &, quint32, quint32)`、`submitAdjustedAudioFrame(const QByteArray &, quint32, quint32)`。

- [ ] **Step 1: 写入页面控件与隔离测试**

验证对象名：

```text
liveLocalizationPage
liveLocalizationView
liveLocalizationEnabledCheck
liveLocalizationDisplayModeCombo
liveLocalizationLocatorCombo
liveHeatmapOpacitySpin
liveCameraCenterAzimuthSpin
liveCameraCenterElevationSpin
liveCameraHorizontalFovSpin
liveCameraVerticalFovSpin
liveCameraRollSpin
startFourPointCalibrationButton
cancelFourPointCalibrationButton
clearVideoCalibrationButton
liveCalibrationProgressLabel
liveCalibrationRmsLabel
```

验证显示模式含 `marker`、`heatmap`，五个视角字段使用设置值，页面析构后专用线程退出。

- [ ] **Step 2: 运行 RED**

- [ ] **Step 3: 构建页面和校准状态机**

页面创建 worker 线程，使用 queued signal/slot。四点流程：

```text
Idle -> Capturing(0..3) -> Fitting -> Idle
Idle/Capturing -> Cancel -> Idle
```

点击后请求 worker 捕获稳定方向；有效结果与点击点配对。四点完成调用 `fitCameraView()`，成功立即更新 spin、叠加、RMS 和设置；失败保留旧相机参数并显示错误。手动 spin 改变立即预览，`editingFinished` 保存。清除恢复默认参数并删除校准点。

根据 `dynamic_cast<ISpatialResponseProvider *>` 能力状态禁用 heatmap 项。修改相机参数后重建按视频宽高比约 64×36 的反投影方向网格并递增 generation。

- [ ] **Step 4: 运行 GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure
```

- [ ] **Step 5: 提交**

```powershell
git add CMakeLists.txt src/ui/livelocalizationpage.* tests/ui/test_dashboard.cpp
git commit -m "新增实时声源定位与四点校准页面"
```

---

### Task 7: 主窗口实时音视频集成

**Files:**
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`
- Modify: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- `MainWindow` 将解码视频帧和 `adjustedFrameReady` payload 旁路给页面。

- [ ] **Step 1: 写入主窗口集成回归**

验证存在“声源定位”标签；将实时播放复选框设为开启，切入和切出页面后仍保持开启。通过 `showVideoFrame()` 注入图像后，定位页视频控件具有相同帧尺寸。录音状态和输出通道控件保持不变。

- [ ] **Step 2: 运行 RED**

- [ ] **Step 3: 集成页面**

在标签顺序中加入“声源定位”。`showVideoFrame()` 缓存一次图像，渲染节拍同时调用两个视频控件的 `setFrame()`。`adjustedFrameReady` 继续先服务录音，并在页面激活时 queued 投递给实时定位 worker。标签切换只调用 `setActive()`，不得修改 `m_audioPlaybackCheck`。

主窗口析构时先销毁实时定位页，再停止现有媒体线程，避免页面持有的 `AppSettings*` 晚于设置对象析构。

- [ ] **Step 4: 运行 UI、媒体和分析回归**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui test_media test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_(ui|media|analysis)" --output-on-failure
```

- [ ] **Step 5: 提交**

```powershell
git add src/ui/mainwindow.* tests/ui/test_dashboard.cpp
git commit -m "集成实时声源定位音视频链路"
```

---

### Task 8: 文档、完整验证和人工验收清单

**Files:**
- Modify: `doc/Readme.md`

- [ ] **Step 1: 更新功能文档**

记录新页面、独立线程、SRP-PHAT 接口、四点拟合五个相机参数、手动参数、持久化、页面切换不改变播放状态和源码扩展方法，并追加 2026-07-23 变更记录。

- [ ] **Step 2: 完整构建**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
```

Expected: exit 0。

- [ ] **Step 3: 完整自动化验证**

Run:

```powershell
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
git diff --check
```

Expected: 6 个 CTest 全部通过、45 个 Python 测试通过、自检通过、diff check 无错误。

- [ ] **Step 4: 检查工作区范围**

Run: `git status --short`。

Expected: 仅本任务文档变化或用户已有未提交文件；不得包含生成的 build/dist 文件。

- [ ] **Step 5: 提交文档**

```powershell
git add doc/Readme.md
git commit -m "补充实时声源定位使用文档"
```

- [ ] **Step 6: 输出人工验收清单**

交付说明明确自动测试不替代真实阵列验收，列出四点校准、手动微调、纯标记、热力图、播放状态保持、混响和多声源检查项。
