"""Run predeclared fresh challenges without training or threshold selection."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

import numpy as np
import onnxruntime as ort
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from array128.fusion.data import prepare_input
from array128.fusion.inference import decode_detections
from evaluate import aggregate, score_scene
from generate_dataset import validate_heat, write_json
from scene_model import canonical_hash, geometry_document, scene_input, synthesize_audio
from stress_report import write_report
from stress_scenarios import (FAMILIES, MASTER_SEED, challenge_rgb, live_gate_reason,
                             make_challenge, make_plan)


def file_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def wilson_interval(successes: int, count: int) -> list[float] | None:
    if count < 0 or not 0 <= successes <= count:
        raise ValueError("invalid binomial counts")
    if count == 0:
        return None
    z = 1.959963984540054
    p, z2 = successes / count, z * z
    denominator = 1 + z2 / count
    center = (p + z2 / (2 * count)) / denominator
    radius = z * math.sqrt(p * (1 - p) / count + z2 / (4 * count * count)) / denominator
    return [max(0.0, center - radius), min(1.0, center + radius)]


def summary(records: list[dict]) -> dict:
    methods = ("raw_fusion", "gated_fusion", "srp")
    metrics = {method: aggregate([r["scores"][method] for r in records]) for method in methods}
    near = [r["scores"]["raw_fusion"] for r in records if r["scores"]["raw_fusion"]["near_pair"]]
    successes = sum(r["near_pair_strict_resolved"] for r in near)
    weak = [1 in r["scores"]["raw_fusion"]["matched_truth_indices"] for r in records
            if r["spec"]["family"] == "imbalance"]
    return {"metrics": metrics, "near_successes": successes,
            "near_wilson95": wilson_interval(successes, len(near)),
            "weak_source_recall": float(np.mean(weak)) if weak else None,
            "gated_cases": sum(bool(r["gate_reason"]) for r in records)}


def previous_data_audit(prior: Path, metadata: dict) -> dict:
    expected = metadata["training_configuration"]["dataset_provenance"]["manifest_sha256"]
    if file_hash(prior / "manifest.jsonl") != expected:
        raise ValueError("prior dataset manifest differs from frozen training provenance")
    rows = [json.loads(line) for line in (prior / "manifest.jsonl").read_text(encoding="utf-8").splitlines()]
    seeds = set()
    for row in rows:
        scene = json.loads((prior / row["truth_path"]).read_text(encoding="utf-8"))
        seeds.update([scene["scenarioSeed"], scene["waveformSeed"]])
        seeds.update(item["waveform_seed"] for item in scene["objects"])
    return {"seeds": seeds, "rgb": {row["rgb_sha256"] for row in rows},
            "pcm": {row["pcm_sha256"] for row in rows}, "manifest_sha256": expected}


def run_case(scene: dict, output: Path, executable: Path, session, metadata: dict,
             prior: dict) -> dict:
    sid = scene["scene_id"]
    case_dir = output / "scenes" / sid
    case_dir.mkdir(parents=True, exist_ok=False)
    used_seeds = {scene["scenarioSeed"], scene["waveformSeed"]} | {o["waveform_seed"] for o in scene["objects"]}
    if used_seeds & prior["seeds"]:
        raise ValueError("challenge seed overlaps old train/val/test")
    rgb = challenge_rgb(scene)
    audio = synthesize_audio(scene)
    pcm_hash = hashlib.sha256(audio.tobytes()).hexdigest()
    rgb_hash = hashlib.sha256(rgb.tobytes()).hexdigest()
    if pcm_hash in prior["pcm"] or rgb_hash in prior["rgb"]:
        raise ValueError("challenge content overlaps old train/val/test")
    audio.tofile(case_dir / "audio.f32")
    write_json(case_dir / "input.json", scene_input(scene, "audio.f32"))
    write_json(case_dir / "truth.json", scene)
    Image.fromarray(rgb).save(case_dir / "rgb.png")
    process = subprocess.run([str(executable), "--input", str(case_dir / "input.json"),
                              "--output", str(case_dir / "srp.json")],
                             check=False, capture_output=True, timeout=60)
    if process.returncode:
        raise RuntimeError(f"C++ SRP failed for {sid}: {process.stderr[-2000:]!r}")
    srp = json.loads((case_dir / "srp.json").read_text(encoding="utf-8"))
    heat = validate_heat(srp, sid, 72, 45)
    if not isinstance(srp["valid_channels"], int) or not 0 <= srp["valid_channels"] <= 128 - len(scene["disabled_channels"]):
        raise ValueError("invalid C++ channel quality count")
    # Only image and acoustic response enter the neural network. Labels/angles
    # are deliberately not in this call or in the C++ input JSON.
    tensor = prepare_input(rgb, heat)[None].numpy()
    started = time.perf_counter()
    raw = session.run(["detections"], {"rgb_srp": tensor})[0][0]
    inference_ms = (time.perf_counter() - started) * 1000
    detections = decode_detections(raw, metadata["confidence_threshold"], metadata["active_threshold"],
                                   metadata["nms_iou"], metadata["max_detections"])
    prediction = {"scene_id": sid, "sources": [
        {"point_uv": item["source_point_uv"], "confidence": item["confidence"],
         "active_probability": item["active_probability"], "box_cxcywh": item["box_cxcywh"], "trusted": True}
        for item in detections]}
    reason = live_gate_reason(scene, srp)
    gated = {"scene_id": sid, "sources": []} if reason else prediction
    scores = {"raw_fusion": score_scene(scene, prediction, 0, 2),
              "gated_fusion": score_scene(scene, gated, 0, 2), "srp": score_scene(scene, srp, 0, 2)}
    record = {"spec": scene["challenge"], "scenario_seed": str(scene["scenarioSeed"]),
              "waveform_seed": str(scene["waveformSeed"]), "gate_reason": reason,
              "scores": scores, "inference_ms": inference_ms, "srp_ms": srp["elapsed_ms"],
              "valid_channels": srp["valid_channels"], "pcm_sha256": pcm_hash, "rgb_sha256": rgb_hash,
              "raw_fusion": prediction, "gated_fusion": gated}
    np.savez_compressed(case_dir / "input.npz", rgb=rgb, heat=heat)
    np.asarray(raw, dtype="<f4").tofile(case_dir / "network_output.f32")
    write_json(case_dir / "predictions.json", record)
    record["files_sha256"] = {name: file_hash(case_dir / name) for name in
        ("audio.f32", "input.json", "truth.json", "rgb.png", "srp.json", "input.npz", "network_output.f32", "predictions.json")}
    return record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--srp-exe", required=True, type=Path, help="the frozen training SRP producer")
    parser.add_argument("--prior-dataset", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=MASTER_SEED)
    parser.add_argument("--limit-replicates", type=int, help="smoke tests only; omit for the formal plan")
    args = parser.parse_args()
    model, executable, output = args.model.resolve(), args.srp_exe.resolve(), args.output.resolve()
    metadata_path = model.with_suffix(".json")
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if metadata["domain"] != "simulation_only" or file_hash(model) != metadata["model_sha256"]:
        raise ValueError("model domain/hash differs from frozen metadata")
    if file_hash(executable) != metadata["srp_executable_sha256"]:
        raise ValueError("use the exact frozen C++ SRP producer from model provenance")
    if canonical_hash(geometry_document()) != metadata["geometry_reference_hash"]:
        raise ValueError("geometry changed from model training")
    if metadata["training_srp_grid"] != [72, 45] or metadata["training_srp_settings"] != {
            "max_sources": 4, "minimum_source_separation_deg": 4.0, "sound_speed_m_s": 343.0, "window_samples": 2048}:
        raise ValueError("unsupported SRP contract")
    prior = previous_data_audit(args.prior_dataset.resolve(), metadata)
    specs = make_plan(args.limit_replicates)
    scenes = [make_challenge(spec, args.seed) for spec in specs]
    if output.exists():
        raise ValueError("output exists; preserve previous evidence")
    if shutil.disk_usage(output.parent).free < len(specs) * 1400000 + 200000000:
        raise ValueError("insufficient free disk for all retained PCM and evidence")
    output.mkdir(parents=True, exist_ok=False)
    (output / "engine").mkdir()
    frozen_exe = output / "engine" / executable.name
    shutil.copy2(executable, frozen_exe)
    (output / "model").mkdir()
    frozen_model = output / "model" / "fusion.onnx"
    shutil.copy2(model, frozen_model)
    shutil.copy2(metadata_path, output / "model" / "fusion.json")
    code_files = [Path(__file__), Path(__file__).with_name("stress_scenarios.py"),
                  Path(__file__).with_name("stress_report.py"), Path(__file__).with_name("scene_model.py"),
                  Path(__file__).with_name("evaluate.py"), Path(__file__).parents[1] / "fusion" / "data.py",
                  Path(__file__).parents[1] / "fusion" / "inference.py"]
    source_hashes = {str(path.relative_to(Path(__file__).parents[1])): file_hash(path) for path in code_files}
    plan = {"format": "micarray128-frozen-challenge-plan-v1", "master_seed": args.seed,
            "sample_count": len(specs), "specs": specs, "source_sha256": source_hashes,
            "model_sha256": file_hash(model), "metadata_sha256": file_hash(metadata_path),
            "srp_executable_sha256": file_hash(executable), "prior_manifest_sha256": prior["manifest_sha256"],
            "thresholds": {"confidence": metadata["confidence_threshold"], "active": metadata["active_threshold"]},
            "threshold_selection": "unchanged_from_prior_validation_only", "match_tolerance_deg": 2.0,
            "example_selection": "first case, first missed case, first false-positive case, first fully correct nonempty case per family",
            "scope": "New seeds; paired interventions, simulation only. No test-time tuning, training or physical acceptance."}
    write_json(output / "plan.json", plan)  # Must exist BEFORE the first inference.
    options = ort.SessionOptions()
    options.intra_op_num_threads = 4
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(str(frozen_model), sess_options=options, providers=["CPUExecutionProvider"])
    records, started = {}, time.monotonic()
    write_json(output / "status.json", {"status": "running", "completed": 0, "planned": len(specs)})
    try:
        with (output / "manifest.jsonl").open("x", encoding="utf-8") as manifest:
            for scene in scenes:
                record = run_case(scene, output, frozen_exe, session, metadata, prior)
                records[scene["scene_id"]] = record
                manifest.write(json.dumps({"scene_id": scene["scene_id"], "files_sha256": record["files_sha256"]}, allow_nan=False) + "\n")
                manifest.flush()
                if len(records) % 20 == 0:
                    elapsed = time.monotonic() - started
                    write_json(output / "status.json", {"status": "running", "completed": len(records), "planned": len(specs), "elapsed_seconds": elapsed})
                    print(f"{len(records)}/{len(specs)} scenes, {elapsed:.1f}s", flush=True)
        families = {}
        for family, (_, values, _) in FAMILIES.items():
            selected = [r for r in records.values() if r["spec"]["family"] == family]
            levels = []
            for value in values:
                rows = [r for r in selected if r["spec"]["value"] == value]
                levels.append({"value": value, "label": rows[0]["spec"]["value_label"], **summary(rows)})
            families[family] = {**summary(selected), "levels": levels,
                                "case_ids": [r["spec"]["scene_id"] for r in selected]}
        report = {"format": "micarray128-challenge-report-v1", "sample_count": len(records),
                  "independent_replicate_groups": len({scene["scenarioSeed"] for scene in scenes}),
                  "model_sha256": plan["model_sha256"], "metadata_sha256": plan["metadata_sha256"],
                  "plan_sha256": file_hash(output / "plan.json"), "thresholds": plan["thresholds"],
                  "prior_overlap": {"seeds": 0, "rgb": 0, "pcm": 0}, "families": families, "cases": records,
                  "timing": {"onnx_ms_median": float(np.median([r["inference_ms"] for r in records.values()])),
                             "onnx_ms_p95": float(np.percentile([r["inference_ms"] for r in records.values()], 95))},
                  "scope": plan["scope"]}
        for source, expected in ((model, plan["model_sha256"]), (metadata_path, plan["metadata_sha256"]),
                                 (executable, plan["srp_executable_sha256"])):
            if file_hash(source) != expected:
                raise RuntimeError("original frozen resource changed during testing")
        for path in code_files:
            if file_hash(path) != source_hashes[str(path.relative_to(Path(__file__).parents[1]))]:
                raise RuntimeError("test source changed during run; do not mix implementations")
        write_json(output / "report.json", report)
        write_report(report, output)
        write_json(output / "status.json", {"status": "complete", "completed": len(records), "planned": len(specs),
                                            "elapsed_seconds": time.monotonic() - started,
                                            "report_sha256": file_hash(output / "report.json")})
    except Exception as error:
        write_json(output / "status.json", {"status": "failed", "completed": len(records), "error": str(error)})
        raise
    print(json.dumps({"output": str(output), "samples": len(records), "reference": families["reference"]["metrics"]}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
