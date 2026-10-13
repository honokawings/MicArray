"""Audit complete synthetic AV datasets without trusting summary counts."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from generate_dataset import validate_heat
from scene_model import direction_vector, project, render_rgb, synthesize_audio


def audit(dataset: Path, regenerate: int = 5) -> dict:
    metadata = json.loads((dataset / "dataset.json").read_text(encoding="utf-8"))
    if metadata.get("status") != "complete":
        raise ValueError("only a completed dataset can be used for training/evaluation")
    executable = Path(metadata["srp_executable"])
    if hashlib.sha256(executable.read_bytes()).hexdigest() != metadata["srp_executable_sha256"]:
        raise ValueError("frozen SRP executable hash mismatch")
    rows = [json.loads(line) for line in (dataset / "manifest.jsonl").read_text(encoding="utf-8").splitlines() if line]
    if len(rows) != sum(metadata["counts"].values()) or len(rows) != metadata["completed"]:
        raise ValueError("manifest count does not match all requested split counts")
    ids, observed_seeds, waveform_seeds, rgb_hashes, pcm_hashes = set(), {}, {}, {}, {}
    counts = {split: 0 for split in ("train", "val", "test")}
    active_histogram = {split: [0] * 5 for split in counts}
    near_count, masked_count, empty_heat_count = 0, 0, 0
    regenerate_indices = set(np.linspace(0, max(0, len(rows) - 1), min(regenerate, len(rows)), dtype=int).tolist())
    for index, row in enumerate(rows):
        sid, split = row["sample_id"], row["split"]
        if sid in ids or split not in counts:
            raise ValueError("duplicate id or invalid split")
        ids.add(sid)
        scene = json.loads((dataset / row["truth_path"]).read_text(encoding="utf-8"))
        if scene["scene_id"] != sid or scene["split"] != split:
            raise ValueError("truth/manifest identity mismatch")
        for mapping, key in ((observed_seeds, scene["scenarioSeed"]), (waveform_seeds, scene["waveformSeed"])):
            if key in mapping:
                raise ValueError("reused independent scene or waveform seed")
            mapping[key] = split
        for obj in scene["objects"]:
            key = obj["waveform_seed"]
            if key in waveform_seeds:
                raise ValueError("reused per-object waveform seed")
            waveform_seeds[key] = split
            uv = project(direction_vector(obj["azimuth_deg"], obj["elevation_deg"]), scene["camera"])
            np.testing.assert_allclose(uv, obj["point_uv"], atol=1e-12)
        with np.load(dataset / row["npz_path"], allow_pickle=False) as sample:
            n = len(scene["objects"])
            if sample["rgb"].shape != (192, 320, 3) or sample["rgb"].dtype != np.uint8:
                raise ValueError("invalid RGB tensor")
            if sample["boxes"].shape != (n, 4) or sample["points"].shape != (n, 2):
                raise ValueError("label shape mismatch")
            for name in ("heat", "boxes", "active", "points"):
                if not np.isfinite(sample[name]).all():
                    raise ValueError("nonfinite input or label")
            np.testing.assert_array_equal(sample["classes"], np.zeros(n, dtype=np.int64))
            np.testing.assert_allclose(sample["boxes"], [o["box_cxcywh"] for o in scene["objects"]], atol=1e-7)
            np.testing.assert_allclose(sample["points"], [o["point_uv"] for o in scene["objects"]], atol=1e-7)
            np.testing.assert_array_equal(sample["active"], [float(o["active"]) for o in scene["objects"]])
            result = json.loads((dataset / row["heat_path"]).read_text(encoding="utf-8"))
            np.testing.assert_array_equal(sample["heat"], validate_heat(result, sid, 72, 45))
            empty_heat_count += not bool(result["heat"])
            rgb_hash = hashlib.sha256(sample["rgb"].tobytes()).hexdigest()
            if rgb_hash != row["rgb_sha256"]:
                raise ValueError("RGB tensor hash mismatch")
            for lookup, digest in ((rgb_hashes, rgb_hash), (pcm_hashes, row["pcm_sha256"])):
                if digest in lookup and lookup[digest] != split:
                    raise ValueError("identical RGB/PCM content leaks between splits")
                lookup[digest] = split
            if index in regenerate_indices:
                np.testing.assert_array_equal(sample["rgb"], render_rgb(scene))
                pcm = synthesize_audio(scene)
                if hashlib.sha256(pcm.tobytes()).hexdigest() != row["pcm_sha256"]:
                    raise ValueError("PCM regeneration does not reproduce original source tensor")
        counts[split] += 1
        active_histogram[split][scene["active_count"]] += 1
        near_count += scene["near_pair"]
        masked_count += bool(scene["disabled_channels"])
    if counts != metadata["counts"]:
        raise ValueError("split counts do not match dataset specification")
    return {"format": "micarray128-av-audit-v1", "status": "passed", "samples": len(rows),
            "split_counts": counts, "active_count_histograms": active_histogram,
            "near_pair_scenes": near_count, "masked_channel_scenes": masked_count,
            "empty_heat_scenes": empty_heat_count, "reproduced_pcm_and_rgb_samples": len(regenerate_indices),
            "split_seed_and_content_leaks": 0,
            "scope": "source/data contract audit only; algorithm accuracy is evaluated independently"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", required=True, type=Path)
    parser.add_argument("--regenerate", type=int, default=5)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.regenerate < 0:
        parser.error("--regenerate must be nonnegative")
    result = audit(args.dataset.resolve(), args.regenerate)
    with args.output.open("x", encoding="utf-8") as handle:
        json.dump(result, handle, indent=2, allow_nan=False)
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
