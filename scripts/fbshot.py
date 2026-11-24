#!/usr/bin/env python3
"""Full-resolution screenshots for visual A/B checks, and their comparison.
  fbshot.py grab LOG OUT.png [X Y W H]   console fbdump of a framebuffer region (default the whole 1200x2670 panel), saved by
                                         capture.py as a dump, CRC-checked, written as an 8-bit RGB PNG (BGRA in the framebuffer)
  fbshot.py diff A.png B.png [OUT.png]   per-channel differences: pixels that differ, by how much, and where (OUT: the
                                         difference amplified x32, for looking)"""
import os, re, subprocess, sys, time, zlib


def grab(log, out, region=(0, 0, 1200, 2670)):
    x, y, w, h = region
    start = os.path.getsize(log)
    subprocess.run(['timeout', '6', 'python3', os.path.join(os.path.dirname(__file__), '../perf-test/phone.py'), 'cmd',
                    f'fbdump {x} {y} {w} {h}'], capture_output=True)
    deadline = time.time() + 60
    saved = phone = None
    while time.time() < deadline:
        time.sleep(0.5)
        with open(log, 'rb') as f:
            f.seek(start)
            text = f.read().decode('latin-1')
        s = re.search(r'HOUJI DUMP SAVED (\S+) [0-9a-f]+ (\d+) crc ([0-9a-f]+)', text)
        p = re.search(r'HOUJI DUMP: framebuffer (\d+) bytes from [0-9a-f]+, crc ([0-9a-f]+), \d+ ms, error (-?\d+)', text)
        if s and p:
            saved, phone = s, p
            break
    if not saved:
        sys.exit('no framebuffer dump in the log')
    if phone[3] != '0' or saved[3] != phone[2] or int(saved[2]) != w * h * 4:
        sys.exit(f'dump failed: error {phone[3]}, crc phone {phone[2]} host {saved[3]}, {saved[2]} bytes of {w * h * 4}')
    data = open(saved[1], 'rb').read()
    assert zlib.crc32(data) & 0xffffffff == int(saved[3], 16)
    from PIL import Image
    img = Image.frombuffer('RGBA', (w, h), data, 'raw', 'BGRA', 0, 1).convert('RGB')
    img.save(out)
    print(out, w, h, 'crc', saved[3])


def diff(a, b, out=None):
    from PIL import Image, ImageChops
    A, B = Image.open(a).convert('RGB'), Image.open(b).convert('RGB')
    if A.size != B.size:
        sys.exit(f'sizes differ: {A.size} {B.size}')
    d = ImageChops.difference(A, B)
    hist = [0] * 256
    for band in d.split():
        for v, n in enumerate(band.histogram()):
            hist[v] += n
    px = A.size[0] * A.size[1]
    dmax = max(v for v in range(256) if hist[v]) if any(hist) else 0
    m = d.convert('L').point(lambda v: 255 if v else 0)
    differing = m.histogram()[255]
    print(f'{a} vs {b}: {differing} of {px} pixels differ ({100 * differing / px:.3f} %), largest channel difference {dmax}, '
          f'channel values by difference: ' + ', '.join(f'{v}: {hist[v]}' for v in range(1, 256) if hist[v])[:400])
    bbox = m.getbbox()
    print('  differing region (bounding box):', bbox)
    if out:
        d.point(lambda v: min(255, v * 32)).save(out)


if __name__ == '__main__':
    if sys.argv[1] == 'grab':
        grab(sys.argv[2], sys.argv[3], tuple(int(v) for v in sys.argv[4:8]) if len(sys.argv) >= 8 else (0, 0, 1200, 2670))
    elif sys.argv[1] == 'diff':
        diff(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
