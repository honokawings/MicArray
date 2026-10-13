"""Model-output-only postprocessing shared by evaluation and ONNX smoke tests."""

import numpy as np


def decode_detections(output, confidence=0.2, active_threshold=0.5, iou_threshold=0.45,
                      max_detections=16):
    output = np.asarray(output, dtype=np.float32)
    if output.ndim != 2 or output.shape[1] != 9:
        raise ValueError("Expected YOLOX fusion [anchors,9] output")
    if not np.isfinite(output).all():
        raise ValueError("Nonfinite network output")
    score = output[:, 4] * output[:, 5]
    selected = ((score >= confidence) & (output[:, 8] >= active_threshold)
                & (output[:, 2:4] > 0).all(1)
                & (output[:, 6:8] >= 0).all(1) & (output[:, 6:8] <= 1).all(1))
    candidates = np.flatnonzero(selected)
    candidates = candidates[np.argsort(-(score[candidates] * output[candidates, 8]), kind="stable")]
    kept = []
    while len(candidates) and len(kept) < max_detections:
        current = candidates[0]
        kept.append(int(current))
        rest = candidates[1:]
        if not len(rest):
            break
        lo = np.maximum(output[current, :2] - output[current, 2:4] / 2,
                        output[rest, :2] - output[rest, 2:4] / 2)
        hi = np.minimum(output[current, :2] + output[current, 2:4] / 2,
                        output[rest, :2] + output[rest, 2:4] / 2)
        intersection = np.maximum(hi - lo, 0).prod(1)
        union = output[current, 2:4].prod() + output[rest, 2:4].prod(1) - intersection
        candidates = rest[intersection / np.maximum(union, 1e-8) <= iou_threshold]
    return [{"box_cxcywh": output[i, :4].tolist(), "class_id": 0,
             "confidence": float(score[i]), "active_probability": float(output[i, 8]),
             "source_point_uv": output[i, 6:8].tolist()} for i in kept]


def point_metrics(predictions, truths, tolerance_pixels=8.0, width=320, height=192):
    """Exact one-to-one small-set matching; independent scoring adds angular gates."""
    true_positive = false_positive = false_negative = 0
    errors = []
    exact_count = 0
    for prediction, truth in zip(predictions, truths):
        targets = np.asarray(truth, dtype=np.float32).reshape(-1, 2)
        points = np.asarray([d["source_point_uv"] for d in prediction], dtype=np.float32).reshape(-1, 2)
        exact_count += int(len(points) == len(targets))
        if len(targets) > 16:
            raise ValueError("Validation matcher supports at most 16 simultaneous ground-truth sources")
        # DP over truth subsets maximizes matches first and minimizes distance
        # second. Greedy matching can wrongly lose a nearby second source.
        states = {0: (0.0, [])}
        for point in points:
            updated = dict(states)
            distances = np.linalg.norm((point[None] - targets) * [width, height], axis=1)
            for mask, (cost, chosen) in states.items():
                for index, distance in enumerate(distances):
                    if mask & (1 << index) or distance > tolerance_pixels:
                        continue
                    new_mask = mask | (1 << index)
                    value = (cost + float(distance), chosen + [float(distance)])
                    if new_mask not in updated or value[0] < updated[new_mask][0]:
                        updated[new_mask] = value
            states = updated
        matched_mask = min(states, key=lambda mask: (-mask.bit_count(), states[mask][0]))
        matched = matched_mask.bit_count()
        true_positive += matched
        false_positive += len(points) - matched
        false_negative += len(targets) - matched
        errors.extend(states[matched_mask][1])
    precision = true_positive / max(true_positive + false_positive, 1)
    recall = true_positive / max(true_positive + false_negative, 1)
    return {"tp": true_positive, "fp": false_positive, "fn": false_negative,
            "precision": precision, "recall": recall,
            "f1": 2 * precision * recall / max(precision + recall, 1e-12),
            "exact_count_rate": exact_count / max(len(truths), 1),
            "matched_point_error_px_median": float(np.median(errors)) if errors else None,
            "matched_point_error_px_p95": float(np.percentile(errors, 95)) if errors else None,
            "point_match_gate_px": tolerance_pixels, "samples": len(truths)}
