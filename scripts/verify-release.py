"""Verify a downloaded paired release ZIP against the source-branch manifest."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("asset", type=Path, help="Downloaded release ZIP")
    args = parser.parse_args()
    manifest = Path(__file__).resolve().parents[1] / "doc/validation/release_manifest.json"
    entries = json.loads(manifest.read_text(encoding="utf-8"))["assets"]
    entry = next((x for x in entries if x["name"] == args.asset.name), None)
    if entry is None:
        raise SystemExit("Unknown asset name; choose the paired release ZIP.")
    digest = hashlib.sha256()
    with args.asset.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    if args.asset.stat().st_size != entry["bytes"] or digest.hexdigest() != entry["sha256"]:
        raise SystemExit("FAIL: size or SHA256 differs from the paired release manifest.")
    print("PASS:", args.asset.name, digest.hexdigest())


if __name__ == "__main__":
    main()
