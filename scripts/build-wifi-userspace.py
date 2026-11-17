#!/usr/bin/env python3
"""Build/sign the pinned iOS supplicant and make a signed iOS 27 launchd cache.

Run this on macOS. Outputs are local installation material: do not commit them.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PIN = "5f7f5a8a9f02f7ab45ed10a2f226e348c519a838"
LABEL = "org.unixnu.wifi.supplicant"


def run(*command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def cdhash(path):
    detail = subprocess.run(["codesign", "--display", "--verbose=4", str(path)],
                            check=True, capture_output=True, text=True).stderr
    match = re.search(r"^CDHash=([0-9a-f]{40})$", detail, re.M)
    if not match:
        raise SystemExit(f"no SHA-1 CDHash reported for {path}")
    return match.group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    default_core = ROOT.parent / "UniXNU"
    parser.add_argument("--component", type=Path,
                        default=default_core / "drivers/Ath12kWifi")
    parser.add_argument("--launchd-cache", type=Path, required=True,
                        help="stock 24B156 System/Library/xpc/launchd.plist")
    args = parser.parse_args()
    out, component = args.output.resolve(), args.component.resolve()
    revision = subprocess.check_output(["git", "-C", component, "rev-parse", "HEAD"], text=True).strip()
    if revision != PIN:
        raise SystemExit(f"Ath12kWifi must be {PIN}, found {revision}")
    original = plistlib.loads(args.launchd_cache.read_bytes())
    if original.get("VersionNumber") != 7 or not any(
            job.get("Label") == "com.apple.wifid" for job in original.get("LaunchDaemons", {}).values()):
        raise SystemExit("input is not the expected iOS 27 launchd cache")
    shutil.rmtree(out, ignore_errors=True)
    for name in ("bin", "etc/wifi", "daemons", "xpc"):
        (out / name).mkdir(parents=True, exist_ok=True)
    run(sys.executable, component / "tools/build-supplicant.py", "--target", "ios")
    binary = out / "bin/unixnu-wpa_supplicant"
    shutil.copy2(component / "out/wpa-ios/bin/wpa_supplicant", binary)
    binary.chmod(0o755)
    entitlements = out / "entitlements.plist"
    entitlements.write_bytes(plistlib.dumps({"com.apple.private.security.no-container": True}))
    run("codesign", "--force", "--sign", "-", "--pagesize", "4096", "--identifier", LABEL,
        "--entitlements", entitlements, binary)
    run("codesign", "--verify", "--strict", binary)
    service_hash = cdhash(binary)
    (out / "etc/wifi/wpa_supplicant.conf").write_text(
        "# Networks and credentials arrive from iOS; never put keys here.\nap_scan=1\n")
    job = {"Label": LABEL, "ProgramArguments": [
        "/usr/libexec/unixnu-wpa_supplicant", "-Dsheng", "-ien0",
        "-c/private/var/ngpu/etc/wifi/wpa_supplicant.conf", "-q", "-s"],
        "RunAtLoad": True, "KeepAlive": True, "ThrottleInterval": 3,
        "UserName": "root", "Umask": 0o77, "ProcessType": "Interactive"}
    (out / f"daemons/{LABEL}.plist").write_bytes(plistlib.dumps(job))
    builder = component.parents[1] / "scripts/build-boot-runtime.py"
    spec = importlib.util.spec_from_file_location("boot_runtime", builder)
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)
    cache = out / "xpc/launchd.plist"
    cache_report = runtime.merge_launchd_cache(args.launchd_cache, out / "daemons", cache)
    cache_report.update(runtime.sign_launchd_cache(cache))
    files = [p for p in out.rglob("*") if p.is_file() and p.name != "report.json"]
    report = {"built": True, "component_revision": revision, "service_cdhash": service_hash,
              "launchd_cache_cdhash": cache_report["cdhash"], "cache": cache_report,
              "bytes": {str(p.relative_to(out)): p.stat().st_size for p in files},
              "sha256": {str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"wifi userspace: binary={binary.stat().st_size} cache={cache.stat().st_size} "
          f"trust={service_hash},{cache_report['cdhash']}")


if __name__ == "__main__":
    main()
