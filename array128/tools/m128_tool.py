"""M128 v1 independent UDP probe / simulator / evidence inspection tool.

No commands are ever transmitted to FPGA. The simulator only sends M128 audio
to the explicitly selected destination (loopback by default).
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import socket
import struct
import time

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct(">4sBBBBIHHQII")
REFERENCE_SHA = "fb653a4b25043495c2f54b36fbe9df7a9d9d6df06cbb95ce1d888bec444dc75e"
MASK64 = (1 << 64) - 1


def decode(data: bytes) -> dict:
    if len(data) != 1208:
        raise ValueError(f"length={len(data)}, expected 1208")
    h = HEADER.unpack_from(data)
    if h[:8] != (b"M128", 1, 32, 1, 128, 48000, 3, 392):
        raise ValueError("invalid fixed header")
    sequence, epoch, flags = h[8:]
    if flags & ~0x3F or (flags & 0x20 and not flags & 0x10):
        raise ValueError("invalid flags")
    frames = [(struct.unpack_from(">Q", data, p)[0], data[p + 8:p + 392])
              for p in (32, 424, 816)]
    return {"sequence": sequence, "epoch": epoch, "flags": flags, "frames": frames}


def reference() -> bytes:
    data = (ROOT / "resources/selftest_pcm.bin").read_bytes()
    if hashlib.sha256(data).hexdigest() != REFERENCE_SHA:
        raise ValueError("selftest reference SHA-256 mismatch")
    return data


def make_packet(sequence: int, epoch: int = 1, flags: int = 0x10,
                frame_id: int | None = None) -> bytes:
    ref = reference()
    first = (96 + sequence * 3) & MASK64 if frame_id is None else frame_id
    header = HEADER.pack(b"M128", 1, 32, 1, 128, 48000, 3, 392,
                         sequence & MASK64, epoch, flags)
    return header + b"".join(struct.pack(">Q", (first + r) & MASK64)
                             + ref[((first + r) % 16) * 384:((first + r) % 16 + 1) * 384]
                             for r in range(3))


def probe(args: argparse.Namespace) -> None:
    ref = reference()
    result = {"packets": 0, "invalid": 0, "rejected_source": 0,
              "selftest_frames": 0, "mismatching_frames": 0,
              "error_flag_packets": 0, "continuity_errors": 0,
              "sessions": [], "flags_or": 0}
    previous = None
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
        sock.bind((args.bind, args.port))
        sock.settimeout(0.2)
        result["socket_buffer_bytes"] = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        started = time.monotonic()
        while time.monotonic() - started < args.seconds:
            try:
                data, source = sock.recvfrom(65535)
            except socket.timeout:
                continue
            if args.source and source != (args.source, args.source_port):
                result["rejected_source"] += 1
                continue
            try:
                packet = decode(data)
            except ValueError as error:
                result["invalid"] += 1
                result["last_error"] = str(error)
                continue
            result["packets"] += 1
            result["flags_or"] |= packet["flags"]
            key = [source[0], source[1], packet["epoch"]]
            if key not in result["sessions"]:
                result["sessions"].append(key)
            if packet["flags"] & 0x2F:
                result["error_flag_packets"] += 1
            for fid, pcm in packet["frames"]:
                if previous and previous[0] == key and fid != (previous[1] + 1) & MASK64:
                    result["continuity_errors"] += 1
                previous = (key, fid)
                if packet["flags"] & 0x10:
                    result["selftest_frames"] += 1
                    if pcm != ref[(fid % 16) * 384:(fid % 16 + 1) * 384]:
                        result["mismatching_frames"] += 1
            result["last_sequence"] = str(packet["sequence"])
            result["last_frame_ids"] = [str(f[0]) for f in packet["frames"]]
        result["elapsed_seconds"] = time.monotonic() - started
    result["packets_per_second"] = result["packets"] / result["elapsed_seconds"]
    result["status"] = "NO_DATA" if not result["packets"] else "RECEIVED"
    if result["selftest_frames"]:
        result["status"] = ("PASS" if result["selftest_frames"] >= 48000
                            and not any(result[k] for k in ("invalid", "mismatching_frames",
                                                            "error_flag_packets", "continuity_errors"))
                            else "FAIL_OR_INCOMPLETE")
    payload = json.dumps(result, ensure_ascii=False, indent=2)
    print(payload)
    if args.output:
        # A new evidence filename must be used for every run.
        with open(args.output, "x", encoding="utf-8") as output:
            output.write(payload + "\n")


def simulate(args: argparse.Namespace) -> None:
    # Cache a 16-packet cycle so normal full-rate operation does not hash/read
    # the reference 16,000 times per second.
    templates = [make_packet(i) for i in range(16)]
    sequence = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1" if args.destination == "127.0.0.1" else "0.0.0.0", args.source_port))
        start = time.perf_counter()
        while time.perf_counter() - start < args.seconds:
            target = min(int((time.perf_counter() - start) * args.rate) + 1, sequence + 256)
            while sequence < target:
                data = bytearray(templates[sequence % 16])
                struct.pack_into(">Q", data, 16, sequence)
                struct.pack_into(">I", data, 28, args.flags)
                for r, offset in enumerate((32, 424, 816)):
                    struct.pack_into(">Q", data, offset, 96 + 3 * sequence + r)
                if not args.drop_every or (sequence + 1) % args.drop_every:
                    sock.sendto(data, (args.destination, args.port))
                sequence += 1
            time.sleep(0.001)
    print(json.dumps({"sent_sequence_count": sequence, "seconds": time.perf_counter() - start}))


def inspect_capture(args: argparse.Namespace) -> None:
    ref = reference()
    result = {"packets": 0, "selftest_frames": 0, "mismatching_frames": 0,
              "error_flag_packets": 0, "continuity_errors": 0, "truncated_bytes": 0}
    previous = None
    with open(args.path, "rb") as source:
        if source.read(8) != b"M128CAP1":
            raise ValueError("not a M128CAP1 capture")
        while record := source.read(1224):
            if len(record) != 1224:
                result["truncated_bytes"] = len(record)
                break
            arrival, run = struct.unpack_from(">QQ", record)
            packet = decode(record[16:])
            result["packets"] += 1
            if packet["flags"] & 0x2F:
                result["error_flag_packets"] += 1
            for fid, pcm in packet["frames"]:
                key = (run, packet["epoch"])
                if previous and key == previous[0] and fid != (previous[1] + 1) & MASK64:
                    result["continuity_errors"] += 1
                previous = (key, fid)
                if packet["flags"] & 0x10:
                    result["selftest_frames"] += 1
                    if pcm != ref[(fid % 16) * 384:(fid % 16 + 1) * 384]:
                        result["mismatching_frames"] += 1
            result["last_arrival_ns"] = str(arrival)
    # Capture-file integrity alone cannot attest to unrecorded host queue drops.
    result["scope"] = "recorded packets only; also inspect manifest final_diagnostics and queue drops"
    print(json.dumps(result, ensure_ascii=False, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    receive = sub.add_parser("probe")
    receive.add_argument("--bind", default="0.0.0.0")
    receive.add_argument("--port", type=int, default=5000)
    receive.add_argument("--source", default="192.168.10.2")
    receive.add_argument("--source-port", type=int, default=5000)
    receive.add_argument("--seconds", type=float, default=5)
    receive.add_argument("--output")
    sender = sub.add_parser("simulate")
    sender.add_argument("--destination", default="127.0.0.1")
    sender.add_argument("--port", type=int, default=5000)
    sender.add_argument("--source-port", type=int, default=5001)
    sender.add_argument("--seconds", type=float, default=10)
    sender.add_argument("--rate", type=int, default=16000)
    sender.add_argument("--flags", type=lambda value: int(value, 0), default=0x10)
    sender.add_argument("--drop-every", type=int, default=0)
    inspector = sub.add_parser("inspect")
    inspector.add_argument("path", type=Path)
    args = parser.parse_args()
    if args.command == "probe":
        probe(args)
    elif args.command == "simulate":
        simulate(args)
    else:
        inspect_capture(args)


if __name__ == "__main__":
    main()
