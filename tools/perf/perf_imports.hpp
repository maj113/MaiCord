#pragma once
// Import address table slots of a loaded image, for the perf probe's hooks.
// Images linked with /ALLOWBIND:NO (MaiCord's own) have no import lookup
// tables, so once loaded their static import address tables hold only
// resolved addresses: a name's slots are the ones holding the address the
// descriptor's DLL resolves that name to. Nothing here ships.
#include "maicord/core/base.hpp"
#include "maicord/core/win32_image.hpp"

namespace maicord::perf {

inline const IMAGE_NT_HEADERS64* image_headers(const u8* base) noexcept {
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base +
                                                       reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
}

// False while the loader has not bound the image's static imports yet.
inline bool imports_bound(const u8* base) noexcept {
    const auto* nt = image_headers(base);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return true;
    for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         import->Name; ++import) {
        const auto* slots = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + import->FirstThunk);
        if (import->OriginalFirstThunk) {
            const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + import->OriginalFirstThunk);
            if (names->u1.AddressOfData)
                return slots->u1.Function != names->u1.AddressOfData;
        } else if (slots->u1.Function) {
            // Unbound, the slot still holds its hint/name RVA or ordinal.
            return !IMAGE_SNAP_BY_ORDINAL64(slots->u1.Ordinal) &&
                   slots->u1.Function >= nt->OptionalHeader.SizeOfImage;
        }
    }
    return true;
}

inline bool import_name_equals(const char* a, const char* b) noexcept {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

// Calls on_slot(i, slot) for every import address table slot of the bound
// image at `base` that imports name_of(i) (i < count) by name; on_slot returns
// true when no further name applies to that slot. With `delayed`, delay-load
// slots too: they hold the image's resolver thunks until first called, and
// their names come from the delay-load name tables.
template <typename NameOf, typename OnSlot>
void for_each_import_slot(u8* base, u32 count, NameOf&& name_of, bool delayed, OnSlot&& on_slot) noexcept {
    const auto by_name = [&](const IMAGE_THUNK_DATA64* names, IMAGE_THUNK_DATA64* slots) noexcept {
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            const char* imported = reinterpret_cast<const char*>(
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name);
            for (u32 i = 0; i < count; ++i)
                if (import_name_equals(imported, name_of(i)) && on_slot(i, slots))
                    break;
        }
    };
    const auto* nt = image_headers(base);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress)
        for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
             import->Name; ++import) {
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + import->FirstThunk);
            if (import->OriginalFirstThunk) {
                by_name(reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + import->OriginalFirstThunk), slots);
                continue;
            }
            // GetModuleHandleW appends the ".dll" /ALLOWBIND:NO drops from the name.
            const char* dll = reinterpret_cast<const char*>(base + import->Name);
            wchar_t wide[64];
            u32 length = 0;
            while (dll[length] && length + 1 < sizeof(wide) / sizeof(wide[0])) {
                wide[length] = static_cast<wchar_t>(static_cast<u8>(dll[length]));
                ++length;
            }
            wide[length] = 0;
            const HMODULE module = dll[length] ? nullptr : GetModuleHandleW(wide);
            if (!module)
                continue;
            for (u32 i = 0; i < count; ++i) {
                const auto target = reinterpret_cast<u64>(GetProcAddress(module, name_of(i)));
                if (target)
                    for (auto* slot = slots; slot->u1.Function; ++slot)
                        if (slot->u1.Function == target)
                            on_slot(i, slot);
            }
        }
    const auto& delay = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayed && delay.VirtualAddress)
        for (auto* entry = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delay.VirtualAddress);
             entry->DllNameRVA; ++entry)
            if (entry->Attributes.RvaBased)
                by_name(reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + entry->ImportNameTableRVA),
                        reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->ImportAddressTableRVA));
}

} // namespace maicord::perf
