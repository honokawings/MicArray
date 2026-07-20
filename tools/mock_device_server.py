#!/usr/bin/env python3
"""Standalone TCP mock device server for MicArray upper-computer testing."""

from __future__ import annotations

import argparse
import math
import random
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path


HEADER = 0x7E7E
TAIL = 0x0D0A

TYPE_AUDIO = 0xEE11
TYPE_VIDEO = 0xEE22
TYPE_HEARTBEAT = 0xCC00
TYPE_CONTROL = 0xCC11

AUDIO_CHANNELS = 48
BYTES_PER_CHANNEL = 4
SAMPLES_PER_AUDIO_FRAME = 200
DEFAULT_SAMPLE_RATE = 48_000
BYTES_PER_SAMPLE_POINT = AUDIO_CHANNELS * BYTES_PER_CHANNEL
AUDIO_PAYLOAD_LEN = SAMPLES_PER_AUDIO_FRAME * BYTES_PER_SAMPLE_POINT
MEDIA_HEADER_LEN = 16
MEDIA_TAIL_LEN = 2
VIDEO_TAIL_LEN = 1

BLOCKED_SEND_THRESHOLD_SECONDS = 0.010
BITS_PER_MEGABIT = 1_000_000
CLIENT_SOCKET_TIMEOUT_SECONDS = 0.5

HEARTBEAT_FRAME_LEN = 10
CONTROL_FRAME_LEN = 26

VIDEO_ASSET_PATH = Path(__file__).resolve().parent / "assets" / "mock_video_1920x1080.jpg"


def load_video_payload(path: Path) -> bytes:
    try:
        payload = path.read_bytes()
    except OSError as exc:
        raise RuntimeError(f"Unable to read mock video asset '{path}': {exc}") from exc
    if not payload:
        raise RuntimeError(f"Mock video asset '{path}' is empty")
    return payload


JPEG_PAYLOAD = load_video_payload(VIDEO_ASSET_PATH)


@dataclass
class ClientStats:
    heartbeats: int = 0
    controls: int = 0
    received_bytes: int = 0
    sent_audio_frames: int = 0
    sent_video_frames: int = 0
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

    def record_send(self, frame_type: int, byte_count: int, elapsed: float) -> None:
        self.sent_bytes += byte_count
        self.send_calls += 1
        self.interval_sent_bytes += byte_count
        self.max_send_seconds = max(self.max_send_seconds, elapsed)

        if frame_type == TYPE_AUDIO:
            self.sent_audio_frames += 1
            self.interval_audio_frames += 1
        elif frame_type == TYPE_VIDEO:
            self.sent_video_frames += 1
            self.interval_video_frames += 1

        if elapsed >= BLOCKED_SEND_THRESHOLD_SECONDS:
            self.blocked_send_calls += 1
            self.total_blocked_seconds += elapsed


def send_media_frame(
    conn: socket.socket,
    frame_type: int,
    payload: bytes,
    stats: ClientStats,
    clock=time.monotonic,
) -> int:
    frame = pack_media_frame(frame_type, payload)
    started_at = clock()
    conn.sendall(frame)
    elapsed = clock() - started_at
    stats.record_send(frame_type, len(frame), elapsed)
    return len(frame)


def send_next_audio(
    conn,
    args,
    stats: ClientStats,
    sequence: int,
    clock=time.monotonic,
) -> int:
    payload = make_audio_payload(sequence, args.pattern, args.hot_channel, args.sample_rate)
    return send_media_frame(conn, TYPE_AUDIO, payload, stats, clock=clock)


def send_burst(
    conn,
    args,
    stats: ClientStats,
    sequence: int,
    target_bytes: int,
    clock=time.monotonic,
    should_stop=None,
    deadline: float | None = None,
) -> tuple[int, int]:
    sent_bytes = 0
    next_sequence = sequence

    while sent_bytes < target_bytes:
        if should_stop is not None and should_stop():
            return next_sequence, sent_bytes
        if deadline is not None and clock() >= deadline:
            return next_sequence, sent_bytes
        sent_bytes += send_next_audio(conn, args, stats, next_sequence, clock=clock)
        next_sequence += 1

    stats.burst_count += 1
    stats.burst_bytes += sent_bytes
    return next_sequence, sent_bytes


def rate_limit_delay(
    start_time: float,
    sent_bytes: int,
    target_mbps: float,
    now: float,
) -> float:
    if target_mbps <= 0:
        return 0.0
    expected_elapsed = sent_bytes * 8 / (target_mbps * BITS_PER_MEGABIT)
    return max(0.0, expected_elapsed - max(0.0, now - start_time))


def compute_audio_send_delay(
    args: argparse.Namespace,
    stats: ClientStats,
    audio_interval: float,
    now: float,
    jitter_source=random.uniform,
) -> float:
    if args.stress_mode == "sustained":
        return rate_limit_delay(stats.session_started_at, stats.sent_bytes, args.target_mbps, now)

    if audio_interval <= 0:
        return 0.0

    jitter_seconds = jitter_source(-args.jitter_ms, args.jitter_ms) / 1000.0 if args.jitter_ms > 0 else 0.0
    return max(0.0, audio_interval + jitter_seconds)


def format_periodic_stats(stats: ClientStats, now: float) -> str:
    interval_seconds = max(0.0, now - stats.interval_started_at)
    interval_mbps = stats.interval_sent_bytes * 8 / interval_seconds / BITS_PER_MEGABIT if interval_seconds > 0 else 0.0
    audio_fps = stats.interval_audio_frames / interval_seconds if interval_seconds > 0 else 0.0
    video_fps = stats.interval_video_frames / interval_seconds if interval_seconds > 0 else 0.0
    stats.peak_interval_mbps = max(stats.peak_interval_mbps, interval_mbps)
    sent_mib = stats.sent_bytes / 1024 / 1024
    blocked_ms = stats.total_blocked_seconds * 1000.0

    stats.interval_started_at = now
    stats.interval_sent_bytes = 0
    stats.interval_audio_frames = 0
    stats.interval_video_frames = 0

    return (
        "[stats] interval={:.2f}s tx={:.2f} Mb/s audio={:.2f} fps video={:.2f} fps "
        "sent={:.2f} MB blocked={:.1f} ms"
    ).format(
        interval_seconds,
        interval_mbps,
        audio_fps,
        video_fps,
        sent_mib,
        blocked_ms,
    )


def format_session_summary(stats: ClientStats, now: float) -> str:
    duration = max(0.0, now - stats.session_started_at)
    average_mbps = stats.sent_bytes * 8 / duration / BITS_PER_MEGABIT if duration > 0 else 0.0
    sent_mib = stats.sent_bytes / 1024 / 1024

    return (
        "[summary] duration={:.2f}s sent={:.2f} MB avg={:.2f} Mb/s peak={:.2f} Mb/s "
        "audio_frames={} video_frames={} blocked={}/{} max_send={:.1f} ms "
        "bursts={} burst_bytes={:.2f} MB overruns={}"
    ).format(
        duration,
        sent_mib,
        average_mbps,
        stats.peak_interval_mbps,
        stats.sent_audio_frames,
        stats.sent_video_frames,
        stats.blocked_send_calls,
        stats.send_calls,
        stats.max_send_seconds * 1000.0,
        stats.burst_count,
        stats.burst_bytes / 1024 / 1024,
        stats.burst_overruns,
    )


def monotonic_timestamp() -> tuple[int, int]:
    now = time.time()
    seconds = int(now)
    micros = int((now - seconds) * 1_000_000)
    return seconds, micros


def pack_media_frame(frame_type: int, payload: bytes) -> bytes:
    seconds, micros = monotonic_timestamp()
    frame = (
        struct.pack("<H", HEADER)
        + struct.pack("<H", frame_type)
        + struct.pack("<I", len(payload))
        + struct.pack("<II", seconds, micros)
        + payload
    )
    if frame_type == TYPE_VIDEO:
        return frame + b"\x0a"
    return frame + struct.pack("<H", TAIL)


def amplitude_for_channel(channel: int, pattern: str, hot_channel: int) -> int:
    channel = max(0, min(AUDIO_CHANNELS - 1, channel))
    hot_channel = max(0, min(AUDIO_CHANNELS - 1, hot_channel))

    if pattern == "gradient":
        value = int(18_000_000 * (0.12 + 0.88 * ((channel + 1) / AUDIO_CHANNELS)))
    elif pattern == "pulse":
        value = 18_000_000 if channel == hot_channel else 3_500_000
    elif pattern == "single-hot":
        value = 20_000_000 if channel == hot_channel else 1_200_000
    elif pattern == "checker":
        row = channel // 8
        col = channel % 8
        value = 15_000_000 if (row + col) % 2 == 0 else 3_000_000
    else:
        value = 6_000_000

    return max(0, min(2_000_000_000, value))


def sample_for_channel(channel: int, absolute_sample: int, pattern: str, hot_channel: int, sample_rate: int) -> int:
    channel = max(0, min(AUDIO_CHANNELS - 1, channel))
    sample_rate = max(1, sample_rate)
    amplitude = amplitude_for_channel(channel, pattern, hot_channel)

    base_frequency = 440.0 + (channel % 8) * 12.5
    phase_offset = channel * 0.17
    t = absolute_sample / sample_rate
    carrier = math.sin(2.0 * math.pi * base_frequency * t + phase_offset)
    harmonic = 0.22 * math.sin(2.0 * math.pi * base_frequency * 2.0 * t + phase_offset * 0.5)

    if pattern == "pulse":
        gate = 1.0 if (absolute_sample // max(1, sample_rate // 4)) % 2 == 0 else 0.18
        value = amplitude * gate * (carrier + harmonic)
    elif pattern == "single-hot":
        value = amplitude * (carrier + harmonic)
    else:
        slow_mod = 0.85 + 0.15 * math.sin(2.0 * math.pi * 1.0 * t + channel * 0.05)
        value = amplitude * slow_mod * (carrier + harmonic)

    return int(max(-2_000_000_000, min(2_000_000_000, value)))


def make_audio_payload(sequence: int, pattern: str, hot_channel: int, sample_rate: int) -> bytes:
    payload = bytearray()
    for sample_point in range(SAMPLES_PER_AUDIO_FRAME):
        absolute_sample = sequence * SAMPLES_PER_AUDIO_FRAME + sample_point
        for channel in range(AUDIO_CHANNELS):
            sample = sample_for_channel(channel, absolute_sample, pattern, hot_channel, sample_rate)
            payload.extend(struct.pack("<i", sample))
    return bytes(payload)


def format_hex(data: bytes, max_len: int = 64) -> str:
    clipped = data[:max_len]
    suffix = " ..." if len(data) > max_len else ""
    return clipped.hex(" ") + suffix


def finite_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value):
        raise argparse.ArgumentTypeError("value must be finite")
    return value


def mib_to_target_bytes(size_mib: float) -> int:
    byte_value = size_mib * 1024 * 1024
    if size_mib <= 0 or byte_value < 1:
        raise ValueError("--burst-size-mb must produce at least 1 byte")
    if not math.isfinite(size_mib) or not math.isfinite(byte_value) or byte_value > sys.maxsize:
        raise ValueError("--burst-size-mb is too large to convert to target bytes")
    return math.ceil(byte_value)


def parse_control_frame(frame: bytes) -> tuple[int, ...] | None:
    if len(frame) != CONTROL_FRAME_LEN:
        return None
    header = struct.unpack("<H", frame[:2])[0]
    frame_type = struct.unpack("<H", frame[2:4])[0]
    payload_len = struct.unpack("<I", frame[4:8])[0]
    tail = struct.unpack("<H", frame[-2:])[0]
    if header != HEADER or frame_type != TYPE_CONTROL or payload_len != 0x16 or tail != TAIL:
        return None
    return struct.unpack("<8h", frame[8:24])


def receiver_loop(conn: socket.socket, stop_event: threading.Event, stats: ClientStats, verbose: bool) -> None:
    buffer = bytearray()

    while not stop_event.is_set():
        try:
            chunk = conn.recv(4096)
        except socket.timeout:
            continue
        except OSError as exc:
            if not stop_event.is_set():
                print(f"[rx] socket error: {exc}", flush=True)
            break

        if not chunk:
            print("[rx] client disconnected", flush=True)
            stop_event.set()
            break

        stats.received_bytes += len(chunk)
        buffer.extend(chunk)
        parse_uplink_buffer(buffer, stats, verbose)


def parse_uplink_buffer(buffer: bytearray, stats: ClientStats, verbose: bool) -> None:
    while True:
        header_index = buffer.find(b"\x7e\x7e")
        if header_index < 0:
            if buffer and verbose:
                print(f"[rx] discard {len(buffer)} bytes without header", flush=True)
            buffer.clear()
            return

        if header_index > 0:
            if verbose:
                print(f"[rx] discard {header_index} bytes before header", flush=True)
            del buffer[:header_index]

        if len(buffer) < 8:
            return

        frame_type = struct.unpack("<H", buffer[2:4])[0]
        payload_len = struct.unpack("<I", buffer[4:8])[0]
        if frame_type == TYPE_HEARTBEAT:
            if len(buffer) < HEARTBEAT_FRAME_LEN:
                return
            frame = bytes(buffer[:HEARTBEAT_FRAME_LEN])
            del buffer[:HEARTBEAT_FRAME_LEN]
            tail = struct.unpack("<H", frame[-2:])[0]
            if payload_len == 0 and tail == TAIL:
                stats.heartbeats += 1
                print(f"[rx] heartbeat #{stats.heartbeats} at {time.strftime('%H:%M:%S')}", flush=True)
            else:
                print(f"[rx] invalid heartbeat: {format_hex(frame)}", flush=True)
            continue

        if frame_type == TYPE_CONTROL:
            if len(buffer) < CONTROL_FRAME_LEN:
                return
            frame = bytes(buffer[:CONTROL_FRAME_LEN])
            del buffer[:CONTROL_FRAME_LEN]
            values = parse_control_frame(frame)
            if values is None:
                print(f"[rx] invalid control: {format_hex(frame)}", flush=True)
            else:
                stats.controls += 1
                print(
                    f"[rx] control #{stats.controls} values={list(values)} raw={format_hex(frame)}",
                    flush=True,
                )
            continue

        print(f"[rx] unsupported uplink type=0x{frame_type:04x}; resync", flush=True)
        del buffer[:1]


def sleep_with_jitter(base_seconds: float, jitter_ms: float) -> None:
    if base_seconds <= 0:
        return
    jitter_seconds = random.uniform(-jitter_ms, jitter_ms) / 1000.0 if jitter_ms > 0 else 0.0
    time.sleep(max(0.0, base_seconds + jitter_seconds))


def maybe_emit_periodic_stats(stats: ClientStats, stats_interval: float, now: float) -> None:
    if now - stats.interval_started_at < stats_interval:
        return
    print(format_periodic_stats(stats, now), flush=True)


def flush_final_interval_stats(stats: ClientStats, now: float, emit=print) -> bool:
    if stats.interval_sent_bytes == 0 and stats.interval_audio_frames == 0 and stats.interval_video_frames == 0:
        return False
    emit(format_periodic_stats(stats, now))
    return True


def maybe_print_verbose_progress(stats: ClientStats, args: argparse.Namespace) -> None:
    if not args.verbose:
        return
    if stats.sent_audio_frames % max(1, int(args.audio_fps)) != 0:
        return
    print(
        "[tx] audio_frames={} video_frames={} rx_bytes={}".format(
            stats.sent_audio_frames,
            stats.sent_video_frames,
            stats.received_bytes,
        ),
        flush=True,
    )


def run_transmit_loop(
    conn: socket.socket,
    args: argparse.Namespace,
    stats: ClientStats,
    stop_event: threading.Event,
    *,
    clock=time.monotonic,
    wait=None,
    emit=print,
) -> None:
    if wait is None:
        wait = stop_event.wait

    session_started_at = clock()
    stats.session_started_at = session_started_at
    stats.interval_started_at = session_started_at
    session_deadline = session_started_at + args.duration if args.duration > 0 else None
    audio_interval = 1.0 / args.audio_fps if args.audio_fps > 0 else 0.0
    video_interval = 1.0 / args.video_fps if args.video_fps > 0 else 0.0
    next_audio_deadline = session_started_at
    next_video_deadline = session_started_at
    next_burst_deadline = session_started_at
    burst_target_bytes = mib_to_target_bytes(args.burst_size_mb)
    current_burst_bytes = 0
    sequence = 0

    while not stop_event.is_set():
        now = clock()
        stats_deadline = stats.interval_started_at + args.stats_interval
        if now >= stats_deadline:
            emit(format_periodic_stats(stats, now))
            continue
        if session_deadline is not None and now >= session_deadline:
            break

        video_enabled = not args.no_video and args.video_fps > 0
        if video_enabled and now >= next_video_deadline and sequence > 0:
            send_media_frame(conn, TYPE_VIDEO, JPEG_PAYLOAD, stats, clock=clock)
            next_video_deadline = clock() + video_interval
            continue

        if args.stress_mode == "burst":
            if now >= next_burst_deadline:
                current_burst_bytes += send_next_audio(
                    conn,
                    args,
                    stats,
                    sequence,
                    clock=clock,
                )
                sequence += 1
                maybe_print_verbose_progress(stats, args)
                if current_burst_bytes >= burst_target_bytes:
                    stats.burst_count += 1
                    # Count bytes only after the entire frame-granular burst completes.
                    stats.burst_bytes += current_burst_bytes
                    current_burst_bytes = 0
                    scheduled_deadline = next_burst_deadline + args.burst_interval
                    if clock() > scheduled_deadline:
                        stats.burst_overruns += 1
                    next_burst_deadline = scheduled_deadline
                continue
            audio_deadline = next_burst_deadline
        elif args.stress_mode == "sustained":
            delay = rate_limit_delay(
                session_started_at,
                stats.sent_bytes,
                args.target_mbps,
                now,
            )
            audio_deadline = now + delay
            if delay <= 0:
                send_next_audio(conn, args, stats, sequence, clock=clock)
                sequence += 1
                maybe_print_verbose_progress(stats, args)
                continue
        else:
            audio_deadline = next_audio_deadline
            if now >= audio_deadline:
                send_next_audio(conn, args, stats, sequence, clock=clock)
                sequence += 1
                maybe_print_verbose_progress(stats, args)
                delay = compute_audio_send_delay(
                    args,
                    stats,
                    audio_interval,
                    clock(),
                )
                next_audio_deadline = clock() + delay
                continue

        deadlines = [audio_deadline, stats_deadline]
        if video_enabled:
            deadlines.append(next_video_deadline)
        if session_deadline is not None:
            deadlines.append(session_deadline)
        timeout = max(0.0, min(deadlines) - now)
        if timeout > 0 and wait(timeout):
            break


def handle_client(conn: socket.socket, addr: tuple[str, int], args: argparse.Namespace) -> None:
    print(f"[server] client connected from {addr[0]}:{addr[1]}", flush=True)
    stop_event = threading.Event()
    stats = ClientStats()
    session_started_at = time.monotonic()
    stats.session_started_at = session_started_at
    stats.interval_started_at = session_started_at
    rx_thread = None
    rx_started = False

    try:
        conn.settimeout(CLIENT_SOCKET_TIMEOUT_SECONDS)
        rx_thread = threading.Thread(
            target=receiver_loop,
            args=(conn, stop_event, stats, args.verbose),
            daemon=True,
        )
        rx_thread.start()
        rx_started = True
        run_transmit_loop(
            conn,
            args,
            stats,
            stop_event,
            emit=lambda line: print(line, flush=True),
        )
    except (BrokenPipeError, ConnectionResetError, OSError) as exc:
        print(f"[tx] client connection closed: {exc}", flush=True)
    except Exception as exc:
        print(
            f"[tx] unexpected client session error: {type(exc).__name__}: {exc}",
            flush=True,
        )
    finally:
        summary_now = time.monotonic()
        flush_final_interval_stats(stats, summary_now, emit=lambda line: print(line, flush=True))
        stop_event.set()
        try:
            conn.close()
        except OSError:
            pass
        if rx_started:
            rx_thread.join(timeout=1.0)
        print(format_session_summary(stats, summary_now), flush=True)
        print(
            "[server] client session ended: audio_frames={} video_frames={} heartbeats={} controls={}".format(
                stats.sent_audio_frames,
                stats.sent_video_frames,
                stats.heartbeats,
                stats.controls,
            ),
            flush=True,
        )


def run_server(args: argparse.Namespace) -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.host, args.port))
        server.listen(1)
        stress_settings = [f"stress={args.stress_mode}", f"duration={args.duration}", f"stats_interval={args.stats_interval}"]
        if args.stress_mode == "sustained":
            stress_settings.append(f"target_mbps={args.target_mbps}")
        elif args.stress_mode == "burst":
            stress_settings.append(f"burst_size_mb={args.burst_size_mb}")
            stress_settings.append(f"burst_interval={args.burst_interval}")
        else:
            stress_settings.append(f"jitter_ms={args.jitter_ms}")
        print(
            "[server] listening on {}:{} audio_fps={} video_fps={} pattern={} {}".format(
                args.host,
                args.port,
                args.audio_fps,
                0 if args.no_video else args.video_fps,
                args.pattern,
                " ".join(stress_settings),
            ),
            flush=True,
        )

        while True:
            try:
                conn, addr = server.accept()
            except KeyboardInterrupt:
                print("\n[server] stopped", flush=True)
                return 0

            with conn:
                handle_client(conn, addr, args)
                print("[server] ready for next client", flush=True)


def self_test() -> int:
    payload = make_audio_payload(sequence=0, pattern="gradient", hot_channel=0, sample_rate=DEFAULT_SAMPLE_RATE)
    assert len(payload) == AUDIO_PAYLOAD_LEN, len(payload)
    first_sample = struct.unpack("<i", payload[:4])[0]
    assert first_sample == sample_for_channel(0, 0, "gradient", 0, DEFAULT_SAMPLE_RATE), payload[:4].hex()
    output_channel_samples = [
        struct.unpack("<i", payload[(i * BYTES_PER_SAMPLE_POINT):(i * BYTES_PER_SAMPLE_POINT + 4)])[0]
        for i in range(SAMPLES_PER_AUDIO_FRAME)
    ]
    assert max(output_channel_samples) > 0, max(output_channel_samples)
    assert min(output_channel_samples) < 0, min(output_channel_samples)
    assert JPEG_PAYLOAD, "mock video payload is empty"
    assert JPEG_PAYLOAD == VIDEO_ASSET_PATH.read_bytes(), str(VIDEO_ASSET_PATH)

    frame = pack_media_frame(TYPE_AUDIO, payload)
    assert len(frame) == MEDIA_HEADER_LEN + AUDIO_PAYLOAD_LEN + MEDIA_TAIL_LEN, len(frame)
    assert frame[:8] == bytes.fromhex("7e 7e 11 ee 00 96 00 00"), frame[:8].hex(" ")
    assert frame[-2:] == bytes.fromhex("0a 0d"), frame[-2:].hex(" ")

    header = struct.unpack("<H", frame[:2])[0]
    frame_type = struct.unpack("<H", frame[2:4])[0]
    payload_len = struct.unpack("<I", frame[4:8])[0]
    _seconds, _micros = struct.unpack("<II", frame[8:MEDIA_HEADER_LEN])
    tail = struct.unpack("<H", frame[-2:])[0]
    assert header == HEADER, hex(header)
    assert frame_type == TYPE_AUDIO, hex(frame_type)
    assert payload_len == AUDIO_PAYLOAD_LEN, payload_len
    assert tail == TAIL, hex(tail)

    video_frame = pack_media_frame(TYPE_VIDEO, JPEG_PAYLOAD)
    assert video_frame[MEDIA_HEADER_LEN:-VIDEO_TAIL_LEN] == JPEG_PAYLOAD
    assert video_frame[-1:] == b"\x0a", video_frame[-4:].hex(" ")
    assert video_frame[-2:] != bytes.fromhex("0a 0d"), video_frame[-4:].hex(" ")

    control = (
        struct.pack("<H", HEADER)
        + struct.pack("<H", TYPE_CONTROL)
        + struct.pack("<I", 0x16)
        + struct.pack("<8h", -1, 0, 1, 100, -100, 32767, -32768, 42)
        + struct.pack("<H", TAIL)
    )
    values = parse_control_frame(control)
    assert values == (-1, 0, 1, 100, -100, 32767, -32768, 42), values

    class FakeSocket:
        def __init__(self) -> None:
            self.writes: list[bytes] = []

        def sendall(self, data: bytes) -> None:
            self.writes.append(data)

    burst_args = build_parser().parse_args(["--stress-mode", "burst", "--no-video"])
    validate_args(burst_args)
    burst_stats = ClientStats()
    fake_socket = FakeSocket()
    frame_bytes = MEDIA_HEADER_LEN + AUDIO_PAYLOAD_LEN + MEDIA_TAIL_LEN
    burst_sequence, actual_burst_bytes = send_burst(
        fake_socket,
        burst_args,
        burst_stats,
        sequence=0,
        target_bytes=100000,
        clock=lambda: 0.0,
    )
    assert actual_burst_bytes >= 100000, actual_burst_bytes
    assert actual_burst_bytes % frame_bytes == 0, actual_burst_bytes
    assert burst_sequence == len(fake_socket.writes), (burst_sequence, len(fake_socket.writes))
    for write in fake_socket.writes:
        assert write[:4] == bytes.fromhex("7e 7e 11 ee"), write[:4].hex(" ")
        assert write[-2:] == bytes.fromhex("0a 0d"), write[-2:].hex(" ")

    assert math.isclose(rate_limit_delay(10.0, 12_500_000, 100.0, 10.75), 0.25, rel_tol=1e-9, abs_tol=1e-9)

    print("self-test passed")
    return 0


def validate_args(args: argparse.Namespace) -> None:
    if args.audio_fps <= 0:
        raise ValueError("--audio-fps must be > 0")
    if args.sample_rate <= 0:
        raise ValueError("--sample-rate must be > 0")
    if args.video_fps < 0:
        raise ValueError("--video-fps must be >= 0")
    if args.jitter_ms < 0:
        raise ValueError("--jitter-ms must be >= 0")
    if args.duration < 0:
        raise ValueError("--duration must be >= 0")
    if args.target_mbps < 0:
        raise ValueError("--target-mbps must be >= 0")
    if args.stats_interval <= 0:
        raise ValueError("--stats-interval must be > 0")
    mib_to_target_bytes(args.burst_size_mb)
    if args.burst_interval <= 0:
        raise ValueError("--burst-interval must be > 0")
    if args.target_mbps != 0 and args.stress_mode != "sustained":
        raise ValueError("--target-mbps requires --stress-mode sustained")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Standalone mock TCP device server for MicArray testing.",
    )
    parser.add_argument("--host", default="0.0.0.0", help="listen address, default: 0.0.0.0")
    parser.add_argument("--port", type=int, default=9968, help="listen port, default: 9968")
    parser.add_argument("--audio-fps", type=finite_float, default=240.0, help="audio frame send rate, default: 240 for 200 samples/frame at 48 kHz")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE, help="simulated audio sample rate, default: 48000")
    parser.add_argument("--video-fps", type=finite_float, default=1.0, help="JPEG frame send rate, default: 1")
    parser.add_argument(
        "--stress-mode",
        choices=("normal", "sustained", "burst"),
        default="normal",
        help="pressure test mode",
    )
    parser.add_argument("--duration", type=finite_float, default=0.0, help="test duration in seconds")
    parser.add_argument("--target-mbps", type=finite_float, default=0.0, help="target throughput in Mbps")
    parser.add_argument("--stats-interval", type=finite_float, default=1.0, help="stats interval in seconds")
    parser.add_argument("--burst-size-mb", type=finite_float, default=32.0, help="burst size in MB")
    parser.add_argument("--burst-interval", type=finite_float, default=1.0, help="burst interval in seconds")
    parser.add_argument("--no-video", action="store_true", help="disable video frame sending")
    parser.add_argument(
        "--pattern",
        choices=("gradient", "pulse", "single-hot", "checker"),
        default="gradient",
        help="48-channel amplitude pattern",
    )
    parser.add_argument("--hot-channel", type=int, default=0, help="hot channel for pulse/single-hot patterns")
    parser.add_argument("--jitter-ms", type=finite_float, default=0.0, help="random send interval jitter in milliseconds")
    parser.add_argument("--verbose", action="store_true", help="print periodic tx/rx diagnostics")
    parser.add_argument("--self-test", action="store_true", help="run frame construction checks and exit")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    args.hot_channel = max(0, min(AUDIO_CHANNELS - 1, args.hot_channel))

    try:
        validate_args(args)
        return run_server(args)
    except ValueError as exc:
        parser.error(str(exc))
    except KeyboardInterrupt:
        print("\n[server] stopped", flush=True)
        return 0


if __name__ == "__main__":
    sys.exit(main())
