# PL 源码与恢复说明

适配 XC7Z035 FFG676-2、48 通道 48 kHz 的原板卡。使用 Vivado / Vitis HLS **2020.2**，PL 100 MHz；PS 沿用原程序，本目录没有 PS ELF。运行 bit 和上位机须使用根 README 的配对发布包。

## 文件与综合边界

- `rtl/denoise_beam3.v`：三路分数延时波束。
- `rtl/wiener1024_fixed.v`：固定 Wiener。
- `rtl/denoise_post_profile_v3.v`：后处理封装，连接带通、LogMMSE、AGC。
- `hls/filters`：带通/AGC 数学源；`hls/logmmse`：选定的 R5_1p LogMMSE 数学源。
- `hls_rtl/{bandpass,agc,logmmse}`：对应生成 RTL、ROM 和浮点 IP 的创建 Tcl。保留工具生成声明；需本机 Vivado IP 生成支持。
- `profile/`：原演示 profile 常数、窗和系数。
- `netlist_interfaces/`：原 checkpoint 接口存档，**不是可执行数学实现**；不得与同名实现一起加入综合或用 stub 通过功能验收。
- `board_config/`：原 BD/XCI/约束及校验一致的板卡源参考。保留历史目录层次，避免同名旧版本被误当作最终顶层。

`doc/validation/source_provenance.json` 记录复制时的源 SHA256（移植可运行路径前）。生成源码、HLS 数学源、主要数据面模块及板卡自定义 RTL 与已验证冻结版本核对。板卡配置保留原目录层次，旧顶层与最终数据面不能混用。本目录不是已经在全新环境验收的一键完整 Vivado 工程。

## 恢复已布线实现

下载同版本 `MicArray-PLDenoise-Routed-Recovery-20261021.zip`，验证随包 SHA256。它包含最终 routed checkpoint，能够保留原整板实现与 IP 连接，不需要用历史同名顶层猜测组装。

```powershell
vivado -mode batch -source firmware/scripts/restore_routed.tcl -tclargs "<recovery>/routed.dcp" "build/recovered"
```

脚本检查 FPGA part，导出 route/timing/utilization/DRC 报告，并从该 checkpoint 写 bit。本次未再次执行此恢复脚本的整板工具流程；发布的 bit 是原已验证冻结文件。恢复文件的头部、时间戳可能改变文件摘要；原冻结 bit 的 SHA256 只用于确认原发布文件身份。恢复 checkpoint 不等于验证修改后的源代码：修改 RTL 后要重新综合、链接 IP、实现并完成时序/DRC/协议/数值测试。

## HLS 模块重建入口

在具备工具和有效器件/IP 支持的环境中：

```powershell
$env:DENOISE_HLS_STAGE="bandpass" # 或 agc
vitis_hls -f firmware/hls/filters/build_hls.tcl
vitis_hls -f firmware/hls/logmmse/build_hop.tcl
vivado -mode batch -source firmware/hls/logmmse/build_math_ooc.tcl
```

输出默认在仓库 `build/`，可通过 `DENOISE_HLS_BUILD_ROOT`、`DENOISE_LOG_OOC_BUILD_ROOT` 指定。模块综合完成不代表整板实现完成。未提供现场声音参考时，filter C 仿真明确跳过，不宣称数值验证通过。需要 filter ModelSim 协同仿真时，另设 `DENOISE_GOLDEN_ROOT` 和 `DENOISE_COMPILED_SIM_LIB`，使用 `cosim_export_hls.tcl`；它保留 2020 HLS FIFO-monitor 与 ModelSim 的兼容说明。

LogMMSE 的有限数值域、迭代预算及浮点转换的 subnormal flush-to-zero 行为见 `doc/validation/`，不能扩大为任意 IEEE 输入逐位等效。本次整理只调整构建路径和参数输入，未重新运行 HLS 综合或整板布线；使用已冻结的 RTL 与物理结果作为发布依据。
