// The parts of data_path.hpp that are the same on both systems, included by src/core/data_path.cpp (Windows) and
// src/core/linux/data_path.cpp.

namespace maicord {

u32 wide_length(const wchar_t* text, u32 limit) noexcept {
    u32 length = 0;
    if (text)
        while (length < limit && text[length])
            ++length;
    return length;
}

bool wide_append(wchar_t* output, u32 capacity, u32* length, const wchar_t* text) noexcept {
    const u32 count = wide_length(text, capacity);
    if (!output || !length || *length + count >= capacity)
        return false;
    copy_bytes(output + *length, text, count * static_cast<u32>(sizeof(wchar_t)));
    *length += count;
    output[*length] = 0;
    return true;
}

bool wide_append_hex(wchar_t* output, u32 capacity, u32* length, u64 value) noexcept {
    if (!output || !length || *length + 16 >= capacity)
        return false;
    for (u32 shift = 64; shift;) {
        shift -= 4;
        output[(*length)++] = L"0123456789abcdef"[(value >> shift) & 15];
    }
    output[*length] = 0;
    return true;
}

bool profile_data_path(u64 profile_id, wchar_t* path, u32 capacity, const wchar_t* stem,
                       const wchar_t* extension) noexcept {
    if (!profile_id)
        return false;
    const DataPathResult built = data_path(path, capacity, stem);
    u32 length = built.length;
    return built.ok() && wide_append_hex(path, capacity, &length, profile_id) &&
           wide_append(path, capacity, &length, extension);
}

} // namespace maicord
