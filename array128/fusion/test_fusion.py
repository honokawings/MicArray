"""CPU regression tests; these do not claim a trained model's accuracy."""

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

import numpy as np
import torch

from .data import prepare_input, validate_dataset
from .export import geometry_metadata, srp_settings_metadata
from .inference import decode_detections, point_metrics
from .loss import fusion_loss
from .model import FusionYoloXNano


class FusionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        torch.set_num_threads(2)

    def test_network_shape_and_separate_point_head(self):
        model = FusionYoloXNano().eval()
        with torch.no_grad():
            output = model(torch.zeros(1, 4, 192, 320))
        self.assertEqual(tuple(output.shape), (1, 1260, 9))
        self.assertTrue(torch.isfinite(output).all())
        self.assertFalse(torch.allclose(output[..., :2], output[..., 6:8]))

    def test_input_half_pixel_resize_and_rgb_order(self):
        rgb = np.zeros((192, 320, 3), np.uint8)
        rgb[..., 0] = 255
        heat = np.array([[0, 1], [0, 1]], np.float32)
        image = prepare_input(rgb, heat)
        self.assertEqual(tuple(image.shape), (4, 192, 320))
        self.assertEqual(float(image[0].mean()), 1)
        self.assertEqual(float(image[1:3].max()), 0)
        self.assertAlmostEqual(float(image[3, 96, 159]), 0.496875, places=6)
        with self.assertRaises(ValueError):
            prepare_input(rgb, np.full((4, 4), np.nan))

    def test_empty_scene_and_supervised_point_gradients(self):
        model = FusionYoloXNano()
        images = torch.rand(2, 4, 192, 320)
        empty = {"boxes": torch.empty(0, 4), "classes": torch.empty(0, dtype=torch.long),
                 "active": torch.empty(0), "points": torch.empty(0, 2)}
        labels = {"boxes": torch.tensor([[0.4, 0.5, 0.15, 0.2], [0.7, 0.5, 0.1, 0.2]]),
                  "classes": torch.zeros(2, dtype=torch.long), "active": torch.tensor([1.0, 0.0]),
                  "points": torch.tensor([[0.42, 0.47], [float("nan"), float("nan")]])}
        losses = fusion_loss(model(images, raw=True), [empty, labels], model)
        self.assertTrue(torch.isfinite(losses["total"]))
        losses["total"].backward()
        self.assertTrue(any(head.source_point.weight.grad.abs().sum() > 0 for head in model.heads))
        self.assertTrue(any(head.activity.weight.grad.abs().sum() > 0 for head in model.heads))
        self.assertGreater(float(model.pafpn.backbone.stem.conv.conv.weight.grad[:, 3::4].abs().sum()), 0)

    def test_postprocessing_uses_learned_point_and_activity(self):
        raw = np.array([[0.5, 0.5, 0.1, 0.1, 0.9, 0.9, 0.47, 0.53, 0.9],
                        [0.2, 0.2, 0.1, 0.1, 0.9, 0.9, 0.21, 0.22, 0.1]], np.float32)
        result = decode_detections(raw)
        self.assertEqual(len(result), 1)
        self.assertAlmostEqual(result[0]["source_point_uv"][0], 0.47, places=6)
        self.assertNotEqual(result[0]["source_point_uv"], result[0]["box_cxcywh"][:2])

    def test_dataset_audit_rejects_partial_generation_and_seed_leaks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            metadata = {"status": "generating", "image_height": 192, "image_width": 320,
                        "heat_resize": "bilinear align_corners=False",
                        "counts": {"train": 1, "val": 1, "test": 1}}
            rows = [{"sample_id": split, "split": split, "scenarioSeed": str(i), "waveformSeed": str(i + 10)}
                    for i, split in enumerate(("train", "val", "test"))]
            (root / "dataset.json").write_text(json.dumps(metadata), encoding="utf-8")
            (root / "manifest.jsonl").write_text("\n".join(json.dumps(row) for row in rows), encoding="utf-8")
            with self.assertRaises(ValueError):
                validate_dataset(root)
            metadata["status"] = "complete"
            (root / "dataset.json").write_text(json.dumps(metadata), encoding="utf-8")
            self.assertEqual(validate_dataset(root)["status"], "complete")
            rows[2]["waveformSeed"] = rows[0]["waveformSeed"]
            (root / "manifest.jsonl").write_text("\n".join(json.dumps(row) for row in rows), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "boundary"):
                validate_dataset(root)

    def test_point_matching_does_not_greedily_lose_close_second_source(self):
        predictions = [[{"source_point_uv": [0.508, 0.5]}, {"source_point_uv": [0.49, 0.5]}]]
        result = point_metrics(predictions, [[[0.5, 0.5], [0.52, 0.5]]])
        self.assertEqual(result["tp"], 2)
        self.assertEqual(result["fp"], 0)
        self.assertEqual(result["fn"], 0)

    def test_geometry_metadata_binds_coordinate_hash_and_channel_order(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fermat_v5_coordinates.json"
            source = {"candidate_hash": "candidate", "coordinates": [
                {"channel": channel, "x_m": channel * 0.001, "y_m": 0, "z_m": 0}
                for channel in reversed(range(128))]}
            path.write_text(json.dumps(source), encoding="utf-8")
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            provenance = {"generator_sha256": {path.name: digest}}
            result = geometry_metadata(directory, provenance)
            self.assertEqual(len(result["geometry_coordinates_m"]), 128)
            self.assertEqual(result["geometry_coordinates_m"][0], [0, 0, 0])
            self.assertAlmostEqual(result["geometry_coordinates_m"][127][0], 0.127)
            self.assertFalse(result["geometry_channel_mapping_confirmed"])
            self.assertEqual(result["geometry_coordinates_source_sha256"], digest)
            path.write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "differs"):
                geometry_metadata(directory, provenance)

    def test_srp_metadata_reads_only_train_and_rejects_setting_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rows = [{"split": "train", "input_path": f"{index}.json"} for index in range(2)]
            rows.append({"split": "test", "input_path": "never-read-test.json"})
            (root / "manifest.jsonl").write_text("\n".join(json.dumps(row) for row in rows), encoding="utf-8")
            document = {"pcm": {"sample_rate": 48000, "channels": 128, "frames": 2048},
                        "grid": {"columns": 72, "rows": 45}, "max_sources": 4,
                        "minimum_source_separation_deg": 4.0, "sound_speed_m_s": 343.0}
            for index in range(2):
                (root / f"{index}.json").write_text(json.dumps(document), encoding="utf-8")
            result = srp_settings_metadata(directory)
            self.assertEqual(result["training_srp_settings_validated_inputs"], 2)
            self.assertEqual(result["training_srp_settings"]["minimum_source_separation_deg"], 4.0)
            document["minimum_source_separation_deg"] = 8.0
            (root / "1.json").write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "inconsistent"):
                srp_settings_metadata(directory)


if __name__ == "__main__":
    unittest.main()
