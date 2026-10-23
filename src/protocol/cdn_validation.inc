// Shared by the CDN worker and protocol implementation inside their private namespaces.
bool valid_target(ConstSlice<u8> target) noexcept {
    if (!target.data || target.count == 0 || target.count > kCdnMaximumTargetBytes ||
        target[0] != '/' || (target.count > 1 && target[1] == '/')) {
        return false;
    }
    for (u32 index = 0; index < target.count; ++index) {
        const u8 value = target[index];
        if (value <= 0x20 || value > 0x7E || value == '#' || value == '\\') {
            return false;
        }
    }
    return true;
}

bool valid_etag(ConstSlice<u8> etag) noexcept {
    if ((!etag.data && etag.count != 0) || etag.count > kCdnMaximumEtagBytes) {
        return false;
    }
    for (u32 index = 0; index < etag.count; ++index) {
        if (etag[index] < 0x21 || etag[index] > 0x7E) {
            return false;
        }
    }
    return true;
}
