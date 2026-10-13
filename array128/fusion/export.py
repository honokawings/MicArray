"""Export trained YOLOX fusion weights and verify the real ONNX Runtime output."""

import argparse
import hashlib
import json
import time
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import torch

from .data import FusionDataset, validate_dataset
from .model import FusionYoloXNano, HEIGHT, OUTPUT_LAYOUT, WIDTH


def geometry_metadata(dataset, provenance):
    """Bind model coordinates to the exact channel-ordered training geometry."""
    filename = "fermat_v5_coordinates.json"
    path = Path(dataset) / filename
    if not path.exists():
        path = Path(__file__).resolve().parents[1] / "simulation" / filename
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    expected = provenance.get("generator_sha256", {}).get(filename)
    if not expected or digest != expected:
        raise ValueError("Geometry coordinate source differs from the training dataset")
    source = json.loads(data)
    points = sorted(source["coordinates"], key=lambda item: item["channel"])
    if [item["channel"] for item in points] != list(range(128)):
        raise ValueError("Training geometry must contain each channel 0..127 exactly once")
    coordinates = [[item["x_m"], item["y_m"], item["z_m"]] for item in points]
    if not np.isfinite(np.asarray(coordinates, dtype=np.float64)).all():
        raise ValueError("Training geometry contains nonfinite coordinates")
    return {"geometry_coordinates_m": coordinates,
            "geometry_coordinates_source_sha256": digest,
            "geometry_coordinates_source_name": filename,
            "geometry_candidate_hash": source["candidate_hash"],
            "geometry_channel_mapping_confirmed": False}


def srp_settings_metadata(dataset):
    """Read frozen training inputs, never test labels, to bind SRP preprocessing."""
    root = Path(dataset).resolve()
    manifest = (root / "manifest.jsonl").read_text(encoding="utf-8").splitlines()
    expected, count = None, 0
    source_hash = hashlib.sha256()
    for line in manifest:
        if not line.strip():
            continue
        row = json.loads(line)
        if row["split"] != "train":
            continue
        path = (root / row["input_path"]).resolve()
        if not path.is_relative_to(root):
            raise ValueError("Frozen SRP input path must stay inside the dataset")
        data = path.read_bytes()
        document = json.loads(data)
        settings = {"max_sources": document["max_sources"],
                    "minimum_source_separation_deg": document["minimum_source_separation_deg"],
                    "sound_speed_m_s": document["sound_speed_m_s"],
                    "window_samples": document["pcm"]["frames"]}
        if document["pcm"]["sample_rate"] != 48000 or document["pcm"]["channels"] != 128:
            raise ValueError("Training audio format differs from the supported deployment contract")
        if document["grid"] != {"columns": 72, "rows": 45}:
            raise ValueError("Training SRP grid differs from the supported deployment contract")
        if expected is not None and settings != expected:
            raise ValueError("Training SRP settings are inconsistent between scenes")
        expected = settings
        source_hash.update(row["input_path"].encode("utf-8"))
        source_hash.update(hashlib.sha256(data).digest())
        count += 1
    if not count:
        raise ValueError("No frozen training SRP inputs found")
    return {"training_srp_settings": expected, "training_sample_rate_hz": 48000,
            "training_srp_settings_validated_inputs": count,
            "training_srp_settings_source_sha256": source_hash.hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    torch.set_num_threads(args.threads)
    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    if checkpoint.get("epoch", 0) < 1 or "validation" not in checkpoint:
        raise ValueError("Only an actually trained and validated checkpoint may be exported")
    provenance = checkpoint["configuration"].get("dataset_provenance", {})
    current = validate_dataset(args.dataset)
    for key in ("dataset_metadata_sha256", "manifest_sha256"):
        if provenance.get(key) != current[key]:
            raise ValueError("Export dataset differs from the checkpoint's training dataset")
    geometry = geometry_metadata(args.dataset, provenance)
    srp_settings = srp_settings_metadata(args.dataset)
    model = FusionYoloXNano().eval()
    model.load_state_dict(checkpoint["model"])
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    if (output / "fusion.onnx").exists():
        raise ValueError("Refusing to replace an existing model export")
    dummy = torch.zeros(1, 4, HEIGHT, WIDTH)
    model_path = output / "fusion.onnx"
    torch.onnx.export(model, dummy, model_path, input_names=["rgb_srp"],
                      output_names=["detections"], opset_version=17, dynamo=False)
    graph = onnx.load(str(model_path))
    onnx.checker.check_model(graph)
    options = ort.SessionOptions()
    options.intra_op_num_threads = args.threads
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(str(model_path), sess_options=options,
                                   providers=["CPUExecutionProvider"])
    validation = FusionDataset(args.dataset, "val")
    comparisons, elapsed = [], []
    for index in range(min(8, len(validation))):
        image, _, name = validation[index]
        batch = image[None].numpy()
        with torch.no_grad():
            expected = model(image[None]).numpy()
        start = time.perf_counter()
        actual = session.run(["detections"], {"rgb_srp": batch})[0]
        elapsed.append(1000 * (time.perf_counter() - start))
        maximum = float(np.max(np.abs(actual - expected)))
        if not np.allclose(actual, expected, atol=1e-4, rtol=1e-4):
            raise RuntimeError(f"ONNX equivalence failed for {name}: {maximum}")
        comparisons.append({"sample": name, "max_absolute_error": maximum})
        if index == 0:
            np.savez_compressed(output / "golden_input_output.npz", input=batch, output=actual)
    metadata = {
        "format": "micarray128-yolox-fusion-v1",
        "architecture": "YOLOX-Nano-derived-RGB-SRP-point-active",
        "domain": "simulation_only",
        "class_names": ["visible_candidate_simulation_domain"],
        "class_meaning_zh": "可见候选目标（仿真域），不代表真实人物或设备语义识别",
        "input_name": "rgb_srp", "input_shape": [1, 4, HEIGHT, WIDTH],
        "input_dtype": "float32", "channel_order": ["R", "G", "B", "SRP"],
        "rgb_normalization": "uint8_div_255", "srp_normalization": "cpp_response_0_1",
        "srp_resize": "bilinear_half_pixel_align_corners_false",
        "srp_color_map_or_focus_transform": False,
        "output_name": "detections", "output_shape": [1, 1260, 9],
        "output_layout": OUTPUT_LAYOUT,
        "box_format": "normalized_cxcywh", "source_point_format": "normalized_video_uv",
        "source_point_origin": "independent_learned_regression_head_not_box_center_or_srp_peak",
        "confidence_rule": "objectness_times_class_0",
        "confidence_threshold": checkpoint.get("thresholds", {}).get("confidence", 0.15),
        "active_threshold": checkpoint.get("thresholds", {}).get("activity", 0.5),
        "threshold_selection": checkpoint.get("thresholds", {}).get("selected_on", "fixed_defaults"),
        "nms_iou": 0.45, "max_detections": 16,
        "projection": "pinhole", "audio_video_synchronization": "host_approximate_not_hardware_synchronous",
        "geometry_reference_hash": provenance["geometryHash"],
        "srp_executable_sha256": provenance["srp_executable_sha256"],
        "training_grid_shape": [45, 72],
        "training_srp_grid": [72, 45],
        "training_epoch": checkpoint["epoch"], "training_configuration": checkpoint["configuration"],
        "validation": checkpoint["validation"],
        "onnx_equivalence": comparisons, "onnx_cpu_ms_median": float(np.median(elapsed)),
        "onnx_cpu_ms_max": max(elapsed), "onnxruntime_version": ort.__version__,
        "model_sha256": hashlib.sha256(model_path.read_bytes()).hexdigest(),
        "checkpoint_sha256": hashlib.sha256(Path(args.checkpoint).read_bytes()).hexdigest(),
        "physical_generalization_verified": False,
        "super_resolution_verified": False,
        "release_status": "interface_only_do_not_use_for_performance" if (Path(args.checkpoint).parent / "NOT_FOR_PERFORMANCE.txt").exists() else "candidate_requires_independent_evaluation",
        "limitations": ["Synthetic training domain only; real paired audiovisual data not validated",
                        "Sub-grid regression is implemented; improved physical resolution requires independent evaluation"],
    } | geometry | srp_settings
    (output / "fusion.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"model": str(model_path), "sha256": metadata["model_sha256"],
                      "equivalence_samples": len(comparisons), "cpu_ms_median": metadata["onnx_cpu_ms_median"]}), flush=True)


if __name__ == "__main__":
    main()
