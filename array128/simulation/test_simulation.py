from __future__ import annotations

import copy
import json
from pathlib import Path
import unittest

import numpy as np

from evaluate import aggregate, angular_matrix, match_directions, score_scene, snap_predictions_to_grid
from generate_dataset import validate_heat
from scene_model import (canonical_hash, direction_vector, geometry_document, geometry_points,
                         make_scene, project, render_rgb, scene_input, seed_for, synthesize_audio,
                         unproject, vector_angles)


class SimulationTests(unittest.TestCase):
    def test_geometry_matches_frozen_design(self):
        points = geometry_points()
        distances = np.linalg.norm(points[:, None, :] - points[None, :, :], axis=2)
        unique = distances[np.triu_indices(128, 1)]
        self.assertAlmostEqual(float(unique.max()), .395, places=12)
        self.assertAlmostEqual(float(unique.min()), .01773339846707412, places=12)
        np.testing.assert_allclose(points.mean(axis=0), 0, atol=1e-15)
        file = json.loads(Path(__file__).with_name("geometry_fermat_v5.json").read_text(encoding="utf-8"))
        self.assertFalse(file["channel_mapping_confirmed"])
        np.testing.assert_array_equal(geometry_points(file), points)

    def test_split_seeds_and_waveforms_are_disjoint(self):
        seeds = {split: {seed_for(split, index, purpose) for index in range(100)
                         for purpose in (0, 1, 2, 64, 65, 66, 67, 68, 69, 70)}
                 for split in ("train", "val", "test")}
        self.assertFalse(seeds["train"] & seeds["val"])
        self.assertFalse(seeds["test"] & seeds["val"])
        self.assertFalse(seeds["train"] & seeds["test"])

    def test_seed_reproduction_and_zero_to_four_sources(self):
        for n in range(5):
            scene = make_scene("test", n)
            self.assertEqual(sum(o["active"] for o in scene["objects"]), n)
            self.assertEqual(canonical_hash(scene), canonical_hash(make_scene("test", n)))

    def test_visual_does_not_leak_speaking_status(self):
        scene = make_scene("test", 4)
        rgb = render_rgb(scene)
        modified = copy.deepcopy(scene)
        for item in modified["objects"]:
            item["active"] = not item["active"]
            item["waveform_seed"] += 123
            item["relative_level_db"] -= 9
        np.testing.assert_array_equal(rgb, render_rgb(modified))
        self.assertEqual(rgb.shape, (192, 320, 3))
        self.assertEqual(rgb.dtype, np.uint8)
        # Even the generative geometry/appearance must be identical if only
        # the independently selected active source count changes.
        for count in range(5):
            np.testing.assert_array_equal(rgb, render_rgb(make_scene("test", 4, active_count=count)))
        stressed = render_rgb(make_scene("test", 3, active_count=0, difficulty="stress"))
        for count in range(1, 5):
            np.testing.assert_array_equal(stressed, render_rgb(make_scene("test", 3, active_count=count,
                                                                      difficulty="stress")))

    def test_camera_true_projection_round_trip(self):
        camera = make_scene("train", 9)["camera"]
        for uv in ((.1, .2), (.85, .76), (.5, .5)):
            vector = unproject(uv, camera)
            az, el = vector_angles(vector)
            np.testing.assert_allclose(direction_vector(az, el), vector, atol=1e-14)
            np.testing.assert_allclose(project(vector, camera), uv, atol=1e-14)
        level_camera = {"center_azimuth_deg": 0, "center_elevation_deg": 0,
                        "horizontal_fov_deg": 90, "vertical_fov_deg": 60, "roll_deg": 0}
        np.testing.assert_allclose(project(direction_vector(45, 0), level_camera), [1, .5], atol=1e-14)
        np.testing.assert_allclose(project(direction_vector(0, 30), level_camera), [.5, 0], atol=1e-14)

    def test_source_truth_is_not_srp_input(self):
        scene = make_scene("train", 4)
        input_document = scene_input(scene, "audio.f32")
        self.assertNotIn("objects", input_document)
        self.assertNotIn("active_count", input_document)
        self.assertNotIn("waveformSeed", input_document)
        self.assertEqual(input_document["geometry"], geometry_document())

    def test_physical_arrival_has_correct_sign(self):
        # Cross-correlate a plane pair with an independently known path order:
        # the +x microphone must observe a right-hand source earlier.
        scene = make_scene("test", 1, active_count=1, difficulty="ideal")
        active = next(o for o in scene["objects"] if o["active"])
        active.update(azimuth_deg=45, elevation_deg=0, range_m=100)
        scene.update(failed_channels=[], disabled_channels=[], snr_db=100)
        points = np.zeros((128, 3))
        points[1, 0] = .1
        pcm = synthesize_audio(scene, points)
        correlation = np.correlate(pcm[:, 1], pcm[:, 0], mode="full")
        lag = int(correlation.argmax()) - (len(pcm) - 1)
        self.assertLess(lag, 0)
        self.assertLess(abs(lag - (-.1 * np.sin(np.pi / 4) / 343 * 48000)), 1)

    def test_masked_faults_do_not_change_healthy_audio(self):
        scene = make_scene("test", 4, difficulty="ideal")
        scene["failed_channels"] = []
        clean = synthesize_audio(scene)
        scene["failed_channels"] = [1, 4, 16]
        damaged = synthesize_audio(scene)
        retained = [i for i in range(128) if i not in scene["failed_channels"]]
        np.testing.assert_array_equal(clean[:, retained], damaged[:, retained])
        self.assertGreater(float(np.std(damaged[:, 1])), .1)

    def test_silence_has_finite_sensor_noise(self):
        scene = make_scene("test", 0)
        scene["failed_channels"] = []
        pcm = synthesize_audio(scene)
        self.assertTrue(np.isfinite(pcm).all())
        self.assertGreater(float(np.std(pcm)), 0)
        self.assertLess(float(np.std(pcm)), .001)

    def test_predictions_are_scored_bijectively_and_empty_counted(self):
        matched = match_directions([(10, 0), (10.1, 0)], [(10, 0), (-20, 0)])
        self.assertEqual((matched["tp"], matched["fp"], matched["fn"]), (1, 1, 1))
        self.assertEqual(match_directions([(0, 0)], [])["fp"], 1)
        self.assertEqual(match_directions([], [(0, 0)])["fn"], 1)
        self.assertGreater(angular_matrix([(179, 0)], [(-179, 0)])[0, 0], 1.99)
        self.assertLess(angular_matrix([(179, 0)], [(-179, 0)])[0, 0], 2.01)

    def test_truth_prediction_scores_zero_error_and_point_not_bbox_center(self):
        scene = make_scene("test", 4)
        active = [item for item in scene["objects"] if item["active"]]
        prediction = {"scene_id": scene["scene_id"], "sources": [{"point_uv": item["point_uv"], "confidence": 1} for item in active]}
        row = score_scene(scene, prediction)
        self.assertTrue(row["complete_resolved"])
        self.assertEqual((row["tp"], row["fp"], row["fn"]), (4, 0, 0))
        self.assertLess(max(row["errors_deg"]), .00001)
        self.assertNotEqual(active[0]["point_uv"], active[0]["box_cxcywh"][:2])
        self.assertEqual(aggregate([row])["recall"], 1)

    def test_heat_is_never_synthesized_on_export_failure(self):
        document = {"format": "micarray128-srp-result-v1", "scene_id": "test", "columns": 0, "rows": 0, "heat": [], "reason": "silence"}
        self.assertFalse(validate_heat(document, "test", 72, 45).any())
        document.update(columns=72, rows=45)
        document["heat"] = [float("nan")] * (72 * 45)
        with self.assertRaises(ValueError):
            validate_heat(document, "test", 72, 45)

    def test_duplicate_nearby_points_are_not_super_resolution(self):
        scene = make_scene("test", 1, active_count=2)
        active = [o for o in scene["objects"] if o["active"]]
        scene["near_pair"] = True
        scene["close_pair_ids"] = [o["source_id"] for o in active]
        for index, item in enumerate(active):
            item.update(azimuth_deg=-.75 + 1.5 * index, elevation_deg=0)
            item["point_uv"] = list(project(direction_vector(item["azimuth_deg"], 0), scene["camera"]))
        prediction = {"scene_id": scene["scene_id"], "sources": [
            {"azimuth_deg": 0, "elevation_deg": 0, "confidence": 1},
            {"azimuth_deg": 0, "elevation_deg": 0, "confidence": 1}]}
        row = score_scene(scene, prediction)
        self.assertEqual(row["tp"], 2)  # Both fit the ordinary 2-degree gate.
        self.assertFalse(row["near_pair_strict_resolved"])
        prediction["sources"][0]["azimuth_deg"] = -.75
        prediction["sources"][1]["azimuth_deg"] = .75
        self.assertTrue(score_scene(scene, prediction)["near_pair_strict_resolved"])

    def test_grid_ablation_quantizes_predictions_without_truth_or_count_changes(self):
        camera = make_scene("val", 1)["camera"]
        prediction = {"scene_id": "arbitrary", "sources": [
            {"point_uv": [.234, .678], "confidence": .42, "trusted": True},
            {"azimuth_deg": -150, "elevation_deg": 0, "confidence": .05, "trusted": False}]}
        before = copy.deepcopy(prediction)
        result = snap_predictions_to_grid(prediction, camera, 72, 45)
        self.assertEqual(prediction, before)
        self.assertEqual(len(result["sources"]), 2)
        self.assertEqual(result["sources"][0]["confidence"], .42)
        self.assertEqual(result["sources"][0]["point_uv"], [round(.234 * 71) / 71, round(.678 * 44) / 44])
        self.assertEqual(result["sources"][1], prediction["sources"][1])
        boundary = {"scene_id": "edge", "sources": [{"point_uv": [0, 1], "trusted": True}]}
        self.assertEqual(snap_predictions_to_grid(boundary, camera, 72, 45)["sources"][0]["point_uv"], [0, 1])
        with self.assertRaises(ValueError):
            snap_predictions_to_grid(prediction, camera, 1, 45)


if __name__ == "__main__":
    unittest.main()
