#!/usr/bin/env python3
"""Synthetic layout tests only; these do not qualify RSA or device storage."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('tables', Path(__file__).with_name('add-root-tables.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class LayoutTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir='/tmp/opencode')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source, self.output = self.root / 'source.img', self.root / 'output.img'
        header = bytearray(4096)
        header[:8] = b'SHNGROOT'
        struct.pack_into('<IIQ', header, 8, 1, 2, 3 * module.MIB)
        with self.source.open('wb') as handle:
            handle.truncate(3 * module.MIB)
            for index, name in enumerate(('OS', 'SystemOS')):
                data = bytes([index + 1]) * 4096
                digest = hashlib.sha256(data).digest()
                handle.seek((index + 1) * module.MIB)
                handle.write(data)
                struct.pack_into('<32sQQII32s40s', header, 24 + index * 128,
                                 name.encode(), (index + 1) * module.MIB, len(data), 512, 1, digest, bytes(40))
                table = self.root / f'verified-root-{name}'
                table.mkdir()
                (table / 'hashes.bin').write_bytes(digest)
                policy = self.root / name
                policy.mkdir()
                signed = bytearray(544)
                signed[:8] = b'VROOT01\0'
                struct.pack_into('<QQ', signed, 24, len(data), 1)
                signed[56:88], signed[88:120] = digest, hashlib.sha256(digest).digest()
                (policy / 'manifest.header.bin').write_bytes(signed)
                (policy / 'verification.json').write_text(json.dumps(dict(
                    real_c_signature_and_block_verification=True, image_sha256=digest.hex())))
            header[4064:] = hashlib.sha256(header[:4064]).digest()
            handle.seek(0)
            handle.write(header)

    def build(self, capacity=5 * module.MIB):
        module.build(self.source, self.output, self.root, self.root, capacity)

    def test_round_trip_and_no_overwrite(self):
        self.build()
        data = self.output.read_bytes()
        self.assertEqual(struct.unpack_from('<IIQ', data, 8), (2, 4, 5 * module.MIB))
        self.assertEqual(data[4064:4096], hashlib.sha256(data[:4064]).digest())
        for index in (0, 1):
            entry = struct.unpack_from('<32sQQII32s', data, 24 + (index + 2) * 128)
            self.assertEqual(entry[3:5], (4096, 3))
            self.assertEqual(hashlib.sha256(data[entry[1]:entry[1] + entry[2]]).digest(), entry[5])
        with self.assertRaises(FileExistsError):
            self.build()

    def test_corrupt_window_rejected(self):
        with self.source.open('r+b') as handle:
            handle.seek(module.MIB)
            handle.write(b'X')
        with self.assertRaisesRegex(ValueError, 'image hash mismatch'):
            self.build()
        self.assertFalse(self.output.exists())

    def test_corrupt_table_rejected(self):
        (self.root / 'verified-root-OS/hashes.bin').write_bytes(bytes(32))
        with self.assertRaisesRegex(ValueError, 'identity mismatch'):
            self.build()

    def test_capacity_rejected(self):
        with self.assertRaisesRegex(ValueError, 'capacity'):
            self.build(4 * module.MIB)


if __name__ == '__main__':
    unittest.main()
