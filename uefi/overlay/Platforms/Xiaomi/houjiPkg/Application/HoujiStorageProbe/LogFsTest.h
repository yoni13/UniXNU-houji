/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Validate the backed-up GPT before connecting its logfs FAT child. The command
 * gate is the final write boundary, even for filesystem metadata/repair paths.
 */
#ifndef HOUJI_CALL_TRACE
#define HOUJI_CALL_TRACE(step,address,value) ((void)0)
#endif
STATIC EFI_STATUS
ValidateLogFsParent (EFI_HANDLE *Parent, EFI_BLOCK_IO_PROTOCOL **ParentIo,
                     EFI_PARTITION_ENTRY *Entry)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, Matches = 0;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
    NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return Status; }
  CONST EFI_GUID Disk = {0xE7D1484D,0xC58A,0xAA30,{0x98,0xC1,0xB1,0x10,0xB4,0xBC,0x5E,0xC4}};
  CONST EFI_GUID Part = {0x04BB810B,0x92B4,0x38CA,{0x8E,0xF5,0x9D,0xD0,0x5A,0x04,0xFE,0x5C}};
  for (UINTN I = 0; I < Count; I++) {
    EFI_BLOCK_IO_PROTOCOL *Io = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiBlockIoProtocolGuid, (VOID **)&Io)) ||
        !Io || !Io->Media) { continue; }
    EFI_BLOCK_IO_MEDIA *M = Io->Media;
    if (M->LogicalPartition || !M->MediaPresent || M->ReadOnly ||
        M->BlockSize != 4096 || M->LastBlock != 0x7AABFFF) { continue; }
    UINTN Align = M->IoAlign < EFI_PAGE_SIZE ? EFI_PAGE_SIZE : M->IoAlign;
    if (Align > 0x200000 || (Align & (Align - 1))) { continue; }
    UINT8 *Data = AllocateAlignedPages (3, Align);
    if (!Data) { continue; }
    Status = Io->ReadBlocks (Io, M->MediaId, 1, 3 * 4096, Data);
    EFI_PARTITION_TABLE_HEADER *H = (VOID *)Data;
    UINT32 Crc = 0;
    BOOLEAN Good = !EFI_ERROR (Status) && H->Header.Signature == EFI_PTAB_HEADER_ID &&
      H->Header.Revision == 0x10000 && H->Header.HeaderSize == 92 &&
      H->Header.Reserved == 0 && H->Header.CRC32 == 0x631FC7EA &&
      H->MyLBA == 1 && H->AlternateLBA == M->LastBlock &&
      CompareGuid (&H->DiskGUID, &Disk) && H->PartitionEntryLBA == 2 &&
      H->NumberOfPartitionEntries == 64 && H->SizeOfPartitionEntry == 128 &&
      H->PartitionEntryArrayCRC32 == 0x9F1BB496;
    if (Good) {
      H->Header.CRC32 = 0;
      Good = !EFI_ERROR (gBS->CalculateCrc32 (Data, 92, &Crc)) && Crc == 0x631FC7EA;
    }
    if (Good) {
      Good = !EFI_ERROR (gBS->CalculateCrc32 (Data + 4096, 8192, &Crc)) && Crc == 0x9F1BB496;
    }
    EFI_PARTITION_ENTRY *E = (VOID *)(Data + 4096 + 12 * 128);
    if (Good) {
      Good = E->StartingLBA == HOUJI_LOGFS_FIRST && E->EndingLBA == HOUJI_LOGFS_END - 1 &&
        E->Attributes == 0 && CompareGuid (&E->UniquePartitionGUID, &Part) &&
        StrCmp (E->PartitionName, L"logfs") == 0;
    }
    if (Good) { Matches++; *Parent = Handles[I]; *ParentIo = Io; CopyMem (Entry, E, sizeof (*E)); }
    FreeAlignedPages (Data, 3);
  }
  FreePool (Handles);
  Print (L"HOUJI LOGFS: pinned GPT and logfs entry matches=%u\n", Matches);
  return Matches == 1 ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
}

STATIC EFI_STATUS
FindLogFsChild (EFI_HANDLE Parent, CONST EFI_PARTITION_ENTRY *Entry, EFI_HANDLE *Child)
{
  EFI_DEVICE_PATH_PROTOCOL *ParentPath = DevicePathFromHandle (Parent);
  if (!ParentPath) { return EFI_NOT_FOUND; }
  UINTN Prefix = GetDevicePathSize (ParentPath) - END_DEVICE_PATH_LENGTH;
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, Matches = 0;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid,
    NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN I = 0; I < Count; I++) {
    EFI_PARTITION_INFO_PROTOCOL *Info = NULL;
    EFI_BLOCK_IO_PROTOCOL *Io = NULL;
    EFI_DEVICE_PATH_PROTOCOL *Path = DevicePathFromHandle (Handles[I]);
    if (!Path || GetDevicePathSize (Path) != Prefix + sizeof (HARDDRIVE_DEVICE_PATH) + END_DEVICE_PATH_LENGTH ||
        CompareMem (Path, ParentPath, Prefix) != 0 ||
        EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info)) ||
        !Info || Info->Type != PARTITION_TYPE_GPT || CompareMem (&Info->Info.Gpt, Entry, sizeof (*Entry)) != 0 ||
        EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiBlockIoProtocolGuid, (VOID **)&Io)) ||
        !Io || !Io->Media || !Io->Media->LogicalPartition ||
        Io->Media->BlockSize != 4096 || Io->Media->LastBlock != 0x7FF) { continue; }
    HARDDRIVE_DEVICE_PATH *Hd = (VOID *)((UINT8 *)Path + Prefix);
    if (Hd->Header.Type != MEDIA_DEVICE_PATH || Hd->Header.SubType != MEDIA_HARDDRIVE_DP ||
        Hd->PartitionNumber != 13 || Hd->PartitionStart != HOUJI_LOGFS_FIRST ||
        Hd->PartitionSize != 0x800 || Hd->SignatureType != SIGNATURE_TYPE_GUID ||
        CompareMem (Hd->Signature, &Entry->UniquePartitionGUID, sizeof (EFI_GUID)) != 0) { continue; }
    Matches++; *Child = Handles[I];
  }
  FreePool (Handles);
  Print (L"HOUJI LOGFS: exact parent/partition child matches=%u\n", Matches);
  return Matches == 1 ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *mGpuCheckpointFs;
STATIC EFI_BLOCK_IO_PROTOCOL *mGpuCheckpointParent;

// Kernel contract (fork HoujiLogFile.h): XnuLog.bin must contain the exact
// pristine 65536-byte initial image -- magic HJXNU001, LE32(0), LE32(1), the
// fixed run ID, then zeros -- whose CRC32 the kernel's native-root bring-up
// verifies before appending its snapshot ("log file ownership/run ID
// changed" otherwise). boot-houji.sh restores it with dd; a standalone boot
// has no host, so restore it here through the same validated logfs FAT path
// used for the UEFI transcript, with size, content and CRC verified.
#define HOUJI_XNU_LOG_INITIAL_CRC 0xABBE97D5U
STATIC CONST UINT8 mXnuLogRunId[16] =
  { 0xAB,0x7A,0x39,0x33,0x1D,0xA9,0x46,0xBF,0x95,0x23,0x93,0x6E,0x56,0x25,0x63,0xCE };

STATIC EFI_STATUS
ResetNativeXnuLog (EFI_FILE_PROTOCOL *Root)
{
  STATIC UINT8 Image[65536];
  STATIC CONST UINT8 Magic[8] = {'H','J','X','N','U','0','0','1'};
  SetMem (Image, sizeof (Image), 0);
  CopyMem (Image, Magic, sizeof (Magic));
  Image[12] = 1;
  CopyMem (Image + 16, mXnuLogRunId, sizeof (mXnuLogRunId));
  UINT32 Crc = 0;
  EFI_STATUS Status = gBS->CalculateCrc32 (Image, sizeof (Image), &Crc);
  if (EFI_ERROR (Status) || Crc != HOUJI_XNU_LOG_INITIAL_CRC) {
    Print (L"HOUJI XNU LOG: initial image CRC %08x differs from the kernel constant\n", Crc);
    return EFI_CRC_ERROR;
  }
  EFI_FILE_PROTOCOL *File = NULL;
  Status = Root->Open (Root, &File, L"\\XnuLog.bin", EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) { return Status; }
  UINT64 Position = 0;
  Status = File->SetPosition (File, 0xFFFFFFFFFFFFFFFFULL);
  if (!EFI_ERROR (Status)) { Status = File->GetPosition (File, &Position); }
  File->Close (File);
  File = NULL;
  if (EFI_ERROR (Status) || Position != sizeof (Image)) {
    Print (L"HOUJI XNU LOG: XnuLog.bin size %lu differs from the reserved 65536\n", Position);
    return EFI_VOLUME_CORRUPTED;
  }
  Status = Root->Open (Root, &File, L"\\XnuLog.bin", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (EFI_ERROR (Status)) { return Status; }
  UINTN Written = sizeof (Image);
  Status = File->SetPosition (File, 0);
  if (!EFI_ERROR (Status)) { Status = File->Write (File, &Written, Image); }
  if (!EFI_ERROR (Status) && Written != sizeof (Image)) { Status = EFI_DEVICE_ERROR; }
  if (!EFI_ERROR (Status)) { Status = File->Flush (File); }
  if (!EFI_ERROR (Status)) { Status = File->Close (File); }
  File = NULL;
  if (EFI_ERROR (Status)) { return Status; }
  Status = Root->Open (Root, &File, L"\\XnuLog.bin", EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) { return Status; }
  UINTN Read = sizeof (Image);
  Status = File->Read (File, &Read, Image);
  EFI_STATUS Closed = File->Close (File);
  if (!EFI_ERROR (Status)) { Status = Closed; }
  if (EFI_ERROR (Status)) { return Status; }
  if (Read != sizeof (Image) ||
      EFI_ERROR (gBS->CalculateCrc32 (Image, Read, &Crc)) ||
      Crc != HOUJI_XNU_LOG_INITIAL_CRC) { return EFI_CRC_ERROR; }
  Print (L"HOUJI XNU LOG: pristine initial state restored and verified (crc=%08x)\n", Crc);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
RunLogFsTest (EFI_HANDLE ImageHandle)
{
  EFI_HANDLE Parent = NULL, Child = NULL;
  EFI_BLOCK_IO_PROTOCOL *ParentIo = NULL;
  EFI_PARTITION_ENTRY Entry;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
  EFI_FILE_PROTOCOL *Root = NULL, *File = NULL;
  VOID *Readback = NULL;
  EFI_STATUS Status = ValidateLogFsParent (&Parent, &ParentIo, &Entry);
  if (EFI_ERROR (Status)) { return Status; }
  EFI_LOADED_IMAGE_PROTOCOL *Self = NULL;
  EFI_HANDLE PartitionDriver = NULL;
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Self);
  if (EFI_ERROR (Status) || !Self || !Self->DeviceHandle) { return EFI_NOT_FOUND; }
  EFI_DEVICE_PATH_PROTOCOL *Path = FileDevicePath (Self->DeviceHandle, L"\\Drivers\\PartitionDxe.efi");
  if (!Path) { return EFI_OUT_OF_RESOURCES; }
  Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &PartitionDriver);
  FreePool (Path);
  Print (L"HOUJI LOGFS r87: PartitionDxe LoadImage=%r\n", Status);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->StartImage (PartitionDriver, NULL, NULL);
  Print (L"HOUJI LOGFS: PartitionDxe StartImage=%r\n", Status);
  if (EFI_ERROR (Status)) { return Status; }
  VOID *DiskIo = NULL;
  EFI_STATUS DiskBefore = gBS->HandleProtocol (Parent, &gEfiDiskIoProtocolGuid, &DiskIo);
  Print (L"HOUJI LOGFS: parent DevicePath=%u DiskIo before=%r\n",
    DevicePathFromHandle (Parent) != NULL, DiskBefore);
  // FALSE is intentional: enumerate partition handles, do not mount all FATs.
  Status = gBS->ConnectController (Parent, NULL, NULL, FALSE);
  EFI_STATUS DiskAfter = gBS->HandleProtocol (Parent, &gEfiDiskIoProtocolGuid, &DiskIo);
  Print (L"HOUJI LOGFS: parent nonrecursive connect=%r DiskIo after=%r\n", Status, DiskAfter);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FindLogFsChild (Parent, &Entry, &Child);
  if (EFI_ERROR (Status)) { return Status; }
  mLogFsWritesArmed = TRUE;
  Print (L"HOUJI LOGFS: writes armed ONLY physical LUN0 [1000,1800)\n");
  Status = gBS->ConnectController (Child, NULL, NULL, FALSE);
  Print (L"HOUJI LOGFS: child FAT connect=%r\n", Status);
  if (EFI_ERROR (Status)) { goto Done; }
  Status = gBS->HandleProtocol (Child, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status) || !Fs) { Status = EFI_NOT_FOUND; goto Done; }
  Status = Fs->OpenVolume (Fs, &Root);
  Print (L"HOUJI LOGFS: OpenVolume=%r\n", Status);
  if (EFI_ERROR (Status)) { goto Done; }
  Status = Root->Open (Root, &File, L"\\UniXNU.log", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status)) {
    // Standalone boots have no host to remove the previous UEFI transcript.
    // Recreate only this known diagnostic file inside the validated logfs.
    Status = File->Delete (File);
    File = NULL; // EFI_FILE_PROTOCOL.Delete closes the handle, including on error.
    if (Status != EFI_SUCCESS) { goto Done; }
    Status = EFI_NOT_FOUND;
  }
  if (Status != EFI_NOT_FOUND) { goto Done; }
  File = NULL;
  if (mProbeLogTruncated) { Status = EFI_BUFFER_TOO_SMALL; goto Done; }
  Print (L"HOUJI LOGFS: r87 snapshot end; app text only; commit/readback follows\n");
  UINTN PayloadBytes = mProbeLogLength;
  UINT32 PayloadCrc = 0;
  Status = gBS->CalculateCrc32 (mProbeLog, PayloadBytes, &PayloadCrc);
  if (EFI_ERROR (Status)) { goto Done; }
  Print (L"HOUJI LOGFS CHECKSUM: payload-bytes=%u payload-crc32=%08x\n", PayloadBytes, PayloadCrc);
  if (mProbeLogTruncated) { Status = EFI_BUFFER_TOO_SMALL; goto Done; }
  mProbeLogFrozen = TRUE;
  UINTN Bytes = mProbeLogLength, Written = Bytes;
  UINT32 Crc = 0, BackCrc = 0;
  Status = gBS->CalculateCrc32 (mProbeLog, Bytes, &Crc);
  if (EFI_ERROR (Status)) { goto Done; }
  Status = Root->Open (Root, &File, L"\\UniXNU.log",
    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  Print (L"HOUJI LOGFS: create=%r bytes=%u CRC=%08x\n", Status, Bytes, Crc);
  if (EFI_ERROR (Status)) { goto Done; }
  Status = File->Write (File, &Written, mProbeLog);
  Print (L"HOUJI LOGFS: Write=%r bytes=%u\n", Status, Written);
  if (EFI_ERROR (Status) || Written != Bytes) { Status = EFI_DEVICE_ERROR; goto Done; }
  Status = File->Flush (File);
  Print (L"HOUJI LOGFS: file Flush=%r\n", Status);
  if (EFI_ERROR (Status)) { goto Done; }
  Status = File->Close (File); File = NULL;
  if (EFI_ERROR (Status)) { goto Done; }
  Status = Root->Open (Root, &File, L"\\UniXNU.log", EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) { goto Done; }
  Readback = AllocatePool (Bytes + 1);
  if (!Readback) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  UINTN Read = Bytes + 1;
  Status = File->Read (File, &Read, Readback);
  if (EFI_ERROR (Status) || Read != Bytes || CompareMem (Readback, mProbeLog, Bytes) != 0) {
    Status = EFI_CRC_ERROR; goto Done;
  }
  Status = gBS->CalculateCrc32 (Readback, Read, &BackCrc);
  if (EFI_ERROR (Status) || Crc != BackCrc) { Status = EFI_CRC_ERROR; goto Done; }
  Print (L"HOUJI LOGFS: file readback PASS bytes=%u CRC=%08x\n", Read, BackCrc);
  if (mXnuUfsRun) {
    // A standalone power-on has no host to reset XnuLog.bin; restore the
    // pristine initial image through this validated logfs FAT path before
    // the kernel's native-root ownership check runs.
    Status = ResetNativeXnuLog (Root);
    if (EFI_ERROR (Status)) { goto Done; }
  }
Done:
  if (File) { EFI_STATUS S = File->Close (File); if (EFI_ERROR (S) && !EFI_ERROR (Status)) { Status = S; } }
  if (Root) { EFI_STATUS S = Root->Close (Root); if (EFI_ERROR (S) && !EFI_ERROR (Status)) { Status = S; } }
  if (Readback) { FreePool (Readback); }
  if (mLogFsWritesArmed) {
    EFI_STATUS Flush = ParentIo->FlushBlocks (ParentIo);
    Print (L"HOUJI LOGFS: device Flush=%r\n", Flush);
    if (EFI_ERROR (Flush) && !EFI_ERROR (Status)) { Status = Flush; }
  }
  mLogFsWritesArmed = FALSE;
  Print (L"HOUJI LOGFS: result=%r write-commands=%u cache-flushes=%u denied=%u\n",
    Status, mLogFsWriteCommands, mLogFsFlushCommands, mUfsDenied);
  if (!EFI_ERROR (Status) && mLogFsWriteCommands > 0 && mUfsDenied == 0) {
    if (mXnuUfsRun) {
      Print (L"HOUJI LOGFS: PASS; checkpoint closed and write gate disabled before XNU\n");
      mGpuCheckpointFs=Fs;mGpuCheckpointParent=ParentIo;
      mProbeLogFrozen=FALSE;
      return EFI_SUCCESS;
    }
    Print (L"HOUJI LOGFS: PASS; returning to Android for independent ADB readback\n");
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    return EFI_DEVICE_ERROR;
  }
  return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
}

// Update only the file created by this boot's validated logfs test. Every
// checkpoint closes, flushes and independently reads back before returning.
// The GPU adapter calls this at application TPL, with CM3 still in reset.
EFI_STATUS EFIAPI ProbeCheckpoint(CONST CHAR8 *Stage) {
  HOUJI_CALL_TRACE(0x2000,(UINTN)Stage,0);
  EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
  VOID *Back=NULL;
  EFI_STATUS Status;
  UINTN Bytes=0,Written=0,Read=0;
  UINT32 Crc=0,BackCrc=0;
  if(!Stage || !mXnuUfsRun || !mGpuCheckpointFs || !mGpuCheckpointParent ||
     mLogFsWritesArmed || mProbeLogFrozen) {
    HOUJI_CALL_TRACE(0x2001,(UINTN)Stage,EFI_NOT_READY);return EFI_NOT_READY;
  }
  HOUJI_CALL_TRACE(0x2002,0,TPL_HIGH_LEVEL);
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);HOUJI_CALL_TRACE(0x2003,0,Tpl);
  HOUJI_CALL_TRACE(0x2004,0,Tpl);gBS->RestoreTPL(Tpl);HOUJI_CALL_TRACE(0x2005,0,Tpl);
  if(Tpl!=TPL_APPLICATION) {HOUJI_CALL_TRACE(0x2001,(UINTN)Stage,EFI_UNSUPPORTED);return EFI_UNSUPPORTED;}
  HOUJI_CALL_TRACE(0x2006,(UINTN)Stage,0);
  Print(L"HOUJI GPU CHECKPOINT: %a\n",Stage);
  HOUJI_CALL_TRACE(0x2007,(UINTN)Stage,0);
  if(mProbeLogTruncated) {HOUJI_CALL_TRACE(0x2001,(UINTN)Stage,EFI_BUFFER_TOO_SMALL);return EFI_BUFFER_TOO_SMALL;}
  Bytes=mProbeLogLength;
  HOUJI_CALL_TRACE(0x2008,(UINTN)mProbeLog,Bytes);
  Status=gBS->CalculateCrc32(mProbeLog,Bytes,&Crc);
  HOUJI_CALL_TRACE(0x2009,(UINTN)mProbeLog,Status);
  if(EFI_ERROR(Status)) {HOUJI_CALL_TRACE(0x2001,(UINTN)Stage,Status);return Status;}
  HOUJI_CALL_TRACE(0x200a,Bytes,Crc);
  Print(L"HOUJI LOGFS CHECKSUM: payload-bytes=%u payload-crc32=%08x\n",Bytes,Crc);
  HOUJI_CALL_TRACE(0x200b,Bytes,Crc);
  if(mProbeLogTruncated) {HOUJI_CALL_TRACE(0x2001,(UINTN)Stage,EFI_BUFFER_TOO_SMALL);return EFI_BUFFER_TOO_SMALL;}
  mProbeLogFrozen=TRUE;Bytes=mProbeLogLength;
  HOUJI_CALL_TRACE(0x200c,(UINTN)mProbeLog,Bytes);
  Status=gBS->CalculateCrc32(mProbeLog,Bytes,&Crc);
  HOUJI_CALL_TRACE(0x200d,(UINTN)mProbeLog,Status);
  if(EFI_ERROR(Status))goto Done;
  mLogFsWritesArmed=TRUE;
  HOUJI_CALL_TRACE(0x200e,(UINTN)mGpuCheckpointFs,0);
  Status=mGpuCheckpointFs->OpenVolume(mGpuCheckpointFs,&Root);
  HOUJI_CALL_TRACE(0x200f,(UINTN)Root,Status);
  if(EFI_ERROR(Status))goto Done;
  HOUJI_CALL_TRACE(0x2010,(UINTN)Root,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE);
  Status=Root->Open(Root,&File,L"\\UniXNU.log",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
  HOUJI_CALL_TRACE(0x2011,(UINTN)File,Status);
  if(EFI_ERROR(Status))goto Done;
  HOUJI_CALL_TRACE(0x2012,(UINTN)File,0);
  Status=File->SetPosition(File,0);
  HOUJI_CALL_TRACE(0x2013,(UINTN)File,Status);
  if(EFI_ERROR(Status))goto Done;
  mLogFsWritesArmed=TRUE;Written=Bytes;
  HOUJI_CALL_TRACE(0x2014,(UINTN)File,Written);
  Status=File->Write(File,&Written,mProbeLog);
  HOUJI_CALL_TRACE(0x2015,Written,Status);
  if(EFI_ERROR(Status) || Written!=Bytes) {Status=EFI_DEVICE_ERROR;goto Done;}
  HOUJI_CALL_TRACE(0x2016,(UINTN)File,0);
  Status=File->Flush(File);
  HOUJI_CALL_TRACE(0x2017,(UINTN)File,Status);
  if(EFI_ERROR(Status))goto Done;
  HOUJI_CALL_TRACE(0x2018,(UINTN)File,0);
  Status=File->Close(File);HOUJI_CALL_TRACE(0x2019,(UINTN)File,Status);File=NULL;
  if(EFI_ERROR(Status))goto Done;
  HOUJI_CALL_TRACE(0x201a,(UINTN)mGpuCheckpointParent,0);
  Status=mGpuCheckpointParent->FlushBlocks(mGpuCheckpointParent);
  HOUJI_CALL_TRACE(0x201b,(UINTN)mGpuCheckpointParent,Status);
  if(EFI_ERROR(Status))goto Done;
  HOUJI_CALL_TRACE(0x201c,(UINTN)Root,EFI_FILE_MODE_READ);
  Status=Root->Open(Root,&File,L"\\UniXNU.log",EFI_FILE_MODE_READ,0);
  HOUJI_CALL_TRACE(0x201d,(UINTN)File,Status);
  if(EFI_ERROR(Status))goto Done;
  HOUJI_CALL_TRACE(0x201e,0,Bytes+1);
  Back=AllocatePool(Bytes+1);
  HOUJI_CALL_TRACE(0x201f,(UINTN)Back,Bytes+1);
  if(!Back) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  Read=Bytes+1;HOUJI_CALL_TRACE(0x2020,(UINTN)File,Read);
  Status=File->Read(File,&Read,Back);HOUJI_CALL_TRACE(0x2021,Read,Status);
  INTN Different=0;
  if(!EFI_ERROR(Status) && Read==Bytes) {
    HOUJI_CALL_TRACE(0x2022,(UINTN)Back,Bytes);
    Different=CompareMem(Back,mProbeLog,Bytes);HOUJI_CALL_TRACE(0x2023,(UINTN)Back,Different);
  }
  if(EFI_ERROR(Status) || Read!=Bytes || Different) {
    Status=EFI_CRC_ERROR;goto Done;
  }
  HOUJI_CALL_TRACE(0x2024,(UINTN)Back,Read);
  Status=gBS->CalculateCrc32(Back,Read,&BackCrc);
  HOUJI_CALL_TRACE(0x2025,(UINTN)Back,Status);
  if(EFI_ERROR(Status) || BackCrc!=Crc)Status=EFI_CRC_ERROR;
Done:
  HOUJI_CALL_TRACE(0x2026,(UINTN)Stage,Status);
  if(File) {
    HOUJI_CALL_TRACE(0x2028,(UINTN)File,0);
    EFI_STATUS S=File->Close(File);HOUJI_CALL_TRACE(0x2029,(UINTN)File,S);
    if(EFI_ERROR(S) && !EFI_ERROR(Status))Status=S;
  }
  if(Root) {
    HOUJI_CALL_TRACE(0x202a,(UINTN)Root,0);
    EFI_STATUS S=Root->Close(Root);HOUJI_CALL_TRACE(0x202b,(UINTN)Root,S);
    if(EFI_ERROR(S) && !EFI_ERROR(Status))Status=S;
  }
  if(mLogFsWritesArmed) {
    HOUJI_CALL_TRACE(0x202c,(UINTN)mGpuCheckpointParent,0);
    EFI_STATUS S=mGpuCheckpointParent->FlushBlocks(mGpuCheckpointParent);
    HOUJI_CALL_TRACE(0x202d,(UINTN)mGpuCheckpointParent,S);
    if(EFI_ERROR(S) && !EFI_ERROR(Status))Status=S;
  }
  mLogFsWritesArmed=FALSE;mProbeLogFrozen=FALSE;
  if(Back) {
    HOUJI_CALL_TRACE(0x202e,(UINTN)Back,0);FreePool(Back);HOUJI_CALL_TRACE(0x202f,0,0);
  }
  HOUJI_CALL_TRACE(0x2027,(UINTN)Stage,Status);
  HOUJI_CALL_TRACE(0x2030,(UINTN)Stage,Status);
  Print(L"HOUJI GPU CHECKPOINT: %a result=%r written=%u read=%u crc=%08x gate=closed\n",Stage,Status,Written,Read,BackCrc);
  HOUJI_CALL_TRACE(0x2031,(UINTN)Stage,Status);
  HOUJI_CALL_TRACE(0x2001,(UINTN)Stage,Status);
  return Status;
}
