#include "maicord/protocol/mls_objects.hpp"
#include "maicord/protocol/mls_reader.hpp"
#include "maicord/protocol/mls_encoding.hpp"

namespace maicord {
namespace {
struct Writer {
    Slice<u8> output;
    u32 used = 0;
    MlsWireError error = MlsWireError::none;
    bool emit = false;
    void fail(MlsWireError value) noexcept {
        if (error == MlsWireError::none)
            error = value;
    }
    void raw(ConstSlice<u8> data) noexcept {
        if (error != MlsWireError::none)
            return;
        if (!emit) {
            if (!byte_range_valid(data) || byte_ranges_overlap(data, {output.data, output.count})) {
                fail(MlsWireError::invalid_input);
                return;
            }
            if (data.count > ~u32{0} - used) {
                fail(MlsWireError::size_overflow);
                return;
            }
        } else if (data.count) {
            copy_bytes(output.data + used, data.data, data.count);
        }
        used += data.count;
    }
    void integer(u64 value, u32 count) noexcept {
        u8 bytes[8];
        for (u32 i = 0; i != count; ++i)
            bytes[count - i - 1] = static_cast<u8>(value >> (i * 8));
        raw({bytes, count});
    }
    void vector(ConstSlice<u8> data) noexcept {
        u8 prefix[4];
        const u32 count = mls_vector_length(data.count, prefix);
        if (!count) {
            fail(MlsWireError::size_overflow);
            return;
        }
        raw({prefix, count});
        raw(data);
    }
    void integers16(ConstSlice<u8> data) noexcept {
        if (data.count & 1)
            fail(MlsWireError::malformed);
        vector(data);
    }
    void list(ConstSlice<u8> data, bool extension) noexcept {
        // Validate spans/size/aliasing before traversing opaque nested entries.
        vector(data);
        if (emit || error != MlsWireError::none)
            return;
        MlsReader reader{data};
        while (reader.error == MlsWireError::none && reader.used < data.count) {
            if (extension)
                reader.integer(2);
            reader.vector();
        }
        if (reader.error != MlsWireError::none)
            fail(reader.error);
    }
};
void write_tbs(Writer& w, const MlsLeafView& v) noexcept {
    w.vector(v.encryption_key);
    w.vector(v.signature_key);
    w.integer(v.credential.type, 2);
    if (v.credential.type == 1)
        w.vector(v.credential.identity);
    else if (v.credential.type == 2)
        w.list(v.credential.certificates, false);
    else
        w.fail(MlsWireError::unsupported_credential);
    w.integers16(v.capabilities.versions);
    w.integers16(v.capabilities.cipher_suites);
    w.integers16(v.capabilities.extensions);
    w.integers16(v.capabilities.proposals);
    w.integers16(v.capabilities.credentials);
    w.integer(static_cast<u8>(v.source), 1);
    switch (v.source) {
    case MlsLeafSource::key_package:
        w.integer(v.not_before, 8);
        w.integer(v.not_after, 8);
        break;
    case MlsLeafSource::update:
        break;
    case MlsLeafSource::commit:
        w.vector(v.parent_hash);
        break;
    default:
        w.fail(MlsWireError::invalid_leaf_source);
        break;
    }
    w.list(v.extensions, true);
}
void write(Writer& w, const MlsLeafView& v) noexcept {
    write_tbs(w, v);
    w.vector(v.signature);
}
void write_tbs(Writer& w, const MlsKeyPackageView& v) noexcept {
    w.integer(v.version, 2);
    w.integer(v.cipher_suite, 2);
    w.vector(v.init_key);
    write(w, v.leaf);
    w.list(v.extensions, true);
}
template <typename T> MlsEncodeResult encode_tbs(const T& input, Slice<u8> output) noexcept {
    if (!byte_range_valid({output.data, output.count}) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(&input), sizeof(T)}, {output.data, output.count}))
        return {0, MlsWireError::invalid_input};
    Writer check{output};
    write_tbs(check, input);
    if (check.error != MlsWireError::none)
        return {0, check.error};
    if (check.used > output.count)
        return {0, MlsWireError::output_capacity};
    Writer writer{output, 0, MlsWireError::none, true};
    write_tbs(writer, input);
    return {writer.used, MlsWireError::none};
}
} // namespace
MlsEncodeResult mls_encode_tbs(const MlsLeafView& v, Slice<u8> out) noexcept {
    return encode_tbs(v, out);
}
MlsEncodeResult mls_encode_tbs(const MlsKeyPackageView& v, Slice<u8> out) noexcept {
    return encode_tbs(v, out);
}
} // namespace maicord
