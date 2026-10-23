// Included inside the REST client and protocol implementation namespaces.
bool valid_base64_metadata(ConstSlice<u8> value) noexcept {
    if (!value.data || value.count == 0 || value.count > kRestUserSuperPropertiesMaximumBytes ||
        (value.count & 3u) != 0) {
        return false;
    }
    u32 padding = 0;
    for (u32 index = 0; index < value.count; ++index) {
        const u8 byte = value[index];
        const bool alphabet = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                              (byte >= '0' && byte <= '9') || byte == '+' || byte == '/';
        if (byte == '=') {
            ++padding;
            if (index < value.count - 2 || padding > 2) {
                return false;
            }
        } else if (!alphabet || padding != 0) {
            return false;
        }
    }
    return true;
}
