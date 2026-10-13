"""Evaluate a frozen ONNX model without adjusting weights or test thresholds."""

import argparse
import hashlib
import json
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort

from .data import FusionDataset, validate_dataset
from .inference import decode_detections, point_metrics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--split", choices=["val", "test"], default="test")
    parser.add_argument("--output", required=True)
    parser.add_argument("--predictions-dir", help="New directory with one JSONL per ablation for independent angular scoring")
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    model = Path(args.model)
    metadata_bytes = model.with_suffix(".json").read_bytes()
    metadata = json.loads(metadata_bytes)
    if hashlib.sha256(model.read_bytes()).hexdigest() != metadata["model_sha256"]:
        raise ValueError("Model SHA-256 differs from the frozen metadata")
    provenance = validate_dataset(args.dataset)
    if (provenance["geometryHash"] != metadata["geometry_reference_hash"]
            or provenance["srp_executable_sha256"] != metadata["srp_executable_sha256"]):
        raise ValueError("Evaluation geometry or SRP producer differs from the frozen model contract")
    destination = Path(args.output)
    if destination.exists():
        raise ValueError("Evaluation output exists; preserve previous evidence")
    options = ort.SessionOptions()
    options.intra_op_num_threads = args.threads
    session = ort.InferenceSession(str(model), sess_options=options, providers=["CPUExecutionProvider"])
    report = {"format": "micarray128-fusion-evaluation-v1", "split": args.split,
              "model_sha256": metadata["model_sha256"], "domain": metadata["domain"],
              "metadata_sha256": hashlib.sha256(metadata_bytes).hexdigest(),
              "dataset_manifest_sha256": provenance["manifest_sha256"],
              "confidence_threshold": metadata["confidence_threshold"],
              "active_threshold": metadata["active_threshold"],
              "threshold_selection": metadata["threshold_selection"], "ablations": {}}
    predictions_directory = Path(args.predictions_dir) if args.predictions_dir else None
    if predictions_directory:
        predictions_directory.mkdir(parents=True, exist_ok=False)
    for ablation in ("none", "rgb_only", "srp_only", "shuffled_srp"):
        dataset = FusionDataset(args.dataset, args.split, ablation)
        predictions, truths, cases, times = [], [], [], []
        for image, label, name in dataset:
            start = time.perf_counter()
            output = session.run(["detections"], {"rgb_srp": image[None].numpy()})[0][0]
            times.append(1000 * (time.perf_counter() - start))
            detections = decode_detections(output, metadata["confidence_threshold"], metadata["active_threshold"],
                                           metadata["nms_iou"], metadata["max_detections"])
            truth = label["points"][label["active"] > 0.5].numpy()
            predictions.append(detections)
            truths.append(truth)
            cases.append({"sample": name, "predictions": detections, "truth_source_points_uv": truth.tolist()})
        report["ablations"][ablation] = {"metrics": point_metrics(predictions, truths), "cases": cases,
                                          "cpu_ms_median": float(np.median(times)), "cpu_ms_p95": float(np.percentile(times, 95))}
        if predictions_directory:
            with (predictions_directory / f"{ablation}.jsonl").open("x", encoding="utf-8") as handle:
                for case in cases:
                    row = {"scene_id": Path(case["sample"]).stem,
                           "sources": [{"point_uv": item["source_point_uv"],
                                        "confidence": item["confidence"], "active_probability": item["active_probability"],
                                        "box_cxcywh": item["box_cxcywh"], "trusted": True}
                                       for item in case["predictions"]]}
                    handle.write(json.dumps(row, allow_nan=False) + "\n")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({key: value["metrics"] for key, value in report["ablations"].items()}), flush=True)


if __name__ == "__main__":
    main()
