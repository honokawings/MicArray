"""Windows release entry point: routing, cache safety, and native failure gates."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
POWERSHELL = shutil.which("powershell.exe")


@unittest.skipUnless(os.name == "nt" and POWERSHELL, "Windows PowerShell required")
class PackageReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="m128 package ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "checkout with spaces"
        (self.root / "scripts").mkdir(parents=True)
        (self.root / "array128").mkdir()
        shutil.copy2(REPO / "scripts/package-release.ps1", self.root / "scripts")
        shutil.copy2(REPO / "array128/package.ps1", self.root / "array128")
        self.cmake = self.root / "CMakeLists.txt"
        self.cmake.write_text(
            'option(MICARRAY_BUILD_LEGACY_48 "Build original host" OFF)\n',
            encoding="utf-8",
        )
        (self.root / "array128/CMakeLists.txt").write_text(
            "project(MicArray128)\n", encoding="utf-8"
        )

    def invoke(self, *args, script="scripts/package-release.ps1"):
        return subprocess.run(
            [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
             str(self.root / script), *map(str, args)],
            cwd=self.temp.name, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=30,
        )

    def plan(self, *args, **kwargs):
        result = self.invoke(*args, "-PlanOnly", **kwargs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def cache(self, directory, source, legacy=None, generator="Ninja"):
        build = self.root / directory
        build.mkdir()
        values = [f"CMAKE_HOME_DIRECTORY:INTERNAL={source}",
                  f"CMAKE_GENERATOR:INTERNAL={generator}"]
        if legacy is not None:
            values.append(f"MICARRAY_BUILD_LEGACY_48:BOOL={legacy}")
        path = build / "CMakeCache.txt"
        path.write_text("\n".join(values), encoding="utf-8")
        return path

    def test_default_selects_current_128_variant(self):
        plan = self.plan()
        self.assertEqual(plan["Variant"], "Array128")
        self.assertEqual(plan["Target"], "MicArray128")
        self.assertEqual(Path(plan["SourceDir"]), self.root / "array128")
        self.assertEqual(plan["TestTargets"], ["test_array128"])
        self.assertTrue(plan["Zip"])
        self.assertFalse((self.root / "dist").exists())
        self.assertFalse((self.root / "build-array128").exists())

    def test_explicit_legacy_enables_all_six_test_targets(self):
        plan = self.plan("-Variant", "Legacy48")
        self.assertEqual(plan["Target"], "MicArray")
        self.assertEqual(plan["ConfigureOptions"], ["-DMICARRAY_BUILD_LEGACY_48=ON"])
        self.assertEqual(set(plan["TestTargets"]), {
            "test_protocol", "test_device", "test_media", "test_app", "test_ui", "test_analysis",
        })

    def test_legacy_checkout_does_not_select_leftover_array_directory(self):
        self.cmake.write_text("project(MicArray)\n", encoding="utf-8")
        plan = self.plan()
        self.assertEqual(plan["Variant"], "Legacy48")
        self.assertEqual(plan["ConfigureOptions"], [])

    def test_root_configured_128_cache_is_supported(self):
        self.cache("root-build", self.root, "OFF", "Ninja Multi-Config")
        plan = self.plan("-BuildDir", "root-build", "-Configuration", "Debug")
        self.assertEqual(Path(plan["SourceDir"]), self.root)
        self.assertEqual(plan["ExecutableSubdir"], "array128")
        self.assertEqual(plan["Generator"], "Ninja Multi-Config")
        self.assertEqual(plan["ConfigureOptions"], ["-DMICARRAY_BUILD_LEGACY_48=OFF"])

    def test_wrong_variant_cache_is_rejected_without_changes(self):
        cache = self.cache("wrong-build", self.root / "array128")
        before = cache.read_bytes()
        result = self.invoke("-Variant", "Legacy48", "-BuildDir", "wrong-build", "-PlanOnly")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("another source/variant", result.stderr)
        self.assertEqual(cache.read_bytes(), before)

    def test_wrapper_and_existing_switches_forward(self):
        plan = self.plan("-NoZip", "-SkipTests", "-BuildDir", "build custom",
                         "-DistDir", "release custom", script="array128/package.ps1")
        self.assertEqual(plan["Variant"], "Array128")
        self.assertFalse(plan["Zip"])
        self.assertTrue(plan["SkipTests"])
        self.assertEqual(Path(plan["BuildDir"]), self.root / "build custom")
        self.assertEqual(Path(plan["DistDir"]), self.root / "release custom")

    def test_native_configure_build_and_test_failures_stop_packaging(self):
        tools = self.root / "fake tools"
        tools.mkdir()
        qt = tools / "qt kit"
        (qt / "bin").mkdir(parents=True)
        # Never executed: each failure must stop before deployment.
        (qt / "bin/windeployqt.exe").write_bytes(b"must not execute")
        cmake = tools / "cmake.cmd"
        ctest = tools / "ctest.cmd"
        ctest.write_text("@exit /b 37\n", encoding="ascii")
        for stage, code, command in (
            ("configure", 23, "@exit /b 23\n"),
            ("build", 29, '@if "%~1"=="--build" exit /b 29\n@exit /b 0\n'),
            ("test", 37, "@exit /b 0\n"),
        ):
            with self.subTest(stage=stage):
                cmake.write_text(command, encoding="ascii")
                result = self.invoke("-CMakePath", cmake, "-CMakePrefixPath", qt,
                                     "-CTestPath", ctest,
                                     "-CxxCompiler", sys.executable, "-MakeProgram", sys.executable)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"Command failed (exit {code})", result.stderr)
                self.assertFalse((self.root / "dist").exists())


if __name__ == "__main__":
    unittest.main()
