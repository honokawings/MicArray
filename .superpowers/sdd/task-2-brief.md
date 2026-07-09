# Task 2: Instrumented Sending and Rate Limiting

## Scope

Modify only:

- `tools/mock_device_server.py`
- `tests/tools/test_mock_device_server.py`

Do not modify Qt/C++ sources, docs, plans, specs, or unrelated files. Other
work exists in the repository; preserve it.

## Requirements

Follow test-driven development and preserve all Task 1 tests.

Add constants:

```python
BLOCKED_SEND_THRESHOLD_SECONDS = 0.010
BITS_PER_MEGABIT = 1_000_000
```

Extend `ClientStats` without removing `heartbeats`, `controls`,
`received_bytes`, `sent_audio_frames`, or `sent_video_frames`. Add:

```python
sent_bytes: int = 0
send_calls: int = 0
blocked_send_calls: int = 0
total_blocked_seconds: float = 0.0
max_send_seconds: float = 0.0
burst_count: int = 0
burst_bytes: int = 0
burst_overruns: int = 0
peak_interval_mbps: float = 0.0
session_started_at: float = 0.0
interval_started_at: float = 0.0
interval_sent_bytes: int = 0
interval_audio_frames: int = 0
interval_video_frames: int = 0
```

Add:

```python
def ClientStats.record_send(
    self, frame_type: int, byte_count: int, elapsed: float
) -> None
```

It must:

- Count successful bytes and calls.
- Count audio/video frame totals and interval totals by frame type.
- Update maximum send duration.
- Count a blocked call when elapsed is at least 10 ms.
- Add only blocked-call elapsed time to `total_blocked_seconds`.

Add:

```python
def send_media_frame(
    conn: socket.socket,
    frame_type: int,
    payload: bytes,
    stats: ClientStats,
    clock=time.monotonic,
) -> int
```

It must pack one complete media frame, time `conn.sendall(frame)`, update
statistics only after `sendall` returns successfully, and return the complete
frame byte count. Do not catch socket errors here.

Add:

```python
def rate_limit_delay(
    start_time: float,
    sent_bytes: int,
    target_mbps: float,
    now: float,
) -> float
```

Use cumulative application bytes:

```text
expected elapsed = sent_bytes * 8 / (target_mbps * 1,000,000)
delay = max(0, expected elapsed - max(0, now - start_time))
```

Return zero when target is zero or negative.

## Tests

Add a simple `FakeSocket` that records complete writes. Cover:

1. A 25 ms video send updates sent bytes, video frames, call count, blocked
   count, cumulative blocked time, and maximum send time.
2. A send below 10 ms is not counted as blocked but still updates maximum.
3. A failing `sendall` does not update any successful-send counters.
4. Sending 12,500,000 bytes at 100 Mb/s after 0.75 s returns a 0.25 s delay.
5. Unlimited target returns zero.
6. A sender already behind schedule returns zero.

Use injected iterator-backed clocks for deterministic timing.

Run:

```powershell
python -m unittest tests.tools.test_mock_device_server.SendingMetricsTests -v
python -m unittest tests.tools.test_mock_device_server.ArgumentValidationTests -v
python tools/mock_device_server.py --self-test
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
```

Report exact red-test evidence and complete final test summaries.
