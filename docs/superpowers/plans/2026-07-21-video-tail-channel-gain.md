# 视频帧尾、六路控制与多通道移位增益实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将视频帧改为 `0A + 任意字节` 双字节帧尾，把控制业务收敛为六路，并实现 48 通道共享移位增益、独立持续 AGC、本地配置及增益后录制。

**Architecture:** 协议层保留原始帧并只调整视频尾边界；新建纯 C++ `ChannelGainProcessor` 统一生成增益后 payload，由音频工作线程向录音和解码分发同一数据。`ArrayHeatmapWidget` 负责 6x8 幅值与竖向增益控制，`GainProfileStore` 负责 JSON 原子保存/完整校验，`AppSettings` 仅持久化最近有效配置。

**Tech Stack:** Qt 6.5+、C++20、Qt Widgets、Qt Test、QSettings、QJsonDocument、QSaveFile、Python 3.9+ unittest。

## Global Constraints

- 视频尾固定占两个字节，第一个字节必须为 `0x0A`，第二个字节必须存在但取值任意。
- 音频帧、心跳帧和控制帧继续使用线路字节 `0A 0D`。
- 控制业务只暴露接口 0-5；`0xCC11` 仍为 26 字节并在最后自动编码两个零值。
- 每通道增益只能为整数 `-8~+8 bit`；左移使用 `qint64` 中间值并饱和到 `qint32`，右移保持符号。
- 自动增益按通道独立运行：上限 80%，下限 35%，低幅保持 500 ms，之后每 250 ms 最多增加 1 bit。
- 幅值、监听和录音必须使用同一增益后 payload；高级帧调试必须显示原始 payload。
- 音频 payload 必须恰好为 38400 字节；增益队列和录音队列同时限制字节数和帧数。
- JSON 配置必须原子写入、完整校验、一次性应用；复位为 48 路 `0 bit + Manual`。
- 关键行为变更必须同步更新 `refer/音频矩阵通信协议.md` 和 `doc/Readme.md`。

---

## File Map

- `src/protocol/protocolcodec.*`：视频双字节通配尾解析、帧大小和调试预览边界。
- `src/control/controlvalues.*`：六路业务控制值。
- `src/media/channelgainprocessor.*`：48 路移位、饱和、独立 AGC 和快照。
- `src/media/gainprofilestore.*`：增益 JSON 序列化、校验和原子文件 I/O。
- `src/media/audioprocessor.*`：在线程内调用共享增益处理器，向录音与解码分发同一结果。
- `src/media/audiorecorder.*`：严格 payload 校验和帧数/字节数双重有界队列。
- `src/media/multichannelwavwriter.*`：录制清单写入增益元数据。
- `src/app/appsettings.*`：最近增益配置与旧八路控制预设迁移。
- `src/ui/arrayheatmapwidget.*`：48 个幅值单元、竖向滑条和独立自动状态。
- `src/ui/mainwindow.*`：批量命令、保存/载入、工作线程分发、录制和日志集成。
- `tools/mock_device_server.py`：默认视频尾 `0A 00`。

---

### Task 1: 视频双字节通配帧尾

**Files:**
- Modify: `src/protocol/protocolcodec.cpp`
- Modify: `src/protocol/protocolcodec.h`
- Modify: `tests/protocol/test_protocolcodec.cpp`
- Modify: `tests/device/test_deviceclient.cpp`
- Modify: `tools/mock_device_server.py`
- Modify: `tests/tools/test_mock_device_server.py`

**Interfaces:**
- Produces: 视频帧 `frameSize = 16 + payloadLength + 2`，尾合法条件为 `tail[0] == 0x0A`。
- Produces: `HostFrame::rawFrame` 包含两个尾字节，`postTailPreview` 从第二尾字节之后开始。

- [ ] **Step 1: 写入失败的协议测试**

在 `test_protocolcodec.cpp` 增加：

```cpp
void acceptsAnySecondVideoTailByte();
void waitsForSecondVideoTailByte();
void rejectsInvalidFirstVideoTailByte();
void videoPostTailStartsAfterWildcardByte();
```

使用 `...aabb0a00`、`...aabb0aff`、仅 `...aabb0a` 和 `...aabb7f00` 验证帧数、错误和 `postTailPreview`。

- [ ] **Step 2: 运行红灯测试**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_protocol && ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_protocol --output-on-failure`

Expected: 旧实现把第二尾字节当成帧后数据，至少一个新增断言失败。

- [ ] **Step 3: 实现双字节视频尾**

将视频尾大小固定为 2，并把校验限定为首字节：

```cpp
bool hasValidIncomingTail(const QByteArray &buffer, qsizetype offset, quint16 rawType)
{
    if (rawType == static_cast<quint16>(FrameType::Video)) {
        return static_cast<uchar>(buffer.at(offset)) == 0x0a;
    }
    return readU16(buffer, offset) == ProtocolCodec::HostTail;
}
```

诊断实际尾保存两个字节；错误提示将视频期望值表示为 `0A ??`，不得读取缺失的第二字节。

- [ ] **Step 4: 更新模拟服务端并写 Python 红绿测试**

`pack_media_frame(TYPE_VIDEO, payload)` 返回 `header + payload + b"\x0a\x00"`；音频仍追加 `struct.pack("<H", TAIL)`。测试断言视频长度增加 2 且尾为 `0A 00`。

- [ ] **Step 5: 验证任务 1**

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_protocol|test_device" --output-on-failure`

Run: `python -m unittest discover -s tests/tools -p "test_*.py" -v`

Expected: 全部通过。

### Task 2: 六路控制业务与旧预设迁移

**Files:**
- Modify: `src/control/controlvalues.h`
- Modify: `src/control/controlvalues.cpp`
- Modify: `src/protocol/protocolcodec.cpp`
- Modify: `src/app/appsettings.cpp`
- Modify: `src/app/appsettings.h`
- Modify: `src/ui/mainwindow.cpp`
- Modify: `src/ui/mainwindow.h`
- Modify: `tests/protocol/test_protocolcodec.cpp`
- Modify: `tests/app/test_app.cpp`
- Modify: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Produces: `ControlValues::ChannelCount == 6`。
- Produces: `ProtocolCodec::encodeControl()` 在六个业务值后追加两个 `qint16(0)`。
- Produces: `AppSettings::controlPresets()` 接受六路新预设并将八路旧预设截取为六路。

- [ ] **Step 1: 写入六路控制失败测试**

断言：

```cpp
QCOMPARE(ControlValues::ChannelCount, 6);
QCOMPARE(frame.size(), ProtocolCodec::ControlFrameSize);
QCOMPARE(frame.mid(20, 4), QByteArray(4, '\0'));
```

应用测试写入八路旧预设，期望读取 `{1,2,3,4,5,6}`；UI 测试期望找不到 `controlSpin_6` 和 `controlSpin_7`。

- [ ] **Step 2: 运行红灯测试**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_protocol test_app test_ui`

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_protocol|test_app|test_ui" --output-on-failure`

Expected: 旧八路模型和预设长度断言失败。

- [ ] **Step 3: 收敛控制模型和编码器**

删除 `ActiveChannelCount` 与 `isReserved()`；编码器使用：

```cpp
for (qint16 value : values.values()) {
    appendI16(frame, value);
}
appendI16(frame, 0);
appendI16(frame, 0);
```

UI 数组、循环、预设、撤销和复位自然按六路运行，不保留隐藏的 6、7 控件。

- [ ] **Step 4: 实现预设迁移并验证**

读取 `values` 长度为 8 时 `resize(6)`；长度为 6 时原样使用；其他长度拒绝。保存只接受六路。

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_protocol|test_app|test_ui" --output-on-failure`

Expected: 全部通过。

### Task 3: 48 通道移位增益与独立 AGC 核心

**Files:**
- Create: `src/media/channelgainprocessor.h`
- Create: `src/media/channelgainprocessor.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/media/test_media.cpp`

**Interfaces:**
- Produces: `GainConfiguration`，包含 48 项 `shifts`、48 项 `automatic` 和四个 AGC 参数。
- Produces: `GainSnapshot`，包含当前移位、模式、输入/输出峰值比和削顶计数。
- Produces: `GainProcessResult ChannelGainProcessor::process(const QByteArray &payload, int sampleRate)`。
- Produces: `bool ChannelGainProcessor::setConfiguration(const GainConfiguration &, QString *error)` 原子校验接口。

- [ ] **Step 1: 写入移位和校验红灯测试**

覆盖：`+1`、`-1`、负数算术右移、`INT32_MAX`/`INT32_MIN` 饱和、仅目标通道变化、错误 payload 拒绝和配置越界不部分应用。

- [ ] **Step 2: 运行红灯测试**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media`

Expected: 因 `channelgainprocessor.h` 尚不存在而编译失败。

- [ ] **Step 3: 实现配置、快照和两遍处理**

核心饱和移位：

```cpp
qint32 applyShift(qint32 sample, int shift, quint64 *clipCount)
{
    if (shift < 0) {
        return sample >> -shift;
    }
    const qint64 magnitude = sample < 0 ? -static_cast<qint64>(sample) : static_cast<qint64>(sample);
    const qint64 shiftedMagnitude = magnitude << shift;
    const qint64 widened = sample < 0 ? -shiftedMagnitude : shiftedMagnitude;
    const qint64 saturated = std::clamp(widened,
                                        qint64(std::numeric_limits<qint32>::min()),
                                        qint64(std::numeric_limits<qint32>::max()));
    if (saturated != widened) {
        ++(*clipCount);
    }
    return static_cast<qint32>(saturated);
}
```

处理前必须验证 38400 字节，输出保持相同小端交织格式。

- [ ] **Step 4: 写入 AGC 红灯测试**

构造不同通道峰值，验证：强信号同帧降低移位；低于 35% 未满 500 ms 不增加；满 500 ms 后进入释放；每 250 ms 最多 +1；静音最终 +8；通道 0 的变化不影响通道 1。

- [ ] **Step 5: 实现独立 AGC 并验证**

帧时长按 `SamplesPerFrame * 1'000'000 / sampleRate` 累加。第一遍更新每通道峰值和 AGC，第二遍应用最终移位。

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_media --output-on-failure`

Expected: 全部通过。

### Task 4: 增益配置 JSON 与最近配置

**Files:**
- Create: `src/media/gainprofilestore.h`
- Create: `src/media/gainprofilestore.cpp`
- Modify: `src/app/appsettings.h`
- Modify: `src/app/appsettings.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/media/test_media.cpp`
- Modify: `tests/app/test_app.cpp`

**Interfaces:**
- Produces: `bool GainProfileStore::save(const QString &, const GainConfiguration &, QString *error)`。
- Produces: `std::optional<GainConfiguration> GainProfileStore::load(const QString &, QString *error)`。
- Produces: `GainConfiguration AppSettings::gainConfiguration() const` 和 `saveGainConfiguration(...)`。

- [ ] **Step 1: 写入 JSON 与 QSettings 红灯测试**

覆盖 48 项往返、schema 错误、数组长度错误、非整数 shift、越界、无效比例、无效时间、目标文件不可写，以及任一错误时原配置不变。

- [ ] **Step 2: 运行红灯测试**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media test_app`

Expected: 新接口不存在导致编译失败。

- [ ] **Step 3: 实现统一序列化和原子保存**

`GainProfileStore` 负责 `QJsonObject` 转换和 `QSaveFile::commit()`；`AppSettings` 复用相同验证规则写入数组，禁止维护第二套范围逻辑。

- [ ] **Step 4: 验证任务 4**

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_media|test_app" --output-on-failure`

Expected: 全部通过。

### Task 5: 音频线程、增益后录制与有界队列

**Files:**
- Modify: `src/media/audioprocessor.h`
- Modify: `src/media/audioprocessor.cpp`
- Modify: `src/media/audiorecorder.h`
- Modify: `src/media/audiorecorder.cpp`
- Modify: `src/media/multichannelwavwriter.h`
- Modify: `src/media/multichannelwavwriter.cpp`
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`
- Modify: `tests/media/test_media.cpp`
- Modify: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Consumes: Task 3 的 `ChannelGainProcessor`。
- Produces: `AudioProcessor::adjustedFrameReady(const QByteArray &, quint32, quint32, const GainSnapshot &)`。
- Produces: `AudioProcessor` 的配置槽 `setGainConfiguration(...)`、`setChannelShift(...)`、`setChannelAutomatic(...)`、`setAllAutomatic(...)` 和 `resetGain()`。
- Produces: 录制器同步拒绝非 38400 字节 payload，并限制 `MaxPendingFrames`。

- [ ] **Step 1: 写入增益后分发与零长度队列红灯测试**

验证 `AudioProcessor` 输出的解码幅值和 `adjustedFrameReady` payload 一致；录制器对空 payload 和错误大小第一次调用即返回 false，连续调用不能增加 pending frame/byte 计数。

- [ ] **Step 2: 运行红灯测试**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_media test_ui`

Expected: 录音仍接受空 payload，且没有增益后分发信号。

- [ ] **Step 3: 集成共享增益处理**

`processArrayFrame()` 顺序必须是：校验和增益处理 -> 发出 adjusted frame -> 解码 adjusted payload -> 播放 adjusted PCM -> 发出幅值和完成信号。原始 `HostFrame` 仍只送调试页。

主窗口删除收到设备帧时直接调用 `m_audioRecorder.enqueueFrame(frame.payload)` 的逻辑，改为监听 `adjustedFrameReady` 后录制。

- [ ] **Step 4: 修复录制队列边界和清单元数据**

增加 `MaxPendingFrames` 和 `m_pendingFrames`；只有 payload 恰好 38400 字节时才允许入队。录制设置/清单记录 `gainApplied`、开始与结束配置。

- [ ] **Step 5: 验证任务 5**

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_media|test_ui" --output-on-failure`

Expected: 全部通过。

### Task 6: 6x8 幅值与竖向增益 HMI

**Files:**
- Modify: `src/ui/arrayheatmapwidget.h`
- Modify: `src/ui/arrayheatmapwidget.cpp`
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`
- Modify: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Consumes: `GainSnapshot` 和 `GainConfiguration`。
- Produces signals: `channelShiftChanged(int channel, int shift)`、`channelAutomaticChanged(int channel, bool enabled)`、`setAllAutomaticRequested(bool)`、`resetRequested()`、`saveRequested()`、`loadRequested()`。
- Produces: 每个滑条 object name `channelGainSlider_<channel>`，自动复选框 `channelAutoGain_<channel>`。

- [ ] **Step 1: 写入 48 通道 UI 红灯测试**

断言存在 48 个 `Qt::Vertical` 滑条、范围 `-8~8`、48 个自动复选框；拖动自动通道后发出自动关闭与移位信号；批量自动、手动、复位按钮生效；幅值文本包含当前 bit 和 `A/M`。

- [ ] **Step 2: 运行红灯测试**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui && ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure`

Expected: 找不到新增控件。

- [ ] **Step 3: 重构 ArrayHeatmapWidget**

每个网格项使用固定尺寸复合 QWidget：左侧幅值按钮，右侧竖向 `QSlider` 和自动复选框。工具栏加入全部自动、全部手动、复位、保存、载入；使用稳定最小尺寸和内部滚动区域。

- [ ] **Step 4: 接入工作线程和配置文件对话框**

主窗口把单通道/批量命令 queued 投递给 `AudioProcessor`；保存读取最近快照，载入先由 `GainProfileStore` 完整校验再一次性投递；成功后更新 `QSettings` 和日志。

- [ ] **Step 5: 布局与交互验证**

UI 测试显示 1366x768 主窗口，检查 48 个滑条位于阵列滚动内容中、尺寸非零、互不重叠；检查接口 6、7 不存在。

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure`

Expected: 全部通过。

### Task 7: 协议文档、功能文档与全量验收

**Files:**
- Modify: `refer/音频矩阵通信协议.md`
- Modify: `doc/Readme.md`
- Modify: `docs/superpowers/specs/2026-07-20-video-tail-channel-gain-design.md`（仅在实现事实需要勘误时）

- [ ] **Step 1: 更新协议文档**

视频帧尾表改为两个字节：第一个 `0x0A`，第二个任意；明确视频帧总长增加 1。控制帧仍列八个字段，并标记最后两项由上位机固定填零。

- [ ] **Step 2: 更新功能文档和变更记录**

记录六路控制、48 路移位/AGC、JSON、增益后录制、双重队列上限、模拟服务端双字节视频尾和操作流程。

- [ ] **Step 3: 完整构建和 C++ 测试**

Run: `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug`

Run: `ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure`

Expected: 5/5 测试目标通过。

- [ ] **Step 4: Python 工具验证**

Run: `python -m unittest discover -s tests/tools -p "test_*.py" -v`

Run: `python tools/mock_device_server.py --self-test`

Expected: 全部通过且自检输出 `self-test passed`。

- [ ] **Step 5: 静态差异检查**

Run: `git diff --check`

Expected: 无空白错误；仅允许 Git 的 CRLF 提示。
