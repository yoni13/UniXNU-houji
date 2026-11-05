#!/usr/bin/env python3
"""Replace NativeAdrenoMetal.bundle in an offline root image (guestfs).

Installs the CI-built bundle (with the texture-limits frontend) over the
image's existing /System/Library/Extensions/NativeAdrenoMetal.bundle,
preserving the clients/publish sidecar files from the source directory.
"""
import argparse
import os
import guestfs

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', required=True)
    ap.add_argument('--bundle-dir', required=True, help='directory containing NativeAdrenoMetal.bundle/ and sidecars')
    args = ap.parse_args()
    bundle = os.path.join(args.bundle_dir, 'NativeAdrenoMetal.bundle')
    assert os.path.isdir(bundle)
    g = guestfs.GuestFS(python_return_dict=True)
    g.set_network(False); g.set_memsize(2048); g.set_smp(4)
    g.add_drive_opts(args.image, format='raw'); g.launch()
    g.mount_vfs('force,rw', 'hfsplus', '/dev/sda', '/')
    dst = '/System/Library/Extensions/NativeAdrenoMetal.bundle'
    assert g.is_dir(dst), 'bundle missing in image'
    # remove old bundle content
    for f in g.find(dst):
        if g.is_file(f):
            g.rm(f)
    # copy new bundle
    for root, dirs, files in os.walk(bundle):
        rel = os.path.relpath(root, bundle)
        target = dst if rel == '.' else f'{dst}/{rel}'
        if not g.is_dir(target):
            g.mkdir(target)
        for f in files:
            src = os.path.join(root, f)
            g.write(f'{target}/{f}', open(src, 'rb').read())
            mode = os.stat(src).st_mode & 0o777
            g.chmod(mode, f'{target}/{f}')
            g.chown(0, 0, f'{target}/{f}')
    # sidecars
    for side in ('NativeAdrenoMetal.clients', 'NativeAdrenoMetal.publish'):
        p = os.path.join(args.bundle_dir, side)
        if os.path.isfile(p):
            g.write(f'/System/Library/Extensions/{side}', open(p, 'rb').read())
            g.chmod(0o644, f'/System/Library/Extensions/{side}')
            g.chown(0, 0, f'/System/Library/Extensions/{side}')
    print('bundle installed')
    g.umount('/'); g.sync()
    print('INSTALL-OK')
