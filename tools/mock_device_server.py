#!/usr/bin/env python3
"""Standalone TCP mock device server for MicArray upper-computer testing."""

from __future__ import annotations

import argparse
import base64
import math
import random
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass


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

HEARTBEAT_FRAME_LEN = 10
CONTROL_FRAME_LEN = 26

# A tiny 2x2 JPEG generated from a PPM test pattern with cjpeg. The
# upper-computer only needs a decodable payload for smoke testing the video path;
# the audio and calibration paths are the focus.
JPEG_PAYLOAD = base64.b64decode(
    b"/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5"
    b"PTgyPC4zNDL/2wBDAQgJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAAC"
    b"AAIDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1Fh"
    b"ByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJ"
    b"ipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEB"
    b"AQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl"
    b"8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3"
    b"uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD1jwfpenzeCdBllsbV5H063ZnaFSWJjXJJxya"
    b"KKKrFUKXt5+4vil0Xd+Rh7Ck9XBX9F/kf/9k="
)


@dataclass
class ClientStats:
    heartbeats: int = 0
    controls: int = 0
    received_bytes: int = 0
    sent_audio_frames: int = 0
    sent_video_frames: int = 0


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
    conn.settimeout(0.5)

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


def handle_client(conn: socket.socket, addr: tuple[str, int], args: argparse.Namespace) -> None:
    print(f"[server] client connected from {addr[0]}:{addr[1]}", flush=True)
    stop_event = threading.Event()
    stats = ClientStats()
    rx_thread = threading.Thread(
        target=receiver_loop,
        args=(conn, stop_event, stats, args.verbose),
        daemon=True,
    )
    rx_thread.start()

    audio_interval = 1.0 / args.audio_fps if args.audio_fps > 0 else 0.0
    video_interval = 1.0 / args.video_fps if args.video_fps > 0 else 0.0
    last_video_send = 0.0
    sequence = 0

    try:
        while not stop_event.is_set():
            audio_payload = make_audio_payload(sequence, args.pattern, args.hot_channel, args.sample_rate)
            conn.sendall(pack_media_frame(TYPE_AUDIO, audio_payload))
            stats.sent_audio_frames += 1

            now = time.monotonic()
            if not args.no_video and args.video_fps > 0 and now - last_video_send >= video_interval:
                conn.sendall(pack_media_frame(TYPE_VIDEO, JPEG_PAYLOAD))
                stats.sent_video_frames += 1
                last_video_send = now

            if args.verbose and stats.sent_audio_frames % max(1, int(args.audio_fps)) == 0:
                print(
                    "[tx] audio_frames={} video_frames={} rx_bytes={}".format(
                        stats.sent_audio_frames,
                        stats.sent_video_frames,
                        stats.received_bytes,
                    ),
                    flush=True,
                )

            sequence += 1
            sleep_with_jitter(audio_interval, args.jitter_ms)
    except (BrokenPipeError, ConnectionResetError, OSError) as exc:
        print(f"[tx] client connection closed: {exc}", flush=True)
    finally:
        stop_event.set()
        try:
            conn.close()
        except OSError:
            pass
        rx_thread.join(timeout=1.0)
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
        print(
            "[server] listening on {}:{} audio_fps={} video_fps={} pattern={}".format(
                args.host,
                args.port,
                args.audio_fps,
                0 if args.no_video else args.video_fps,
                args.pattern,
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
    assert JPEG_PAYLOAD.startswith(b"\xff\xd8"), JPEG_PAYLOAD[:4].hex()
    assert JPEG_PAYLOAD.endswith(b"\xff\xd9"), JPEG_PAYLOAD[-4:].hex()

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

    print("self-test passed")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Standalone mock TCP device server for MicArray testing.",
    )
    parser.add_argument("--host", default="0.0.0.0", help="listen address, default: 0.0.0.0")
    parser.add_argument("--port", type=int, default=9968, help="listen port, default: 9968")
    parser.add_argument("--audio-fps", type=float, default=240.0, help="audio frame send rate, default: 240 for 200 samples/frame at 48 kHz")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE, help="simulated audio sample rate, default: 48000")
    parser.add_argument("--video-fps", type=float, default=1.0, help="JPEG frame send rate, default: 1")
    parser.add_argument("--no-video", action="store_true", help="disable video frame sending")
    parser.add_argument(
        "--pattern",
        choices=("gradient", "pulse", "single-hot", "checker"),
        default="gradient",
        help="48-channel amplitude pattern",
    )
    parser.add_argument("--hot-channel", type=int, default=0, help="hot channel for pulse/single-hot patterns")
    parser.add_argument("--jitter-ms", type=float, default=0.0, help="random send interval jitter in milliseconds")
    parser.add_argument("--verbose", action="store_true", help="print periodic tx/rx diagnostics")
    parser.add_argument("--self-test", action="store_true", help="run frame construction checks and exit")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    args.hot_channel = max(0, min(AUDIO_CHANNELS - 1, args.hot_channel))
    if args.audio_fps <= 0:
        parser.error("--audio-fps must be > 0")
    if args.sample_rate <= 0:
        parser.error("--sample-rate must be > 0")
    if args.video_fps < 0:
        parser.error("--video-fps must be >= 0")
    if args.jitter_ms < 0:
        parser.error("--jitter-ms must be >= 0")

    try:
        return run_server(args)
    except KeyboardInterrupt:
        print("\n[server] stopped", flush=True)
        return 0


if __name__ == "__main__":
    sys.exit(main())
