# YOLOX 派生实现与第三方声明

`model.py` 的 CSPDarknet、PAN/FPN、解耦检测头拓扑，以及 `loss.py` 的 SimOTA 分配、IoU/objectness/class 损失，依据 Megvii 官方 YOLOX 源码派生；不是 Ultralytics 软件或权重。原作者版权标记和 Apache-2.0 许可证保留于源码头及 `LICENSE.yolox`。

核对来源（2026-10-02）：

- https://github.com/Megvii-BaseDetection/YOLOX
- https://github.com/Megvii-BaseDetection/YOLOX/blob/main/yolox/models/network_blocks.py
- https://github.com/Megvii-BaseDetection/YOLOX/blob/main/yolox/models/darknet.py
- https://github.com/Megvii-BaseDetection/YOLOX/blob/main/yolox/models/yolo_pafpn.py
- https://github.com/Megvii-BaseDetection/YOLOX/blob/main/yolox/models/yolo_head.py
- https://arxiv.org/abs/2107.08430

本地修改：保持 Nano depth=0.33 / width=0.25 / depthwise 骨干与三个 stride=8/16/32 检测尺度；输入改为 RGB+数值 SRP 四通道；增加独立发声点连续回归与活动概率分支；训练标签/推理契约/仿真域验证由本项目实现。训练从随机初始化开始，未下载或分发上游 COCO 预训练权重。不宣称完全复现上游 COCO 性能。

训练环境中的 PyTorch、NumPy、ONNX、ONNX Runtime、Pillow 及传递依赖各自许可证随其安装包提供。发布运行库时须同时保留该版本库中的 LICENSE/ThirdPartyNotices；本目录的 YOLOX 许可证不能替代运行库许可证。
