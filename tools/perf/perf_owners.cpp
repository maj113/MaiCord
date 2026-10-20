// Allocation owners outside the client image (see perf_owners.hpp). Nothing here ships.
#include "perf_owners.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/intrin.hpp"
#include "maicord/sys/process_info.hpp"
#include "maicord/core/win32_image.hpp"

namespace maicord::perf {
namespace {

using HeapAllocFn = void*(WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapReAllocFn = void*(WINAPI*)(HANDLE, DWORD, void*, SIZE_T);
using HeapFreeFn = BOOL(WINAPI*)(HANDLE, DWORD, void*);
using HeapCreateFn = HANDLE(WINAPI*)(DWORD, SIZE_T, SIZE_T);
using HeapDestroyFn = BOOL(WINAPI*)(HANDLE);
using VirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, DWORD);
using VirtualFreeFn = BOOL(WINAPI*)(void*, SIZE_T, DWORD);
using LocalAllocFn = HLOCAL(WINAPI*)(UINT, SIZE_T);
using LocalReAllocFn = HLOCAL(WINAPI*)(HLOCAL, SIZE_T, UINT);
using LocalFreeFn = HLOCAL(WINAPI*)(HLOCAL);
using MallocFn = void*(__cdecl*)(usize);
using CallocFn = void*(__cdecl*)(usize, usize);
using ReallocFn = void*(__cdecl*)(void*, usize);
using FreeFn = void(__cdecl*)(void*);
using AlignedMallocFn = void*(__cdecl*)(usize, usize);

OwnerModule* g_modules = nullptr;
volatile LONG g_module_count = 0;
volatile LONG g_pending = 0;
volatile LONG g_scanning = 0;
// Scans hold it shared; an unload takes it exclusively before the image goes.
SRWLOCK g_module_lock = SRWLOCK_INIT;
u64 g_patched_slots = 0;

// Live blocks: open addressing on the block address with backward-shift
// deletion. size 0 marks an empty slot (zero-byte blocks are stored as 1).
struct LiveBlock {
    u64 address;
    u32 bytes;
    u16 owner;
    u8 tag;
    u8 unused;
};
constexpr u32 kLiveCapacity = 1u << 17;
LiveBlock* g_live = nullptr;
u64 g_live_count = 0;
u64 g_live_dropped = 0;
SRWLOCK g_live_lock = SRWLOCK_INIT;

// Region bases and heap handles by owner (small sorted arrays).
struct OwnedBase {
    u64 base;
    u32 owner;
};
constexpr u32 kRegionCapacity = 8192;
OwnedBase* g_regions = nullptr;
u32 g_region_count = 0;
constexpr u32 kHeapCapacity = 64;
OwnedBase g_heaps[kHeapCapacity];
u32 g_heap_count = 0;
SRWLOCK g_base_lock = SRWLOCK_INIT;

// The client graphics call in progress on up to eight threads.
struct TagSlot {
    volatile LONG thread;
    volatile LONG tag;
};
constexpr u32 kTagSlots = 8;
TagSlot g_tags[kTagSlots];

u8 current_tag() noexcept {
    const LONG thread = static_cast<LONG>(GetCurrentThreadId());
    for (TagSlot& slot : g_tags)
        if (slot.thread == thread)
            return static_cast<u8>(slot.tag);
    return 0;
}

u32 owner_of(const void* address) noexcept {
    const u64 at = reinterpret_cast<u64>(address);
    const u32 count = static_cast<u32>(g_module_count);
    for (u32 i = 0; i < count; ++i)
        if (at - g_modules[i].base < g_modules[i].size)
            return i;
    return kOwnerNone;
}

u32 live_slot(u64 address) noexcept {
    return static_cast<u32>((address >> 4) * 0x9E3779B97F4A7C15ull >> 47) & (kLiveCapacity - 1);
}

void live_insert(void* address, usize bytes, u32 owner) noexcept {
    if (!address || owner == kOwnerNone || !g_live)
        return;
    const u64 key = reinterpret_cast<u64>(address);
    const u32 stored = static_cast<u32>(bytes > 0xFFFF'FFFFull ? 0xFFFF'FFFFull : bytes ? bytes : 1);
    AcquireSRWLockExclusive(&g_live_lock);
    if (g_live_count * 4 >= kLiveCapacity * 3) {
        ++g_live_dropped;
    } else {
        u32 slot = live_slot(key);
        while (g_live[slot].bytes && g_live[slot].address != key)
            slot = (slot + 1) & (kLiveCapacity - 1);
        if (g_live[slot].bytes) {
            // A block freed through an unhooked path was reused.
            OwnerModule& previous = g_modules[g_live[slot].owner];
            InterlockedAdd64(&previous.heap_live_bytes, -static_cast<LONG64>(g_live[slot].bytes));
            InterlockedAdd64(&previous.heap_live_blocks, -1);
        } else {
            ++g_live_count;
        }
        g_live[slot] = {key, stored, static_cast<u16>(owner), current_tag(), 0};
        InterlockedAdd64(&g_modules[owner].heap_live_bytes, stored);
        InterlockedAdd64(&g_modules[owner].heap_live_blocks, 1);
    }
    InterlockedAdd64(&g_modules[owner].heap_calls, 1);
    ReleaseSRWLockExclusive(&g_live_lock);
}

// Removes a block and returns its owner (kOwnerNone when untracked).
u32 live_remove(const void* address) noexcept {
    if (!address || !g_live)
        return kOwnerNone;
    const u64 key = reinterpret_cast<u64>(address);
    u32 owner = kOwnerNone;
    AcquireSRWLockExclusive(&g_live_lock);
    u32 slot = live_slot(key);
    while (g_live[slot].bytes && g_live[slot].address != key)
        slot = (slot + 1) & (kLiveCapacity - 1);
    if (g_live[slot].bytes) {
        owner = g_live[slot].owner;
        InterlockedAdd64(&g_modules[owner].heap_live_bytes, -static_cast<LONG64>(g_live[slot].bytes));
        InterlockedAdd64(&g_modules[owner].heap_live_blocks, -1);
        --g_live_count;
        // Backward-shift deletion keeps every probe chain unbroken.
        u32 hole = slot;
        for (u32 next = (slot + 1) & (kLiveCapacity - 1); g_live[next].bytes;
             next = (next + 1) & (kLiveCapacity - 1)) {
            const u32 home = live_slot(g_live[next].address);
            const bool movable =
                hole <= next ? (home <= hole || home > next) : (home <= hole && home > next);
            if (movable) {
                g_live[hole] = g_live[next];
                hole = next;
            }
        }
        g_live[hole] = {};
    }
    ReleaseSRWLockExclusive(&g_live_lock);
    return owner;
}

void base_insert(OwnedBase* table, u32* count, u32 capacity, u64 base, u32 owner) noexcept {
    AcquireSRWLockExclusive(&g_base_lock);
    u32 at = 0;
    while (at < *count && table[at].base < base)
        ++at;
    if (at < *count && table[at].base == base) {
        table[at].owner = owner;
    } else if (*count < capacity) {
        for (u32 i = *count; i > at; --i)
            table[i] = table[i - 1];
        table[at] = {base, owner};
        ++*count;
    }
    ReleaseSRWLockExclusive(&g_base_lock);
}

void base_remove(OwnedBase* table, u32* count, u64 base) noexcept {
    AcquireSRWLockExclusive(&g_base_lock);
    for (u32 at = 0; at < *count; ++at)
        if (table[at].base == base) {
            for (u32 i = at + 1; i < *count; ++i)
                table[i - 1] = table[i];
            --*count;
            break;
        }
    ReleaseSRWLockExclusive(&g_base_lock);
}

u32 base_find(const OwnedBase* table, u32 count, u64 base) noexcept {
    u32 low = 0, high = count;
    while (low < high) {
        const u32 mid = (low + high) / 2;
        if (table[mid].base < base)
            low = mid + 1;
        else
            high = mid;
    }
    return low < count && table[low].base == base ? table[low].owner : kOwnerNone;
}

// One hook per (export, provider): CRT pointers must return to the CRT that
// allocated them, so msvcrt and ucrtbase each have their own set.
enum class Provider : u8 {
    any,
    msvcrt,
    ucrt
};
struct Hook {
    const char* name;
    void* replacement;
    void* original;
    Provider provider;
};

#define OWNER_CALLER owner_of(_ReturnAddress())
void scan_if_pending() noexcept;

// Process and private heaps (kernel32 HeapAlloc forwards to ntdll).
HeapAllocFn g_heap_alloc = nullptr;
HeapReAllocFn g_heap_realloc = nullptr;
HeapFreeFn g_heap_free = nullptr;
HeapCreateFn g_heap_create = nullptr;
HeapDestroyFn g_heap_destroy = nullptr;
VirtualAllocFn g_virtual_alloc = nullptr;
VirtualFreeFn g_virtual_free = nullptr;
LocalAllocFn g_local_alloc = nullptr;
LocalReAllocFn g_local_realloc = nullptr;
LocalFreeFn g_local_free = nullptr;

void* WINAPI hook_heap_alloc(HANDLE heap, DWORD flags, SIZE_T bytes) noexcept {
    scan_if_pending();
    void* result = g_heap_alloc(heap, flags, bytes);
    live_insert(result, bytes, OWNER_CALLER);
    return result;
}
void* WINAPI hook_heap_realloc(HANDLE heap, DWORD flags, void* memory, SIZE_T bytes) noexcept {
    const u32 caller = OWNER_CALLER;
    void* result = g_heap_realloc(heap, flags, memory, bytes);
    if (result) {
        const u32 owner = live_remove(memory);
        live_insert(result, bytes, owner != kOwnerNone ? owner : caller);
    }
    return result;
}
BOOL WINAPI hook_heap_free(HANDLE heap, DWORD flags, void* memory) noexcept {
    live_remove(memory);
    return g_heap_free(heap, flags, memory);
}
HANDLE WINAPI hook_heap_create(DWORD options, SIZE_T initial, SIZE_T maximum) noexcept {
    HANDLE heap = g_heap_create(options, initial, maximum);
    const u32 owner = OWNER_CALLER;
    if (heap && owner != kOwnerNone)
        base_insert(g_heaps, &g_heap_count, kHeapCapacity, reinterpret_cast<u64>(heap), owner);
    return heap;
}
BOOL WINAPI hook_heap_destroy(HANDLE heap) noexcept {
    base_remove(g_heaps, &g_heap_count, reinterpret_cast<u64>(heap));
    return g_heap_destroy(heap);
}
void* WINAPI hook_virtual_alloc(void* address, SIZE_T bytes, DWORD type, DWORD protect) noexcept {
    void* result = g_virtual_alloc(address, bytes, type, protect);
    const u32 owner = OWNER_CALLER;
    if (result && !address && (type & MEM_RESERVE) && owner != kOwnerNone && g_regions) {
        base_insert(g_regions, &g_region_count, kRegionCapacity, reinterpret_cast<u64>(result), owner);
        InterlockedAdd64(&g_modules[owner].va_reserve_calls, 1);
    }
    return result;
}
BOOL WINAPI hook_virtual_free(void* address, SIZE_T bytes, DWORD type) noexcept {
    if ((type & MEM_RELEASE) && g_regions)
        base_remove(g_regions, &g_region_count, reinterpret_cast<u64>(address));
    return g_virtual_free(address, bytes, type);
}
HLOCAL WINAPI hook_local_alloc(UINT flags, SIZE_T bytes) noexcept {
    HLOCAL result = g_local_alloc(flags, bytes);
    live_insert(result, bytes, OWNER_CALLER);
    return result;
}
HLOCAL WINAPI hook_local_realloc(HLOCAL memory, SIZE_T bytes, UINT flags) noexcept {
    const u32 caller = OWNER_CALLER;
    HLOCAL result = g_local_realloc(memory, bytes, flags);
    if (result) {
        const u32 owner = live_remove(memory);
        live_insert(result, bytes, owner != kOwnerNone ? owner : caller);
    }
    return result;
}
HLOCAL WINAPI hook_local_free(HLOCAL memory) noexcept {
    live_remove(memory);
    return g_local_free(memory);
}

// CRT allocators: one instance per CRT DLL.
template <Provider P> struct Crt {
    static inline MallocFn malloc_fn = nullptr;
    static inline CallocFn calloc_fn = nullptr;
    static inline ReallocFn realloc_fn = nullptr;
    static inline FreeFn free_fn = nullptr;
    static inline MallocFn new_fn = nullptr;
    static inline MallocFn new_array_fn = nullptr;
    static inline FreeFn delete_fn = nullptr;
    static inline FreeFn delete_array_fn = nullptr;
    static inline AlignedMallocFn aligned_malloc_fn = nullptr;
    static inline FreeFn aligned_free_fn = nullptr;

    static void* __cdecl hook_malloc(usize bytes) noexcept {
        void* result = malloc_fn(bytes);
        live_insert(result, bytes, OWNER_CALLER);
        return result;
    }
    static void* __cdecl hook_calloc(usize count, usize bytes) noexcept {
        void* result = calloc_fn(count, bytes);
        live_insert(result, count * bytes, OWNER_CALLER);
        return result;
    }
    static void* __cdecl hook_realloc(void* memory, usize bytes) noexcept {
        const u32 caller = OWNER_CALLER;
        void* result = realloc_fn(memory, bytes);
        if (result || !bytes) {
            const u32 owner = live_remove(memory);
            live_insert(result, bytes, owner != kOwnerNone ? owner : caller);
        }
        return result;
    }
    static void __cdecl hook_free(void* memory) noexcept {
        live_remove(memory);
        free_fn(memory);
    }
    static void* __cdecl hook_new(usize bytes) noexcept {
        void* result = new_fn(bytes);
        live_insert(result, bytes, OWNER_CALLER);
        return result;
    }
    static void* __cdecl hook_new_array(usize bytes) noexcept {
        void* result = new_array_fn(bytes);
        live_insert(result, bytes, OWNER_CALLER);
        return result;
    }
    static void __cdecl hook_delete(void* memory) noexcept {
        live_remove(memory);
        delete_fn(memory);
    }
    static void __cdecl hook_delete_array(void* memory) noexcept {
        live_remove(memory);
        delete_array_fn(memory);
    }
    static void* __cdecl hook_aligned_malloc(usize bytes, usize alignment) noexcept {
        void* result = aligned_malloc_fn(bytes, alignment);
        live_insert(result, bytes, OWNER_CALLER);
        return result;
    }
    static void __cdecl hook_aligned_free(void* memory) noexcept {
        live_remove(memory);
        aligned_free_fn(memory);
    }
};

#define OWNER_HOOK(name, function, original, provider)                                                     \
    {name, reinterpret_cast<void*>(function), reinterpret_cast<void*>(&original), provider}
#define OWNER_CRT(P)                                                                                       \
    OWNER_HOOK("malloc", Crt<P>::hook_malloc, Crt<P>::malloc_fn, P),                                       \
        OWNER_HOOK("calloc", Crt<P>::hook_calloc, Crt<P>::calloc_fn, P),                                   \
        OWNER_HOOK("realloc", Crt<P>::hook_realloc, Crt<P>::realloc_fn, P),                                \
        OWNER_HOOK("free", Crt<P>::hook_free, Crt<P>::free_fn, P),                                         \
        OWNER_HOOK("_malloc_base", Crt<P>::hook_malloc, Crt<P>::malloc_fn, P),                             \
        OWNER_HOOK("_calloc_base", Crt<P>::hook_calloc, Crt<P>::calloc_fn, P),                             \
        OWNER_HOOK("_realloc_base", Crt<P>::hook_realloc, Crt<P>::realloc_fn, P),                          \
        OWNER_HOOK("_free_base", Crt<P>::hook_free, Crt<P>::free_fn, P),                                   \
        OWNER_HOOK("??2@YAPEAX_K@Z", Crt<P>::hook_new, Crt<P>::new_fn, P),                                 \
        OWNER_HOOK("??_U@YAPEAX_K@Z", Crt<P>::hook_new_array, Crt<P>::new_array_fn, P),                    \
        OWNER_HOOK("??3@YAXPEAX@Z", Crt<P>::hook_delete, Crt<P>::delete_fn, P),                            \
        OWNER_HOOK("??_V@YAXPEAX@Z", Crt<P>::hook_delete_array, Crt<P>::delete_array_fn, P),               \
        OWNER_HOOK("_aligned_malloc", Crt<P>::hook_aligned_malloc, Crt<P>::aligned_malloc_fn, P),          \
        OWNER_HOOK("_aligned_free", Crt<P>::hook_aligned_free, Crt<P>::aligned_free_fn, P)

// "malloc" and "_malloc_base" share one original: a slot is patched only when
// it already points at that original, so both names must resolve to it.
Hook g_hooks[] = {
    OWNER_HOOK("HeapAlloc", hook_heap_alloc, g_heap_alloc, Provider::any),
    OWNER_HOOK("RtlAllocateHeap", hook_heap_alloc, g_heap_alloc, Provider::any),
    OWNER_HOOK("HeapReAlloc", hook_heap_realloc, g_heap_realloc, Provider::any),
    OWNER_HOOK("RtlReAllocateHeap", hook_heap_realloc, g_heap_realloc, Provider::any),
    OWNER_HOOK("HeapFree", hook_heap_free, g_heap_free, Provider::any),
    OWNER_HOOK("RtlFreeHeap", hook_heap_free, g_heap_free, Provider::any),
    OWNER_HOOK("HeapCreate", hook_heap_create, g_heap_create, Provider::any),
    OWNER_HOOK("HeapDestroy", hook_heap_destroy, g_heap_destroy, Provider::any),
    OWNER_HOOK("VirtualAlloc", hook_virtual_alloc, g_virtual_alloc, Provider::any),
    OWNER_HOOK("VirtualFree", hook_virtual_free, g_virtual_free, Provider::any),
    OWNER_HOOK("LocalAlloc", hook_local_alloc, g_local_alloc, Provider::any),
    OWNER_HOOK("LocalReAlloc", hook_local_realloc, g_local_realloc, Provider::any),
    OWNER_HOOK("LocalFree", hook_local_free, g_local_free, Provider::any),
    OWNER_CRT(Provider::msvcrt),
    OWNER_CRT(Provider::ucrt),
};

bool same_name(const char* a, const char* b) noexcept {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

bool wide_has(const wchar_t* name, const wchar_t* needle) noexcept {
    for (const wchar_t* at = name; *at; ++at) {
        u32 k = 0;
        while (needle[k] && at[k] && ((at[k] | 32) == (needle[k] | 32)))
            ++k;
        if (!needle[k])
            return true;
    }
    return false;
}

u32 module_named(const wchar_t* needle) noexcept {
    const u32 count = static_cast<u32>(g_module_count);
    for (u32 i = 0; i < count; ++i)
        if (wide_has(g_modules[i].name, needle))
            return i;
    return kOwnerNone;
}

bool slot_in(u64 value, u32 module) noexcept {
    return module != kOwnerNone && value - g_modules[module].base < g_modules[module].size;
}

// False while the loader has not bound the module's imports yet.
bool imports_bound(u8* base) noexcept {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return true;
    for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         import->Name; ++import) {
        if (!import->OriginalFirstThunk)
            continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + import->OriginalFirstThunk);
        const auto* slots = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + import->FirstThunk);
        if (names->u1.AddressOfData)
            return slots->u1.Function != names->u1.AddressOfData;
    }
    return true;
}

void patch_module(u32 index) noexcept {
    OwnerModule& module = g_modules[index];
    auto* base = reinterpret_cast<u8*>(module.base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    module.patched = true;
    if (!directory.VirtualAddress)
        return;
    const u32 msvcrt = module_named(L"msvcrt.dll");
    const u32 ucrt = module_named(L"ucrtbase.dll");
    for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         import->Name; ++import) {
        if (!import->OriginalFirstThunk)
            continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + import->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + import->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            const char* name = reinterpret_cast<const char*>(
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name);
            const u64 current = slots->u1.Function;
            for (Hook& hook : g_hooks) {
                if (!same_name(name, hook.name))
                    continue;
                if ((hook.provider == Provider::msvcrt && !slot_in(current, msvcrt)) ||
                    (hook.provider == Provider::ucrt && !slot_in(current, ucrt)) ||
                    (hook.provider == Provider::any &&
                     (slot_in(current, msvcrt) || slot_in(current, ucrt))))
                    continue;
                void** original = static_cast<void**>(hook.original);
                if (!*original)
                    *original = reinterpret_cast<void*>(current);
                if (*original != reinterpret_cast<void*>(current))
                    break;
                DWORD old = 0;
                if (VirtualProtect(&slots->u1.Function, sizeof(u64), PAGE_READWRITE, &old)) {
                    slots->u1.Function = reinterpret_cast<u64>(hook.replacement);
                    VirtualProtect(&slots->u1.Function, sizeof(u64), old, &old);
                    ++g_patched_slots;
                }
                break;
            }
        }
    }
}

void scan_modules() noexcept {
    if (InterlockedCompareExchange(&g_scanning, 1, 0) != 0)
        return;
    InterlockedExchange(&g_pending, 0);
    AcquireSRWLockShared(&g_module_lock);
    const u32 count = static_cast<u32>(g_module_count);
    for (u32 i = 0; i < count; ++i) {
        OwnerModule& module = g_modules[i];
        if (module.patched || module.excluded)
            continue;
        if (imports_bound(reinterpret_cast<u8*>(module.base)))
            patch_module(i);
        else
            InterlockedExchange(&g_pending, 1);
    }
    ReleaseSRWLockShared(&g_module_lock);
    InterlockedExchange(&g_scanning, 0);
}

void scan_if_pending() noexcept {
    if (g_pending)
        scan_modules();
}

void add_module(void* base, u32 size, const wchar_t* name, u32 length) noexcept {
    const u32 count = static_cast<u32>(g_module_count);
    for (u32 i = 0; i < count; ++i)
        if (g_modules[i].base == reinterpret_cast<u64>(base))
            return;
    if (count == kOwnerModuleCapacity)
        return;
    OwnerModule& module = g_modules[count];
    module.base = reinterpret_cast<u64>(base);
    module.size = size;
    for (u32 i = 0; i < length && i + 1 < sizeof(module.name) / sizeof(module.name[0]); ++i)
        module.name[i] = name[i];
    // The probe image has its own hooks; the loader and CRT DLLs implement the
    // hooked functions (their internal calls would charge them for callers).
    module.excluded = base == GetModuleHandleW(nullptr) || wide_has(module.name, L"ntdll.dll") ||
                      wide_has(module.name, L"kernel32.dll") || wide_has(module.name, L"kernelbase.dll") ||
                      wide_has(module.name, L"msvcrt.dll") || wide_has(module.name, L"ucrtbase.dll");
    InterlockedExchange(&g_module_count, static_cast<LONG>(count + 1));
    InterlockedExchange(&g_pending, 1);
}

} // namespace

bool owners_initialize(void* (*allocate)(u64 bytes), u64* range_begin, u64* range_end) noexcept {
    const u64 module_bytes = (sizeof(OwnerModule) * kOwnerModuleCapacity + 4095) & ~4095ull;
    const u64 region_bytes = (sizeof(OwnedBase) * kRegionCapacity + 4095) & ~4095ull;
    const u64 bytes = module_bytes + region_bytes + sizeof(LiveBlock) * u64{kLiveCapacity};
    auto* memory = static_cast<u8*>(allocate(bytes));
    if (!memory)
        return false;
    g_modules = reinterpret_cast<OwnerModule*>(memory);
    g_regions = reinterpret_cast<OwnedBase*>(memory + module_bytes);
    g_live = reinterpret_cast<LiveBlock*>(memory + module_bytes + region_bytes);
    *range_begin = reinterpret_cast<u64>(memory);
    *range_end = *range_begin + ((bytes + 65535) & ~65535ull);
    HMODULE handles[256];
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed)) {
        for (u32 i = 0; i < needed / sizeof(HMODULE) && i < 256; ++i) {
            MODULEINFO info{};
            wchar_t name[40]{};
            const DWORD length = K32GetModuleBaseNameW(GetCurrentProcess(), handles[i], name, 39);
            if (K32GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info)))
                add_module(info.lpBaseOfDll, info.SizeOfImage, name, length);
        }
    }
    scan_modules();
    return true;
}

void owners_module_loaded(void* base, u32 size, const wchar_t* name, u32 name_length) noexcept {
    if (!g_modules)
        return;
    add_module(base, size, name, name_length);
    scan_modules();
}

void owners_module_unloaded(void* base) noexcept {
    if (!g_modules)
        return;
    AcquireSRWLockExclusive(&g_module_lock);
    const u32 count = static_cast<u32>(g_module_count);
    for (u32 i = 0; i < count; ++i)
        if (g_modules[i].base == reinterpret_cast<u64>(base))
            g_modules[i].excluded = true;
    ReleaseSRWLockExclusive(&g_module_lock);
}

void owners_scan() noexcept {
    if (g_modules)
        scan_modules();
}

u32 owners_module_count() noexcept {
    return static_cast<u32>(g_module_count);
}

const OwnerModule* owners_module(u32 index) noexcept {
    return index < static_cast<u32>(g_module_count) ? &g_modules[index] : nullptr;
}

u32 owners_region_owner(u64 allocation_base) noexcept {
    if (!g_regions)
        return kOwnerNone;
    AcquireSRWLockShared(&g_base_lock);
    const u32 owner = base_find(g_regions, g_region_count, allocation_base);
    ReleaseSRWLockShared(&g_base_lock);
    return owner;
}

u32 owners_heap_creator(void* heap) noexcept {
    AcquireSRWLockShared(&g_base_lock);
    const u32 owner = base_find(g_heaps, g_heap_count, reinterpret_cast<u64>(heap));
    ReleaseSRWLockShared(&g_base_lock);
    return owner;
}

void owners_set_tag(u32 tag) noexcept {
    const LONG thread = static_cast<LONG>(GetCurrentThreadId());
    for (TagSlot& slot : g_tags)
        if (slot.thread == thread) {
            if (tag)
                slot.tag = static_cast<LONG>(tag);
            else
                InterlockedExchange(&slot.thread, 0);
            return;
        }
    if (!tag)
        return;
    for (TagSlot& slot : g_tags)
        if (InterlockedCompareExchange(&slot.thread, thread, 0) == 0) {
            slot.tag = static_cast<LONG>(tag);
            return;
        }
}

void owners_block_totals(u32 owner, u64* bytes_by_tag, u64* bytes_by_size) noexcept {
    if (!g_live)
        return;
    AcquireSRWLockShared(&g_live_lock);
    for (u32 i = 0; i < kLiveCapacity; ++i) {
        const LiveBlock& block = g_live[i];
        if (!block.bytes || block.owner != owner)
            continue;
        bytes_by_tag[block.tag & (kOwnerTagCount - 1)] += block.bytes;
        u32 bucket = 0;
        for (u64 limit = 1024; bucket + 1 < kOwnerSizeBuckets && block.bytes >= limit; limit *= 4)
            ++bucket;
        bytes_by_size[bucket] += block.bytes;
    }
    ReleaseSRWLockShared(&g_live_lock);
}

void owners_counters(u64* live_entries, u64* dropped, u64* patched_slots) noexcept {
    AcquireSRWLockShared(&g_live_lock);
    *live_entries = g_live_count;
    *dropped = g_live_dropped;
    ReleaseSRWLockShared(&g_live_lock);
    *patched_slots = g_patched_slots;
}

} // namespace maicord::perf
