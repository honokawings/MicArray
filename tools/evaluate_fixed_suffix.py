"""Compare actual host Worker suffix for fixed-model and original float beam files."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    workspace = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=workspace / "build/host/pl_denoise_replay.exe")
    parser.add_argument("--config", type=Path, default=workspace / "config/github_dsp_saved_demo_profile.json")
    parser.add_argument("--model-root", type=Path, default=workspace / "pl/tb_script/denoise_beam/results")
    parser.add_argument("--golden-root", type=Path, default=workspace / "validation/golden_reference_current_3s_v2")
    parser.add_argument("--output", type=Path, default=workspace / "host/validation/worker_fixed_model_3s")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env["PATH"] = r"E:\QT\6.11.1\mingw_64\bin;E:\QT\Tools\mingw1310_64\bin;" + env.get("PATH", "")
    cases = {}
    for case in ("background", "A_only", "B_only", "A_and_B"):
        reports = {}
        for label, beam_dir in (("fixed_model", args.model_root / case),
                                ("original_float", args.golden_root / case / "reference")):
            output = args.output / case / label
            command = [str(args.binary), "--beam-dir", str(beam_dir), "--config", str(args.config),
                       "--golden-dir", str(args.golden_root / case / "reference"), "--output", str(output)]
            with (args.output / f"{case}_{label}.log").open("w", encoding="utf-8") as handle:
                handle.write(json.dumps(command, ensure_ascii=False) + "\n")
                handle.flush()
                subprocess.run(command, env=env, stdout=handle, stderr=subprocess.STDOUT, check=True)
            report = json.loads((output / "comparison.json").read_text(encoding="utf-8"))
            report["input_beam_sha256"] = {name: sha256(beam_dir / (name + ".f32le"))
                                          for name in ("beam_A", "beam_B1", "beam_B2")}
            if label == "original_float":
                report["all_stages_exact"] = all(stage["different_float32_samples"] == 0
                                                  for stage in report["stages"].values())
            reports[label] = report
        cases[case] = reports
        print(case + ": PASS", flush=True)
    passed = all(report["passed"] and (label != "original_float" or report["all_stages_exact"])
                 for values in cases.values() for label, report in values.items())
    summary = {"passed": passed, "binary_sha256": sha256(args.binary),
               "config_sha256": sha256(args.config), "cases": cases,
               "scope": "Full 3 s fixed-model beams through actual host PL entry and original PC suffix, versus original GitHub golden. No time shift, gain fit, normalization, flush or padding; listening and board acceptance pending."}
    target = args.output / "summary.json"
    target.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(str(target), flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
