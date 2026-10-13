"""Independent multi-source metrics against physical truth, including empty scenes.

Evaluate production SRP with --srp, or JSONL neural predictions of the form
{"scene_id":"test-000001","sources":[{"point_uv":[.2,.3],"confidence":.8}]}.
Sources may instead provide azimuth_deg/elevation_deg. Matching uses true
spherical angular distance, not image distance or nearest SRP maxima.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from scipy.optimize import linear_sum_assignment

from scene_model import direction_vector, project, unproject, vector_angles


def angular_matrix(predicted: list[tuple[float, float]], truth: list[tuple[float, float]]) -> np.ndarray:
    a = np.asarray([direction_vector(*point) for point in predicted], dtype=np.float64).reshape(-1, 3)
    b = np.asarray([direction_vector(*point) for point in truth], dtype=np.float64).reshape(-1, 3)
    if not np.isfinite(a).all() or not np.isfinite(b).all():
        raise ValueError("nonfinite predicted or true direction")
    return np.rad2deg(np.arccos(np.clip(a @ b.T, -1, 1)))


def match_directions(predicted: list[tuple[float, float]], truth: list[tuple[float, float]],
                     tolerance_deg: float = 2.0) -> dict:
    if not math.isfinite(tolerance_deg) or not 0 < tolerance_deg < 90:
        raise ValueError("angular tolerance must be finite and in (0,90)")
    if not predicted or not truth:
        return {"tp": 0, "fp": len(predicted), "fn": len(truth), "errors_deg": [],
                "matched_truth_indices": [], "matched_prediction_indices": []}
    errors = angular_matrix(predicted, truth)
    # Maximise true matches first, then minimise their angular error. A large
    # penalty avoids choosing one excellent match at the cost of another TP.
    cost = np.where(errors <= tolerance_deg, errors, 1000.0)
    rows, columns = linear_sum_assignment(cost)
    matched = [(int(r), int(c)) for r, c in zip(rows, columns) if errors[r, c] <= tolerance_deg]
    return {"tp": len(matched), "fp": len(predicted) - len(matched), "fn": len(truth) - len(matched),
            "errors_deg": [float(errors[r, c]) for r, c in matched],
            "matched_truth_indices": [c for _, c in matched],
            "matched_prediction_indices": [r for r, _ in matched]}


def prediction_angles(source: dict, camera: dict) -> tuple[float, float]:
    if "point_uv" in source:
        uv = np.asarray(source["point_uv"], dtype=np.float64)
        if uv.shape != (2,) or not np.isfinite(uv).all() or np.any(uv < 0) or np.any(uv > 1):
            raise ValueError("predicted point_uv must be two finite normalised coordinates")
        return vector_angles(unproject(tuple(uv), camera))
    az, el = float(source["azimuth_deg"]), float(source["elevation_deg"])
    if not math.isfinite(az) or not math.isfinite(el) or not -180 <= az <= 180 or not -90 <= el <= 90:
        raise ValueError("invalid predicted azimuth/elevation")
    return az, el


def snap_predictions_to_grid(prediction: dict, camera: dict, columns: int, rows: int) -> dict:
    """Quantization-only ablation of predicted points; never consult source truth.

    This retains the production detector's count/confidence decisions. It is
    deliberately NOT presented as a separate unrefined SRP peak detector.
    """
    if columns < 2 or rows < 2:
        raise ValueError("grid dimensions must each be at least two")
    sources = []
    for item in prediction.get("sources", []):
        if not item.get("trusted", True):
            sources.append(dict(item))
            continue
        u, v = project(direction_vector(*prediction_angles(item, camera)), camera)
        if not math.isfinite(u) or not math.isfinite(v) or not -1e-9 <= u <= 1 + 1e-9 or not -1e-9 <= v <= 1 + 1e-9:
            raise ValueError("trusted SRP point lies outside the video projection")
        u, v = min(1.0, max(0.0, u)), min(1.0, max(0.0, v))
        point = [round(u * (columns - 1)) / (columns - 1),
                 round(v * (rows - 1)) / (rows - 1)]
        sources.append({**item, "point_uv": point})
    return {**prediction, "sources": sources}


def score_scene(scene: dict, prediction: dict, confidence_threshold: float = .25,
                tolerance_deg: float = 2.0) -> dict:
    if not math.isfinite(confidence_threshold) or not 0 <= confidence_threshold <= 1:
        raise ValueError("confidence threshold must be finite and in [0,1]")
    if prediction.get("scene_id") != scene["scene_id"]:
        raise ValueError("prediction identity does not match truth")
    sources = []
    for item in prediction.get("sources", []):
        confidence = float(item.get("confidence", 1.0))
        if not math.isfinite(confidence) or not 0 <= confidence <= 1:
            raise ValueError("prediction confidence must be finite in [0,1]")
        if confidence >= confidence_threshold and item.get("trusted", True):
            sources.append(prediction_angles(item, scene["camera"]))
    active = [item for item in scene["objects"] if item["active"]]
    truth = [(item["azimuth_deg"], item["elevation_deg"]) for item in active]
    scored = match_directions(sources, truth, tolerance_deg)
    pairwise = angular_matrix(truth, truth)
    minimum_separation = float(np.min(pairwise[np.triu_indices(len(truth), 1)])) if len(truth) > 1 else None
    near_resolved, near_tolerance = None, None
    if scene["near_pair"]:
        near_ids = scene["close_pair_ids"]
        truth_indices = [i for i, item in enumerate(active) if item["source_id"] in near_ids]
        if len(truth_indices) != 2:
            raise ValueError("near-pair truth identity mismatch")
        separation = float(pairwise[truth_indices[0], truth_indices[1]])
        near_tolerance = min(tolerance_deg, separation / 3)
        matching = {truth_index: (prediction_index, error)
                    for prediction_index, truth_index, error in
                    zip(scored["matched_prediction_indices"], scored["matched_truth_indices"], scored["errors_deg"])}
        near_resolved = all(i in matching and matching[i][1] <= near_tolerance for i in truth_indices)
        if near_resolved:
            p0, p1 = [sources[matching[i][0]] for i in truth_indices]
            near_resolved = bool(angular_matrix([p0], [p1])[0, 0] >= separation / 2)
    # The grid-cell baseline is computed from true projection, not SRP peaks.
    grid_estimates = []
    for item in active:
        u, v = item["point_uv"]
        cell = (round(u * 71) / 71, round(v * 44) / 44)
        grid_estimates.append(vector_angles(unproject(cell, scene["camera"])))
    quantization = angular_matrix(grid_estimates, truth).diagonal().tolist()
    return {"scene_id": scene["scene_id"], "split": scene["split"], "difficulty": scene["difficulty"],
            "true_count": len(truth), "predicted_count": len(sources),
            "disabled_count": len(scene["disabled_channels"]), "minimum_separation_deg": minimum_separation,
            "near_pair": scene["near_pair"], "complete_resolved": scored["tp"] == len(truth) and scored["fp"] == 0,
            "near_pair_strict_resolved": near_resolved, "near_pair_tolerance_deg": near_tolerance,
            "grid_quantization_errors_deg": [float(x) for x in quantization], **scored}


def aggregate(rows: list[dict]) -> dict:
    tp, fp, fn = (sum(item[key] for item in rows) for key in ("tp", "fp", "fn"))
    errors = [error for item in rows for error in item["errors_deg"]]
    empty = [item for item in rows if item["true_count"] == 0]
    near = [item for item in rows if item["near_pair"]]
    def percentile(values: list[float], q: int) -> float | None:
        return float(np.percentile(values, q)) if values else None
    precision, recall = (tp / (tp + fp) if tp + fp else None), (tp / (tp + fn) if tp + fn else None)
    return {"scenes": len(rows), "tp": tp, "fp": fp, "fn": fn,
            "precision": precision, "recall": recall,
            "f1": 2 * tp / (2 * tp + fp + fn) if 2 * tp + fp + fn else None,
            "matched_angle_p50_deg": percentile(errors, 50), "matched_angle_p95_deg": percentile(errors, 95),
            "matched_angle_max_deg": max(errors) if errors else None,
            "all_sources_resolved_rate": float(np.mean([r["complete_resolved"] for r in rows])) if rows else None,
            "count_mae": float(np.mean([abs(r["true_count"] - r["predicted_count"]) for r in rows])) if rows else None,
            "silent_scenes": len(empty), "silent_scene_false_alarm_rate": float(np.mean([r["fp"] > 0 for r in empty])) if empty else None,
            "near_pair_scenes": len(near),
            "near_pair_resolved_rate": float(np.mean([r["near_pair_strict_resolved"] for r in near])) if near else None,
            "near_scene_all_sources_resolved_rate": float(np.mean([r["complete_resolved"] for r in near])) if near else None,
            "grid_quantization_p50_deg": percentile([v for row in rows for v in row["grid_quantization_errors_deg"]], 50),
            "scope": "Error statistics describe matched true positives ONLY; recall/misses must be read alongside them."}


def evaluate_dataset(dataset: Path, predictions: dict[str, dict] | None = None,
                     split: str = "test", confidence_threshold: float = .25,
                     tolerance_deg: float = 2.0, snap_srp_to_grid: bool = False) -> dict:
    metadata = json.loads((dataset / "dataset.json").read_text(encoding="utf-8"))
    if metadata.get("status") != "complete":
        raise ValueError("development/incomplete datasets are not valid accuracy evidence")
    manifest = [json.loads(line) for line in (dataset / "manifest.jsonl").read_text(encoding="utf-8").splitlines() if line]
    selected = [item for item in manifest if item["split"] == split]
    if not selected:
        raise ValueError("requested evaluation split is empty")
    expected = {item["sample_id"] for item in selected}
    if predictions is not None and set(predictions) != expected:
        raise ValueError("prediction set must exactly equal the held-out manifest split (no missing/extra cases)")
    if predictions is not None and snap_srp_to_grid:
        raise ValueError("the quantization-only baseline applies to production SRP, not neural predictions")
    rows = []
    for item in selected:
        scene = json.loads((dataset / item["truth_path"]).read_text(encoding="utf-8"))
        prediction = predictions[item["sample_id"]] if predictions is not None else json.loads((dataset / item["heat_path"]).read_text(encoding="utf-8"))
        if snap_srp_to_grid:
            prediction = snap_predictions_to_grid(prediction, scene["camera"], metadata["grid_columns"], metadata["grid_rows"])
        rows.append(score_scene(scene, prediction, confidence_threshold, tolerance_deg))
    return {"format": "micarray128-multisource-evaluation-v1", "split": split,
            "evaluator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "dataset_manifest_sha256": hashlib.sha256((dataset / "manifest.jsonl").read_bytes()).hexdigest(),
            "srp_executable_sha256": metadata["srp_executable_sha256"], "geometryHash": metadata["geometryHash"],
            "confidence_threshold": confidence_threshold, "match_tolerance_deg": tolerance_deg,
            "method": ("production SRP points snapped to response grid; quantization-only ablation, same detector/count decisions"
                       if snap_srp_to_grid else "production SRP" if predictions is None else "submitted predictions"),
            "overall": aggregate(rows),
            "by_source_count": {str(n): aggregate([r for r in rows if r["true_count"] == n]) for n in range(5)},
            "by_difficulty": {key: aggregate([r for r in rows if r["difficulty"] == key]) for key in ("ideal", "stress")},
            "by_channel_mask": {key: aggregate([r for r in rows if bool(r["disabled_count"]) == masked])
                                for key, masked in (("all_enabled", False), ("defective_channels_masked", True))},
            "cases": rows}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", required=True, type=Path)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--srp", action="store_true")
    source.add_argument("--srp-grid", action="store_true", help="snap production SRP predictions to grid, preserving detection/count decisions")
    source.add_argument("--predictions", type=Path)
    parser.add_argument("--split", choices=("train", "val", "test"), default="test")
    parser.add_argument("--confidence", type=float, default=.25)
    parser.add_argument("--tolerance-deg", type=float, default=2.0)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    predictions = None
    if args.predictions:
        predictions = {}
        for line in args.predictions.read_text(encoding="utf-8").splitlines():
            row = json.loads(line)
            if row["scene_id"] in predictions:
                raise ValueError("duplicate prediction scene")
            predictions[row["scene_id"]] = row
    report = evaluate_dataset(args.dataset.resolve(), predictions, args.split, args.confidence, args.tolerance_deg, args.srp_grid)
    if args.predictions:
        report["predictions_sha256"] = hashlib.sha256(args.predictions.read_bytes()).hexdigest()
    with args.output.open("x", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, allow_nan=False)
    print(json.dumps(report["overall"], indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
