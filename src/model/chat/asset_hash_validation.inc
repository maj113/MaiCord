// Shared by the guild and user stores inside their private namespaces.
bool asset_hash_valid(Utf8View hash) noexcept {
    if (hash.bytes == 0) {
        return true;
    }
    const u32 begin = asset_hash_animated(hash) ? 2u : 0u;
    if (!hash.data || hash.bytes - begin != 32) {
        return false;
    }
    for (u32 index = begin; index < hash.bytes; ++index) {
        if (!ascii_is_hex_digit(hash.data[index])) {
            return false;
        }
    }
    return true;
}
