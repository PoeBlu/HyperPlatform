// Copyright (c) 2015-2016, tandasat. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements EPT functions.

#include "ept.h"
#include "asm.h"
#include "common.h"
#include "log.h"
#include "util.h"
#ifndef HYPERPLATFORM_PERFORMANCE_ENABLE_PERFCOUNTER
#define HYPERPLATFORM_PERFORMANCE_ENABLE_PERFCOUNTER 1
#endif  // HYPERPLATFORM_PERFORMANCE_ENABLE_PERFCOUNTER
#include "performance.h"

extern "C" {
////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

// Followings are how 64bits of a pysical address is used to locate EPT entries:
//
// EPT Page map level 4 selector           9 bits
// EPT Page directory pointer selector     9 bits
// EPT Page directory selector             9 bits
// EPT Page table selector                 9 bits
// EPT Byte within page                   12 bits

// Get the highest 25 bits
static const auto kVmxpPxiShift = 39ull;

// Get the highest 34 bits
static const auto kVmxpPpiShift = 30ull;

// Get the highest 43 bits
static const auto kVmxpPdiShift = 21ull;

// Get the highest 52 bits
static const auto kVmxpPtiShift = 12ull;

// Use 9 bits; 0b0000_0000_0000_0000_0000_0000_0001_1111_1111
static const auto kVmxpPtxMask = 0x1ffull;

// How many EPT entries are preallocated. When the number exceeds it, the
// hypervisor issues a bugcheck.
static const auto kVmxpNumberOfPreallocatedEntries = 50;

////////////////////////////////////////////////////////////////////////////////
//
// types
//

// A structure consists of common fields across all EPT entry types
union EptCommonEntry {
  ULONG64 all;
  struct {
    ULONG64 read_access : 1;       // [0]
    ULONG64 write_access : 1;      // [1]
    ULONG64 execute_access : 1;    // [2]
    ULONG64 memory_type : 3;       // [3:5]
    ULONG64 reserved1 : 6;         // [6:11]
    ULONG64 physial_address : 36;  // [12:48-1]
    ULONG64 reserved2 : 16;        // [48:63]
  } fields;
};
static_assert(sizeof(EptCommonEntry) == 8, "Size check");

struct EptData {
  EptPointer *ept_pointer;
  EptCommonEntry *ept_pml4;

  EptCommonEntry **preallocated_entries;
  volatile long preallocated_entries_count;
};

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

_When_(ept_data == nullptr,
       _IRQL_requires_max_(DISPATCH_LEVEL)) static EptCommonEntry
    *EptpConstructTables(_In_ EptCommonEntry *table, _In_ ULONG table_level,
                         _In_ ULONG64 physical_address,
                         _In_opt_ EptData *ept_data);

static void EptpDestructTables(_In_ EptCommonEntry *table,
                               _In_ ULONG table_level);

_Must_inspect_result_ __drv_allocatesMem(Mem)
    _When_(ept_data == nullptr,
           _IRQL_requires_max_(DISPATCH_LEVEL)) static EptCommonEntry
        *EptpAllocateEptEntry(_In_opt_ EptData *ept_data);

static EptCommonEntry *EptpAllocateEptEntryFromPreAllocated(
    _In_ EptData *ept_data);

_Must_inspect_result_ __drv_allocatesMem(Mem) _IRQL_requires_max_(
    DISPATCH_LEVEL) static EptCommonEntry *EptpAllocateEptEntryFromPool();

static void EptpInitTableEntry(_In_ EptCommonEntry *Entry,
                               _In_ ULONG table_level,
                               _In_ ULONG64 physical_address);

static ULONG64 EptpAddressToPxeIndex(_In_ ULONG64 physical_address);

static ULONG64 EptpAddressToPpeIndex(_In_ ULONG64 physical_address);

static ULONG64 EptpAddressToPdeIndex(_In_ ULONG64 physical_address);

static ULONG64 EptpAddressToPteIndex(_In_ ULONG64 physical_address);

static bool EptpIsDeviceMemory(_In_ ULONG64 physical_address);

static EptCommonEntry *EptpGetEptPtEntry(_In_ EptCommonEntry *table,
                                         _In_ ULONG table_level,
                                         _In_ ULONG64 physical_address);

static bool EptpIsCopiedKiInterruptTemplate(_In_ void *virtual_address);

_IRQL_requires_min_(DISPATCH_LEVEL) static void EptpAddDisabledEntry(
    _In_ EptData *ept_data, _In_ EptCommonEntry *ept_entry);

_IRQL_requires_min_(DISPATCH_LEVEL) static void EptpResetDisabledEntriesUnsafe(
    _In_ EptData *ept_data);

static void EptpFreeUnusedPreAllocatedEntries(
    _Pre_notnull_ __drv_freesMem(Mem) EptCommonEntry **preallocated_entries,
    _In_ long used_count);

#if defined(ALLOC_PRAGMA)
#pragma alloc_text(INIT, EptIsEptAvailable)
#pragma alloc_text(INIT, EptGetEptPointer)
#pragma alloc_text(INIT, EptInitialization)
#endif

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

_Use_decl_annotations_ bool EptIsEptAvailable() {
  PAGED_CODE();

  int regs[4] = {};
  __cpuidex(regs, 0x80000008, 0);
  Cpuid80000008Eax cpuidEax = {static_cast<ULONG32>(regs[0])};
  HYPERPLATFORM_LOG_DEBUG("Physical Address Range = %d bits",
                           cpuidEax.fields.physical_address_bits);

  // No processors supporting the Intel 64 architecture support more than 48
  // physical-address bits
  if (cpuidEax.fields.physical_address_bits > 48) {
    return false;
  }

  // page walk length is 4 steps
  // extended page tables can be laid out in write-back memory
  // INVEPT instruction with all possible types is supported
  Ia32VmxEptVpidCapMsr vpid = {UtilReadMsr64(Msr::kIa32VmxEptVpidCap)};
  if (!vpid.fields.support_page_walk_length4 ||
      !vpid.fields.support_write_back_memory_type ||
      !vpid.fields.support_invept ||
      !vpid.fields.support_single_context_invept ||
      !vpid.fields.support_all_context_invept) {
    return false;
  }
  return true;
}

_Use_decl_annotations_ ULONG64 EptGetEptPointer(EptData *ept_data) {
  return ept_data->ept_pointer->all;
}

_Use_decl_annotations_ EptData *EptInitialization() {
  PAGED_CODE();

  static const auto kEptPageWalkLevel = 4ul;

  // Allocate ept_data
  const auto ept_data = reinterpret_cast<EptData *>(ExAllocatePoolWithTag(
      NonPagedPoolNx, sizeof(EptData), kHyperPlatformCommonPoolTag));
  if (!ept_data) {
    return nullptr;
  }
  RtlZeroMemory(ept_data, sizeof(EptData));

  // Allocate EptPointer
  const auto ept_poiner = reinterpret_cast<EptPointer *>(ExAllocatePoolWithTag(
      NonPagedPoolNx, PAGE_SIZE, kHyperPlatformCommonPoolTag));
  if (!ept_poiner) {
    ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
    return nullptr;
  }
  RtlZeroMemory(ept_poiner, PAGE_SIZE);

  // Allocate EPT_PML4 and initialize EptPointer
  const auto ept_pml4 =
      reinterpret_cast<EptCommonEntry *>(ExAllocatePoolWithTag(
          NonPagedPoolNx, PAGE_SIZE, kHyperPlatformCommonPoolTag));
  if (!ept_pml4) {
    ExFreePoolWithTag(ept_poiner, kHyperPlatformCommonPoolTag);
    ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
    return nullptr;
  }
  RtlZeroMemory(ept_pml4, PAGE_SIZE);
  ept_poiner->fields.memory_type =
      static_cast<ULONG64>(memory_type::kWriteBack);
  ept_poiner->fields.page_walk_length = kEptPageWalkLevel - 1;
  ept_poiner->fields.pml4_address = UtilPfnFromPa(UtilPaFromVa(ept_pml4));

  // Initialize all EPT entries for all physical memory pages
  const auto pm_ranges = UtilGetPhysicalMemoryRanges();
  for (auto run_index = 0ul; run_index < pm_ranges->number_of_runs;
       ++run_index) {
    const auto run = &pm_ranges->run[run_index];
    const auto base_addr = run->base_page * PAGE_SIZE;
    for (auto page_index = 0ull; page_index < run->page_count; ++page_index) {
      const auto indexed_addr = base_addr + page_index * PAGE_SIZE;
      const auto ept_pt_entry =
          EptpConstructTables(ept_pml4, 4, indexed_addr, nullptr);
      if (!ept_pt_entry) {
        EptpDestructTables(ept_pml4, 4);
        ExFreePoolWithTag(ept_poiner, kHyperPlatformCommonPoolTag);
        ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
        return nullptr;
      }
    }
  }

  // Initialize an EPT entry for APIC_BASE. It is required to allocated it now
  // for some reasons, or else, system hangs.
  const Ia32ApicBaseMsr apic_msr = {UtilReadMsr64(Msr::kIa32ApicBase)};
  if (!EptpConstructTables(ept_pml4, 4, apic_msr.fields.apic_base * PAGE_SIZE,
                           nullptr)) {
    EptpDestructTables(ept_pml4, 4);
    ExFreePoolWithTag(ept_poiner, kHyperPlatformCommonPoolTag);
    ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
    return nullptr;
  }

  // Allocate preallocated_entries
  const auto preallocated_entries_size =
      sizeof(EptCommonEntry *) * kVmxpNumberOfPreallocatedEntries;
  const auto preallocated_entries = reinterpret_cast<EptCommonEntry **>(
      ExAllocatePoolWithTag(NonPagedPoolNx, preallocated_entries_size,
                            kHyperPlatformCommonPoolTag));
  if (!preallocated_entries) {
    EptpDestructTables(ept_pml4, 4);
    ExFreePoolWithTag(ept_poiner, kHyperPlatformCommonPoolTag);
    ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
    return nullptr;
  }
  RtlZeroMemory(preallocated_entries, preallocated_entries_size);

  // And fill preallocated_entries with newly created entries
  for (auto i = 0ul; i < kVmxpNumberOfPreallocatedEntries; ++i) {
    const auto ept_entry = EptpAllocateEptEntry(nullptr);
    if (!ept_entry) {
      EptpFreeUnusedPreAllocatedEntries(preallocated_entries, 0);
      EptpDestructTables(ept_pml4, 4);
      ExFreePoolWithTag(ept_poiner, kHyperPlatformCommonPoolTag);
      ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
      return nullptr;
    }
    preallocated_entries[i] = ept_entry;
  }

  // Initialization completed
  ept_data->ept_pointer = ept_poiner;
  ept_data->ept_pml4 = ept_pml4;
  ept_data->preallocated_entries = preallocated_entries;
  ept_data->preallocated_entries_count = 0;
  return ept_data;
}

// Allocate and initialize all EPT entries associated with the physical_address
_Use_decl_annotations_ static EptCommonEntry *EptpConstructTables(
    EptCommonEntry *table, ULONG table_level, ULONG64 physical_address,
    EptData *ept_data) {
  switch (table_level) {
    case 4: {
      // table == PML4 (512 GB)
      const auto pxe_index = EptpAddressToPxeIndex(physical_address);
      const auto ept_pml4_entry = &table[pxe_index];
      if (!ept_pml4_entry->all) {
        const auto ept_pdpt = EptpAllocateEptEntry(ept_data);
        if (!ept_pdpt) {
          return nullptr;
        }
        EptpInitTableEntry(ept_pml4_entry, table_level, UtilPaFromVa(ept_pdpt));
      }
      return EptpConstructTables(
          reinterpret_cast<EptCommonEntry *>(
              UtilVaFromPfn(ept_pml4_entry->fields.physial_address)),
          table_level - 1, physical_address, ept_data);
    }
    case 3: {
      // table == PDPT (1 GB)
      const auto ppe_index = EptpAddressToPpeIndex(physical_address);
      const auto ept_pdpt_entry = &table[ppe_index];
      if (!ept_pdpt_entry->all) {
        const auto ept_pdt = EptpAllocateEptEntry(ept_data);
        if (!ept_pdt) {
          return nullptr;
        }
        EptpInitTableEntry(ept_pdpt_entry, table_level, UtilPaFromVa(ept_pdt));
      }
      return EptpConstructTables(
          reinterpret_cast<EptCommonEntry *>(
              UtilVaFromPfn(ept_pdpt_entry->fields.physial_address)),
          table_level - 1, physical_address, ept_data);
    }
    case 2: {
      // table == PDT (2 MB)
      const auto pde_index = EptpAddressToPdeIndex(physical_address);
      const auto ept_pdt_entry = &table[pde_index];
      if (!ept_pdt_entry->all) {
        const auto ept_pt = EptpAllocateEptEntry(ept_data);
        if (!ept_pt) {
          return nullptr;
        }
        EptpInitTableEntry(ept_pdt_entry, table_level, UtilPaFromVa(ept_pt));
      }
      return EptpConstructTables(
          reinterpret_cast<EptCommonEntry *>(
              UtilVaFromPfn(ept_pdt_entry->fields.physial_address)),
          table_level - 1, physical_address, ept_data);
    }
    case 1: {
      // table == PT (4 KB)
      const auto pte_index = EptpAddressToPteIndex(physical_address);
      const auto ept_pt_entry = &table[pte_index];
      NT_ASSERT(!ept_pt_entry->all);
      EptpInitTableEntry(ept_pt_entry, table_level, physical_address);
      return ept_pt_entry;
    }
    default:
      HYPERPLATFORM_COMMON_DBG_BREAK();
      return nullptr;
  }
}

// Return a new EPT entry either by creating new one or from pre-allocated ones
_Use_decl_annotations_ static EptCommonEntry *EptpAllocateEptEntry(
    EptData *ept_data) {
  if (ept_data) {
    return EptpAllocateEptEntryFromPreAllocated(ept_data);
  } else {
    return EptpAllocateEptEntryFromPool();
  }
}

// Return a new EPT entry from pre-allocated ones.
_Use_decl_annotations_ static EptCommonEntry *
EptpAllocateEptEntryFromPreAllocated(EptData *ept_data) {
  const auto count =
      InterlockedIncrement(&ept_data->preallocated_entries_count);
  if (count > kVmxpNumberOfPreallocatedEntries) {
    HYPERPLATFORM_COMMON_BUG_CHECK(
        HyperPlatformBugCheck::kExhaustedPreallocatedEntries, count,
        reinterpret_cast<ULONG_PTR>(ept_data), 0);
  }
  return ept_data->preallocated_entries[count - 1];
}

// Return a new EPT entry either by creating new one
_Use_decl_annotations_ static EptCommonEntry *EptpAllocateEptEntryFromPool() {
  static const auto kAllocSize = 512 * sizeof(EptCommonEntry);
  static_assert(kAllocSize == PAGE_SIZE, "Size check");

  const auto entry = reinterpret_cast<EptCommonEntry *>(ExAllocatePoolWithTag(
      NonPagedPoolNx, kAllocSize, kHyperPlatformCommonPoolTag));
  if (!entry) {
    return nullptr;
  }
  RtlZeroMemory(entry, kAllocSize);
  return entry;
}

// Initialize an EPT entry with a "pass through" attribute
_Use_decl_annotations_ static void EptpInitTableEntry(
    EptCommonEntry *entry, ULONG table_level, ULONG64 physical_address) {
  entry->fields.read_access = true;
  entry->fields.write_access = true;
  entry->fields.execute_access = true;
  entry->fields.physial_address = UtilPfnFromPa(physical_address);
  if (table_level == 1) {
    entry->fields.memory_type = static_cast<ULONG64>(memory_type::kWriteBack);
  }
}

// Return an address of PXE
_Use_decl_annotations_ static ULONG64 EptpAddressToPxeIndex(
    ULONG64 physical_address) {
  const auto index = (physical_address >> kVmxpPxiShift) & kVmxpPtxMask;
  return index;
}

// Return an address of PPE
_Use_decl_annotations_ static ULONG64 EptpAddressToPpeIndex(
    ULONG64 physical_address) {
  const auto index = (physical_address >> kVmxpPpiShift) & kVmxpPtxMask;
  return index;
}

// Return an address of PDE
_Use_decl_annotations_ static ULONG64 EptpAddressToPdeIndex(
    ULONG64 physical_address) {
  const auto index = (physical_address >> kVmxpPdiShift) & kVmxpPtxMask;
  return index;
}

// Return an address of PTE
_Use_decl_annotations_ static ULONG64 EptpAddressToPteIndex(
    ULONG64 physical_address) {
  const auto index = (physical_address >> kVmxpPtiShift) & kVmxpPtxMask;
  return index;
}

// Deal with EPT violation VM-exit.
_Use_decl_annotations_ void EptHandleEptViolation(EptData *ept_data) {
  const EptViolationQualification exit_qualification = {
      UtilVmRead(VmcsField::kExitQualification)};

  const auto fault_pa = UtilVmRead64(VmcsField::kGuestPhysicalAddress);
  const auto fault_va =
      exit_qualification.fields.valid_guest_linear_address
          ? reinterpret_cast<void *>(UtilVmRead(VmcsField::kGuestLinearAddress))
          : nullptr;

  if (!exit_qualification.fields.ept_readable &&
      !exit_qualification.fields.ept_writeable &&
      !exit_qualification.fields.ept_executable) {
    // EPT entry miss. It should be device memory.
    HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
    // HYPERPLATFORM_LOG_DEBUG_SAFE(
    //    "[INIT] Dev VA = %p, PA = %016llx, Used = %d",
    //    0, fault_pa, ept_data->preallocated_entries_count);

    if (!IsReleaseBuild()) {
      const auto is_device_memory = EptpIsDeviceMemory(fault_pa);
      NT_ASSERT(is_device_memory);
      UNREFERENCED_PARAMETER(is_device_memory);
    }

    // There is a race condition here. If multiple processors reach this code
    // with the same fault_pa, this function may create multiple EPT entries for
    // one physical address and leads memory leak. This call should probably be
    // guarded by a spin-lock but is not yet just because impact is so small.
    EptpConstructTables(ept_data->ept_pml4, 4, fault_pa, ept_data);

    UtilInveptAll();

  } else {
    HYPERPLATFORM_LOG_DEBUG_SAFE("[IGNR] OTH VA = %p, PA = %016llx", fault_va,
                                  fault_pa);
  }
}

// Returns if the physical_address is device memory (which could not have a
// corresponding PFN entry)
_Use_decl_annotations_ static bool EptpIsDeviceMemory(
    ULONG64 physical_address) {
  const auto pm_ranges = UtilGetPhysicalMemoryRanges();
  for (auto i = 0ul; i < pm_ranges->number_of_runs; ++i) {
    const auto current_run = &pm_ranges->run[i];
    const auto base_addr =
        static_cast<ULONG64>(current_run->base_page) * PAGE_SIZE;
    const auto endAddr = base_addr + current_run->page_count * PAGE_SIZE - 1;
    if (UtilIsInBounds(physical_address, base_addr, endAddr)) {
      return false;
    }
  }
  return true;
}

// Returns an EPT entry corresponds to the physical_address
_Use_decl_annotations_ static EptCommonEntry *EptpGetEptPtEntry(
    EptCommonEntry *table, ULONG table_level, ULONG64 physical_address) {
  switch (table_level) {
    case 4: {
      // table == PML4
      const auto pxe_index = EptpAddressToPxeIndex(physical_address);
      const auto ept_pml4_entry = &table[pxe_index];
      return EptpGetEptPtEntry(reinterpret_cast<EptCommonEntry *>(UtilVaFromPfn(
                                   ept_pml4_entry->fields.physial_address)),
                               table_level - 1, physical_address);
    }
    case 3: {
      // table == PDPT
      const auto ppe_index = EptpAddressToPpeIndex(physical_address);
      const auto ept_pdpt_entry = &table[ppe_index];
      return EptpGetEptPtEntry(reinterpret_cast<EptCommonEntry *>(UtilVaFromPfn(
                                   ept_pdpt_entry->fields.physial_address)),
                               table_level - 1, physical_address);
    }
    case 2: {
      // table == PDT
      const auto pde_index = EptpAddressToPdeIndex(physical_address);
      const auto ept_pdt_entry = &table[pde_index];
      return EptpGetEptPtEntry(reinterpret_cast<EptCommonEntry *>(UtilVaFromPfn(
                                   ept_pdt_entry->fields.physial_address)),
                               table_level - 1, physical_address);
    }
    case 1: {
      // table == PT
      const auto pte_index = EptpAddressToPteIndex(physical_address);
      const auto ept_pt_entry = &table[pte_index];
      return ept_pt_entry;
    }
    default:
      HYPERPLATFORM_COMMON_DBG_BREAK();
      return nullptr;
  }
}

// Frees all EPT stuff
_Use_decl_annotations_ void EptTermination(EptData *ept_data) {
  HYPERPLATFORM_LOG_DEBUG("Used pre-allocated entries  = %2d / %2d",
                          ept_data->preallocated_entries_count,
                          kVmxpNumberOfPreallocatedEntries);

  EptpFreeUnusedPreAllocatedEntries(ept_data->preallocated_entries,
                                    ept_data->preallocated_entries_count);
  EptpDestructTables(ept_data->ept_pml4, 4);
  ExFreePoolWithTag(ept_data->ept_pointer, kHyperPlatformCommonPoolTag);
  ExFreePoolWithTag(ept_data, kHyperPlatformCommonPoolTag);
}

// Frees all unused pre-allocated EPT entries. Other used entries should be
// freed with EptpDestructTables().
_Use_decl_annotations_ static void EptpFreeUnusedPreAllocatedEntries(
    EptCommonEntry **preallocated_entries, long used_count) {
  for (auto i = used_count; i < kVmxpNumberOfPreallocatedEntries; ++i) {
    if (!preallocated_entries[i]) {
      break;
    }
#pragma warning(push)
#pragma warning(disable : 6001)
    ExFreePoolWithTag(preallocated_entries[i], kHyperPlatformCommonPoolTag);
#pragma warning(pop)
  }
  ExFreePoolWithTag(preallocated_entries, kHyperPlatformCommonPoolTag);
}

// Frees all used EPT entries by walking through whole EPT
_Use_decl_annotations_ static void EptpDestructTables(EptCommonEntry *table,
                                                      ULONG table_level) {
  for (auto i = 0ul; i < 512; ++i) {
    const auto entry = table[i];
    if (entry.fields.physial_address) {
      const auto sub_table = reinterpret_cast<EptCommonEntry *>(
          UtilVaFromPfn(entry.fields.physial_address));

      switch (table_level) {
        case 4:  // table == PML4, sub_table == PDPT
        case 3:  // table == PDPT, sub_table == PDT
          EptpDestructTables(sub_table, table_level - 1);
          break;
        case 2:  // table == PDT, sub_table == PT
          ExFreePoolWithTag(sub_table, kHyperPlatformCommonPoolTag);
          break;
        default:
          HYPERPLATFORM_COMMON_DBG_BREAK();
          break;
      }
    }
  }
  ExFreePoolWithTag(table, kHyperPlatformCommonPoolTag);
}

}  // extern "C"
