# MicArray 上位机功能文档

本文档记录 MicArray Qt6 上位机的核心功能、协议假设、模块边界、测试与发布方式。后续每次功能更新、协议调整、UI 调整、测试策略变化或发布流程变化，都必须同步更新本文档的“变更记录”。

## 维护规则

- 每次功能更新必须在“变更记录”追加一条记录。
- 记录格式：日期、类型、影响模块、变更摘要、验证方式。
- 如果改动涉及协议字段、帧格式、默认配置、设备连接方式或发布脚本，必须同步更新对应章节。
- 如果实现与 `refer/音频矩阵通信协议.md` 不一致，必须在本文档写明原因和联调结论。

## 系统定位

MicArray 是面向自适应降噪麦克风阵列定向拾音设备的 Qt6 Widgets 工业上位机。上位机通过 TCP 连接设备 PS 端，接收实时音频/视频数据，发送心跳和控制帧，并提供监控、诊断、阵列校准、日志和发布打包能力。

系统通信链路：

```text
PL / FPGA <-> PS / ARM <-> Qt6 上位机
           DMA/GPIO       TCP
```

PS 作为 TCP 服务端，上位机作为 TCP client。协议文档说明 PS 只支持单 client 连接。

## 默认连接参数

| 连接方式 | IP | 端口 |
|---|---:|---:|
| 有线 | `192.168.2.10` | `9968` |
| 无线 | `192.168.5.1` | `9968` |

UI 支持有线、无线和自定义 IP/端口。

## 音频阵列格式

当前音频帧采用 48 通道阵列格式：

- 阵列芯片：MMICT5848。
- 采样率：默认 `48 kHz`。
- 阵列输入通道数：`48`。
- 芯片输出：24 bit I2S。
- FPGA 传输格式：每通道按 32 bit 小端样本传输。
- 单个采样点：`48 * 4 = 192` 字节。
- 单个 TCP 音频帧包含 `200` 个采样点。
- 单帧 payload：`48 * 4 * 200 = 38400` 字节，即 `0x9600`。
- 音频数据排列方式：采样点 0 的通道 0-47，随后采样点 1 的通道 0-47，依次到采样点 199；每个通道样本为 32 bit 小端。

如果联调确认 FPGA 实际发送小端格式，只允许在 `src/media/audioframedecoder.*` 集中调整，不应把端序逻辑分散到 UI 或设备层。

## 核心模块

### 应用基础设施

路径：

- `src/app/appsettings.h`
- `src/app/appsettings.cpp`
- `src/app/logger.h`
- `src/app/logger.cpp`

功能：

- 使用 `QSettings` 保存连接参数、媒体参数、输出通道、平均窗口和自动重连配置。
- 使用文件日志记录关键运行事件。
- 日志按日期写入系统应用数据目录。
- 日志事件通过 Qt signal 同步显示到 UI 日志面板。

### 协议层

路径：

- `src/protocol/protocolcodec.h`
- `src/protocol/protocolcodec.cpp`

功能：

- 编码上位机心跳帧。
- 编码上位机 8 路控制帧。
- 流式解析设备上传的音频帧和视频帧。
- 支持 TCP 半包、粘包、错位帧头恢复。
- 只要帧头、payload 长度和帧尾完整合法，即使帧类型不是当前业务支持类型，也会保留为结构化帧供调试页显示。
- 解析错误以结构化错误返回，避免阻塞 UI。

当前帧类型：

| 类型 | 数值 | 方向 | 用途 |
|---|---:|---|---|
| Audio | `0xee11` | 设备 -> 上位机 | 音频数据帧 |
| Video | `0xee22` | 设备 -> 上位机 | 视频数据帧 |
| Heartbeat | `0xcc00` | 上位机 -> 设备 | 心跳帧 |
| Control | `0xcc11` | 上位机 -> 设备 | 控制帧 |
| PlPs | `0xeeff` | PL <-> PS | PL/PS 固定帧 |

协议层字段端序：

- 所有帧协议多字节字段统一按小端解析和编码。
- 帧头语义值：`0x7e7e`，帧内字节为 `7e 7e`。
- 帧类型：小端，例如 `0xee11` 在帧内为 `11 ee`。
- payload 长度：小端 32 bit，例如 `192` 在帧内为 `c0 00 00 00`。
- 时间戳字段：小端 32 bit。
- 控制帧 8 路 `int16` 控制值：小端。
- 音频帧、心跳帧、控制帧帧尾语义值：`0x0d0a`，帧内字节为 `0a 0d`。
- 视频帧帧尾为单字节 `0a`。
- 当帧尾不匹配时，协议日志会显示实际读到的无效帧尾值和期望值。

### 音频解码与校准

路径：

- `src/media/audioframedecoder.h`
- `src/media/audioframedecoder.cpp`
- `src/media/audioprocessor.h`
- `src/media/audioprocessor.cpp`

功能：

- 将 38400 字节 payload 解析为 200 个采样点、每点 48 通道样本。
- 支持选择 0-47 任一通道作为监听输出。
- 将选中通道的 200 个采样点转换为 `S16LE / mono` 供 `QAudioSink` 播放。
- 统计每通道瞬时幅值。
- 按可配置帧数窗口计算每通道平均幅值。
- 以最高平均幅值为基准做归一化，用于阵列校准热力图。
- 记录最大平均幅值通道、最小平均幅值通道和输出通道。

### 控制层

路径：

- `src/control/controlvalues.h`
- `src/control/controlvalues.cpp`

功能：

- 管理 8 路 `qint16` 控制值。
- 通道 0-5 为有效控制通道。
- 通道 6-7 为预留通道，UI 默认禁用。
- 控制帧发送前由 UI 收集当前控制值，再交给协议层编码。

### 设备通信层

路径：

- `src/device/deviceclient.h`
- `src/device/deviceclient.cpp`

功能：

- 使用 `QTcpSocket` 连接设备。
- 支持手动连接、断开和自动重连。
- 连接成功后每 1 秒发送心跳帧。
- 收到 TCP 数据后交给协议层解析。
- 音频帧、视频帧通过 Qt signal 分发到媒体层。
- 所有结构完整的解析帧都会通过通用调试信号分发到帧调试页；未知类型帧只进入调试显示，不进入音频/视频解码。
- 维护状态机、收包时间、字节计数、帧计数和解析错误计数。

状态机：

```text
Disconnected -> Connecting -> Connected
Connected -> Disconnected
Connected -> Reconnecting -> Connecting
Connecting -> Error
```

### 视频层

路径：

- `src/media/videodecoder.h`
- `src/media/videodecoder.cpp`

功能：

- 默认按 JPEG payload 解码。
- 使用 `QImage::loadFromData(payload, "JPG")`。
- 统计成功解码帧数、失败帧数和估算 FPS。
- 解码失败不会断开设备连接，只写入日志并保留运行状态。

### UI / HMI

路径：

- `src/ui/mainwindow.h`
- `src/ui/mainwindow.cpp`

主界面采用 Qt Widgets 代码构建，当前不依赖 `.ui` 文件。

当前页面：

- `Monitor`：连接、控制、媒体设置、视频预览、音频电平、诊断和日志。
- 实时监控页的视频预览下方显示当前播放通道的滚动音频波形，用于快速确认选中通道是否有信号、是否削顶或静音。
- `Frame Debug`：显示最近解析帧的元信息、前 16 字节和尾 16 字节；支持自定义历史保留帧数，并用分段动效展示帧头、类型、长度、时间戳、payload 和帧尾；原始字节预览和历史记录使用等宽字体，便于对齐查看。
- 帧调试页包含“解析异常诊断”，当帧尾、长度或帧结构异常时显示帧类型、payload 长度、计算帧尾偏移、缓冲区大小、实际帧尾、期望帧尾、头 16 字节和帧尾附近上下文。
- `Array Calibration`：显示 6x8 阵列热力图，通道从左到右、从上到下编号 0-47。

阵列校准显示规则：

- 每个阵元显示通道编号和归一化百分比。
- 归一化基准为当前平均窗口内最高平均幅值。
- 颜色从低幅值冷色到高幅值暖色映射。
- 平均窗口单位为帧，可在 UI 中配置。

交互规则：

- 未连接时禁止发送控制帧。
- 控制值修改后，发送按钮显示待发送状态。
- 断线后保留当前控制值和连接参数。
- 连接配置和媒体配置会写入 `QSettings`。
- 输出通道变化后立即影响后续监听输出。
- 平均窗口变化后立即影响后续校准热力图。

## 发布脚本

路径：

- `scripts/package-release.ps1`
- `scripts/Readme.md`

默认命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-release.ps1
```

脚本功能：

- 配置 Release 构建目录。
- 构建 `MicArray.exe`。
- 构建并运行测试。
- 调用 `windeployqt` 收集 Qt 运行库和插件。
- 生成 `RELEASE.txt`。
- 输出发布目录和 zip 包到 `dist/`。

发布包不包含 `refer/` 和 `doc/` 目录，现场包只保留可执行程序、Qt 运行库/插件和 `RELEASE.txt`。

常用参数：

```powershell
.\scripts\package-release.ps1 -SkipTests
.\scripts\package-release.ps1 -NoZip
.\scripts\package-release.ps1 -CMakePrefixPath "D:\Application\ProgramingIDE\Qt6\6.11.1\mingw_64"
```

## 测试

测试目录：

- `tests/protocol/test_protocolcodec.cpp`
- `tests/device/test_deviceclient.cpp`
- `tests/media/test_media.cpp`

测试覆盖：

- 协议心跳帧编码。
- 协议控制帧编码。
- 音频/视频帧解析。
- TCP 半包、粘包和错误恢复。
- 模拟 TCP 服务端连接、心跳和收帧。
- 未连接时控制帧发送失败。
- JPEG 解码和坏帧错误。
- 48 通道、200 采样点、38400 字节音频 payload 解码。
- 选中通道监听 PCM 转换。
- 阵列平均幅值窗口和归一化计算。
- 音频输出格式配置。

验证命令：

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
```

## 模拟服务端

独立测试程序路径：

- `tools/mock_device_server.py`

该程序是纯 Python 标准库命令行脚本，不加入 CMake，不链接 Qt，也不复用上位机 C++ 模块。它模拟 PS TCP 服务端，一次只接受一个 client，用于验证上位机连接、心跳、控制帧、48 通道音频解析、帧调试页和阵列校准页。

快速启动：

```powershell
python tools\mock_device_server.py --host 0.0.0.0 --port 9968 --pattern gradient
```

上位机测试连接：

- 连接方式选择“自定义”。
- IP 填写 `127.0.0.1`。
- 端口填写 `9968`。

自检命令：

```powershell
python tools\mock_device_server.py --self-test
```

常用参数：

```powershell
python tools\mock_device_server.py --pattern gradient --audio-fps 20
python tools\mock_device_server.py --pattern single-hot --hot-channel 17 --no-video
python tools\mock_device_server.py --pattern checker --audio-fps 10 --jitter-ms 20 --verbose
python tools\mock_device_server.py --video-fps 1
```

模拟音频格式：

- 帧类型：`0xee11`。
- payload 固定 `38400` 字节，即 `0x9600`。
- 单帧包含 `200` 个采样点。
- 每个采样点包含 48 通道，每通道 4 字节。
- 数据排列方式为通道 0-47 依次排列，并按采样点重复 200 次。
- 每通道样本使用 32 bit 小端格式。
- 默认发送可听、可观察的有符号正弦复合信号；不同通道带有轻微频率和相位差，幅值模式仍由 `gradient`、`pulse`、`single-hot`、`checker` 控制。
- 默认 `--audio-fps 240`，对应 `200` 采样点/帧下约 `48 kHz` 的模拟采样节奏；需要降低联调压力时可手动减小。
- 默认 `gradient` 模式让通道编号越大平均幅值越高，便于观察 6x8 阵列热力图归一化效果。
- `pulse` 和 `single-hot` 可用于验证指定通道监听、峰值统计和热力图响应。
- `checker` 可用于验证阵列通道编号和颜色映射是否错位。

模拟服务端会解析并打印上位机发来的：

- `0xcc00` 心跳帧。
- `0xcc11` 控制帧，包括 8 路 `int16` 控制值和原始 hex。

建议验证点：

- 连接状态应变为已连接。
- 服务端控制台应每秒收到心跳。
- 上位机发送控制帧后，服务端应打印 8 路控制值。
- 帧调试页应显示最近音频帧的前 16 字节和尾 16 字节。
- 阵列校准页在 `gradient` 模式下应显示稳定的 6x8 幅值梯度。
- 切换监听输出通道后，音频统计和监听通道应随之变化。

## 参考资料

- `refer/音频矩阵通信协议.md`
- `refer/TCP_audio+video.vi`

协议文档是第一可信来源。LabVIEW VI 是二进制文件，只作为功能线索参考。

## 变更记录

| 日期 | 类型 | 影响模块 | 变更摘要 | 验证方式 |
|---|---|---|---|---|
| 2026-07-06 | 协议修正 | 协议、UI、测试工具、文档 | 视频帧接收尾部改为单字节 `0A` 判定；音频帧、心跳帧和控制帧仍按 `0A 0D` 判定；帧调试页和模拟服务端同步按帧类型显示/生成不同帧尾。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure`、`python tools\mock_device_server.py --self-test` |
| 2026-07-06 | 缺陷修复 | 音频、UI | 修复当前播放通道波形显示近似静止的问题：PCM16 转换改为适配 24-bit 量级样本，波形控件按当前显示窗口峰值自适应缩放。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure` |
| 2026-07-06 | 测试工具更新 | tools、文档 | 模拟服务端音频 payload 改为有意义的有符号正弦复合音频信号，支持播放通道波形和音频电平测试；默认音频帧率调整为 240 FPS，对齐 200 采样点/帧下的 48 kHz 节奏。 | `python tools\mock_device_server.py --self-test`、`python -m py_compile tools\mock_device_server.py` |
| 2026-07-06 | 诊断/UI 更新 | 协议、设备通信、UI | 帧调试页新增解析异常诊断，显示帧类型、payload 长度、尾偏移、缓冲区大小、实际/期望帧尾、头 16 字节和尾部上下文；实时监控页在视频下方新增当前播放通道滚动音频波形。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure` |
| 2026-07-06 | 音频协议更新 | 音频、UI、测试工具、文档 | 音频 payload 从 192 字节单采样点升级为 38400 字节，包含 200 个采样点；每个采样点按通道 0-47 交织排列，每通道为 32 bit 小端样本；监听输出改为输出选中通道的 200 个 PCM16 样本。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure`、`python tools\mock_device_server.py --self-test` |
| 2026-07-06 | 协议更新 | 协议、设备通信、测试工具、UI | 帧协议所有多字节字段统一改为小端解析/编码，包括帧头、帧类型、payload 长度、时间戳、控制值和帧尾；非法帧尾告警增加实际帧尾值显示；模拟服务端同步全小端协议。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure`、`python tools\mock_device_server.py --self-test` |
| 2026-07-05 | 协议/发布更新 | 协议、设备通信、测试工具、发布 | 帧类型和 payload 长度字段改为小端解析/编码，时间戳和样本解析保持原约定；模拟服务端同步小端类型/长度；发布脚本停止打包 `refer/` 与 `doc/` 目录。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure`、`python tools\mock_device_server.py --self-test` |
| 2026-07-05 | UI 更新 | UI、文档 | 帧调试页的前 16 字节、尾 16 字节和解析帧历史窗口改用等宽字体，提升十六进制字节对齐可读性。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure` |
| 2026-07-05 | 功能更新 | 协议、设备通信、UI | 帧调试页支持显示结构完整但类型未知的解析帧；新增可配置历史帧数量，历史记录展示每帧前 16 字节和后 16 字节；新增帧结构分段动效。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure` |
| 2026-07-05 | 缺陷修复 | tools、视频测试 | 修复模拟服务端内置 JPEG payload 无法被上位机识别的问题，改为使用 `cjpeg` 生成的 2x2 合法 JPEG 测试帧，并在脚本自检中增加 JPEG SOI/EOI 校验。 | `python tools\mock_device_server.py --self-test`、`djpeg` 解码验证 |
| 2026-07-05 | 测试工具 | tools、文档 | 新增独立 Python 模拟设备 TCP 服务端，支持发送 48 通道音频帧、可选 JPEG 视频帧，接收并打印心跳帧和 8 路控制帧；补充启动、自检和联调说明。 | `python tools\mock_device_server.py --self-test` |
| 2026-07-05 | UI 更新 | UI、配置 | 关键界面标题和操作按钮调整为中文；采样率控件改为以 kHz 显示，默认显示 `48 kHz`，内部仍按 `48000 Hz` 保存。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure` |
| 2026-07-05 | 功能更新 | 音频、UI、发布 | 音频默认改为 48 kHz 48 通道阵列输入；新增 192 字节 payload 解码、输出通道选择、帧调试页、6x8 阵列校准热力图和平均窗口配置。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure` |
| 2026-07-04 | 初始文档 | 全部 | 创建核心功能文档，记录当前 Qt6 上位机架构、协议、设备通信、媒体、UI、测试和发布脚本。 | 文档审阅 |
