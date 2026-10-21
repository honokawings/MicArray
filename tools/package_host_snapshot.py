"""Collect a verified stage-1 host package; does not build, program, or touch original trees."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    workspace = Path(__file__).resolve().parents[2]
    stage = workspace / "deliverables/host_staging"
    build = workspace / "build/host"
    reports = stage / "reports/qt"
    reports.mkdir(parents=True, exist_ok=True)
    suites = {}
    for source in sorted(build.glob("test_*-results.txt")):
        text = source.read_text(encoding="utf-8-sig")
        match = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped", text)
        if not match:
            raise RuntimeError("Missing test totals: " + str(source))
        suites[source.stem] = dict(zip(("passed_count", "failed_count", "skipped_count"), map(int, match.groups())))
        shutil.copy2(source, reports / source.name)
    if len(suites) != 11 or any(item["failed_count"] for item in suites.values()):
        raise RuntimeError("All eleven suites must pass before packaging")
    final_ctest = workspace / "build/host_ctest_final.log"
    if "100% tests passed, 0 tests failed out of 11" not in final_ctest.read_text(encoding="utf-8-sig"):
        raise RuntimeError("Current full CTest run did not pass")
    smoke = json.loads((stage / "standalone_smoke.json").read_text(encoding="utf-8-sig"))
    fixed_path = workspace / "host/validation/worker_fixed_model_3s_final/summary.json"
    fixed = json.loads(fixed_path.read_text(encoding="utf-8"))
    if not smoke["passed"] or not fixed["passed"]:
        raise RuntimeError("Standalone runtime or fixed-model suffix verification failed")
    shutil.copy2(final_ctest, stage / "reports/ctest_final.log")
    shutil.copy2(workspace / "build/host_windeploy.log", stage / "reports/windeploy.log")
    shutil.copy2(fixed_path, stage / "reports/fixed_model_full_suffix.json")
    shutil.copy2(workspace / "host/doc/PL_DENOISE_MODE.md", stage / "HOST_README_CN.md")
    shutil.copy2(build / "pl_denoise_mode_preview.png", stage / "reports/pl_denoise_mode_preview.png")
    source_root = workspace.parent / "github_micarray_port_20261014/host_reference"
    algorithm_files = ["fractionaldelaybeamformer.cpp", "wienernoisereducer.cpp", "bandpassfilter.cpp",
                       "logmmseprocessor.cpp", "automaticgaincontrol.cpp"]
    original_algorithms = {}
    for name in algorithm_files:
        original = digest(source_root / "src/media" / name)
        current = digest(workspace / "host/src/media" / name)
        if original != current:
            raise RuntimeError("Original algorithm changed: " + name)
        original_algorithms[name] = original
    executable = stage / "qt/bin/MicArray.exe"
    executable_hash = digest(executable)
    if executable_hash != smoke["executable_sha256"]:
        raise RuntimeError("Packaged executable changed after smoke test")
    summary = {
        "passed": True,
        "stage": 1,
        "app_version": "2026.10.20-pl-three-beam-denoise",
        "mode_label": "PL 三路波束 + 上位机原降噪（AUD3/DNS1）",
        "executable": "qt/bin/MicArray.exe",
        "executable_sha256": executable_hash,
        "protocol": {"audio": "AUD3 v1 flags0x11 float32 triple", "status": "DNS1 stage1 build0x20261020",
                     "control": "RESET/BEGIN/6Q26 slopes/COMMIT, 26 bytes, CRC16 and per-field ACK"},
        "processing_split": {"PL": ["A", "B1", "B2"],
                             "PC": ["original Wiener", "original ordered BandPass/LogMMSE/AGC"]},
        "full_software_mode_preserved": True,
        "automatic_fallback": False,
        "raw48_pcm_and_channel_order_preserved": True,
        "voiceprint": False,
        "portable_settings_separate": True,
        "camera_production_sources_unchanged": True,
        "original_algorithm_sha256": original_algorithms,
        "suites": suites,
        "total_test_pass_count": sum(item["passed_count"] for item in suites.values()),
        "standalone_smoke": smoke,
        "fixed_model_full_suffix": {case: {
            "output_samples": data["fixed_model"]["output_samples"],
            "final_max_abs_error": data["fixed_model"]["stages"]["final"]["max_absolute_error"],
            "final_error_snr_db_without_gain_fit": data["fixed_model"]["stages"]["final"]["error_snr_db_without_gain_fit"],
            "final_rms_level_difference_db": data["fixed_model"]["stages"]["final"]["rms_level_difference_db"],
            "original_float_all_suffix_stages_exact": data["original_float"]["all_stages_exact"],
        } for case, data in fixed["cases"].items()},
        "board_programmed_by_this_package_task": False,
        "physical_audio_acceptance": "PENDING_USER_BOARD_TEST",
        "scope": "Host and offline tests verified. Does not assert the new bit is routed or board-tested; PL evidence belongs to its own current build. Stage2 Wiener candidate is not included.",
    }
    (stage / "host_validation.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    paths = sorted(path for path in stage.rglob("*") if path.is_file() and path.name != "SHA256SUMS.json")
    hashes = {str(path.relative_to(stage)).replace("\\", "/"): digest(path) for path in paths}
    (stage / "SHA256SUMS.json").write_text(json.dumps(hashes, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "suites": len(suites), "tests": summary["total_test_pass_count"],
                      "files": len(hashes), "executable_sha256": executable_hash,
                      "validation": str(stage / "host_validation.json")}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
