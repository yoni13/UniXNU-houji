#!/usr/bin/env python3
"""Persist CAEnableDeepFramebuffer=false (and framebuffer compression off) on disk.

The kernel display copy (SweetFramebuffer) only understands 8-bit 'BGRA' surfaces. iPhone10,3
reports a deep-colour display, so Core Animation later rebuilds backboardd's framebuffer as
'b3a8' (extended-range) unless the preference is set before it decides; the plugin's in-process
CFPreferencesSetAppValue is too late. Written for backboardd's own domain (mobile and root homes)
and the global domain.
"""
import json, plistlib, sys
import guestfs

image, out = sys.argv[1], sys.argv[2]
KEYS = {'CAEnableDeepFramebuffer': False, 'CAEnableFramebufferCompression': False}
TARGETS = [
    ('/private/var/mobile/Library/Preferences/com.apple.backboardd.plist', 501),
    ('/private/var/mobile/Library/Preferences/.GlobalPreferences.plist', 501),
    ('/private/var/root/Library/Preferences/com.apple.backboardd.plist', 0),
]
g = guestfs.GuestFS(python_return_dict=True)
g.set_network(False)
g.add_drive_opts(image, format='raw')
g.launch()
g.mount_vfs('force,rw', 'hfsplus', '/dev/sda', '/')
report = {}
try:
    for path, uid in TARGETS:
        parent = path.rsplit('/', 1)[0]
        if not g.exists(parent):
            g.mkdir_p(parent)
            g.chown(uid, uid, parent)
            g.chmod(0o755, parent)
        existed = g.exists(path)
        data = plistlib.loads(g.read_file(path)) if existed else {}
        assert isinstance(data, dict)
        data.update(KEYS)
        g.write(path, plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
        g.chown(uid, uid, path)
        g.chmod(0o600, path)
        back = plistlib.loads(g.read_file(path))
        assert all(back.get(k) is False for k in KEYS)
        st = g.statns(path)
        report[path] = {'existed': existed, 'keys': sorted(back), 'uid': st['st_uid'], 'mode': oct(st['st_mode'] & 0o777)}
    g.sync()
    g.umount_all()
    g.shutdown()
finally:
    g.close()
open(out, 'w').write(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
