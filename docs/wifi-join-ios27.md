# Wi-Fi scan and join on iOS 27

## Sheng reference

Sheng pins Ath12kWifi `5f7f5a8a`, whose `tools/build-supplicant.py --target ios` builds wpa_supplicant 2.12 plus the native `sheng` control backend. `build-wifi-userspace.py` ad-hoc signs it with the no-container entitlement, writes a key-free `ap_scan=1` configuration and an `org.unixnu.wifi.supplicant` launchd job, and stages firmware separately. UniXNU merges that job into the stock `System/Library/xpc/launchd.plist`, serializes it with Apple `plutil`, makes a detached 4 KiB-page signature, and adds both cache and executable CDHashes to the trust bundle. Before PID 1, sheng bind-mounts Data directories over `/System/Library/xpc` and `/usr/libexec`; launchd starts `/usr/libexec/unixnu-wpa_supplicant -Dsheng -ien0`. iOS selects the network and supplies credentials through its native control channel; `wifi-aj`/`join.txt` is testing-only and must not be shipped.

## Houji gap and minimal plan

Houji already has firmware, Ath12kWifi, `en0`, and Apple80211, but its GPU add-on only exposes the Metal extension (and inherited optional GPU paths). Merely putting a plist in Data cannot add an iOS 27 job: launchd reads its compiled cache. Apply `docs/wifi-join-ios27.patch`, build with `HOUJI_WIFI_LINK=1`, run the new Mac builder against the stock **24B156** launchd cache, then stage its four runtime files and append its two reported CDHashes. The kernel patch is drafted against the post-series `kern_exec.c`; it intentionally lives after sheng's patches. Boot with `wifi=5 wifi-userspace=1`; `wifi-userspace` is independent of `gpu-addon`. Choose and enter the network only in Settings.

The staging script reports the exact packed Data-seed delta. Before building, budget roughly 1–3 MiB for the signed supplicant, merged 0.8 MiB cache and compression, in addition to the already measured firmware seed delta (about 5.0 MiB). A 69.3 MB GPU image therefore has little margin under 77 MB; the final boot image size is authoritative.

## Apple80211 requests and risks

Settings needs power/state/capabilities, scan set requests 10/86, scan-done event 10, iterative scan-result get 11, and channel/country data. Join needs associate 20, result 21, auth type 2, SSID/BSSID/state, and association/link/SSID/BSSID events. `ENOENT` (2) for SSID/BSSID/channel/RSSI while disconnected is expected. `EOPNOTSUPP` (102) on a critical request above can suppress networks or joining; unrelated diagnostics may remain unsupported.

Risks are a 24B156 cache/parser mismatch, rejected detached signature or trust hash, xpcproxy path policy, control ABI drift, repeated launchd crashes, incomplete scan-result layout/events, and exceeding the bootloader limit.
