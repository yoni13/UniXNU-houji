#!/usr/bin/env python3
"""Install the keyboard/status-bar geometry dylib into an offline root image.

Runs inside the unixnu-root container (guestfs). Installs
/usr/lib/HoujiKeyboardGeometry.dylib (built by tools/build-userland-compat.py)
and adds an LC_LOAD_WEAK_DYLIB load command for it to every APPL main
executable under /Applications, using the Mach-O header padding. The
executables are ad-hoc signed without enforced page hashes, so the header
patch does not invalidate their signatures. Malformed weak entries from an
earlier buggy installer (name offset 8 instead of 24) are dropped first.
"""
import argparse
import struct
import guestfs

LC_LOAD_WEAK_DYLIB = 0x80000018
DYLIB_PATH = '/usr/lib/HoujiKeyboardGeometry.dylib'

def walk(d):
    ncmds = struct.unpack_from('<I', d, 16)[0]
    cmds = []
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', d, off)
        cmds.append((off, cmd, cmdsize))
        off += cmdsize
    return cmds, off

def cmd_name(d, off):
    nameoff = struct.unpack_from('<I', d, off+8)[0]
    if nameoff < 24 or nameoff >= 4096:
        return None
    end = d.find(b'\0', off+nameoff)
    if end < 0 or end - (off+nameoff) > 256:
        return None
    return d[off+nameoff:end]

def first_text_fileoff(d, cmds):
    for off, cmd, cmdsize in cmds:
        if cmd == 0x19:
            segname = d[off+8:off+24].rstrip(b'\0').decode(errors='replace')
            nsects = struct.unpack_from('<I', d, off+64)[0]
            if segname == '__TEXT' and nsects:
                so = off + 72
                _, _, fileoff = struct.unpack_from('<QQI', d, so+32)
                return fileoff
    return None

def process(d):
    cmds, end = walk(d)
    keep = []
    found_valid = False
    for off, cmd, cmdsize in cmds:
        if cmd == LC_LOAD_WEAK_DYLIB:
            name = cmd_name(d, off)
            if name == DYLIB_PATH.encode():
                found_valid = True
                keep.append((off, cmdsize))
                continue
            if name is None or len(name) < 5:
                continue  # malformed: drop
            keep.append((off, cmdsize))
        else:
            keep.append((off, cmdsize))
    if found_valid:
        return None
    out = bytearray(d)
    blob = bytearray()
    for off, cmdsize in keep:
        blob += d[off:off+cmdsize]
    name = DYLIB_PATH.encode() + b'\0'
    cmdsize = 24 + ((len(name)+3)//4)*4
    lc = struct.pack('<II', LC_LOAD_WEAK_DYLIB, cmdsize)
    lc += struct.pack('<I', 24)          # name offset: past the 24-byte fixed fields
    lc += struct.pack('<III', 0, 0, 0)   # timestamp, current_version, compatibility_version
    lc += name
    lc += b'\0' * (cmdsize - len(lc))
    blob += lc
    limit = first_text_fileoff(d, cmds)
    assert limit is not None
    assert len(blob) <= limit - 32, f'command area overflow: {len(blob)} > {limit-32}'
    out[32:32+len(blob)] = blob
    for i in range(32+len(blob), limit):
        out[i] = 0
    struct.pack_into('<I', out, 16, len(keep)+1)
    struct.pack_into('<I', out, 20, len(blob))
    return bytes(out)

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', required=True, help='offline root image (hfsx)')
    ap.add_argument('--dylib', required=True, help='HoujiKeyboardGeometry.dylib to install')
    args = ap.parse_args()
    g = guestfs.GuestFS(python_return_dict=True)
    g.set_network(False); g.set_memsize(2048); g.set_smp(4)
    g.add_drive_opts(args.image, format='raw'); g.launch()
    g.mount_vfs('force,rw', 'hfsplus', '/dev/sda', '/')
    if not g.is_file(DYLIB_PATH):
        dylib = open(args.dylib, 'rb').read()
        g.write(DYLIB_PATH, dylib)
        g.chmod(0o755, DYLIB_PATH)
        g.chown(0, 0, DYLIB_PATH)
        print('dylib installed', len(dylib), 'bytes')
    else:
        print('dylib already present')
    patched = already = failed = 0
    for entry in g.ls('/Applications'):
        if not entry.endswith('.app'):
            continue
        name = entry[:-4]
        main = f'/Applications/{entry}/{name}'
        if not g.is_file(main):
            continue
        data = g.read_file(main)
        try:
            out = process(data)
        except Exception as e:
            print('FAIL', main, repr(e)); failed += 1
            continue
        if out is None:
            already += 1
            continue
        g.write(main, out)
        patched += 1
    print(f'patched={patched} already={already} failed={failed}')
    g.umount('/'); g.sync()
    print('INSTALL-OK')
