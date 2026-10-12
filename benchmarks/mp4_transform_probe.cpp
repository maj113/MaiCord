#include "mp4_transform_probe.hpp"
#include "maicord/core/memory.hpp"
#include <mftransform.h>
#include <mfapi.h>

namespace maicord {
namespace {
void hex(char* text, u32* cursor, u64 value, u32 digits) noexcept {
    constexpr char alphabet[] = "0123456789abcdef";
    while (digits) text[(*cursor)++] = alphabet[(value >> (--digits * 4u)) & 15u];
}
void attributes(IMFAttributes* values, u32 scope,
                void (*metric)(const char*, u64)) noexcept {
    if (!values) return;
    u32 count = 0;
    if (FAILED(values->GetCount(&count))) return;
    for (u32 index = 0; index < minimum(count, 128u); ++index) {
        GUID key{};
        PROPVARIANT value{};
        if (FAILED(values->GetItemByIndex(index, &key, &value))) {
            PropVariantClear(&value);
            continue;
        }
        char name[64] = "inspect_";
        u32 cursor = 8;
        name[cursor++] = static_cast<char>('0' + scope); name[cursor++] = '_';
        hex(name, &cursor, key.Data1, 8); name[cursor++] = '-';
        hex(name, &cursor, key.Data2, 4); name[cursor++] = '-';
        hex(name, &cursor, key.Data3, 4); name[cursor++] = '-';
        for (u32 byte = 0; byte < 8; ++byte) {
            if (byte == 2) name[cursor++] = '-';
            hex(name, &cursor, key.Data4[byte], 2);
        }
        name[cursor++] = '_';
        copy_bytes(name + cursor, "type", 5);
        metric(name, value.vt);
        copy_bytes(name + cursor, "value", 6);
        if (value.vt == VT_UI4) metric(name, value.ulVal);
        else if (value.vt == VT_UI8) metric(name, value.uhVal.QuadPart);
        else if (value.vt == VT_I4) metric(name, static_cast<u32>(value.lVal));
        else if (value.vt == VT_BOOL) metric(name, value.boolVal != VARIANT_FALSE);
        else if (value.vt == (VT_VECTOR | VT_UI1)) metric(name, value.caub.cElems);
        else if (value.vt == VT_CLSID && value.puuid) {
            const auto* words = reinterpret_cast<const u8*>(value.puuid);
            for (u32 word = 0; word < 4; ++word) {
                u32 number = 0; copy_bytes(&number, words + word * 4, 4);
                name[cursor] = static_cast<char>('0' + word); name[cursor + 1] = 0;
                metric(name, number);
            }
        }
        PropVariantClear(&value);
    }
}
}
void mp4_inspect_transform(IMFTransform* transform,
                          void (*metric)(const char*, u64)) noexcept {
    metric("inspect_transform_available", transform != nullptr);
    if (!transform) return;
    const auto* vtable = *reinterpret_cast<const uintptr_t* const*>(transform);
    const u64 decoder_base = reinterpret_cast<u64>(GetModuleHandleW(L"msmpeg2vdec.dll"));
    // IMFTransform's documented COM method order, for disassembly correlation
    // when this Windows build's private/public PDB is not published.
    constexpr u32 slots[]{15, 16, 23, 24, 25};
    constexpr const char* names[]{"inspect_set_input_rva", "inspect_set_output_rva", "inspect_message_rva",
        "inspect_process_input_rva", "inspect_process_output_rva"};
    for (u32 index = 0; index < 5; ++index) {
        HMODULE module = nullptr;
        if (decoder_base && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(vtable[slots[index]]), &module) &&
            reinterpret_cast<u64>(module) == decoder_base)
            metric(names[index], vtable[slots[index]] - decoder_base);
    }
    IMFAttributes* values = nullptr;
    if (SUCCEEDED(transform->GetAttributes(&values))) { attributes(values, 0, metric); values->Release(); }
    if (SUCCEEDED(transform->GetInputStreamAttributes(0, &values))) { attributes(values, 1, metric); values->Release(); }
    if (SUCCEEDED(transform->GetOutputStreamAttributes(0, &values))) { attributes(values, 2, metric); values->Release(); }
    IMFMediaType* type = nullptr;
    if (SUCCEEDED(transform->GetInputCurrentType(0, &type))) { attributes(type, 3, metric); type->Release(); }
    if (SUCCEEDED(transform->GetOutputCurrentType(0, &type))) { attributes(type, 4, metric); type->Release(); }
}
}
