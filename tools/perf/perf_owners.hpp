#pragma once
// Allocation owners outside the client image for MaiCordPerfProbe: import-table
// hooks in every other loaded module (system DLLs, the GPU driver, the synthetic
// account) record which module allocated each live heap block (process and
// private heaps, LocalAlloc, msvcrt/ucrtbase malloc and operator new), which
// module reserved each VirtualAlloc region and which module created each heap.
// Allocations made before a module's imports are patched, inside ntdll, or
// through GetProcAddress stay unattributed. Nothing here ships.
#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::perf {

inline constexpr u32 kOwnerModuleCapacity = 128;
inline constexpr u32 kOwnerNone = ~0u;

struct OwnerModule {
    u64 base, size;
    wchar_t name[40];
    volatile LONG64 heap_live_bytes, heap_live_blocks, heap_calls;
    volatile LONG64 va_reserve_calls;
    bool patched, excluded;
};

// Process entry, before other threads exist. `allocate` commits zeroed probe
// memory; the tables are reported through `range` for exclusion from the
// client attribution.
bool owners_initialize(void* (*allocate)(u64 bytes), u64* range_begin, u64* range_end) noexcept;
// LdrRegisterDllNotification (loader lock held): remembers the module and
// patches its imports once they are bound.
void owners_module_loaded(void* base, u32 size, const wchar_t* name, u32 name_length) noexcept;
// LdrRegisterDllNotification unload: the module is never scanned again.
void owners_module_unloaded(void* base) noexcept;
// Patches modules whose imports were not yet bound when they were reported.
void owners_scan() noexcept;

u32 owners_module_count() noexcept;
const OwnerModule* owners_module(u32 index) noexcept;
// The module that reserved the region starting at `allocation_base`.
u32 owners_region_owner(u64 allocation_base) noexcept;
// The module that created `heap` (kOwnerNone for the process heap or unknown).
u32 owners_heap_creator(void* heap) noexcept;
// Tags blocks allocated on this thread until cleared (0): the client graphics
// call in progress (perf_startup.cpp numbers them).
inline constexpr u32 kOwnerTagCount = 64;
void owners_set_tag(u32 tag) noexcept;
// Live bytes of `owner` by tag and by size (<1 KiB, <4, <16, <64, <256 KiB,
// <1 MiB, >= 1 MiB); the arrays are added to.
inline constexpr u32 kOwnerSizeBuckets = 7;
void owners_block_totals(u32 owner, u64* bytes_by_tag, u64* bytes_by_size) noexcept;
// Live table entries, entries dropped because the table was full, and
// patched import slots.
void owners_counters(u64* live_entries, u64* dropped, u64* patched_slots) noexcept;

} // namespace maicord::perf
