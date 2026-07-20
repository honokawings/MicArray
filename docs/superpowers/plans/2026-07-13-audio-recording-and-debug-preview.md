# Audio Recording and Debug Preview Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Qt6 上位机增加可选择任意阵列通道的无损多通道 WAV 录音，并完成主页面命名、自适应字节统计和帧尾后 16 字节诊断预览。

**Architecture:** 原始音频帧从 `DeviceClient` 并行送入独立 `AudioRecorder` 线程，线程内使用 `MultiChannelWavWriter` 解交织并写入 WAVE_FORMAT_EXTENSIBLE 文件，播放链路保持不变。协议层即时输出媒体帧，同时单独延迟调试事件直到取得帧尾后的 16 个实际流字节。UI 只负责配置、状态显示和有界入队。

**Tech Stack:** C++20、Qt 6 Core/Widgets/Multimedia/Test、Qt signals/slots、WAVE_FORMAT_EXTENSIBLE、JSON、CMake/CTest。

## Global Constraints

- Qt 最低版本保持 6.5，使用现有 C++20 和 Qt Widgets 架构。
- 音频输入固定按 48 通道、每帧 200 采样点、每样本 4 字节小端、payload 38400 字节处理。
- WAV 使用 32-bit 容器并声明 24-bit 有效位，选择通道按物理编号升序交织。
- 网络、协议、播放和 UI 线程不得执行文件写盘。
- 录音待写队列上限固定为 64 MiB；无法接收完整帧时停止录音，不静默丢帧。
- 累计量和缓冲量使用 IEC 自适应单位；协议长度、偏移等联调字段保持精确字节值。
- 保留当前工作区的既有未提交改动，不重置、不整理、不提交无关文件；除非用户明确要求，不创建 Git 提交。

---

### Task 1: 自适应字节显示与页面命名

**Files:**
- Create: `src/ui/displayformat.h`
- Create: `src/ui/displayformat.cpp`
- Modify: `src/ui/mainwindow.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Produces: `QString formatByteSize(quint64 bytes)` 和 `QString formatByteRate(double bytesPerSecond)`。
- Consumers: 主窗口累计接收、解析、丢弃、缓冲、待解析、音频和后续录音统计。

- [ ] **Step 1: 写入失败测试**

在 `tests/ui/test_dashboard.cpp` 增加：

```cpp
#include "ui/displayformat.h"

void formatsAdaptiveByteUnits();
void mainWindowUsesAudioVideoFusionTitle();

void DashboardTest::formatsAdaptiveByteUnits()
{
    QCOMPARE(formatByteSize(0), QStringLiteral("0 B"));
    QCOMPARE(formatByteSize(1023), QStringLiteral("1023 B"));
    QCOMPARE(formatByteSize(1024), QStringLiteral("1 KiB"));
    QCOMPARE(formatByteSize(1536), QStringLiteral("1.5 KiB"));
    QCOMPARE(formatByteSize(5ULL * 1024 * 1024), QStringLiteral("5 MiB"));
    QCOMPARE(formatByteRate(1536.0), QStringLiteral("1.5 KiB/s"));
}

void DashboardTest::mainWindowUsesAudioVideoFusionTitle()
{
    MainWindow window;
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs != nullptr);
    QVERIFY(tabs->indexOf(tabs->findChild<QWidget *>(QStringLiteral("monitorPage"))) >= 0);
    QCOMPARE(tabs->tabText(tabs->indexOf(tabs->findChild<QWidget *>(QStringLiteral("monitorPage")))),
             QStringLiteral("音视频融合"));
}
```

- [ ] **Step 2: 运行 UI 测试并确认失败**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui`

Expected: 编译失败，提示 `ui/displayformat.h` 不存在，或页签仍为旧名称。

- [ ] **Step 3: 实现统一格式化工具**

`displayformat.h` 声明两个命名空间函数。`displayformat.cpp` 以 1024 进位选择 `B/KiB/MiB/GiB/TiB`，数值小于 10 保留最多两位、小于 100 保留最多一位、其余取整，并移除小数尾零。`formatByteRate()` 调用 `formatByteSize()` 的共同内部实现并追加 `/s`。

在 `mainwindow.cpp` 中：

- 将监控页设置 `objectName="monitorPage"`。
- 页签文字改为“音视频融合”。
- `m_rateLabel`、`m_protocolStatsLabel`、`m_audioStatsLabel` 的累计量和队列量改用格式化函数。
- 帧 payload 长度、总帧长、tailOffset 和 bufferSize 诊断保持精确整数。

- [ ] **Step 4: 更新 CMake 并运行测试**

将新源文件加入 `MICARRAY_SOURCES/MICARRAY_HEADERS`。

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui`

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure`

Expected: `test_ui` PASS。

- [ ] **Step 5: 审查阶段差异**

确认协议长度字段没有被近似显示，且没有改变任何统计值本身，只改变展示单位。

---

### Task 2: 帧尾后的真实 16 字节调试预览

**Files:**
- Modify: `src/protocol/protocolcodec.h`
- Modify: `src/protocol/protocolcodec.cpp`
- Modify: `src/protocol/protocolworker.cpp`
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`
- Test: `tests/protocol/test_protocolcodec.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- `HostFrame` 新增 `QByteArray postTailPreview` 和 `bool postTailPreviewComplete = false`。
- `ParseResult` 新增 `QList<HostFrame> debugFrames`。
- `ProtocolCodec::clear()` 返回 `QList<HostFrame>`，用于输出未满 16 字节的最后一个调试预览。
- `ParseResult::frames` 仍即时用于媒体分发；`debugFrames` 只用于 `ProtocolWorker::frameParsed`。

- [ ] **Step 1: 写同 chunk、跨 chunk 和清空测试**

新增测试覆盖：

```cpp
void capturesSixteenBytesAfterTailInSameChunk();
void completesPostTailPreviewAcrossChunksWithoutDelayingMediaFrame();
void flushesIncompletePostTailPreviewOnClear();
```

关键断言：

```cpp
ParseResult first = codec.parseIncoming(audioFrame);
QCOMPARE(first.frames.size(), 1);       // 媒体帧即时可用
QCOMPARE(first.debugFrames.size(), 0);  // 调试预览等待后续字节

ParseResult second = codec.parseIncoming(QByteArray::fromHex("00112233445566778899aabbccddeeff"));
QCOMPARE(second.debugFrames.size(), 1);
QCOMPARE(second.debugFrames.first().postTailPreview.toHex(),
         QByteArray("00112233445566778899aabbccddeeff"));
QVERIFY(second.debugFrames.first().postTailPreviewComplete);
```

`clear()` 测试断言返回一帧、预览长度小于 16 且 `postTailPreviewComplete == false`。

- [ ] **Step 2: 运行协议测试并确认失败**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_protocol`

Expected: 编译失败，提示新字段或 `debugFrames` 不存在。

- [ ] **Step 3: 实现调试帧待预览状态**

在 `ProtocolCodec` 增加：

```cpp
std::optional<HostFrame> m_pendingDebugFrame;
static constexpr qsizetype DebugPostTailPreviewSize = 16;
```

每次追加 chunk 后，先用 `m_buffer.left(remaining)` 补齐待预览帧；补满后追加到 `result.debugFrames`，但不移除这些字节。解析出新帧时先追加到 `result.frames`，再从 `m_buffer.mid(frameSize, 16)` 取得预览；满 16 字节直接进入 `debugFrames`，不足则保存为唯一待预览帧。由于不足 16 字节时也不足以解析下一份 16 字节媒体头，最多只存在一个待预览帧。

`clear()` 返回未完成调试帧后清空 `m_buffer` 和待预览状态。

- [ ] **Step 4: 分离媒体分发与调试分发**

`ProtocolWorker::ingestBytes()`：

- 遍历 `result.frames` 累计帧数并立即发出 audio/video 信号。
- 遍历 `result.debugFrames` 执行现有 100 ms 调试采样并发出 `frameParsed`。
- `clear()` 在重置计数前发出 codec 返回的未完成调试帧。

- [ ] **Step 5: 更新高级帧调试 UI**

增加 `m_debugPostTailLabel`，最近帧区域新增“帧后 16 字节”。历史行追加：

```text
帧后16: 7E 7E ... (16/16 B)
```

不足时显示 `(n/16 B，不完整)`。标签和历史继续使用等宽字体。

- [ ] **Step 6: 运行协议和 UI 测试**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_protocol test_ui`

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_protocol|test_ui" --output-on-failure`

Expected: 两个测试目标 PASS，原有 `frames` 数量和解析字节统计不变。

---

### Task 3: 多通道 WAV 写入核心

**Files:**
- Create: `src/media/multichannelwavwriter.h`
- Create: `src/media/multichannelwavwriter.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/media/test_media.cpp`

**Interfaces:**
- `RecordingSettings { QString outputDirectory; QString baseName; QVector<int> channels; int sampleRate; }`。
- `RecordingSummary { QString manifestPath; QStringList segmentPaths; quint64 inputFrames; quint64 sampleFrames; quint64 dataBytes; QString stopReason; }`。
- `MultiChannelWavWriter(quint64 segmentDataLimit = DefaultSegmentDataLimit)`。
- `bool open(const RecordingSettings &, QString *errorMessage)`。
- `bool appendFrame(const QByteArray &payload, QString *errorMessage)`。
- `bool close(const QString &stopReason, QString *errorMessage)`。

- [ ] **Step 1: 写 WAV 失败测试**

在临时目录录制通道 `{1, 4}`，构造 38400 字节 payload，并为 sample point 0/1 写入可识别的 32-bit 小端值。断言：

- 文件以 `RIFF`、`WAVE` 开头。
- fmt tag 为 `0xfffe`、通道数 2、采样率 48000、容器位数 32、有效位 24、channel mask 0。
- data 前 16 字节依次是 sample0 CH1、sample0 CH4、sample1 CH1、sample1 CH4。
- data size 等于 `200 * 2 * 4`，RIFF size 等于实际文件大小减 8。
- JSON 中 channels 为 `[1,4]`，inputFrames 为 1。

增加小分段上限测试，写入三帧后断言只在完整帧边界生成多个 WAV，JSON 包含全部分段。

- [ ] **Step 2: 运行 media 测试并确认失败**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media`

Expected: 编译失败，提示 `multichannelwavwriter.h` 不存在。

- [ ] **Step 3: 实现 WAV 头和通道解交织**

写入固定 68 字节 WAVE_FORMAT_EXTENSIBLE 头：

```text
RIFF + uint32 size + WAVE
fmt  + uint32(40) + 0xfffe + channels + sampleRate + byteRate
blockAlign + uint16(32) + uint16(22) + uint16(24) + uint32(0)
PCM subtype GUID 00000001-0000-0010-8000-00aa00389b71
data + uint32 dataSize
```

`appendFrame()` 必须先验证 payload 精确等于 38400 字节。输出缓冲按 200 个 sample point 遍历，再按升序选择通道复制原始 4 字节。任何 `QFile::write()` 短写返回失败。

- [ ] **Step 4: 实现完整帧分段、封口和 JSON**

追加一帧会超过当前分段上限时，先回写当前 WAV 头并打开 `_partNNN.wav`。`close()` 封口最后分段，并使用 `QSaveFile` 原子写入 JSON。非法目录、重复打开、空通道、重复/越界通道均返回具体错误。

- [ ] **Step 5: 更新 CMake 并运行 media 测试**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media`

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_media --output-on-failure`

Expected: `test_media` PASS。

- [ ] **Step 6: 检查生成文件**

测试中读取全部头字段和 JSON，不依赖外部播放器；确认写入字节与输入选中通道逐字节一致。

---

### Task 4: 有界异步录音控制器

**Files:**
- Create: `src/media/audiorecorder.h`
- Create: `src/media/audiorecorder.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/media/test_media.cpp`

**Interfaces:**
- `enum class RecordingState { Idle, Starting, Recording, Stopping, Error }`。
- `RecordingStatistics { RecordingState state; QString outputPath; qint64 elapsedMs; quint64 inputFrames; quint64 dataBytes; quint64 fileBytes; quint64 pendingBytes; QString error; }`。
- `bool AudioRecorder::startRecording(const RecordingSettings &settings)`。
- `bool AudioRecorder::enqueueFrame(const QByteArray &payload)`，只做容量检查和 queued 投递。
- `void AudioRecorder::stopRecording(const QString &reason = "user")`。
- `bool AudioRecorder::stopAndWait(int timeoutMs = 5000)`。
- `static constexpr quint64 MaxPendingBytes = 64ULL * 1024 * 1024`。

- [ ] **Step 1: 写状态机与异步写入失败测试**

测试使用 `QTemporaryDir` 和 `QSignalSpy`：

- 空通道启动返回 false 并进入 Error/Idle 可恢复状态。
- 合法启动最终发出 Recording。
- 入队一帧最终增加 inputFrames 和 dataBytes。
- 停止最终进入 Idle，输出文件和 JSON 存在。
- 重复开始被拒绝。
- 测试容量构造入口验证超过 MaxPendingBytes 时拒绝当前完整帧并触发停止原因 `queue-overflow`。

- [ ] **Step 2: 运行 media 测试并确认失败**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media`

Expected: 编译失败，提示 `AudioRecorder` 不存在。

- [ ] **Step 3: 实现 worker 和线程生命周期**

`AudioRecorder` 作为 UI 线程 facade，内部创建 `QThread` 和 cpp 私有 `AudioRecordingWorker`。facade 通过 queued signals 发送 start/append/stop；worker 持有 `MultiChannelWavWriter`。同一发送者到同一接收者的事件顺序保证 stop 位于已接受帧之后。

facade 在每次入队前检查 `m_pendingBytes + payload.size()`。超过 64 MiB 时不投递该帧，状态改为 Stopping，并请求 worker 以 `queue-overflow` 停止。worker 每完成一帧回传 consumed bytes，facade 扣减 pending 计数。

- [ ] **Step 4: 节流统计与错误传播**

worker 最多每 200 ms 发一次进度，停止时强制发最终进度。短写、非法 payload 和封口失败进入 Error，错误信号包含文件路径和原因。下一次合法 start 可从 Error 恢复。

- [ ] **Step 5: 运行 media 测试和线程清理检查**

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_media --output-on-failure`

Expected: PASS，测试退出时无 `QThread: Destroyed while thread is still running`。

---

### Task 5: 音频录制页面与主链路接入

**Files:**
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`
- Modify: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- `QWidget *MainWindow::buildRecordingPage()`。
- `void MainWindow::startRecording()`、`stopRecording()`、`chooseRecordingDirectory()`、`updateRecordingStatistics(...)`。
- MainWindow 持有 `AudioRecorder m_audioRecorder`，不直接持有录音线程。

- [ ] **Step 1: 写录音页面失败测试**

断言：

- 顶层存在“音频录制”页。
- 存在 48 个 `recordChannel_0` 至 `recordChannel_47` 可勾选按钮。
- “全选”后 48 个通道选中，“清空”后均未选，“当前播放通道”只选择输出通道。
- 未连接时 `startRecordingButton` 禁用。
- 统计文本使用自适应单位。

- [ ] **Step 2: 运行 UI 测试并确认失败**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui`

Expected: 新控件查找失败。

- [ ] **Step 3: 构建 6x8 录音页面**

使用 checkable `QToolButton` 构建固定 6x8 网格，按钮显示 `CH0` 至 `CH47`，尺寸稳定且与校准排列一致。增加通道快捷命令、保存目录、浏览按钮、开始/停止按钮和状态标签。默认选择当前播放通道，默认目录使用 `QStandardPaths::MusicLocation`，不可用时回退到 home。

- [ ] **Step 4: 接入原始音频帧与生命周期**

在 `DeviceClient::audioFrameReceived` 回调中，先在录音激活时调用 `m_audioRecorder.enqueueFrame(frame.payload)`，再进入现有 `m_audioFrameQueue`，保证录音不受播放队列丢旧帧策略影响。

断线时调用 `stopRecording("device-disconnected")`；析构最前面调用 `stopAndWait()`；录音期间禁用通道、目录和采样率修改。录音错误写入 `Logger` 并显示在录音页，不断开设备。

- [ ] **Step 5: 连接状态和录音统计显示**

开始按钮仅在 Connected、非录音、至少一个通道且目录非空时启用。统计显示持续时间、`formatByteSize(dataBytes)`、`formatByteRate(dataBytes / elapsedSeconds)` 和 `formatByteSize(pendingBytes)`。

- [ ] **Step 6: 运行 UI 与 media 测试**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui test_media`

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_ui|test_media" --output-on-failure`

Expected: 两个测试目标 PASS。

---

### Task 6: 文档、全量回归与代码审查

**Files:**
- Modify: `doc/Readme.md`
- Review: all files changed by Tasks 1-5

**Interfaces:**
- 文档记录最终用户操作、文件格式、断线行为、4 GiB 分段、自适应单位和帧后预览。

- [ ] **Step 1: 更新功能文档**

在 `doc/Readme.md` 增加“音频录制”章节和高级帧调试更新，说明：

- 6x8 通道选择与快捷选择。
- 单个逻辑多通道 WAV、32/24-bit、升序通道映射和 JSON。
- 分段、停止原因、断线自动封口和队列溢出策略。
- 自适应单位只用于统计，协议字段保持精确值。
- 帧历史新增帧尾后 16 字节。

在变更记录追加 2026-07-13 条目。

- [ ] **Step 2: 执行 C++ 全量构建和测试**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug`

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure`

Expected: 所有 C++ 测试 PASS。

- [ ] **Step 3: 执行 Python 回归和模拟服务端自检**

Run: `python -m unittest discover -s tests/tools -p "test_*.py" -v`

Run: `python tools/mock_device_server.py --self-test`

Expected: Python 测试和 self-test PASS。

- [ ] **Step 4: 静态差异检查**

Run: `git diff --check`

确认没有空白错误、无意修改 refer/doc 之外的协议文档、生成文件或现有用户改动。

- [ ] **Step 5: 分阶段规格审查**

逐项对照设计规格：页签命名、自适应统计、多通道 WAV、JSON 映射、独立线程、64 MiB 上限、断线封口、帧尾后 16 字节、测试和文档均有可定位实现。

- [ ] **Step 6: 最终代码审查**

重点检查线程析构、queued signal 顺序、WAV 长度溢出、短写处理、录音期间配置锁定、UI 线程写盘、协议字节统计是否被调试预览重复计算。发现问题先补回归测试再修复。

