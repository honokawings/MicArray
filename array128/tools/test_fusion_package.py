"""Read-only optional fusion package validation; never builds or deploys Qt."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
POWERSHELL = shutil.which("powershell.exe")


def varint(value):
    data = bytearray()
    while value >= 128:
        data.append((value & 127) | 128)
        value >>= 7
    data.append(value)
    return bytes(data)


def integer(number, value):
    return varint(number << 3) + varint(value)


def message(number, value):
    if isinstance(value, str):
        value = value.encode("utf-8")
    return varint((number << 3) | 2) + varint(len(value)) + value


def tensor_info(name, dimensions):
    shape = b"".join(message(1, integer(1, value)) for value in dimensions)
    tensor = integer(1, 1) + message(2, shape)  # FLOAT and static shape
    return message(1, name) + message(2, message(1, tensor))


def constant_onnx(fields=9):
    """A valid constant ONNX graph fixture, not trained weights or a detector."""
    tensor = b"".join(integer(1, value) for value in (1, 1260, fields))
    tensor += integer(2, 1) + message(9, bytes(1260 * fields * 4))
    attribute = message(1, "value") + message(5, tensor) + integer(20, 4)
    node = message(2, "detections") + message(4, "Constant") + message(5, attribute)
    graph = message(1, node) + message(2, "package_shape_fixture")
    graph += message(11, tensor_info("rgb_srp", (1, 4, 192, 320)))
    graph += message(12, tensor_info("detections", (1, 1260, fields)))
    return integer(1, 8) + message(7, graph) + message(8, integer(2, 17))


def metadata(model):
    return {
        "format": "micarray128-yolox-fusion-v1",
        "architecture": "YOLOX-Nano-derived-RGB-SRP-point-active",
        "domain": "simulation_only",
        "input_name": "rgb_srp", "input_dtype": "float32",
        "output_name": "detections", "input_shape": [1, 4, 192, 320],
        "output_shape": [1, 1260, 9], "channel_order": ["R", "G", "B", "SRP"],
        "rgb_normalization": "uint8_div_255", "srp_normalization": "cpp_response_0_1",
        "srp_resize": "bilinear_half_pixel_align_corners_false",
        "srp_color_map_or_focus_transform": False,
        "output_layout": ["cx", "cy", "width", "height", "objectness", "class_0", "source_x", "source_y", "active"],
        "class_names": ["visible_candidate_simulation_domain"],
        "box_format": "normalized_cxcywh", "source_point_format": "normalized_video_uv",
        "source_point_origin": "independent_learned_regression_head_not_box_center_or_srp_peak",
        "confidence_rule": "objectness_times_class_0", "projection": "pinhole",
        "model_sha256": hashlib.sha256(model).hexdigest(),
        "geometry_coordinates_m": [[i * .001, (i % 8) * .005, 0] for i in range(128)],
        "geometry_coordinates_source_sha256": "1" * 64,
        "geometry_channel_mapping_confirmed": False,
        "training_srp_grid": [72, 45],
        "training_srp_settings": {
            "max_sources": 4, "minimum_source_separation_deg": 4.0,
            "sound_speed_m_s": 343.0, "window_samples": 2048,
        },
        "release_status": "candidate_requires_independent_evaluation",
    }


@unittest.skipUnless(os.name == "nt" and POWERSHELL, "Windows PowerShell required")
class FusionPackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="m128 fusion package ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "checkout with spaces"
        (self.root / "scripts").mkdir(parents=True)
        (self.root / "array128/fusion").mkdir(parents=True)
        shutil.copy2(REPO / "scripts/package-release.ps1", self.root / "scripts")
        shutil.copy2(REPO / "array128/package.ps1", self.root / "array128")
        for name in ("LICENSE.yolox", "THIRD_PARTY.md"):
            shutil.copy2(REPO / "array128/fusion" / name, self.root / "array128/fusion" / name)
        (self.root / "CMakeLists.txt").write_text('option(MICARRAY_BUILD_LEGACY_48 "Legacy" OFF)\n', encoding="utf-8")
        (self.root / "array128/CMakeLists.txt").write_text(
            "project(MicArray128)\nqt_add_executable(test_array128_fusion tests/test_fusion.cpp)\n"
            "qt_add_executable(test_array128_fusionworker tests/test_fusionworker.cpp)\n"
            "qt_add_executable(test_array128_fusioncontext tests/test_fusioncontext.cpp)\n"
            "qt_add_executable(test_array128_fusionpipeline tests/test_fusionpipeline.cpp)\n", encoding="utf-8")
        self.model = self.root / "explicit model"
        self.runtime = self.root / "explicit runtime"
        self.model.mkdir(); self.runtime.mkdir()
        self.model_bytes = constant_onnx()
        (self.model / "fusion.onnx").write_bytes(self.model_bytes)
        self.description = metadata(self.model_bytes)
        self.write_metadata()

    def write_metadata(self):
        (self.model / "fusion.json").write_text(json.dumps(self.description), encoding="utf-8")

    def invoke(self, *args, script="scripts/package-release.ps1"):
        result = subprocess.run(
            [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
             str(self.root / script), "-PlanOnly", *map(str, args)],
            cwd=self.temporary.name, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=30,
        )
        self.assertFalse((self.root / "dist").exists())
        self.assertFalse((self.root / "build-array128").exists())
        return result

    def fusion(self, *args):
        return self.invoke("-FusionModelDir", self.model, "-OnnxRuntimeDir", self.runtime, *args)

    def test_default_has_no_optional_files_or_neural_readiness_claim(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads(result.stdout)
        self.assertFalse(plan["Fusion"]["Enabled"])
        self.assertEqual(plan["Fusion"]["Files"], [])
        self.assertIn("not ready", plan["Fusion"]["Capability"])
        self.assertIn("test_array128_fusionworker", plan["TestTargets"])
        self.assertEqual(len(plan["TestTargets"]), 6)

    def test_legacy_still_has_six_tests_and_rejects_fusion_options(self):
        plan = self.invoke("-Variant", "Legacy48")
        self.assertEqual(plan.returncode, 0, plan.stderr)
        self.assertEqual(len(json.loads(plan.stdout)["TestTargets"]), 6)
        result = self.fusion("-Variant", "Legacy48")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("only for Array128", result.stderr)

    def test_partial_options_fail_before_any_build(self):
        for argument, value in (("-FusionModelDir", self.model), ("-OnnxRuntimeDir", self.runtime)):
            with self.subTest(argument=argument):
                result = self.invoke(argument, value)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("together", result.stderr)
        forwarded = self.invoke("-FusionModelDir", self.model, script="array128/package.ps1")
        self.assertIn("together", forwarded.stderr)

    def test_hash_and_preprocessing_contract_are_strict(self):
        original = self.description.copy()
        for key, value in (
            ("model_sha256", "0" * 64), ("input_shape", [1, 3, 192, 320]),
            ("input_shape", ["1", 4, 192, 320]), ("channel_order", "R,G,B,SRP"),
            ("srp_color_map_or_focus_transform", True), ("class_names", ["person"]),
            ("source_point_origin", "box_center"), ("geometry_coordinates_m", []),
            ("release_status", "interface_only_do_not_use_for_performance"),
        ):
            with self.subTest(key=key, value=value):
                self.description = dict(original); self.description[key] = value; self.write_metadata()
                result = self.fusion()
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("Required command", result.stderr)
                self.assertNotIn("exactly one onnxruntime.dll", result.stderr)

    def test_graph_shape_is_checked_even_when_metadata_and_hash_match(self):
        changed = constant_onnx(fields=8)
        (self.model / "fusion.onnx").write_bytes(changed)
        self.description["model_sha256"] = hashlib.sha256(changed).hexdigest()
        self.write_metadata()
        result = self.fusion()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ONNX graph", result.stderr)

    def test_training_srp_contract_rejects_missing_fields(self):
        original = self.description.copy()
        for key in ("training_srp_grid", "training_srp_settings"):
            with self.subTest(missing=key):
                self.description = dict(original)
                del self.description[key]
                self.write_metadata()
                result = self.fusion()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(key, result.stderr)
        for key in original["training_srp_settings"]:
            with self.subTest(missing_setting=key):
                self.description = dict(original)
                self.description["training_srp_settings"] = dict(original["training_srp_settings"])
                del self.description["training_srp_settings"][key]
                self.write_metadata()
                result = self.fusion()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("training_srp_settings." + key, result.stderr)

    def test_training_srp_contract_rejects_wrong_values_and_types(self):
        original = self.description.copy()
        for key, value in (
            ("training_srp_grid", [45, 72]),
            ("training_srp_grid", ["72", 45]),
            ("training_srp_grid", "72,45"),
            ("training_srp_settings", []),
            ("training_srp_settings", None),
        ):
            with self.subTest(key=key, value=value):
                self.description = dict(original)
                self.description[key] = value
                self.write_metadata()
                result = self.fusion()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(key, result.stderr)
        for key, value in (
            ("max_sources", 5), ("max_sources", 4.5), ("max_sources", True),
            ("minimum_source_separation_deg", 8.0),
            ("minimum_source_separation_deg", "4.0"),
            ("sound_speed_m_s", 340.0), ("sound_speed_m_s", None),
            ("window_samples", 1024), ("window_samples", "2048"),
        ):
            with self.subTest(setting=key, value=value):
                self.description = dict(original)
                self.description["training_srp_settings"] = dict(original["training_srp_settings"])
                self.description["training_srp_settings"][key] = value
                self.write_metadata()
                result = self.fusion()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("training_srp_settings." + key, result.stderr)

    def test_no_runtime_discovery_or_non_pe_acceptance(self):
        result = self.fusion()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly one onnxruntime.dll", result.stderr)
        (self.runtime / "onnxruntime.dll").write_bytes(b"not a DLL")
        result = self.fusion()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Windows PE DLL", result.stderr)

    def test_explicit_official_runtime_plan_is_allowlisted(self):
        """Optional full positive fixture: three explicit paths, never discovery."""
        paths = [os.environ.get(name) for name in (
            "M128_PACKAGE_RUNTIME_DLL", "M128_PACKAGE_RUNTIME_LICENSE", "M128_PACKAGE_RUNTIME_NOTICES")]
        if not all(paths):
            self.skipTest("Set three explicit M128_PACKAGE_RUNTIME_* file paths to verify official runtime packaging")
        for path, name in zip(paths, ("onnxruntime.dll", "LICENSE", "ThirdPartyNotices.txt")):
            shutil.copy2(path, self.runtime / name)
        # These unrelated files must never be enumerated into the plan/package.
        (self.runtime / "private-recording.wav").write_bytes(b"private")
        (self.runtime / "python.exe").write_bytes(b"not a runtime dependency")
        (self.model / "secret-checkpoint.pt").write_bytes(b"not deployable")
        result = self.fusion()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        plan = json.loads(result.stdout)["Fusion"]
        self.assertTrue(plan["Enabled"]); self.assertEqual(plan["Domain"], "simulation_only")
        self.assertEqual(plan["TrainingSrpGrid"], [72, 45])
        self.assertEqual(plan["TrainingSrpSettings"], self.description["training_srp_settings"])
        self.assertIn("SIMULATION ONLY", plan["Capability"])
        self.assertIn("Visual C++", plan["Prerequisite"])
        self.assertIn("msvcp140.dll", plan["RuntimeImports"])
        self.assertEqual(len(plan["FileSHA256"]), 7)
        self.assertEqual(set(plan["Files"]), {
            "fusion/fusion.onnx", "fusion/fusion.json", "onnxruntime.dll",
            "licenses/onnxruntime/LICENSE", "licenses/onnxruntime/ThirdPartyNotices.txt",
            "licenses/yolox/LICENSE", "licenses/yolox/THIRD_PARTY.md",
        })
        for private_name in ("private-recording", "python.exe", "secret-checkpoint"):
            self.assertNotIn(private_name, result.stdout)
        wrapper = self.invoke("-FusionModelDir", self.model, "-OnnxRuntimeDir", self.runtime, script="array128/package.ps1")
        self.assertEqual(wrapper.returncode, 0, wrapper.stdout + wrapper.stderr)
        # Inspect an unknown dependency without ever loading/executing the DLL.
        original = (self.runtime / "onnxruntime.dll").read_bytes()
        changed = original.replace(b"KERNEL32.dll", b"UNSAFE32.dll")
        self.assertNotEqual(changed, original)
        (self.runtime / "onnxruntime.dll").write_bytes(changed)
        result = self.fusion()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported runtime DLL dependency", result.stderr)
        (self.runtime / "onnxruntime.dll").write_bytes(original)
        # The official release's lib/ layout is also explicit and supported.
        (self.runtime / "lib").mkdir()
        (self.runtime / "onnxruntime.dll").rename(self.runtime / "lib/onnxruntime.dll")
        result = self.fusion()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (self.runtime / "LICENSE").unlink()
        result = self.fusion()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("LICENSE and ThirdPartyNotices", result.stderr)


if __name__ == "__main__":
    unittest.main()
