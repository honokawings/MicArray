"""Independent physical/visual forward model, never a localization oracle.

The renderer does not inspect whether an object is speaking. Acoustic truth is
established before SRP, using source-to-microphone Euclidean distances. The
geometry is a research template: its channel order is NOT confirmed PCB wiring.
"""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


WIDTH, HEIGHT = 320, 192
SAMPLE_RATE, FRAME_COUNT, CHANNELS = 48000, 2048, 128
GEOMETRY_CANDIDATE = "f43a7cba6d5d1cdbc2f17773bf021cd875a644f40c2159b1cdbefffc18b4439e"
ORIGINAL_GEOMETRY_SHA256 = "249a2191fa8bb2f7b8ca2a088af23d77d489c4ad86b3aac94e4ba26356435a12"
SPLIT_CODES = {"train": 11, "val": 23, "test": 47}


def seed_for(split: str, index: int, purpose: int, master_seed: int = 128202610) -> int:
    if split not in SPLIT_CODES or index < 0 or not 0 <= purpose <= 255:
        raise ValueError("invalid split/index/purpose")
    return int(np.random.SeedSequence([master_seed, SPLIT_CODES[split], index, purpose])
               .generate_state(1, dtype=np.uint64)[0])


def geometry_document() -> dict:
    source = json.loads(Path(__file__).with_name("fermat_v5_coordinates.json").read_text(encoding="utf-8"))
    if source["candidate_hash"] != GEOMETRY_CANDIDATE:
        raise ValueError("unexpected geometry candidate")
    points = sorted(source["coordinates"], key=lambda p: p["channel"])
    if [p["channel"] for p in points] != list(range(CHANNELS)):
        raise ValueError("geometry channel numbering must be a bijection of 0..127")
    return {
        "format": "micarray128-geometry-v1", "units": "m",
        "name": "Fermat V5 395 mm SIMULATION ONLY - PCB channel map unconfirmed",
        "microphones": [{"channel": p["channel"], "x": p["x_m"],
                         "y": p["y_m"], "z": p["z_m"]} for p in points],
    }


def geometry_points(document: dict | None = None) -> np.ndarray:
    document = document or geometry_document()
    return np.asarray([[p["x"], p["y"], p["z"]] for p in document["microphones"]], dtype=np.float64)


def direction_vector(azimuth_deg: float, elevation_deg: float) -> np.ndarray:
    az, el = np.deg2rad([azimuth_deg, elevation_deg])
    return np.asarray([np.sin(az) * np.cos(el), np.sin(el), np.cos(az) * np.cos(el)])


def vector_angles(vector: np.ndarray) -> tuple[float, float]:
    unit = np.asarray(vector, dtype=np.float64)
    unit /= np.linalg.norm(unit)
    return float(np.rad2deg(np.arctan2(unit[0], unit[2]))), float(np.rad2deg(np.arcsin(unit[1])))


def camera_basis(camera: dict) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    forward = direction_vector(camera["center_azimuth_deg"], camera["center_elevation_deg"])
    right = np.cross([0.0, 1.0, 0.0], forward)
    right /= np.linalg.norm(right)
    up = np.cross(forward, right)
    roll = np.deg2rad(camera["roll_deg"])
    return right * np.cos(roll) + up * np.sin(roll), up * np.cos(roll) - right * np.sin(roll), forward


def unproject(point: tuple[float, float], camera: dict) -> np.ndarray:
    right, up, forward = camera_basis(camera)
    x = (point[0] - .5) * 2 * np.tan(np.deg2rad(camera["horizontal_fov_deg"]) / 2)
    y = (.5 - point[1]) * 2 * np.tan(np.deg2rad(camera["vertical_fov_deg"]) / 2)
    vector = forward + x * right + y * up
    return vector / np.linalg.norm(vector)


def project(vector: np.ndarray, camera: dict) -> tuple[float, float]:
    right, up, forward = camera_basis(camera)
    depth = float(np.dot(vector, forward))
    if depth <= 0:
        raise ValueError("source is behind the camera")
    x = .5 + np.dot(vector, right) / depth / (2 * np.tan(np.deg2rad(camera["horizontal_fov_deg"]) / 2))
    y = .5 - np.dot(vector, up) / depth / (2 * np.tan(np.deg2rad(camera["vertical_fov_deg"]) / 2))
    return float(x), float(y)


def make_scene(split: str, index: int, master_seed: int = 128202610,
               active_count: int | None = None, difficulty: str = "mixed") -> dict:
    """Scene and waveform random streams are independent and split-isolated."""
    scenario_seed = seed_for(split, index, 0, master_seed)
    waveform_seed = seed_for(split, index, 1, master_seed)
    rng = np.random.default_rng(scenario_seed)
    if active_count is None:
        active_count = index % 5
    if active_count not in range(5) or difficulty not in ("mixed", "ideal", "stress"):
        raise ValueError("active_count must be 0..4 and difficulty must be mixed/ideal/stress")
    stress = difficulty == "stress" or (difficulty == "mixed" and index % 4 == 3)
    camera = {
        "center_azimuth_deg": float(rng.uniform(-8, 8)),
        "center_elevation_deg": float(rng.uniform(-5, 5)),
        "horizontal_fov_deg": float(rng.uniform(75, 100)),
        "vertical_fov_deg": float(rng.uniform(48, 67)),
        "roll_deg": float(rng.uniform(-5, 5)),
    }
    # Visible-object count and appearance do not reveal which ones are active.
    object_count = int(rng.integers(4, 8))
    points = []
    for _ in range(object_count):
        for attempt in range(300):
            uv = tuple(rng.uniform([.13, .18], [.87, .75]))
            if all(np.linalg.norm(np.asarray(uv) - other) > .11 for other in points):
                break
        points.append(np.asarray(uv))
    # Layout is sampled BEFORE and independently of speech activity. Otherwise
    # a pair's closeness would leak its active labels to an RGB-only model.
    near_layout = bool(rng.random() < .65)
    near_separation = None
    close_pair_ids = []
    if near_layout:
        first, second = [int(i) for i in rng.choice(object_count, 2, replace=False)]
        # Geodesic offset of a physical ray, not an offset of an observed peak.
        base = unproject(tuple(points[first]), camera)
        right, _, _ = camera_basis(camera)
        tangent = right - np.dot(right, base) * base
        tangent /= np.linalg.norm(tangent)
        near_separation = float(rng.uniform(1.5, 9.0))
        delta = np.deg2rad(near_separation)
        moved = base * np.cos(delta) + tangent * np.sin(delta)
        moved_uv = project(moved, camera)
        if .12 < moved_uv[0] < .9:
            points[second] = np.asarray(moved_uv)
            close_pair_ids = [first, second]
        else:
            near_layout = False
            near_separation = None
    active_rng = np.random.default_rng(seed_for(split, index, 2, master_seed))
    active_ids = set(int(x) for x in active_rng.choice(object_count, active_count, replace=False))
    near_pair = bool(close_pair_ids) and all(i in active_ids for i in close_pair_ids)
    objects = []
    for i, uv in enumerate(points):
        az, el = vector_angles(unproject(tuple(uv), camera))
        category = "person" if rng.random() < .6 else "loudspeaker"
        width = float(rng.uniform(.045, .085))
        height = float(rng.uniform(.14, .24) if category == "person" else rng.uniform(.09, .16))
        # A person's emitting mouth is above the object's bbox centre. Speaker
        # drivers are also deliberately off-centre; bbox centre is not truth.
        mouth_fraction = .22 if category == "person" else float(rng.uniform(.35, .65))
        x0, y0 = float(uv[0] - width / 2), float(uv[1] - height * mouth_fraction)
        box = [x0 + width / 2, y0 + height / 2, width, height]
        objects.append({
            "source_id": i, "active": i in active_ids, "category": category,
            "box_cxcywh": box, "point_uv": [float(x) for x in uv],
            "azimuth_deg": az, "elevation_deg": el,
            "range_m": float(rng.uniform(7, 20)),
            "relative_level_db": float(rng.uniform(-7, 0)),
            "appearance_seed": seed_for(split, index, 32 + i, master_seed),
            "waveform_seed": seed_for(split, index, 64 + i, master_seed),
        })
    failed_count = int(rng.choice([0, 0, 2, 4, 8, 16]))
    failed = sorted(int(x) for x in rng.choice(CHANNELS, failed_count, replace=False))
    reflections = []
    for item in objects:
        if not item["active"] or not stress:
            continue
        for reflection_index in range(int(rng.integers(1, 3))):
            ray = unproject(tuple(rng.uniform([.05, .05], [.95, .95])), camera)
            az, el = vector_angles(ray)
            reflections.append({"source_id": item["source_id"], "azimuth_deg": az,
                                "elevation_deg": el, "extra_delay_s": float(rng.uniform(.002, .02)),
                                "gain": float(rng.uniform(.06, .25)), "reflection_index": reflection_index})
    return {
        "format": "micarray128-simulation-scene-v1", "scene_id": f"{split}-{index:06d}",
        "split": split, "index": index, "scenarioSeed": scenario_seed,
        "waveformSeed": waveform_seed, "master_seed": master_seed, "camera": camera,
        "objects": objects, "active_count": active_count, "near_pair": near_pair,
        "near_layout": near_layout, "close_pair_ids": close_pair_ids,
        "near_separation_deg": near_separation, "difficulty": "stress" if stress else "ideal",
        "snr_db": float(rng.uniform(1, 12) if stress else rng.uniform(15, 30)),
        "reflections": reflections, "actual_sound_speed_m_s": float(rng.uniform(340, 346)) if stress else 343.0,
        "steering_sound_speed_m_s": 343.0, "failed_channels": failed, "disabled_channels": failed,
        "geometry_candidate": GEOMETRY_CANDIDATE, "geometry_source_sha256": ORIGINAL_GEOMETRY_SHA256,
        "geometry_channel_mapping_confirmed": False,
        "propagation": "spherical-distance direct paths + statistical far-field specular reflections",
        "visual_domain": "procedural synthetic people/speakers, NOT real-camera semantic validation",
    }


def render_rgb(scene: dict) -> np.ndarray:
    """Render identical RGB if only active/waveform/acoustic fields change."""
    rng = np.random.default_rng(scene["scenarioSeed"] ^ 0x56495355414C)
    y, x = np.mgrid[:HEIGHT, :WIDTH]
    base = rng.uniform(40, 150, size=3)
    color = base + y[..., None] * rng.uniform(-.1, .3, size=3) + x[..., None] * rng.uniform(-.1, .1, size=3)
    color += rng.normal(0, 2.5, (HEIGHT, WIDTH, 1))
    image = Image.fromarray(np.uint8(np.clip(color, 0, 255)))
    draw = ImageDraw.Draw(image)
    for _ in range(8):
        x0, y0 = rng.integers([0, 0], [WIDTH, HEIGHT])
        x1, y1 = min(WIDTH, x0 + int(rng.integers(10, 70))), min(HEIGHT, y0 + int(rng.integers(5, 45)))
        fill = tuple(int(v) for v in rng.integers(25, 180, 3))
        draw.rectangle((x0, y0, x1, y1), fill=fill)
    for item in sorted(scene["objects"], key=lambda value: -value["range_m"]):
        appearance = np.random.default_rng(item["appearance_seed"])
        cx, cy, width, height = item["box_cxcywh"]
        x0, x1 = (cx - width / 2) * WIDTH, (cx + width / 2) * WIDTH
        y0, y1 = (cy - height / 2) * HEIGHT, (cy + height / 2) * HEIGHT
        px, py = item["point_uv"][0] * WIDTH, item["point_uv"][1] * HEIGHT
        dark = tuple(int(v) for v in appearance.integers(15, 65, 3))
        colored = tuple(int(v) for v in appearance.integers(70, 225, 3))
        if item["category"] == "person":
            draw.rounded_rectangle((x0, y0 + height * HEIGHT * .3, x1, y1), radius=4, fill=colored)
            skin = tuple(int(v) for v in appearance.integers([110, 70, 45], [235, 195, 170]))
            draw.ellipse((x0 + 1, y0, x1 - 1, y0 + height * HEIGHT * .37), fill=skin, outline=dark)
            # All people have exactly the same neutral-mouth logic: no active cue.
            draw.line((px - 2, py, px + 2, py), fill=dark, width=1)
            draw.point((px - 3, py - 4), fill=dark)
            draw.point((px + 3, py - 4), fill=dark)
        else:
            draw.rounded_rectangle((x0, y0, x1, y1), radius=2, fill=dark, outline=colored)
            radius = max(2, width * WIDTH * .29)
            draw.ellipse((px - radius, py - radius, px + radius, py + radius), fill=colored, outline=(10, 10, 10))
            draw.ellipse((px - 1, py - 1, px + 1, py + 1), fill=dark)
    return np.asarray(image, dtype=np.uint8)


def synthesize_audio(scene: dict, points: np.ndarray | None = None,
                     num_samples: int = FRAME_COUNT) -> np.ndarray:
    """Return finite float32 [time,128], with explicit defective-channel noise.

    A source is generated with an independent random complex spectrum and
    propagated using its Euclidean distance to each mic. It is not made from
    steering code, SRP output, labels drawn from a peak, or image luminance.
    """
    points = geometry_points() if points is None else np.asarray(points, dtype=np.float64)
    if points.shape != (CHANNELS, 3) or not np.isfinite(points).all():
        raise ValueError("expected finite 128x3 geometry")
    length = max(8192, 4 * num_samples)
    frequencies = np.fft.rfftfreq(length, 1.0 / SAMPLE_RATE)
    combined = np.zeros((len(frequencies), CHANNELS), dtype=np.complex128)
    band = (frequencies >= 300) & (frequencies <= 7800)
    for item in scene["objects"]:
        if not item["active"]:
            continue
        rng = np.random.default_rng(item["waveform_seed"])
        spectral_tilt = float(rng.uniform(-.6, .1))
        profile = (np.maximum(frequencies, 300) / 1000) ** spectral_tilt
        profile *= .5 + np.exp(-.5 * ((frequencies - rng.uniform(800, 2100)) / rng.uniform(200, 600)) ** 2)
        spectrum = (rng.normal(size=len(frequencies)) + 1j * rng.normal(size=len(frequencies))) * profile * band
        reference = np.fft.irfft(spectrum, n=length)
        spectrum *= .035 / max(float(np.std(reference)), 1e-12) * 10 ** (item["relative_level_db"] / 20)
        source_position = item["range_m"] * direction_vector(item["azimuth_deg"], item["elevation_deg"])
        distances = np.linalg.norm(points - source_position, axis=1)
        delays = (distances - item["range_m"]) / scene["actual_sound_speed_m_s"]
        combined += spectrum[:, None] * (item["range_m"] / distances)[None, :] * np.exp(-2j * np.pi * frequencies[:, None] * delays)
        for reflection in scene["reflections"]:
            if reflection["source_id"] != item["source_id"]:
                continue
            direction = direction_vector(reflection["azimuth_deg"], reflection["elevation_deg"])
            delays = reflection["extra_delay_s"] - points @ direction / scene["actual_sound_speed_m_s"]
            combined += reflection["gain"] * spectrum[:, None] * np.exp(-2j * np.pi * frequencies[:, None] * delays)
    clean = np.fft.irfft(combined, n=length, axis=0)[length // 2:length // 2 + num_samples]
    rng = np.random.default_rng(scene["waveformSeed"])
    rms = math.sqrt(float(np.mean(clean * clean)))
    # The no-source case still has sensor noise; no exact-zero shortcut.
    noise_rms = max(.00015, rms * 10 ** (-scene["snr_db"] / 20))
    result = clean + rng.normal(0, noise_rms, clean.shape)
    if scene["difficulty"] == "stress":
        result *= 10 ** (rng.normal(0, .4, CHANNELS) / 20)
    for channel in scene["failed_channels"]:
        kind = channel % 3
        if kind == 0:
            result[:, channel] = 0
        elif kind == 1:
            result[:, channel] = rng.uniform(-.8, .8, num_samples)
        else:
            result[:, channel] = .7 + rng.normal(0, .04, num_samples)
    if not np.isfinite(result).all():
        raise RuntimeError("nonfinite simulation")
    return np.asarray(np.clip(result, -.999, .999), dtype="<f4")


def scene_input(scene: dict, pcm_path: str, columns: int = 72, rows: int = 45) -> dict:
    """Truth-free input accepted by the real production-DSP CLI."""
    return {
        "format": "micarray128-srp-input-v1", "scene_id": scene["scene_id"],
        "pcm": {"path": pcm_path, "format": "f32le-interleaved", "channels": CHANNELS,
                "sample_rate": SAMPLE_RATE, "frames": FRAME_COUNT},
        "geometry": geometry_document(), "camera": scene["camera"],
        "disabled_channels": scene["disabled_channels"], "grid": {"columns": columns, "rows": rows},
        "sound_speed_m_s": scene["steering_sound_speed_m_s"], "confidence_threshold": .35,
        "max_sources": 4, "minimum_source_separation_deg": 4.0,
    }


def canonical_hash(value: dict) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()
