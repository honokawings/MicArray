import argparse
import contextlib
import importlib.util
import io
import pathlib
import socket
import struct
import sys
import tempfile
import threading
import unittest


MODULE_PATH = pathlib.Path(__file__).resolve().parents[2] / "tools" / "mock_device_server.py"


def load_module():
    spec = importlib.util.spec_from_file_location("tools.mock_device_server", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class VideoPayloadTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_builtin_video_payload_equals_nonempty_asset_bytes(self):
        self.assertTrue(self.mod.VIDEO_ASSET_PATH.is_file())
        self.assertEqual(self.mod.JPEG_PAYLOAD, self.mod.VIDEO_ASSET_PATH.read_bytes())
        self.assertTrue(self.mod.JPEG_PAYLOAD)

    def test_load_video_payload_rejects_missing_asset(self):
        missing = pathlib.Path(__file__).with_name("missing-video-asset.jpg")
        with self.assertRaisesRegex(RuntimeError, "Unable to read mock video asset"):
            self.mod.load_video_payload(missing)

    def test_load_video_payload_rejects_empty_asset(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "empty.jpg"
            path.write_bytes(b"")
            with self.assertRaisesRegex(RuntimeError, "is empty"):
                self.mod.load_video_payload(path)

    def test_load_video_payload_accepts_nonempty_opaque_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "opaque.bin"
            payload = b"opaque mock video payload\x00\xff"
            path.write_bytes(payload)
            self.assertEqual(self.mod.load_video_payload(path), payload)


class ArgumentValidationTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_default_pressure_arguments_validate_successfully(self):
        parser = self.mod.build_parser()
        args = parser.parse_args([])
        self.assertEqual(args.stress_mode, "normal")
        self.assertEqual(args.duration, 0.0)
        self.assertEqual(args.target_mbps, 0.0)
        self.assertEqual(args.stats_interval, 1.0)
        self.assertEqual(args.burst_size_mb, 32.0)
        self.assertEqual(args.burst_interval, 1.0)
        self.mod.validate_args(args)

    def test_sustained_mode_accepts_target_mbps(self):
        args = self.mod.build_parser().parse_args(["--stress-mode", "sustained", "--target-mbps", "100"])
        self.mod.validate_args(args)

    def test_burst_mode_rejects_target_mbps_via_main(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit):
                self.mod.main(["--stress-mode", "burst", "--target-mbps", "100"])
        self.assertIn("--target-mbps requires --stress-mode sustained", stderr.getvalue())

    def test_negative_duration_is_rejected(self):
        args = self.mod.build_parser().parse_args(["--duration", "-0.1"])
        with self.assertRaises(ValueError):
            self.mod.validate_args(args)

    def test_nan_target_is_rejected(self):
        with self.assertRaises(argparse.ArgumentTypeError):
            self.mod.finite_float("nan")

    def test_zero_stats_interval_is_rejected(self):
        args = self.mod.build_parser().parse_args(["--stats-interval", "0"])
        with self.assertRaises(ValueError):
            self.mod.validate_args(args)

    def test_zero_burst_size_is_rejected(self):
        args = self.mod.build_parser().parse_args(["--burst-size-mb", "0"])
        with self.assertRaises(ValueError):
            self.mod.validate_args(args)

    def test_infinite_burst_interval_is_rejected(self):
        with self.assertRaises(argparse.ArgumentTypeError):
            self.mod.finite_float("inf")

    def test_burst_size_help_uses_mib_units(self):
        burst_size_action = next(
            action for action in self.mod.build_parser()._actions if action.dest == "burst_size_mb"
        )
        self.assertEqual(burst_size_action.help, "burst size in MiB")

    def test_legacy_float_arguments_reject_non_finite_values(self):
        parser = self.mod.build_parser()
        for option, value in (
            ("--audio-fps", "nan"),
            ("--video-fps", "inf"),
            ("--jitter-ms", "nan"),
        ):
            with self.subTest(option=option):
                with contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        parser.parse_args([option, value])

    def test_mib_conversion_rejects_zero_with_actionable_error(self):
        with self.assertRaisesRegex(ValueError, "--burst-size-mb must produce at least 1 byte"):
            self.mod.mib_to_target_bytes(0.0)

    def test_mib_conversion_rejects_overflow_with_actionable_error(self):
        with self.assertRaisesRegex(ValueError, "--burst-size-mb is too large"):
            self.mod.mib_to_target_bytes(sys.float_info.max)

    def test_validation_rejects_overflowing_burst_size(self):
        args = self.mod.build_parser().parse_args(
            ["--burst-size-mb", str(sys.float_info.max)]
        )
        with self.assertRaisesRegex(ValueError, "--burst-size-mb is too large"):
            self.mod.validate_args(args)


class FakeSocket:
    def __init__(self, *, fail: Exception | None = None):
        self.writes: list[bytes] = []
        self.fail = fail

    def sendall(self, data: bytes) -> None:
        if self.fail is not None:
            raise self.fail
        self.writes.append(data)


class FailOnNthSendSocket(FakeSocket):
    def __init__(self, fail_on_call: int):
        super().__init__()
        self.fail_on_call = fail_on_call
        self.calls = 0

    def sendall(self, data: bytes) -> None:
        self.calls += 1
        if self.calls == self.fail_on_call:
            raise OSError("burst send failed")
        self.writes.append(data)


class FakeTimeline:
    def __init__(self, start: float = 0.0):
        self.now = start
        self.waits: list[float] = []

    def clock(self) -> float:
        return self.now

    def wait(self, timeout: float) -> bool:
        self.waits.append(timeout)
        self.now += timeout
        return False


class TimelineSocket(FakeSocket):
    def __init__(
        self,
        timeline: FakeTimeline,
        *,
        audio_send_seconds: float = 0.0,
        stop_event: threading.Event | None = None,
        stop_after_audio_frames: int | None = None,
    ):
        super().__init__()
        self.timeline = timeline
        self.audio_send_seconds = audio_send_seconds
        self.stop_event = stop_event
        self.stop_after_audio_frames = stop_after_audio_frames
        self.audio_frames = 0

    def sendall(self, data: bytes) -> None:
        super().sendall(data)
        frame_type = struct.unpack("<H", data[2:4])[0]
        if frame_type != 0xEE11:
            return
        self.audio_frames += 1
        self.timeline.now += self.audio_send_seconds
        if self.stop_after_audio_frames == self.audio_frames:
            assert self.stop_event is not None
            self.stop_event.set()


class SendingMetricsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _clock(self, *values: float):
        iterator = iter(values)
        return lambda: next(iterator)

    def test_video_send_records_blocked_metrics_for_slow_send(self):
        stats = self.mod.ClientStats()
        conn = FakeSocket()
        payload = b"video"

        frame_bytes = self.mod.send_media_frame(
            conn,
            self.mod.TYPE_VIDEO,
            payload,
            stats,
            clock=self._clock(100.0, 100.025),
        )

        self.assertEqual(frame_bytes, len(conn.writes[0]))
        self.assertEqual(stats.sent_bytes, frame_bytes)
        self.assertEqual(stats.sent_video_frames, 1)
        self.assertEqual(stats.sent_audio_frames, 0)
        self.assertEqual(stats.send_calls, 1)
        self.assertEqual(stats.blocked_send_calls, 1)
        self.assertAlmostEqual(stats.total_blocked_seconds, 0.025)
        self.assertAlmostEqual(stats.max_send_seconds, 0.025)
        self.assertEqual(stats.interval_video_frames, 1)
        self.assertEqual(stats.interval_audio_frames, 0)
        self.assertEqual(stats.interval_sent_bytes, frame_bytes)

    def test_fast_send_updates_maximum_without_blocked_count(self):
        stats = self.mod.ClientStats()
        conn = FakeSocket()

        self.mod.send_media_frame(
            conn,
            self.mod.TYPE_AUDIO,
            b"audio",
            stats,
            clock=self._clock(200.0, 200.009),
        )

        self.assertEqual(stats.send_calls, 1)
        self.assertEqual(stats.blocked_send_calls, 0)
        self.assertEqual(stats.total_blocked_seconds, 0.0)
        self.assertAlmostEqual(stats.max_send_seconds, 0.009)

    def test_failed_sendall_does_not_update_successful_counters(self):
        stats = self.mod.ClientStats()
        conn = FakeSocket(fail=OSError("send failed"))

        with self.assertRaises(OSError):
            self.mod.send_media_frame(
                conn,
                self.mod.TYPE_AUDIO,
                b"audio",
                stats,
                clock=self._clock(300.0, 300.020),
            )

        self.assertEqual(conn.writes, [])
        self.assertEqual(stats.sent_bytes, 0)
        self.assertEqual(stats.send_calls, 0)
        self.assertEqual(stats.blocked_send_calls, 0)
        self.assertEqual(stats.total_blocked_seconds, 0.0)
        self.assertEqual(stats.max_send_seconds, 0.0)
        self.assertEqual(stats.sent_audio_frames, 0)
        self.assertEqual(stats.sent_video_frames, 0)

    def test_rate_limit_delay_for_target_schedule(self):
        delay = self.mod.rate_limit_delay(10.0, 12_500_000, 100.0, 10.75)
        self.assertAlmostEqual(delay, 0.25)

    def test_rate_limit_delay_returns_zero_for_unlimited_target(self):
        self.assertEqual(self.mod.rate_limit_delay(10.0, 1_000_000, 0.0, 10.5), 0.0)

    def test_rate_limit_delay_returns_zero_when_already_behind(self):
        delay = self.mod.rate_limit_delay(10.0, 1_000_000, 100.0, 10.5)
        self.assertEqual(delay, 0.0)


class SchedulerTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.args = self.mod.build_parser().parse_args([])

    def _stepping_clock(self, start: float = 100.0, step: float = 0.001):
        current = start - step

        def inner():
            nonlocal current
            current += step
            return current

        return inner

    def test_burst_reaches_target_using_only_complete_audio_frames(self):
        stats = self.mod.ClientStats()
        conn = FakeSocket()

        next_sequence, actual_sent_bytes = self.mod.send_burst(
            conn,
            self.args,
            stats,
            sequence=10,
            target_bytes=100000,
            clock=self._stepping_clock(),
        )

        self.assertEqual(len(conn.writes), 3)
        self.assertEqual(actual_sent_bytes, 3 * (self.mod.MEDIA_HEADER_LEN + self.mod.AUDIO_PAYLOAD_LEN + self.mod.MEDIA_TAIL_LEN))
        self.assertEqual(next_sequence, 13)

    def test_burst_frames_are_protocol_valid_audio_frames(self):
        stats = self.mod.ClientStats()
        conn = FakeSocket()

        self.mod.send_burst(
            conn,
            self.args,
            stats,
            sequence=0,
            target_bytes=100000,
            clock=self._stepping_clock(),
        )

        for frame in conn.writes:
            self.assertEqual(frame[:4], bytes.fromhex("7e 7e 11 ee"))
            self.assertEqual(struct.unpack("<I", frame[4:8])[0], self.mod.AUDIO_PAYLOAD_LEN)
            self.assertEqual(frame[-2:], bytes.fromhex("0a 0d"))

    def test_burst_metrics_update_only_after_completion(self):
        stats = self.mod.ClientStats()
        conn = FailOnNthSendSocket(fail_on_call=2)

        with self.assertRaises(OSError):
            self.mod.send_burst(
                conn,
                self.args,
                stats,
                sequence=0,
                target_bytes=100000,
                clock=self._stepping_clock(),
            )

        self.assertEqual(stats.burst_count, 0)
        self.assertEqual(stats.burst_bytes, 0)
        self.assertEqual(stats.sent_audio_frames, 1)

    def test_burst_stop_after_complete_frame_does_not_count_incomplete_burst(self):
        stats = self.mod.ClientStats()
        conn = FakeSocket()
        frame_bytes = self.mod.MEDIA_HEADER_LEN + self.mod.AUDIO_PAYLOAD_LEN + self.mod.MEDIA_TAIL_LEN

        next_sequence, actual_sent_bytes = self.mod.send_burst(
            conn,
            self.args,
            stats,
            sequence=7,
            target_bytes=100000,
            clock=self._stepping_clock(),
            should_stop=lambda: len(conn.writes) >= 1,
        )

        self.assertEqual(len(conn.writes), 1)
        self.assertEqual(next_sequence, 8)
        self.assertEqual(actual_sent_bytes, frame_bytes)
        self.assertEqual(stats.burst_count, 0)
        self.assertEqual(stats.burst_bytes, 0)
        self.assertEqual(stats.sent_audio_frames, 1)
        self.assertEqual(conn.writes[0][:4], bytes.fromhex("7e 7e 11 ee"))
        self.assertEqual(struct.unpack("<I", conn.writes[0][4:8])[0], self.mod.AUDIO_PAYLOAD_LEN)
        self.assertEqual(conn.writes[0][-2:], bytes.fromhex("0a 0d"))

    def test_periodic_report_computes_rate_and_resets_only_interval_counters(self):
        stats = self.mod.ClientStats(
            sent_bytes=1_500_000,
            sent_audio_frames=300,
            sent_video_frames=12,
            total_blocked_seconds=0.050,
            session_started_at=1.0,
            interval_started_at=10.0,
            interval_sent_bytes=1_000_000,
            interval_audio_frames=240,
            interval_video_frames=5,
        )

        report = self.mod.format_periodic_stats(stats, now=11.0)

        self.assertIn("tx=8.00 Mb/s", report)
        self.assertIn("audio=240.00 fps", report)
        self.assertIn("video=5.00 fps", report)
        self.assertIn("sent=1.43 MiB", report)
        self.assertIn("blocked=50.0 ms", report)
        self.assertAlmostEqual(stats.peak_interval_mbps, 8.0)
        self.assertEqual(stats.interval_started_at, 11.0)
        self.assertEqual(stats.interval_sent_bytes, 0)
        self.assertEqual(stats.interval_audio_frames, 0)
        self.assertEqual(stats.interval_video_frames, 0)
        self.assertEqual(stats.sent_bytes, 1_500_000)
        self.assertEqual(stats.sent_audio_frames, 300)
        self.assertEqual(stats.sent_video_frames, 12)
        self.assertAlmostEqual(stats.total_blocked_seconds, 0.050)

    def test_session_summary_reports_expected_rates_and_burst_metrics(self):
        stats = self.mod.ClientStats(
            sent_audio_frames=300,
            sent_video_frames=12,
            sent_bytes=12_500_000,
            send_calls=100,
            blocked_send_calls=2,
            total_blocked_seconds=0.050,
            max_send_seconds=0.125,
            burst_count=3,
            burst_overruns=1,
            peak_interval_mbps=120.0,
            session_started_at=20.0,
        )

        summary = self.mod.format_session_summary(stats, now=21.0)

        self.assertIn("duration=1.00s", summary)
        self.assertIn("sent=11.92 MiB", summary)
        self.assertIn("avg=100.00 Mb/s", summary)
        self.assertIn("peak=120.00 Mb/s", summary)
        self.assertIn("audio_frames=300", summary)
        self.assertIn("video_frames=12", summary)
        self.assertIn("blocked=2/100", summary)
        self.assertIn("max_send=125.0 ms", summary)
        self.assertIn("bursts=3", summary)
        self.assertIn("overruns=1", summary)

    def test_final_interval_flush_emits_stats_and_updates_peak_before_summary(self):
        stats = self.mod.ClientStats(
            sent_bytes=1_000_000,
            session_started_at=10.0,
            interval_started_at=10.0,
            interval_sent_bytes=1_000_000,
            interval_audio_frames=240,
        )
        lines: list[str] = []

        emitted = self.mod.flush_final_interval_stats(stats, now=11.0, emit=lines.append)

        self.assertTrue(emitted)
        self.assertEqual(len(lines), 1)
        self.assertIn("tx=8.00 Mb/s", lines[0])
        self.assertAlmostEqual(stats.peak_interval_mbps, 8.0)
        summary = self.mod.format_session_summary(stats, now=11.0)
        self.assertIn("peak=8.00 Mb/s", summary)

    def test_final_interval_flush_skips_empty_interval(self):
        stats = self.mod.ClientStats(session_started_at=10.0, interval_started_at=10.0)
        lines: list[str] = []

        emitted = self.mod.flush_final_interval_stats(stats, now=11.0, emit=lines.append)

        self.assertFalse(emitted)
        self.assertEqual(lines, [])

    def test_delay_helper_keeps_normal_pacing_and_skips_audio_interval_in_sustained_mode(self):
        normal_args = self.mod.build_parser().parse_args(["--audio-fps", "200"])
        sustained_args = self.mod.build_parser().parse_args(
            ["--stress-mode", "sustained", "--audio-fps", "200", "--target-mbps", "100"]
        )
        stats = self.mod.ClientStats(session_started_at=10.0, sent_bytes=12_500_000)
        audio_interval = 1.0 / normal_args.audio_fps

        normal_delay = self.mod.compute_audio_send_delay(
            normal_args,
            stats,
            audio_interval,
            now=10.75,
        )
        sustained_delay = self.mod.compute_audio_send_delay(
            sustained_args,
            stats,
            audio_interval,
            now=10.75,
        )

        self.assertAlmostEqual(normal_delay, 0.005)
        self.assertAlmostEqual(sustained_delay, 0.25)

    def test_transmit_loop_keeps_normal_audio_fps_pacing(self):
        args = self.mod.build_parser().parse_args(
            ["--audio-fps", "200", "--duration", "0.011", "--no-video"]
        )
        timeline = FakeTimeline()
        conn = TimelineSocket(timeline)
        stats = self.mod.ClientStats()

        self.mod.run_transmit_loop(
            conn,
            args,
            stats,
            threading.Event(),
            clock=timeline.clock,
            wait=timeline.wait,
            emit=lambda _line: None,
        )

        self.assertEqual(stats.sent_audio_frames, 3)
        self.assertEqual(len(timeline.waits), 3)
        for actual, expected in zip(timeline.waits, [0.005, 0.005, 0.001]):
            self.assertAlmostEqual(actual, expected)

    def test_sustained_transmit_loop_does_not_wait_for_normal_audio_interval(self):
        args = self.mod.build_parser().parse_args(
            ["--stress-mode", "sustained", "--audio-fps", "1", "--no-video"]
        )
        timeline = FakeTimeline()
        stop_event = threading.Event()
        conn = TimelineSocket(
            timeline,
            stop_event=stop_event,
            stop_after_audio_frames=3,
        )
        stats = self.mod.ClientStats()

        self.mod.run_transmit_loop(
            conn,
            args,
            stats,
            stop_event,
            clock=timeline.clock,
            wait=timeline.wait,
            emit=lambda _line: None,
        )

        self.assertEqual(stats.sent_audio_frames, 3)
        self.assertEqual(timeline.waits, [])

    def test_low_rate_wait_is_cut_short_by_stats_video_and_duration_deadlines(self):
        args = self.mod.build_parser().parse_args(
            [
                "--stress-mode",
                "sustained",
                "--target-mbps",
                "0.001",
                "--video-fps",
                "20",
                "--stats-interval",
                "0.1",
                "--duration",
                "0.25",
            ]
        )
        timeline = FakeTimeline()
        conn = TimelineSocket(timeline)
        stats = self.mod.ClientStats()
        lines: list[str] = []

        self.mod.run_transmit_loop(
            conn,
            args,
            stats,
            threading.Event(),
            clock=timeline.clock,
            wait=timeline.wait,
            emit=lines.append,
        )

        self.assertEqual(stats.sent_audio_frames, 1)
        self.assertEqual(stats.sent_video_frames, 5)
        self.assertEqual(timeline.now, 0.25)
        self.assertTrue(timeline.waits)
        self.assertLessEqual(max(timeline.waits), 0.05 + 1e-12)
        self.assertEqual(len(lines), 2)

    def test_overrunning_burst_services_due_video_and_stats_between_audio_frames(self):
        frame_bytes = self.mod.MEDIA_HEADER_LEN + self.mod.AUDIO_PAYLOAD_LEN + self.mod.MEDIA_TAIL_LEN
        burst_size_mib = (frame_bytes * 3 - 1) / (1024 * 1024)
        args = self.mod.build_parser().parse_args(
            [
                "--stress-mode",
                "burst",
                "--burst-size-mb",
                str(burst_size_mib),
                "--burst-interval",
                "0.1",
                "--video-fps",
                "20",
                "--stats-interval",
                "0.05",
                "--duration",
                "0.18",
            ]
        )
        timeline = FakeTimeline()
        conn = TimelineSocket(timeline, audio_send_seconds=0.06)
        stats = self.mod.ClientStats()
        lines: list[str] = []

        self.mod.run_transmit_loop(
            conn,
            args,
            stats,
            threading.Event(),
            clock=timeline.clock,
            wait=timeline.wait,
            emit=lines.append,
        )

        frame_types = [struct.unpack("<H", frame[2:4])[0] for frame in conn.writes]
        self.assertEqual(
            frame_types,
            [
                self.mod.TYPE_AUDIO,
                self.mod.TYPE_VIDEO,
                self.mod.TYPE_AUDIO,
                self.mod.TYPE_VIDEO,
                self.mod.TYPE_AUDIO,
            ],
        )
        self.assertEqual(len(lines), 3)
        self.assertEqual(stats.burst_count, 1)
        self.assertEqual(stats.burst_bytes, frame_bytes * 3)
        self.assertEqual(stats.burst_overruns, 1)

    def test_duration_prevents_scheduling_another_complete_frame(self):
        args = self.mod.build_parser().parse_args(
            ["--audio-fps", "240", "--duration", "0.05", "--no-video"]
        )
        timeline = FakeTimeline()
        conn = TimelineSocket(timeline, audio_send_seconds=0.06)
        stats = self.mod.ClientStats()

        self.mod.run_transmit_loop(
            conn,
            args,
            stats,
            threading.Event(),
            clock=timeline.clock,
            wait=timeline.wait,
            emit=lambda _line: None,
        )

        self.assertEqual(stats.sent_audio_frames, 1)


class ClientBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.args = self.mod.build_parser().parse_args(
            ["--duration", "0.000001", "--no-video"]
        )

    def _run_with_deferred_receiver(self, transmit_loop):
        events: list[object] = []

        class RecordingSocket:
            def settimeout(self, timeout):
                events.append(("timeout", timeout))

            def sendall(self, _data):
                events.append("legacy-send")

            def close(self):
                events.append("close")

        class DeferredThread:
            def __init__(self, *args, **kwargs):
                pass

            def start(self):
                events.append("receiver-start")

            def join(self, timeout=None):
                events.append(("receiver-join", timeout))

        def recording_transmit_loop(*args, **kwargs):
            events.append("transmit")
            return transmit_loop(*args, **kwargs)

        original_thread = self.mod.threading.Thread
        original_transmit_loop = self.mod.run_transmit_loop
        self.mod.threading.Thread = DeferredThread
        self.mod.run_transmit_loop = recording_transmit_loop
        output = io.StringIO()
        try:
            with contextlib.redirect_stdout(output):
                self.mod.handle_client(RecordingSocket(), ("127.0.0.1", 1234), self.args)
        finally:
            self.mod.threading.Thread = original_thread
            self.mod.run_transmit_loop = original_transmit_loop
        return events, output.getvalue()

    def test_handle_client_sets_finite_timeout_before_starting_workers(self):
        events, _output = self._run_with_deferred_receiver(
            lambda *_args, **_kwargs: None
        )

        self.assertEqual(
            events[:3],
            [
                ("timeout", self.mod.CLIENT_SOCKET_TIMEOUT_SECONDS),
                "receiver-start",
                "transmit",
            ],
        )

    def test_unexpected_session_exception_is_logged_and_does_not_escape(self):
        def fail_transmit(*_args, **_kwargs):
            raise struct.error("generated payload overflow")

        events, output = self._run_with_deferred_receiver(fail_transmit)

        self.assertIn("unexpected client session error", output)
        self.assertIn("generated payload overflow", output)
        self.assertIn("close", events)

    def test_keyboard_interrupt_is_not_caught_at_client_boundary(self):
        def interrupt_transmit(*_args, **_kwargs):
            raise KeyboardInterrupt()

        with self.assertRaises(KeyboardInterrupt):
            self._run_with_deferred_receiver(interrupt_transmit)


class SelfTestCoverageTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_self_test_exercises_burst_mode_without_video(self):
        captured: dict[str, object] = {}
        frame = self.mod.pack_media_frame(
            self.mod.TYPE_AUDIO,
            self.mod.make_audio_payload(
                sequence=0,
                pattern="gradient",
                hot_channel=0,
                sample_rate=self.mod.DEFAULT_SAMPLE_RATE,
            ),
        )
        original_send_burst = self.mod.send_burst

        def fake_send_burst(conn, args, stats, sequence, target_bytes, **kwargs):
            captured["args"] = args
            captured["target_bytes"] = target_bytes
            for _ in range(3):
                conn.sendall(frame)
            return sequence + 3, len(frame) * 3

        self.mod.send_burst = fake_send_burst
        try:
            self.assertEqual(self.mod.self_test(), 0)
        finally:
            self.mod.send_burst = original_send_burst

        args = captured.get("args")
        self.assertIsNotNone(args)
        self.assertEqual(args.stress_mode, "burst")
        self.assertTrue(args.no_video)
        self.assertEqual(captured["target_bytes"], 100000)

    def test_self_test_rejects_wrong_rate_limit_delay(self):
        original_rate_limit_delay = self.mod.rate_limit_delay
        self.mod.rate_limit_delay = lambda *args, **kwargs: 0.0
        try:
            with self.assertRaises(AssertionError):
                self.mod.self_test()
        finally:
            self.mod.rate_limit_delay = original_rate_limit_delay

    def test_self_test_rejects_cached_video_payload_from_different_resource(self):
        with tempfile.TemporaryDirectory() as directory:
            replacement_path = pathlib.Path(directory) / "different-video-resource.bin"
            replacement_path.write_bytes(b"different non-empty video resource")
            original_video_asset_path = self.mod.VIDEO_ASSET_PATH
            self.mod.VIDEO_ASSET_PATH = replacement_path
            try:
                with self.assertRaises(AssertionError):
                    self.mod.self_test()
            finally:
                self.mod.VIDEO_ASSET_PATH = original_video_asset_path


@unittest.skipUnless(hasattr(socket, "socketpair"), "socket.socketpair() is unavailable")
class SocketIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_send_media_frame_writes_complete_audio_frame_to_socket_peer(self):
        payload = self.mod.make_audio_payload(
            sequence=0,
            pattern="gradient",
            hot_channel=0,
            sample_rate=self.mod.DEFAULT_SAMPLE_RATE,
        )
        stats = self.mod.ClientStats()

        tx_sock, rx_sock = socket.socketpair()
        with contextlib.closing(tx_sock), contextlib.closing(rx_sock):
            tx_sock.settimeout(1.0)
            rx_sock.settimeout(1.0)

            frame_bytes = self.mod.send_media_frame(
                tx_sock,
                self.mod.TYPE_AUDIO,
                payload,
                stats,
            )

            received = bytearray()
            while len(received) < frame_bytes:
                chunk = rx_sock.recv(frame_bytes - len(received))
                self.assertTrue(chunk)
                received.extend(chunk)

        self.assertEqual(frame_bytes, len(received))
        self.assertEqual(bytes(received[:8]), bytes.fromhex("7e 7e 11 ee 00 96 00 00"))
        self.assertEqual(bytes(received[-2:]), bytes.fromhex("0a 0d"))


if __name__ == "__main__":
    unittest.main()
