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
- 编码上位机控制帧：业务层提供 6 路控制值，线协议末尾两个 `int16` 保留字段固定填零。
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
- 控制帧 8 个 `int16` 线字段：小端；接口 0-5 来自业务层，字段 6-7 固定为零。
- 音频帧、心跳帧、控制帧帧尾语义值：`0x0d0a`，帧内字节为 `0a 0d`。
- 视频帧总长度为 `16 + payloadLength + 2`；帧尾第一个字节固定为 `0a`，第二个字节可为任意 `0x00~0xff` 值但必须存在。解析器仅校验第一个字节，收到完整两个尾字节后才提交帧。
- 视频帧诊断和 `rawFrame` 保留两个实际尾字节，帧尾后预览从第二个尾字节之后开始；视频尾错误期望值显示为 `0A ??`。音频、心跳和控制帧仍严格要求 `0a 0d`，例如音频 `0a 00` 会被拒绝。
- 当帧尾不匹配时，协议日志会显示实际读到的无效帧尾值和期望值。

### 音频解码与校准

路径：

- `src/media/audioframedecoder.h`
- `src/media/audioframedecoder.cpp`
- `src/media/audioprocessor.h`
- `src/media/audioprocessor.cpp`
- `src/media/channelgainprocessor.h`
- `src/media/channelgainprocessor.cpp`
- `src/media/gainprofilestore.h`
- `src/media/gainprofilestore.cpp`

功能：

- 将 38400 字节 payload 解析为 200 个采样点、每点 48 通道样本。
- 支持选择 0-47 任一通道作为监听输出。
- 将选中通道的 200 个采样点转换为 `S16LE / mono` 供 `QAudioSink` 播放。
- 每个通道具有独立的 `-8..+8 bit` 移位增益；右移用于衰减，左移使用 64-bit 中间值并饱和到 `qint32`，记录各通道累计削顶次数。
- 每通道可独立启用连续自动增益：输出峰值高于 80% 时同帧快速衰减；低于 35% 持续 500 ms 后开始放大，之后每 250 ms 最多增加 1 bit，静音也可逐步放大到 `+8 bit`。
- 自动增益通道发生手动滑条操作时立即切换为手动模式；通道之间的增益状态和计时互不影响。
- 增益处理后的同一份 38400 字节 payload 同时用于幅值统计、监听播放和音频录制，避免显示、播放与文件数据不一致。
- 增益配置可保存为严格校验的 JSON，包含 48 路 shift、48 路自动模式和 AGC 参数；写入使用 `QSaveFile` 原子替换，最近配置由 `QSettings` 恢复。
- 统计每通道瞬时幅值。
- 按可配置帧数窗口计算每通道平均幅值。
- 以最高平均幅值为基准做归一化，用于阵列校准热力图。
- 记录最大平均幅值通道、最小平均幅值通道和输出通道。
- `AudioProcessor` 运行在独立音频处理线程，音频 payload 解码、平均幅值统计、监听通道 PCM 转换和可选播放不占用 UI 线程。UI 侧允许最多 16 帧连续投递到音频线程，并保留 32 帧有界等待队列，音频处理不再逐帧等待 UI 完成回调。
- UI 只缓存最新的 `DecodedAudioFrame`，波形、阵列热力图和校准网格统一由 33 ms 定时器刷新；页面不可见时跳过对应控件绘制，避免 240 FPS 音频输入触发高频样式计算和绘制。

### 多通道音频录制

路径：

- `src/media/audiorecorder.h`
- `src/media/audiorecorder.cpp`
- `src/media/multichannelwavwriter.h`
- `src/media/multichannelwavwriter.cpp`

功能与文件约定：

- 可从 48 路阵列中任意选择一个或多个通道，按物理通道编号升序写入单个多通道 WAV 文件。
- 录制只接收 `AudioProcessor` 确认并完成增益处理后的完整音频 payload，不依赖播放通道、音频播放是否启用或 UI 波形刷新；在音频处理队列入口被主动丢弃的帧不会进入录音。
- WAV 使用 `WAVE_FORMAT_EXTENSIBLE`，每通道为 32-bit 容器、24-bit 有效位，采样率使用开始录制时的音频设置。
- 每个音频协议帧的 200 个采样点作为不可拆分单元写入；文件接近 RIFF 32-bit 大小上限时自动从完整帧边界分段，后续文件使用 `_part002.wav` 等后缀。
- 同目录生成 JSON 清单，记录录制通道与 WAV 通道位置映射、分段文件、样本帧数、数据量、有效性、停止原因，以及 `gainApplied`、开始/结束时的 48 路增益与 AGC 参数。
- 录制文件与清单均采用不覆盖策略；同名目标已存在时开始录制失败，避免覆盖现场数据。
- `AudioRecorder` 在专用工作线程串行写盘，UI 线程只执行有界入队；非 38400 字节 payload 在排队前同步拒绝。待写队列同时限制为 `64 MiB` 和 2048 帧，任一上限触发或写盘失败都会进入错误状态并停止接受后续帧。
- 用户停止、设备断线和应用退出都会按顺序完成已接受帧、更新 WAV 头和 JSON 清单；异常或无效分段会在清单中明确标记。

### 离线音频分析与声源定位

路径：

- `src/analysis/`
- `src/ui/audioanalysispage.*`
- `src/ui/directionphotowidget.*`

“音频分析”页可打开录制生成的多通道 WAV，选择任一通道进行离线播放，并按播放位置约每 `50 ms` 更新一次定位结果；暂停后拖动时间轴也会在选定位置重新分析。WAV 读取支持 PCM 与 WAVE_FORMAT_EXTENSIBLE 中的 PCM 子格式，以及 16/24/32 bit 整数样本。非 48 通道文件仍可单通道播放，但会明确禁用阵列定位。

默认定位器为 GCC-PHAT 时延估计加鲁棒最小二乘方向求解：

- 麦克风阵列为 6 行 × 8 列、相邻间距 `0.05 m`，坐标原点位于阵列中心。
- 坐标系采用 `+X` 向右、`+Y` 向上、`+Z` 向前。
- 方位角 `0°` 指向正前方，向右为正；俯仰角向上为正，仅求解阵列正面半球。
- 当前输出一个主声源方向，结果接口使用向量，为后续多声源算法保留扩展能力。
- 分析窗口、更新周期、声速、平滑系数和置信度阈值均可配置并持久化。

定位算法采用源码级插件接口。自定义算法实现 `src/analysis/isoundsourcelocator.h` 中的 `ISoundSourceLocator`，提供稳定的算法 ID、说明、`configure()` 和 `locate()`；随后在 `registerBuiltInSoundSourceLocators()` 或项目自定义注册入口向 `SoundSourceLocatorRegistry` 注册工厂，即可出现在算法选择框中。接口接收阵列几何、采样率和多通道浮点窗口，不依赖 UI，也不承诺动态库 ABI。

定位方向可叠加到用户选择的静态照片。照片使用针孔相机投影，可配置画面中心方位角、中心俯仰角、水平/垂直视场角和滚转角；视场内显示十字标记，视场外但仍位于正面半球的方向显示边缘箭头。照片保持原始宽高比，不参与算法计算。

离线 WAV 读取、播放和定位运行在独立 `OfflineAudioAnalysisThread`。切换进入“音频分析”页会关闭“音视频融合”页的实时选中通道播放，但不会断开设备、停止视频、控制、增益或录音；离开分析页会停止离线播放，也不会自动恢复实时通道播放。

### 实时声源定位与视频校准

路径：

- `src/analysis/livelocalizationworker.*`
- `src/analysis/ispatialresponseprovider.h`
- `src/analysis/cameraviewcalibrator.*`
- `src/ui/livelocalizationpage.*`
- `src/ui/livelocalizationview.*`

“声源定位”页复用设备实时视频和增益后的 48 通道阵列音频。`LiveLocalizationWorker` 在独立 `LiveSourceLocalizationThread` 中连续累积 200 样本协议帧，默认以 2048 样本窗口、约 10 Hz 更新主声源方向；实时分析旁路使用独立的 32 帧有界队列，计算过载时丢弃旁路新帧，不阻塞现有音频播放、录音、视频解码或 UI。

页面提供两种显示方式：

- 纯标记：把 GCC-PHAT 主声源方位角、俯仰角和置信度投影为视频准星。
- 真实空间响应热力图：按当前视频视角反投影约 64×36 个方向，使用 SRP-PHAT 理论阵元时延累加真实互相关响应，再以半透明色阶覆盖视频。

热力图方向网格的宽度和高度可在页面中独立调整，默认 `64×36`，宽度范围为 `16..640`、高度范围为 `9..360`，修改后立即重建并自动保存。更高分辨率会显著增加 SRP-PHAT 计算量，但分析仍由独立线程和有界音频旁路隔离，不会阻塞实时播放、录音或视频。

GCC-PHAT 的实时热力图使用高性能纯 CPU 路径：122 个麦克风对按相同阵元间距聚合为 4 类基线，在频域累加 PHAT 互谱后只生成 4 条聚合互相关；定位阶段生成的聚合结果由同一音频窗口的热力图直接复用。视频视角或网格变化时会预计算全部方向的环形相关索引与插值系数，逐帧扫描不再计算三角函数，并由专用 Qt 线程池按连续行块并行执行，保留一个逻辑核心给主音视频链路。

当前开发机 Release 实测中，`640×360` 网格的“定位 + 热力图”连续五帧中位耗时由优化前单独热力图约 254 ms 降至约 70 ms，其中复用定位缓存后的热力图阶段约 1 ms，达到约 14 FPS。实际结果会随 CPU、采样窗口和系统负载变化。

空间响应采用可选源码级接口 `ISpatialResponseProvider`。只实现 `ISoundSourceLocator` 的自定义算法仍可用于纯标记；同时实现空间响应接口后即可启用真实热力图，不需要修改页面。

实时视频视角参数与离线静态照片设置相互独立，但使用相同的 `CameraViewSettings`：中心方位角、中心俯仰角、水平视场角、垂直视场角和滚转角均可手动调整，修改后立即更新叠加并自动保存。

四点自动校准要求用户依次把稳定声源放在视频中的四个分散位置并点击声源。每次点击会对最近约一秒的可靠定位结果进行单位方向加权平均；样本不足、置信度低、方向波动大或与已有点过近时拒绝记录。四点完成后通过受限多起点阻尼最小二乘拟合五个物理视角参数，显示点击点、预测点和 RMS 像素误差；拟合失败不会覆盖旧设置。归一化点击坐标和校准结果自动保存，下次启动自动载入，不因视频分辨率或宽高比变化提示重新校准。

进入和离开“声源定位”页不会修改实时选中通道播放状态，也不会改变设备连接、视频、录音、增益或控制状态。

### 控制层

路径：

- `src/control/controlvalues.h`
- `src/control/controlvalues.cpp`

功能：

- 管理 6 路 `qint16` 有效控制值，UI 不显示接口 6、7。
- 协议编码器在 6 路业务值后追加两个零值，保持设备要求的 8 字段、26 字节控制帧兼容。
- 控制帧发送前由 UI 收集当前控制值，再交给协议层编码。

### 设备通信层

路径：

- `src/device/deviceclient.h`
- `src/device/deviceclient.cpp`
- `src/device/networkworker.h`
- `src/device/networkworker.cpp`
- `src/protocol/protocolworker.h`
- `src/protocol/protocolworker.cpp`

功能：

- `DeviceClient` 作为 UI 线程门面，不再直接持有 `QTcpSocket`。
- `NetworkWorker` 在专用 `QThread` 中创建并持有 `QTcpSocket`、心跳定时器和重连定时器，负责连接、断开、读写和心跳。
- `NetworkWorker` 将 `QTcpSocket` 读缓冲限制为 `8 MB`，在接收吞吐异常或协议线程处理不及时的情况下，通过 Qt/系统 TCP 流控限制应用侧无界内存增长。
- `ProtocolWorker` 在专用 `QThread` 中持有 `ProtocolCodec`，负责 TCP 字节流解析和帧分发。
- 网络线程到协议线程之间采用 ACK 背压：网络线程累计已投递但协议线程未确认的字节数，超过 `4 MB` 时暂停读取 socket，降到 `2 MB` 以下后恢复读取，避免 queued `QByteArray` 无界堆积。
- 支持手动连接、断开和自动重连。
- 连接成功后每 1 秒发送心跳帧。
- 收到 TCP 数据后，网络线程只执行 `readAll()` 并通过 queued signal 投递给协议线程，避免 UI、音频或视频处理阻塞 TCP 接收。
- 音频帧、视频帧通过 Qt signal 分发到媒体层。
- 所有结构完整的解析帧都会通过通用调试信号分发到帧调试页；未知类型帧只进入调试显示，不进入音频/视频解码。
- 维护状态机、收包时间、字节计数、帧计数和解析错误计数。
- 维护协议解析链路指标：累计接收字节、累计成功解析字节、累计主动丢弃字节和当前协议缓冲区残留字节，用于定位 TCP 吞吐、帧错位和协议字段异常。
- 手动断开和对象析构会同步投递到网络线程执行断开请求，避免退出线程时遗留未处理的 socket 断开操作。

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
- `VideoDecoder` 运行在独立视频处理线程，JPEG 解码与 FPS 统计不占用 UI 线程。
- UI 只缓存最新一帧图像，并以约 33 ms 的周期刷新预览，视频帧堆积时优先显示最新状态。

### UI / HMI

路径：

- `src/ui/mainwindow.h`
- `src/ui/mainwindow.cpp`

主界面采用 Qt Widgets 代码构建，当前不依赖 `.ui` 文件。

当前页面：

- `音视频融合`：连接、6 路控制、媒体设置、视频预览、音频电平、阵列幅值与增益、诊断和日志。
- 实时监控页的视频预览下方显示当前播放通道的滚动音频波形，用于快速确认选中通道是否有信号、是否削顶或静音。
- `音视频融合 -> 阵列幅值` 以 6x8 顺序显示 48 个通道；每个通道左侧是增益后归一化幅值，右侧是竖向移位滑条和独立自动增益复选框。工具栏提供全部自动、全部手动、复位、保存和载入。
- `音频录制`：以 6x8 通道矩阵选择一个或多个通道，支持全选、清空、反选和同步当前监听通道，显示录制状态、时长、写入量、速率、待写队列、帧数、丢弃帧和清单路径。
- `高级帧调试`：显示最近解析帧的元信息、前 16 字节、尾 16 字节和帧尾之后的实际 16 字节；跨 TCP chunk 时等待后续数据补齐，连接清理时以“不完整”标记未满 16 字节的后续预览。页面支持自定义历史保留帧数，并用分段动效展示帧头、类型、长度、时间戳、payload 和帧尾；原始字节预览和历史记录使用等宽字体，便于对齐查看。
- 帧调试页包含“解析异常诊断”，当帧尾、长度或帧结构异常时显示帧类型、payload 长度、计算帧尾偏移、缓冲区大小、实际帧尾、期望帧尾、头 16 字节和帧尾附近上下文。
- 实时诊断区域显示协议链路累计 `received/parsed/discarded/buffered` 指标；正常稳定链路下 parsed 应持续接近 received，discarded 不应持续增长，buffered 应只在半包期间短暂波动。
- 实时诊断区域显示接收背压状态和待协议线程确认的字节数；背压短时触发表示应用正在自我保护，长期保持启用则说明协议解析或后续处理吞吐不足，需要降低设备帧率或继续优化处理链路。
- `阵列校准`：显示 6x8 阵列热力图，通道从左到右、从上到下编号 0-47。

字节统计显示规则：

- 面向操作人员的累计字节数、队列大小、写入量和传输速率使用 `B/KiB/MiB/GiB/TiB` 自适应显示，速率追加 `/s`。
- 协议字段中的 payload 长度、帧大小、偏移和错误上下文保持精确字节数，不做单位换算，便于逐字节联调。

阵列校准显示规则：

- 每个阵元显示通道编号和归一化百分比。
- 阵列幅值使用已经应用各通道移位增益的数据；通道文本同时显示当前 bit 值和自动/手动模式。
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
- 单通道增益和自动模式变更通过 queued functor 投递到音频线程；AGC 只在状态实际变化时刷新 48 路增益控件。
- 增益复位、JSON 保存/载入和最近配置恢复均为完整 48 路操作；载入失败时保持当前配置不变。
- 视频预览、音频波形、阵列热力图和校准网格采用 UI 线程定时渲染，当前周期内多次到达的数据会合并为最新状态；热力图幅值颜色使用调色板更新，只有选中/峰值边框变化时才重设样式表。
- 网络接收、协议解析、音频处理、视频解码和 UI 绘图分别由不同线程/职责承担，UI 不直接执行 TCP 读取、流式拆包或媒体解码。
- 录制期间锁定通道选择、采样率、保存目录和恢复默认设置，防止运行中改变 WAV 文件结构；监听通道仍可独立切换。
- 未连接、未选择录制通道或保存目录不可写时禁用开始录制；设备断线会自动停止当前录制并保留已成功写入的数据。

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
- `tests/ui/test_mainwindow.cpp`

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
- 多通道 WAV 头、通道交织、自动分段、JSON 清单、不覆盖策略和文件写入异常。
- 录制工作线程的开始/停止顺序、有界队列、队列溢出、格式错误和启动失败处理。
- 自适应字节单位、录制页面交互、持久化默认通道和 1366x768 布局。
- 帧尾后 16 字节在同一 chunk、跨 chunk 和连接清理场景下的调试预览。

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

视频测试资源：

- `tools/assets/mock_video_1920x1080.jpg` 是随模拟服务端提供的 `1920x1080` 工业测试图，包含彩色条、灰阶、网格和分辨率标识，用于观察上位机的视频解码、缩放和显示。
- `tools/generate_mock_video_pattern.ps1` 用于重新生成该测试图；日常运行不需要执行该脚本。
- 服务端从 `mock_device_server.py` 所在目录相对定位并一次性读取该资源，将其作为不透明字节缓存和发送，因此启动位置不影响资源查找。
- 模拟服务端的视频帧在 payload 后追加 `0A 00`；接收端接受首字节为 `0A` 的任意完整双字节视频帧尾。
- 连接模拟服务端后，实时监控页应显示视频分辨率 `1920x1080`，预览保持原始宽高比。
- Python 模拟服务端不解析或验证 JPEG 编码内容；JPEG 是否可解码由 Qt 上位机的 `QImage::loadFromData` 在接收端确认。

常用参数：

```powershell
python tools\mock_device_server.py --pattern gradient --audio-fps 20
python tools\mock_device_server.py --pattern single-hot --hot-channel 17 --no-video
python tools\mock_device_server.py --pattern checker --audio-fps 10 --jitter-ms 20 --verbose
python tools\mock_device_server.py --video-fps 1
```

压力/吞吐模式示例：

```powershell
python tools\mock_device_server.py --stress-mode normal --duration 60 --audio-fps 240 --video-fps 1
python tools\mock_device_server.py --stress-mode sustained --duration 60
python tools\mock_device_server.py --stress-mode sustained --target-mbps 100 --duration 60 --stats-interval 1
python tools\mock_device_server.py --stress-mode burst --burst-size-mb 64 --burst-interval 1 --duration 60 --no-video
```

- `normal` 模式保持既有 `--audio-fps` / `--video-fps` 的 FPS 调度节奏，只是在指定时长内补充统计输出，不主动改写原有发送节拍。
- `sustained` 模式在 `--target-mbps 0`（默认）时表示不限速持续发送；上例第二条命令用于连续 60 秒的无限速发送压测。
- `sustained` 模式在设置 `--target-mbps 100` 后，会按十进制 `100 Mb/s` 目标做速率整形；上例第三条命令用于连续 60 秒的 100 Mb/s 压测。
- `burst` 模式按 `--burst-size-mb` 和 `--burst-interval` 发送音频突发；上例第四条命令表示每秒发送 1 次、单次目标 `64 MiB` 的突发流量并持续 60 秒。

压力参数说明：

| 参数 | 说明 |
|---|---|
| `--stress-mode` | 默认值：`normal`。压力模式：`normal` 维持原始 FPS 节奏，`sustained` 追求持续吞吐，`burst` 发送周期性突发。 |
| `--duration` | 默认值：`0`。测试持续时间，单位秒；`0` 表示不限时，通常建议压测显式给出持续时长。 |
| `--target-mbps` | 默认值：`0`。仅 `sustained` 模式使用的目标吞吐，单位十进制 `Mb/s`；`0` 表示不限速。 |
| `--stats-interval` | 默认值：`1.0`。周期统计输出间隔，单位秒，用于打印阶段性吞吐、帧率和阻塞信息。 |
| `--burst-size-mb` | 默认值：`32.0`。`burst` 模式单次突发目标大小，单位 `MiB`；实现按完整音频帧向上取整，避免发送截断帧。 |
| `--burst-interval` | 默认值：`1.0`。`burst` 模式两次突发之间的计划间隔，单位秒。 |

单位约定：

- 文档中的 `Mb/s` 按十进制计算，即 `1 Mb/s = 1,000,000 bit/s`。
- 文档中的 `MiB` 按二进制计算，即 `1 MiB = 1024 * 1024 bytes`。
- 周期统计中的 `tx` 表示当前统计窗口的瞬时发送吞吐；会随 `--stats-interval` 改变而变化。
- 会话摘要中的 `average`（日志字段显示为 `avg`）表示整个会话的平均发送吞吐。
- 会话摘要中的 `peak` 表示所有统计窗口中出现过的最大 `tx`。
- 周期统计中的 `blocked` 表示累计发送阻塞时间；会话摘要中的 `blocked=A/B` 表示阻塞发送次数 / 总发送次数。
- `max_send` 表示单次 `sendall()` 观测到的最长耗时。
- `bursts` 表示已完成的突发次数。
- `burst_bytes` 只统计已完成突发的字节数；如果会话在突发中途结束，最后一段不足目标大小的部分不会计入该字段。
- `overruns` 表示突发模式下计划下一次发送时刻已经被当前工作拖过的次数，可用于判断是否跟不上设定节奏。

压力模式发送的数据仍然是完整、合法、可被上位机协议层解析的音频/视频协议帧，不会为了追求吞吐而发送截断或伪造帧。压测结果可用于发现上位机链路、协议解析和 UI 处理瓶颈，但不能替代真实设备、真实网络拓扑和现场链路条件下的最终验收。

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
| 2026-07-23 | SRP-PHAT CPU 性能优化 | 分析、线程、构建、测试、文档 | 将 122 个阵元对聚合为 4 类基线，复用定位窗口的 PHAT 结果，缓存方向转向索引并使用专用 CPU 线程池并行扫描；当前开发机 Release 下 `640×360` 实时链路中位约 70 ms，热力图阶段约 1 ms。 | Release `test_analysis::evaluatesMaximumSpatialGridWithinCpuBudget`、Debug 全量 `ctest`、Python 测试、模拟服务端自检、`git diff --check` |
| 2026-07-23 | 实时热力图网格增强 | 分析、UI、配置、测试、文档 | 实时空间响应方向网格支持独立调整宽度和高度，默认 `64×36`、最大 `640×360`，配置自动保存并在启动时恢复。 | `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug`、`ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure`、`git diff --check` |
| 2026-07-23 | 实时声源定位与视频校准 | 分析、音频、视频、UI、配置、测试、文档 | 新增独立线程的实时 48 通道声源定位页面，支持纯标记和真实 SRP-PHAT 热力图；复用实时视频并提供四点稳定声源采集，自动拟合且允许手动调整五个视频视角参数，校准结果本地持久化；页面切换保持实时播放及其他设备功能不变。 | `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug`、`ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure`、`python -m unittest discover -s tests/tools -p "test_*.py" -v`、`python tools/mock_device_server.py --self-test`、`git diff --check` |
| 2026-07-23 | 离线分析与声源定位 | 分析、音频、UI、配置、测试、文档 | 新增独立线程的多通道 WAV 读取与单通道播放、6×8/5 cm 阵列 GCC-PHAT 声源定位、可扩展源码级定位器接口，以及按自定义相机视角在静态照片上动态标记方向；进入分析页仅关闭实时通道播放。 | `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug`、`ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure`、`python -m unittest discover -s tests/tools -p "test_*.py" -v`、`python tools/mock_device_server.py --self-test`、`git diff --check` |
| 2026-07-21 | 音频播放缺陷修复 | 音频、UI、测试、文档 | 修复进入“音视频融合”页面后音频播放异常：阵列热力图原先绕过 33 ms 渲染定时器，以约 240 FPS 对 48 个控件重复设置动态样式，且音频线程每处理一帧都等待 UI 回调后才接收下一帧。现将可视化严格限制到渲染节拍、隐藏页面跳过绘制、热力图颜色改为低开销调色板更新，并采用最多 16 帧在途和 32 帧有界等待的连续音频投递机制。 | `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug`、`ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure`、`git diff --check` |
| 2026-07-21 | 48 路增益与录音链路增强 | 音频、录音、UI、配置、控制、测试、文档 | 新增 48 路 `-8..+8 bit` 独立移位增益、每通道连续 AGC、6x8 竖向滑条 HMI、全自动/手动/复位和 JSON 保存载入；幅值、播放和单文件多通道 WAV 统一使用增益后数据，清单记录起止增益，录音队列增加字节与帧双上限；业务控制接口缩减为 6 路，线协议末两字段固定填零；模拟服务端二进制容量单位统一显示为 MiB。 | `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug`、`ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure`、`python -m unittest discover -s tests/tools -p "test_*.py" -v`、`python tools/mock_device_server.py --self-test`、`git diff --check` |
| 2026-07-21 | 协议修正与审查修复 | 协议、设备通信、测试工具、测试、文档 | 视频帧尾改为完整双字节：首字节固定 `0A`，第二字节可为任意值但必须存在；帧总长改为 `16 + payloadLength + 2`，原始帧与帧尾后预览按双字节偏移计算。音频、心跳和控制帧仍严格使用 `0A 0D`，新增测试确认音频 `0A 00` 被拒绝。此前的单字节视频尾规则已被本规则取代。 | `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_protocol`、`ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_protocol --output-on-failure`、`git diff --check` |
| 2026-07-13 | UI、录制与诊断增强 | UI、协议、媒体、测试、文档 | “交互驾驶舱”更名为“音视频融合”；操作统计改用自适应字节单位；高级帧调试历史增加帧尾后实际 16 字节及不完整标记；新增可任意选择 1-48 路通道的单文件多通道 WAV 异步录制、完整帧分段、JSON 清单、不覆盖保护、断线停止和录制状态诊断。 | `cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug`、`ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure`、`python -m unittest discover -s tests/tools -p "test_*.py" -v`、`python tools/mock_device_server.py --self-test` |
| 2026-07-13 | 模拟视频自检与文档修订 | tools、测试、文档 | 模拟服务端将 `1920x1080` 工业测试图作为脚本相对路径的不透明字节资源加载；当时自检确认缓存 payload 非空、与配置资源一致、完整写入视频帧且视频帧尾为单字节 `0A`。JPEG 解码有效性由 Qt 上位机确认，不在 Python 服务端解析；该单字节规则已于 2026-07-21 被双字节规则取代。 | `python -m unittest discover -s tests/tools -p "test_*.py" -v`、`python tools\mock_device_server.py --self-test`、从其他当前工作目录运行自检、`python -m py_compile tools\mock_device_server.py tests\tools\test_mock_device_server.py`、`cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug`、`ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure`、`git diff --check -- tools/mock_device_server.py tests/tools/test_mock_device_server.py doc/Readme.md docs/superpowers/specs/2026-07-12-mock-server-1080p-test-pattern-design.md docs/superpowers/plans/2026-07-12-mock-server-1080p-test-pattern.md` |
| 2026-07-09 | 压测能力增强 | tools、测试、文档 | `mock_device_server.py` 新增 `normal/sustained/burst` 压测用法说明与 `tx/average/peak/blocked/max_send/bursts/overruns` 指标解释；内置自检补充 burst/no-video 与限速校验；新增基于 `socket.socketpair()` 的自动化集成测试。 | `python -m unittest discover -s tests/tools -p "test_*.py" -v`、`python tools/mock_device_server.py --self-test`、`python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py`、`git diff --check -- tools/mock_device_server.py tests/tools/test_mock_device_server.py doc/Readme.md` |
| 2026-07-09 | 稳定性增强 | 设备通信、协议线程、UI、测试、文档 | 新增网络到协议线程的 ACK 背压机制：`NetworkWorker` 统计已投递但未确认的待解析字节，超过 `4 MB` 暂停读取，降到 `2 MB` 以下恢复；`ProtocolWorker` 每个 chunk 解析完成后回 ACK；UI 诊断显示背压状态和待解析队列字节数。 | `cmake --build ... --target test_device`、`ctest --test-dir ... -R device --output-on-failure` |
| 2026-07-08 | 稳定性增强 | 设备通信、测试、文档 | `NetworkWorker` 为 `QTcpSocket` 配置 `8 MB` 读缓冲上限并上报配置日志，避免高吞吐异常时应用侧接收缓冲无界增长；`DeviceClient` 断开请求改为同步投递到网络线程执行，提高析构和手动断开的确定性。 | `cmake --build ... --target test_device`、`ctest --test-dir ... -R device --output-on-failure` |
| 2026-07-08 | 诊断增强 | 协议、设备通信、UI、测试、文档 | `ProtocolCodec` 增加单次解析指标，`ProtocolWorker` 汇总累计 appended/parsed/discarded/buffered 数据，`DeviceClient` 暴露协议链路计数，实时诊断区域显示接收、已解析、丢弃和协议缓冲残留字节，便于现场判断 TCP 吞吐和协议错位问题。 | `cmake --build ... --target test_protocol`、`ctest --test-dir ... -R protocol --output-on-failure`、`cmake --build ... --target test_device`、`ctest --test-dir ... -R device --output-on-failure` |
| 2026-07-08 | 媒体/UI 线程重构 | 音频、视频、UI、文档 | 将 `AudioProcessor` 与 `VideoDecoder` 分别迁移到独立工作线程；主界面改为缓存最新音频/视频处理结果，并通过 33 ms 定时器节流刷新视频预览、音频波形和阵列校准热力图，降低媒体解码和高频绘图阻塞 UI/接收链路的风险。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure` |
| 2026-07-08 | 架构重构 | 设备通信、协议、测试、文档 | 新增 `NetworkWorker` 与 `ProtocolWorker`，将 `QTcpSocket` 接收/心跳/重连迁移到网络线程，将 `ProtocolCodec` 流式解析迁移到协议线程；`DeviceClient` 改为 UI 门面并通过 queued signal 分发帧和诊断，降低 UI/媒体处理阻塞 TCP 接收的风险。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure` |
| 2026-07-06 | 协议修正 | 协议、UI、测试工具、文档 | 视频帧接收尾部当时改为单字节 `0A` 判定；音频帧、心跳帧和控制帧仍按 `0A 0D` 判定；帧调试页和模拟服务端同步按帧类型显示/生成不同帧尾。该单字节视频规则已于 2026-07-21 被双字节规则取代。 | `cmake --build ...`、`ctest --test-dir ... --output-on-failure`、`python tools\mock_device_server.py --self-test` |
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
