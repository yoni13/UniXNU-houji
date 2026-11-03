#!/usr/bin/env python3
"""Turn off the WiFiManager feature flag LinkUpEventAfter4way in the root image.

With the flag on, wifid holds a WPA network's link-up (and so its IPv4, default route and DNS setup) until the driver posts
APPLE80211_M_SUPPLICANT_EVENT for the 4-way handshake. The houji Wi-Fi driver (ath12k module) runs the handshake itself and reports
link up only after the keys are installed; it posts no supplicant event. Seen in wifimanager.log: "Hold off dispatching client link
up event until 4 way completes." (__WiFiManagerDeviceLinkCallback, gated by _os_feature_enabled_impl("WiFiManager",
"LinkUpEventAfter4way")).
"""
import json, plistlib, sys
import guestfs

image, out = sys.argv[1], sys.argv[2]
PATH = '/System/Library/FeatureFlags/Domain/WiFiManager.plist'
g = guestfs.GuestFS(python_return_dict=True)
g.set_network(False)
g.add_drive_opts(image, format='raw')
g.launch()
g.mount_vfs('force,rw', 'hfsplus', '/dev/sda', '/')
try:
    raw = g.read_file(PATH)
    fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
    flags = plistlib.loads(raw)
    flags['LinkUpEventAfter4way'] = {'Enabled': False}
    st = g.statns(PATH)
    g.write(PATH, plistlib.dumps(flags, fmt=fmt))
    g.chown(st['st_uid'], st['st_gid'], PATH)
    g.chmod(st['st_mode'] & 0o7777, PATH)
    g.sync()
    assert plistlib.loads(g.read_file(PATH))['LinkUpEventAfter4way'] == {'Enabled': False}
    g.umount_all()
    g.shutdown()
finally:
    g.close()
open(out, 'w').write(json.dumps({'path': PATH, 'LinkUpEventAfter4way': False}, indent=2) + '\n')
print('LinkUpEventAfter4way disabled')
