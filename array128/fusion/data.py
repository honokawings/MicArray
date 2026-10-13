"""Strict simulation dataset loading; no labels are derived from SRP peaks."""

import hashlib
import json
from pathlib import Path

import numpy as np
import torch
from torch.nn import functional as F
from torch.utils.data import Dataset

from .model import HEIGHT, WIDTH


def validate_dataset(root):
    """Audit split identities/producer contract without reading test samples."""
    root = Path(root).resolve()
    metadata_path = root / "dataset.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if metadata.get("status") != "complete":
        raise ValueError("Dataset generation is not complete; partial datasets cannot train a release model")
    if (metadata.get("image_height"), metadata.get("image_width")) != (HEIGHT, WIDTH):
        raise ValueError("Dataset image shape differs from the fixed model contract")
    if metadata.get("heat_resize") != "bilinear align_corners=False":
        raise ValueError("Dataset SRP resizing differs from deployment")
    rows = [json.loads(row) for row in (root / "manifest.jsonl").read_text(encoding="utf-8").splitlines() if row]
    identities, partitions = set(), {name: [] for name in ("train", "val", "test")}
    for row in rows:
        if row["sample_id"] in identities:
            raise ValueError("Duplicate sample identity")
        identities.add(row["sample_id"])
        partitions[row["split"]].append(row)
    for split, items in partitions.items():
        if len(items) != metadata["counts"][split]:
            raise ValueError("Dataset manifest count mismatch")
    for key in ("scenarioSeed", "waveformSeed"):
        seen = set()
        for split, items in partitions.items():
            seeds = {str(row[key]) for row in items}
            if seen & seeds:
                raise ValueError(f"{key} crosses train/val/test boundary")
            seen |= seeds
    return metadata | {"dataset_metadata_sha256": hashlib.sha256(metadata_path.read_bytes()).hexdigest(),
                       "manifest_sha256": hashlib.sha256((root / "manifest.jsonl").read_bytes()).hexdigest()}


def prepare_input(rgb, heat):
    rgb = np.asarray(rgb)
    heat = np.asarray(heat, dtype=np.float32)
    if rgb.shape != (HEIGHT, WIDTH, 3) or rgb.dtype != np.uint8:
        raise ValueError(f"RGB must be uint8[{HEIGHT},{WIDTH},3]")
    if heat.ndim != 2 or min(heat.shape) < 2 or not np.isfinite(heat).all():
        raise ValueError("SRP must be a finite 2D numeric response, not a color image")
    if heat.min() < -1e-5 or heat.max() > 1.00001:
        raise ValueError("SRP normalization differs from the [0,1] deployment contract")
    heat_tensor = torch.from_numpy(heat.copy())[None, None]
    heat_tensor = F.interpolate(heat_tensor, (HEIGHT, WIDTH), mode="bilinear",
                                align_corners=False)[0]
    rgb_tensor = torch.from_numpy(rgb.copy()).permute(2, 0, 1).float() / 255.0
    return torch.cat((rgb_tensor, heat_tensor), 0)


class FusionDataset(Dataset):
    def __init__(self, root, split, ablation="none"):
        self.root = Path(root).resolve()
        if split not in {"train", "val", "test"}:
            raise ValueError("Unknown dataset split")
        self.split, self.ablation = split, ablation
        self.files = sorted((self.root / split).rglob("*.npz"))
        if not self.files:
            manifest = self.root / "manifest.json"
            document = json.loads(manifest.read_text(encoding="utf-8"))
            entries = document if isinstance(document, list) else document["samples"]
            for entry in entries:
                if entry["split"] != split:
                    continue
                name = entry.get("npz_path", entry.get("path"))
                if name is None:
                    raise ValueError("Manifest sample is missing npz_path")
                path = (self.root / name).resolve()
                if self.root not in path.parents:
                    raise ValueError("Sample path must stay inside the dataset root")
                self.files.append(path)
        if not self.files:
            raise ValueError(f"Empty {split} dataset")

    def __len__(self):
        return len(self.files)

    def __getitem__(self, index):
        with np.load(self.files[index], allow_pickle=False) as sample:
            image = prepare_input(sample["rgb"], sample["heat"])
            boxes = np.asarray(sample["boxes"], dtype=np.float32).reshape(-1, 4)
            points = np.asarray(sample["points"], dtype=np.float32).reshape(-1, 2)
            classes = np.asarray(sample["classes"], dtype=np.int64).reshape(-1)
            active = np.asarray(sample["active"], dtype=np.float32).reshape(-1)
        count = len(boxes)
        if len(points) != count or len(classes) != count or len(active) != count:
            raise ValueError("Every visible candidate needs box, class, activity and point")
        if not np.isfinite(boxes).all() or (boxes < 0).any() or (boxes > 1).any():
            raise ValueError("Boxes must be normalized finite cxcywh")
        if count and ((boxes[:, 2:] <= 0).any() or (classes != 0).any()):
            raise ValueError("Version 1 supports positive-size class-0 candidate boxes")
        if not np.isin(active, [0, 1]).all():
            raise ValueError("Activity labels must be binary ground truth")
        sounding = active > 0.5
        if not np.isfinite(points[sounding]).all() or (points[sounding] < 0).any() or (points[sounding] > 1).any():
            raise ValueError("Active source points must be normalized finite ground truth")
        if self.ablation == "rgb_only":
            image[3].zero_()
        elif self.ablation == "srp_only":
            image[:3].zero_()
        elif self.ablation == "shuffled_srp":
            if len(self.files) < 2:
                raise ValueError("Shuffled-SRP ablation needs at least two distinct samples")
            other = (index + len(self.files) // 2) % len(self.files)
            with np.load(self.files[other], allow_pickle=False) as mismatched:
                heat = torch.from_numpy(np.asarray(mismatched["heat"], dtype=np.float32).copy())[None, None]
            image[3] = F.interpolate(heat, (HEIGHT, WIDTH), mode="bilinear", align_corners=False)[0, 0]
        elif self.ablation != "none":
            raise ValueError("Unknown ablation")
        labels = {"boxes": torch.from_numpy(boxes), "points": torch.from_numpy(points),
                  "classes": torch.from_numpy(classes), "active": torch.from_numpy(active)}
        return image, labels, str(self.files[index].relative_to(self.root))


def collate(samples):
    images, labels, names = zip(*samples)
    return torch.stack(images), list(labels), list(names)
