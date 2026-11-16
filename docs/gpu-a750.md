# houji GPU (Adreno 750) on iOS 27: board facts

The port reuses UniXNU Adreno740 and AdrenoMetal (verified on sheng, SM8550 / A740) with houji board inputs. This page records the
facts the board layer is built from, each with its source. Hardware observations are added as they are made.

## Blocks (this phone's sm8650.dtsi)

| Block | Value |
|---|---|
| GPU | `qcom,adreno-43051401` (A750). `kgsl_3d0_reg_memory` 0x03d00000 + 0x40000, `cx_mem` 0x03d9e000 + 0x2000, `cx_dbgc` 0x03d61000 + 0x800. IRQ GIC SPI 300 |
| GMU | `qcom,adreno-gmu-750.1`. `gmu` 0x03d6a000 + 0x35000 (with `rscc`, `gmu_pdc`). IRQ GIC SPI 304 |
| GPU SMMU | `qcom,sm8650-smmu-500`, `qcom,adreno-smmu` at 0x03da0000 + 0x40000. GPU streams 0 and 1, GMU stream 5 (as sheng) |
| GPU clock controller | `qcom,sm8650-gpucc` at 0x03d90000 |
| ZAP shader | `memory-region = <&gpu_micro_code_mem>` |
| Interconnect | `MASTER_GFX3D` -> `SLAVE_EBI1` (`gfx-mem`) |

## Clock and power registers (Linux drivers/clk/qcom/gpucc-sm8650.c, gcc-sm8650.c)

Compared with sheng's `package/GpuPowerBoard.h` (SM8550):

| Register | SM8650 | SM8550 (sheng) |
|---|---|---|
| `gpu_cx_gdsc` GDSCR | 0x9108 | 0x9108 |
| `gpu_cx_gdsc` gds_hw_ctrl | **0x9168** | 0x953c |
| `gpu_gx_gdsc` GDSCR | 0x905c (clamp_io_ctrl 0x9504) | 0x905c |
| `gpu_cc_ahb_clk` | 0x911c | 0x911c |
| `gpu_cc_cx_gmu_clk` | 0x913c | 0x913c |
| `gpu_cc_cxo_clk` | 0x9144 | 0x9144 |
| `gpu_cc_hlos1_vote_gpu_smmu_clk` | 0x7000 | 0x7000 |
| `gpu_cc_pll1` | 0x1000 (Lucid OLE) | 0x1000 |
| `gpu_cc_gmu_clk_src` RCGR | 0x9318 (19.2 / 260 / 625 MHz) | 0x9318 |
| `gpu_cc_hub_clk_src` RCGR | 0x93ec | 0x93ec |
| GCC `gpu_memnoc_gfx_clk` | 0x71010 | 0x71010 |
| GCC `gpu_snoc_dvm_gfx_clk` | 0x71018 | 0x71018 |
| GCC `gpu_gpll0_clk_src` / `_div` | vote 0x52000 bits 15 / 16 | same |
| GCC `gpu_cfg_ahb_clk` | 0x71004, a critical CBCR enabled at probe | 0x71004, enabled at probe |
| `gpu_cc_pll0` L / alpha | **0x20 / 0x4aaa** | 0x1e / 0xbaaa |
| `gpu_cc_pll1` L / alpha | **0x1b / 0x1555** (520 MHz; GMU 260 MHz = PLL1 / 2) | 0x16 / 0xeaaa (440 MHz; GMU 220 MHz) |

## Firmware (this phone's vendor and odm partitions; local only, never committed)

| Image | Size | SHA-256 (first 16) |
|---|---|---|
| `vendor/firmware/gen70900_sqe.fw` | 85300 | 8e2c5ce74140de7e |
| `vendor/firmware/gen70900_aqe.fw` | 33176 | 25237c161d727b8e |
| `vendor/firmware/gmu_gen70900.bin` | 78588 | 9685f62c42543bef |
| `odm/firmware/gen70900_zap.mbn` | 12088 | 53597ba96c8eb272 |

Adreno740 takes the GMU, SQE and ZAP images as embedded archive symbols (`native_adreno_firmware_image`, `native_adreno_sqe_firmware_image`,
`native_adreno_zap_image`; weak empty defaults refuse to run). Linux's A750 entry loads only SQE and GMU (plus the ZAP shader); the AQE
file is not used. Pins (sizes and hashes only): `scripts/houji-gpu-firmware.json`.

## GMU vote inputs (scripts/prepare-houji-gmu-votes.py)

Generated from this phone's mainline DTB (`sm8650-xiaomi-houji.dtb`, SHA-256 78aca71b65eac87b…, the SteamOS houji build) and Linux
`drivers/gpu/drm/msm/adreno/a7xx_catalog.c` (SHA-256 6941242f183b7614…, entry `ADRENO_CHIP_IDS(0x43051401)`: family
ADRENO_7XX_GEN3, gmu_chipid 0x7090100, gmem 3 MiB). The packed iOS 27 DTB is the stock vendor tree, whose KGSL nodes
(`qcom,kgsl-3d0`, `qcom,gen7-gmu` at 0x3d69000) the A740 driver does not read, so the mainline values are the board input.

| Output | Value |
|---|---|
| GPU OPPs | 11: 231, 310, 366, 422, 500, 578, 629, 680, 720, 770, 834 MHz; RPMh levels 52 ... 320; ACD on all |
| GMU OPPs | 260 MHz (level 64), 625 MHz (level 128) |
| Bandwidths | 7 steps, 2136718 ... 14398437 kBps |
| BCM policy | SH0 buswidth 16, MC0 buswidth 4, ACV fixed perfmode BIT(2) 10687500 kBps |
| Command DB | 0x81c60000 + 131072 (`qcom,cmd-db`) |

## Build

- `ios27/gpu-inputs.sh` (local) runs the three generators and copies the headers and the pinned firmware to the build Mac's
  `~/b/houji-gpu` (`UNIXNU_HOUJI_GPU_INPUTS`). Nothing from there enters Git.
- `board.toml` archives: `build-houji-adreno.py inputs` (headers into `build/sheng-gmu`, `build/sheng-gpu-smmu`), `component:Adreno740`,
  `build-houji-adreno.py kits` (GMU/HFI/SMMU/PM4 kits and firmware into `libAdreno740.a`). `CONFIG_XNU_NATIVE_ADRENO=y` links it.
- `package/GpuPowerProbe.cpp`, `GpuPowerBoard.h`, `GpuMemoryPool.cpp`, `ShengGmuVotes.c`: sheng 6880068 with the A750 chip id and the
  SM8650 clock deltas; built by `build-houji-display.py` and started from `LegacyDisplayProbe.cpp` only with `gpu-power=<1..9>`.
  Each result row is also logged as `HOUJI GPU: <row> <text>`.

## Prior work on this phone

The iOS 16 port (fork/kernel/overlay/iokit/Kernel/houji-ufs-root) reached bounded GPU writeback, batched submission, cache coherency
and a write-combining pool on the A750, with colour errors and stalled animations remaining (fork/docs/roadmap.md row 5). Its
`GpuGen7Table.h`/`GpuGen7Config.h` (register table), GMU, ZAP, HFI and factory OPP files are the A750 reference for the driver deltas.

## Steps

1. Board layer: houji `GpuPowerBoard.h` from sheng's with the deltas above; GMU RPMh votes through `QcomRpmh` (already used for touch).
2. Firmware archive with the four images; AQE slot in the driver.
3. A750 deltas in Adreno740: chip id 0x43051401, gen7_9 hardware-init table, GMU 750.1 / HFI.
4. Device checks in order: GMU boot, CP init, one retired ring submission, fences, GPU scanout copy.
5. AdrenoMetal and the user-space Turnip (A750 support in the local Mesa checkout) / AIR pieces.
