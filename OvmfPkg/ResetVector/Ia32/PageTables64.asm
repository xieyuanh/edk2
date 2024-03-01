;------------------------------------------------------------------------------
; @file
; Sets the CR3 register for 64-bit paging
;
; Copyright (c) 2008 - 2013, Intel Corporation. All rights reserved.<BR>
; Copyright (c) 2017 - 2020, Advanced Micro Devices, Inc. All rights reserved.<BR>
; SPDX-License-Identifier: BSD-2-Clause-Patent
;
;------------------------------------------------------------------------------

BITS    32

; common for all levels
%define PAGE_PRESENT            0x01
%define PAGE_READ_WRITE         0x02
%define PAGE_USER_SUPERVISOR    0x04
%define PAGE_WRITE_THROUGH      0x08
%define PAGE_CACHE_DISABLE     0x010
%define PAGE_ACCESSED          0x020
%define PAGE_DIRTY             0x040
%define PAGE_GLOBAL           0x0100

; page table entries (level 1)
%define PAGE_PTE_PAT           0x080

; page directory entries (level 2+)
%define PAGE_PDE_LARGEPAGE     0x080
%define PAGE_PDE_PAT         0x01000

%define PAGE_4K_PDE_ATTR (PAGE_ACCESSED + \
                          PAGE_DIRTY + \
                          PAGE_READ_WRITE + \
                          PAGE_PRESENT)

%define PAGE_PDE_LARGEPAGE_ATTR (PAGE_PDE_LARGEPAGE + \
                                 PAGE_ACCESSED + \
                                 PAGE_DIRTY + \
                                 PAGE_READ_WRITE + \
                                 PAGE_PRESENT)

%define PAGE_PDE_DIRECTORY_ATTR (PAGE_ACCESSED + \
                                 PAGE_READ_WRITE + \
                                 PAGE_PRESENT)

%define TDX_BSP         1
%define TDX_AP          2

;
; For OVMF, build some initial page tables at
; PcdOvmfSecPageTablesBase - (PcdOvmfSecPageTablesBase + 0x6000).
;
; This range should match with PcdOvmfSecPageTablesSize which is
; declared in the FDF files.
;
; At the end of PEI, the pages tables will be rebuilt into a
; more permanent location by DxeIpl.
;
%macro ClearOvmfPageTables 0
    mov     ecx, 6 * 0x1000 / 4
    xor     eax, eax
.clearPageTablesMemoryLoop:
    mov     dword[ecx * 4 + PT_ADDR (0) - 4], eax
    loop    .clearPageTablesMemoryLoop
%endmacro

;
; Create page tables for 4-level paging
;
; Argument: upper 32 bits of the page table entries
;
%macro CreatePageTables4Level 1
    ;
    ; Top level Page Directory Pointers (1 * 512GB entry)
    ;
    mov     dword[PT_ADDR (0)], PT_ADDR (0x1000) + PAGE_PDE_DIRECTORY_ATTR
    mov     dword[PT_ADDR (4)], %1

    ;
    ; Next level Page Directory Pointers (4 * 1GB entries => 4GB)
    ;
    mov     dword[PT_ADDR (0x1000)], PT_ADDR (0x2000) + PAGE_PDE_DIRECTORY_ATTR
    mov     dword[PT_ADDR (0x1004)], %1
    mov     dword[PT_ADDR (0x1008)], PT_ADDR (0x3000) + PAGE_PDE_DIRECTORY_ATTR
    mov     dword[PT_ADDR (0x100C)], %1
    mov     dword[PT_ADDR (0x1010)], PT_ADDR (0x4000) + PAGE_PDE_DIRECTORY_ATTR
    mov     dword[PT_ADDR (0x1014)], %1
    mov     dword[PT_ADDR (0x1018)], PT_ADDR (0x5000) + PAGE_PDE_DIRECTORY_ATTR
    mov     dword[PT_ADDR (0x101C)], %1

    ;
    ; Page Table Entries (2048 * 2MB entries => 4GB)
    ;
    mov     ecx, 0x800
.pageTableEntriesLoop4Level:
    mov     eax, ecx
    dec     eax
    shl     eax, 21
    add     eax, PAGE_PDE_LARGEPAGE_ATTR
    mov     dword[ecx * 8 + PT_ADDR (0x2000 - 8)], eax
    mov     dword[(ecx * 8 + PT_ADDR (0x2000 - 8)) + 4], %1
    loop    .pageTableEntriesLoop4Level
%endmacro

;
; Modified:  EAX, EBX, ECX, EDX
;
SetCr3ForPageTables64:
    ; Check the TDX features.
    ; If it is TDX APs, then jump to SetCr3 directly.
    ; In TD guest the initialization is done by BSP, including building
    ; the page tables. APs will spin on until byte[TDX_WORK_AREA_PGTBL_READY]
    ; is set.
    OneTimeCall   CheckTdxFeaturesBeforeBuildPagetables
    cmp       eax, TDX_BSP
    je        ClearOvmfPageTables
    cmp       eax, TDX_AP
    je        SetCr3

    ; Check whether the SEV is active and populate the SevEsWorkArea
    OneTimeCall   CheckSevFeatures

    ; If SEV is enabled, the C-bit position is always above 31.
    ; The mask will be saved in the EDX and applied during the
    ; the page table build below.
    OneTimeCall   GetSevCBitMaskAbove31

ClearOvmfPageTables:
    ClearOvmfPageTables
    CreatePageTables4Level edx

    ; Clear the C-bit from the GHCB page if the SEV-ES is enabled.
    OneTimeCall   SevClearPageEncMaskForGhcbPage

    ; TDX will do some PostBuildPages task, such as setting
    ; byte[TDX_WORK_AREA_PGTBL_READY].
    OneTimeCall   TdxPostBuildPageTables

SetCr3:
    ;
    ; Set CR3 now that the paging structures are available
    ;
    mov     eax, PT_ADDR (0)
    mov     cr3, eax

    OneTimeCallRet SetCr3ForPageTables64
