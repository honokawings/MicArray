# MicArray Qt6 上位机开发交接摘要

## 使用方式

在新的 Codex 对话中引用本文件，并使用下面的启动语句：

> 请阅读 `doc/SessionHandoff-2026-07-22.md`、`doc/Readme.md` 和 `AGENTS.md`，在当前工作区未提交改动的基础上继续开发。不要回滚或覆盖已有改动；先检查 `git status` 和相关代码，再处理我的新需求。

## 项目目标

MicArray 是一套 Qt 6 Widgets 工业上位机，用于连接设备 PS 端 TCP 服务，接收音频和视频数据、发送心跳和控制帧，并提供实时监控、帧诊断、阵列校准、音频播放与录制、配置持久化和发布打包。

用户偏好与约束：

- 使用中文沟通。
- UI 关键标题和操作使用中文，风格按工业软件标准设计。
- 功能、协议、UI、测试或发布流程发生变化时，必须同步更新 `doc/Readme.md` 及其变更记录。
- 不要回滚工作区中用户或此前任务产生的未提交改动。
- Qt 最低版本保持 6.5，当前主要环境为 Qt 6.11.1、MinGW、Ninja、C++20。

## 当前架构

生产代码位于 `src/`：

- `app/`：设置和日志。
- `control/`：业务控制值。
- `device/`：TCP 生命周期、网络线程、协议线程门面。
- `protocol/`：协议编解码、流式拆包和诊断统计。
- `media/`：48 通道音频解码、增益、播放、录制和视频解码。
- `ui/`：主窗口、热力图、波形、趋势和视频预览。
- `monitoring/`、`diagnostics/`：健康状态和诊断导出。

线程边界：

- `NetworkWorker`：独立网络线程，负责 `QTcpSocket`、心跳和重连。
- `ProtocolWorker`：独立协议线程，负责流式拆包。
- `AudioProcessor`：独立音频线程，负责增益、解码、统计和 `QAudioSink` 播放。
- `VideoDecoder`：独立视频线程，负责 JPEG 解码。
- `AudioRecorder::Worker`：独立录制线程，负责 WAV 和 JSON 清单写盘。
- UI 线程只做状态装配和定时绘制，不应执行 TCP 读取、拆包或媒体解码。

## 当前协议结论

协议文档为 `refer/音频矩阵通信协议.md`，当前实现结论如下：

- 所有多字节字段均按小端序处理。
- 上位机/PS 帧头语义值为 `0x7e7e`。
- Audio：`0xee11`。
- Video：`0xee22`。
- Heartbeat：`0xcc00`。
- Control：`0xcc11`。
- 音频、心跳和控制帧尾严格为两个字节 `0A 0D`。
- 视频帧尾为完整两个字节：第一个字节固定 `0A`，第二个字节可为任意值，但必须存在。
- 视频帧总长为 `16 + payloadLength + 2`。
- 帧调试页保留实际帧尾，并显示帧尾之后的 16 字节。
- 音频和视频候选帧头只接受业务类型 `0xee11`、`0xee22`；payload 中的其他 `7E 7E xxxx` 按伪头处理。

## 音频格式

- 默认采样率：48 kHz。
- 48 通道，每通道一个 32-bit 小端容器。
- 单个 TCP 音频 payload：38400 字节，即 `200 * 48 * 4`。
- 单帧包含 200 个采样点。
- 排列顺序：采样点 0 的通道 0-47，随后采样点 1 的通道 0-47，直到采样点 199。
- 监听输出从任意通道提取 200 个样本并转换为单声道 PCM S16LE。
- 阵列按 6 行 8 列显示，编号从左到右、从上到下为 0-47。

## 已实现主要功能

### 通信与诊断

- 有线、无线和自定义地址连接。
- 自动重连、心跳、连接状态和吞吐统计。
- TCP 半包、粘包、错位恢复和长度保护。
- 网络到协议线程 ACK 背压。
- `received/parsed/discarded/buffered` 等接收链路统计。
- 高级帧调试历史：帧头、类型、长度、时间戳、payload、帧尾、前后字节预览和异常上下文。
- 字节统计使用自适应 `B/KiB/MiB/GiB/TiB` 单位。

### UI

- 主页面名称为“音视频融合”。
- 视频预览、视频分辨率/FPS、音频电平和当前通道滚动波形。
- 6x8 阵列幅值热力图和独立阵列校准页。
- 关键标题和操作已中文化。
- 支持 1366x768 及以上布局。

### 控制

- UI 只显示 6 路业务控制接口。
- 协议层仍编码 8 个 `int16` 字段，末两个字段固定为零，保持 26 字节控制帧兼容。

### 48 路增益与自动增益

- 每通道独立 `-8..+8 bit` 移位增益。
- 正值左移放大，负值右移衰减；左移使用 64-bit 中间值并饱和到 `qint32`。
- 每通道可独立启用连续 AGC。
- 强信号快速衰减，弱信号经过保持时间后平滑逐步放大；静音也可放大到最大移位。
- 手动移动通道滑条会把该通道切回手动模式。
- 幅值显示、监听播放和录音统一使用增益处理后的数据。
- 增益配置通过 QSettings 保存最近状态，也支持严格校验的 JSON 保存、载入和复位。

### 音频录制

- 可选择任意一个或多个通道，写入单个多通道 WAV。
- WAV 使用 `WAVE_FORMAT_EXTENSIBLE`、32-bit 容器和 24-bit 有效位。
- 录制数据使用增益处理后的完整 38400 字节 payload。
- 录制在独立线程执行，队列同时限制为 64 MiB 和 2048 帧。
- 支持完整帧边界分段、不覆盖已有文件、断线停止和 JSON 清单。
- 清单记录通道映射、分段、数据量、有效性、停止原因以及开始/结束增益配置。

### 模拟设备与压测

- 独立脚本：`tools/mock_device_server.py`，不依赖 Qt 上位机代码。
- 默认发送 48 通道、200 采样点、38400 字节音频帧。
- 音频为有意义的复合正弦信号，支持 `gradient/pulse/single-hot/checker`。
- 内置 1920x1080 工业 JPEG 测试图。
- 支持 `--stress-mode sustained|burst` 大数据持续和突发测试。
- 支持 `--self-test`。

常用命令：

```powershell
python tools\mock_device_server.py --self-test
python tools\mock_device_server.py --host 0.0.0.0 --port 9968 --pattern gradient
python tools\mock_device_server.py --stress-mode sustained --target-mbps 100 --duration 60 --stats-interval 1
python tools\mock_device_server.py --stress-mode burst --burst-size-mb 64 --burst-interval 1 --duration 60
```

## 最新严重问题及修复

问题：切换到“音视频融合”页面时音频播放异常，切换到其他页面后恢复正常。

已定位根因：

1. 每个音频帧只有 200 个采样点，48 kHz 下约为 240 帧/秒。
2. `showArrayFrame()` 原先绕过 33 ms 渲染定时器，每帧直接刷新 48 个热力图控件。
3. 每个控件每次刷新都会重设动态样式，页面可见时造成大量样式解析和绘制。
4. 音频处理原先每处理一帧都必须等待 `frameProcessed` 回到 UI，UI 再投递下一帧；UI 卡顿会直接导致 `QAudioSink` 供数不足。

已完成的代码调整：

- `showArrayFrame()` 只缓存最新帧。
- 波形、热力图和校准网格统一在 33 ms 定时器中刷新。
- 页面不可见时跳过对应绘制。
- 热力图背景改用 `QPalette` 更新，仅在选中/峰值边框改变时重设样式表。
- 音频链路允许最多 16 帧在途，并使用容量 32 的有界等待队列，不再逐帧等待 UI 回调。
- 健康监控的音频队列深度包含等待帧和在途帧。

相关文件：

- `src/ui/mainwindow.cpp`
- `src/ui/mainwindow.h`
- `src/ui/arrayheatmapwidget.cpp`
- `src/ui/arrayheatmapwidget.h`
- `tests/ui/test_dashboard.cpp`
- `doc/Readme.md`

新增回归测试：

- 可视化必须等到渲染节拍才更新。
- UI 未收到处理完成回调时允许多帧在途。
- 仅幅值变化时热力图样式表保持稳定。

自动化验证结果：

- Debug 完整构建成功。
- CTest 5/5 通过：protocol、device、media、app、ui。
- `git diff --check` 通过，仅有行尾转换警告。

尚未自动验证：真实声卡和真实设备下的听感。下一步应使用模拟服务端和真实设备分别切换页面复测，观察是否仍有 `QAudioSink` 欠载、音频队列丢帧或 UI 长帧。

## 构建、测试和发布

```powershell
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="<Qt-kit-path>"
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
```

当前常用构建目录：

```text
build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug
build/Desktop_Qt_6_11_1_MinGW_64_bit_Release
```

发布脚本：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-release.ps1
```

发布包不包含 `refer/` 和 `doc/`。

## 当前工作区注意事项

- 当前开发分支此前为 `codex/video-tail-channel-gain`；新对话开始时必须重新运行 `git branch --show-current` 确认。
- 工作区存在大量尚未提交的关联改动，包括协议、控制、音频增益、录音、UI、测试、模拟服务端和文档。
- 存在新增但尚未提交的增益实现文件以及设计/计划文档。
- 不要使用 `git reset --hard`、`git checkout --` 或其他方式回滚这些改动。
- 开始任何修改前先运行 `git status --short`，并阅读相关 diff。

此前确认存在的新增文件包括：

- `src/media/channelgainprocessor.h/.cpp`
- `src/media/gainprofilestore.h/.cpp`
- `docs/superpowers/specs/2026-07-20-video-tail-channel-gain-design.md`
- `docs/superpowers/plans/2026-07-21-video-tail-channel-gain.md`

## 新对话建议先做的检查

1. 阅读 `AGENTS.md`、本文件和 `doc/Readme.md`。
2. 运行 `git status --short` 和 `git diff --check`。
3. 查看用户新需求所涉及模块，不要从旧设计假设重新开始。
4. 若继续处理播放问题，优先采集 `QAudioSink::state/error/bytesFree/processedUSecs`、音频在途数、等待队列深度、输入帧率和主动丢帧数。
5. 修改后补针对性测试，并更新 `doc/Readme.md` 变更记录。

