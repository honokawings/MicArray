# Task 3 Report

## Scope

Implemented Task 3 only in:

- `tools/mock_device_server.py`
- `tests/tools/test_mock_device_server.py`

Report file:

- `.superpowers/sdd/task-3-report.md`

No other code files were modified for this task.

## Original Red-Test Evidence

Command:

```powershell
python -m unittest tests.tools.test_mock_device_server.SchedulerTests -v
```

Key output:

```text
ERROR: test_burst_frames_are_protocol_valid_audio_frames
AttributeError: module 'tools.mock_device_server' has no attribute 'send_burst'

ERROR: test_delay_helper_keeps_normal_pacing_and_skips_audio_interval_in_sustained_mode
AttributeError: module 'tools.mock_device_server' has no attribute 'compute_audio_send_delay'

ERROR: test_periodic_report_computes_rate_and_resets_only_interval_counters
AttributeError: module 'tools.mock_device_server' has no attribute 'format_periodic_stats'

ERROR: test_session_summary_reports_expected_rates_and_burst_metrics
AttributeError: module 'tools.mock_device_server' has no attribute 'format_session_summary'

Ran 6 tests in 0.031s
FAILED (errors=6)
```

## Scheduling Design

1. Added `send_next_audio()` to generate one 38400-byte audio payload, frame it through `send_media_frame()`, and return the full frame byte count.
2. Added `send_burst()` to send complete audio frames until the sent total reaches or exceeds the target, with no aggregate burst buffer and no partial-frame sends.
3. Added `compute_audio_send_delay()` as the small pure pacing helper used by tests:
   - `normal`: returns `audio_interval + jitter`
   - `sustained`: returns only `rate_limit_delay(...)`
4. Added periodic/session formatting helpers:
   - periodic stats compute decimal Mb/s from interval bytes, update `peak_interval_mbps`, advance `interval_started_at`, and reset only interval counters
   - session summary computes duration, total MiB, average/peak Mb/s, blocked/send counts, max send ms, and burst metrics
5. Reworked `handle_client()` scheduling:
   - single monotonic session start initializes both `session_started_at` and `interval_started_at`
   - `normal`: audio pacing preserved through interval+jitter delay helper
   - `sustained`: continuous audio sending, optional rate-limit sleep only
   - `burst`: monotonic burst deadlines, interruptible wait via `stop_event.wait(timeout)`, overdue bursts counted by `burst_overruns`, video only between bursts
   - periodic stats emitted in every mode
   - final summary printed in `finally`
6. Updated the listen banner to include stress mode and its relevant settings.

## Final Verification

Command:

```powershell
python -m unittest tests.tools.test_mock_device_server.SchedulerTests -v
```

Output:

```text
Ran 6 tests in 0.240s
OK
```

Command:

```powershell
python -m unittest tests.tools.test_mock_device_server -v
```

Output:

```text
Ran 20 tests in 0.272s
OK
```

Command:

```powershell
python tools/mock_device_server.py --self-test
```

Output:

```text
self-test passed
```

Command:

```powershell
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
```

Output:

```text
[no output, exit code 0]
```

## Self-Review

- Burst sending is frame-granular only; no partial audio frame writes and no whole-burst concatenation buffer.
- Burst accounting updates only after a successful full burst. The failure test keeps `burst_count` and `burst_bytes` unchanged.
- Periodic stats reset only interval counters and preserve cumulative counters.
- Sustained mode does not add `audio_interval` sleep; normal mode still uses FPS pacing.
- Session summary time is captured before cleanup joins so teardown does not inflate the reported duration.
- Existing connection error handling and receiver thread lifecycle were preserved.

## File List

- `tools/mock_device_server.py`
- `tests/tools/test_mock_device_server.py`
- `.superpowers/sdd/task-3-report.md`

## Review Fix Round 1

### Root Cause

1. `send_burst()` only stopped after reaching `target_bytes`, so a burst could continue sending multiple extra frames after `stop_event` or session `duration` had become terminal.
2. `handle_client()` printed the session summary without flushing a non-empty final interval, so sessions shorter than `stats_interval` never updated `peak_interval_mbps` from their last traffic window.

### New Red-Test Evidence

Command:

```powershell
python -m unittest tests.tools.test_mock_device_server.SchedulerTests -v
```

Full output:

```text
test_burst_frames_are_protocol_valid_audio_frames (tests.tools.test_mock_device_server.SchedulerTests.test_burst_frames_are_protocol_valid_audio_frames) ... ok
test_burst_metrics_update_only_after_completion (tests.tools.test_mock_device_server.SchedulerTests.test_burst_metrics_update_only_after_completion) ... ok
test_burst_reaches_target_using_only_complete_audio_frames (tests.tools.test_mock_device_server.SchedulerTests.test_burst_reaches_target_using_only_complete_audio_frames) ... ok
test_burst_stop_after_complete_frame_does_not_count_incomplete_burst (tests.tools.test_mock_device_server.SchedulerTests.test_burst_stop_after_complete_frame_does_not_count_incomplete_burst) ... ERROR
test_delay_helper_keeps_normal_pacing_and_skips_audio_interval_in_sustained_mode (tests.tools.test_mock_device_server.SchedulerTests.test_delay_helper_keeps_normal_pacing_and_skips_audio_interval_in_sustained_mode) ... ok
test_final_interval_flush_emits_stats_and_updates_peak_before_summary (tests.tools.test_mock_device_server.SchedulerTests.test_final_interval_flush_emits_stats_and_updates_peak_before_summary) ... ERROR
test_final_interval_flush_skips_empty_interval (tests.tools.test_mock_device_server.SchedulerTests.test_final_interval_flush_skips_empty_interval) ... ERROR
test_periodic_report_computes_rate_and_resets_only_interval_counters (tests.tools.test_mock_device_server.SchedulerTests.test_periodic_report_computes_rate_and_resets_only_interval_counters) ... ok
test_session_summary_reports_expected_rates_and_burst_metrics (tests.tools.test_mock_device_server.SchedulerTests.test_session_summary_reports_expected_rates_and_burst_metrics) ... ok

======================================================================
ERROR: test_burst_stop_after_complete_frame_does_not_count_incomplete_burst (tests.tools.test_mock_device_server.SchedulerTests.test_burst_stop_after_complete_frame_does_not_count_incomplete_burst)
----------------------------------------------------------------------
Traceback (most recent call last):
  File "D:\Workspace\QT\MicArray\tests\tools\test_mock_device_server.py", line 254, in test_burst_stop_after_complete_frame_does_not_count_incomplete_burst
    next_sequence, actual_sent_bytes = self.mod.send_burst(
                                       ^^^^^^^^^^^^^^^^^^^^
TypeError: send_burst() got an unexpected keyword argument 'should_stop'

======================================================================
ERROR: test_final_interval_flush_emits_stats_and_updates_peak_before_summary (tests.tools.test_mock_device_server.SchedulerTests.test_final_interval_flush_emits_stats_and_updates_peak_before_summary)
----------------------------------------------------------------------
Traceback (most recent call last):
  File "D:\Workspace\QT\MicArray\tests\tools\test_mock_device_server.py", line 342, in test_final_interval_flush_emits_stats_and_updates_peak_before_summary
    emitted = self.mod.flush_final_interval_stats(stats, now=11.0, emit=lines.append)
              ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
AttributeError: module 'tools.mock_device_server' has no attribute 'flush_final_interval_stats'

======================================================================
ERROR: test_final_interval_flush_skips_empty_interval (tests.tools.test_mock_device_server.SchedulerTests.test_final_interval_flush_skips_empty_interval)
----------------------------------------------------------------------
Traceback (most recent call last):
  File "D:\Workspace\QT\MicArray\tests\tools\test_mock_device_server.py", line 355, in test_final_interval_flush_skips_empty_interval
    emitted = self.mod.flush_final_interval_stats(stats, now=11.0, emit=lines.append)
              ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
AttributeError: module 'tools.mock_device_server' has no attribute 'flush_final_interval_stats'

----------------------------------------------------------------------
Ran 9 tests in 0.244s

FAILED (errors=3)
```

### Fix Design

1. Extended `send_burst()` with backward-compatible optional `should_stop` and `deadline` parameters.
2. Added stop checks before each new frame send so an in-flight `sendall()` is never truncated, but no additional frames are started once stop/deadline is observed.
3. Kept incomplete bursts out of `burst_count` and `burst_bytes`.
4. Added `flush_final_interval_stats()` to flush only non-empty interval counters through `format_periodic_stats()` before summary generation.
5. Wired `handle_client()` burst mode to pass `stop_event.is_set` and the session deadline into `send_burst()`, and flush the final interval in `finally`.

### Final Verification After Review Fix

Command:

```powershell
python -m unittest tests.tools.test_mock_device_server.SchedulerTests -v
```

Full output:

```text
test_burst_frames_are_protocol_valid_audio_frames (tests.tools.test_mock_device_server.SchedulerTests.test_burst_frames_are_protocol_valid_audio_frames) ... ok
test_burst_metrics_update_only_after_completion (tests.tools.test_mock_device_server.SchedulerTests.test_burst_metrics_update_only_after_completion) ... ok
test_burst_reaches_target_using_only_complete_audio_frames (tests.tools.test_mock_device_server.SchedulerTests.test_burst_reaches_target_using_only_complete_audio_frames) ... ok
test_burst_stop_after_complete_frame_does_not_count_incomplete_burst (tests.tools.test_mock_device_server.SchedulerTests.test_burst_stop_after_complete_frame_does_not_count_incomplete_burst) ... ok
test_delay_helper_keeps_normal_pacing_and_skips_audio_interval_in_sustained_mode (tests.tools.test_mock_device_server.SchedulerTests.test_delay_helper_keeps_normal_pacing_and_skips_audio_interval_in_sustained_mode) ... ok
test_final_interval_flush_emits_stats_and_updates_peak_before_summary (tests.tools.test_mock_device_server.SchedulerTests.test_final_interval_flush_emits_stats_and_updates_peak_before_summary) ... ok
test_final_interval_flush_skips_empty_interval (tests.tools.test_mock_device_server.SchedulerTests.test_final_interval_flush_skips_empty_interval) ... ok
test_periodic_report_computes_rate_and_resets_only_interval_counters (tests.tools.test_mock_device_server.SchedulerTests.test_periodic_report_computes_rate_and_resets_only_interval_counters) ... ok
test_session_summary_reports_expected_rates_and_burst_metrics (tests.tools.test_mock_device_server.SchedulerTests.test_session_summary_reports_expected_rates_and_burst_metrics) ... ok

----------------------------------------------------------------------
Ran 9 tests in 0.284s

OK
```

Command:

```powershell
python -m unittest tests.tools.test_mock_device_server -v
```

Full output:

```text
test_burst_mode_rejects_target_mbps_via_main (tests.tools.test_mock_device_server.ArgumentValidationTests.test_burst_mode_rejects_target_mbps_via_main) ... ok
test_default_pressure_arguments_validate_successfully (tests.tools.test_mock_device_server.ArgumentValidationTests.test_default_pressure_arguments_validate_successfully) ... ok
test_infinite_burst_interval_is_rejected (tests.tools.test_mock_device_server.ArgumentValidationTests.test_infinite_burst_interval_is_rejected) ... ok
test_nan_target_is_rejected (tests.tools.test_mock_device_server.ArgumentValidationTests.test_nan_target_is_rejected) ... ok
test_negative_duration_is_rejected (tests.tools.test_mock_device_server.ArgumentValidationTests.test_negative_duration_is_rejected) ... ok
test_sustained_mode_accepts_target_mbps (tests.tools.test_mock_device_server.ArgumentValidationTests.test_sustained_mode_accepts_target_mbps) ... ok
test_zero_burst_size_is_rejected (tests.tools.test_mock_device_server.ArgumentValidationTests.test_zero_burst_size_is_rejected) ... ok
test_zero_stats_interval_is_rejected (tests.tools.test_mock_device_server.ArgumentValidationTests.test_zero_stats_interval_is_rejected) ... ok
test_burst_frames_are_protocol_valid_audio_frames (tests.tools.test_mock_device_server.SchedulerTests.test_burst_frames_are_protocol_valid_audio_frames) ... ok
test_burst_metrics_update_only_after_completion (tests.tools.test_mock_device_server.SchedulerTests.test_burst_metrics_update_only_after_completion) ... ok
test_burst_reaches_target_using_only_complete_audio_frames (tests.tools.test_mock_device_server.SchedulerTests.test_burst_reaches_target_using_only_complete_audio_frames) ... ok
test_burst_stop_after_complete_frame_does_not_count_incomplete_burst (tests.tools.test_mock_device_server.SchedulerTests.test_burst_stop_after_complete_frame_does_not_count_incomplete_burst) ... ok
test_delay_helper_keeps_normal_pacing_and_skips_audio_interval_in_sustained_mode (tests.tools.test_mock_device_server.SchedulerTests.test_delay_helper_keeps_normal_pacing_and_skips_audio_interval_in_sustained_mode) ... ok
test_final_interval_flush_emits_stats_and_updates_peak_before_summary (tests.tools.test_mock_device_server.SchedulerTests.test_final_interval_flush_emits_stats_and_updates_peak_before_summary) ... ok
test_final_interval_flush_skips_empty_interval (tests.tools.test_mock_device_server.SchedulerTests.test_final_interval_flush_skips_empty_interval) ... ok
test_periodic_report_computes_rate_and_resets_only_interval_counters (tests.tools.test_mock_device_server.SchedulerTests.test_periodic_report_computes_rate_and_resets_only_interval_counters) ... ok
test_session_summary_reports_expected_rates_and_burst_metrics (tests.tools.test_mock_device_server.SchedulerTests.test_session_summary_reports_expected_rates_and_burst_metrics) ... ok
test_failed_sendall_does_not_update_successful_counters (tests.tools.test_mock_device_server.SendingMetricsTests.test_failed_sendall_does_not_update_successful_counters) ... ok
test_fast_send_updates_maximum_without_blocked_count (tests.tools.test_mock_device_server.SendingMetricsTests.test_fast_send_updates_maximum_without_blocked_count) ... ok
test_rate_limit_delay_for_target_schedule (tests.tools.test_mock_device_server.SendingMetricsTests.test_rate_limit_delay_for_target_schedule) ... ok
test_rate_limit_delay_returns_zero_for_unlimited_target (tests.tools.test_mock_device_server.SendingMetricsTests.test_rate_limit_delay_returns_zero_for_unlimited_target) ... ok
test_rate_limit_delay_returns_zero_when_already_behind (tests.tools.test_mock_device_server.SendingMetricsTests.test_rate_limit_delay_returns_zero_when_already_behind) ... ok
test_video_send_records_blocked_metrics_for_slow_send (tests.tools.test_mock_device_server.SendingMetricsTests.test_video_send_records_blocked_metrics_for_slow_send) ... ok

----------------------------------------------------------------------
Ran 23 tests in 0.313s

OK
```

Command:

```powershell
python tools/mock_device_server.py --self-test
```

Full output:

```text
self-test passed
```

Command:

```powershell
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
```

Full output:

```text
[no output, exit code 0]
```

### Review Fix Self-Review

- Burst stop/deadline checks happen only at frame boundaries, so no partially-sent protocol frame can escape.
- A burst interrupted after one or more full frames returns the advanced `sequence` and actual bytes sent, but does not count as a completed burst.
- Final interval flushing is idempotent for empty intervals and updates `peak_interval_mbps` before summary formatting on short sessions.
- `normal` and `sustained` pacing behavior stayed unchanged outside the reviewed edge cases.
