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
template <typename T, MlsDecodeResult (*Exact)(ConstSlice<u8>, T*) noexcept,
          MlsDecodeResult (*Prefix)(ConstSlice<u8>, T*) noexcept>
T exercise(Check check, u8* data, u32 n, u8* encoded) noexcept {
    T view{};
    check(Exact({data, n}, &view).ok(), "MLS independent join object decode");
    memset(encoded, 0xa5, capacity);
    const auto size = mls_encoded_size(view);
    const auto wrote = mls_encode(view, {encoded, capacity});
    check(size.ok() && size.bytes == n && wrote.ok() && wrote.bytes == n && bytes_equal(encoded, data, n) &&
              filled(encoded + n, capacity - n),
          "MLS independent join object exact reserialization");
    T unchanged;
    memset(&unchanged, 0xa5, sizeof(unchanged));
    for (u32 cut = 0; cut != n; ++cut) {
        const auto short_read = Exact({data, cut}, &unchanged);
        check(!short_read.ok() && !short_read.bytes && short_read.error_offset <= cut &&
                  filled(&unchanged, sizeof(unchanged)),
              "MLS every join object truncation is atomic");
    }
    data[n] = 0xaa;
    check(Prefix({data, n + 1}, &unchanged).bytes == n, "MLS join prefix consumes one object");
    memset(&unchanged, 0xa5, sizeof(unchanged));
    check(Exact({data, n + 1}, &unchanged).error == MlsWireError::trailing_data &&
              filled(&unchanged, sizeof(unchanged)),
          "MLS join exact decoder rejects trailing bytes");
    const u32 capacities[]{0, n / 2, n - 1};
    for (const auto available : capacities) {
        memset(encoded, 0xa5, capacity);
        check(mls_encode(view, {encoded, available}).error == MlsWireError::output_capacity &&
                  filled(encoded, capacity),
              "MLS join capacity failure never writes output");
    }
    check(mls_encode(view, {reinterpret_cast<u8*>(&view), sizeof(view)}).error ==
              MlsWireError::invalid_input,
          "MLS join writer rejects input descriptor overlap");
    return view;
}
void corpus(Check check, u8* data, u8* encoded) noexcept {
    void* hash = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
          "MLS join corpus digest init");
    if (!hash)
        return;
    for (const auto* hex : mls_join_message_vectors) {
        const u32 n = test_unhex(hex, data);
        auto message = exercise<MlsMessageView, mls_message_decode, mls_message_decode_prefix>(check, data,
                                                                                               n, encoded);
        Digest digest{check, hash, {data, n}};
        digest.number(message.version, 2);
        digest.number(static_cast<u16>(message.format), 2);
        if (message.format == MlsWireFormat::welcome) {
            auto raw = exercise<MlsWelcomeView, mls_welcome_decode, mls_welcome_decode_prefix>(
                check, data + 4, n - 4, encoded);
            check(raw.serialized.data == message.welcome.serialized.data,
                  "MLS Welcome body excludes header");
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
                const auto wrote = mls_encode(entry, {encoded, capacity});
                check(wrote.ok() && wrote.bytes == result.bytes &&
                          bytes_equal(encoded, entry.serialized.data, wrote.bytes),
                      "MLS Welcome encrypted member entry exact roundtrip");
                MlsEncryptedGroupSecretsView exact{};
                check(mls_encrypted_group_secrets_decode(entry.serialized, &exact).ok(),
                      "MLS exact encrypted member entry");
                used += result.bytes;
            }
            digest.field(raw.encrypted_group_info);
            digest.field(raw.serialized);
        } else {
            auto raw = exercise<MlsGroupInfoView, mls_group_info_decode, mls_group_info_decode_prefix>(
                check, data + 4, n - 4, encoded);
            check(raw.context.serialized.data == data + 4 && raw.tbs.data == data + 4 &&
                      message.group_info.context.serialized.data == data + 4 &&
                      message.group_info.tbs.data == data + 4,
                  "MLS nested GroupInfo signed context excludes message header");
            digest.context(raw.context);
            digest.field(raw.extensions);
            digest.field(raw.confirmation_tag);
            digest.number(raw.signer, 4);
            digest.field(raw.signature);
            digest.field(raw.serialized);
            digest.field(raw.tbs);
            raw.signature = {nullptr, 1};
            const auto tbs = mls_encode_tbs(raw, {encoded, capacity});
            check(tbs.ok() && tbs.bytes == raw.tbs.count && bytes_equal(encoded, raw.tbs.data, tbs.bytes),
                  "MLS GroupInfo TBS exactly matches independent signed bytes");
        }
        digest.field(message.serialized);
    }
    u8 expected[32], actual[32];
    test_unhex(mls_join_message_digest, expected);
    check(BCryptFinishHash(hash, actual, 32, 0) >= 0 && bytes_equal(expected, actual, 32),
          "All 604 MLS join messages match independent normalized fields");
    BCryptDestroyHash(hash);
}
void malformed(Check check, u8* data, u8* encoded) noexcept {
    MlsMessageView output;
    memset(&output, 0xa5, sizeof(output));
    const u8 headers[][4]{{0, 2, 0, 3}, {0, 1, 0, 0}, {0, 1, 0, 1}, {0, 1, 0, 2}, {0, 1, 0, 6}};
    for (const auto& header : headers) {
        const auto result = mls_message_decode({header, 4}, &output);
        check(result.error == (header[1] == 2 ? MlsWireError::unsupported_version
                                              : MlsWireError::unsupported_wire_format) &&
                  filled(&output, sizeof(output)),
              "MLS unsupported message version/arms fail explicitly");
    }
    const u32 n = test_unhex(mls_join_message_vectors[1], data); // GroupInfo
    data[5] = 2;
    check(mls_message_decode({data, n}, &output).error == MlsWireError::version_mismatch &&
              filled(&output, sizeof(output)),
          "MLS envelope and GroupInfo version mismatch rejected atomically");
    data[5] = 1;
    check(mls_message_decode({data, n}, &output).ok(), "MLS message recovery after version mismatch");
    output.group_info.context.version = 2;
    memset(encoded, 0xa5, capacity);
    check(mls_encode(output, {encoded, capacity}).error == MlsWireError::version_mismatch &&
              filled(encoded, capacity),
          "MLS writer rejects mismatched nested version without output");
    output.format = MlsWireFormat::public_message;
    check(mls_encoded_size(output).error == MlsWireError::unsupported_wire_format &&
              mls_encode(output, {encoded, capacity}).error == MlsWireError::unsupported_wire_format &&
              filled(encoded, capacity),
          "MLS writer cannot silently emit an unsupported arm");
    const u8 bad_entries[]{0, 0}; // Missing third vector
    MlsWelcomeView welcome{2, {bad_entries, 2}, {}, {}};
    check(mls_encoded_size(welcome).error == MlsWireError::truncated &&
              mls_encode(welcome, {encoded, capacity}).error == MlsWireError::truncated &&
              filled(encoded, capacity),
          "MLS Welcome writer validates complete entry triples before emitting");
    const u8 bad_welcome[]{0, 2, 2, 0, 0, 0};
    MlsWelcomeView unchanged;
    memset(&unchanged, 0xa5, sizeof(unchanged));
    const auto bad = mls_welcome_decode({bad_welcome, sizeof(bad_welcome)}, &unchanged);
    check(bad.error == MlsWireError::truncated && bad.error_offset == 5 &&
              filled(&unchanged, sizeof(unchanged)),
          "MLS Welcome nested entry failures report absolute input offset");
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
    auto* output = reinterpret_cast<MlsMessageView*>(memory + page * 3 - sizeof(MlsMessageView));
    for (u32 kind = 0; kind != 2; ++kind) {
        const u32 n = test_unhex(mls_join_message_vectors[kind], buffer);
        auto* input = memory + page - n;
        check(VirtualProtect(memory, page, PAGE_READWRITE, &old) != 0, "MLS join guard input write");
        copy_bytes(input, buffer, n);
        check(VirtualProtect(memory, page, PAGE_READONLY, &old) != 0, "MLS join immutable wire input");
        check(mls_message_decode({input, n}, output).ok(), "MLS join exact view output guard");
        const auto view = *output;
        auto* encoded = memory + page * 3 - n;
        const auto wrote = mls_encode(view, {encoded, n});
        check(wrote.ok() && wrote.bytes == n && bytes_equal(encoded, input, n),
              "MLS join exact serialized output guard with read-only source");
        const u32 cuts[]{0, 1, 2, 3, 4, 5, n - 1};
        for (const auto cut : cuts) {
            check(VirtualProtect(memory, page, PAGE_READWRITE, &old) != 0, "MLS join guard prefix write");
            copy_bytes(memory + page - cut, buffer, cut);
            check(VirtualProtect(memory, page, PAGE_READONLY, &old) != 0,
                  "MLS join guard prefix read-only");
            memset(output, 0xa5, sizeof(*output));
            check(!mls_message_decode({memory + page - cut, cut}, output).ok() &&
                      filled(output, sizeof(*output)),
                  "MLS truncated join input guard preserves output");
        }
    }
    VirtualFree(memory, 0, MEM_RELEASE);
}
} // namespace
void test_mls_join_messages(Check check) noexcept {
    auto* storage =
        static_cast<u8*>(VirtualAlloc(nullptr, capacity * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(storage != nullptr, "MLS join test buffers");
    if (!storage)
        return;
    corpus(check, storage, storage + capacity);
    malformed(check, storage, storage + capacity);
    guards(check, storage);
    VirtualFree(storage, 0, MEM_RELEASE);
}
} // namespace maicord
