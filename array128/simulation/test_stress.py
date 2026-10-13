from __future__ import annotations

import copy
import unittest

import numpy as np

from evaluate import angular_matrix, score_scene
from run_stress_suite import summary, wilson_interval
from scene_model import (canonical_hash, direction_vector, make_scene, project,
                         render_rgb, scene_input, synthesize_audio)
from stress_report import heat_colors
from stress_scenarios import challenge_rgb, live_gate_reason, make_challenge, make_plan


def spec(family, value, replicate=0):
    return next(row for row in make_plan() if row["family"] == family
                and row["value"] == value and row["replicate"] == replicate)


class StressTests(unittest.TestCase):
    def test_plan_is_fixed_complete_and_uniquely_identified(self):
        plan = make_plan()
        self.assertEqual(len(plan), 628)
        self.assertEqual(len({r["scene_id"] for r in plan}), len(plan))
        self.assertEqual(len(make_plan(1)), 43)
        self.assertEqual(plan, make_plan())
        with self.assertRaises(ValueError):
            make_plan(0)

    def test_every_scene_has_valid_physical_truth_and_complete_activity(self):
        for row in make_plan():
            scene = make_challenge(row)
            self.assertEqual(scene["active_count"], sum(o["active"] for o in scene["objects"]))
            self.assertTrue(set(scene["disabled_channels"]) <= set(scene["failed_channels"]))
            self.assertEqual(len(set(scene["failed_channels"])), len(scene["failed_channels"]))
            for obj in scene["objects"]:
                np.testing.assert_allclose(project(direction_vector(obj["azimuth_deg"], obj["elevation_deg"]), scene["camera"]), obj["point_uv"], atol=1e-12)
                self.assertTrue(np.isfinite(obj["box_cxcywh"]).all())
                self.assertTrue(all(0 <= x <= 1 for x in obj["point_uv"]))
                self.assertTrue(all(0 < x < 1 for x in obj["box_cxcywh"][2:]))

    def test_source_count_does_not_leak_into_visual_input(self):
        reference = challenge_rgb(make_challenge(spec("reference", 0)))
        for count in range(1, 5):
            np.testing.assert_array_equal(reference, challenge_rgb(make_challenge(spec("reference", count))))

    def test_new_master_seeds_are_disjoint_from_original(self):
        old = make_scene("test", 11000)
        new = make_challenge(spec("separation", 3))
        self.assertNotEqual(old["scenarioSeed"], new["scenarioSeed"])
        self.assertNotEqual(old["waveformSeed"], new["waveformSeed"])
        self.assertNotEqual(old["objects"][0]["waveform_seed"], new["objects"][0]["waveform_seed"])

    def test_near_pair_has_exact_requested_spherical_separation(self):
        for separation in (1.5, 2, 3, 4, 6, 9, 12):
            scene = make_challenge(spec("separation", separation))
            a, b = scene["objects"][:2]
            distance = angular_matrix([(a["azimuth_deg"], a["elevation_deg"])], [(b["azimuth_deg"], b["elevation_deg"])])[0, 0]
            self.assertAlmostEqual(distance, separation, places=10)
            self.assertTrue(scene["near_pair"])

    def test_inactive_neighbor_is_identical_rgb_to_two_active_sources(self):
        for separation in (3, 6):
            both = make_challenge(spec("separation", separation))
            one = make_challenge(spec("inactive_neighbor", separation))
            np.testing.assert_array_equal(challenge_rgb(both), challenge_rgb(one))
            self.assertEqual(one["active_count"], 1)
            self.assertFalse(one["near_pair"])

    def test_noise_and_level_sweeps_only_change_the_intended_acoustic_field(self):
        for family, base_value, altered, field in (("snr", 30, -10, "snr_db"),
                                                  ("imbalance", 0, 24, "relative_level_db")):
            base, changed = make_challenge(spec(family, base_value)), make_challenge(spec(family, altered))
            np.testing.assert_array_equal(challenge_rgb(base), challenge_rgb(changed))
            a, b = copy.deepcopy(base), copy.deepcopy(changed)
            for scene in (a, b):
                scene.pop("scene_id"); scene.pop("challenge")
                if family == "snr":
                    scene.pop(field)
                else:
                    scene["objects"][1].pop(field)
            self.assertEqual(canonical_hash(a), canonical_hash(b))

    def test_same_faulty_pcm_with_and_without_explicit_mask(self):
        a, b = make_challenge(spec("mask", "random16")), make_challenge(spec("unmasked", "random16"))
        np.testing.assert_array_equal(challenge_rgb(a), challenge_rgb(b))
        np.testing.assert_array_equal(synthesize_audio(a), synthesize_audio(b))
        self.assertEqual(a["failed_channels"], b["failed_channels"])
        self.assertEqual(a["disabled_channels"], a["failed_channels"])
        self.assertEqual(b["disabled_channels"], [])

    def test_channel_failures_are_nested_within_each_layout(self):
        for layout in ("random", "sector"):
            small = make_challenge(spec("mask", layout + "4"))["failed_channels"]
            middle = make_challenge(spec("mask", layout + "8"))["failed_channels"]
            large = make_challenge(spec("mask", layout + "16"))["failed_channels"]
            self.assertTrue(set(small) < set(middle) < set(large))

    def test_visual_interventions_do_not_change_audio_or_truth(self):
        reference = make_challenge(spec("vision", "normal"))
        reference_rgb = challenge_rgb(reference)
        for intervention in ("dark", "blur", "occlusion", "shift12"):
            scene = make_challenge(spec("vision", intervention))
            self.assertEqual(scene["objects"], reference["objects"])
            self.assertEqual(scene["waveformSeed"], reference["waveformSeed"])
            self.assertFalse(np.array_equal(challenge_rgb(scene), reference_rgb))
            np.testing.assert_array_equal(render_rgb(scene), reference_rgb)
        self.assertLess(challenge_rgb(make_challenge(spec("vision", "dark"))).mean(), reference_rgb.mean() * .26)

    def test_truth_and_challenge_spec_never_enter_srp_input(self):
        for row in make_plan(1):
            data = scene_input(make_challenge(row), "audio.f32")
            for forbidden in ("objects", "challenge", "active_count", "pair_center_uv", "waveformSeed"):
                self.assertNotIn(forbidden, data)
            self.assertEqual(data["max_sources"], 4)
            self.assertEqual(data["minimum_source_separation_deg"], 4)

    def test_live_gate_does_not_call_empty_heat_a_valid_deployment(self):
        scene = make_challenge(spec("reference", 0))
        self.assertTrue(live_gate_reason(scene, {"heat": [], "valid_channels": 128}))
        self.assertTrue(live_gate_reason(scene, {"heat": [0, 1], "valid_channels": 111}))
        self.assertFalse(live_gate_reason(scene, {"heat": [0, 1], "valid_channels": 112}))

    def test_intervals_do_not_claim_certainty_from_small_samples(self):
        self.assertIsNone(wilson_interval(0, 0))
        self.assertGreater(wilson_interval(0, 16)[1], .19)
        self.assertLess(wilson_interval(16, 16)[0], .81)
        with self.assertRaises(ValueError):
            wilson_interval(17, 16)

    def test_weak_source_statistics_count_the_known_weaker_truth(self):
        scene = make_challenge(spec("imbalance", 24))
        prediction = {"scene_id": scene["scene_id"], "sources": [{"point_uv": scene["objects"][0]["point_uv"]}]}
        score = score_scene(scene, prediction, 0)
        record = {"spec": scene["challenge"], "scores": {method: score for method in ("raw_fusion", "gated_fusion", "srp")}, "gate_reason": ""}
        result = summary([record])
        self.assertEqual(result["weak_source_recall"], 0)
        self.assertEqual(result["metrics"]["raw_fusion"]["recall"], .5)

    def test_display_coloring_does_not_modify_numeric_response(self):
        heat = np.arange(12, dtype=np.float32).reshape(3, 4) / 11
        original = heat.copy()
        colored = heat_colors(heat)
        self.assertEqual(colored.shape, (3, 4, 3))
        self.assertEqual(colored.dtype, np.uint8)
        np.testing.assert_array_equal(heat, original)


if __name__ == "__main__":
    unittest.main()
