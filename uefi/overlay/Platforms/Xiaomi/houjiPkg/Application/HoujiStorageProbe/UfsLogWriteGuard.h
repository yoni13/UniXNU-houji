/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Explicit r21 logfs-only extension. Never used by --ufs-readonly.
 * Absolute physical LUN 0 LBAs, not partition-relative LBAs.
 */
#define HOUJI_LOGFS_FIRST 0x1000U
#define HOUJI_LOGFS_END   0x1800U

STATIC UINT32
UfsRequestLe32 (CONST UINT8 *P)
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) |
    ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC BOOLEAN
UfsLogWriteAllowed (CONST UINT8 *R, UINTN Bytes, BOOLEAN Armed)
{
  if (!Armed || !R || Bytes < 0xC8 || R[0x48] != 1 || R[0x4A] != 0 ||
      R[5] || R[0x6C]) { return FALSE; }
  CONST UINT8 *C = R + 0x5C;
  UINT32 Length = UfsRequestLe32 (R + 0x88);
  if (Length != UfsRequestLe32 (R + 0x54)) { return FALSE; }
  if (C[0] == 0x35) {
    // Flush only the device cache. This introduces no new data write range.
    if (R[1] || R[0x49] || Length) { return FALSE; }
    for (UINTN I = 1; I < 16; I++) { if (C[I]) { return FALSE; } }
    return TRUE;
  }
  if (C[0] != 0x2A || R[1] != 1 || R[0x49] != 0x20 ||
      (C[1] != 0 && C[1] != 8) || C[6] || C[9]) { return FALSE; }
  for (UINTN I = 10; I < 16; I++) { if (C[I]) { return FALSE; } }
  UINT32 Lba = ((UINT32)C[2] << 24) | ((UINT32)C[3] << 16) |
    ((UINT32)C[4] << 8) | C[5];
  UINT32 Blocks = ((UINT32)C[7] << 8) | C[8];
  return Lba >= HOUJI_LOGFS_FIRST && Lba < HOUJI_LOGFS_END &&
    Blocks > 0 && Blocks <= HOUJI_LOGFS_END - Lba && Length == Blocks * 4096U;
}

// Standalone self-boot only. The installed image arms the exact no-wipe
// recovery request itself (boot-recovery/recovery) instead of requiring a
// host-side dd to misc. Permitted traffic is bounded to one 4096-byte block
// write at absolute LBA 1024 plus the device cache flush, and only while the
// single arming call in RunUfsReadonly holds mBcbArmWritesArmed.
STATIC BOOLEAN
UfsBcbArmWriteAllowed (CONST UINT8 *R, UINTN Bytes, BOOLEAN Armed)
{
  if (!Armed || !R || Bytes < 0xC8 || R[0x48] != 1 || R[0x4A] != 0 ||
      R[5] || R[0x6C]) { return FALSE; }
  CONST UINT8 *C = R + 0x5C;
  UINT32 Length = UfsRequestLe32 (R + 0x88);
  if (Length != UfsRequestLe32 (R + 0x54)) { return FALSE; }
  if (C[0] == 0x35) {
    // Flush only the device cache; this introduces no new data write range.
    if (R[1] || R[0x49] || Length) { return FALSE; }
    for (UINTN I = 1; I < 16; I++) { if (C[I]) { return FALSE; } }
    return TRUE;
  }
  if (C[0] != 0x2A || R[1] != 1 || R[0x49] != 0x20 ||
      (C[1] != 0 && C[1] != 8) || C[6] || C[9]) { return FALSE; }
  for (UINTN I = 10; I < 16; I++) { if (C[I]) { return FALSE; } }
  UINT32 Lba = ((UINT32)C[2] << 24) | ((UINT32)C[3] << 16) |
    ((UINT32)C[4] << 8) | C[5];
  UINT32 Blocks = ((UINT32)C[7] << 8) | C[8];
  return Lba == 1024 && Blocks == 1 && Length == 4096U;
}
