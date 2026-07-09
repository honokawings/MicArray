# Task 2 Report: Instrumented Sending and Rate Limiting

## Scope

Only these files were changed:

- `tools/mock_device_server.py`
- `tests/tools/test_mock_device_server.py`

## Original Red-Lights

First run of the new sending-metrics test class failed because the required API was missing:

```text
python -m unittest tests.tools.test_mock_device_server.SendingMetricsTests -v
...
AttributeError: module 'tools.mock_device_server' has no attribute 'send_media_frame'
AttributeError: module 'tools.mock_device_server' has no attribute 'rate_limit_delay'
```

That red state matched the brief: the server had no instrumented send path and no rate-limit helper yet.

## Implementation

In `tools/mock_device_server.py`:

- Added `BLOCKED_SEND_THRESHOLD_SECONDS = 0.010`.
- Added `BITS_PER_MEGABIT = 1_000_000`.
- Extended `ClientStats` with the requested send-metrics fields while preserving the existing heartbeat/control/receive/frame counters.
- Added `ClientStats.record_send(...)` to:
  - count successful send bytes and calls,
  - count audio/video totals and interval totals,
  - update max send duration,
  - count blocked calls at or above 10 ms,
  - accumulate only blocked-call elapsed time.
- Added `send_media_frame(...)` to:
  - pack one complete media frame,
  - time `sendall`,
  - update stats only after successful send completion,
  - return the full frame length,
  - leave socket exceptions uncaught.
- Added `rate_limit_delay(...)` using cumulative bytes and the requested delay formula.
- Wired the new send helper into `handle_client`, and applied the rate-limit delay in sustained mode.
- Initialized session/interval timestamps at client start.

In `tests/tools/test_mock_device_server.py`:

- Added a simple `FakeSocket` that records full writes.
- Added `SendingMetricsTests` covering:
  - a 25 ms video send,
  - a send under 10 ms,
  - a failing `sendall`,
  - the target-rate delay formula,
  - unlimited target behavior,
  - behind-schedule behavior.

## Self-Tests

Passed:

```text
python -m unittest tests.tools.test_mock_device_server.SendingMetricsTests -v
Ran 6 tests in 0.034s
OK
```

Passed:

```text
python -m unittest tests.tools.test_mock_device_server.ArgumentValidationTests -v
Ran 8 tests in 0.022s
OK
```

Passed:

```text
python tools/mock_device_server.py --self-test
self-test passed
```

Passed:

```text
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
```

## Final Command Results

All brief-required commands completed successfully after implementation.

## Self-Review

- The new tests are behavior-focused and use real code paths through `send_media_frame` and `rate_limit_delay`.
- The failure path leaves success metrics unchanged, which matches the brief.
- The rate-limit helper follows the exact cumulative-byte formula requested.
- I did not modify any unrelated repository files, and I did not revert existing work.
- Remaining risk is limited to untested integration behavior beyond the helper API, but the direct coverage requested by the brief is in place.

## File List

- `tools/mock_device_server.py`
- `tests/tools/test_mock_device_server.py`
- `.superpowers/sdd/task-2-report.md`

## Repair Note

Per review feedback, I removed the premature Task 2 wiring from `handle_client`:

- restored direct `conn.sendall(pack_media_frame(...))` sends for audio and video,
- restored the original per-frame counters in the run loop,
- removed the sustained-mode limiter sleep from the live send path,
- removed the unused session timestamp initialization.

The Task 2 support code and tests remain in place:

- `ClientStats` new fields
- `ClientStats.record_send(...)`
- `send_media_frame(...)`
- `rate_limit_delay(...)`
- `SendingMetricsTests`

## Repair Verification

Re-ran the combined Task 1 + Task 2 test file:

```text
python -m unittest tests.tools.test_mock_device_server -v
Ran 14 tests in 0.036s
OK
```

Re-ran the requested self-test:

```text
python tools/mock_device_server.py --self-test
self-test passed
```

Re-ran bytecode compilation:

```text
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
```
