"""Generate split-isolated audiovisual scenes using the real C++ SRP executable."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

import numpy as np
from PIL import Image

from scene_model import (FRAME_COUNT, HEIGHT, WIDTH, canonical_hash, geometry_document,
                         make_scene, render_rgb, scene_input, synthesize_audio)


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def validate_heat(result: dict, scene_id: str, columns: int, rows: int) -> np.ndarray:
    if result.get("format") != "micarray128-srp-result-v1" or result.get("scene_id") != scene_id:
        raise ValueError("SRP result identity mismatch")
    heat = np.asarray(result["heat"], dtype=np.float32)
    if heat.size == 0:
        # Silence/invalid processing is explicit. A zero plane carries no peak;
        # the original C++ reason and counts remain in the saved result JSON.
        if not result.get("reason"):
            raise ValueError("empty SRP output must include an explicit reason")
        return np.zeros((rows, columns), dtype=np.float32)
    if result.get("columns") != columns or result.get("rows") != rows:
        raise ValueError("SRP grid shape mismatch")
    if heat.size != columns * rows or not np.isfinite(heat).all() or np.any(heat < 0) or np.any(heat > 1.00001):
        raise ValueError("SRP heat must be finite normalized row-major response")
    return heat.reshape(rows, columns)


def generate_sample(output: Path, split: str, index: int, executable: Path,
                    master_seed: int, keep_pcm: bool, difficulty: str = "mixed",
                    runtime_directory: Path | None = None) -> dict:
    scene = make_scene(split, index, master_seed, difficulty=difficulty)
    case_dir = output / "scenes" / scene["scene_id"]
    case_dir.mkdir(parents=True, exist_ok=False)
    rgb = render_rgb(scene)
    audio = synthesize_audio(scene)
    pcm_path, input_path, heat_path = case_dir / "audio.f32", case_dir / "input.json", case_dir / "heat.json"
    audio.tofile(pcm_path)
    scene["pcm_sha256"] = hashlib.sha256(audio.tobytes()).hexdigest()
    scene["rgb_sha256"] = hashlib.sha256(rgb.tobytes()).hexdigest()
    scene["geometryHash"] = canonical_hash(geometry_document())
    scene["sample_rate"] = 48000
    scene["frames"] = FRAME_COUNT
    write_json(input_path, scene_input(scene, "audio.f32"))
    # There is no Python substitute/fallback map. A missing/failing C++ model
    # aborts the dataset instead of emitting a deceptively plausible heat map.
    environment = os.environ.copy()
    if runtime_directory is not None:
        environment["PATH"] = str(runtime_directory) + os.pathsep + environment.get("PATH", "")
    run = subprocess.run([str(executable), "--input", str(input_path), "--output", str(heat_path)],
                         check=False, capture_output=True, text=True, timeout=60, env=environment)
    if run.returncode != 0:
        raise RuntimeError(f"SRP exporter failed ({run.returncode}) for {scene['scene_id']}: {run.stderr[-2000:]}")
    result = json.loads(heat_path.read_text(encoding="utf-8"))
    heat = validate_heat(result, scene["scene_id"], 72, 45)
    valid_channels = result.get("valid_channels")
    if not isinstance(valid_channels, int) or not 0 <= valid_channels <= 128 - len(scene["disabled_channels"]):
        raise ValueError("C++ valid channel count exceeds enabled input count")
    target_dir = output / split
    target_dir.mkdir(exist_ok=True)
    npz_path = target_dir / f"{scene['scene_id']}.npz"
    objects = scene["objects"]
    np.savez_compressed(npz_path, rgb=rgb, heat=heat,
                        boxes=np.asarray([item["box_cxcywh"] for item in objects], dtype=np.float32),
                        classes=np.zeros(len(objects), dtype=np.int64),
                        active=np.asarray([item["active"] for item in objects], dtype=np.float32),
                        points=np.asarray([item["point_uv"] for item in objects], dtype=np.float32))
    write_json(npz_path.with_suffix(".json"), scene)
    write_json(case_dir / "truth.json", scene)
    Image.fromarray(rgb).save(case_dir / "rgb.png")
    if not keep_pcm:
        pcm_path.unlink()  # only this newly generated, successfully consumed file
    return {
        "sample_id": scene["scene_id"], "split": split,
        "scenarioSeed": str(scene["scenarioSeed"]), "waveformSeed": str(scene["waveformSeed"]),
        "npz_path": npz_path.relative_to(output).as_posix(),
        "truth_path": (case_dir / "truth.json").relative_to(output).as_posix(),
        "input_path": input_path.relative_to(output).as_posix(),
        "heat_path": heat_path.relative_to(output).as_posix(),
        "rgb_path": (case_dir / "rgb.png").relative_to(output).as_posix(),
        "pcm_retained": keep_pcm, "active_count": scene["active_count"],
        "difficulty": scene["difficulty"], "near_pair": scene["near_pair"],
        "disabled_count": len(scene["disabled_channels"]),
        "pcm_sha256": scene["pcm_sha256"], "rgb_sha256": scene["rgb_sha256"],
        "srp_elapsed_ms": result.get("elapsed_ms"), "srp_valid_channels": valid_channels,
        "srp_reason": result.get("reason"),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--srp-exe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="new output directory; refuses overwrite")
    parser.add_argument("--train", type=int, default=2000)
    parser.add_argument("--val", type=int, default=300)
    parser.add_argument("--test", type=int, default=300)
    parser.add_argument("--seed", type=int, default=128202610)
    parser.add_argument("--keep-pcm", action="store_true")
    parser.add_argument("--difficulty", choices=("mixed", "ideal", "stress"), default="mixed")
    args = parser.parse_args()
    executable, output = args.srp_exe.resolve(), args.output.resolve()
    if not executable.is_file():
        parser.error("--srp-exe must be the built C++ exporter")
    if min(args.train, args.val, args.test) < 0 or args.train + args.val + args.test == 0:
        parser.error("split sizes must be nonnegative and at least one sample is required")
    if output.exists():
        parser.error("output already exists; use a new directory to preserve evidence")
    output.mkdir(parents=True)
    # Freeze the DSP program once: another developer may rebuild its original
    # target while a long dataset run is in progress. DLLs are resolved from
    # the original build/runtime directory, never from an arbitrary download.
    source_executable = executable
    engine_dir = output / "engine"
    engine_dir.mkdir()
    executable = engine_dir / source_executable.name
    shutil.copy2(source_executable, executable)
    metadata = {"format": "micarray128-av-dataset-v1", "status": "generating", "master_seed": args.seed,
                "counts": {"train": args.train, "val": args.val, "test": args.test},
                "image_width": WIDTH, "image_height": HEIGHT, "grid_columns": 72, "grid_rows": 45,
                "heat_resize": "bilinear align_corners=False", "difficulty": args.difficulty,
                "srp_executable": str(executable), "srp_executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                "srp_source_executable": str(source_executable),
                "generator_sha256": {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                                     for name in ("scene_model.py", "generate_dataset.py", "fermat_v5_coordinates.json")},
                "geometryHash": canonical_hash(geometry_document()), "geometry_channel_mapping_confirmed": False,
                "scope": "procedural visual domain and simulated acoustics; not real camera or physical-array acceptance"}
    write_json(output / "dataset.json", metadata)
    started = time.monotonic()
    completed = 0
    try:
        with (output / "manifest.jsonl").open("x", encoding="utf-8") as manifest:
            for split in ("train", "val", "test"):
                for index in range(getattr(args, split)):
                    row = generate_sample(output, split, index, executable, args.seed, args.keep_pcm, args.difficulty,
                                          source_executable.parent)
                    manifest.write(json.dumps(row, allow_nan=False) + "\n")
                    manifest.flush()
                    completed += 1
                    if completed % 25 == 0:
                        print(f"{completed} samples, {time.monotonic()-started:.1f}s", flush=True)
    except Exception as error:
        metadata.update(status="failed", completed=completed, error=str(error), elapsed_seconds=time.monotonic()-started)
        write_json(output / "dataset.json", metadata)
        raise
    metadata.update(status="complete", completed=completed, elapsed_seconds=time.monotonic()-started)
    write_json(output / "dataset.json", metadata)
    print(json.dumps(metadata, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
