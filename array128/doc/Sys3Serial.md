# SYS3 串口收发

## 使用入口

“设备诊断→SYS3 串口”；也可从“系统配置→接口设置→打开串口面板”进入。选择/输入实际端口后点击“连接串口”。默认 COM3，固定 115200 / 8N1 / 无流控，UART 电平 3.3 V。关闭占用同一端口的其他串口助手。端口选择可保存，但启动不自动连接，连接不自动发命令。

硬件需输出 `SYS3`，`CLK2` 不能用于此面板。SYS3 识别不等于位流身份验证，更不能检测 1.8 V MEMS 供电是否正确。

## 命令与保护

| 按钮 | 字节 | 行为与完成判定 |
|---|---|---|
| 读取状态 / 状态别名 | s / h | 请求快照；识别回执后仍须分别检查各时钟域的新鲜标记 |
| 系统复位 | r | 二次确认，录制/写盘中禁用；先停止主机 UDP，发送后观察新鲜音频域、时钟恢复、校准完成及实采模式；不声称测量了复位脉宽；请手动重新开始 UDP 接收 |
| 切换自测 | t | 二次确认，录制/写盘中禁用；新鲜音频域及正常时钟才可发送；间隔至少 200 ms，后续音频/以太网快照须确认模式改变、新 epoch 和校准完成 |
| 清时钟历史 | c | 二次确认；后续快照检查时钟历史掩码清零，不等于清除当前故障、PHY/MAC 计数或采集错误 |

所有命令均为单字节，不加 CR/LF。识别依据是 16 位命令计数恰好递增一次（支持回绕）且最近字符匹配。`r/t/c` 必须再看到后续递增快照，不能把同一条识别回执直接当成完成。出现外部命令干预、观察域重启或断连时结果标为未知；5 秒未确认则超时，**不自动重试**。任何时刻最多有一条命令待处理，不建立后台命令队列。

连接中有效 SYS3 帧超过 1500 ms 未递增时禁用发送；重复帧不能刷新时效。发送线程取出命令前再次检查时效，超过 200 ms 尚未发送的请求丢弃，模式切换还会重新检查音频域和时钟条件。确认框默认选择“不执行”。

## 状态与报告

- 严格解析 312 字节：`SYS3 `、34 个八位大写十六进制字、单空格分隔、CRLF。支持任意串口分片和多帧合并；过长行有界丢弃并从下一行恢复。
- 观察域状态与音频、以太网（含 PHY/MDIO/MAC）、RX 域分别标注“新鲜/历史/未知”。域新鲜位为零时不会冒充当前读数；串口断开/全局超时后所有域均为历史数据。
- 显示时钟掩码与相对频率、采集/校准/自测、PHY/MDIO/PHYSR、ARP、UDP TX、RX 计数等；另提供完整 34 字的 HEX、十进制和域时效表。
- MDIO 写事务的 ACK 显示“不适用”；扫描时读无 ACK 不自动判定设备故障。FPGA `UdpSent` 与 TX BIST 不代表 PC 已收到或逐样本验证通过。
- 最近 400 条原始行/收发事件在界面和日志导出中保留；超出会丢弃最旧记录，不是无限时长取证。诊断 JSON 增加 `sys3_uart`，记录连接、全局/分域时效、原始字、命令状态、连续性计数和日志。
- `hardware_reference.uart_supported_by_host=true` 表示软件具备能力，不表示串口已连接。位流身份、电压、真实接线和声学性能仍需外部确认。

## 实现与依赖

`sys3protocol.*` 为可独立回归的解析/会话状态；`sys3client.*` 用 Windows 原生串口和独立 QThread；`sys3panel.*` 为界面。Qt 套件没有 SerialPort 模块，因此无需安装或部署额外 QtSerialPort DLL。原 48 通道源码及 M128 UDP 格式不变。

端口独占打开，DTR/RTS 关闭，读采用立即返回模式，写超时 250 ms，UI 不执行串口 I/O。实现参考 Microsoft 的 [COMMTIMEOUTS](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-commtimeouts) 和[串口资源句柄](https://learn.microsoft.com/en-us/windows/win32/devio/communications-resource-handles)说明。非 Windows 构建明确返回不支持，而非伪造连接成功。

可使用 `MicArray128.exe --serial-port COM3 --smoke-seconds 5 --report <新路径> --screenshot <新路径>` 做有时长限制的只读接收。不会自动发送任何命令；`--smoke-seconds` 使用隔离测试设置，不保存测试端口到操作员配置。

## 验证

Qt 回归覆盖全部分片边界、粘包、错误字符/长度、CLK2、超长行恢复、计数回绕、重复不刷新、观察域重启、分域过期、发送前复查、识别/动作分离、超时与外部干预。另解码硬件仓库历史 `capture_20260920_024312/raw.log` 的首条 SYS3，固定于 `tests/fixtures/sys3_historical.txt`；这是历史协议样本，不是新位流的本次实机验收。

Windows 原生收发使用已存在的 com0com 配对端口 COM17/COM18 验证；一端是本上位机收发代码，另一端是独立 .NET SerialPort 模拟器，逐字节确认 `shrtc`，回包刻意拆开十六进制字段。脚本先校验注册表中的配对关系，拒绝物理或未知端口，不安装/修改驱动。

在 PowerShell 7 中执行（需要 Qt 运行库 PATH 与 offscreen 插件路径）：

```powershell
& .\array128\tools\test_sys3_loopback.ps1
& .\array128\tools\test_sys3_loopback.ps1 -ApplicationSmoke -TestExe build-array128/MicArray128.exe
```

普通 CTest 不碰串口，会跳过可选虚拟端口用例。单独回环已通过全部五种命令；应用级回环收到 15 条有效 SYS3、零异常行、零自动命令，截图与报告位于 `build-array128/sys3-loopback/run-a8795ae552db4828920ef4dfa6159766/`。1120×700 与 1440×900 页面也已检查。

本次未枚举到 COM3；COM10 用途未确认，因此没有连接或向真实 FPGA 发送 `r/t/c` 等命令。新版 FPGA 的串口电气、复位/模式切换与 UDP 联合验收仍待现场进行。虚拟串口通过不替代真实 FPGA 验收。

最终常规 Qt 回归 26 项通过、1 项可选串口用例跳过（该用例已单独回环通过）；Python 3+52 项通过。发布包 `dist/MicArray128-Release-20260920-144411-018-7af4448a.zip` 已在 PATH 不含 Qt 开发环境时完成原生启动、虚拟串口接收和报告导出，零自动命令，证据为 `build-array128/sys3-loopback/run-d39bb9bbd330428ea47f6405bda16d21/`。
