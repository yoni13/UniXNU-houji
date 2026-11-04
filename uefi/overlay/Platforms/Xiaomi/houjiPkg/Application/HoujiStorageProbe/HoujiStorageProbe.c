/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Diagnostic app: HOB inventory or explicitly selected OEM foundation services.
 * UFS startup requires an explicit guarded profile. r21 may create one file
 * inside the backed-up logfs only. No erase, format or GPT modification.
 */
#include <Uefi.h>
#include <PiDxe.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/Cpu.h>
#include <Protocol/UniXNULoader.h>
#include <Guid/Gpt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "GpuCallTrace.h"
#define HOUJI_CALL_TRACE(step,address,value) HoujiGpuCallTrace((step),(UINT64)(UINTN)(address),(UINT64)(value))
#include "ProbeLog.h"
#include <Protocol/EFIKernelInterface.h>
BOOLEAN EFIAPI HoujiGpuHandleReadFault(EFI_EXCEPTION_TYPE Type, EFI_SYSTEM_CONTEXT_AARCH64 *C);
BOOLEAN EFIAPI HoujiGpuHandleDoorbellFault(EFI_EXCEPTION_TYPE Type, EFI_SYSTEM_CONTEXT_AARCH64 *C);
BOOLEAN EFIAPI HoujiGpuHandleHostWriteFault(EFI_EXCEPTION_TYPE Type, EFI_SYSTEM_CONTEXT_AARCH64 *C);

extern EFI_GUID gEfiClockProtocolGuid;
extern EFI_GUID gQcomScmProtocolGuid;
extern EFI_GUID gEfiPlatformInfoProtocolGuid;
extern EFI_GUID gEfiUsbfnIoProtocolGuid;
extern EFI_GUID gEfiKernelProtocolGuid;
extern EFI_GUID gEfiSchedulerInterfaceHobGuid;
extern EFI_GUID gEfiSMEMProtocolGuid;
extern EFI_GUID gEfiDxeServicesTableGuid;

typedef struct {
  CONST CHAR16 *Name;
  CONST CHAR16 *Path;
  EFI_GUID *Dependencies;
  UINTN DependencyCount;
  EFI_GUID *Provides;
} OEM_SERVICE;

#include "OemServices.inc"

// Provider GUIDs confirmed at each OEM InstallMultipleProtocolInterfaces call.
STATIC EFI_GUID mCmdDb = {0x1c34f691, 0xd33d, 0x4e14, {0xb8, 0xf8, 0x32, 0xc6, 0xc0, 0x29, 0xe9, 0x5b}};
STATIC EFI_GUID mPwrUtils = {0x3c50e0f7, 0x7d40, 0x4a86, {0x90, 0x67, 0x11, 0xa2, 0x37, 0x2b, 0xfc, 0x3d}};
STATIC EFI_GUID mNpa = {0x79d6c870, 0x725e, 0x489e, {0xa0, 0xa1, 0x27, 0xe7, 0xa5, 0xd0, 0xcb, 0x35}};
STATIC UINT64 mClockImageBase, mClockImageBytes;
STATIC BOOLEAN mClockTraceEnabled, mClockTraceSelfTestPassed;
STATIC BOOLEAN mClockStorageOnly;
STATIC BOOLEAN mStorageServicesRun;
STATIC BOOLEAN mUfsReadonlyRun, mUfsGuardEnabled, mUfsGuardSelfTestPassed;
STATIC UINTN mUfsAllowed, mUfsDenied;
STATIC BOOLEAN mLogFsTestRun, mLogFsWritesArmed, mUfsHandoffRun, mXnuUfsRun, mBcbArmWritesArmed;
STATIC UINTN mLogFsWriteCommands, mLogFsFlushCommands;
STATIC UINT64 mStorageImageBase, mStorageImageBytes;
STATIC CONST CHAR16 *mStorageDriverName = L"none";
STATIC EFI_GUID mIommuProvider = {0x54b6d3b4, 0x5d33, 0x4f91, {0x86, 0x00, 0x6c, 0x41, 0xd5, 0xde, 0xb1, 0x9a}};
STATIC EFI_GUID mIcbProvider = {0x5824f9de, 0x17d2, 0x4b1f, {0xa5, 0xfe, 0x5e, 0xb7, 0x7f, 0xa5, 0x30, 0x93}};
STATIC UINTN mClockTraceHits;

#include "ClockTraceSites.inc"
#include "ClockTraceStep.h"
#include "ScmTraceSites.inc"
#include "ClockStoragePolicy.h"
#include "UfsCommandGuard.h"
#include "UfsLogWriteGuard.h"
#include "UfsReadyRetry.h"
#include "UfsHandoffProbe.h"
STATIC UINTN mClockTraceSiteHits[ARRAY_SIZE (mClockTraceSites)];
STATIC BOOLEAN mScmTraceActive, mScmTraceSelfTestPassed, mScmTraceModuleVerified, mScmTraceStackVerified;
STATIC UINT64 mScmTraceImageBase, mScmTraceImageBytes, mScmTraceEntrySp;
STATIC UINT64 mScmTraceStackLow, mScmTraceStackHigh;
typedef struct {
  UINT64 Pc, Sp, Lr, Requested, Result, Pointer56, Pointer8192;
  UINT32 Seen, Flags;
} SCM_TRACE_SNAPSHOT;
STATIC SCM_TRACE_SNAPSHOT mScmTraceSnapshots[ARRAY_SIZE (mScmTraceSites)];

STATIC VOID
UfsRetryReport (UINTN Stage, INT32 Status)
{
  if (Stage == 0) { Print (L"HOUJI UFS ACTIVE: exact NOT READY 02/04/02; trying original SSU Active\n"); }
  else if (Stage == 1) { Print (L"HOUJI UFS ACTIVE: original power command returned %d\n", Status); }
  else { Print (L"HOUJI UFS ACTIVE: original ready retry returned %d\n", Status); }
}

STATIC INT32 EFIAPI
UfsReadyWithActive (UINTN Handle)
{
  // Runs on the normal caller stack, never inside the exception handler.
  if (!UfsReadyHandleValid (mStorageImageBase, mStorageImageBytes, Handle)) { return -22; }
  return UfsReadyRetry (Handle, (CONST UINT8 *)(UINTN)(mStorageImageBase + 0x22AC0 + 0x478),
    (HOUJI_UFS_READY_FN)(UINTN)(mStorageImageBase + 0xCA4C),
    (HOUJI_UFS_POWER_FN)(UINTN)(mStorageImageBase + 0xCB78), UfsRetryReport);
}

// The first instruction is checked before calling this naked diagnostic stub.
// The handler resumes at RET, proving that ELR/LR context restoration works.
STATIC VOID __attribute__((naked))
ClockTraceSelfTest (VOID)
{
  __asm__ volatile ("brk #0x484b\n\tret");
}

// Only exact diagnostic MOV/ADRP sites in this pinned, still-loaded SCM PE.
// This path does not format output, allocate, call BS or read hardware.
STATIC BOOLEAN
HandleScmTrace (EFI_EXCEPTION_TYPE Type, EFI_SYSTEM_CONTEXT_AARCH64 *C)
{
  if (!mScmTraceActive || !mScmTraceModuleVerified || !C || Type != EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS ||
      ((C->ESR >> 26) & 0x3F) != 0x3C) { return FALSE; }
  if (C->ELR == (UINTN)ClockTraceSelfTest && (C->ESR & 0xFFFF) == 0x484B &&
      *(CONST UINT32 *)(UINTN)C->ELR == 0xD4290960U) {
    mScmTraceSelfTestPassed = TRUE;
    C->ELR += 4;
    return TRUE;
  }
  if (!mScmTraceSelfTestPassed || (C->ESR & 0xFFFF) != HOUJI_SCM_TRACE_ISS ||
      mScmTraceImageBytes != HOUJI_SCM_TRACE_BYTES ||
      C->ELR < mScmTraceImageBase || C->ELR - mScmTraceImageBase >= mScmTraceImageBytes) { return FALSE; }
  for (UINTN I = 0; I < ARRAY_SIZE (mScmTraceSites); I++) {
    CONST SCM_TRACE_SITE *Site = &mScmTraceSites[I];
    if (C->ELR - mScmTraceImageBase != Site->Rva) { continue; }
    UINT64 Value;
    if (mScmTraceSnapshots[I].Seen || *(CONST UINT32 *)(UINTN)C->ELR != HOUJI_SCM_TRACE_BRK ||
        !ScmTraceOriginalValue (I, mScmTraceImageBase, C->ELR, C->X0, &Value)) { return FALSE; }
    SCM_TRACE_SNAPSHOT *S = &mScmTraceSnapshots[I];
    S->Pc = C->ELR; S->Sp = C->SP; S->Lr = C->LR; S->Result = C->X0;
    // The precise frame is on the caller's existing CPU stack. A failed
    // diagnostic bound merely leaves size invalid; it never reads that word.
    BOOLEAN Stack = mScmTraceStackVerified && C->SP >= mScmTraceStackLow &&
      C->SP <= mScmTraceStackHigh - 16 && !(C->SP & 15) &&
      C->SP <= mScmTraceEntrySp && mScmTraceEntrySp - C->SP <= 0x10000;
    if (Stack && Site->Sample != 0 && Site->Sample != 3 && Site->Sample != 6) {
      S->Requested = *(CONST UINT64 *)(UINTN)(C->SP + 8); S->Flags |= 1;
    } else if (Site->Sample == 0) {
      S->Requested = 56; S->Flags |= 2;
    } else if (Site->Sample == 3) {
      S->Requested = C->X8; S->Flags |= 4; // Original STR SP+8 still runs next.
    }
    S->Pointer56 = *(CONST UINT64 *)(UINTN)(mScmTraceImageBase + 0xB270);
    S->Pointer8192 = *(CONST UINT64 *)(UINTN)(mScmTraceImageBase + 0xB278);
    if (Site->Sample == 2) { S->Flags |= 0x100; } // X0 is a pointer, not EFI_STATUS.
    if (Site->Sample == 5) { S->Flags |= 0x200; } // Actual EFI_STATUS return.
    switch (Site->Register) {
      case 2: C->X2 = Value; break;
      case 8: C->X8 = Value; break;
      case 9: C->X9 = Value; break;
      case 19: C->X19 = Value; break;
      case 22: C->X22 = Value; break;
      default: return FALSE;
    }
    C->ELR += 4; S->Seen = 1;
    HOUJI_CALL_TRACE (0x8080 + I, S->Requested, S->Flags);
    HOUJI_CALL_TRACE (0x8090 + I, S->Pointer56, S->Pointer8192);
    HOUJI_CALL_TRACE (0x80A0 + I, S->Sp, S->Lr);
    HOUJI_CALL_TRACE (0x8000 + I, S->Pc,
      (Site->Sample == 2 || Site->Sample == 5) ? S->Result : S->Requested);
    return TRUE;
  }
  return FALSE;
}

STATIC BOOLEAN
HandleClockTrace (EFI_EXCEPTION_TYPE Type, EFI_SYSTEM_CONTEXT_AARCH64 *C)
{
  if (!mClockTraceEnabled || Type != EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS ||
      ((C->ESR >> 26) & 0x3F) != 0x3C) { return FALSE; }
  if (C->ELR == (UINTN)ClockTraceSelfTest && (C->ESR & 0xFFFF) == 0x484B) {
    mClockTraceSelfTestPassed = TRUE;
    C->ELR += 4;
    return TRUE;
  }
  if (!mClockTraceSelfTestPassed || (C->ESR & 0xFFFF) != HOUJI_CLOCK_TRACE_ISS ||
      C->ELR < mClockImageBase || C->ELR - mClockImageBase >= mClockImageBytes) { return FALSE; }
  for (UINTN I = 0; I < ARRAY_SIZE (mClockTraceSites); I++) {
    CONST CLOCK_TRACE_SITE *Site = &mClockTraceSites[I];
    if (C->ELR - mClockImageBase != Site->Rva) { continue; }
    UINT64 Next, Lr = C->LR;
    if (*(CONST UINT32 *)(UINTN)C->ELR != HOUJI_CLOCK_TRACE_BRK ||
        !ClockTraceStep (Site->Opcode, C->ELR, C->X0, C->X8, &Next, &Lr)) { return FALSE; }
    ++mClockTraceHits;
    ++mClockTraceSiteHits[I];
    C->ELR = Next;
    C->LR = Lr;
    // Do not print every iteration of the driver's bounded polling loop.
    // Each first delay/read call is visible; every original instruction runs.
    if ((Site->Rva == 0x53B4 || Site->Rva == 0x53BC) && mClockTraceSiteHits[I] > 1) { return TRUE; }
    CHAR8 Text[224];
    UINTN Length = AsciiSPrint (Text, sizeof (Text),
      "HOUJI TRACE %u: %a %a +%05x x0=%lx x1=%lx\n",
      mClockTraceHits, Site->Name, Site->IsReturn ? "END" : "BEGIN", Site->Rva,
      C->X0, C->X1);
    // BL/BLR and CBZ/CBNZ only affect control flow (and BL/BLR's LR).
    // Preserve every input/result register, flag, stack and original branch.
    SerialPortWrite ((UINT8 *)Text, Length);
    if (!Site->IsReturn && (Site->Rva == 0xA620 || Site->Rva == 0xA6B4) &&
        C->X1 >= mClockImageBase && C->X1 - mClockImageBase < mClockImageBytes) {
      // Only inspect a bounded ASCII string inside this loaded PE, never an
      // arbitrary pointer or peripheral. Lookup X1 is a static clock name.
      CHAR8 Name[48];
      UINTN N = 0;
      while (N < sizeof (Name) - 1 && C->X1 - mClockImageBase + N < mClockImageBytes) {
        CHAR8 Ch = *(CONST CHAR8 *)(UINTN)(C->X1 + N);
        if (!Ch) { break; }
        if (Ch < 32 || Ch > 126) { N = 0; break; }
        Name[N++] = Ch;
      }
      Name[N] = 0;
      if (N) {
        Length = AsciiSPrint (Text, sizeof (Text), "HOUJI CLOCK NAME: %a\n", Name);
        SerialPortWrite ((UINT8 *)Text, Length);
      }
    }
    return TRUE;
  }
  return FALSE;
}

STATIC BOOLEAN
ValidateClockCode (VOID)
{
  if (!mClockImageBase || mClockImageBytes != 0x38000) { return FALSE; }
  for (UINTN I = 0; I < ARRAY_SIZE (mClockTraceSites); I++) {
    CONST CLOCK_TRACE_SITE *Site = &mClockTraceSites[I];
    if (Site->Rva > mClockImageBytes - sizeof (UINT32)) { return FALSE; }
    UINT32 Word = *(CONST UINT32 *)(UINTN)(mClockImageBase + Site->Rva);
    if (Word != (mClockTraceEnabled ? HOUJI_CLOCK_TRACE_BRK : Site->Opcode)) { return FALSE; }
  }
  Print (L"HOUJI CLOCK: %s code profile verified at all %u sites\n",
    mClockTraceEnabled ? L"diagnostic trace" : L"original", ARRAY_SIZE (mClockTraceSites));
  return TRUE;
}

STATIC VOID EFIAPI
ClockException (EFI_EXCEPTION_TYPE Type, EFI_SYSTEM_CONTEXT Context)
{
  EFI_SYSTEM_CONTEXT_AARCH64 *C = Context.SystemContextAArch64;
  if (HandleClockTrace (Type, C)) { return; }
  CHAR8 Text[320];
  BOOLEAN InImage = C->ELR >= mClockImageBase && C->ELR - mClockImageBase < mClockImageBytes;
  UINTN Length = AsciiSPrint (Text, sizeof (Text),
    "\nHOUJI CLOCK EXCEPTION: type=%u PC=%016lx ESR=%016lx FAR=%016lx\n"
    "HOUJI CLOCK IMAGE: base=%016lx in-image=%u offset=%lx LR=%016lx\n",
    (UINTN)Type, C->ELR, C->ESR, C->FAR, mClockImageBase, (UINTN)InImage,
    InImage ? C->ELR - mClockImageBase : 0, C->LR);
  // Use the firmware's direct framebuffer serial path, with no pool or BS
  // calls from an exception. Never resume a faulting clock initialization.
  SerialPortWrite ((UINT8 *)Text, Length);
  CpuDeadLoop ();
}

STATIC VOID EFIAPI
StorageException (EFI_EXCEPTION_TYPE Type, EFI_SYSTEM_CONTEXT Context)
{
  EFI_SYSTEM_CONTEXT_AARCH64 *C = Context.SystemContextAArch64;
  if (HandleScmTrace (Type, C)) { return; }
  if(C) {
    HOUJI_CALL_TRACE(0x7000,C->ELR,C->ESR);
    HOUJI_CALL_TRACE(0x7001,C->FAR,C->LR);
  }
  if (HoujiGpuHandleDoorbellFault (Type, C)) { return; }
  if (HoujiGpuHandleHostWriteFault (Type, C)) { return; }
  if (HoujiGpuHandleReadFault (Type, C)) { return; }
  if (HandleHandoffReadFault (Type, C)) { return; }
  if (Type == EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS &&
      ((C->ESR >> 26) & 0x3F) == 0x3C && mUfsGuardEnabled) {
    if (C->ELR == (UINTN)ClockTraceSelfTest && (C->ESR & 0xFFFF) == 0x484B) {
      mUfsGuardSelfTestPassed = TRUE;
      C->ELR += 4;
      return;
    }
    if (mUfsGuardSelfTestPassed && (C->ESR & 0xFFFF) == 0x4852 &&
        C->ELR == mStorageImageBase + 0xB008 &&
        *(CONST UINT32 *)(UINTN)C->ELR == 0xD4290A40U &&
        UfsReadyHandleValid (mStorageImageBase, mStorageImageBytes, C->X0)) {
      // Emulate the original BL's LR, redirecting to a normal-context wrapper.
      // The wrapper still calls the original TUR and preserves real failures.
      C->LR = C->ELR + 4;
      C->ELR = (UINTN)UfsReadyWithActive;
      return;
    }
    if (mUfsGuardSelfTestPassed && (C->ESR & 0xFFFF) == HOUJI_UFS_GUARD_ISS &&
        C->ELR == mStorageImageBase + HOUJI_UFS_GUARD_RVA &&
        *(CONST UINT32 *)(UINTN)C->ELR == HOUJI_UFS_GUARD_BRK) {
      // All six pinned synchronous callers place a 0xc8-byte request at
      // caller SP or SP+0x20. The real callee has already reserved 0xe0.
      BOOLEAN Stack = (C->SP >= 0xB8000000 && C->SP <= 0xD3A00000 - 0x1000) ||
        (C->SP >= 0xA760D000 && C->SP <= 0xA764D000 - 0x1000);
      BOOLEAN Bounded = Stack && !(C->X1 & 7) &&
        C->X1 >= C->SP + 0xE0 && C->X1 <= C->SP + 0x120;
      CONST UINT8 *Request = (CONST UINT8 *)(UINTN)C->X1;
      BOOLEAN LogWrite = Bounded && UfsLogWriteAllowed (Request, 0xC8, mLogFsWritesArmed);
      BOOLEAN BcbWrite = Bounded && UfsBcbArmWriteAllowed (Request, 0xC8, mBcbArmWritesArmed);
      BOOLEAN Allowed = Bounded && (UfsCommandAllowed (Request, 0xC8) || LogWrite || BcbWrite);
      if (LogWrite) {
        if (Request[0x5C] == 0x2A) { mLogFsWriteCommands++; }
        else { mLogFsFlushCommands++; }
      }
      if (Allowed) { mUfsAllowed++; } else { mUfsDenied++; }
      if (!Allowed || mUfsAllowed <= 24 || (LogWrite && mLogFsWriteCommands <= 8)) {
        CHAR8 Message[160];
        UINTN Length = AsciiSPrint (Message, sizeof (Message),
          "HOUJI UFS CMD: %a type=%02x op=%02x id/lun=%02x/%02x bounded=%u\n",
          Allowed ? "ALLOW" : "DENY", Bounded ? Request[0x48] : 0xFF,
          Bounded ? Request[0x5C] : 0xFF, Bounded ? Request[0x5D] : 0xFF,
          Bounded ? Request[0x4A] : 0xFF, (UINTN)Bounded);
        SerialPortWrite ((UINT8 *)Message, Length);
      }
      UfsGuardResume (Allowed, Request, mStorageImageBase, &C->X0, &C->X8, &C->ELR);
      return;
    }
  }
  HoujiGpuCallTraceFatal(C->ELR,C->ESR,C->FAR,C->LR);
  BOOLEAN InImage = C->ELR >= mStorageImageBase && C->ELR - mStorageImageBase < mStorageImageBytes;
  CHAR8 Text[320];
  UINTN Length = AsciiSPrint (Text, sizeof (Text),
    "\nHOUJI STORAGE EXCEPTION: %s type=%u PC=%016lx ESR=%016lx FAR=%016lx\n"
    "HOUJI STORAGE IMAGE: base=%016lx in-image=%u offset=%lx LR=%016lx\n",
    mStorageDriverName, (UINTN)Type, C->ELR, C->ESR, C->FAR,
    mStorageImageBase, (UINTN)InImage, InImage ? C->ELR - mStorageImageBase : 0, C->LR);
  SerialPortWrite ((UINT8 *)Text, Length);
  HoujiGpuCallTraceFatal(C->ELR,C->ESR,C->FAR,C->LR);
  CpuDeadLoop ();
}

typedef struct {
  CONST CHAR16 *Name;
  UINT64 Base;
  UINT64 Length;
} CLOCK_BANK;

// All eight original houji ClockBSP base records (RVA 0x20280, stride 0x18).
// Linux FDT children alone do not cover the parent requested by DAL HWIO.
STATIC CONST CLOCK_BANK mClockBanks[] = {
  {L"APSS_HM",      0x17000000, 0x2000000},
  {L"CLK_CTL",      0x00100000, 0x0200000},
  {L"CORE_TOP_CSR", 0x01F00000, 0x0100000},
  {L"GFX",          0x03D00000, 0x0100000},
  {L"IRIS",         0x0AA00000, 0x0200000},
  {L"MDSS",         0x0AE00000, 0x0200000},
  {L"PCIE_RSCC",    0x01D00000, 0x0015000},
  {L"TITAN_SS",     0x0AC00000, 0x0200000},
};

STATIC BOOLEAN
MappingCheck (CONST CLOCK_BANK *Banks, UINTN BankCount, CONST CHAR16 *Name)
{
  UINT64 El, SavedPar;
  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (El));
  Print (L"HOUJI MAP: %s address translation check; CurrentEL=%lx\n", Name, El);
  Print (L"HOUJI MAP: no peripheral loads/stores in this check\n");
  if (El != 4) { Print (L"HOUJI MAP: STOP; expected EL1\n"); return FALSE; }
  EFI_DXE_SERVICES *Dxe = NULL;
  EFI_STATUS Table = EfiGetSystemConfigurationTable (&gEfiDxeServicesTableGuid, (VOID **)&Dxe);
  if (EFI_ERROR (Table) || !Dxe) {
    Print (L"HOUJI MAP: STOP; DXE services unavailable (%r)\n", Table);
    return FALSE;
  }
  __asm__ volatile ("mrs %0, par_el1" : "=r" (SavedPar));
  UINTN Faults = 0, NonIdentity = 0, WrongType = 0, Checked = 0;
  for (UINTN N = 0; N < BankCount; N++) {
    CONST CLOCK_BANK *Bank = &Banks[N];
    UINTN BeforeFaults = Faults, BeforeIdentity = NonIdentity, BeforeType = WrongType;
    BOOLEAN Reported = FALSE;
    for (UINT64 Address = Bank->Base; Address < Bank->Base + Bank->Length; Address += EFI_PAGE_SIZE) {
      UINT64 ReadPar, WritePar;
      // AT performs translation checks. It does not read the peripheral.
      __asm__ volatile ("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r" (ReadPar) : "r" (Address) : "memory");
      __asm__ volatile ("at s1e1w, %1\n\tisb\n\tmrs %0, par_el1" : "=r" (WritePar) : "r" (Address) : "memory");
      BOOLEAN ReadOk = !(ReadPar & 1), WriteOk = !(WritePar & 1);
      if (!ReadOk || !WriteOk) { Faults++; }
      BOOLEAN Identity = (!ReadOk || (ReadPar & 0x0000FFFFFFFFF000ULL) == Address) &&
                         (!WriteOk || (WritePar & 0x0000FFFFFFFFF000ULL) == Address);
      if (!Identity) { NonIdentity++; }
      EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor = {0};
      EFI_STATUS Gcd = Dxe->GetMemorySpaceDescriptor (Address, &Descriptor);
      BOOLEAN Device = !EFI_ERROR (Gcd) && Descriptor.GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo &&
                       (!ReadOk || (ReadPar >> 56) <= 0x0C) && (!WriteOk || (WritePar >> 56) <= 0x0C);
      if (!Device) { WrongType++; }
      if ((!ReadOk || !WriteOk || !Identity || !Device) && !Reported) {
        Print (L"HOUJI MAP: %s first bad VA=%08lx PAR R=%016lx W=%016lx GCD=%r type=%u\n",
          Bank->Name, Address, ReadPar, WritePar, Gcd, (UINTN)Descriptor.GcdMemoryType);
        Reported = TRUE;
      }
      Checked++;
    }
    Print (L"HOUJI MAP: %s %08lx+%lx pages=%u %s\n", Bank->Name, Bank->Base, Bank->Length,
      (UINTN)(Bank->Length / EFI_PAGE_SIZE),
      Faults == BeforeFaults && NonIdentity == BeforeIdentity && WrongType == BeforeType ? L"PASS" : L"FAIL");
  }
  __asm__ volatile ("msr par_el1, %0\n\tisb" :: "r" (SavedPar) : "memory");
  Print (L"HOUJI MAP: end; fault-pages=%u nonidentity-pages=%u wrong-type=%u\n", Faults, NonIdentity, WrongType);
  Print (L"HOUJI MAP: checked all %u pages in %u %s windows\n", Checked, BankCount, Name);
  Print (L"HOUJI MAP: check itself made no mapping or peripheral changes\n");
  return !Faults && !NonIdentity && !WrongType;
}

STATIC BOOLEAN
ClockMappingCheck (VOID)
{
  return MappingCheck (mClockBanks, ARRAY_SIZE (mClockBanks), L"OEM ClockBSP");
}

STATIC BOOLEAN
HasProtocol (EFI_GUID *Guid, CONST CHAR16 *Name)
{
  VOID *Interface = NULL;
  EFI_STATUS Status = gBS->LocateProtocol (Guid, NULL, &Interface);
  BOOLEAN Present = !EFI_ERROR (Status) && Interface;
  Print (L"HOUJI PROTOCOL: %s %s (%r)\n", Name, Present ? L"present" : L"missing", Status);
  return Present;
}

STATIC BOOLEAN
MappedRam (EFI_PHYSICAL_ADDRESS Address, UINTN Length)
{
  UINTN Size = 0, Key, DescriptorSize;
  UINT32 Version;
  EFI_STATUS Status = gBS->GetMemoryMap (&Size, NULL, &Key, &DescriptorSize, &Version);
  if (Status != EFI_BUFFER_TOO_SMALL || !DescriptorSize || Address > MAX_UINT64 - Length) { return FALSE; }
  Size += 4 * DescriptorSize;
  EFI_MEMORY_DESCRIPTOR *Map = AllocatePool (Size);
  if (!Map) { return FALSE; }
  Status = gBS->GetMemoryMap (&Size, Map, &Key, &DescriptorSize, &Version);
  BOOLEAN Found = FALSE;
  if (!EFI_ERROR (Status)) {
    for (UINTN Offset = 0; Offset + DescriptorSize <= Size; Offset += DescriptorSize) {
      EFI_MEMORY_DESCRIPTOR *D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if (D->Type > EfiConventionalMemory || D->NumberOfPages > (MAX_UINT64 - D->PhysicalStart) / EFI_PAGE_SIZE) { continue; }
      UINT64 End = D->PhysicalStart + D->NumberOfPages * EFI_PAGE_SIZE;
      if (Address >= D->PhysicalStart && Address + Length <= End) { Found = TRUE; break; }
    }
  }
  FreePool (Map);
  return Found;
}

STATIC BOOLEAN
HasScheduler (VOID)
{
  EFI_KERNEL_PROTOCOL *Scheduler = NULL;
  EFI_STATUS Status = gBS->LocateProtocol (&gEfiKernelProtocolGuid, NULL, (VOID **)&Scheduler);
  if (EFI_ERROR (Status) || !Scheduler) {
    Print (L"HOUJI STEP: locating Scheduler HOB\n");
    VOID *Hob = GetFirstGuidHob (&gEfiSchedulerInterfaceHobGuid);
    if (!Hob || GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (UINT64)) {
      Print (L"HOUJI OEM: Scheduler HOB absent or wrong size\n");
      return FALSE;
    }
    UINT64 Address = ReadUnaligned64 (GET_GUID_HOB_DATA (Hob));
    Print (L"HOUJI STEP: HOB pointer=%lx; checking RAM map\n", Address);
    if (!Address || (Address & 7) || !MappedRam (Address, sizeof (*Scheduler))) {
      Print (L"HOUJI OEM: Scheduler HOB pointer outside mapped RAM: %lx\n", Address);
      return FALSE;
    }
    Scheduler = (EFI_KERNEL_PROTOCOL *)(UINTN)Address;
    Print (L"HOUJI OEM: using real Scheduler HOB at %lx\n", Address);
  } else if (!MappedRam ((UINTN)Scheduler, sizeof (*Scheduler))) { return FALSE; }
  Print (L"HOUJI STEP: reading Scheduler version and lock pointer\n");
  Print (L"HOUJI STEP: version=%lx lock=%lx\n", Scheduler->Version, (UINTN)Scheduler->Lock);
  if ((Scheduler->Version >> 16) != 1 || (Scheduler->Version & 0xffff) < 5 ||
      !Scheduler->Lock || !MappedRam ((UINTN)Scheduler->Lock, sizeof (*Scheduler->Lock)) ||
      !Scheduler->Lock->InitLock || !MappedRam ((UINTN)Scheduler->Lock->InitLock, 4)) {
    Print (L"HOUJI OEM: Scheduler version/lock interface failed validation\n");
    return FALSE;
  }
  Print (L"HOUJI OEM: Scheduler version=%lx lock interface valid\n", Scheduler->Version);
  // Match the OEM SmemDxe constructor's HOB fallback. Do not install a new
  // Kernel protocol or replace any OEM function pointer.
  return TRUE;
}

// Ordinary-context AT preflight only. The exception handler never probes a
// translation, touches an unvalidated page or asks firmware for a memory map.
STATIC BOOLEAN
ScmTraceReadable (UINT64 Base, UINT64 Bytes)
{
  if (!Bytes || Base > MAX_UINT64 - (Bytes - 1)) { return FALSE; }
  UINT64 El, SavedPar, Par;
  __asm__ volatile ("mrs %0,CurrentEL" : "=r" (El));
  if (El != 4) { return FALSE; }
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  __asm__ volatile ("mrs %0,par_el1" : "=r" (SavedPar));
  BOOLEAN Good = TRUE;
  UINT64 Last = (Base + Bytes - 1) & ~4095ULL;
  for (UINT64 Page = Base & ~4095ULL;; Page += 4096) {
    __asm__ volatile ("at s1e1r,%1; isb; mrs %0,par_el1" : "=r" (Par) : "r" (Page) : "memory");
    if ((Par & 1) || (Par & 0x0000FFFFFFFFF000ULL) != Page || (Par >> 56) != 0xFF) { Good = FALSE; break; }
    if (Page == Last) { break; }
  }
  __asm__ volatile ("msr par_el1,%0; isb" :: "r" (SavedPar) : "memory");
  gBS->RestoreTPL (OldTpl);
  return Good;
}

STATIC BOOLEAN
ValidateScmTraceCode (CONST EFI_LOADED_IMAGE_PROTOCOL *Info)
{
  if (!Info || !Info->ImageBase || Info->ImageSize != HOUJI_SCM_TRACE_BYTES ||
      ((UINTN)Info->ImageBase & 4095) ||
      (UINTN)Info->ImageBase > MAX_UINT64 - HOUJI_SCM_TRACE_BYTES) { return FALSE; }
  UINT64 Base = (UINTN)Info->ImageBase;
  if (!ScmTraceReadable (Base + 0x1000, 4096) || !ScmTraceReadable (Base + 0x4000, 4096) ||
      !ScmTraceReadable (Base + 0xB000, 4096)) { return FALSE; }
  for (UINTN I = 0; I < ARRAY_SIZE (mScmTraceSites); I++) {
    if (*(CONST UINT32 *)(UINTN)(Base + mScmTraceSites[I].Rva) != HOUJI_SCM_TRACE_BRK) { return FALSE; }
  }
  // These real calls must stay at their original addresses and opcodes.
  CONST UINT32 Calls[][2] = {{0x150C,0x94000240},{0x1E98,0xD63F0100},
    {0x1EC0,0xD63F0120},{0x1EDC,0x94000529},{0x1EE8,0x94000B90}};
  for (UINTN I = 0; I < ARRAY_SIZE (Calls); I++) {
    if (*(CONST UINT32 *)(UINTN)(Base + Calls[I][0]) != Calls[I][1]) { return FALSE; }
  }
  return TRUE;
}

STATIC EFI_STATUS
RunScmTrace (EFI_HANDLE Driver, CONST EFI_LOADED_IMAGE_PROTOCOL *Info)
{
  if (mScmTraceActive || !ValidateScmTraceCode (Info)) {
    Print (L"HOUJI SCM TRACE: refused; expected pinned diagnostic PE and unchanged real calls\n");
    return EFI_COMPROMISED_DATA;
  }
  mScmTraceSelfTestPassed = FALSE;
  mScmTraceModuleVerified = FALSE; mScmTraceStackVerified = FALSE;
  EFI_CPU_ARCH_PROTOCOL *Cpu = NULL;
  EFI_STATUS Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status) || !Cpu) { return EFI_NOT_READY; }
  EFI_STATUS Sync = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, StorageException);
  EFI_STATUS Serror = EFI_NOT_STARTED;
  if (!EFI_ERROR (Sync)) { Serror = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SERROR, StorageException); }
  BOOLEAN Started = FALSE;
  Status = EFI_ABORTED;
  if (EFI_ERROR (Sync) || EFI_ERROR (Serror)) { goto Cleanup; }
  if (!ScmTraceReadable ((UINTN)ClockTraceSelfTest, 8)) { goto Cleanup; }
  if (*(CONST UINT32 *)(UINTN)ClockTraceSelfTest != 0xD4290960U ||
      *((CONST UINT32 *)(UINTN)ClockTraceSelfTest + 1) != 0xD65F03C0U) { goto Cleanup; }
  ZeroMem (mScmTraceSnapshots, sizeof (mScmTraceSnapshots));
  mScmTraceImageBase = (UINTN)Info->ImageBase;
  mScmTraceImageBytes = Info->ImageSize;
  mScmTraceModuleVerified = TRUE;
  mScmTraceSelfTestPassed = FALSE;
  UINT64 OldBase = mStorageImageBase, OldBytes = mStorageImageBytes;
  CONST CHAR16 *OldName = mStorageDriverName;
  mStorageImageBase = mScmTraceImageBase; mStorageImageBytes = mScmTraceImageBytes;
  mStorageDriverName = L"ScmDxeCompat trace";
  __asm__ volatile ("mov %0, sp" : "=r" (mScmTraceEntrySp));
  if (mScmTraceEntrySp >= 0x10000 && mScmTraceEntrySp <= MAX_UINT64 - 16) {
    mScmTraceStackLow = (mScmTraceEntrySp - 0x10000) & ~4095ULL;
    mScmTraceStackHigh = (mScmTraceEntrySp + 15) & ~4095ULL;
    if (mScmTraceStackHigh <= MAX_UINT64 - 4096) { mScmTraceStackHigh += 4096; }
    BOOLEAN OwnedStack = (mScmTraceStackLow >= 0xA760D000 && mScmTraceStackHigh <= 0xA764D000) ||
      (mScmTraceStackLow >= 0xB8000000 && mScmTraceStackHigh <= 0xD3A00000);
    if (OwnedStack) { mScmTraceStackVerified = ScmTraceReadable (mScmTraceStackLow, mScmTraceStackHigh - mScmTraceStackLow); }
  }
  // Initialize GOP here, before foundation reaches SCM. No disk logger or GPU
  // resources are needed; the later GPU Start still resets its own RAM ring.
  HoujiGpuCallTraceInit ();
  HoujiGpuCallTraceStart ();
  Started = TRUE; mScmTraceActive = TRUE;
  ClockTraceSelfTest ();
  if (mScmTraceSelfTestPassed) {
    Print (L"HOUJI SCM TRACE: BRK resume self-test PASS; module-AT=1 stack-size-AT=%u; 9 MOV/ADRP sites; original BL/BLR/SMC/memory instructions retained\n", mScmTraceStackVerified);
    Status = gBS->StartImage (Driver, NULL, NULL);
  }
  mScmTraceActive = FALSE;
  HoujiGpuCallTraceStop ();
  mScmTraceModuleVerified = FALSE; mScmTraceStackVerified = FALSE;
  mStorageImageBase = OldBase; mStorageImageBytes = OldBytes; mStorageDriverName = OldName;
Cleanup: ;
  // Never replace somebody else's handler or remove an unowned registration.
  EFI_STATUS SyncOff = EFI_SUCCESS, SerrorOff = EFI_SUCCESS;
  if (!EFI_ERROR (Sync)) { SyncOff = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, NULL); }
  if (!EFI_ERROR (Serror)) { SerrorOff = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SERROR, NULL); }
  Print (L"HOUJI SCM TRACE: sync=%r SError=%r selftest=%u driver=%r unregister=%r/%r; no storage access by trace\n",
    Sync, Serror, mScmTraceSelfTestPassed, Status, SyncOff, SerrorOff);
  if (Started) {
    HoujiGpuCallTraceReport ();
    for (UINTN I = 0; I < ARRAY_SIZE (mScmTraceSites); I++) {
      CONST SCM_TRACE_SNAPSHOT *S = &mScmTraceSnapshots[I];
      Print (L"HOUJI SCM TRACE SAMPLE: step=%x seen=%u PC=%lx size=%lu flags=%x raw-X0=%lx saved56=%lx saved8192=%lx SP=%lx LR=%lx; ret-kind=%a\n",
        0x8000 + (UINT32)I, S->Seen, S->Pc, S->Requested, S->Flags, S->Result,
        S->Pointer56, S->Pointer8192, S->Sp, S->Lr,
        mScmTraceSites[I].Sample == 2 ? "pointer" : (mScmTraceSites[I].Sample == 5 ? "EFI_STATUS" : "not-a-return"));
    }
  }
  if (EFI_ERROR (SyncOff) || EFI_ERROR (SerrorOff)) {
    Print (L"HOUJI SCM TRACE: retained STOP; handler removal failed; app and loaded resources must remain resident. Hold power for recovery.\n");
    CpuDeadLoop ();
  }
  return Status;
}

STATIC EFI_STATUS
StartFoundationRange (EFI_HANDLE ImageHandle, UINTN First, UINTN Limit, UINTN LoadDelayUs, UINTN StartDelayUs, UINTN DelayFrom)
{
  if (First >= Limit || Limit > ARRAY_SIZE (mServices)) { return EFI_INVALID_PARAMETER; }
  EFI_LOADED_IMAGE_PROTOCOL *Loaded = NULL;
  EFI_STATUS Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status) || !Loaded || !Loaded->DeviceHandle) { return EFI_NOT_FOUND; }
  if (!HasScheduler ()) { return EFI_NOT_READY; }
  for (UINTN N = First; N < Limit; N++) {
    OEM_SERVICE *Service = &mServices[N];
    Print (L"HOUJI OEM: [%u/%u] checking %s\n", N + 1, Limit, Service->Name);
    for (UINTN D = 0; D < Service->DependencyCount; D++) {
      VOID *Interface = NULL;
      Status = gBS->LocateProtocol (&Service->Dependencies[D], NULL, &Interface);
      // DEPEX tests protocol existence. Architectural marker protocols may
      // legitimately have a NULL interface; use the status for this check.
      if (EFI_ERROR (Status)) {
        Print (L"HOUJI OEM: missing %g; stopped before %s\n", &Service->Dependencies[D], Service->Name);
        return EFI_NOT_READY;
      }
    }
    EFI_DEVICE_PATH_PROTOCOL *Path = FileDevicePath (Loaded->DeviceHandle, (CHAR16 *)Service->Path);
    if (!Path) { return EFI_OUT_OF_RESOURCES; }
    EFI_HANDLE Driver = NULL;
    if (LoadDelayUs && N >= DelayFrom) {
      Print (L"HOUJI STEP: %s LoadImage in %u seconds\n", Service->Name, LoadDelayUs / 1000000);
      gBS->Stall (LoadDelayUs);
    }
    Print (L"HOUJI STEP: calling LoadImage for %s\n", Service->Name);
    Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &Driver);
    FreePool (Path);
    Print (L"HOUJI OEM: %s LoadImage returned %r\n", Service->Name, Status);
    if (EFI_ERROR (Status)) {
      Print (L"HOUJI OEM: %s LoadImage=%r\n", Service->Name, Status);
      return Status;
    }
    EFI_LOADED_IMAGE_PROTOCOL *DriverInfo = NULL;
    EFI_STATUS InfoStatus = gBS->HandleProtocol (Driver, &gEfiLoadedImageProtocolGuid, (VOID **)&DriverInfo);
    if (!EFI_ERROR (InfoStatus) && DriverInfo) {
      Print (L"HOUJI OEM: %s image=%lx bytes=%lx\n", Service->Name, (UINTN)DriverInfo->ImageBase, DriverInfo->ImageSize);
      if (StrCmp (Service->Name, L"ClockDxe") == 0) {
        mClockImageBase = (UINTN)DriverInfo->ImageBase;
        mClockImageBytes = DriverInfo->ImageSize;
        if (!ValidateClockCode ()) {
          Print (L"HOUJI CLOCK: refused; code differs from selected profile\n");
          return EFI_COMPROMISED_DATA;
        }
        if (!ClockStoragePolicy ((UINT8 *)(UINTN)mClockImageBase, (UINTN)mClockImageBytes, mClockStorageOnly)) {
          Print (L"HOUJI CLOCK: refused; storage BSP layout differs\n");
          return EFI_COMPROMISED_DATA;
        }
        if (mClockStorageOnly) {
          Print (L"HOUJI CLOCK POLICY: storage-only r16; camera/PCIe CESTA list absent\n");
          Print (L"HOUJI CLOCK POLICY: only loaded writable BSP list/count changed; no disk writes\n");
        }
      }
      if (mStorageServicesRun && N >= 14) {
        mStorageImageBase = (UINTN)DriverInfo->ImageBase;
        mStorageImageBytes = DriverInfo->ImageSize;
        mStorageDriverName = Service->Name;
      }
    }
    if (StrCmp (Service->Name, L"ClockDxe") == 0 &&
        (EFI_ERROR (InfoStatus) || !DriverInfo)) { return EFI_COMPROMISED_DATA; }
    if (mStorageServicesRun && N >= 14 &&
        (EFI_ERROR (InfoStatus) || !DriverInfo)) { return EFI_COMPROMISED_DATA; }
    if (StartDelayUs && N >= DelayFrom) {
      Print (L"HOUJI STEP: %s StartImage in %u seconds\n", Service->Name, StartDelayUs / 1000000);
      gBS->Stall (StartDelayUs);
    }
    Print (L"HOUJI OEM: starting %s NOW\n", Service->Name);
    Status = StrCmp (Service->Name, L"ScmDxeCompat") == 0 ?
      RunScmTrace (Driver, EFI_ERROR (InfoStatus) ? NULL : DriverInfo) : gBS->StartImage (Driver, NULL, NULL);
    Print (L"HOUJI OEM: %s returned %r\n", Service->Name, Status);
    // Stop on failure and issue no additional UnloadImage call. StartImage's
    // internal failure cleanup is controlled by the firmware image loader.
    if (EFI_ERROR (Status)) { return Status; }
    if (Service->Provides && !HasProtocol (Service->Provides, Service->Name)) {
      Print (L"HOUJI OEM: %s did not publish its real interface; stopped\n", Service->Name);
      return EFI_NOT_FOUND;
    }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
StartFoundation (EFI_HANDLE ImageHandle, UINTN Limit, UINTN LoadDelayUs, UINTN StartDelayUs, UINTN DelayFrom)
{
  return StartFoundationRange (ImageHandle, 0, Limit, LoadDelayUs, StartDelayUs, DelayFrom);
}

STATIC EFI_STATUS
RunMappedClock (EFI_HANDLE ImageHandle)
{
  CONST CHAR16 *Profile = mStorageServicesRun ? L"untraced storage clock r17" :
    (mClockStorageOnly ? L"storage scope r16" : (mClockTraceEnabled ? L"CESTA trace r15" : L"mapped r13"));
  EFI_CPU_ARCH_PROTOCOL *Cpu = NULL;
  EFI_STATUS Found = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  EFI_STATUS Sync = EFI_NOT_FOUND, Serror = EFI_NOT_FOUND;
  if (!EFI_ERROR (Found) && Cpu) {
    // Do not replace an existing registered handler. Undo only registrations
    // owned by this diagnostic if ClockDxe returns.
    Sync = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, ClockException);
    Serror = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SERROR, ClockException);
  }
  Print (L"HOUJI CLOCK: exception reporter sync=%r SError=%r\n", Sync, Serror);
  EFI_STATUS Status = EFI_ABORTED;
  if (mClockTraceEnabled) {
    if (EFI_ERROR (Sync) || *(CONST UINT32 *)(UINTN)ClockTraceSelfTest != 0xD4290960U) {
      Print (L"HOUJI TRACE: refused; own synchronous handler/stub unavailable\n");
      goto Finished;
    }
    ClockTraceSelfTest ();
    if (!mClockTraceSelfTestPassed) { goto Finished; }
    Print (L"HOUJI TRACE: BRK resume self-test PASS; original calls/conditions preserved\n");
  }
  if (mUfsReadonlyRun) {
    Print (L"HOUJI CLOCK: verified prerequisite; no observation delay\n");
    Status = StartFoundationRange (ImageHandle, 13, 14, 0, 0, 13);
  } else {
    Print (L"HOUJI CLOCK: %s; ClockDxe starts after 15 seconds\n", Profile);
    gBS->Stall (15000000);
    Status = StartFoundationRange (ImageHandle, 13, 14, 2000000, 8000000, 13);
  }
Finished:
  if (!EFI_ERROR (Sync)) { (VOID)Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, NULL); }
  if (!EFI_ERROR (Serror)) { (VOID)Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SERROR, NULL); }
  Print (L"HOUJI CLOCK: %s result=%r\n", Profile, Status);
  (VOID)HasProtocol (&gEfiClockProtocolGuid, L"Clock after test");
  Print (L"HOUJI CLOCK: no SMMU/UFS driver or disk writes requested\n");
  return Status;
}

STATIC EFI_STATUS
RunStorageServices (EFI_HANDLE ImageHandle)
{
  if (ARRAY_SIZE (mServices) != 16 ||
      !HasProtocol (&gEfiClockProtocolGuid, L"Clock prerequisite")) { return EFI_NOT_READY; }
  if (HasProtocol (&mIommuProvider, L"HALIOMMU before test") ||
      HasProtocol (&mIcbProvider, L"ICB before test")) {
    Print (L"HOUJI STORAGE: refused duplicate service initialization\n");
    return EFI_ALREADY_STARTED;
  }
  if (gST->ConOut) { gST->ConOut->ClearScreen (gST->ConOut); }
  Print (L"HOUJI STORAGE r17: HALIOMMU and ICB services only\n");
  Print (L"HOUJI STORAGE: original Clock PE, CESTA absent for this boot\n");
  Print (mUfsReadonlyRun ? L"HOUJI STORAGE: prerequisites first; guarded UFS follows\n" :
    L"HOUJI STORAGE: UFSDxe is absent; no block or provisioning requests\n");
  EFI_CPU_ARCH_PROTOCOL *Cpu = NULL;
  EFI_STATUS Found = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  EFI_STATUS Sync = EFI_NOT_FOUND, Serror = EFI_NOT_FOUND;
  if (!EFI_ERROR (Found) && Cpu) {
    Sync = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, StorageException);
    Serror = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SERROR, StorageException);
  }
  Print (L"HOUJI STORAGE: exception reporter sync=%r SError=%r\n", Sync, Serror);
  EFI_STATUS Status = StartFoundationRange (ImageHandle, 14, 16,
    mUfsReadonlyRun ? 0 : 2000000, mUfsReadonlyRun ? 0 : 10000000, 14);
  if (!EFI_ERROR (Sync)) { (VOID)Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, NULL); }
  if (!EFI_ERROR (Serror)) { (VOID)Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SERROR, NULL); }
  Print (L"HOUJI STORAGE r17: services result=%r\n", Status);
  (VOID)HasProtocol (&mIommuProvider, L"HALIOMMU after test");
  (VOID)HasProtocol (&mIcbProvider, L"ICB after test");
  if (!mUfsReadonlyRun || EFI_ERROR (Status)) {
    Print (L"HOUJI STORAGE: TEST ENDED; this result page stays still\n");
    Print (L"HOUJI STORAGE: hold power, then VolumeUp+Power for OrangeFox; UFS not started\n");
  } else { Print (L"HOUJI STORAGE: prerequisites complete; continuing to guarded UFS\n"); }
  return Status;
}

STATIC UINTN
ReadHeaders (VOID)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, Checked = 0;
  EFI_GUID Seen[8];
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  Print (L"HOUJI UFS: BlockIO handles=%u status=%r\n", Count, Status);
  if (EFI_ERROR (Status)) { return 0; }
  for (UINTN N = 0; N < Count; N++) {
    EFI_BLOCK_IO_PROTOCOL *Io = NULL;
    Status = gBS->HandleProtocol (Handles[N], &gEfiBlockIoProtocolGuid, (VOID **)&Io);
    if (EFI_ERROR (Status) || !Io || !Io->Media) { continue; }
    EFI_BLOCK_IO_MEDIA *M = Io->Media;
    if (M->LogicalPartition || !M->MediaPresent || M->BlockSize != 4096 || M->LastBlock < 2) { continue; }
    UINTN Alignment = M->IoAlign < EFI_PAGE_SIZE ? EFI_PAGE_SIZE : M->IoAlign;
    if (Alignment > 0x200000 || (Alignment & (Alignment - 1))) { continue; }
    VOID *Buffer = AllocateAlignedPages (1, Alignment);
    if (!Buffer) { continue; }
    Status = Io->ReadBlocks (Io, M->MediaId, 1, M->BlockSize, Buffer);
    Print (L"HOUJI UFS: handle%u lastLBA=%lx block=%u read=%r\n", N, M->LastBlock, M->BlockSize, Status);
    if (!EFI_ERROR (Status)) {
      EFI_PARTITION_TABLE_HEADER *H = Buffer;
      if (H->Header.Signature == EFI_PTAB_HEADER_ID &&
          H->Header.HeaderSize >= 92 && H->Header.HeaderSize <= M->BlockSize &&
          H->MyLBA == 1 && H->AlternateLBA == M->LastBlock) {
        UINT32 Expected = H->Header.CRC32, Actual = 0;
        H->Header.CRC32 = 0;
        EFI_STATUS CrcStatus = gBS->CalculateCrc32 (Buffer, H->Header.HeaderSize, &Actual);
        H->Header.CRC32 = Expected;
        BOOLEAN Good = !EFI_ERROR (CrcStatus) && Actual == Expected;
        Print (L"HOUJI UFS: GPT header CRC %s expected=%08x actual=%08x\n", Good ? L"PASS" : L"FAIL", Expected, Actual);
        for (UINTN I = 0; Good && I < Checked; I++) {
          if (CompareGuid (&Seen[I], &H->DiskGUID)) {
            Good = FALSE;
            Print (L"HOUJI UFS: duplicate disk GUID; not counted\n");
          }
        }
        if (Good && Checked < ARRAY_SIZE (Seen)) { CopyGuid (&Seen[Checked++], &H->DiskGUID); }
      } else { Print (L"HOUJI UFS: no matching primary GPT header\n"); }
    }
    FreeAlignedPages (Buffer, 1);
  }
  FreePool (Handles);
  Print (L"HOUJI UFS: %u GPT headers passed. No disk writes requested.\n", Checked);
  return Checked;
}

#include "TouchHandoff.h"
#include "UsbHandoff.h"
#include "GpuHandoff.h"
#include "GpuPowerOnlyHandoff.h"
#include "GpuPowerHandoffGuid.h"
EFI_STATUS HoujiGpuPowerOnlyPrepare(EFI_HANDLE ImageHandle,struct hph_record *Record);
VOID HoujiGpuPowerPrepare(EFI_HANDLE ImageHandle);
BOOLEAN HoujiGpuMayBoot(VOID);
BOOLEAN HoujiGpuMaySave(VOID);
#include "LogFsTest.h"

#include "GpuRecoveryRequest.h"
// GPU batch profile: finish and return directly, without the unrelated high
// RAM loader test or XNU startup. All reset paths require an exact readback
// of the host-backed-up no-wipe recovery request and a committed checkpoint.
#define HOUJI_GPU_BATCH_UEFI_ONLY 1
#define HOUJI_GPU_CPU_ABI_XNU_ONLY 1
#define HOUJI_GPU_POWER_HANDOFF_XNU_ONLY 1
STATIC BOOLEAN
GpuBatchReturnRecovery(VOID)
{
  STATIC UINT8 Block[4096] __attribute__((aligned(4096)));
  UINT8 Expected[864];
  if(!mGpuCheckpointParent || !mGpuCheckpointParent->Media ||
     !mGpuCheckpointParent->Media->MediaPresent ||
     mGpuCheckpointParent->Media->BlockSize!=4096 ||
     mGpuCheckpointParent->Media->IoAlign>4096 ||
     mGpuCheckpointParent->Media->LastBlock<1024 || !hgr_record(Expected,sizeof(Expected)))return FALSE;
  EFI_STATUS Status=mGpuCheckpointParent->ReadBlocks(mGpuCheckpointParent,
    mGpuCheckpointParent->Media->MediaId,1024,sizeof(Block),Block);
  BOOLEAN Match=!EFI_ERROR(Status) && CompareMem(Block,Expected,sizeof(Expected))==0;
  Print(L"HOUJI GPU BATCH RECOVERY: read=%r exact-no-wipe-request=%u; UEFI does not write misc\n",Status,Match);
  if(!Match || EFI_ERROR(ProbeCheckpoint("gpu-batch-recovery-request-verified")))return FALSE;
  gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);
  return FALSE; // No reset result may be treated as a successful handoff.
}

STATIC EFI_STATUS
BootNativeUfsProbe (EFI_HANDLE ImageHandle)
{
  EFI_LOADED_IMAGE_PROTOCOL *Self = NULL;
  EFI_STATUS Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Self);
  if (EFI_ERROR (Status) || !Self || !Self->DeviceHandle) { return EFI_NOT_FOUND; }
  UNIXNU_LOADER_PROTOCOL *Loader = NULL;
  Status = gBS->LocateProtocol (&gUniXNULoaderProtocolGuid, NULL, (VOID **)&Loader);
  if (EFI_ERROR (Status)) {
    EFI_DEVICE_PATH_PROTOCOL *Path = FileDevicePath (Self->DeviceHandle, L"\\Drivers\\DarwinBoot.efi");
    if (!Path) { return EFI_OUT_OF_RESOURCES; }
    EFI_HANDLE Driver = NULL;
    Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &Driver);
    FreePool (Path);
    if (EFI_ERROR (Status)) { return Status; }
    Status = gBS->StartImage (Driver, NULL, NULL);
    if (EFI_ERROR (Status)) { return Status; }
    Status = gBS->LocateProtocol (&gUniXNULoaderProtocolGuid, NULL, (VOID **)&Loader);
  }
  if (EFI_ERROR (Status) || !Loader || Loader->Revision != UNIXNU_LOADER_REVISION || !Loader->Boot) { return EFI_UNSUPPORTED; }
  UNIXNU_LOADER_BOOT_REQUEST Request;
  ZeroMem (&Request, sizeof (Request));
  Request.Volume = Self->DeviceHandle;
  Request.KernelPath = L"\\Payloads\\kernel.macho";
  // yoni13 fork: the boot line this kernel runs with (houji_gpu_runtime=4 unbounded desktop, houji_usb=1 USB console, wfi=1,
  // houji_m2v=cG Metal bridge grouped completions, houji_usb_sdp_ma=900 USB SDP charge limit).
  CONST CHAR16 *BaseBootArgs = L"-v sweetfb_hz=120 serial=1 debug=0x14e keepsyms=1 houji_smp=1 houji_native_root=1 houji_ufs_log=1 houji_return_recovery=1 houji_gpu_fw_mem_probe=1 houji_gpu_scm_probe=1 houji_gpu_stock_probe=8 houji_panic_recovery=1 sweetfb_noconsole=1 houji_fb_gpu_sync=1 houji_fb_gpu_cache=1 houji_dvfs=3 houji_epoch=1792839100 msgbuf=65536 rd=md0 rootrw=1 houji_usb=1 wfi=1 houji_m2v=cG houji_usb_sdp_ma=900 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 houji_touch_probe=1";
  CHAR16 BootArgs[608];
  // Darwin 22's command line includes its trailing NUL in this budget.
  // Never silently truncate the final touch handoff when adding options.
  if (StrLen(BaseBootArgs) + StrLen(L" houji_touch=1") >= ARRAY_SIZE(BootArgs)) {
    Print(L"HOUJI: boot arguments exceed the Darwin 22 command-line budget\n");
    return EFI_BAD_BUFFER_SIZE;
  }
  UnicodeSPrint(BootArgs,sizeof(BootArgs),L"%s houji_touch=%u",BaseBootArgs,mTouchHandoffReady?1:0);
  Request.BootArgs=BootArgs;
  Print (L"HOUJI NATIVE UFS r241: checkpoint saved; entering XNU fixed-file log trial\n");
  return Loader->Boot (Loader, &Request);
}


// One controlled exit for completed/error trials. Unknown GPU ownership must
// never reopen storage. A reset is only attempted after a committed snapshot
// and GpuBatchReturnRecovery's actual exact no-wipe BCB readback.
STATIC BOOLEAN mTrialRecoveryAttempted;
STATIC BOOLEAN
GpuTrialReturnRecovery(CONST CHAR8 *Reason)
{
  if(mTrialRecoveryAttempted || !Reason || !mXnuUfsRun ||
     !mGpuCheckpointParent || !HoujiGpuMaySave())return FALSE;
  mTrialRecoveryAttempted=TRUE;
  EFI_STATUS Saved=ProbeCheckpoint(Reason);
  Print(L"HOUJI AUTO RECOVERY: reason=%a checkpoint=%r; exact recovery request required\n",Reason,Saved);
  if(EFI_ERROR(Saved))return FALSE;
  return GpuBatchReturnRecovery();
}


#include "GpuPrearmedRecoveryNative.inc"

STATIC EFI_GUID mGpuPowerHandoffGuid=HOUJI_GPU_POWER_HANDOFF_GUID;
STATIC struct hph_record mGpuPowerHandoffRecord;
STATIC BOOLEAN mGpuPowerHandoffAttempted;
// Reject any existing GUID, including a NULL VendorTable, before power I/O.
STATIC BOOLEAN GpuPowerHandoffGuidVacant(VOID) {
 if(!gST || gST->NumberOfTableEntries>256 ||
  (gST->NumberOfTableEntries && !gST->ConfigurationTable))return FALSE;
 for(UINTN I=0;I<gST->NumberOfTableEntries;I++)
  if(CompareGuid(&gST->ConfigurationTable[I].VendorGuid,&mGpuPowerHandoffGuid))return FALSE;
 return TRUE;
}
STATIC BOOLEAN GpuPowerHandoffReturnRecovery(CONST CHAR8 *Reason) {
 if(HoujiGpuMaySave())return GpuTrialReturnRecovery(Reason);
 return GpuPowerRecoveryResetWithoutIO();
}
STATIC EFI_STATUS GpuPowerHandoffPrepare(EFI_HANDLE ImageHandle) {
 struct hph_record Returned;EFI_STATUS Status;EFI_TPL OldTpl;
 if(mGpuPowerHandoffAttempted)return EFI_ALREADY_STARTED;
 mGpuPowerHandoffAttempted=TRUE;
 OldTpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(OldTpl);
 if(OldTpl!=TPL_APPLICATION)return EFI_UNSUPPORTED;
 if(!GpuPowerHandoffGuidVacant())return EFI_ALREADY_STARTED;
 if(!GpuPowerRecoveryPrearm())return EFI_ACCESS_DENIED;
 Status=HoujiGpuPowerOnlyPrepare(ImageHandle,&Returned);
 if(Status!=EFI_SUCCESS)return EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
 if(!hph_decode(&Returned,sizeof(Returned),&mGpuPowerHandoffRecord) ||
  mGpuPowerHandoffRecord.complete!=1)return EFI_COMPROMISED_DATA;
 // No context/firmware/table pointer is published. DarwinBoot copies this
 // immutable scalar record before ExitBootServices; no post-EBS EFI access.
 OldTpl=gBS->RaiseTPL(TPL_NOTIFY);
 if(!GpuPowerHandoffGuidVacant())Status=EFI_ALREADY_STARTED;
 else Status=gBS->InstallConfigurationTable(&mGpuPowerHandoffGuid,&mGpuPowerHandoffRecord);
 gBS->RestoreTPL(OldTpl);
 return Status==EFI_SUCCESS?EFI_SUCCESS:EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
}


// The installed image arms the exact no-wipe recovery request itself, so a
// normal power-on needs no host-side dd to misc. GpuPowerRecoveryPrearm
// below still verifies the identical 864-byte record at absolute LBA 1024
// with bytes 864..4095 preserved; only the arming step moved into the image.
STATIC EFI_STATUS
ArmStandaloneRecoveryRequest (VOID)
{
  EFI_BLOCK_IO_PROTOCOL *Parent = mGpuCheckpointParent;
  if (!Parent || !Parent->Media || !Parent->Media->MediaPresent ||
      Parent->Media->BlockSize != 4096 || Parent->Media->IoAlign > 4096 ||
      Parent->Media->LastBlock < 1024) { return EFI_NOT_READY; }
  STATIC UINT8 Block[4096] __attribute__((aligned(4096)));
  UINT8 Record[864];
  if (!hgr_record (Record, sizeof (Record))) { return EFI_NOT_READY; }
  UINT32 MediaId = Parent->Media->MediaId;
  EFI_STATUS Status = Parent->ReadBlocks (Parent, MediaId, 1024, sizeof (Block), Block);
  Print (L"HOUJI SELF BOOT: misc read=%r\n", Status);
  if (EFI_ERROR (Status)) { return Status; }
  if (CompareMem (Block, Record, sizeof (Record)) == 0) {
    Print (L"HOUJI SELF BOOT: exact recovery request already armed\n");
    return EFI_SUCCESS;
  }
  CopyMem (Block, Record, sizeof (Record));
  mBcbArmWritesArmed = TRUE;
  Status = Parent->WriteBlocks (Parent, MediaId, 1024, sizeof (Block), Block);
  EFI_STATUS Flush = Parent->FlushBlocks (Parent);
  mBcbArmWritesArmed = FALSE;
  Print (L"HOUJI SELF BOOT: misc arm write=%r flush=%r\n", Status, Flush);
  if (EFI_ERROR (Status)) { return Status; }
  if (EFI_ERROR (Flush)) { return Flush; }
  Status = Parent->ReadBlocks (Parent, MediaId, 1024, sizeof (Block), Block);
  if (EFI_ERROR (Status)) { return Status; }
  if (CompareMem (Block, Record, sizeof (Record)) != 0) { return EFI_CRC_ERROR; }
  Print (L"HOUJI SELF BOOT: recovery request armed and verified at LBA 1024\n");
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
RunUfsReadonly (EFI_HANDLE ImageHandle)
{
  CONST CLOCK_BANK Ufs = {L"UFS_MEM_UFS_2LANE_G5_REGS", 0x01D80000, 0x40000};
  if (!MappingCheck (&Ufs, 1, L"UFS")) { return EFI_NO_MAPPING; }
  EFI_CPU_ARCH_PROTOCOL *Cpu = NULL;
  EFI_STATUS Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status) || !Cpu) { return EFI_NOT_READY; }
  EFI_STATUS Sync = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, StorageException);
  EFI_STATUS Serror = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SERROR, StorageException);
  Print (L"HOUJI UFS: own exception guard sync=%r SError=%r\n", Sync, Serror);
  if (EFI_ERROR (Sync)) {
    if (!EFI_ERROR (Serror)) { (VOID)Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SERROR, NULL); }
    return EFI_ACCESS_DENIED;
  }
  mUfsGuardEnabled = TRUE;
  // Exercise the actual exception registration before allowing any UFS entry.
  if (*(CONST UINT32 *)(UINTN)ClockTraceSelfTest == 0xD4290960U) { ClockTraceSelfTest (); }
  Status = EFI_ABORTED;
  if (!mUfsGuardSelfTestPassed) { goto Finished; }
  Print (L"HOUJI UFS: BRK guard self-test PASS\n");
  EFI_LOADED_IMAGE_PROTOCOL *Self = NULL, *DriverInfo = NULL;
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Self);
  if (EFI_ERROR (Status) || !Self || !Self->DeviceHandle) { Status = EFI_NOT_FOUND; goto Finished; }
  EFI_DEVICE_PATH_PROTOCOL *Path = FileDevicePath (Self->DeviceHandle, L"\\Drivers\\UFSDxeReadOnly.efi");
  if (!Path) { Status = EFI_OUT_OF_RESOURCES; goto Finished; }
  EFI_HANDLE Driver = NULL;
  Print (L"HOUJI UFS r20: loading restricted diagnostic copy\n");
  Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &Driver);
  FreePool (Path);
  Print (L"HOUJI UFS: LoadImage=%r\n", Status);
  if (EFI_ERROR (Status)) { goto Finished; }
  Status = gBS->HandleProtocol (Driver, &gEfiLoadedImageProtocolGuid, (VOID **)&DriverInfo);
  if (EFI_ERROR (Status) || !DriverInfo || DriverInfo->ImageSize != 0x25000) {
    Status = EFI_COMPROMISED_DATA; goto Finished;
  }
  mStorageImageBase = (UINTN)DriverInfo->ImageBase;
  mStorageImageBytes = DriverInfo->ImageSize;
  mStorageDriverName = L"UFSDxeReadOnly";
  CONST UINT32 Rva[] = {0x4A9C, 0x11834, 0x11B0C, 0x11B10, 0x11830, 0x1185C, 0xB008};
  // Completion wait 11b0c is used by synchronous NOP/read/query too. It must
  // remain original; reject async requests at the actual command guard.
  CONST UINT32 Word[] = {0x17FFFEED, HOUJI_UFS_GUARD_BRK, 0xA9BB7BFD, 0xA90167FA, 0xF81F83A8, 0xF0000069, 0xD4290A40};
  for (UINTN I = 0; I < ARRAY_SIZE (Rva); I++) {
    if (*(CONST UINT32 *)(UINTN)(mStorageImageBase + Rva[I]) != Word[I]) {
      Print (L"HOUJI UFS: REFUSED; guarded code differs at %x\n", Rva[I]);
      Status = EFI_COMPROMISED_DATA; goto Finished;
    }
  }
  Print (L"HOUJI UFS: no RPMB/listeners/timers; async requests guarded\n");
  Print (L"HOUJI UFS r19: original completion wait restored\n");
  Print (L"HOUJI UFS r20: exact 02/04/02 permits one real Active retry\n");
  Print (L"HOUJI UFS: writes and unknown commands return -EROFS\n");
  Print (L"HOUJI UFS: starting restricted UFSDxe NOW\n");
  Status = gBS->StartImage (Driver, NULL, NULL);
  Print (L"HOUJI UFS: StartImage returned %r\n", Status);
  if (!EFI_ERROR (Status)) {
    UINTN Passed = ReadHeaders ();
    if (Passed != 6) { Status = EFI_NOT_READY; }
  }
Finished:
  // Keep our synchronous handler while the diagnostic image is resident: its
  // command entry contains a BRK. This app holds its result page indefinitely.
  Print (L"HOUJI UFS r20: GPT read result=%r allowed=%u denied=%u\n", Status, mUfsAllowed, mUfsDenied);
  if (mUfsHandoffRun && !EFI_ERROR (Status)) {
    CONST CLOCK_BANK Smmu = {L"HOUJI SMMU", 0x15000000, 0xD0000};
    EFI_STATUS Probe = MappingCheck (&Smmu, 1, L"SMMU") ? RunUfsHandoffProbe () : EFI_NO_MAPPING;
    Print (L"HOUJI HANDOFF: inventory result=%r; save result even if partial\n", Probe);
  }
  if (mLogFsTestRun && !EFI_ERROR (Status)) {
    // Establish a durable, validated log BEFORE entering experimental GPU I/O.
    Status = RunLogFsTest (ImageHandle);
    if (mXnuUfsRun && !EFI_ERROR(Status)) {
      // A standalone power-on has no host to dd misc with the exact no-wipe
      // recovery request; the installed image arms it itself before the
      // prearm-verified GPU power handoff below.
      Status = ArmStandaloneRecoveryRequest ();
      if (EFI_ERROR (Status)) {
        Print (L"HOUJI SELF BOOT: recovery request arm failed (%r); STOP before XNU\n", Status);
        (VOID)GpuTrialReturnRecovery ("standalone-bcb-arm-failed");
        // A reset that returns must never fall through to the XNU loader.
        for(;;)gBS->Stall(1000000);
      }
    }
    if (mXnuUfsRun && !EFI_ERROR(Status)) {
      if(HOUJI_GPU_POWER_HANDOFF_XNU_ONLY) {
        Print(L"HOUJI GPU POWER HANDOFF: UEFI power/clocks only; XNU fresh RO observation\n");
        Status=GpuPowerHandoffPrepare(ImageHandle);
        if(Status==EFI_SUCCESS)Status=ProbeCheckpoint("before-XNU-power-only-handoff-observe");
        if(Status!=EFI_SUCCESS) {
          Print(L"HOUJI GPU POWER HANDOFF: not ready (%r); STOP before XNU\n",Status);
          (VOID)GpuPowerHandoffReturnRecovery("gpu-power-only-not-ready");
          // A reset that returns must never fall through to the XNU loader.
          for(;;)gBS->Stall(1000000);
        }
      } else {
      if(HOUJI_GPU_CPU_ABI_XNU_ONLY) {
        Print(L"HOUJI GPU CPU ABI: UEFI GPU profile skipped; XNU CPU page/allocation observations only\n");
      } else {
      QueryGpuClockInterface();
      HoujiGpuPowerPrepare(ImageHandle);
      if(!HoujiGpuMayBoot()) {
        Print(L"HOUJI GPU: resources retained; STOP before XNU. Hold power, then volume-up + power for recovery.\n");
        (VOID)GpuTrialReturnRecovery("gpu-retained-stop-before-XNU");
        for(;;)gBS->Stall(1000000);
      }
      if(HOUJI_GPU_BATCH_UEFI_ONLY) {
        Print(L"HOUJI GPU BATCH: tests complete; direct recovery return; XNU/userland/high-RAM loader test skipped\n");
        (VOID)GpuTrialReturnRecovery("gpu-batch-complete");
        Print(L"HOUJI GPU BATCH: recovery reset not confirmed; STOP, no Android or XNU fallback\n");
        for(;;)gBS->Stall(1000000);
      }
      }
      Status=ProbeCheckpoint(HOUJI_GPU_CPU_ABI_XNU_ONLY?"before-XNU-CPU-page-geometry":"after-gpu-and-touch");
      }
    }
    if (mXnuUfsRun && !EFI_ERROR (Status)) {
      // Standalone USB: reproduce ABL-fastboot's controller state (GDSC,
      // GCC USB clocks, eUSB2 HS PHY, repeater, DWC3 device mode) so the
      // kernel CDC console works without a PC. Runs before the transcript
      // freezes; non-fatal by design.
      EFI_STATUS UsbStatus = PrepareUsbHandoff ();
      Print (L"HOUJI USB UEFI: preparation=%r; standalone console preconditions=%u\n",
        UsbStatus, mUsbHandoffAttempted);
      // Touch handoff is also required when GPU initialization belongs to XNU.
      EFI_STATUS TouchStatus=PrepareTouchHandoff();
      Print (L"HOUJI TOUCH UEFI: preparation=%r; driver enabled=%u\n",TouchStatus,mTouchHandoffReady);
      Status=ProbeCheckpoint(mTouchHandoffReady?"touch-handoff-enabled-before-XNU":"touch-handoff-not-ready-before-XNU");
      if(EFI_ERROR(TouchStatus) || !mTouchHandoffReady || EFI_ERROR(Status)) {
        (VOID)GpuTrialReturnRecovery("touch-handoff-not-ready");
        for(;;)gBS->Stall(1000000);
      }
      Status = BootNativeUfsProbe (ImageHandle);
      Print (L"HOUJI NATIVE UFS: loader returned %r\n", Status);
      (VOID)GpuTrialReturnRecovery("native-loader-returned");
    }
    Print (L"HOUJI LOGFS: stopped with %r; hold power, then VolumeUp+Power for OrangeFox\n", Status);
  }
  if(mXnuUfsRun && EFI_ERROR(Status))
    (VOID)GpuTrialReturnRecovery("storage-test-returned-error");
  Print (L"HOUJI UFS: TEST ENDED; hold power, then VolumeUp+Power for OrangeFox\n");
  return Status;
}

EFI_STATUS EFIAPI
HoujiStorageProbeEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  (VOID)SystemTable;
  if (gST->ConOut) { gST->ConOut->ClearScreen (gST->ConOut); }
  EFI_LOADED_IMAGE_PROTOCOL *Self = NULL;
  // Installed production image: always execute the qualified native-XNU
  // profile. Diagnostic-only images must change this at build time; no boot
  // manager or ABL load-option value may return this image to the old UFS test
  // stop screen.
  BOOLEAN FoundationRequested = TRUE;
  BOOLEAN SmemOnly = FALSE;
  BOOLEAN PowerBootstrap = TRUE;
  BOOLEAN ClockBootstrap = FALSE;
  BOOLEAN ClockMapOnly = TRUE;
  BOOLEAN ClockMappedRun = TRUE;
  mClockStorageOnly = TRUE;
  mStorageServicesRun = TRUE;
  mUfsReadonlyRun = TRUE;
  mXnuUfsRun = TRUE;
  mLogFsTestRun = TRUE;
  EFI_STATUS Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Self);
  // Production image: the boot manager/ABL path has supplied different or
  // stale options across fastboot and boot_a launches. Normalize the loaded
  // image options before the existing parser, so both paths execute the exact
  // --xnu-ufs-read profile that is hardware-qualified. Diagnostic profiles
  // belong in separately built images, never in the installed boot image.
  STATIC CONST CHAR16 ProductionLoadOptions[] = L"--xnu-ufs-read";
  if (!EFI_ERROR (Status) && Self) {
    Self->LoadOptions = (VOID *)ProductionLoadOptions;
    Self->LoadOptionsSize = sizeof (ProductionLoadOptions);
    Print (L"HOUJI SELF BOOT: forcing production profile --xnu-ufs-read\n");
  }
  if (!EFI_ERROR (Status) && Self && Self->LoadOptions &&
      Self->LoadOptionsSize == sizeof (L"--foundation") &&
      CompareMem (Self->LoadOptions, L"--foundation", sizeof (L"--foundation")) == 0) {
    FoundationRequested = TRUE;
  }
  if (!EFI_ERROR (Status) && Self && Self->LoadOptions &&
      Self->LoadOptionsSize == sizeof (L"--power-bootstrap") &&
      CompareMem (Self->LoadOptions, L"--power-bootstrap", sizeof (L"--power-bootstrap")) == 0) {
    FoundationRequested = TRUE;
    PowerBootstrap = TRUE;
  }
  if (!EFI_ERROR (Status) && Self && Self->LoadOptions &&
      Self->LoadOptionsSize == sizeof (L"--clock-bootstrap") &&
      CompareMem (Self->LoadOptions, L"--clock-bootstrap", sizeof (L"--clock-bootstrap")) == 0) {
    FoundationRequested = TRUE;
    PowerBootstrap = TRUE;
    ClockBootstrap = TRUE;
  }
  if (!EFI_ERROR (Status) && Self && Self->LoadOptions &&
      Self->LoadOptionsSize == sizeof (L"--clock-mmio-check") &&
      CompareMem (Self->LoadOptions, L"--clock-mmio-check", sizeof (L"--clock-mmio-check")) == 0) {
    FoundationRequested = TRUE;
    PowerBootstrap = TRUE;
    ClockMapOnly = TRUE;
  }
  if (!EFI_ERROR (Status) && Self && Self->LoadOptions &&
      Self->LoadOptionsSize == sizeof (L"--clock-mapped") &&
      CompareMem (Self->LoadOptions, L"--clock-mapped", sizeof (L"--clock-mapped")) == 0) {
    FoundationRequested = TRUE;
    PowerBootstrap = TRUE;
    ClockMapOnly = TRUE;
    ClockMappedRun = TRUE;
  }
  if (!EFI_ERROR (Status) && Self && Self->LoadOptions &&
      Self->LoadOptionsSize == sizeof (L"--clock-trace") &&
      CompareMem (Self->LoadOptions, L"--clock-trace", sizeof (L"--clock-trace")) == 0) {
    FoundationRequested = TRUE;
    PowerBootstrap = TRUE;
    ClockMapOnly = TRUE;
    ClockMappedRun = TRUE;
    mClockTraceEnabled = TRUE;
  }
  if (!EFI_ERROR (Status) && Self && Self->LoadOptions &&
      Self->LoadOptionsSize == sizeof (L"--clock-storage") &&
      CompareMem (Self->LoadOptions, L"--clock-storage", sizeof (L"--clock-storage")) == 0) {
    FoundationRequested = TRUE;
    PowerBootstrap = TRUE;
    ClockMapOnly = TRUE;
    ClockMappedRun = TRUE;
    mClockTraceEnabled = TRUE;
    mClockStorageOnly = TRUE;
  }
  if (!EFI_ERROR (Status) && Self && Self->LoadOptions &&
      Self->LoadOptionsSize == sizeof (L"--storage-services") &&
      CompareMem (Self->LoadOptions, L"--storage-services", sizeof (L"--storage-services")) == 0) {
    FoundationRequested = TRUE;
    PowerBootstrap = TRUE;
    ClockMapOnly = TRUE;
    ClockMappedRun = TRUE;
    mClockStorageOnly = TRUE;
    mStorageServicesRun = TRUE;
    // Use the original Clock PE without software-breakpoint instrumentation.
    mClockTraceEnabled = FALSE;
  }
  if (!EFI_ERROR (Status) && Self && Self->LoadOptions &&
      ((Self->LoadOptionsSize == sizeof (L"--ufs-readonly") &&
        CompareMem (Self->LoadOptions, L"--ufs-readonly", sizeof (L"--ufs-readonly")) == 0) ||
       (Self->LoadOptionsSize == sizeof (L"--logfs-write-test") &&
        CompareMem (Self->LoadOptions, L"--logfs-write-test", sizeof (L"--logfs-write-test")) == 0) ||
       (Self->LoadOptionsSize == sizeof (L"--ufs-handoff-check") &&
        CompareMem (Self->LoadOptions, L"--ufs-handoff-check", sizeof (L"--ufs-handoff-check")) == 0) ||
       (Self->LoadOptionsSize == sizeof (L"--xnu-ufs-read") &&
        CompareMem (Self->LoadOptions, L"--xnu-ufs-read", sizeof (L"--xnu-ufs-read")) == 0))) {
    FoundationRequested = TRUE;
    PowerBootstrap = TRUE;
    ClockMapOnly = TRUE;
    ClockMappedRun = TRUE;
    mClockStorageOnly = TRUE;
    mStorageServicesRun = TRUE;
    mUfsReadonlyRun = TRUE;
    mXnuUfsRun = Self->LoadOptionsSize == sizeof (L"--xnu-ufs-read");
    mUfsHandoffRun = Self->LoadOptionsSize == sizeof (L"--ufs-handoff-check");
    mLogFsTestRun = mXnuUfsRun || mUfsHandoffRun || Self->LoadOptionsSize == sizeof (L"--logfs-write-test");
    if (mLogFsTestRun) {
      Print (L"HOUJI LOGFS r241: app diagnostic capture; not full OEM/XNU output\n");
      Print (L"HOUJI LOGFS: backup pinned; one new file; no GPT/boot writes\n");
    }
  }
  if (!EFI_ERROR (Status) && Self && Self->LoadOptions &&
      Self->LoadOptionsSize == sizeof (L"--smem-only") &&
      CompareMem (Self->LoadOptions, L"--smem-only", sizeof (L"--smem-only")) == 0) {
    FoundationRequested = TRUE;
    SmemOnly = TRUE;
  }
  // A normal ABL boot from boot_a supplies no EFI load options. The old
  // diagnostic default stopped at the HOB inventory and therefore required a
  // PC's `fastboot boot`, whose launcher supplied --xnu-ufs-read. Make the
  // installed image self-booting: no options selects the same full XNU path;
  // every named diagnostic option above keeps its existing bounded behavior.
  if (!EFI_ERROR (Status) && Self && !FoundationRequested) {
    FoundationRequested = TRUE;
    PowerBootstrap = TRUE;
    ClockMapOnly = TRUE;
    ClockMappedRun = TRUE;
    mClockStorageOnly = TRUE;
    mStorageServicesRun = TRUE;
    mUfsReadonlyRun = TRUE;
    mXnuUfsRun = TRUE;
    mLogFsTestRun = TRUE;
    Print (L"HOUJI SELF BOOT: no recognized diagnostic option; defaulting to --xnu-ufs-read\n");
  }
  // Final production override after every legacy option parser above. This is
  // deliberately unconditional: the installed image cannot enter the old
  // HOB/UFS diagnostic-stop modes, irrespective of LoadOptions contents.
  FoundationRequested = TRUE;
  SmemOnly = FALSE;
  PowerBootstrap = TRUE;
  ClockBootstrap = FALSE;
  ClockMapOnly = TRUE;
  ClockMappedRun = TRUE;
  mClockTraceEnabled = FALSE;
  mClockStorageOnly = TRUE;
  mStorageServicesRun = TRUE;
  mUfsReadonlyRun = TRUE;
  mUfsHandoffRun = FALSE;
  mXnuUfsRun = TRUE;
  mLogFsTestRun = TRUE;
  Print (L"HOUJI SELF BOOT: final production override; diagnostic stop disabled\n");
  (VOID)gBS->SetWatchdogTimer (0, 0, 0, NULL);
  if (!FoundationRequested) {
    Print (L"HOUJI STORAGE: Scheduler HOB-only checkpoint r4\n");
    Print (L"HOUJI OEM: zero OEM service entry points will run\n");
    Print (L"HOUJI STEP: HOB check starts after 15 seconds\n");
    gBS->Stall (15000000);
    BOOLEAN SchedulerValid = HasScheduler ();
    Print (L"HOUJI OEM: HOB-only checkpoint %s; no services loaded\n", SchedulerValid ? L"PASS" : L"STOP");
    Print (L"HOUJI STORAGE: hold power, then VolumeUp+Power for OrangeFox\n");
    for (;;) { gBS->Stall (1000000); }
  }
  if (SmemOnly) {
    Print (L"HOUJI STORAGE: SmemDxe-only checkpoint r5\n");
    Print (L"HOUJI OEM: only SmemDxe may run; no DAL/clock/SMMU/UFS\n");
    if (HasProtocol (&gEfiSMEMProtocolGuid, L"SMEM before test")) {
      Print (L"HOUJI OEM: SMEM already installed; refusing duplicate initialization\n");
    } else {
      EFI_STATUS Smem = StartFoundation (ImageHandle, 1, 15000000, 20000000, 0);
      Print (L"HOUJI OEM: SmemDxe-only result=%r\n", Smem);
      (VOID)HasProtocol (&gEfiSMEMProtocolGuid, L"SMEM after test");
    }
    Print (L"HOUJI STORAGE: single-service test ended; hold power, then VolumeUp+Power for OrangeFox\n");
    for (;;) { gBS->Stall (1000000); }
  }
  UINTN Limit = ClockMapOnly ? 13 : (ClockBootstrap ? 14 : (PowerBootstrap ? 11 : 8));
  if (Limit > ARRAY_SIZE (mServices)) {
    Print (L"HOUJI OEM: payload has too few services for this selection\n");
    for (;;) { gBS->Stall (1000000); }
  }
  Print (L"HOUJI STORAGE: %s\n", ClockMappedRun ? (mClockStorageOnly ? L"Clock storage scope r16" : (mClockTraceEnabled ? L"CESTA stage trace r15" : L"mapped clock bootstrap r13")) : (ClockMapOnly ? L"clock MMIO translation check" : (ClockBootstrap ? L"clock bootstrap" : (PowerBootstrap ? L"power bootstrap" : L"OEM foundation"))));
  Print (mUfsReadonlyRun ? L"HOUJI OEM: storage prerequisites then guarded UFS r20\n" :
    (mStorageServicesRun ? L"HOUJI OEM: storage prerequisites; UFSDxe absent\n" : L"HOUJI OEM: no SMMU or UFS driver in this profile\n"));
  if (mUfsReadonlyRun) {
    Print (L"HOUJI STEP: verified prerequisites start immediately\n");
  } else {
    Print (L"HOUJI STEP: service sequence starts after 15 seconds\n");
    gBS->Stall (15000000);
  }
  // The first eleven services passed r8. Explicit load options select the
  // bounded service count; pause only before the new services in each profile.
  EFI_STATUS Foundation = StartFoundation (ImageHandle, Limit, ClockMapOnly ? 0 : 2000000, ClockMapOnly ? 0 : 10000000, ClockBootstrap ? 11 : (PowerBootstrap ? 8 : 6));
  Print (L"HOUJI OEM: foundation result=%r\n", Foundation);
  if (ClockMapOnly) {
    if (EFI_ERROR (Foundation)) {
      Print (L"HOUJI MAP: prerequisite service failed; stopped\n");
    } else {
      if (gST->ConOut) { gST->ConOut->ClearScreen (gST->ConOut); }
      BOOLEAN MapGood = ClockMappingCheck ();
      if (ClockMappedRun) {
        if (MapGood) {
          EFI_STATUS Clock = RunMappedClock (ImageHandle);
          if (mStorageServicesRun && !EFI_ERROR (Clock)) {
            EFI_STATUS Services = RunStorageServices (ImageHandle);
            if (mUfsReadonlyRun && !EFI_ERROR (Services)) { (VOID)RunUfsReadonly (ImageHandle); }
          }
        }
        else { Print (L"HOUJI CLOCK: refused; mapping checks failed\n"); }
      }
    }
    for (;;) { gBS->Stall (1000000); }
  }
  if (PowerBootstrap) {
    (VOID)HasProtocol (&mCmdDb, L"CmdDb");
    (VOID)HasProtocol (&mPwrUtils, L"PwrUtils");
    (VOID)HasProtocol (&mNpa, L"NPA");
  }
  (VOID)HasProtocol (&gEfiSMEMProtocolGuid, L"SMEM");
  (VOID)HasProtocol (&gEfiClockProtocolGuid, ClockBootstrap ? L"Clock" : L"Clock (not loaded)");
  (VOID)HasProtocol (&gQcomScmProtocolGuid, L"SCM");
  (VOID)HasProtocol (&gEfiPlatformInfoProtocolGuid, L"PlatformInfo");
  (VOID)HasProtocol (&gEfiUsbfnIoProtocolGuid, L"USB Function IO");
  Print (L"HOUJI UFS: not started in this profile\n");
  ReadHeaders ();
  Print (L"HOUJI STORAGE: preflight ended; hold power, then VolumeUp+Power for OrangeFox.\n");
  (VOID)gBS->SetWatchdogTimer (0, 0, 0, NULL);
  for (;;) { gBS->Stall (1000000); }
}
