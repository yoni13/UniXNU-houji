#!/usr/bin/env python3
"""Merge the UniXNU --no-auto-dim SpringBoard preferences into the root image.

SBDisableAutoDim, SBDontDimOrLockOnAC, SBDisableIdleSleep: without them iOS turns the
display off ~30 s after SpringBoard starts and idle-sleeps; this port cannot wake from that.
Source: UniXNU bd127c00 scripts/seed-data-skip-setup.py; houji r53 applied the same.
"""
import json, plistlib, sys
import guestfs

image, out = sys.argv[1], sys.argv[2]
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
    merged.update(KEYS)
    g.write(PATH, plistlib.dumps(merged, fmt=plistlib.FMT_BINARY))
    g.chown(501, 501, PATH)
    g.chmod(0o600, PATH)
    g.sync()
    after = plistlib.loads(g.read_file(PATH))
    st = g.statns(PATH)
    assert all(after.get(k) is True for k in KEYS)
    assert (st['st_uid'], st['st_gid'], st['st_mode'] & 0o777) == (501, 501, 0o600)
    g.umount_all()
    g.shutdown()
finally:
    g.close()
open(out, 'w').write(json.dumps({'path': PATH, 'before': sorted(before), 'after': {k: str(v) for k, v in after.items()}}, indent=2) + '\n')
print('merged', sorted(KEYS))
