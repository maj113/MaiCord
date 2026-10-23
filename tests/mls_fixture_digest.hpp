#pragma once

#include "maicord/protocol/mls_objects.hpp"
#include "maicord/sys/crypto.hpp"

namespace maicord {

// Normalizes decoded views for comparison with independent corpus digests.
struct MlsFixtureDigest {
    void (*check)(bool, const char*) noexcept;
    void* hash;
    ConstSlice<u8> input;
    const char* borrow_label = "MLS view borrows only supplied input";
    const char* length_label = "MLS normalized field length";
    const char* bytes_label = "MLS normalized field bytes";
    void field(ConstSlice<u8> value, bool borrowed = true) noexcept {
        if (borrowed && value.count) {
            const auto begin = reinterpret_cast<usize>(input.data), p = reinterpret_cast<usize>(value.data);
            check(p >= begin && p - begin <= input.count && value.count <= input.count - (p - begin),
                  borrow_label);
        }
        u8 size[4];
        for (u32 i = 0; i != 4; ++i)
            size[i] = static_cast<u8>(value.count >> (24 - i * 8));
        check(BCryptHashData(hash, size, 4, 0) >= 0, length_label);
        if (value.count)
            check(BCryptHashData(hash, const_cast<u8*>(value.data), value.count, 0) >= 0,
                  bytes_label);
    }
    void number(u64 value, u32 n) noexcept {
        u8 b[8];
        for (u32 i = 0; i != n; ++i)
            b[n - 1 - i] = static_cast<u8>(value >> (i * 8));
        field({b, n}, false);
    }
    void leaf(const MlsLeafView& v) noexcept {
        field(v.encryption_key);
        field(v.signature_key);
        number(v.credential.type, 2);
        field(v.credential.identity);
        field(v.credential.certificates);
        field(v.capabilities.versions);
        field(v.capabilities.cipher_suites);
        field(v.capabilities.extensions);
        field(v.capabilities.proposals);
        field(v.capabilities.credentials);
        number(static_cast<u8>(v.source), 1);
        number(v.not_before, 8);
        number(v.not_after, 8);
        field(v.parent_hash);
        field(v.extensions);
        field(v.signature);
        field(v.serialized);
        field(v.tbs);
    }
    void package(const MlsKeyPackageView& v) noexcept {
        number(v.version, 2);
        number(v.cipher_suite, 2);
        field(v.init_key);
        leaf(v.leaf);
        field(v.extensions);
        field(v.signature);
        field(v.serialized);
        field(v.tbs);
    }
    void group(const MlsGroupContextView& v) noexcept {
        number(v.version, 2);
        number(v.cipher_suite, 2);
        field(v.group_id);
        number(v.epoch, 8);
        field(v.tree_hash);
        field(v.confirmed_transcript_hash);
        field(v.extensions);
        field(v.serialized);
    }
};

} // namespace maicord
