# Final Review Fix Brief

## Scope

Modify only:

- `tools/mock_device_server.py`
- `tests/tools/test_mock_device_server.py`
- `doc/Readme.md` only if externally visible behavior changes

Preserve all existing work and do not touch Qt/C++.

## Required Fixes

1. Long/overrunning bursts must not starve video or periodic statistics.
   Keep frame-granular bounded memory. After every complete audio frame, the
   live burst scheduler must be able to send due video and emit due statistics.
   Never split an audio frame.
2. All scheduler waits must be bounded by the nearest applicable deadline:
   session duration, next statistics line, next video frame, burst start, and
   sustained rate-limit target. Reaching duration stops new frame scheduling.
3. Configure a finite socket timeout before starting sender/receiver work so a
   blocked `sendall()` cannot hang the server indefinitely. A socket timeout
   ends only the current client session through existing error handling.
4. Use `finite_float` for all float CLI inputs, including existing audio FPS,
   video FPS, and jitter. Reject non-finite and invalid derived burst sizes
   before accepting a client. Add a pure checked conversion helper for MiB to
   target bytes; it must reject overflow and zero/negative derived values with
   an actionable `ValueError`.
5. Catch unexpected per-client generation/arithmetic/packing exceptions at the
   client-session boundary, log them, close that client, and let `run_server`
   continue accepting the next client. Do not catch `KeyboardInterrupt` or
   `SystemExit`.
6. Add scheduler-level automated coverage. Prefer extracting a deterministic
   transmit-loop helper with injected monotonic clock/wait/output functions,
   or use a bounded fake socket. Cover:
   - normal pacing remains FPS based;
   - sustained mode does not add normal audio interval;
   - low target rate wait is cut short by session/stat/video deadlines;
   - burst overrun still services due video/statistics between complete frames;
   - duration prevents scheduling another frame;
   - unexpected client-session exception does not escape `handle_client`;
   - non-finite legacy floats and overflowing burst size are rejected.
7. Keep existing protocol, self-test, socketpair, and all 27 tests passing.

## Clarifications

- The working tree is intentionally uncommitted at the user's request. The
  untracked test file is a deliverable and must remain present; do not stage,
  commit, or delete it.
- `burst_bytes` means bytes in completed bursts, consistent with the existing
  completed-burst count. Keep this meaning and document it in a short code
  comment or `doc/Readme.md`; do not count partial bursts as completed.
- Do not add third-party dependencies.

## Verification

Run and report:

```powershell
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
git diff --check -- tools/mock_device_server.py tests/tools/test_mock_device_server.py doc/Readme.md
```

Append the complete fix and test report to
`.superpowers/sdd/final-fix-report.md`.
