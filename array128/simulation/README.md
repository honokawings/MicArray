# 128 阵列声视联合仿真

此目录提供独立物理前向模型、程序化 RGB 场景、真实 C++ SRP 数据集构建与多目标评价。
没有用高斯点、真值方向或视觉标记伪造 SRP。神经网络效果仅能证明程序化视觉域内的能力，
不能代替实拍视频、真实噪声/混响与已确认硬件坐标的现场验收。

## 几何与角度

`geometry_fermat_v5.json` 是上位机可读取的 128 点、395 mm 单臂 Fermat V5 **仿真模板**；
`fermat_v5_coordinates.json` 保留原始设计格式。来源为
`D:/Workspace/QT/MicSpiralArray/results/aperture_400mm_v5_literature/full_run/coordinates_v5.json`，
原文件 SHA-256 为 `249a2191fa8bb2f7b8ca2a088af23d77d489c4ad86b3aac94e4ba26356435a12`。
最小间距 17.733398467 mm。设计序号尚未证明对应 FPGA CH，禁止自动用于实际设备或视作制造布线表。

面阵位于 xy 平面，朝 +z；+x 向右，+y 向上。此工具使用上位机的球面方位/俯仰：
`[sin(az)cos(el), sin(el), cos(az)cos(el)]`，与研究项目的独立 H/V 正切角定义有别。
摄像机采用针孔投影，含中心方位、俯仰和 roll，真值为连续的发声点投影，不是边框中心或 SRP 峰。

## 生成与数据契约

使用项目内的隔离 Python 环境，不依赖系统 Python 或外部阵列项目的环境。
已验证 Python 3.12.14、NumPy 2.5.3、Pillow 12.3.0、SciPy 1.18.0；依赖由
`requirements.txt` 精确锁定。若该环境尚不存在，可用 `py -3.12 -m venv build-array128/fusion-env`
创建；已有环境直接复用，不在训练进程运行时更换其 NumPy/PyTorch 等依赖。
然后使用现有 CMake 生成的 `m128_srp_export.exe`：

```powershell
$simulationPython = './build-array128/fusion-env/Scripts/python.exe'
& $simulationPython -m pip install -r array128/simulation/requirements.txt
& $simulationPython -B array128/simulation/generate_dataset.py --srp-exe build-array128/m128_srp_export.exe --output build-array128/av-data-001 --train 2000 --val 300 --test 300
& $simulationPython -B -m unittest discover -s array128/simulation -p 'test_*.py' -v
& $simulationPython -B array128/simulation/audit_dataset.py --dataset build-array128/av-data-001 --output build-array128/av-data-audit-001.json
```

仅缺少评价依赖时，可在已满足 NumPy/Pillow 版本的隔离环境中执行
`& $simulationPython -m pip install scipy==1.18.0`。SciPy 的 `linear_sum_assignment`
用于一对一匹配，缺失时应安装依赖，不替换匹配算法。2026-10-02 在上述项目环境完成
14 项单元回归、正式 2600 样本审计及 15 组 PCM/RGB 哈希精确重建；正式 03 数据最初使用
NumPy 2.3.5 生成，本次已验证锁定的 2.5.3 可精确复现这批抽样，不据此保证任意未来版本兼容。

输出目录必须尚不存在，不覆盖已完成证据。每个样本先以 f32le/time-major/channel-interleaved
保存真实模拟 2048×128、48 kHz PCM，再调用生产 C++ SRP；真值不传入该可执行程序。
默认成功导出后移除本次创建的 PCM 以节省约 1 MiB/样本，`--keep-pcm` 可保留。
后续可以从记录的 scenario/waveform seed 精确重建 PCM，并核对保存 SHA-256。
若导出失败即终止并保留失败状态，绝不回退到人工热点图。
每次运行冻结一份 C++ 导出器副本并记录 SHA-256，避免长批次中重编译导致混用算法版本。
审计工具逐个核对真实热图和训练输入、标签与物理投影、分割 seed/内容泄漏，并抽样重生成
PCM/RGB 比较内容哈希。状态不是 `complete` 的开发/中断批次禁止用于训练或正式评价。

每个 `{train,val,test}/<scene_id>.npz` 包含：

| 字段 | 格式 |
|---|---|
| rgb | uint8 `[192,320,3]` |
| heat | float32 `[45,72]`，真实归一 SRP；无有效图时全零并保留原始 reason |
| boxes | float32 `[N,4]`，归一 cx/cy/w/h，所有可见候选（含静默者） |
| classes | int64 `[N]`，0=程序化可见候选，不声称真实物体语义识别 |
| active | float32 `[N]`，独立活动标记 0/1 |
| points | float32 `[N,2]`，连续发声点 uv；静默者点有限，但发声点损失需由 active 屏蔽 |

同名 JSON 与 `scenes/<id>/truth.json` 保存物理方向、独立随机种子、掩码、噪声、反射及
camera 参数；`manifest.jsonl` 绑定所有产物；`dataset.json` 绑定导出 EXE SHA-256、几何与生成状态。
训练/部署均使用 RGB/255 + SRP 四通道，heat 上采样固定双线性、`align_corners=False`。

训练/验证/测试的场景、每目标 waveform 和传感器噪声使用分离的种子命名空间，禁止混用。
场景均衡覆盖 0–4 个独立发声源、4–7 个候选物体；静默与发声物体的绘图逻辑和外观分布相同。
近邻布局也先独立生成，再抽取发声者；同一场景在 0–4 个活动目标之间切换时 RGB 逐像素不变，
避免让“谁与谁靠近”成为活动标签的视觉捷径。
音频不由像素亮度、边框或预测峰生成。近邻场景取物理球面方向 1.5–9°间隔；所有样本有连续亚网格角度。
噪声、独立源幅度、有限距离球面传播、轻度增益误差与统计反射显式建模，反射并非精确房间声学。
指定坏通道被置零、强噪声或偏置污染，再通过实际 DSP 屏蔽列表移除；健康通道信号不受掩码改变。

## 独立评价

```powershell
python array128/simulation/evaluate.py --dataset build-array128/av-data-001 --srp --output build-array128/srp-evaluation-001.json
python array128/simulation/evaluate.py --dataset build-array128/av-data-001 --srp-grid --confidence 0 --output build-array128/srp-grid-evaluation-001.json
python array128/simulation/evaluate.py --dataset build-array128/av-data-001 --predictions predictions.jsonl --output build-array128/yolo-evaluation-001.json
```

神经预测每行 `{"scene_id":"test-000001","sources":[{"point_uv":[0.2,0.3],"confidence":0.8}]}`，
也支持 `azimuth_deg/elevation_deg`。预测集合必须精确等于指定留出集，禁止跳过失败场景。
评价使用独立物理真值、球面夹角和一对一最优匹配，默认 2°匹配门槛；汇总 precision、recall、F1、
计数误差、静默虚警率、近邻全目标分辨率与成功匹配角误差 P50/P95。
角误差只汇总成功匹配项，必须同时报告漏检和召回率；不得用低误差掩盖漏掉第二声源。
按 0–4 源、理想/压力和屏蔽状态分组，另给出按真值计算的 72×45 网格量化误差基线。
`--srp-grid` 额外将真实 SRP 预测点吸附到原始网格，保留原检测数量与置信度，仅用于量化误差消融；
它不使用真值选点，也不应称为重新运行的“无细化 SRP 峰检测器”。正式模型已冻结阈值并筛选的预测
使用 `--confidence 0`，避免评价脚本再次叠加默认阈值。
近邻超分辨另采用严格判据：两目标各自误差不超过 `min(2°, 真实间隔/3)`，预测点间隔至少为
真实间隔的一半，防止把同一个点重复输出两次、恰好都落入 2°匹配窗而误称双源分辨。

## 冻结模型的新场景压力测试

`run_stress_suite.py` 不训练模型、不挑选新阈值；在第一次推理前写入固定 `plan.json`，
以新种子构造 628 个样本、43 档条件。20/16/12 次独立重复按组内条件配对；同一重复的
场景与波形可跨参数档复用，不能将全部 628 个样本当成独立同分布样本。

| 测试族 | 条件 | 样本数 |
|---|---|---:|
| 常规场景 | 0–4 个发声源，原生成分布，新种子 | 100 |
| 双源角间隔 | 1.5/2/3/4/6/9/12°，独立波形、等电平 | 112 |
| 噪声 | −10/0/5/10/20/30 dB 输入信噪比 | 96 |
| 强弱源 | 0/6/12/18/24 dB 电平差，额外报告弱源召回 | 80 |
| 已屏蔽坏通道 | 无坏通道、分散/扇区各 4/8/16 路 | 84 |
| 未屏蔽坏通道 | 4/16 路，与对应屏蔽组使用相同受污染 PCM | 24 |
| 视频退化 | 正常、亮度×0.25、模糊、固定中央遮挡、水平错位12px | 60 |
| 反射 | 每源两条统计反射，各自幅度增益 0/0.15/0.35/0.6 | 48 |
| 近邻静默干扰 | 3/6°同样画面，但第二目标不发声 | 24 |

普通参考组与人为压力组分别报告，不能混成“应用准确率”。角间隔按球面定义；反射不代表
真实房间 RT60；视频水平错位是校准/配对错位压力，不是假装进行运动时序仿真。
相邻物体可能互相遮挡，所以近邻测试是声视联合分辨，不是纯声学极限测量。

```powershell
# 先按上级说明将 Qt 和 MinGW bin 加入 PATH。
& ./build-array128/fusion-env/Scripts/python.exe -B array128/simulation/run_stress_suite.py `
  --model build-array128/fusion-training/formal-20261002-01/export/fusion.onnx `
  --srp-exe build-array128/av-data-20261002-03/engine/m128_srp_export.exe `
  --prior-dataset build-array128/av-data-20261002-03 `
  --output build-array128/challenge-new-run
```

采用训练时冻结的真实 C++ SRP 导出器并核对 SHA-256；源文件、模型和元数据也记录散列，
运行期间变化立即判失败。与原 train/val/test 的场景/波形种子及 RGB/PCM 散列检查隔离。
每个场景保留 PCM、原始 RGB、纯数值 SRP、网络原始输出、预测与物理真值，完整批次约需
0.8 GiB 磁盘。目录须不存在，旧证据不覆盖。`--limit-replicates 1 --seed 12820261004`
可跑不同种子的小批流程自检，不作正式效果结论。

结果为本地 `report.html`、总览与对照 PNG、`report.json`、逐场景文件及散列清单。
“原始模型”与当前上位机可观测静态条件门控后的指标分别保存；强制测试被门控拒绝的数据
不代表实际 UI 会显示这些结果。CPU 推理时间不含 SRP/相机/绘制。报告示例按预定规则选出
每组首例、首个漏检、首个误报及首个完整正确非空场景，同时保留全部案例，不能只展示成功图。
严格近邻率提供 Wilson 95% 区间；误差分位数只针对正确匹配项。

## 变更记录

- 2026-10-03：增加冻结模型的 9 类、43 档、628 样本配对压力测试；保留原始与门控后指标、
  弱源召回、近邻置信区间和完整输入/输出证据，生成中文 HTML/PNG 报告；新增 15 项回归。

- 2026-10-02：增加独立物理/视觉前向模型、Fermat V5 仿真资源、真实 C++ SRP 数据集通路、
  0–4 源与静默干扰、显式坏通道屏蔽、近邻/亚网格场景和独立球面匹配评价。
