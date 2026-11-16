# houji Wi-Fi (WCN7850, ath12k over PCIe0) on iOS 27: port plan

Not ported yet. This records what the port is built from. Paths: sheng = `ref/UniXNU-sheng`, the iOS 16 port = `fork`.

## Reference: sheng on iOS 27 (SM8550, working)

- Built by `scripts/build-gop-legacy-display-27.py` with `scripts/build-ath12k-wifi.py` (pinned Linux sources, `ATH12K_WIFI_DIR`
  naming the Ath12kWifi checkout) and the board files `package/WifiProbe.cpp`, `WifiNet.cpp`, `WifiFirmware.c/.h`,
  `WifiPcieTables.h`, `MhiBoot.c/.h`, `QcomRpmh.c/.h`, `WcnPower.h`.
- Started from `LegacyDisplayProbe.cpp` by `sheng_wifi_probe()`, staged by `wifi=<n>`: 1 survey, 2 power and link, 3 MHI load,
  4 chip reset and full firmware, 5 Ath12kWifi and the network interface (`wifi=5` in sheng's board.toml).
- Firmware is not in the kernel: `amss.bin`, `m3.bin`, `board-2.bin` are read through the VFS from
  `/private/var/ngpu/firmware/ath12k/WCN7850/hw2.0/` (staged by `scripts/build-wifi-userspace.py`, which also builds wpa_supplicant).
- `WifiNet.cpp` attaches `en0` and publishes `IONetworkStack`, `IO80211Controller` and `IO80211Interface`; scan and join go through
  Apple80211 ioctls.
- Ath12kWifi at UniXNU's pin `28de7b91` (mainline cfg80211/mac80211 runtime, wpa_supplicant). `ref/Ath12kWifi` is the older
  `f6bf561` plus 27 local lines (power-save off after authorization, HTT stats filter, scan, hardware mode and dual-MAC knobs).

## houji (SM8650) values, from the iOS 16 port (`fork/kernel/overlay/iokit/Kernel/houji-wifi/HoujiWifi.cpp`)

| Item | houji | sheng |
|---|---|---|
| PCIe0 PARF / DBI / PHY / ATU / config | same as sheng | 0x1c00000 / 0x60000000 / 0x1c06000 / 0x60001000 / 0x60100000 |
| MSI SPI, SMMU streams | 141, 0x1400/0x1401 | same |
| WLAN_EN GPIO | 16 | 80 |
| GDSC collapse, NoC branches | 0x5214c; 0x10058/64/74 | 0x52020; 0x10030/3c/4c |
| TCSR clock gate | 0x1fc0000 + 0x31100 | 0x1fd5000 |
| RPMh rails | smpi4 940, ldob15 1800, ldoc3 1200, smpc2 976, smpc3 916, smpc1 1316, smpc6 1864 (+ ldok6 1800, RF_CLK1) | six rails |
| Firmware | modem partition `image/kiwi`: amss20.bin, phy_ucode20.elf (m3), bd_n3gl.elf (board), regdb_xiaomi.bin | hash-pinned set |

Power sequence on houji: rails, PERST/CLKREQ, WLAN_EN low 150 ms then high 60 ms, GDSCs/clocks/PHY/SID/PERST/LTSSM.
The nine houji ath12k patches (`fork/kernel/ath12k*`): heartbeat HTT stats filter, power save off after authorization, on-demand
power save, scan policy, hardware mode, dual-MAC (`WMI_PDEV_SET_MAC_CONFIG`).

## Driver revision

houji builds Ath12kWifi at **5f7f5a8a** (sheng's submodule pin at UniXNU-sheng 6880068), not UniXNU's catalog pin 28de7b91: sheng's
`WifiProbe.cpp` uses the newer board interface (`nw_board.diagnostics`, added in 5f7f5a8 "log receive counters while frames arrive").
The build Mac keeps that checkout at `UniXNU/drivers/Ath12kWifi`, which UniXNU prefers over the catalog's git source, and `build.sh`
refuses any other revision.

## Steps

1. Select the Ath12kWifi component (UniXNU pin, the sheng iOS 27 architecture, not `f6bf561`).
2. houji `WifiPcieTables.h` and the board layer from `HoujiWifi.cpp`, written against the `28de7b91` interfaces.
3. `WifiProbe.cpp`, `WifiNet.cpp`, the firmware loader in `kits/houji`, built by `build-houji-display.py`; `houji_wifi_probe()` from
   `LegacyDisplayProbe.cpp` behind `wifi=<n>`; remove the Wi-Fi stubs in `HoujiStage0.c`.
4. SMMU stream 0x1401 and a 32-bit DMA window (`prepare-houji-ufs.py` disables sheng's Wi-Fi mapping today).
5. Firmware staged on the Data seed under sheng's path and names (board.bin vs board-2.bin decided from the files).
6. `/arm-io/wlan` in `dts/houji.dts`, wpa_supplicant and its launchd job, then `wifi=5`.
7. Carry the houji ath12k quirks over where the newer runtime still needs them.

Risks: SMMU ownership shared with UFS and the GPU, 32-bit DMA space, MSI delivery, SM8650 rail and clock order, board data packaging.
