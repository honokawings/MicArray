# YOLOX 音视频融合训练与推理

本目录提供真实神经网络，不使用 SRP 峰值、插值或框中心冒充网络输出。骨干是官方 YOLOX-Nano 派生结构，保留 objectness、类别、bbox 检测及 SimOTA 训练，增加独立连续发声点和活动概率头。默认类别只有“可见候选目标（仿真域）”，不宣称能够识别真实人物/设备类别。来源与许可见 `THIRD_PARTY.md`。

## 输入输出契约

- 输入 `rgb_srp`: float32 `[1,4,192,320]`，RGB/255 + C++ 实际 SRP 的归一化数值响应。
- 声像重采样：双线性、half-pixel、`align_corners=False`。不得使用 UI 伪彩、聚焦 gamma、圆形热点或文字叠加。
- 输出 `detections`: float32 `[1,1260,9]`，每行 `cx,cy,w,h,objectness,class_0,source_x,source_y,active`。
- bbox 和 source point 为归一化视频坐标；objectness/class/active 已 sigmoid。point 为单独网络回归，可能越界，后处理拒绝越界而非夹紧。
- 后处理检测分数为 objectness×class，另检查 active，bbox IoU NMS。不从声像重新选峰替换 source point。
- 发声点经上位机相机标定转为连续角度。“连续/亚网格输出”只是结构能力，是否达到超分辨率必须由独立仿真角度误差和近邻目标分辨验证。

## 数据集

每个 `train/val/test/*.npz` 包含：`rgb` uint8 `[192,320,3]`、`heat` float32 `[45,72]`、`boxes` float32 `[N,4]` 归一化 cxcywh、`classes` int64 `[N]`、`active` float32 `[N]`、`points` float32 `[N,2]` 真实投影坐标。

boxes 包含所有可见候选，含静默干扰物；active 与外观/位置独立。发声点真值来自已知物理声源与相机投影，禁止从 SRP 峰提取。静默候选不参与 point loss。独立场景/声波种子划分由 `array128/simulation` 生成器保证；训练脚本只读取 train 与 val，不读取 test。测试集只能在模型和阈值冻结后评价。

## 命令

在仓库根目录使用项目隔离 Python，例如 `build-array128/fusion-env/Scripts/python.exe`：

```powershell
python -m unittest array128.fusion.test_fusion -v
python -m array128.fusion.train --dataset <dataset> --output <new-run> --epochs 30
python -m array128.fusion.export --checkpoint <new-run>/best.pt --dataset <dataset> --output <new-export>
python -m array128.fusion.evaluate --model <new-export>/fusion.onnx --dataset <dataset> --split test --output <new-evaluation.json>
```

隔离环境采用 Windows CPython 3.12，依赖版本见 `requirements.lock`。CPU 版 PyTorch 来自官方 `https://download.pytorch.org/whl/cpu`，其余依赖来自 PyPI；不用修改系统 Python，也不需要安装 Ultralytics。运行库路径可从环境中的 `Lib/site-packages/onnxruntime/capi/onnxruntime.dll` 获取。`--resume` 只载入模型权重并重新建立优化器，属于新目录中的继续训练，不是完整优化器状态恢复；标记为不可用于性能的旧权重会被拒绝。

导出会实际运行 ONNX Runtime 并逐数值对照 PyTorch，失败不输出有效元数据。权重和元数据均包含训练来源、验证结果、SHA-256、仿真域限制；还包含按通道排列的 128 个阵元三维坐标、坐标源文件散列、72×45 训练声像网格和仅验证集选择的阈值。导出前检查数据集与权重的来源一致，评价前检查模型散列。生成模型、数据和环境放在 `build-array128/`，不提交。研究阵列的 PCB 通道映射尚未确认，元数据明确禁止将仿真成果当作实机验收。

元数据 `training_srp_settings` 从所有冻结训练输入逐项核对，包含最大声源数、最小方向间距、声速和窗口样本数；这些参数会影响实际 SRP 输入分布。部署必须匹配训练配置，而不能只检查热图尺寸。来源输入集合的散列与核对数量同步记录，不读取测试场景来决定参数。

评价内含完整融合、RGB-only、SRP-only 及错配 SRP 四种输入条件；这些分布外消融用于检查模态依赖，不是单独训练的最佳单模态基线。这里的像素匹配只用于训练诊断；独立声学评价必须使用 `array128/simulation/evaluate.py` 的角误差、目标数、误报/漏报、近邻可分辨及 SRP 基线对照。真实场景泛化与采样级同步没有通过仿真自动得到保证。

## 已冻结的仿真模型与 C++ 验证

`build-array128/fusion-training/formal-20261002-01/export/` 保存正式 ONNX、元数据及数值对照夹具；30 轮训练仅由验证集选出第 22 轮，未使用测试集调阈值。2026-10-03 独立球面角评价及能力边界见 [`../README.md`](../README.md) 的交付验证章节。导出时的候选元数据保持不变，后续评价另存报告。

下列环境变量使 CTest **必须**执行真实运行库测试，缺少资源时失败而非跳过。先按上级说明构建并设置 Qt/MinGW 路径：

```powershell
$env:M128_FUSION_MODEL_DIR = 'D:/Workspace/QT/MicArray/build-array128/fusion-training/formal-20261002-01/export'
$env:M128_ONNXRUNTIME = 'D:/Workspace/QT/MicArray/build-array128/fusion-env/Lib/site-packages/onnxruntime/capi/onnxruntime.dll'
$env:M128_FUSION_PIPELINE_FIXTURES = 'D:/Workspace/QT/MicArray/build-array128/fusion-pipeline-fixtures-20261002-01'
$env:M128_REQUIRE_FUSION_RUNTIME = '1'
ctest --test-dir build-array128 --output-on-failure
```

没有这些可选资源时，纯 C++ 规则/线程基础测试仍运行，但实际模型相关测试会明确跳过；不能将这种默认通过等同于完成神经验证。固定 PCM/RGB 联调夹具属于验证集，按源数选取，不使用测试集挑选成功案例，也不把真值或保存热图传入联调推理。
