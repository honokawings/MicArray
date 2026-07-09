# Task 1 Report: Pressure Option Validation

## Red Test Evidence

Initial run of:

```powershell
python -m unittest tests.tools.test_mock_device_server.ArgumentValidationTests -v
```

failed as expected before implementation. The key failures were:

- `AttributeError: module 'tools.mock_device_server' has no attribute 'finite_float'`
- `AttributeError: 'Namespace' object has no attribute 'stress_mode'`
- `argparse` rejected the new pressure options because the parser did not know them yet

That confirmed the tests were exercising missing behavior rather than typos in the test file.

## Implementation Summary

Updated `tools/mock_device_server.py` to add:

- `finite_float(text: str) -> float`
- `validate_args(args: argparse.Namespace) -> None`
- parser options:
  - `--stress-mode`
  - `--duration`
  - `--target-mbps`
  - `--stats-interval`
  - `--burst-size-mb`
  - `--burst-interval`

Validation now covers:

- existing checks for `audio_fps`, `sample_rate`, `video_fps`, and `jitter_ms`
- nonnegative `duration` and `target_mbps`
- strictly positive `stats_interval`, `burst_size_mb`, and `burst_interval`
- `--target-mbps` only when `--stress-mode sustained`

`main()` now routes `ValueError` from `validate_args()` through `parser.error(...)`.

Created `tests/tools/test_mock_device_server.py` with `unittest`, loading the script via `importlib.util.spec_from_file_location` and inserting the module into `sys.modules` before execution so `dataclass` resolves correctly.

## Final Commands And Output

### 1) Unit tests

```powershell
python -m unittest tests.tools.test_mock_device_server.ArgumentValidationTests -v
```

Output:

```text
test_burst_mode_rejects_target_mbps_via_main ... ok
test_default_pressure_arguments_validate_successfully ... ok
test_infinite_burst_interval_is_rejected ... ok
test_nan_target_is_rejected ... ok
test_negative_duration_is_rejected ... ok
test_sustained_mode_accepts_target_mbps ... ok
test_zero_burst_size_is_rejected ... ok
test_zero_stats_interval_is_rejected ... ok

Ran 8 tests in 0.024s

OK
```

### 2) Self-test

```powershell
python tools/mock_device_server.py --self-test
```

Output:

```text
self-test passed
```

### 3) Bytecode compilation

```powershell
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
```

Output:

```text
```

## Self-Review

- The implementation stays within the requested scope.
- The tests cover all eight required validation cases.
- `--self-test` still runs without network access.
- No Qt/C++ sources, docs, or unrelated files were changed.

## Modified Files

- `tools/mock_device_server.py`
- `tests/tools/test_mock_device_server.py`
- `.superpowers/sdd/task-1-report.md`
