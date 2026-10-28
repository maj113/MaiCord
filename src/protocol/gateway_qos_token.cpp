#include "maicord/protocol/gateway_qos_token.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/protocol/base64.hpp"

namespace maicord {
namespace {

bool read_varint(const u8* p, u32 size, u32* at, u64* value) noexcept {
    u64 v = 0;
    for (u32 shift = 0; shift < 70; shift += 7) {
        if (*at >= size)
            return false;
        const u8 b = p[(*at)++];
        v |= static_cast<u64>(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *value = v;
            return true;
        }
    }
    return false;
}

u32 write_varint(u8* out, u64 value) noexcept {
    u32 n = 0;
    do {
        out[n] = static_cast<u8>(value & 0x7F);
        value >>= 7;
        if (value)
            out[n] |= 0x80;
        ++n;
    } while (value);
    return n;
}

struct Span {
    const u8* data;
    u32 bytes;
};

bool reencode(const u8* raw, u32 size, Slice<u8> output, u32* bytes) noexcept {
    Span claims{}, signature{};
    u32 key_id = 0;
    Span unknown[16];
    u32 unknown_count = 0;
    for (u32 at = 0; at < size;) {
        const u32 start = at;
        u64 key = 0;
        if (!read_varint(raw, size, &at, &key))
            return false;
        const u64 field = key >> 3;
        const u32 wire = static_cast<u32>(key & 7);
        u64 length = 0;
        if (wire == 0) {
            if (!read_varint(raw, size, &at, &length))
                return false;
        } else if (wire == 2) {
            if (!read_varint(raw, size, &at, &length) || length > size - at)
                return false;
            at += static_cast<u32>(length);
        } else if (wire == 1 || wire == 5) {
            const u32 width = wire == 1 ? 8 : 4;
            if (width > size - at)
                return false;
            at += width;
        } else {
            return false;
        }
        if ((field == 1 || field == 2) && wire == 2) {
            const Span value{raw + at - static_cast<u32>(length), static_cast<u32>(length)};
            (field == 1 ? claims : signature) = value;
        } else if (field == 3 && wire == 0) {
            key_id = static_cast<u32>(length);
        } else if (field >= 1 && field <= 3) {
            return false; // A known field of the wrong type does not decode.
        } else {
            if (unknown_count == 16)
                return false;
            unknown[unknown_count++] = {raw + start, at - start};
        }
    }
    u8 out[kGatewayQosDerivedMaximumBytes + 32];
    u32 n = 0;
    const Span fields[2] = {claims, signature};
    for (u32 i = 0; i < 2; ++i) {
        if (!fields[i].bytes)
            continue;
        out[n++] = static_cast<u8>(i == 0 ? 0x0A : 0x12);
        n += write_varint(out + n, fields[i].bytes);
        copy_bytes(out + n, fields[i].data, fields[i].bytes);
        n += fields[i].bytes;
    }
    if (key_id) {
        out[n++] = 0x18;
        n += write_varint(out + n, key_id);
    }
    for (u32 i = 0; i < unknown_count; ++i) {
        copy_bytes(out + n, unknown[i].data, unknown[i].bytes);
        n += unknown[i].bytes;
    }
    if (n > output.count || n > kGatewayQosDerivedMaximumBytes) {
        secure_clear_bytes(out, sizeof(out));
        return false;
    }
    copy_bytes(output.data, out, n);
    secure_clear_bytes(out, sizeof(out));
    *bytes = n;
    return true;
}

} // namespace

bool gateway_qos_derived_from_ready(Utf8View base64, Slice<u8> output, u32* bytes) noexcept {
    u8 raw[kGatewayQosDerivedMaximumBytes];
    *bytes = 0;
    if ((!base64.data && base64.bytes) || !output.data)
        return false;
    const Base64Result decoded =
        base64.bytes ? base64_decode(base64, {raw, sizeof(raw)}) : Base64Result{0, 0, Base64Error::none};
    const bool ok = decoded.ok() && reencode(raw, decoded.bytes, output, bytes);
    secure_clear_bytes(raw, sizeof(raw));
    return ok;
}

u32 gateway_qos_token_write(bool active, bool has_derived, ConstSlice<u8> derived, Slice<char> output) noexcept {
    if (derived.count > kGatewayQosDerivedMaximumBytes || (!derived.data && derived.count))
        return 0;
    u8 token[kGatewayQosDerivedMaximumBytes + 16];
    u32 n = 0;
    token[n++] = 0x0A;
    if (active) {
        token[n++] = 2;
        token[n++] = 0x08;
        token[n++] = 1;
    } else {
        token[n++] = 0;
    }
    if (has_derived) {
        token[n++] = 0x12;
        n += write_varint(token + n, derived.count);
        copy_bytes(token + n, derived.data, derived.count);
        n += derived.count;
    }
    const Base64Result encoded = base64_encode({token, n}, output);
    secure_clear_bytes(token, sizeof(token));
    return encoded.ok() ? encoded.bytes : 0;
}

} // namespace maicord
