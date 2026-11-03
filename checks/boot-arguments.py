#!/usr/bin/env python3
"""Check the actual UEFI command line fits Darwin 22 without losing touch."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'uefi/overlay/Platforms/Xiaomi/houjiPkg/Application/HoujiStorageProbe/HoujiStorageProbe.c').read_text()
base = re.search(r'BaseBootArgs = L"([^"]+)";', source).group(1)
capacity = int(re.search(r'CHAR16 BootArgs\[(\d+)\];', source).group(1))
template = re.search(r'UnicodeSPrint\(BootArgs,sizeof\(BootArgs\),L"([^"]+)"', source).group(1)
# yoni13 fork: this kernel's GPU governor takes houji_gpu_maxlevel, not upstream's gpu_max_mhz, so the base line carries no
# ceiling; the budget, duplicate and touch-handoff checks are unchanged.
assert 'gpu_max_mhz=' not in base
for touch in (0, 1):
    command = template % (base, touch)
    assert len(command.encode('ascii')) + 1 <= min(capacity, 608)
    options = [word.split('=', 1)[0] for word in command.split()]
    assert len(options) == len(set(options)), 'duplicate boot argument'
    assert command.endswith(f'houji_touch={touch}')
assert 'StrLen(BaseBootArgs) + StrLen(L" houji_touch=1") >= ARRAY_SIZE(BootArgs)' in source
print('PASS: UEFI boot line and touch handoff fit the 608-byte Darwin 22 command line')
