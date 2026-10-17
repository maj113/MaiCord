#include "maicord/protocol/mls_objects.hpp"
#include <bcrypt.h>
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/mls_join_message_vectors.hpp"
constexpr u32 capacity = 32768;
bool filled(const void* data, u32 n) noexcept {
    const auto* b = static_cast<const u8*>(data);
    for (u32 i = 0; i != n; ++i)
        if (b[i] != 0xa5)
            return false;
    return true;
}
struct Digest {
    Check check;
    void* hash;
    ConstSlice<u8> input;
    void field(ConstSlice<u8> value, bool borrowed = true) noexcept {
        if (borrowed && value.count) {
            const auto start = reinterpret_cast<usize>(input.data), p = reinterpret_cast<usize>(value.data);
            check(p >= start && p - start <= input.count && value.count <= input.count - (p - start),
                  "MLS join fields borrow only supplied wire");
        }
        u8 n[4];
        for (u32 i = 0; i != 4; ++i)
            n[3 - i] = static_cast<u8>(value.count >> (i * 8));
        check(BCryptHashData(hash, n, 4, 0) >= 0, "MLS join normalized field size");
        if (value.count)
            check(BCryptHashData(hash, const_cast<u8*>(value.data), value.count, 0) >= 0,
                  "MLS join normalized field payload");
    }
    void number(u64 value, u32 n) noexcept {
        u8 b[8];
        for (u32 i = 0; i != n; ++i)
            b[n - i - 1] = static_cast<u8>(value >> (i * 8));
        field({b, n}, false);
    }
    void context(const MlsGroupContextView& v) noexcept {
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
template <typename T, MlsDecodeResult (*Exact)(ConstSlice<u8>, T*) noexcept>
T exercise(Check check, u8* data, u32 n) noexcept {
    T view{};
    check(Exact({data, n}, &view).ok(), "MLS independent join object decode");
    T unchanged;
    memset(&unchanged, 0xa5, sizeof(unchanged));
    for (u32 cut = 0; cut != n; ++cut) {
        const auto short_read = Exact({data, cut}, &unchanged);
        check(!short_read.ok() && !short_read.bytes && short_read.error_offset <= cut &&
                  filled(&unchanged, sizeof(unchanged)),
              "MLS every join object truncation is atomic");
    }
    data[n] = 0xaa;
    check(Exact({data, n + 1}, &unchanged).error == MlsWireError::trailing_data &&
              filled(&unchanged, sizeof(unchanged)),
          "MLS join exact decoder rejects trailing bytes");
    return view;
}
// MLSMessage envelope: version(2) wire_format(2), then the Welcome (3) or
// GroupInfo (4) body.
u16 be16(const u8* data) noexcept {
    return static_cast<u16>(data[0] << 8 | data[1]);
}
void corpus(Check check, u8* data) noexcept {
    void* hash = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
          "MLS join corpus digest init");
    if (!hash)
        return;
    for (const auto* hex : mls_join_message_vectors) {
        const u32 n = test_unhex(hex, data);
        const u16 version = be16(data), format = be16(data + 2);
        check(n > 4 && version == 1 && (format == 3 || format == 4), "MLS join envelope is a Welcome or GroupInfo");
        Digest digest{check, hash, {data, n}};
        digest.number(version, 2);
        digest.number(format, 2);
        if (format == 3) {
            auto raw = exercise<MlsWelcomeView, mls_welcome_decode>(check, data + 4, n - 4);
            check(raw.serialized.data == data + 4, "MLS Welcome body excludes header");
            digest.number(raw.cipher_suite, 2);
            digest.field(raw.secrets);
            u32 used = 0;
            while (used < raw.secrets.count) {
                MlsEncryptedGroupSecretsView entry{};
                const auto result = mls_encrypted_group_secrets_decode_prefix(
                    {raw.secrets.data + used, raw.secrets.count - used}, &entry);
                check(result.ok() && result.bytes > 0, "MLS Welcome member entry iteration");
                if (!result.ok() || !result.bytes)
                    break;
                digest.field(entry.new_member);
                digest.field(entry.kem_output);
                digest.field(entry.ciphertext);
                digest.field(entry.serialized);
                used += result.bytes;
            }
            digest.field(raw.encrypted_group_info);
            digest.field(raw.serialized);
        } else {
            auto raw = exercise<MlsGroupInfoView, mls_group_info_decode>(check, data + 4, n - 4);
            check(raw.context.serialized.data == data + 4 && raw.tbs.data == data + 4,
                  "MLS nested GroupInfo signed context excludes message header");
            digest.context(raw.context);
            digest.field(raw.extensions);
            digest.field(raw.confirmation_tag);
            digest.number(raw.signer, 4);
            digest.field(raw.signature);
            digest.field(raw.serialized);
            digest.field(raw.tbs);
        }
        digest.field({data, n});
    }
    u8 expected[32], actual[32];
    test_unhex(mls_join_message_digest, expected);
    check(BCryptFinishHash(hash, actual, 32, 0) >= 0 && bytes_equal(expected, actual, 32),
          "All 604 MLS join messages match independent normalized fields");
    BCryptDestroyHash(hash);
}
void malformed(Check check) noexcept {
    const u8 bad_welcome[]{0, 2, 2, 0, 0, 0};
    MlsWelcomeView unchanged;
    memset(&unchanged, 0xa5, sizeof(unchanged));
    const auto bad = mls_welcome_decode({bad_welcome, sizeof(bad_welcome)}, &unchanged);
    check(bad.error == MlsWireError::truncated && bad.error_offset == 5 &&
              filled(&unchanged, sizeof(unchanged)),
          "MLS Welcome nested entry failures report absolute input offset");
}
// Decodes a body that ends at an inaccessible page into a view that ends at
// another one, from read-only input.
template <typename T, MlsDecodeResult (*Exact)(ConstSlice<u8>, T*) noexcept>
void guard_case(Check check, u8* memory, u32 page, const u8* body, u32 n) noexcept {
    DWORD old = 0;
    auto* output = reinterpret_cast<T*>(memory + page * 3 - sizeof(T));
    check(VirtualProtect(memory, page, PAGE_READWRITE, &old) != 0, "MLS join guard input write");
    copy_bytes(memory + page - n, body, n);
    check(VirtualProtect(memory, page, PAGE_READONLY, &old) != 0, "MLS join immutable wire input");
    check(Exact({memory + page - n, n}, output).ok(), "MLS join exact view output guard");
    const u32 cuts[]{0, 1, 2, 3, 4, 5, n - 1};
    for (const auto cut : cuts) {
        check(VirtualProtect(memory, page, PAGE_READWRITE, &old) != 0, "MLS join guard prefix write");
        copy_bytes(memory + page - cut, body, cut);
        check(VirtualProtect(memory, page, PAGE_READONLY, &old) != 0, "MLS join guard prefix read-only");
        memset(output, 0xa5, sizeof(*output));
        check(!Exact({memory + page - cut, cut}, output).ok() && filled(output, sizeof(*output)),
              "MLS truncated join input guard preserves output");
    }
}
void guards(Check check, u8* buffer) noexcept {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u32 page = system.dwPageSize;
    auto* memory =
        static_cast<u8*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(memory != nullptr, "MLS join guard allocation");
    if (!memory)
        return;
    DWORD old = 0;
    check(VirtualProtect(memory + page, page, PAGE_NOACCESS, &old) &&
              VirtualProtect(memory + page * 3, page, PAGE_NOACCESS, &old),
          "MLS join inaccessible boundaries");
    u32 n = test_unhex(mls_join_message_vectors[0], buffer); // Welcome
    guard_case<MlsWelcomeView, mls_welcome_decode>(check, memory, page, buffer + 4, n - 4);
    n = test_unhex(mls_join_message_vectors[1], buffer); // GroupInfo
    guard_case<MlsGroupInfoView, mls_group_info_decode>(check, memory, page, buffer + 4, n - 4);
    VirtualFree(memory, 0, MEM_RELEASE);
}
} // namespace
void test_mls_join_messages(Check check) noexcept {
    auto* storage = static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(storage != nullptr, "MLS join test buffers");
    if (!storage)
        return;
    corpus(check, storage);
    malformed(check);
    guards(check, storage);
    VirtualFree(storage, 0, MEM_RELEASE);
}
} // namespace maicord
