# Task 1: Pressure Option Validation

## Scope

Modify only:

- `tools/mock_device_server.py`
- `tests/tools/test_mock_device_server.py` (create)

Do not modify Qt/C++ sources, documentation, specs, plans, or unrelated files.
Other work exists in the repository; do not revert or overwrite it.

## Requirements

Follow test-driven development. Use Python 3.9+ standard library only.

Add:

```python
def finite_float(text: str) -> float
def validate_args(args: argparse.Namespace) -> None
```

`finite_float` converts text to float and raises
`argparse.ArgumentTypeError("value must be finite")` for NaN or infinity.

Add parser options:

| Option | Type/default |
| --- | --- |
| `--stress-mode` | choices `normal`, `sustained`, `burst`; default `normal` |
| `--duration` | finite float; default `0.0` |
| `--target-mbps` | finite float; default `0.0` |
| `--stats-interval` | finite float; default `1.0` |
| `--burst-size-mb` | finite float; default `32.0` |
| `--burst-interval` | finite float; default `1.0` |

Validation:

- Existing `audio_fps > 0`, `sample_rate > 0`, `video_fps >= 0`,
  `jitter_ms >= 0` behavior remains.
- `duration >= 0`.
- `target_mbps >= 0`.
- `stats_interval > 0`.
- `burst_size_mb > 0`.
- `burst_interval > 0`.
- A nonzero `target_mbps` is valid only when stress mode is `sustained`;
  otherwise raise `ValueError("--target-mbps requires --stress-mode sustained")`.
- `main()` catches `ValueError` from `validate_args` and calls
  `parser.error(str(exc))`.
- `--self-test` continues to work without requiring network access.

## Tests

Create `tests/tools/test_mock_device_server.py` using `unittest`.
Load the script with `importlib.util.spec_from_file_location`; insert the module
into `sys.modules` before `exec_module` so `dataclass` works correctly.

Cover:

1. Default pressure arguments and successful validation.
2. Sustained mode accepting `--target-mbps 100`.
3. Burst mode rejecting `--target-mbps 100`.
4. Rejection of negative duration.
5. Rejection of NaN target.
6. Rejection of zero stats interval.
7. Rejection of zero burst size.
8. Rejection of infinite burst interval.

Run:

```powershell
python -m unittest tests.tools.test_mock_device_server.ArgumentValidationTests -v
python tools/mock_device_server.py --self-test
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
```

All commands must pass after implementation. Report the initial failing-test
evidence and final passing outputs.
