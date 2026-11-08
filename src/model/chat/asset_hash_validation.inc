// Shared by the guild and user stores inside their private namespaces.
bool asset_hash_valid(Utf8View hash) noexcept {
    return hash.bytes == 0 || asset_hash_hex_valid(hash);
}
