# Task 3: Sustained and Burst Schedulers

## Scope

Modify only:

- `tools/mock_device_server.py`
- `tests/tools/test_mock_device_server.py`

Preserve Tasks 1 and 2. Do not modify Qt/C++, docs, plans, specs, or unrelated
files. Other work exists; never revert it.

## Requirements

Follow test-driven development. Send only complete protocol-valid frames.

Add:

```python
def send_next_audio(
    conn, args, stats: ClientStats, sequence: int, clock=time.monotonic
) -> int
```

Generate the configured 38400-byte audio payload for `sequence`, send it
through `send_media_frame`, and return the complete frame byte count.

Add:

```python
def send_burst(
    conn,
    args,
    stats: ClientStats,
    sequence: int,
    target_bytes: int,
    clock=time.monotonic,
) -> tuple[int, int]
```

Send one complete audio frame at a time until sent bytes are at least
`target_bytes`. Never concatenate the entire burst in memory and never split a
frame. Return `(next_sequence, actual_sent_bytes)`. Increment `burst_count` and
add to `burst_bytes` only after completing the burst.

Add:

```python
def format_periodic_stats(stats: ClientStats, now: float) -> str
def format_session_summary(stats: ClientStats, now: float) -> str
```

Use decimal Mb/s (`bytes * 8 / seconds / 1_000_000`) for rates and MiB
(`bytes / 1024 / 1024`) for quantities.

Periodic output must contain:

- interval `tx=... Mb/s`
- interval audio/video FPS
- cumulative sent MiB
- cumulative blocked milliseconds

It must update `peak_interval_mbps`, advance `interval_started_at`, and reset
only interval bytes/audio/video fields.

Session summary must contain:

- duration
- total sent MiB
- average and peak Mb/s
- total audio/video frames
- blocked calls / send calls
- maximum send duration in ms
- bursts and overruns

Update `handle_client`:

1. Initialize `ClientStats.session_started_at` and `interval_started_at` from
   one monotonic session start value.
2. Keep the receiver thread and existing exception/finally behavior.
3. Stop scheduling when `duration > 0` and elapsed reaches duration.
4. `normal`: send audio via `send_next_audio`; send due video via
   `send_media_frame`; retain existing `audio_interval` plus jitter behavior.
5. `sustained`: continuously send audio; send due video via
   `send_media_frame`; apply only `rate_limit_delay` when target is nonzero.
   Never also sleep for `audio_interval` and never add jitter in this mode.
6. `burst`: each period call `send_burst` using
   `ceil(burst_size_mb * 1024 * 1024)`. Video may be sent between bursts when
   due. Use monotonic burst start deadlines. If a burst completes after its
   next deadline, increment `burst_overruns` and begin the next burst
   immediately; do not spawn threads or queue catch-up work. Waiting must use
   `stop_event.wait(timeout)` so disconnect can interrupt the wait.
7. Emit periodic statistics in all modes at `stats_interval`. A long burst may
   delay a periodic line until that burst completes.
8. Print the session summary in `finally`, plus existing heartbeat/control
   receive totals.
9. The listen banner must include stress mode and its relevant settings.
10. `normal` remains the default behavior.

Do not catch or suppress send errors in scheduler helpers. Existing
`handle_client` connection error handling remains responsible for them.

## Tests

Add `SchedulerTests` covering:

1. Burst reaches a 100000-byte target using only complete audio frames.
2. Every burst write starts `7e 7e 11 ee`, contains payload length 38400, and
   ends `0a 0d`.
3. `sequence` advances by exactly the number of frames.
4. Burst metrics update after completion.
5. Periodic report computes 8.00 Mb/s for 1000000 bytes in 1 second and resets
   only interval counters.
6. Session summary computes 100.00 Mb/s for 12500000 bytes in 1 second and
   includes `blocked=2/100`, `max_send=125.0 ms`, bursts, and overruns.
7. A small local socket or deterministic loop test proves normal mode still
   uses FPS pacing while sustained mode does not add `audio_interval` sleep.
   Extract a small pure scheduling-delay helper if needed; do not open a
   listening TCP port in unit tests.

Run:

```powershell
python -m unittest tests.tools.test_mock_device_server.SchedulerTests -v
python -m unittest tests.tools.test_mock_device_server -v
python tools/mock_device_server.py --self-test
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
```

Report exact red-test evidence and full final summaries.
