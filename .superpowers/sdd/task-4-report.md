# Task 4 Report

## Red Light

Initial targeted TDD run:

```powershell
python -m unittest tests.tools.test_mock_device_server.SelfTestCoverageTests tests.tools.test_mock_device_server.SocketIntegrationTests -v
```

Observed result:

```text
self-test passed
self-test passed
test_self_test_exercises_burst_mode_without_video ... FAIL
test_self_test_rejects_wrong_rate_limit_delay ... FAIL
test_send_media_frame_writes_complete_audio_frame_to_socket_peer ... ok

FAIL: test_self_test_exercises_burst_mode_without_video
AssertionError: unexpectedly None

FAIL: test_self_test_rejects_wrong_rate_limit_delay
AssertionError: AssertionError not raised
```

Note: the newly added socket integration test was green on first execution because
the existing `send_media_frame()` implementation already emitted the expected
on-wire frame bytes through a real `socket.socketpair()` peer. The red phase came
from the missing `self_test()` burst/no-video and rate-limit assertions.

## Implementation

1. Extended `tools/mock_device_server.py:self_test()` to:
   - parse `--stress-mode burst --no-video`
   - run `validate_args()`
   - drive `send_burst()` against a fake socket with a `100000`-byte target
   - assert actual bytes meet/exceed target
   - assert actual bytes are a multiple of the complete audio frame size
   - assert returned sequence equals the number of writes
   - assert every write starts with `7e 7e 11 ee` and ends with `0a 0d`
   - assert `rate_limit_delay(10.0, 12_500_000, 100.0, 10.75)` is `0.25`
2. Extended `tests/tools/test_mock_device_server.py` with:
   - `SelfTestCoverageTests`
   - `SocketIntegrationTests` using `socket.socketpair()` with timeouts and deterministic cleanup
3. Updated `doc/Readme.md` only inside `## 模拟服务端` plus one new `2026-07-09` change-log row:
   - `normal` / `sustained` / `burst` examples
   - pressure parameter table
   - decimal `Mb/s` vs binary `MiB`
   - stat field explanations for `tx`, `average` (`avg`), `peak`, `blocked`, `max_send`, `bursts`, `overruns`
   - protocol-valid-frame and non-acceptance disclaimer

## Final Commands

```powershell
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
git diff --check -- tools/mock_device_server.py tests/tools/test_mock_device_server.py doc/Readme.md
```

## Final Complete Summary

- `python -m unittest discover -s tests/tools -p "test_*.py" -v`
  - Passed: `26` tests, including the new `SelfTestCoverageTests` and `SocketIntegrationTests`.
- `python tools/mock_device_server.py --self-test`
  - Passed: printed `self-test passed`.
- `python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py`
  - Passed with no output.
- `git diff --check -- tools/mock_device_server.py tests/tools/test_mock_device_server.py doc/Readme.md`
  - Passed after removing one trailing-space issue in `doc/Readme.md`.
  - Git still prints LF->CRLF working-copy warnings for existing files, but no diff-check errors remain.

## Self-Review

- Kept edits scoped to the task files plus this report.
- Preserved the pre-existing dirty worktree content in `tools/mock_device_server.py` and `doc/Readme.md`; changes were applied on top of the current workspace state.
- The required socket integration coverage is present and deterministic.
- The main deviation from a perfect TDD cycle is that the socket integration test itself was already green when first introduced, because the underlying framing behavior already existed in the current worktree.

## File List

- `tools/mock_device_server.py`
- `tests/tools/test_mock_device_server.py`
- `doc/Readme.md`
- `.superpowers/sdd/task-4-report.md`

## 2026-07-09 Review Follow-Up

### Red Light

Targeted regression test added for the unit-label review item:

```powershell
python -m unittest tests.tools.test_mock_device_server.ArgumentValidationTests.test_burst_size_help_uses_mib_units -v
```

Observed result:

```text
FAIL: test_burst_size_help_uses_mib_units
AssertionError: 'burst size in MB' != 'burst size in MiB'
```

Root cause:

- Parser defaults in code were already correct and already covered by
  `test_default_pressure_arguments_validate_successfully`.
- `doc/Readme.md` parameter table described the pressure flags but omitted the
  explicit default values requested by review.
- `tools/mock_device_server.py` still exposed `--burst-size-mb` help as `MB`
  even though the implementation and the documentation use binary `MiB`
  semantics (`1024 * 1024`).

### Repair

1. Updated `tools/mock_device_server.py` help text from `burst size in MB` to
   `burst size in MiB`.
2. Updated the `doc/Readme.md` pressure parameter table to explicitly list all
   requested defaults:
   - `--stress-mode`: `normal`
   - `--duration`: `0`
   - `--target-mbps`: `0`
   - `--stats-interval`: `1.0`
   - `--burst-size-mb`: `32.0`
   - `--burst-interval`: `1.0`
3. Kept `SelfTestCoverageTests` unchanged per the low-priority guidance.

### Re-Verification Commands

```powershell
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
git diff --check -- tools/mock_device_server.py tests/tools/test_mock_device_server.py doc/Readme.md
```

### Re-Verification Summary

- `python -m unittest discover -s tests/tools -p "test_*.py" -v`
  - Passed: `27` tests.
- `python tools/mock_device_server.py --self-test`
  - Passed: printed `self-test passed`.
- `python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py`
  - Passed with no output.
- `git diff --check -- tools/mock_device_server.py tests/tools/test_mock_device_server.py doc/Readme.md`
  - Passed; Git still prints LF->CRLF working-copy warnings for existing files,
    but there are no scoped diff-check errors.
