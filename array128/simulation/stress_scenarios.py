"""Predeclared paired challenge scenes for a frozen audiovisual model.

This module never reads predictions. Acoustic/layout interventions precede SRP.
Replicates are independent; parameter levels within a replicate are paired.
"""

from __future__ import annotations

import copy
import math

import numpy as np
from PIL import Image, ImageFilter

from scene_model import (camera_basis, geometry_points, make_scene, project,
                         render_rgb, unproject, vector_angles)


MASTER_SEED = 12820261003
FAMILIES = {
    "reference": ("常规 0–4 源", list(range(5)), 20),
    "separation": ("双源角间隔 / °", [1.5, 2, 3, 4, 6, 9, 12], 16),
    "snr": ("输入信噪比 / dB", [-10, 0, 5, 10, 20, 30], 16),
    "imbalance": ("双源电平差 / dB", [0, 6, 12, 18, 24], 16),
    "mask": ("坏通道已屏蔽", ["none", "random4", "random8", "random16",
                                "sector4", "sector8", "sector16"], 12),
    "unmasked": ("坏通道未屏蔽", ["random4", "random16"], 12),
    "vision": ("视频退化", ["normal", "dark", "blur", "occlusion", "shift12"], 12),
    "reflection": ("两条反射各自增益", [0, .15, .35, .6], 12),
    "inactive_neighbor": ("近邻仅一源发声 / °", [3, 6], 12),
}
VALUE_NAMES = {
    "none": "无坏通道", "random4": "分散 4 路", "random8": "分散 8 路",
    "random16": "分散 16 路", "sector4": "扇区 4 路", "sector8": "扇区 8 路",
    "sector16": "扇区 16 路", "normal": "正常", "dark": "亮度 ×0.25",
    "blur": "模糊 σ2.5px", "occlusion": "中央遮挡", "shift12": "水平错位 12px",
}


def make_plan(replicate_limit: int | None = None) -> list[dict]:
    if replicate_limit is not None and replicate_limit < 1:
        raise ValueError("replicate limit must be positive")
    rows = []
    for family, (label, values, count) in FAMILIES.items():
        for value in values:
            for replicate in range(min(count, replicate_limit or count)):
                slug = str(value).replace(".", "p").replace("-", "m")
                rows.append({"scene_id": f"challenge-{family}-{slug}-{replicate:03d}",
                             "family": family, "family_label": label, "value": value,
                             "value_label": VALUE_NAMES.get(str(value), str(value)),
                             "replicate": replicate,
                             "scope": "same_generator_new_seed" if family == "reference"
                             else "controlled_challenge_not_population_accuracy"})
    return rows


def move_object(item: dict, uv: tuple[float, float], camera: dict) -> None:
    """Move the complete object, preserving the emitting-point bbox offset."""
    dx, dy = np.asarray(uv) - item["point_uv"]
    item["box_cxcywh"][0] += float(dx)
    item["box_cxcywh"][1] += float(dy)
    item["point_uv"] = [float(v) for v in uv]
    item["azimuth_deg"], item["elevation_deg"] = vector_angles(unproject(uv, camera))


def position_pair(scene: dict, separation: float) -> None:
    center = unproject(tuple(scene["pair_center_uv"]), scene["camera"])
    right = camera_basis(scene["camera"])[0]
    tangent = right - np.dot(right, center) * center
    tangent /= np.linalg.norm(tangent)
    half = math.radians(separation / 2)
    for item, sign in zip(scene["objects"][:2], (-1, 1)):
        ray = center * math.cos(half) + sign * tangent * math.sin(half)
        move_object(item, project(ray, scene["camera"]), scene["camera"])
    scene["close_pair_ids"] = [0, 1]
    scene["near_separation_deg"] = separation
    scene["near_layout"] = True
    scene["near_pair"] = all(item["active"] for item in scene["objects"][:2])


def make_challenge(spec: dict, master_seed: int = MASTER_SEED) -> dict:
    family, value, replicate = spec["family"], spec["value"], spec["replicate"]
    if family not in FAMILIES or value not in FAMILIES[family][1]:
        raise ValueError("unknown predeclared intervention")
    # Paired unmasked cases reuse the same physical defects as the mask family;
    # the only changed DSP input is the disabled-channel list.
    seed_family = "mask" if family == "unmasked" else family
    if family == "inactive_neighbor":
        seed_family = "separation"
    seed_index = 10000 + list(FAMILIES).index(seed_family) * 1000 + replicate
    scene = make_scene("test", seed_index, master_seed,
                       active_count=int(value) if family == "reference" else 2,
                       difficulty="mixed" if family == "reference" else "ideal")
    scene.update(scene_id=spec["scene_id"], challenge=copy.deepcopy(spec))
    if family == "reference":
        return scene
    rng = np.random.default_rng(scene["scenarioSeed"] ^ 0x535452455353)
    scene.update(camera={"center_azimuth_deg": 0, "center_elevation_deg": 0,
                         "horizontal_fov_deg": 90, "vertical_fov_deg": 60, "roll_deg": 0},
                 snr_db=25.0, actual_sound_speed_m_s=343.0, failed_channels=[], disabled_channels=[],
                 reflections=[], near_pair=False, near_layout=False, close_pair_ids=[], near_separation_deg=None,
                 pair_center_uv=rng.uniform([.43, .37], [.57, .59]).tolist())
    # Select physical emitting objects without encoding activity into rendering.
    for i, item in enumerate(scene["objects"]):
        item.update(active=i < 2, relative_level_db=0.0, range_m=12.0)
    position_pair(scene, 24.0)
    scene.update(near_pair=False, near_layout=False, close_pair_ids=[], near_separation_deg=None)
    placed = [np.asarray(item["point_uv"]) for item in scene["objects"][:2]]
    for item in scene["objects"][2:]:
        for _ in range(1000):
            uv = rng.uniform([.13, .18], [.87, .73])
            if all(np.linalg.norm(uv - other) > .15 for other in placed):
                break
        else:
            raise RuntimeError("could not place distinct inactive candidates")
        move_object(item, tuple(uv), scene["camera"])
        placed.append(uv)
    if family in ("separation", "inactive_neighbor"):
        position_pair(scene, float(value))
        if family == "inactive_neighbor":
            scene["objects"][1]["active"] = False
            scene.update(active_count=1, near_pair=False)
    elif family == "snr":
        scene["snr_db"] = float(value)
    elif family == "imbalance":
        scene["objects"][1]["relative_level_db"] = -float(value)
    elif family in ("mask", "unmasked"):
        count = 0 if value == "none" else int("".join(c for c in value if c.isdigit()))
        if str(value).startswith("sector"):
            points = geometry_points()
            order = np.argsort(np.arctan2(points[:, 1], points[:, 0]))
            order = np.roll(order, int(rng.integers(128)))
        else:
            order = rng.permutation(128)
        scene["failed_channels"] = sorted(int(ch) for ch in order[:count])
        scene["disabled_channels"] = [] if family == "unmasked" else scene["failed_channels"].copy()
    elif family == "reflection":
        for item in scene["objects"][:2]:
            for i, az in enumerate((-32, 30)):
                scene["reflections"].append({"source_id": item["source_id"],
                    "azimuth_deg": float(az + rng.uniform(-4, 4)), "elevation_deg": float(rng.uniform(-15, 15)),
                    "extra_delay_s": float(rng.uniform(.004, .018)), "gain": float(value), "reflection_index": i})
    return scene


def challenge_rgb(scene: dict) -> np.ndarray:
    rgb = render_rgb(scene)
    spec = scene["challenge"]
    if spec["family"] != "vision" or spec["value"] == "normal":
        return rgb
    intervention = spec["value"]
    if intervention == "dark":
        return np.uint8(rgb.astype(np.float32) * .25)
    if intervention == "blur":
        return np.asarray(Image.fromarray(rgb).filter(ImageFilter.GaussianBlur(2.5)))
    changed = rgb.copy()
    if intervention == "occlusion":
        # Fixed image-space stripe, not a truth-targeted mask or activity cue.
        changed[:, 145:175] = [72, 72, 72]
    elif intervention == "shift12":
        changed[:, :12] = 0
        changed[:, 12:] = rgb[:, :-12]
    else:
        raise ValueError("unknown visual intervention")
    return changed


def live_gate_reason(scene: dict, srp: dict) -> str:
    # These challenge scenes have valid trained camera/geometry/SRP settings.
    # Reflect the MainWindow checks which are observable in this static runner.
    if not srp["heat"]:
        return "SRP 无有效声图"
    if srp["valid_channels"] < 112:
        return "实际有效通道少于 112 路"
    if len(scene["disabled_channels"]) > 16:
        return "屏蔽超过训练域的 16 路"
    return ""
