#pragma once

#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/base64.hpp"

namespace maicord::protocol_detail {

// Protobuf bytes, either raw or as lazily decoded canonical base64 text, so a
// base64 settings message never needs a whole-proto decode buffer. Base64 text
// must already have passed base64_decode; size is its decoded byte count.
struct ProtoSource {
    ConstSlice<u8> raw;
    Utf8View encoded;
    u32 size;
    // The most recently decoded base64 quartet, as its three bytes.
    mutable u32 cached_quartet = ~u32{0};
    mutable u8 cached[3]{};

    static u8 digit(char c) noexcept {
        return c >= 'A' && c <= 'Z'   ? static_cast<u8>(c - 'A')
               : c >= 'a' && c <= 'z' ? static_cast<u8>(c - 'a' + 26)
               : c >= '0' && c <= '9' ? static_cast<u8>(c - '0' + 52)
               : c == '+'             ? 62
               : c == '/'             ? 63
                                      : 0;
    }
    __declspec(noinline) u8 at(u32 p) const noexcept {
        if (raw.data)
            return raw[p];
        const u32 quartet = p / 3;
        if (quartet != cached_quartet) {
            const char* text = encoded.data + quartet * 4;
            const u8 a = digit(text[0]), b = digit(text[1]), c = digit(text[2]), d = digit(text[3]);
            cached[0] = static_cast<u8>((a << 2) | (b >> 4));
            cached[1] = static_cast<u8>((b << 4) | (c >> 2));
            cached[2] = static_cast<u8>((c << 6) | d);
            cached_quartet = quartet;
        }
        return cached[p % 3];
    }
};

inline ProtoSource proto_source(ConstSlice<u8> bytes) noexcept {
    return {bytes, {}, bytes.count};
}

// A base-128 varint of at most ten bytes in [p, end).
__declspec(noinline) inline bool proto_varint(const ProtoSource& w, u32& p, u32 end, u64& n) noexcept {
    n = 0;
    for (u32 i = 0; i < 10; ++i) {
        if (p >= end)
            return false;
        const u8 b = w.at(p++);
        if (i == 9 && b > 1)
            return false;
        n |= static_cast<u64>(b & 127) << (i * 7);
        if (!(b & 128))
            return true;
    }
    return false;
}

// A field tag with a nonzero 32-bit field number. End-group tags (type 4) are
// accepted so proto_skip can close groups; every caller rejects them otherwise.
__declspec(noinline) inline bool
proto_tag(const ProtoSource& w, u32& p, u32 end, u32& field, u32& type) noexcept {
    u64 n = 0;
    if (!proto_varint(w, p, end, n) || n > ~u32{0} || !(n >> 3))
        return false;
    field = static_cast<u32>(n >> 3);
    type = static_cast<u32>(n & 7);
    return true;
}

// A length prefix whose payload [p, stop) fits before end.
__declspec(noinline) inline bool proto_length(const ProtoSource& w, u32& p, u32 end, u32& stop) noexcept {
    u64 n = 0;
    if (!proto_varint(w, p, end, n) || n > end - p)
        return false;
    stop = p + static_cast<u32>(n);
    return true;
}

// Skips the value of a field whose tag was just read; groups nest at most 16 deep.
__declspec(noinline) inline bool proto_skip(const ProtoSource& w, u32& p, u32 end, u32 type, u32 field,
                                            u32 depth = 0) noexcept {
    u64 n = 0;
    if (type == 0)
        return proto_varint(w, p, end, n);
    if (type == 1 || type == 5) {
        const u32 bytes = type == 1 ? 8 : 4;
        if (bytes > end - p)
            return false;
        p += bytes;
        return true;
    }
    if (type == 2) {
        u32 stop = 0;
        if (!proto_length(w, p, end, stop))
            return false;
        p = stop;
        return true;
    }
    if (type != 3 || depth >= 16)
        return false;
    while (p < end) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, end, f, t))
            return false;
        if (t == 4)
            return f == field;
        if (!proto_skip(w, p, end, t, f, depth + 1))
            return false;
    }
    return false;
}

// Little-endian fixed64 at p; the caller checked that eight bytes remain.
inline u64 proto_fixed64(const ProtoSource& w, u32 p) noexcept {
    u64 value = 0;
    for (u32 i = 0; i < 8; ++i)
        value |= static_cast<u64>(w.at(p + i)) << (i * 8);
    return value;
}

inline u32 proto_varint_size(u64 value) noexcept {
    u32 size = 1;
    while (value >= 128) {
        value >>= 7;
        ++size;
    }
    return size;
}

// Writes protobuf into output while it fits and keeps counting past its end,
// so `at` is always the size the message needs; ok turns false once a byte did
// not fit. An empty output measures.
struct ProtoWriter {
    Slice<u8> output;
    u32 at;
    bool ok;

    void byte(u8 value) noexcept {
        if (at < output.count)
            output.data[at] = value;
        else
            ok = false;
        ++at;
    }
    __declspec(noinline) void varint(u64 value) noexcept {
        do {
            const u8 low = static_cast<u8>(value & 127);
            value >>= 7;
            byte(static_cast<u8>(low | (value ? 128 : 0)));
        } while (value);
    }
    void tag(u32 field, u32 type) noexcept {
        varint((static_cast<u64>(field) << 3) | type);
    }
    __declspec(noinline) void fixed64(u64 value) noexcept {
        for (u32 i = 0; i < 8; ++i)
            byte(static_cast<u8>(value >> (i * 8)));
    }
    void bytes(const void* data, u32 count) noexcept {
        if (count > output.count - minimum(at, output.count)) {
            ok = false;
            at += count;
            return;
        }
        copy_bytes(output.data + at, data, count);
        at += count;
    }
    // Copies source bytes [begin, end), decoding base64 sources on the way.
    void copy(const ProtoSource& source, u32 begin, u32 end) noexcept {
        if (source.raw.data) {
            bytes(source.raw.data + begin, end - begin);
            return;
        }
        if (end - begin > output.count - minimum(at, output.count)) {
            ok = false;
            at += end - begin;
            return;
        }
        for (u32 i = begin; i < end; ++i)
            output.data[at + i - begin] = source.at(i);
        at += end - begin;
    }
};

// The settings-proto PATCH body: {"settings":"<base64 proto>"}, with
// ,"required_data_version":N before the closing brace when version is nonzero
// or present.
inline constexpr char kSettingsPatchHead[] = "{\"settings\":\"";
inline constexpr char kSettingsPatchVersion[] = ",\"required_data_version\":";
inline constexpr u32 kSettingsPatchHeadBytes = sizeof(kSettingsPatchHead) - 1;
inline constexpr u32 kSettingsPatchVersionBytes = sizeof(kSettingsPatchVersion) - 1;

inline u32 proto_settings_patch_bytes(u32 proto_bytes, u32 version, bool present = false) noexcept {
    const u32 version_bytes = version || present ? kSettingsPatchVersionBytes + decimal_digits(version) : 0;
    return kSettingsPatchHeadBytes + base64_encoded_size(proto_bytes) + 1 + version_bytes + 1;
}

// Writes the PATCH body; output must hold proto_settings_patch_bytes. False
// when the proto cannot be base64 encoded.
inline bool proto_write_settings_patch(ConstSlice<u8> proto, u32 version, Slice<u8> output,
                                       bool present = false) noexcept {
    copy_bytes(output.data, kSettingsPatchHead, kSettingsPatchHeadBytes);
    u32 at = kSettingsPatchHeadBytes;
    const u32 encoded = base64_encoded_size(proto.count);
    if (!base64_encode(proto, {reinterpret_cast<char*>(output.data + at), encoded}).ok())
        return false;
    at += encoded;
    output.data[at++] = '"';
    if (version || present) {
        copy_bytes(output.data + at, kSettingsPatchVersion, kSettingsPatchVersionBytes);
        at += kSettingsPatchVersionBytes;
        at += decimal_write(output.data + at, version);
    }
    output.data[at] = '}';
    return true;
}

} // namespace maicord::protocol_detail
