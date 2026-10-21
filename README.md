# 48 通道阵列：PL 降噪与配套上位机 R2

本分支将原上位机的固定参数降噪链移植到 FPGA PL，并提供配套 Qt 上位机、协议校验和连续音频回归。对应 **2026-10-21 StreamFix R2**，基于原 48 通道 `master`（`aa281b3`），不属于仓库默认分支的 128 通道螺旋阵列工程。

## 下载与运行

[配套发布包：PL Denoise StreamFix R2](https://github.com/honokawings/MicArray/releases/tag/pl-denoise-streamfix-r2-20261021)

- `MicArray-PLDenoise-StreamFix-R2-Windows-x64-20261021.zip`：**配对的 `pl.bit` 与 Windows 上位机、Qt 运行库、示例配置及校验清单**。
- `MicArray-PLDenoise-Routed-Recovery-20261021.zip`：已布线 checkpoint，供 Vivado 2020.2 恢复实现；日常演示不需要。
- 源码在本分支；Git 中不存储构建缓存、现场录音和个人相机标定文件。

解压到可写目录，沿用原板卡和 PS 转发程序，通过原有烧录流程加载包内 `pl.bit`，运行 `上位机/MicArray.exe` 并设置实际设备地址。先确认连接、原始音频和控制确认状态，再开启降噪与监听。示例配置默认关闭处理/监听；方向及播放增益需要现场设置，未携带作者的相机标定和个人配置。不要混用旧 bit 与新模式。

当前适配硬件：**48 通道，6 行 × 8 列，横竖间距 5 cm，48 kHz，XC7Z035 FFG676-2，PL 100 MHz**。PS 继续转发数据与控制；本次未提供 PS 源码或新 ELF。

## 当前实现了什么

```text
48 路采集 → 分数延时三路波束 A/B1/B2 → 固定 Wiener
          → 带通 100–8000 Hz → LogMMSE → AGC → PS 转发 → 上位机显示/录音/监听
```

三路波束中 A 是目标方向，B1/B2 是噪声参考，采用四抽头三阶 Lagrange 分数延时。全链模式最终输出 **一路单声道降噪音频**，不代表三个方向各输出一路独立增强音频。

| 上位机模式 | PL 计算 | 上位机计算 |
|---|---|---|
| PL 三路波束 + 上位机原降噪（AUD3/DNS1） | 三路波束 | Wiener、带通、LogMMSE、AGC |
| PL 固定维纳 + 上位机原后处理（AUW1/DNS1） | 三路波束、固定 Wiener | 带通、LogMMSE、AGC |
| PL 全链降噪（固定演示参数，AUF1/DNS1） | 三路波束、固定 Wiener、带通、LogMMSE、AGC | 显示、录音、监听及播放增益；不重复降噪 |

保留 48 通道软件波束形成作参考，以及已有的 LOC3 多声源定位、HMP1 空间图和视频叠加功能。界面区分上位机参考热力图和 PL 空间图来源；PL 空间图为 37 × 37，定位最多三个候选。没有声纹身份识别模块。本次发布重点是降噪迁移和连续传输修复，不将已有定位功能重复算作本次新增成果。

## 本次改进

1. **计算迁移**：将原软件固定参数链中的三路波束、固定 Wiener 及后处理落到 PL，保留三种迁移阶段的对照入口。
2. **配套协议与状态检查**：解析 AUD3/AUW1/AUF1 与 DNS1，核对控制确认、模式、profile 指纹及数据阶段。参数不支持或数据不匹配时给出状态，不用旧通道替代当前模式的音频。
3. **R2 连续音频修复**：移除音频处理对 GUI 完成回执的依赖，改为工作线程驱动的有界 FIFO，保持控制、录音和音频任务顺序。界面繁忙时仍可继续处理音频。
4. **降低界面负载**：隐藏的高级调试页不再逐帧格式化原始数据、创建视觉效果；可见预览限频更新、复用动画，避免持续挤占音频调度。

R2 沿用已交付 Stage3 的 bit；此次停播修复集中于配套上位机的调度及调试显示。上位机生产 DSP 的 12 个处理源文件保持原实现。用户已反馈 R2 解决了此前“播一会儿后停声”的问题；该现场反馈与下述仿真/软件测试分别记录。

## “全链”适用范围

全链模式对应**固定演示 profile**，不意味着软件所有可调算法都已做成 PL 通用实现：

| 项目 | 当前 PL 配置 |
|---|---|
| Wiener | 固定参数；1024 点、hop 512；支持单/双参考配置及抑制系数控制 |
| 带通 | 启用，100–8000 Hz |
| LogMMSE | 启用，FFT 2048、重叠 0.5、先验平滑 0.98、噪声平滑 0.95、最小增益 −20 dB |
| AGC | 目标 −24 dBFS，最大增益 20 dB、最大衰减 −20 dB，启动 20 ms、释放 300 ms、限幅 −1 dBFS |
| 后处理顺序 | 带通 → LogMMSE → AGC |
| 标识 | profile fingerprint `b2c8f065`；build_id `0x20261022`（内部标识，不是发布日期） |

方向及固定 Wiener 系数可以下发。自适应 Wiener 和任意后处理参数组合仍需软件路径，不是本版全链 PL 的能力。全链仅上传最终 AGC 输出，不上传所有中间处理音频。切换上位机输入模式不会降低这份完整 bit 已综合的资源占用。

## 验证与硬件资源

冻结交付记录包括：R2 九组软件回归 350 个 Qt PASS、旧连续传输契约 22 个 PASS、12 个真实时钟压力场景（每个 10 s），以及主窗口隐藏/显示调试页各 10 s 的负载验证。压力测试包括 GUI 回执延迟和 GUI 线程阻塞，检查真实输出样本与参考逐位一致；这不是扬声器声学测量。

本次公开源码整理后的重新构建、测试结果与限制见 [验证摘要](doc/validation/README.md)。不公开含现场声音的私有 golden 数据；依赖该数据的 14 个数据测试默认明确标为 SKIP，合成协议与连续流测试正常运行。不能把 SKIP 解释为逐位一致性已复现。

以下为**整份最终已布线设计**，包括原采集、定位和接口，不是降噪模块单独增量：

| 资源 | 已用 / 总量 | 占用 |
|---|---:|---:|
| LUT | 163,976 / 171,900 | 95.39% |
| FF | 128,777 / 343,800 | 37.46% |
| BRAM 36K 等效 | 498 / 500 | 99.60% |
| DSP | 711 / 900 | 79.00% |

100 MHz 已布线记录：setup WNS **+0.006 ns**，hold WHS **+0.006 ns**，32 组 bus-skew 检查无违反，DRC Error 0、未解析黑盒 0。余量很小；修改 RTL 后必须重新实现与检查，不能沿用此结果。尚无统一测试条件下的客观音质等效、SNR 提升或端到端时延实测结论，也不宣称任意声学条件下与软件完全等效。

## 源码结构与构建

| 路径 | 内容 |
|---|---|
| `src/`、`main.cpp` | Qt 上位机与连续音频修复 |
| `tests/` | 协议、媒体、界面及 Windows 连续流压力测试 |
| `firmware/rtl/` | PL 波束、Wiener、传输、定位及后处理封装 |
| `firmware/hls/`、`firmware/hls_rtl/` | 后处理 HLS 数学源与对应生成 RTL/IP 配置 |
| `firmware/profile/` | 固定演示参数、系数及指纹 |
| `firmware/board_config/` | 原板卡 BD、IP 配置与约束参考 |
| `doc/validation/` | 校验标识、验证范围和公开测试结果 |

上位机要求 Qt 6.5+、兼容的 C++20 编译器、CMake 和 Ninja。本次验证使用 Windows x64、Qt 6.11.1、MinGW 13.1。以自己的 Qt kit 路径替换示例：

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<Qt-kit-path>"
cmake --build build/release --parallel 4
ctest --test-dir build/release --output-on-failure -j 1
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
```

Windows 连续流测试默认开启（`MICARRAY_BUILD_STREAMING_TESTS=ON`），需 Qt Test，按序执行约数分钟；CPU 性能门限受主机负载影响。可设置 `MICARRAY_DENOISE_GOLDEN_ROOT` 使用自行保管的完整原始 golden 数据，不能用自生成输出冒充独立参考。

FPGA 工具链、模块重建入口、checkpoint 恢复和整板冷重建的限制见 [firmware/README.md](firmware/README.md)。当前可靠的整板恢复入口是发布的配对 bit / 已布线 checkpoint；本次没有在清空所有 IP 缓存的新环境中完成整板从零重建验收。

历史说明保留于 `doc/`，其中旧候选状态以本 README 和本次验证摘要为准。原始文件及工具生成文件保留各自版权声明，本次不为上游代码另行指定许可证。
