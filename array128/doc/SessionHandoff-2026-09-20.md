# M128 开发交接（2026-09-20）

## 当前工作区

- 分支 `SpiralArray`，基线 `aa281b3 Raw48 软件波束形成`；本次没有创建重复分支，没有 stage/commit。
- 新增 `array128/` 全套独立应用，根 CMake 新增默认路由，`doc/Readme.md` 只增加新入口和变更记录；原 `src/` 不改动。
- 可执行程序：`build-array128/MicArray128.exe`，独立发布包 `dist/MicArray128/MicArray128.exe`。
- 根目录配置默认新应用；`-DMICARRAY_BUILD_LEGACY_48=ON` 显式选择旧程序。独立目录仍可单独配置。

## 已实现

- M128 v1 严格大端头/s24le PCM 解码；源 IP/端口过滤、8192 包有界网络队列、独立处理/定位/写盘线程。
- 七页面：实时监测、声源定位、定向降噪、音频录制、离线分析、系统配置、设备诊断。USB 摄像头直接使用 QCamera/QVideoSink，支持热插拔重新枚举；无 TCP/UDP 控制命令；后续已增加 SYS3 手动命令。
- 调试包含包号/帧号独立区间审计、前处理四故障、六状态位、自测参考逐样本比较、128 通道 DATA/LR/接口/Bank 及质量指标、导出 JSON 和 SYS3 串口面板/指引。时钟负载分组不能按通道号推定；串口须按需连接。
- 自测和故障会话不能进入声学处理；连续性变化清空窗口并用 token 拒绝过期分析结果。
- 真实坐标严格导入；独立 128 SRP-PHAT、真实响应热力、可调阈值/显示聚焦、四点视角校准；软件分数延时 A/B1/B2、维纳和可重排后处理；监听预缓冲和快捷录制。
- PCM24 原始/阶段来源，分离 mono 或多通道 WAV，按缺口/会话分段，原始数据报证据和清单，记录起止诊断；独立工具可 inspect 证据。
- USB 目前枚举结果没有可用摄像头，不能声称已完成真实 USB 图像测试。MEMS 和真实阵元机械坐标也未提供，不能声称真实声学验收。

## 已验证的证据

- 新 Release 构建成功；Qt 关键 CTest 套件通过（含 UDP 回环、自测隔离、128 轨录制、已知方向的合成螺旋阵列定位、连续方向更新保留 DSP 历史、旧 epoch 录制隔离）。Python 3/3 通过，独立发布包已重新部署。
- Python 独立工具 3/3 PASS；`git diff --check` 通过。
- 真实 FPGA 普通流 5 秒 80074 包，flags=1（未接 MEMS 时饱和保持）；用户随后明确表示已按 KEY1 进入自测。
- 真实 FPGA 自测探针：`build-array128/evidence/fpga-selftest-first.json`，79957 包、239871 帧、零错误、flags=0x10/epoch=2、PASS。后续 2 秒复核也 PASS。
- 早期 `qt-fpga-first.json`、`qt-fpga-unsandboxed.json` 均 0 包；恢复测试时实际 Qt 直连已成功。最终 `qt-fpga-20s-final.json`：20 秒 319,896 包 / 959,688 帧，约 16,001 包/s，零格式错误、零主机丢弃、零不连续、零数值不匹配，PASS。
- 实机 128 轨录制：`captures/m128_20260920_011850_463_f6918ecd/`，128,538 包 / 385,614 帧。独立工具重读证据文件，零不匹配/不连续/截断；全部 128 个 mono PCM24 48 kHz WAV 的 49,358,592 个通道样本逐字节等于官方参考表，通道顺序正确。
- 最终审计刷新修复后实机复测：`final-captures/m128_20260920_012159_589_f21f0d0f/`，63,855 包 / 191,565 帧，录制帧数与最终自测审计完全一致，零丢弃/不匹配。所有证据均位于 `build-array128/evidence/`。
- 原生 Windows 独立部署启动测试（PATH 仅 Windows 系统目录）退出码 0；UI 图：`build-array128/evidence/windows-package.png`。窗口、中文字体和运行库正常。此测试没有启动接收或摄像头。

## 防火墙与当前实机边界

此前用户批准的精确规则 `MicArray128-UDP5000-FPGA` 没有创建：管理员助手 session `35343` 已终止，Windows 返回“操作已被用户取消”。不要继续等待或重复启动该旧会话。

用户要求“继续测试”后，只读发现 Windows 已存在 `build-array128/MicArray128.exe` 对应的 TCP/UDP `Query User` 入站允许规则；实际接收已通。本次没有创建精确规则，也没有修改这些既有规则，不要把 Windows 既有规则说成助手创建。程序仍按 `192.168.10.2:5000` 过滤来源。独立发布目录是另一可执行路径，不要擅自放行它。

数字链路、128 轨写盘和诊断证据已实机验证。当前应用未枚举到 USB 摄像头，MEMS 与真实阵元坐标未提供；真实 USB 图像、声学定位/降噪及四点视角的现场验收仍需要这些硬件条件，不能把数字自测替代声学验收。

## 最近补充修复

- A/B1/B2 拖动连续应用实际角度，只在松开时持久保存；仅方向变化不清空滤波历史、定位窗口和监听缓冲。改变其他配置仍按需切断声学窗口，立即拒绝在途旧定位结果。
- 录制器独立检查帧连续性和 epoch；旧 epoch 迟到包前后都独立分段，并把 epoch 写入原始分段清单。
- 调试 UI/JSON 区分源数据报最后到达时间、合法包处理时间/延迟、队列深度及旧 epoch 包计数，避免把处理积压误当源端停流。
- 增加仅与定时诊断配合的 `--capture-array`，使用新唯一录制目录，不修改录制页面原设置。
- 实机测试发现命令行先录制再启动接收会使 `Pipeline::start()` 提前结束录制，已改为先启动接收再录制。
- 停止处理时强制发布最后不足 100 ms 的审计结果，防止最终清单遗漏末尾错误；增加错误末帧停止回归。定时诊断先停止接收再导出，因此最终报告的包数、帧数和审计范围一致。
- 此前实机联调构建和固定目录发布程序 SHA-256 均为 `B877B0455FC8784A723118380F40AF24FD6A8AA5BA18F405962F89FB85193EB1`，不是后续 UI 更新版本。各唯一发布包的当前哈希见自身 RELEASE.txt；未 stage/commit。

## 最新规格与 UI 核对

此节为增加串口收发前的核对记录；当前串口实现见下一节。

- 已阅读硬件仓库 2026-09-20 README、host_protocol_reference 与 system_uart_debug；新增 `hardwareprofile.*` 集中维护八路同源 CLK / 1.8 V / IM72D128V01 / 通带和接口映射，详细差异见 `SpecAudit-2026-09-20.md`。
- 旧“CH/32 时钟组”错误推断已移除，JSON 保留 `clock_group=null`，增加接口/Bank/FPGA 焊盘；全部 64 路 DATA 焊盘与 README 精确比对一致。
- 顶部只留状态和常用操作；网络参数移入“系统配置→接口设置”，坐标表和只读硬件规格分开；常用按钮直达。校准中心方向/视场角成对显示，四点常显且按钮不遮挡；数值和下拉控件禁止滚轮误改。
- UDP/PCM 解码与 DSP 规格未变；区分软件约 250–8000 Hz 分析带、硬件 20 Hz–16 kHz 通带和 FFT 24 kHz 上限。文档参考不是回读，SYS3 未接入明确显示未知。
- 新 UI 回归覆盖 1440×900 / 1120×700，截图在 `build-array128/ui-spec-review/`；本次无固件下载、真实采集、串口命令或防火墙改动。以前的数字自测 PASS 不是新版八路 CLK / 1.8 V 位流验收。
- 本次最终验证：Qt 17/17，128 工具 Python 3/3，原工具及发布脚本 Python 52/52，模拟服务端自检通过；统一发布脚本再次构建/CTest/部署/ZIP 成功，清除开发 PATH 后原生 Windows 启动退出码 0。
- 本次新发布包：`dist/MicArray128-Release-20260920-132220-111-f971d656.zip`；EXE SHA-256 为 `21B67A7A93A2B7B9CE3632BF49EE2A992B2647773EF9F02D8D09BC66A774C2FC`。旧发布包均保留；原生截图为 `build-array128/ui-spec-review/native-package.png`。

## SYS3 串口实现（后续更新）

- `sys3protocol.*`：312 字节/34 字严格解析、分片/粘包、长度上限、32/16 位回绕、重复不刷新时效、观察域重启、1500 ms 超时和分域新鲜度；命令识别/后续完成分离、5 秒超时不重试、发送前复查。
- `sys3client.*`：Windows 原生串口独立 QThread，115200 8N1，无流控，DTR/RTS 关闭；固定单字节发送。无需 QtSerialPort 模块，非 Windows 后端明确报不支持。
- `sys3panel.*`：设备诊断增加“SYS3 串口”，配置页有快捷入口。端口枚举/输入、s/h/r/t/c、34 原始字、分域时效、最近 400 条日志及导出；全局诊断包含 `sys3_uart`。录制/写盘中禁用 r/t，r 前暂停 UDP，恢复后手动重新接收；r/t/c 都有默认不执行的确认。
- 不自动连接/发命令；`--serial-port` 仅明确指定时连接。`--smoke-seconds` 隔离测试配置，避免保存虚拟串口到操作员配置。详见 `Sys3Serial.md`。
- 软件回归 26 PASS / 1 可选跳过；跳过的虚拟串口用例已通过 `tools/test_sys3_loopback.ps1` 单独完成（PowerShell 7），COM17/COM18 经注册表验证是同一 com0com 对，实际单字节序列恰好 shrtc。最近证据 `build-array128/sys3-loopback/run-de2b21acfc6c4b72ab8de1a678a39eef/`。
- 应用级本机回环收到 15 帧、0 异常、0 自动命令，已核查原生截图与 JSON（`run-a8795ae552db4828920ef4dfa6159766`）。所有主页面在 1440×900 / 1120×700 下检查；Python 3+52 项通过。
- 当前系统未枚举到 COM3；COM10 用途未知，未连接。没有向真实 FPGA 发送查询、复位、切换或清历史，也没改串口驱动/防火墙。历史 SYS3 实机样本已固定用于解码回归，但新版实机电气及串口/UDP 联合验收仍待现场完成。
- 最新 SYS3 发布包：`dist/MicArray128-Release-20260920-144411-018-7af4448a.zip`；构建与包内 EXE 的 SHA-256 都为 `0AA068A1A825AA1C166A0F7CCCB9A2A88B1C71817DA4991D6B2BA09DC66ACBA0`。统一脚本构建/CTest/部署/ZIP 通过；PATH 仅 Windows 系统目录时，包内程序仍通过虚拟串口接收、诊断导出和原生启动（`run-d39bb9bbd330428ea47f6405bda16d21`），零自动命令。

## 构建/部署注意

- 统一入口 `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-release.ps1` 现已兼容本分支，默认选择 128；`-Variant Legacy48` 选择旧 48。`array128/package.ps1` 改为兼容参数的委托入口，两者均生成唯一时间戳目录和 ZIP，不覆盖旧固定部署目录。
- 发布脚本此前已真实验证：128 构建/CTest/ZIP/无开发 PATH 启动成功，旧 48 构建/6 个 CTest/NoZip 部署成功；新增发布路由及失败关口测试 7/7，Python 工具总计 52/52。此前 ZIP 为 `dist/MicArray128-Release-20260920-120237-082-75ad547c.zip`；最新包见上节。
- 用 Qt 配套 MinGW13.1（Tools/mingw1310_64）与 Qt6.11.1，PATH 不能混入 MSYS 默认 g++。
- 如手动使用 `cmake --install`，Qt 部署前缀必须绝对路径；统一发布脚本使用 windeployqt 收集独立包运行库。
- Qt CMake 默认 DLL 部署到 bin，已通过 install 阶段 `QT_DEPLOY_BIN_DIR=.` 保持 EXE/DLL 同级。早期生成的 `dist/MicArray128/bin/` 是本次部署残留，不是用户资源；目前保留，根目录才是正式入口。
- windeployqt 到 build 后仅放 qwindows，offscreen 测试必须指定套件的 `QT_QPA_PLATFORM_PLUGIN_PATH`，已写入 CMake 测试环境和 45 秒上限。
- Windows GUI 程序 `--help` 会弹帮助框，不能当无人值守退出测试；已提供 `--smoke-seconds 2` 和可选 `--screenshot <path>`。
- 没有仍在后台运行的自建接收进程；探针与上位机不要同时绑定 5000。
