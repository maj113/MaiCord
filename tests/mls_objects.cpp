#include "maicord/protocol/mls_objects.hpp"
#include "maicord/protocol/mls_reader.hpp"
#include "maicord/protocol/mls_encoding.hpp"
#include "maicord/app/mls_key_package.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/mls_object_vectors.hpp"
bool filled(const void* ptr, u32 n, u8 value = 0xa5) noexcept {
    const auto* p = static_cast<const u8*>(ptr);
    for (u32 i = 0; i != n; ++i)
        if (p[i] != value)
            return false;
    return true;
}
struct Digest {
    Check check;
    void* hash;
    ConstSlice<u8> input;
    void field(ConstSlice<u8> value, bool borrowed = true) noexcept {
        if (borrowed && value.count) {
            const auto begin = reinterpret_cast<usize>(input.data), p = reinterpret_cast<usize>(value.data);
            check(p >= begin && p - begin <= input.count && value.count <= input.count - (p - begin),
                  "MLS view borrows only supplied input");
        }
        u8 size[4];
        for (u32 i = 0; i != 4; ++i)
            size[i] = static_cast<u8>(value.count >> (24 - i * 8));
        check(BCryptHashData(hash, size, 4, 0) >= 0, "MLS normalized field length");
        if (value.count)
            check(BCryptHashData(hash, const_cast<u8*>(value.data), value.count, 0) >= 0,
                  "MLS normalized field bytes");
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
void exercise_tbs(Check, const MlsGroupContextView&, u8*) noexcept {
}
template <typename T> void exercise_tbs(Check check, T view, u8* encoded) noexcept {
    view.signature = {nullptr, 1}; // Excluded outer signature must not be read.
    memset(encoded, 0xa5, 16384);
    auto result = mls_encode_tbs(view, {encoded, 16384});
    check(result.ok() && result.bytes == view.tbs.count &&
              bytes_equal(encoded, view.tbs.data, result.bytes) &&
              filled(encoded + result.bytes, 16384 - result.bytes),
          "MLS TBS writer reproduces independent signed bytes and ignores excluded signature");
    memset(encoded, 0xa5, 16384);
    result = mls_encode_tbs(view, {encoded, view.tbs.count - 1});
    check(result.error == MlsWireError::output_capacity && !result.bytes && filled(encoded, 16384),
          "MLS TBS insufficient capacity preserves entire output");
}
template <typename T, MlsDecodeResult (*Prefix)(ConstSlice<u8>, T*) noexcept>
T exercise(Check check, u8* data, u32 n) noexcept {
    T view{};
    const auto result = Prefix({data, n}, &view);
    check(result.ok() && result.bytes == n, "MLS published/independent object decode");
    u8 encoded[16384];
    exercise_tbs(check, view, encoded);
    T unchanged;
    memset(&unchanged, 0xa5, sizeof(unchanged));
    for (u32 cut = 0; cut != n; ++cut) {
        const auto failed = Prefix({data, cut}, &unchanged);
        check(!failed.ok() && !failed.bytes && failed.error_offset <= cut &&
                  filled(&unchanged, sizeof(unchanged)),
              "MLS every object truncation preserves output and bounded error offset");
    }
    data[n] = 0xaa;
    check(Prefix({data, n + 1}, &unchanged).bytes == n, "MLS prefix consumes exactly one nested object");
    return view;
}
void write_failures(Check check) noexcept {
    u8 output[128];
    MlsLeafView leaf{};
    leaf.credential.type = 1;
    leaf.source = MlsLeafSource::update;
    // Building fields directly must not depend on a previously decoded object.
    leaf.serialized = {nullptr, 42};
    leaf.tbs = {nullptr, 42};
    const u8 expected[]{0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 2, 0};
    auto result = mls_encode_tbs(leaf, {output, sizeof(output)});
    check(result.ok() && result.bytes == sizeof(expected) &&
              bytes_equal(output, expected, sizeof(expected)),
          "MLS serialize newly constructed fields without cached wire bytes");
    auto reject = [&](MlsWireError error) noexcept {
        memset(output, 0xa5, sizeof(output));
        const auto encoded = mls_encode_tbs(leaf, {output, sizeof(output)});
        check(encoded.error == error && !encoded.bytes && filled(output, sizeof(output)),
              "MLS invalid serialization fields rejected atomically");
    };
    leaf.encryption_key = {nullptr, 1};
    reject(MlsWireError::invalid_input);
    leaf.encryption_key = {nullptr, 0x40000000};
    reject(MlsWireError::size_overflow);
    leaf.encryption_key = {};
    leaf.credential.type = 0;
    reject(MlsWireError::unsupported_credential);
    leaf.credential.type = 1;
    leaf.source = static_cast<MlsLeafSource>(0);
    reject(MlsWireError::invalid_leaf_source);
    leaf.source = MlsLeafSource::update;
    leaf.capabilities.versions = {expected, 1};
    reject(MlsWireError::malformed);
    leaf.capabilities.versions = {};
    const u8 bad_extension[]{0, 1, 0x40, 0};
    leaf.extensions = {bad_extension, sizeof(bad_extension)};
    reject(MlsWireError::noncanonical_length);
    leaf.extensions = {bad_extension, 1};
    reject(MlsWireError::truncated);
    leaf.extensions = {};
    leaf.credential.type = 2;
    const u8 bad_certificate[]{2, 0};
    leaf.credential.certificates = {bad_certificate, sizeof(bad_certificate)};
    reject(MlsWireError::truncated);
}
void write_boundaries(Check check) noexcept {
    constexpr u32 capacity = 32768;
    auto* storage =
        static_cast<u8*>(VirtualAlloc(nullptr, capacity * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(storage != nullptr, "MLS writer boundary allocation");
    if (!storage)
        return;
    for (u32 i = 0; i != capacity; ++i)
        storage[i] = static_cast<u8>(i);
    const u32 counts[]{0, 1, 63, 64, 16383, 16384};
    for (const auto count : counts) {
        MlsKeyPackageView package{};
        package.version = 0xffff;
        package.cipher_suite = 0xabcd;
        package.init_key = {storage, count};
        package.leaf.credential.type = 1;
        package.leaf.source = MlsLeafSource::key_package;
        package.leaf.not_after = ~u64{0};
        u8* wire = storage + capacity;
        const auto encoded = mls_encode_tbs(package, {wire, capacity - 1});
        u8 prefix[4];
        const u32 prefix_bytes = mls_vector_length(count, prefix);
        // Header 4, minimal key-package leaf 29, package extensions 1.
        check(encoded.ok() && encoded.bytes == count + prefix_bytes + 34 &&
                  bytes_equal(wire + 4, prefix, prefix_bytes),
              "MLS writer size and canonical vector prefixes across length boundaries");
        wire[encoded.bytes] = 0; // Empty package signature completes the object.
        MlsKeyPackageView decoded{};
        check(encoded.ok() && mls_key_package_decode({wire, encoded.bytes + 1}, &decoded).ok() &&
                  decoded.version == package.version && decoded.cipher_suite == package.cipher_suite &&
                  decoded.leaf.not_after == package.leaf.not_after && decoded.init_key.count == count &&
                  bytes_equal(decoded.init_key.data, storage, count),
              "MLS writer preserves scalar endianness and vector payloads at boundaries");
    }
    VirtualFree(storage, 0, MEM_RELEASE);
}
void corpus(Check check, u8* buffer) noexcept {
    void* hash = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
          "MLS object corpus hash init");
    if (!hash)
        return;
    for (const auto& v : mls_object_vectors) {
        const u32 n = test_unhex(v.hex, buffer);
        Digest d{check, hash, {buffer, n}};
        if (v.kind == 1) {
            d.package(exercise<MlsKeyPackageView, mls_key_package_decode_prefix>(check, buffer, n));
            MlsKeyPackageView unchanged;
            memset(&unchanged, 0xa5, sizeof(unchanged));
            check(mls_key_package_decode({buffer, n + 1}, &unchanged).error == MlsWireError::trailing_data &&
                      filled(&unchanged, sizeof(unchanged)),
                  "MLS exact decode rejects trailing data atomically");
        } else if (v.kind == 2)
            d.leaf(exercise<MlsLeafView, mls_leaf_decode_prefix>(check, buffer, n));
        else
            d.group(exercise<MlsGroupContextView, mls_group_context_decode_prefix>(check, buffer, n));
    }
    u8 actual[32], expected[32];
    test_unhex(mls_object_digest, expected);
    check(BCryptFinishHash(hash, actual, 32, 0) >= 0 && bytes_equal(actual, expected, 32),
          "All 903 object views match independent normalized corpus");
    BCryptDestroyHash(hash);
}
void lengths(Check check) noexcept {
    struct V {
        const char* hex;
        u32 value;
    };
    const V values[]{{"00", 0},
                     {"3f", 63},
                     {"4040", 64},
                     {"7fff", 16383},
                     {"80004000", 16384},
                     {"bfffffff", 0x3fffffff},
                     {"9d7f3e7d", 494878333},
                     {"7bbd", 15293},
                     {"25", 37}};
    for (const auto& v : values) {
        u8 b[4], encoded[4];
        const u32 n = test_unhex(v.hex, b);
        MlsReader r{{b, n}};
        check(r.length() == v.value && r.result().ok() && r.used == n,
              "MLS canonical length independent/RFC answer");
        check(mls_vector_length(v.value, encoded) == n && bytes_equal(b, encoded, n),
              "MLS shared writer canonical bytes");
        for (u32 cut = 0; cut != n; ++cut) {
            MlsReader short_reader{{b, cut}};
            short_reader.length();
            check(!short_reader.result().ok(), "MLS length truncation rejected");
        }
    }
    const char* invalid[]{"4000", "403f", "80000000", "80003fff", "c0", "ff"};
    for (auto hex : invalid) {
        u8 b[4];
        const u32 n = test_unhex(hex, b);
        MlsReader r{{b, n}};
        r.length();
        const auto first = r.result();
        check(!first.ok() && first.error_offset == 0, "MLS rejects overlong or reserved length");
        r.integer(8);
        r.vector();
        check(r.result().error == first.error && r.result().error_offset == 0,
              "MLS reader first error remains stable");
    }
    MlsReader empty{{}};
    check(!empty.take(0).count && empty.result().ok(), "MLS null empty span avoids pointer arithmetic");
    u8 huge[]{0xbf, 0xff, 0xff, 0xff};
    MlsReader no_payload{{huge, 4}};
    no_payload.vector();
    check(no_payload.result().error == MlsWireError::truncated && no_payload.error_offset == 4,
          "MLS maximum vector cannot overrun supplied input");
    u8 minimal[32]{};
    minimal[3] = 1;  // Basic credential; all vectors empty.
    minimal[10] = 1; // KeyPackage source, with a 16-byte lifetime.
    MlsLeafView output;
    memset(&output, 0xa5, sizeof(output));
    minimal[5] = 1; // Odd-length version capability vector.
    check(mls_leaf_decode_prefix({minimal, 29}, &output).error == MlsWireError::malformed &&
              filled(&output, sizeof(output)),
          "MLS rejects partial u16 capability elements");
    minimal[5] = 0;
    minimal[3] = 3;
    check(mls_leaf_decode_prefix({minimal, 29}, &output).error == MlsWireError::unsupported_credential &&
              filled(&output, sizeof(output)),
          "MLS does not guess unknown credential union framing");
    minimal[3] = 1;
    minimal[10] = 4;
    check(mls_leaf_decode_prefix({minimal, 29}, &output).error == MlsWireError::invalid_leaf_source &&
              filled(&output, sizeof(output)),
          "MLS rejects unknown leaf source union");
    minimal[10] = 1;
    minimal[27] = 3;
    minimal[28] = 0;
    minimal[29] = 10;
    minimal[30] = 0x40;
    const auto extension_error = mls_leaf_decode_prefix({minimal, 32}, &output);
    check(extension_error.error == MlsWireError::truncated && extension_error.error_offset == 30 &&
              filled(&output, sizeof(output)),
          "MLS nested extension errors preserve absolute wire offset");
}
void crypto(Check check, u8* buffer) noexcept {
    KdfSha256Workspace workspace{};
    check(kdf_sha256_initialize(&workspace).ok(), "MLS package crypto workspace");
    if (!workspace.storage.data)
        return;
    for (const auto& v : mls_signed_vectors) {
        const u32 n = test_unhex(v.package, buffer);
        u8 ref[32];
        test_unhex(v.reference, ref);
        MlsSignedKeyPackage output{};
        check(mls_key_package_check_crypto(workspace, {buffer, n}, &output).ok() &&
                  bytes_equal(output.reference, ref, 32),
              "Independent signed KeyPackage verifies both signatures and reference, including unknown "
              "extensions");
        check(output.view.leaf.credential.identity.count == 8 && output.view.leaf.not_before == 0 &&
                  output.view.leaf.not_after == ~u64{0},
              "Signed wire view preserves identity/lifetime for later policy validation");
        MlsSignedKeyPackage unchanged;
        memset(&unchanged, 0xa5, sizeof(unchanged));
        for (u32 i = 0; i != n; ++i) {
            buffer[i] ^= 1;
            const auto result = mls_key_package_check_crypto(workspace, {buffer, n}, &unchanged);
            check(!result.ok() && !result.bytes && filled(&unchanged, sizeof(unchanged)),
                  "MLS rejects every single-byte signed package tamper atomically");
            buffer[i] ^= 1;
        }
        check(mls_key_package_check_crypto(workspace, {buffer, n},
                                           reinterpret_cast<MlsSignedKeyPackage*>(buffer))
                      .error == MlsPackageCryptoError::invalid_input,
              "MLS package cannot overwrite its signed backing bytes");
        const u32 capacity = workspace.storage.capacity;
        workspace.storage.capacity = 1;
        check(mls_key_package_check_crypto(workspace, {buffer, n}, &unchanged).error ==
                      MlsPackageCryptoError::hash_failure &&
                  filled(&unchanged, sizeof(unchanged)),
              "MLS package hash provider failure preserves candidate");
        workspace.storage.capacity = capacity;
        check(mls_key_package_check_crypto(workspace, {buffer, n}, &output).ok(),
              "MLS package retry after provider failure");
        const ConstSlice<u8> points[]{output.view.init_key, output.view.leaf.encryption_key,
                                      output.view.leaf.signature_key};
        for (auto point : points) {
            u8 saved_point[65];
            copy_bytes(saved_point, point.data, 65);
            auto* target = buffer + (point.data - buffer);
            memset(target + 1, 0, 64);
            check(mls_key_package_check_crypto(workspace, {buffer, n}, &unchanged).error ==
                          MlsPackageCryptoError::key_failure &&
                      filled(&unchanged, sizeof(unchanged)),
                  "MLS validates each curve point before signature work");
            copy_bytes(target, saved_point, 65);
        }
        // Distinct valid init/leaf keys are checked before signatures; copying
        // the leaf key into init must not be accepted despite both being valid.
        const u32 offset = static_cast<u32>(output.view.init_key.data - buffer);
        u8 saved[65];
        copy_bytes(saved, buffer + offset, 65);
        copy_bytes(buffer + offset, output.view.leaf.encryption_key.data, 65);
        check(mls_key_package_check_crypto(workspace, {buffer, n}, &unchanged).error ==
                  MlsPackageCryptoError::key_reuse,
              "MLS rejects init/leaf key reuse");
        copy_bytes(buffer + offset, saved, 65);
    }
    check(filled(workspace.storage.data, workspace.storage.capacity, 0),
          "MLS package crypto workspace erased");
    kdf_sha256_release(&workspace);
}
void guards(Check check, u8* buffer) noexcept {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u32 page = system.dwPageSize;
    auto* mem = static_cast<u8*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(mem != nullptr, "MLS object guards allocation");
    if (!mem)
        return;
    const u32 n = test_unhex(mls_signed_vectors[0].package, buffer);
    auto* input = mem + page - n;
    auto* end = mem + page * 3;
    copy_bytes(input, buffer, n);
    DWORD old = 0;
    check(VirtualProtect(mem, page, PAGE_READONLY, &old) &&
              VirtualProtect(mem + page, page, PAGE_NOACCESS, &old) &&
              VirtualProtect(end, page, PAGE_NOACCESS, &old),
          "MLS object immutable and inaccessible pages");
    auto* view = reinterpret_cast<MlsKeyPackageView*>(end - sizeof(MlsKeyPackageView));
    check(mls_key_package_decode({input, n}, view).ok() && view->serialized.data == input,
          "MLS exact input and view output guard");
    KdfSha256Workspace workspace{};
    check(kdf_sha256_initialize(&workspace).ok(), "MLS crypto guard workspace");
    auto* signed_view = reinterpret_cast<MlsSignedKeyPackage*>(end - sizeof(MlsSignedKeyPackage));
    check(mls_key_package_check_crypto(workspace, {input, n}, signed_view).ok(),
          "MLS exact signed candidate guard");
    const auto encode_view = signed_view->view;
    const u32 tbs_bytes = encode_view.tbs.count;
    const auto encoded = mls_encode_tbs(encode_view, {end - tbs_bytes, tbs_bytes});
    check(encoded.ok() && encoded.bytes == tbs_bytes && bytes_equal(end - tbs_bytes, input, tbs_bytes),
          "MLS signed package TBS exact output guard and read-only source");
    kdf_sha256_release(&workspace);
    // Every encoded prefix length ends at a guard, rather than merely shortening
    // a readable allocation. Even the one-byte header case must not over-read.
    const u32 cuts[]{0, 1, 2, 3, 4, 63, 64, 65, n - 1};
    for (auto cut : cuts) {
        check(VirtualProtect(mem, page, PAGE_READWRITE, &old) != 0, "MLS guard rewrite");
        copy_bytes(mem + page - cut, buffer, cut);
        check(VirtualProtect(mem, page, PAGE_READONLY, &old) != 0, "MLS guard read-only");
        memset(view, 0xa5, sizeof(*view));
        check(!mls_key_package_decode({mem + page - cut, cut}, view).ok() && filled(view, sizeof(*view)),
              "MLS truncated input guard preserves prior view");
    }
    VirtualFree(mem, 0, MEM_RELEASE);
}
} // namespace
void test_mls_objects(Check check) noexcept {
    lengths(check);
    write_failures(check);
    write_boundaries(check);
    auto* buffer = static_cast<u8*>(VirtualAlloc(nullptr, 16384, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(buffer != nullptr, "MLS object fixture buffer");
    if (!buffer)
        return;
    corpus(check, buffer);
    crypto(check, buffer);
    guards(check, buffer);
    VirtualFree(buffer, 0, MEM_RELEASE);
}
} // namespace maicord
