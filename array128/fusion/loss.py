# Copyright (c) Megvii Inc. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# SimOTA matching and YOLOX detection losses adapted from official yolo_head.py.
# Local additions train independent point regression and acoustic activity.
"""YOLOX SimOTA detection loss with ground-truth source-point supervision."""

import torch
from torch.nn import functional as F


def pair_iou(boxes_a, boxes_b):
    lo_a, hi_a = boxes_a[:, :2] - boxes_a[:, 2:] / 2, boxes_a[:, :2] + boxes_a[:, 2:] / 2
    lo_b, hi_b = boxes_b[:, :2] - boxes_b[:, 2:] / 2, boxes_b[:, :2] + boxes_b[:, 2:] / 2
    wh = (torch.minimum(hi_a[:, None], hi_b) - torch.maximum(lo_a[:, None], lo_b)).clamp(min=0)
    intersection = wh.prod(-1)
    return intersection / (boxes_a[:, 2:].prod(-1)[:, None] + boxes_b[:, 2:].prod(-1)
                           - intersection + 1e-8)


@torch.no_grad()
def simota(prediction, truth_boxes, truth_classes, grid, strides, classes):
    centers = (grid + 0.5) * strides
    geometry = ((centers[None] - truth_boxes[:, None, :2]).abs()
                < 1.5 * strides[None]).all(-1)
    candidate = geometry.any(0)
    # A tiny box at the image boundary still needs a valid candidate.
    if not candidate.any():
        candidate[(centers - truth_boxes[0, :2]).square().sum(-1).argmin()] = True
    overlaps = pair_iou(truth_boxes, prediction[candidate, :4])
    probabilities = (prediction[candidate, 4:5].sigmoid()
                     * prediction[candidate, 5:5 + classes].sigmoid()).sqrt()
    expected = F.one_hot(truth_classes.long(), classes).float()
    class_cost = F.binary_cross_entropy(
        probabilities[None].expand(len(truth_boxes), -1, -1),
        expected[:, None].expand(-1, len(probabilities), -1), reduction="none").sum(-1)
    cost = class_cost - 3.0 * (overlaps + 1e-8).log() + 1e6 * (~geometry[:, candidate])
    matching = torch.zeros_like(cost, dtype=torch.bool)
    count = min(10, overlaps.shape[1])
    dynamic_k = overlaps.topk(count, dim=1).values.sum(1).int().clamp(min=1)
    for index, k in enumerate(dynamic_k.tolist()):
        matching[index, cost[index].topk(k, largest=False).indices] = True
    conflicts = matching.sum(0) > 1
    if conflicts.any():
        matching[:, conflicts] = False
        matching[cost[:, conflicts].argmin(0), conflicts] = True
    selected = matching.any(0)
    anchors = candidate.nonzero().flatten()[selected]
    objects = matching[:, selected].int().argmax(0)
    quality = (matching * overlaps).sum(0)[selected]
    return anchors, objects, quality


def fusion_loss(predictions, labels, model):
    obj_targets = torch.zeros_like(predictions[..., 4])
    box_loss = predictions.sum() * 0
    class_loss = box_loss
    point_loss = box_loss
    active_loss = box_loss
    positive_count, active_count = 0, 0
    point_index = 5 + model.classes
    for batch, label in enumerate(labels):
        if not len(label["boxes"]):
            continue
        boxes = label["boxes"].to(predictions.device) * model.box_scale
        categories = label["classes"].to(predictions.device)
        active = label["active"].to(predictions.device)
        points = label["points"].to(predictions.device) * model.xy_scale
        anchors, objects, quality = simota(predictions[batch].detach(), boxes, categories,
                                          model.grid, model.stride, model.classes)
        predicted = predictions[batch, anchors]
        obj_targets[batch, anchors] = 1
        positive_count += len(anchors)
        overlaps = pair_iou(predicted[:, :4], boxes[objects]).diagonal()
        box_loss = box_loss + (1 - overlaps.square()).sum()
        expected = F.one_hot(categories[objects].long(), model.classes).float() * quality[:, None]
        class_loss = class_loss + F.binary_cross_entropy_with_logits(
            predicted[:, 5:point_index], expected, reduction="sum")
        active_loss = active_loss + F.binary_cross_entropy_with_logits(
            predicted[:, point_index + 2], active[objects], reduction="sum")
        sounding = active[objects] > 0.5
        if sounding.any():
            active_count += int(sounding.sum())
            scale = model.stride[anchors[sounding]]
            point_loss = point_loss + F.smooth_l1_loss(
                predicted[sounding, point_index:point_index + 2] / scale,
                points[objects[sounding]] / scale, beta=0.25, reduction="sum")
    norm = max(positive_count, 1)
    losses = {
        "box": 5 * box_loss / norm,
        "objectness": F.binary_cross_entropy_with_logits(predictions[..., 4], obj_targets,
                                                          reduction="sum") / norm,
        "class": class_loss / norm,
        "active": 2 * active_loss / norm,
        "point": 2 * point_loss / max(active_count, 1),
    }
    losses["total"] = sum(losses.values())
    return losses
