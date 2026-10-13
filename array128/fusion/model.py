# Copyright (c) Megvii Inc. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Architecture derived from official YOLOX network_blocks.py, darknet.py,
# yolo_pafpn.py and yolo_head.py. See THIRD_PARTY.md and LICENSE.yolox.
# Local changes: four input channels and independent source-point/activity heads.
"""Compact YOLOX-Nano architecture with learned acoustic point regression.

This is not a pretrained visual semantic model. The default single class means
"visible candidate in the simulation domain"; activity is learned separately.
"""

import math

import torch
from torch import nn

HEIGHT, WIDTH = 192, 320
OUTPUT_LAYOUT = ["cx", "cy", "width", "height", "objectness", "class_0",
                 "source_x", "source_y", "active"]


class Conv(nn.Module):
    def __init__(self, cin, cout, kernel=1, stride=1, groups=1):
        super().__init__()
        self.conv = nn.Conv2d(cin, cout, kernel, stride, (kernel - 1) // 2,
                              groups=groups, bias=False)
        self.bn = nn.BatchNorm2d(cout, eps=1e-3, momentum=0.03)
        self.act = nn.SiLU(inplace=True)

    def forward(self, x):
        return self.act(self.bn(self.conv(x)))


class Depthwise(nn.Sequential):
    def __init__(self, cin, cout, kernel=3, stride=1):
        super().__init__(Conv(cin, cin, kernel, stride, cin), Conv(cin, cout))


class Bottleneck(nn.Module):
    def __init__(self, channels, shortcut=True):
        super().__init__()
        self.layers = nn.Sequential(Conv(channels, channels),
                                    Depthwise(channels, channels))
        self.shortcut = shortcut

    def forward(self, x):
        y = self.layers(x)
        return x + y if self.shortcut else y


class CSP(nn.Module):
    def __init__(self, cin, cout, repeats=1, shortcut=True):
        super().__init__()
        half = cout // 2
        self.left = Conv(cin, half)
        self.right = Conv(cin, half)
        self.blocks = nn.Sequential(*(Bottleneck(half, shortcut)
                                      for _ in range(repeats)))
        self.join = Conv(2 * half, cout)

    def forward(self, x):
        return self.join(torch.cat((self.blocks(self.left(x)), self.right(x)), 1))


class SpatialPyramid(nn.Module):
    def __init__(self, channels):
        super().__init__()
        self.reduce = Conv(channels, channels // 2)
        self.pools = nn.ModuleList(nn.MaxPool2d(k, 1, k // 2) for k in (5, 9, 13))
        self.join = Conv(2 * channels, channels)

    def forward(self, x):
        x = self.reduce(x)
        return self.join(torch.cat([x] + [pool(x) for pool in self.pools], 1))


class Focus(nn.Module):
    def __init__(self):
        super().__init__()
        self.conv = Conv(4 * 4, 16, 3)

    def forward(self, x):
        return self.conv(torch.cat((x[..., ::2, ::2], x[..., 1::2, ::2],
                                    x[..., ::2, 1::2], x[..., 1::2, 1::2]), 1))


class NanoBackbone(nn.Module):
    """Official Nano depth=.33, width=.25, depthwise CSPDarknet topology."""

    def __init__(self):
        super().__init__()
        self.stem = Focus()
        self.dark2 = nn.Sequential(Depthwise(16, 32, stride=2), CSP(32, 32))
        self.dark3 = nn.Sequential(Depthwise(32, 64, stride=2), CSP(64, 64, 3))
        self.dark4 = nn.Sequential(Depthwise(64, 128, stride=2), CSP(128, 128, 3))
        self.dark5 = nn.Sequential(Depthwise(128, 256, stride=2),
                                   SpatialPyramid(256), CSP(256, 256, shortcut=False))

    def forward(self, x):
        x = self.dark2(self.stem(x))
        p3 = self.dark3(x)
        p4 = self.dark4(p3)
        return p3, p4, self.dark5(p4)


class NanoPafpn(nn.Module):
    def __init__(self):
        super().__init__()
        self.backbone = NanoBackbone()
        self.lateral = Conv(256, 128)
        self.top4 = CSP(256, 128, shortcut=False)
        self.reduce = Conv(128, 64)
        self.top3 = CSP(128, 64, shortcut=False)
        self.down3 = Depthwise(64, 64, stride=2)
        self.bottom4 = CSP(128, 128, shortcut=False)
        self.down4 = Depthwise(128, 128, stride=2)
        self.bottom5 = CSP(256, 256, shortcut=False)
        self.up = nn.Upsample(scale_factor=2, mode="nearest")

    def forward(self, x):
        p3, p4, p5 = self.backbone(x)
        p5_lateral = self.lateral(p5)
        top4 = self.top4(torch.cat((self.up(p5_lateral), p4), 1))
        top4_reduced = self.reduce(top4)
        out3 = self.top3(torch.cat((self.up(top4_reduced), p3), 1))
        out4 = self.bottom4(torch.cat((self.down3(out3), top4_reduced), 1))
        out5 = self.bottom5(torch.cat((self.down4(out4), p5_lateral), 1))
        return out3, out4, out5


class DetectionLevel(nn.Module):
    """YOLOX decoupled box/class head plus independent point/activity branches."""

    def __init__(self, channels, classes):
        super().__init__()
        self.stem = Conv(channels, 64)
        self.class_feature = nn.Sequential(Depthwise(64, 64), Depthwise(64, 64))
        self.box_feature = nn.Sequential(Depthwise(64, 64), Depthwise(64, 64))
        self.source_feature = nn.Sequential(Depthwise(64, 64), Depthwise(64, 64))
        self.box = nn.Conv2d(64, 4, 1)
        self.objectness = nn.Conv2d(64, 1, 1)
        self.classifier = nn.Conv2d(64, classes, 1)
        self.source_point = nn.Conv2d(64, 2, 1)
        self.activity = nn.Conv2d(64, 1, 1)
        for layer in (self.objectness, self.classifier):
            nn.init.constant_(layer.bias, -math.log(99.0))
        nn.init.constant_(self.source_point.bias, 0.5)

    def forward(self, x):
        x = self.stem(x)
        box = self.box_feature(x)
        source = self.source_feature(x)
        return torch.cat((self.box(box), self.objectness(box),
                          self.classifier(self.class_feature(x)),
                          self.source_point(source), self.activity(source)), 1)


class FusionYoloXNano(nn.Module):
    def __init__(self, classes=1, height=HEIGHT, width=WIDTH):
        super().__init__()
        if height % 32 or width % 32 or classes < 1:
            raise ValueError("Input dimensions must be multiples of 32; classes >= 1")
        self.classes, self.height, self.width = classes, height, width
        self.pafpn = NanoPafpn()
        self.heads = nn.ModuleList(DetectionLevel(c, classes) for c in (64, 128, 256))
        self.register_buffer("xy_scale", torch.tensor([width, height], dtype=torch.float32))
        self.register_buffer("box_scale", torch.tensor([width, height, width, height], dtype=torch.float32))
        grids, strides = [], []
        for stride in (8, 16, 32):
            yy, xx = torch.meshgrid(torch.arange(height // stride),
                                    torch.arange(width // stride), indexing="ij")
            grids.append(torch.stack((xx, yy), -1).reshape(-1, 2))
            strides.append(torch.full((xx.numel(), 1), float(stride)))
        self.register_buffer("grid", torch.cat(grids).float())
        self.register_buffer("stride", torch.cat(strides))

    def forward(self, images, raw=False):
        values = torch.cat([head(feature).flatten(2).transpose(1, 2)
                            for head, feature in zip(self.heads, self.pafpn(images))], 1)
        box = torch.cat(((values[..., :2] + self.grid) * self.stride,
                         values[..., 2:4].clamp(-10, 10).exp() * self.stride), -1)
        point_index = 5 + self.classes
        point = (values[..., point_index:point_index + 2] + self.grid) * self.stride
        if raw:
            return torch.cat((box, values[..., 4:point_index], point,
                              values[..., point_index + 2:]), -1)
        return torch.cat((box / self.box_scale, values[..., 4:point_index].sigmoid(),
                          point / self.xy_scale, values[..., point_index + 2:].sigmoid()), -1)
