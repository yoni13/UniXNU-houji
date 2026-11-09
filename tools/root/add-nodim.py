#!/usr/bin/env python3
"""Merge (or, with --remove, take out) the UniXNU --no-auto-dim SpringBoard preferences in the root image.
  add-nodim.py IMAGE REPORT.json [--remove]

SBDisableAutoDim, SBDontDimOrLockOnAC, SBDisableIdleSleep: without them iOS turns the display off after its Auto-Lock time
and idle-sleeps. Before system sleep worked the port could not wake from that, so bring-up images set them (UniXNU bd127c00
scripts/seed-data-skip-setup.py; houji r53 applied the same). With houji_sleep=1 they must be absent: SBDisableIdleSleep keeps
a SystemIsActive assertion, for which powerd holds the kernel's idle sleep at 0, and SBDontDimOrLockOnAC keeps the display on
while charging. --remove takes them out; iOS's own Auto-Lock then works too.
"""
import json, plistlib, sys
import guestfs

image, out = sys.argv[1], sys.argv[2]
remove = '--remove' in sys.argv[3:]
PATH = '/private/var/mobile/Library/Preferences/com.apple.springboard.plist'
KEYS = {'SBDisableAutoDim': True, 'SBDontDimOrLockOnAC': True, 'SBDisableIdleSleep': True}
g = guestfs.GuestFS(python_return_dict=True)
g.set_network(False)
g.add_drive_opts(image, format='raw')
g.launch()
g.mount_vfs('force,rw', 'hfsplus', '/dev/sda', '/')
try:
    before = plistlib.loads(g.read_file(PATH))
    assert isinstance(before, dict)
    merged = dict(before)
    if remove:
        for k in KEYS:
            merged.pop(k, None)
    else:
        merged.update(KEYS)
    g.write(PATH, plistlib.dumps(merged, fmt=plistlib.FMT_BINARY))
    g.chown(501, 501, PATH)
    g.chmod(0o600, PATH)
    g.sync()
    after = plistlib.loads(g.read_file(PATH))
    st = g.statns(PATH)
    if remove:
        assert not any(k in after for k in KEYS)
    else:
        assert all(after.get(k) is True for k in KEYS)
    assert (st['st_uid'], st['st_gid'], st['st_mode'] & 0o777) == (501, 501, 0o600)
    g.umount_all()
    g.shutdown()
finally:
    g.close()
open(out, 'w').write(json.dumps({'path': PATH, 'before': sorted(before), 'after': {k: str(v) for k, v in after.items()}}, indent=2) + '\n')
print('removed' if remove else 'merged', sorted(KEYS))
