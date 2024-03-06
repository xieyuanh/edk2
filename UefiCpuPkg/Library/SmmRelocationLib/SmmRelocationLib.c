/** @file
  SMM Relocation Lib for each processor.

  This Lib produces the SMM_BASE_HOB in HOB database which tells
  the PiSmmCpuDxeSmm driver (runs at a later phase) about the new
  SMBASE for each processor. PiSmmCpuDxeSmm driver installs the
  SMI handler at the SMM_BASE_HOB.SmBase[Index]+0x8000 for processor
  Index.

  Copyright (c) 2024, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/
#include "InternalSmmRelocationLib.h"

UINTN   mMaxNumberOfCpus  = 1;
UINTN   mNumberOfCpus     = 1;
UINT64  *mSmBaseForAllCpu = NULL;

//
// The mode of the CPU at the time an SMI occurs
//
UINT8  mSmmSaveStateRegisterLma;

//
// Record all Processors Info
//
EFI_PROCESSOR_INFORMATION  *mProcessorInfo = NULL;

//
// SmBase Rebased or not
//
volatile BOOLEAN  *mRebased;

/**
  Hook the code executed immediately after an RSM instruction on the currently
  executing CPU.  The mode of code executed immediately after RSM must be
  detected, and the appropriate hook must be selected.  Always clear the auto
  HALT restart flag if it is set.

  @param[in] CpuIndex                 The processor index for the currently
                                      executing CPU.
  @param[in] CpuState                 Pointer to SMRAM Save State Map for the
                                      currently executing CPU.
  @param[in] NewInstructionPointer32  Instruction pointer to use if resuming to
                                      32-bit mode from 64-bit SMM.
  @param[in] NewInstructionPointer    Instruction pointer to use if resuming to
                                      same mode as SMM.

  @retval The value of the original instruction pointer before it was hooked.

**/
UINT64
EFIAPI
HookReturnFromSmm (
  IN UINTN              CpuIndex,
  SMRAM_SAVE_STATE_MAP  *CpuState,
  UINT64                NewInstructionPointer32,
  UINT64                NewInstructionPointer
  )
{
  UINT64  OriginalInstructionPointer;

  /*
  //
  // TODO for SMM MsrSaveState Enable case
  //
  OriginalInstructionPointer = SmmCpuFeaturesHookReturnFromSmm (
                                 CpuIndex,
                                 CpuState,
                                 NewInstructionPointer32,
                                 NewInstructionPointer
                                 );
  if (OriginalInstructionPointer != 0) {
    return OriginalInstructionPointer;
  }
  */

  if (mSmmSaveStateRegisterLma == EFI_MM_SAVE_STATE_REGISTER_LMA_32BIT) {
    OriginalInstructionPointer = (UINT64)CpuState->x86._EIP;
    CpuState->x86._EIP         = (UINT32)NewInstructionPointer;

    //
    // Clear the auto HALT restart flag so the RSM instruction returns
    // program control to the instruction following the HLT instruction.
    //
    if ((CpuState->x86.AutoHALTRestart & BIT0) != 0) {
      CpuState->x86.AutoHALTRestart &= ~BIT0;
    }
  } else {
    OriginalInstructionPointer = CpuState->x64._RIP;
    if ((CpuState->x64.IA32_EFER & LMA) == 0) {
      CpuState->x64._RIP = (UINT32)NewInstructionPointer32;
    } else {
      CpuState->x64._RIP = (UINT32)NewInstructionPointer;
    }

    //
    // Clear the auto HALT restart flag so the RSM instruction returns
    // program control to the instruction following the HLT instruction.
    //
    if ((CpuState->x64.AutoHALTRestart & BIT0) != 0) {
      CpuState->x64.AutoHALTRestart &= ~BIT0;
    }
  }

  return OriginalInstructionPointer;
}

/**
  C function for SMI handler. To change all processor's SMMBase Register.

**/
VOID
EFIAPI
SmmInitHandler (
  VOID
  )
{
  UINT32  ApicId;
  UINTN   Index;

  SMRAM_SAVE_STATE_MAP  *CpuState;

  //
  // Update SMM IDT entries' code segment and load IDT
  //
  AsmWriteIdtr (&gcSmiIdtr);
  ApicId = GetApicId ();

  ASSERT (mNumberOfCpus <= mMaxNumberOfCpus);

  for (Index = 0; Index < mNumberOfCpus; Index++) {
    if (ApicId == (UINT32)mProcessorInfo[Index].ProcessorId) {
      //
      // Configure SmBase.
      //
      CpuState             = (SMRAM_SAVE_STATE_MAP *)(UINTN)(SMM_DEFAULT_SMBASE + SMRAM_SAVE_STATE_MAP_OFFSET);
      CpuState->x86.SMBASE = (UINT32)mSmBaseForAllCpu[Index];

      //
      // Hook return after RSM to set SMM re-based flag
      // SMM re-based flag can't be set before RSM, because SMM save state context might be override
      // by next AP flow before it take effect.
      //
      SemaphoreHook (Index, &mRebased[Index]);
      return;
    }
  }

  ASSERT (FALSE);
}

/**
  Relocate SmmBases for each processor.

  Execute on first boot and all S3 resumes

**/
VOID
EFIAPI
SmmRelocateBases (
  VOID
  )
{
  UINT8                 BakBuf[BACK_BUF_SIZE];
  SMRAM_SAVE_STATE_MAP  BakBuf2;
  SMRAM_SAVE_STATE_MAP  *CpuStatePtr;
  UINT8                 *U8Ptr;
  UINTN                 Index;
  UINTN                 BspIndex;
  UINT32                BspApicId;

  //
  // Make sure the reserved size is large enough for procedure SmmInitTemplate.
  //
  ASSERT (sizeof (BakBuf) >= gcSmmInitSize);

  //
  // Patch ASM code template with current CR0, CR3, and CR4 values
  //
  PatchInstructionX86 (gPatchSmmCr0, AsmReadCr0 (), 4);
  PatchInstructionX86 (gPatchSmmCr3, AsmReadCr3 (), 4);
  PatchInstructionX86 (gPatchSmmCr4, AsmReadCr4 () & (~CR4_CET_ENABLE), 4);

  U8Ptr       = (UINT8 *)(UINTN)(SMM_DEFAULT_SMBASE + SMM_HANDLER_OFFSET);
  CpuStatePtr = (SMRAM_SAVE_STATE_MAP *)(UINTN)(SMM_DEFAULT_SMBASE + SMRAM_SAVE_STATE_MAP_OFFSET);

  //
  // Backup original contents at address 0x38000
  //
  CopyMem (BakBuf, U8Ptr, sizeof (BakBuf));
  CopyMem (&BakBuf2, CpuStatePtr, sizeof (BakBuf2));

  //
  // Load image for relocation
  //
  CopyMem (U8Ptr, gcSmmInitTemplate, gcSmmInitSize);

  //
  // Retrieve the local APIC ID of current processor
  //
  BspApicId = GetApicId ();

  //
  // Relocate SM bases for all APs
  // This is APs' 1st SMI - rebase will be done here, and APs' default SMI handler will be overridden by gcSmmInitTemplate
  //
  BspIndex = (UINTN)-1;
  for (Index = 0; Index < mNumberOfCpus; Index++) {
    mRebased[Index] = FALSE;
    if (BspApicId != (UINT32)mProcessorInfo[Index].ProcessorId) {
      SendSmiIpi ((UINT32)mProcessorInfo[Index].ProcessorId);
      //
      // Wait for this AP to finish its 1st SMI
      //
      while (!mRebased[Index]) {
      }
    } else {
      //
      // BSP will be Relocated later
      //
      BspIndex = Index;
    }
  }

  //
  // Relocate BSP's SMM base
  //
  ASSERT (BspIndex != (UINTN)-1);
  SendSmiIpi (BspApicId);

  //
  // Wait for the BSP to finish its 1st SMI
  //
  while (!mRebased[BspIndex]) {
  }

  //
  // Restore contents at address 0x38000
  //
  CopyMem (CpuStatePtr, &BakBuf2, sizeof (BakBuf2));
  CopyMem (U8Ptr, BakBuf, sizeof (BakBuf));
}

/**
  Initialize IDT to setup exception handlers in SMM.

**/
VOID
InitializeSmmIdt (
  VOID
  )
{
  EFI_STATUS              Status;
  BOOLEAN                 InterruptState;
  IA32_DESCRIPTOR         PeiIdtr;
  CONST EFI_PEI_SERVICES  **PeiServices;

  //
  // There are 32 (not 255) entries in it since only processor
  // generated exceptions will be handled.
  //
  gcSmiIdtr.Limit = (sizeof (IA32_IDT_GATE_DESCRIPTOR) * 32) - 1;

  //
  // Allocate for IDT.
  // sizeof (UINTN) is for the PEI Services Table pointer.
  //
  gcSmiIdtr.Base = (UINTN)AllocateZeroPool (gcSmiIdtr.Limit + 1 + sizeof (UINTN));
  ASSERT (gcSmiIdtr.Base != 0);
  gcSmiIdtr.Base += sizeof (UINTN);

  //
  // Disable Interrupt, save InterruptState and save PEI IDT table
  //
  InterruptState = SaveAndDisableInterrupts ();
  AsmReadIdtr (&PeiIdtr);

  //
  // Save the PEI Services Table pointer
  // The PEI Services Table pointer will be stored in the sizeof (UINTN) bytes
  // immediately preceding the IDT in memory.
  //
  PeiServices                                   = (CONST EFI_PEI_SERVICES **)(*(UINTN *)(PeiIdtr.Base - sizeof (UINTN)));
  (*(UINTN *)(gcSmiIdtr.Base - sizeof (UINTN))) = (UINTN)PeiServices;

  //
  // Load SMM temporary IDT table
  //
  AsmWriteIdtr (&gcSmiIdtr);

  //
  // Setup SMM default exception handlers, SMM IDT table
  // will be updated and saved in gcSmiIdtr
  //
  Status = InitializeCpuExceptionHandlers (NULL);
  ASSERT_EFI_ERROR (Status);

  //
  // Restore PEI IDT table and CPU InterruptState
  //
  AsmWriteIdtr ((IA32_DESCRIPTOR *)&PeiIdtr);
  SetInterruptState (InterruptState);
}

/**
  This routine will split SmramReserve hob to reserve SmmRelocationSize for Smm relocated memory.

  @param[in]       SmmRelocationSize   SmmRelocationSize for all processors.
  @param[in out]   SmmRelocationStart  Return the start address of Smm relocated memory in SMRAM.

  @retval EFI_SUCCESS           The gEfiSmmSmramMemoryGuid is split successfully.
  @retval EFI_DEVICE_ERROR      Failed to build new hob for gEfiSmmSmramMemoryGuid.
  @retval EFI_NOT_FOUND         The gEfiSmmSmramMemoryGuid is not found.

**/
EFI_STATUS
SplitSmramHobForSmmRelocation (
  IN     UINT64                SmmRelocationSize,
  IN OUT EFI_PHYSICAL_ADDRESS  *SmmRelocationStart
  )
{
  EFI_HOB_GUID_TYPE               *GuidHob;
  EFI_SMRAM_HOB_DESCRIPTOR_BLOCK  *DescriptorBlock;
  EFI_SMRAM_HOB_DESCRIPTOR_BLOCK  *NewDescriptorBlock;
  UINTN                           BufferSize;
  UINTN                           SmramRanges;

  NewDescriptorBlock = NULL;

  //
  // Retrieve the GUID HOB data that contains the set of SMRAM descriptors
  //
  GuidHob = GetFirstGuidHob (&gEfiSmmSmramMemoryGuid);
  if (GuidHob == NULL) {
    return EFI_NOT_FOUND;
  }

  DescriptorBlock = (EFI_SMRAM_HOB_DESCRIPTOR_BLOCK *)GET_GUID_HOB_DATA (GuidHob);

  //
  // Allocate one extra EFI_SMRAM_DESCRIPTOR to describe SMRAM memory that contains a pointer
  // to the Smm relocated memory.
  //
  SmramRanges = DescriptorBlock->NumberOfSmmReservedRegions;
  BufferSize  = sizeof (EFI_SMRAM_HOB_DESCRIPTOR_BLOCK) + (SmramRanges * sizeof (EFI_SMRAM_DESCRIPTOR));

  NewDescriptorBlock = (EFI_SMRAM_HOB_DESCRIPTOR_BLOCK *)BuildGuidHob (
                                                           &gEfiSmmSmramMemoryGuid,
                                                           BufferSize
                                                           );
  ASSERT (NewDescriptorBlock != NULL);
  if (NewDescriptorBlock == NULL) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Copy old EFI_SMRAM_HOB_DESCRIPTOR_BLOCK to new allocated region
  //
  CopyMem ((VOID *)NewDescriptorBlock, DescriptorBlock, BufferSize - sizeof (EFI_SMRAM_DESCRIPTOR));

  //
  // Increase the number of SMRAM descriptors by 1 to make room for the ALLOCATED descriptor of size EFI_PAGE_SIZE
  //
  NewDescriptorBlock->NumberOfSmmReservedRegions = (UINT32)(SmramRanges + 1);

  ASSERT (SmramRanges >= 1);
  //
  // Copy last entry to the end - we assume TSEG is last entry.
  //
  CopyMem (&NewDescriptorBlock->Descriptor[SmramRanges], &NewDescriptorBlock->Descriptor[SmramRanges - 1], sizeof (EFI_SMRAM_DESCRIPTOR));

  //
  // Update the entry in the array with a size of SmmRelocationSize and put into the ALLOCATED state
  //
  NewDescriptorBlock->Descriptor[SmramRanges - 1].PhysicalSize = SmmRelocationSize;
  NewDescriptorBlock->Descriptor[SmramRanges - 1].RegionState |= EFI_ALLOCATED;

  //
  // Return the start address of Smm relocated memory in SMRAM.
  //
  if (SmmRelocationStart != NULL) {
    *SmmRelocationStart = NewDescriptorBlock->Descriptor[SmramRanges - 1].CpuStart;
  }

  //
  // Reduce the size of the last SMRAM descriptor by SmmRelocationSize
  //
  NewDescriptorBlock->Descriptor[SmramRanges].PhysicalStart += SmmRelocationSize;
  NewDescriptorBlock->Descriptor[SmramRanges].CpuStart      += SmmRelocationSize;
  NewDescriptorBlock->Descriptor[SmramRanges].PhysicalSize  -= SmmRelocationSize;

  //
  // Last step, we can scrub old one
  //
  ZeroMem (&GuidHob->Name, sizeof (GuidHob->Name));

  return EFI_SUCCESS;
}

/**
  CPU SmmBase Relocation Init.

  This function is to relocate CPU SmmBase.

  @param[in] MpServices2        Pointer to this instance of the MpServices.

  @retval EFI_UNSUPPORTED       CPU SmmBase Relocation unsupported.
  @retval EFI_OUT_OF_RESOURCES  CPU SmmBase Relocation failed.
  @retval EFI_SUCCESS           CPU SmmBase Relocated successfully.

**/
EFI_STATUS
EFIAPI
SmmRelocationInit (
  IN EDKII_PEI_MP_SERVICES2_PPI  *MpServices2
  )
{
  EFI_STATUS            Status;
  UINTN                 NumberOfEnabledCpus;
  UINTN                 TileSize;
  UINT64                SmmRelocationSize;
  EFI_PHYSICAL_ADDRESS  SmmRelocationStart;
  UINTN                 Index;
  SMM_BASE_HOB_DATA     *SmmBaseHobData;
  UINT32                CpuCount;
  UINT32                NumberOfProcessorsInHob;
  UINT32                MaxCapOfProcessorsInHob;
  UINT32                HobCount;
  UINT32                RegEax;
  UINT32                RegEdx;
  UINTN                 FamilyId;
  UINTN                 ModelId;
  UINTN                 SmmStackSize;
  UINT8                 *Stacks;

  SmmRelocationStart      = 0;
  SmmBaseHobData          = NULL;
  CpuCount                = 0;
  NumberOfProcessorsInHob = 0;
  MaxCapOfProcessorsInHob = 0;
  HobCount                = 0;
  Stacks                  = NULL;

  DEBUG ((DEBUG_INFO, "SmmRelocationInit Start \n"));
  if (MpServices2 == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Get the number of processors
  // If support CPU hot plug, we need to allocate resources for possibly hot-added processors
  //
  Status = MpServices2->GetNumberOfProcessors (
                          MpServices2,
                          &mNumberOfCpus,
                          &NumberOfEnabledCpus
                          );
  ASSERT_EFI_ERROR (Status);

  if (FeaturePcdGet (PcdCpuHotPlugSupport)) {
    mMaxNumberOfCpus = PcdGet32 (PcdCpuMaxLogicalProcessorNumber);
  } else {
    mMaxNumberOfCpus = mNumberOfCpus;
  }

  //
  // Calculate SmmRelocationSize for all of the tiles.
  //
  // The CPU save state and code for the SMI entry point are tiled within an SMRAM
  // allocated buffer. The minimum size of this buffer for a uniprocessor system
  // is 32 KB, because the entry point is SMBASE + 32KB, and CPU save state area
  // just below SMBASE + 64KB. If more than one CPU is present in the platform,
  // then the SMI entry point and the CPU save state areas can be tiles to minimize
  // the total amount SMRAM required for all the CPUs. The tile size can be computed
  // by adding the CPU save state size, any extra CPU specific context, and
  // the size of code that must be placed at the SMI entry point to transfer
  // control to a C function in the native SMM execution mode. This size is
  // rounded up to the nearest power of 2 to give the tile size for a each CPU.
  // The total amount of memory required is the maximum number of CPUs that
  // platform supports times the tile size.
  //
  TileSize          = SIZE_8KB;
  SmmRelocationSize = EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (SIZE_32KB + TileSize * (mMaxNumberOfCpus - 1)));

  //
  // Split SmramReserve hob to reserve SmmRelocationSize for Smm relocated memory
  //
  Status = SplitSmramHobForSmmRelocation (
             SmmRelocationSize,
             &SmmRelocationStart
             );
  ASSERT_EFI_ERROR (Status);

  ASSERT (SmmRelocationStart != 0);
  DEBUG ((DEBUG_INFO, "SmmRelocationInit - SmmRelocationSize: 0x%08x\n", SmmRelocationSize));
  DEBUG ((DEBUG_INFO, "SmmRelocationInit - SmmRelocationStart: 0x%08x\n", SmmRelocationStart));

  //
  // Init mSmBaseForAllCpu
  //
  mSmBaseForAllCpu = (UINT64 *)AllocatePages (EFI_SIZE_TO_PAGES (sizeof (UINT64) * mMaxNumberOfCpus));
  if (mSmBaseForAllCpu == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Count the hob instance maximum capacity of CPU (MaxCapOfProcessorsInHob) since the max HobLength is 0xFFF8.
  //
  MaxCapOfProcessorsInHob = (0xFFF8 - sizeof (EFI_HOB_GUID_TYPE) - sizeof (SMM_BASE_HOB_DATA)) / sizeof (UINT64) + 1;
  DEBUG ((DEBUG_INFO, "SmmRelocationInit - MaxCapOfProcessorsInHob: %03x\n", MaxCapOfProcessorsInHob));

  //
  // Create Guided SMM Base HOB Instances.
  //
  while (CpuCount != mMaxNumberOfCpus) {
    NumberOfProcessorsInHob = MIN ((UINT32)mMaxNumberOfCpus - CpuCount, MaxCapOfProcessorsInHob);

    SmmBaseHobData = BuildGuidHob (
                       &gSmmBaseHobGuid,
                       sizeof (SMM_BASE_HOB_DATA) + sizeof (UINT64) * NumberOfProcessorsInHob
                       );
    if (SmmBaseHobData == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto ON_EXIT;
    }

    SmmBaseHobData->ProcessorIndex     = CpuCount;
    SmmBaseHobData->NumberOfProcessors = NumberOfProcessorsInHob;

    DEBUG ((DEBUG_INFO, "SmmRelocationInit - SmmBaseHobData[%d]->ProcessorIndex: %03x\n", HobCount, SmmBaseHobData->ProcessorIndex));
    DEBUG ((DEBUG_INFO, "SmmRelocationInit - SmmBaseHobData[%d]->NumberOfProcessors: %03x\n", HobCount, SmmBaseHobData->NumberOfProcessors));
    for (Index = 0; Index < SmmBaseHobData->NumberOfProcessors; Index++) {
      //
      // Calculate the new SMBASE address
      //
      SmmBaseHobData->SmBase[Index] = (UINTN)(SmmRelocationStart)+ (Index + CpuCount) * TileSize - SMM_HANDLER_OFFSET;
      DEBUG ((DEBUG_INFO, "SmmRelocationInit - SmmBaseHobData[%d]->SmBase[%03x]: %08x\n", HobCount, Index, SmmBaseHobData->SmBase[Index]));

      //
      // Record each SmBase in mSmBaseForAllCpu
      //
      mSmBaseForAllCpu[Index + CpuCount] = SmmBaseHobData->SmBase[Index];
    }

    CpuCount += NumberOfProcessorsInHob;
    HobCount++;
    SmmBaseHobData = NULL;
  }

  //
  // Determine the mode of the CPU at the time an SMI occurs
  //   Intel(R) 64 and IA-32 Architectures Software Developer's Manual
  //   Volume 3C, Section 34.4.1.1
  //
  AsmCpuid (CPUID_VERSION_INFO, &RegEax, NULL, NULL, NULL);
  FamilyId = (RegEax >> 8) & 0xf;
  ModelId  = (RegEax >> 4) & 0xf;
  if ((FamilyId == 0x06) || (FamilyId == 0x0f)) {
    ModelId = ModelId | ((RegEax >> 12) & 0xf0);
  }

  RegEdx = 0;
  AsmCpuid (CPUID_EXTENDED_FUNCTION, &RegEax, NULL, NULL, NULL);
  if (RegEax >= CPUID_EXTENDED_CPU_SIG) {
    AsmCpuid (CPUID_EXTENDED_CPU_SIG, NULL, NULL, NULL, &RegEdx);
  }

  mSmmSaveStateRegisterLma = EFI_MM_SAVE_STATE_REGISTER_LMA_32BIT;
  if ((RegEdx & BIT29) != 0) {
    mSmmSaveStateRegisterLma = EFI_MM_SAVE_STATE_REGISTER_LMA_64BIT;
  }

  if (FamilyId == 0x06) {
    if ((ModelId == 0x17) || (ModelId == 0x0f) || (ModelId == 0x1c)) {
      mSmmSaveStateRegisterLma = EFI_MM_SAVE_STATE_REGISTER_LMA_64BIT;
    }
  }

  //
  // Fix up the address of the global variable or function referred in
  // SmmInit assembly files to be the absolute address
  //
  SmmInitFixupAddress ();

  //
  // Allocate SMI stacks for SMM base relocation.
  // Note: No need allocated for all CPUs since SM bases relocation for all CPUs is one by one.
  //
  SmmStackSize = EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (PcdGet32 (PcdCpuSmmStackSize)));
  Stacks       = (UINT8 *)AllocatePages (EFI_SIZE_TO_PAGES (SmmStackSize));
  if (Stacks == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ON_EXIT;
  }

  DEBUG ((DEBUG_INFO, "SmmRelocationInit - Stacks: 0x%x\n", Stacks));
  DEBUG ((DEBUG_INFO, "SmmRelocationInit - SmmStackSize: 0x%x\n", SmmStackSize));

  //
  // Set SMI stack for SMM base relocation
  //
  PatchInstructionX86 (
    gPatchSmmInitStack,
    (UINTN)(Stacks + SmmStackSize - sizeof (UINTN)),
    sizeof (UINTN)
    );

  //
  // Retrieve the mProcessorInfo
  //
  mProcessorInfo = (EFI_PROCESSOR_INFORMATION *)AllocatePool (sizeof (EFI_PROCESSOR_INFORMATION) * mMaxNumberOfCpus);
  ASSERT (mProcessorInfo != NULL);
  for (Index = 0; Index < mMaxNumberOfCpus; Index++) {
    if (Index < mNumberOfCpus) {
      Status = MpServices2->GetProcessorInfo (MpServices2, Index | CPU_V2_EXTENDED_TOPOLOGY, &mProcessorInfo[Index]);
      ASSERT_EFI_ERROR (Status);
    }
  }

  //
  // Initialize SMM IDT for SMM base relocation
  //
  InitializeSmmIdt ();

  //
  // Relocate SMM Base addresses to the ones allocated from SMRAM
  //
  mRebased = (BOOLEAN *)AllocateZeroPool (sizeof (BOOLEAN) * mMaxNumberOfCpus);
  if (mRebased == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ON_EXIT;
  }

  SmmRelocateBases ();

ON_EXIT:
  if (Stacks != NULL) {
    FreePages (Stacks, EFI_SIZE_TO_PAGES (SmmStackSize));
  }

  if (mSmBaseForAllCpu != NULL) {
    FreePages (mSmBaseForAllCpu, EFI_SIZE_TO_PAGES (sizeof (UINT64) * mMaxNumberOfCpus));
  }

  DEBUG ((DEBUG_INFO, "SmmRelocationInit Done!\n"));
  return Status;
}