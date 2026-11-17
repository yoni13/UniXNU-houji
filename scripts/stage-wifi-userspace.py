#!/usr/bin/env python3
"""Add built Wi-Fi userspace to a Data seed and append its two trust hashes."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unixnu", type=Path, required=True)
    parser.add_argument("--seed", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--userspace", type=Path, required=True)
    parser.add_argument("--trust-in", type=Path, required=True)
    parser.add_argument("--trust-out", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or args.trust_out.exists():
        parser.error("refuse to overwrite output or trust-out")
    report = json.loads((args.userspace / "report.json").read_text())
    inputs = [
        ("bin/unixnu-wpa_supplicant", "ngpu/wifi-bin/unixnu-wpa_supplicant"),
        ("etc/wifi/wpa_supplicant.conf", "ngpu/etc/wifi/wpa_supplicant.conf"),
        ("xpc/launchd.plist", "ngpu/xpc/launchd.plist"),
        ("xpc/launchd.plist.sig", "ngpu/xpc/launchd.plist.sig"),
    ]
    subprocess.run([sys.executable, ROOT / "scripts/add-data-files.py", "--unixnu", args.unixnu,
                    "--seed", args.seed, "--output", args.output,
                    *[f"{args.userspace / source}:{dest}:0" for source, dest in inputs]],
                   check=True, timeout=900)
    subprocess.run([sys.executable, args.unixnu / "scripts/trust-append-module.py",
                    args.trust_in, args.trust_out, report["service_cdhash"],
                    report["launchd_cache_cdhash"]], check=True)
    delta = args.output.stat().st_size - args.seed.stat().st_size
    print(json.dumps({"seed_bytes": args.seed.stat().st_size, "output_bytes": args.output.stat().st_size,
                      "packed_delta_bytes": delta, "packed_delta_mib": round(delta / 1048576, 3),
                      "boot_limit_note": "measure the final boot image; keep it below 77 MB"}))


if __name__ == "__main__":
    main()
