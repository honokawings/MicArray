# Task 4: Integration Coverage and Functional Documentation

## Scope

Modify only:

- `tools/mock_device_server.py`
- `tests/tools/test_mock_device_server.py`
- `doc/Readme.md`

Preserve all prior tasks and unrelated dirty-worktree content. Do not modify
Qt/C++, CMake, specs, plans, `docs/Readme.md`, or unrelated files.

## Requirements

Follow test-driven development for executable behavior.

### Built-in self-test

Extend `self_test()` without opening a listening network port:

- Parse and validate burst mode arguments with video disabled.
- Use a fake socket and `send_burst` with target 100000 bytes.
- Assert actual bytes are at least the target.
- Assert actual bytes are a multiple of the complete audio frame size.
- Assert sequence equals number of writes.
- Assert every write has audio header bytes `7e 7e 11 ee`.
- Assert every write ends `0a 0d`.
- Assert `rate_limit_delay(10.0, 12_500_000, 100.0, 10.75)` is 0.25
  within floating-point tolerance.

Keep all existing self-test assertions.

### Socket integration test

Add `SocketIntegrationTests` using `socket.socketpair()` when available,
decorated with `unittest.skipUnless`.

- Build one real 38400-byte audio payload.
- Call `send_media_frame` through one socket.
- Read from the peer until the returned byte count is received.
- Assert exact frame length, first 8 bytes
  `7e 7e 11 ee 00 96 00 00`, and final bytes `0a 0d`.
- Use timeouts or deterministic cleanup so a failure cannot hang the suite.

### Documentation

Update the existing `## 模拟服务端` section of `doc/Readme.md` in Chinese.
Preserve all existing content. Add:

- `normal` command and statement that it preserves FPS scheduling.
- Unlimited sustained command with 60-second duration.
- 100 Mb/s sustained command with 60-second duration.
- 64 MiB-per-second burst example.
- Parameter table for `--stress-mode`, `--duration`, `--target-mbps`,
  `--stats-interval`, `--burst-size-mb`, `--burst-interval`.
- Clarify decimal Mb/s versus binary MiB.
- Explain `tx`, `average`, `peak`, `blocked`, `max_send`, `bursts`, and
  `overruns`.
- Explain that stress data contains complete legal protocol frames and that
  results do not replace real-device/network acceptance.
- Add a 2026-07-09 change-log row summarizing sustained/burst modes,
  instrumentation, automated tests, and verification commands.

Do not update `docs/Readme.md`; `doc/Readme.md` is the canonical functional
document for this task.

## Verification

Run:

```powershell
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
python -m py_compile tools/mock_device_server.py tests/tools/test_mock_device_server.py
git diff --check -- tools/mock_device_server.py tests/tools/test_mock_device_server.py doc/Readme.md
```

All tests and checks must pass. Report exact red-test evidence for the socket
integration test and final complete summaries.
