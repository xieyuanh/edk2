/** @file

Copyright (c) 2024, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "PiSmmCpuCommon.h"

/**
  Return if the Address is forbidden as SMM communication buffer.

  @param[in] Address the address to be checked

  @return TRUE  The address is forbidden as SMM communication buffer.
  @return FALSE The address is allowed as SMM communication buffer.
**/
BOOLEAN
IsSmmCommBufferForbiddenAddress (
  IN UINT64  Address
  )
{
  EFI_PEI_HOB_POINTERS         Hob;
  EFI_HOB_RESOURCE_DESCRIPTOR  *ResourceDescriptor;

  Hob.Raw = GetFirstHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR);
  while (Hob.Raw != NULL) {
    ResourceDescriptor = (EFI_HOB_RESOURCE_DESCRIPTOR *)Hob.Raw;
    if ((Address >= ResourceDescriptor->PhysicalStart) && (Address < ResourceDescriptor->PhysicalStart + ResourceDescriptor->ResourceLength)) {
      return FALSE;
    }

    Hob.Raw = GET_NEXT_HOB (Hob);
    Hob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, Hob.Raw);
  }

  return TRUE;
}

/*
  Build Memory Map from ResourceDescriptor HOBs by excluding Logging attribute range.

  @param[out]     MemoryMap            Returned Non-Mmram Memory Map.
  @param[out]     MemoryMapSize        A pointer to the size, it is the size of new created memory map.
  @param[out]     DescriptorSize       Size, in bytes, of an individual EFI_MEMORY_DESCRIPTOR.

*/
VOID
BuildMemoryMapFromResDescHobs (
  OUT EFI_MEMORY_DESCRIPTOR  **MemoryMap,
  OUT UINTN                  *MemoryMapSize,
  OUT UINTN                  *DescriptorSize
  )
{
  EFI_PEI_HOB_POINTERS         Hob;
  UINTN                        Count;
  EFI_HOB_RESOURCE_DESCRIPTOR  *ResourceDescriptor;
  UINTN                        Index;

  ASSERT (MemoryMap != NULL && MemoryMapSize != NULL && DescriptorSize != NULL);

  *MemoryMap      = NULL;
  *MemoryMapSize  = 0;
  *DescriptorSize = 0;

  //
  // Get the count.
  //
  Count   = 0;
  Hob.Raw = GetFirstHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR);
  while (Hob.Raw != NULL) {
    ResourceDescriptor = (EFI_HOB_RESOURCE_DESCRIPTOR *)Hob.Raw;
    if ((ResourceDescriptor->ResourceAttribute & EDKII_MM_RESOURCE_ATTRIBUTE_LOGGING) == 0) {
      Count++;
    }

    Hob.Raw = GET_NEXT_HOB (Hob);
    Hob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, Hob.Raw);
  }

  *DescriptorSize = sizeof (EFI_MEMORY_DESCRIPTOR);
  *MemoryMapSize  =  *DescriptorSize * Count;

  *MemoryMap = (EFI_MEMORY_DESCRIPTOR *)AllocateZeroPool (*MemoryMapSize);
  ASSERT (*MemoryMap != NULL);

  Index   = 0;
  Hob.Raw = GetFirstHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR);
  while (Hob.Raw != NULL) {
    ResourceDescriptor = (EFI_HOB_RESOURCE_DESCRIPTOR *)Hob.Raw;
    if ((ResourceDescriptor->ResourceAttribute & EDKII_MM_RESOURCE_ATTRIBUTE_LOGGING) == 0) {
      ASSERT (Index < Count);
      (*MemoryMap)[Index].PhysicalStart = ResourceDescriptor->PhysicalStart;
      (*MemoryMap)[Index].NumberOfPages = EFI_SIZE_TO_PAGES (ResourceDescriptor->ResourceLength);
      (*MemoryMap)[Index].Attribute     = EFI_MEMORY_XP;
      if (ResourceDescriptor->ResourceAttribute == EFI_RESOURCE_ATTRIBUTE_READ_ONLY_PROTECTED) {
        (*MemoryMap)[Index].Attribute |= EFI_MEMORY_RO;
      }

      Index++;
    }

    Hob.Raw = GET_NEXT_HOB (Hob);
    Hob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, Hob.Raw);
  }

  return;
}

/*
  Create extended protection MemoryMap.

  The caller is responsible for freeing MemoryMap via FreePool().

  @param[out]     MemoryMap            Returned Non-Mmram Memory Map.
  @param[out]     MemoryMapSize        A pointer to the size, it is the size of new created memory map.
  @param[out]     DescriptorSize       Size, in bytes, of an individual EFI_MEMORY_DESCRIPTOR.

*/
VOID
CreateExtendedProtectionRange (
  OUT EFI_MEMORY_DESCRIPTOR  **MemoryMap,
  OUT UINTN                  *MemoryMapSize,
  OUT UINTN                  *DescriptorSize
  )
{
  BuildMemoryMapFromResDescHobs (MemoryMap, MemoryMapSize, DescriptorSize);
}

/*
  Create the Non-Mmram Memory Map within the ResourceDescriptor HOBs
  without Logging attribute.

  The caller is responsible for freeing MemoryMap via FreePool().

  @param[in]      PhysicalAddressBits  The bits of physical address to map.
  @param[out]     MemoryMap            Returned Non-Mmram Memory Map.
  @param[out]     MemoryMapSize        A pointer to the size, it is the size of new created memory map.
  @param[out]     DescriptorSize       Size, in bytes, of an individual EFI_MEMORY_DESCRIPTOR.

*/
VOID
CreateNonMmramMemMap (
  IN  UINT8                  PhysicalAddressBits,
  OUT EFI_MEMORY_DESCRIPTOR  **MemoryMap,
  OUT UINTN                  *MemoryMapSize,
  OUT UINTN                  *DescriptorSize
  )
{
  BuildMemoryMapFromResDescHobs (MemoryMap, MemoryMapSize, DescriptorSize);
}