#!/usr/bin/env python3
"""Create a first-install APFS Data seed on Linux (UniXNU scripts/make-data-seed.py without macOS hdiutil).

  make-data-seed-linux.py --unixnu UNIXNU --os-image OS.img --output data.dbr --gib N --build BUILD [--skip-setup]

The OS image is the raw OS window (a bare APFS container). The new volume is formatted on the host with apfsprogs mkfs.apfs;
copying the OS's /private/var into it keeps file owners, so that part runs as root in one privileged container that loop-mounts
both images (linux-apfs-rw module on the host). Then the image is packed and grown with UniXNU's own tools. The seed is
installation state: keep it, and never commit it (it holds files of the OS image).
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

# Runs inside the container: the same steps as make-data-seed.py after its hdiutil attach.
INNER = r'''
import os, plistlib, shutil, stat, subprocess, sys
from pathlib import Path
os_image, seed, build, skip = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] == '1'
def loop(path, ro):
    return subprocess.check_output(['losetup', '--find', '--show'] + (['--read-only'] if ro else []) + [path], text=True).strip()
src_loop, dst_loop = loop(os_image, True), loop(seed, False)
osm, mount = Path('/mnt/os'), Path('/mnt/seed')
osm.mkdir(parents=True, exist_ok=True); mount.mkdir(parents=True, exist_ok=True)
try:
    subprocess.run(['mount', '-t', 'apfs', '-o', 'ro', src_loop, osm], check=True)
    # linux-apfs-rw mounts read-only unless writes are asked for by name.
    subprocess.run(['mount', '-t', 'apfs', '-o', 'readwrite', dst_loop, mount], check=True)
    if b' rw,' not in subprocess.check_output(['grep', ' /mnt/seed ', '/proc/mounts']):
        sys.exit('the new Data volume did not mount read-write')
    source = osm / 'private/var'
    if not (osm / 'sbin/launchd').is_file() or not source.is_dir():
        sys.exit('not an IPSW OS volume')
    for current, dirs, files in os.walk(source, followlinks=False):
        dirs[:] = [name for name in dirs if name != 'staged_system_apps']
        dest = mount / Path(current).relative_to(source)
        dest.mkdir(exist_ok=True)
        st = Path(current).lstat()
        os.chown(dest, st.st_uid, st.st_gid); dest.chmod(stat.S_IMODE(st.st_mode))
        for name in list(dirs) + files:
            src, dst = Path(current) / name, dest / name
            if src.is_symlink():
                dst.symlink_to(os.readlink(src))
                os.lchown(dst, src.lstat().st_uid, src.lstat().st_gid)
                if name in dirs:
                    dirs.remove(name)
            elif src.is_file():
                shutil.copy2(src, dst)
                os.chown(dst, src.lstat().st_uid, src.lstat().st_gid)
    for name in ('keybags', 'mobile/tmp', 'tmp', 'run', 'containers/Bundle',
                 'containers/Data/System', 'containers/Shared/SystemGroup', 'containers/Temp'):
        (mount / name).mkdir(parents=True, exist_ok=True)
    os.chown(mount / 'mobile/tmp', 501, 501)
    (mount / 'tmp').chmod(0o1777)
    def merge(home, name, values):
        path = mount / home / 'Library/Preferences' / (name + '.plist')
        path.parent.mkdir(parents=True, exist_ok=True)
        content = plistlib.loads(path.read_bytes()) if path.is_file() else {}
        content.update(values)
        path.write_bytes(plistlib.dumps(content, fmt=plistlib.FMT_BINARY))
        path.chmod(0o644)
        owner = 501 if home == 'mobile' else 0
        for part in (path, path.parent, path.parent.parent):   # the plist, Preferences/, Library/
            os.chown(part, owner, owner)
    if skip:
        merge('mobile', 'com.apple.purplebuddy', dict(SetupDone=True, SetupFinishedAllSteps=True, SetupVersion=11,
              daemonizeNeedsToRun=False))
        merge('mobile', 'com.apple.purplebuddy.notbackedup', {name: False for name in (
            'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy', 'ShouldRunKeychainSyncMiniBuddy',
            'ShouldRunMesaMiniBuddy', 'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy', 'SilentICDPUpgrade')})
        merge('mobile', 'com.apple.managedconfiguration.notbackedup', dict(LockdownActivationIndicatesCloudConfigurationAvailable=False))
        merge('mobile', 'com.apple.springboard', dict(BKNoWatchdogs=True,
              FBLaunchWatchdogExceptions={'com.apple.purplebuddy': 100.0}, MCForceShowCloudConfigurationUI=False))
        merge('mobile', 'com.apple.migration', dict(LastSystemVersion=build,
              DMUserDataDisposition=dict(SyntheticDidUpgrade=False, buildVersion=build)))
    for home in ('root', 'mobile'):
        for name in ('backboardd', '.GlobalPreferences'):
            merge(home, name, dict(CAEnableDeepFramebuffer=False, CAEnableFramebufferCompression=False,
                  CAEnableIOSurfaceCompression=False))
    files = sum(len(f) for _, _, f in os.walk(mount))
    owners = {(p.lstat().st_uid) for p in (mount / 'mobile').rglob('*')} if (mount / 'mobile').is_dir() else set()
    print(f'COPIED: {files} files; owners under mobile/: {sorted(owners)}')
finally:
    subprocess.run(['umount', mount]); subprocess.run(['umount', osm])
    subprocess.run(['losetup', '-d', dst_loop]); subprocess.run(['losetup', '-d', src_loop])
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path, required=True)
    parser.add_argument('--os-image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gib', type=int, required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--image', default='unixnu-build-seed', help='container image with python3 and util-linux')
    parser.add_argument('--skip-setup', action='store_true')
    args = parser.parse_args()
    if args.output.exists():
        parser.error('refuse to overwrite an existing installation seed')
    if not 3 <= args.gib <= 1024:
        parser.error('--gib must be in 3..1024')
    sys.path.insert(0, str(args.unixnu / 'drivers/DarwinBoot/Test'))
    import packed_ramdisk
    volume = str(uuid.uuid4()).upper()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='new-data-', dir=args.output.parent) as td:
        work = Path(td)
        raw = work / 'seed.raw'
        with raw.open('wb') as f:
            f.truncate(32 << 20)
        subprocess.run(['mkfs.apfs', '-L', 'Data', '-u', volume, str(raw)], check=True, timeout=60)
        (work / 'inner.py').write_text(INNER)
        subprocess.run(['docker', 'run', '--rm', '--privileged', '-v', f'{work}:/w',
                        '-v', f'{args.os_image.resolve()}:/os.img:ro', args.image,
                        'python3', '/w/inner.py', '/os.img', '/w/seed.raw', args.build, '1' if args.skip_setup else '0'],
                       check=True, timeout=900)
        base = work / 'base.dbr'
        packed_ramdisk.pack(raw, base)
        candidate = work / 'data.dbr'
        subprocess.run([sys.executable, str(args.unixnu / 'scripts/seed-data-grow.py'), '--gib', str(args.gib),
                        str(base), str(candidate)], check=True, timeout=600)
        candidate.replace(args.output)
    print(json.dumps(dict(output=str(args.output), volume_uuid=volume, first_install=True,
                          preserve_for_future_builds=True, hardware_tested=False)))


if __name__ == '__main__':
    main()
