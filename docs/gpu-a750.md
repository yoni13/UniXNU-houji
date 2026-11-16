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
| GCC `gpu_cfg_ahb_clk` | not a separate clock (always on) | 0x71004 |

## Firmware (this phone's vendor and odm partitions; local only, never committed)

| Image | Size | SHA-256 (first 16) |
|---|---|---|
| `vendor/firmware/gen70900_sqe.fw` | 85300 | 8e2c5ce74140de7e |
| `vendor/firmware/gen70900_aqe.fw` | 33176 | 25237c161d727b8e |
| `vendor/firmware/gmu_gen70900.bin` | 78588 | 9685f62c42543bef |
| `odm/firmware/gen70900_zap.mbn` | 12088 | 53597ba96c8eb272 |

Adreno740 takes the GMU and SQE images as embedded archive symbols (`native_adreno_firmware_image`, `native_adreno_sqe_firmware_image`;
weak empty defaults refuse to run). The A750 also has the AQE microcode, which the A740 driver has no slot for yet.

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
