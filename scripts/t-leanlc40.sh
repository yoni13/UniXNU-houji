#!/bin/bash
# leanlc40: leanlc5 on the gpu42 GPU userspace (gpu41 + gtr=1 diagnostics: shader dumps, tu_init_hw split, copy detail, shadow log).
# and Siri off). root225: Spotlight's AFNetworkAvailability flapped (the daemon's published "daemon.network.available" state against
# the process's own path monitor on a phone without a joined network), each flip a dictation availability change and a keyboard
# dock rebuild. Cache signed on the Mac: 642025cf...
set -euo pipefail
I=/mnt/6f99f95d-0b93-4a42-84ea-c848405a2812/unixnu-work/ios27; cd $I
rm -f data/data-24B156-gpu8-leanlc40.dbr tmp/trust-24B156-gpu8-leanlc40.bin
timeout 900 python3 UniXNU-houji/scripts/add-data-files.py --unixnu $I/UniXNU --seed $I/data/data-24B156-gpu42-wifi-quiet-ax.dbr \
  --output $I/data/data-24B156-gpu8-leanlc40.dbr "$I/tmp/seedfiles/disabled-lean4.plist:db/com.apple.xpc.launchd/disabled.plist" \
  "$I/tmp/lean-cache4/launchd.plist:ngpu/xpc/launchd.plist" "$I/tmp/lean-cache4/launchd.plist.sig:ngpu/xpc/launchd.plist.sig" \
  "$I/tmp/seedfiles/com.apple.assistant.support.plist:mobile/Library/Preferences/com.apple.assistant.support.plist:501" | grep -c ADDED
read -r P Tu < <(python3 -c "import json;t=json.load(open('gpu-plugin/adreno-metal-plugin27/trust.json'));print(t['cdhash'],t['turnip_cdhash'])")
python3 UniXNU/scripts/trust-append-module.py tmp/trust-24B156.bin tmp/trust-24B156-gpu8-leanlc40.bin $P $Tu \
  8bbe3068cb6fa63aee74011a8c1d0457f6147886 414a733a1a0b10220bdd6dd67e5bdc9b0e8372fb
ls -la data/data-24B156-gpu8-leanlc40.dbr tmp/trust-24B156-gpu8-leanlc40.bin | awk '{print $5, $9}'
