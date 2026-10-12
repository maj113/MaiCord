#include "maicord/app/dave_welcome.hpp"
#include <bcrypt.h>
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/mls_welcome_crypto_vectors.hpp"
#include "data/mls_group_secrets_vectors.hpp"
bool filled(const void* data, u32 count, u8 value = 0) noexcept {
    const auto* b = static_cast<const u8*>(data);
    for (u32 i = 0; i != count; ++i)
        if (b[i] != value)
            return false;
    return true;
}
struct Digest {
    Check check;
    void* hash;
    ConstSlice<u8> input;
    void field(ConstSlice<u8> value, bool borrowed = true) noexcept {
        if (borrowed && value.count) {
            const auto base = reinterpret_cast<usize>(input.data), p = reinterpret_cast<usize>(value.data);
            check(p >= base && p - base <= input.count && value.count <= input.count - (p - base),
                  "GroupSecrets views borrow supplied plaintext only");
        }
        u8 size[4];
        for (u32 i = 0; i != 4; ++i)
            size[3 - i] = static_cast<u8>(value.count >> (8 * i));
        check(BCryptHashData(hash, size, 4, 0) >= 0, "GroupSecrets normalized field length");
        if (value.count)
            check(BCryptHashData(hash, const_cast<u8*>(value.data), value.count, 0) >= 0,
                  "GroupSecrets normalized field bytes");
    }
    void number(u64 value, u32 size) noexcept {
        u8 b[8];
        for (u32 i = 0; i != size; ++i)
            b[size - i - 1] = static_cast<u8>(value >> (8 * i));
        field({b, size}, false);
    }
};
void group_secrets_cases(Check check, u8* buffer) noexcept {
    void* hash = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
          "GroupSecrets corpus digest init");
    if (!hash)
        return;
    for (auto* fixture : group_secrets_vectors) {
        const u32 n = test_unhex(fixture, buffer);
        MlsGroupSecretsView view{};
        check(mls_group_secrets_decode({buffer, n}, &view).ok(), "Published GroupSecrets decode");
        Digest digest{check, hash, {buffer, n}};
        digest.field(view.joiner_secret);
        digest.number(view.has_path_secret ? 1 : 0, 1);
        digest.field(view.path_secret);
        digest.field(view.psks);
        u32 used = 0;
        while (used < view.psks.count) {
            MlsPreSharedKeyIdView id{};
            const auto result =
                mls_psk_id_decode_prefix({view.psks.data + used, view.psks.count - used}, &id);
            check(result.ok() && result.bytes, "GroupSecrets complete PSK ID iteration");
            if (!result.ok() || !result.bytes)
                break;
            digest.number(id.type, 1);
            digest.number(id.usage, 1);
            digest.field(id.id);
            digest.field(id.group_id);
            digest.number(id.epoch, 8);
            digest.field(id.nonce);
            digest.field(id.serialized);
            MlsPreSharedKeyIdView exact{};
            check(mls_psk_id_decode(id.serialized, &exact).ok(), "PSK exact ID decode");
            used += result.bytes;
        }
        digest.field(view.serialized);
        MlsGroupSecretsView unchanged;
        memset(&unchanged, 0xa5, sizeof(unchanged));
        for (u32 cut = 0; cut != n; ++cut) {
            const auto result = mls_group_secrets_decode({buffer, cut}, &unchanged);
            check(!result.ok() && !result.bytes && result.error_offset <= cut &&
                      filled(&unchanged, sizeof(unchanged), 0xa5),
                  "GroupSecrets every truncation preserves prior view");
        }
        buffer[n] = 0xaa;
        check(mls_group_secrets_decode_prefix({buffer, n + 1}, &unchanged).bytes == n,
              "GroupSecrets prefix consumes exactly one object");
        memset(&unchanged, 0xa5, sizeof(unchanged));
        check(mls_group_secrets_decode({buffer, n + 1}, &unchanged).error == MlsWireError::trailing_data &&
                  filled(&unchanged, sizeof(unchanged), 0xa5),
              "GroupSecrets exact decode rejects trailing data atomically");
    }
    u8 actual[32], expected[32];
    test_unhex(group_secrets_digest, expected);
    check(BCryptFinishHash(hash, actual, 32, 0) >= 0 && bytes_equal(actual, expected, 32),
          "All 303 GroupSecrets cases match independent fields including PSK arms");
    BCryptDestroyHash(hash);
    const u8 bad_optional[]{0, 2, 0}, bad_type[]{3, 0}, bad_usage[]{2, 0, 0};
    MlsGroupSecretsView view{};
    MlsPreSharedKeyIdView id{};
    check(mls_group_secrets_decode({bad_optional, 3}, &view).error == MlsWireError::invalid_optional &&
              mls_psk_id_decode({bad_type, 2}, &id).error == MlsWireError::invalid_psk_type &&
              mls_psk_id_decode({bad_usage, 3}, &id).error == MlsWireError::invalid_psk_usage,
          "GroupSecrets invalid optional and PSK union tags fail explicitly");
}
void crypto_cases(Check check, KdfSha256Workspace& workspace, const P256Key& init, ConstSlice<u8> reference,
                  u8* wire, u8* expected) noexcept {
    for (const auto& fixture : welcome_crypto_vectors) {
        const u32 n = test_unhex(fixture.wire, wire);
        DaveWelcomeCandidate candidate{};
        const auto result = dave_welcome_decrypt(workspace, init, reference, {wire, n}, &candidate);
        check(result.error == fixture.error, fixture.name);
        if (fixture.error == DaveWelcomeError::none && result.ok()) {
            const u32 ns = test_unhex(fixture.secrets, expected);
            check(candidate.storage.locked && candidate.storage.data &&
                      candidate.secrets.serialized.data == candidate.storage.data &&
                      candidate.secrets.serialized.count == ns &&
                      bytes_equal(expected, candidate.storage.data, ns),
                  "Welcome decrypted GroupSecrets match independent plaintext in locked storage");
            const u32 ni = test_unhex(fixture.info, expected);
            check(result.bytes == ni &&
                      candidate.group_info.serialized.data == candidate.storage.data + ns &&
                      candidate.group_info.serialized.count == ni && candidate.storage.bytes == ns + ni &&
                      bytes_equal(expected, candidate.group_info.serialized.data, ni),
                  "Welcome GroupInfo matches independent plaintext in same allocation");
            u8 snapshot[sizeof(candidate)];
            copy_bytes(snapshot, &candidate, sizeof(candidate));
            check(dave_welcome_decrypt(workspace, init, reference, {wire, n}, &candidate).error ==
                          DaveWelcomeError::already_initialized &&
                      bytes_equal(snapshot, &candidate, sizeof(candidate)),
                  "Welcome cannot overwrite a live decrypted candidate");
        } else
            check(!result.bytes && filled(&candidate, sizeof(candidate)),
                  "Failed Welcome preserves empty output owner and publishes no secrets");
        dave_welcome_release(&candidate);
        check(filled(&candidate, sizeof(candidate)), "Welcome release clears full owner and views");
        dave_welcome_release(&candidate);
    }
}
void failures(Check check, KdfSha256Workspace& workspace, const P256Key& init, ConstSlice<u8> reference,
              u8* wire) noexcept {
    const u32 n = test_unhex(welcome_crypto_vectors[0].wire, wire);
    DaveWelcomeCandidate candidate{};
    MlsWelcomeView view{};
    check(mls_welcome_decode({wire, n}, &view).ok(), "Welcome mutation fixture parse");
    MlsEncryptedGroupSecretsView entry{};
    check(mls_encrypted_group_secrets_decode_prefix(view.secrets, &entry).ok(),
          "Welcome mutation recipient");
    const ConstSlice<u8> altered[]{entry.kem_output, entry.ciphertext, view.encrypted_group_info};
    for (const auto span : altered)
        for (u32 i = 0; i != span.count; ++i) {
            auto* byte = const_cast<u8*>(span.data + i);
            *byte ^= 1;
            const auto result = dave_welcome_decrypt(workspace, init, reference, {wire, n}, &candidate);
            check(!result.ok() && !result.bytes && filled(&candidate, sizeof(candidate)),
                  "Every mutated Welcome encapsulation/ciphertext/context byte fails without output");
            *byte ^= 1;
            dave_welcome_release(&candidate);
        }
    u8 wrong_reference[32];
    copy_bytes(wrong_reference, reference.data, 32);
    wrong_reference[0] ^= 1;
    check(dave_welcome_decrypt(workspace, init, {wrong_reference, 32}, {wire, n}, &candidate).error ==
              DaveWelcomeError::missing_reference,
          "Welcome must address the retained pending package");
    P256Key wrong{};
    check(p256_generate(&wrong, P256Kind::ecdh).ok(), "Welcome wrong-key fixture");
    check(dave_welcome_decrypt(workspace, wrong, reference, {wire, n}, &candidate).error ==
              DaveWelcomeError::hpke_authentication_failed,
          "Welcome rejects a different init private key");
    p256_release(&wrong);
    check(dave_welcome_decrypt(workspace, init, entry.new_member, {wire, n}, &candidate).error ==
              DaveWelcomeError::invalid_input,
          "Welcome reference cannot be inferred from its own wire input");
    check(dave_welcome_decrypt(workspace, init, reference, {wire, n},
                               reinterpret_cast<DaveWelcomeCandidate*>(wire))
                  .error == DaveWelcomeError::invalid_input,
          "Welcome output cannot overwrite input wire");
    check(dave_welcome_decrypt(workspace, init, reference, {wire, n},
                               reinterpret_cast<DaveWelcomeCandidate*>(&workspace))
                  .error == DaveWelcomeError::invalid_input,
          "Welcome output cannot overwrite KDF workspace owner");
    check(dave_welcome_decrypt(workspace, init, reference, {wire, n},
                               reinterpret_cast<DaveWelcomeCandidate*>(workspace.storage.data))
                  .error == DaveWelcomeError::invalid_input,
          "Welcome output cannot overwrite KDF workspace storage");
    check(dave_welcome_decrypt(workspace, init, reference, {wire, n},
                               reinterpret_cast<DaveWelcomeCandidate*>(const_cast<P256Key*>(&init)))
                  .error == DaveWelcomeError::invalid_input,
          "Welcome output cannot overwrite pending private key owner");
    const u32 capacity = workspace.storage.capacity;
    workspace.storage.capacity = 1;
    check(dave_welcome_decrypt(workspace, init, reference, {wire, n}, &candidate).error ==
                  DaveWelcomeError::hpke_failure &&
              filled(&candidate, sizeof(candidate)),
          "Welcome CNG failure preserves output");
    workspace.storage.capacity = capacity;
    check(dave_welcome_decrypt(workspace, init, reference, {wire, n}, &candidate).ok(),
          "Welcome retry after provider failure");
    dave_welcome_release(&candidate);
}
void guards(Check check, KdfSha256Workspace& workspace, const P256Key& init, ConstSlice<u8> reference,
            u8* buffer) noexcept {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u32 page = system.dwPageSize;
    auto* memory =
        static_cast<u8*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(memory != nullptr, "Welcome guard allocation");
    if (!memory)
        return;
    const u32 n = test_unhex(welcome_crypto_vectors[0].wire, buffer);
    auto* input = memory + page - n;
    copy_bytes(input, buffer, n);
    auto* output =
        reinterpret_cast<DaveWelcomeCandidate*>(memory + page * 3 - sizeof(DaveWelcomeCandidate));
    DWORD old = 0;
    check(VirtualProtect(memory, page, PAGE_READONLY, &old) &&
              VirtualProtect(memory + page, page, PAGE_NOACCESS, &old) &&
              VirtualProtect(memory + page * 3, page, PAGE_NOACCESS, &old),
          "Welcome read-only input and exact owner guards");
    check(dave_welcome_decrypt(workspace, init, reference, {input, n}, output).ok(),
          "Welcome decrypt with guarded output and immutable input");
    check(output->storage.locked, "Guarded Welcome plaintext is locked");
    dave_welcome_release(output);
    check(filled(output, sizeof(*output)), "Guarded Welcome owner release");
    const u32 secret_bytes = test_unhex(group_secrets_vectors[0], buffer);
    const u32 cuts[]{0, 1, 2, secret_bytes - 1, secret_bytes};
    for (const auto cut : cuts) {
        check(VirtualProtect(memory, page, PAGE_READWRITE, &old) != 0, "GroupSecrets guard input rewrite");
        copy_bytes(memory + page - cut, buffer, cut);
        check(VirtualProtect(memory, page, PAGE_READONLY, &old) != 0, "GroupSecrets immutable guard input");
        auto* view =
            reinterpret_cast<MlsGroupSecretsView*>(memory + page * 3 - sizeof(MlsGroupSecretsView));
        memset(view, 0xa5, sizeof(*view));
        const auto decoded = mls_group_secrets_decode({memory + page - cut, cut}, view);
        check(cut == secret_bytes ? decoded.ok() : !decoded.ok() && filled(view, sizeof(*view), 0xa5),
              "GroupSecrets complete/truncated guard boundaries");
    }
    VirtualFree(memory, 0, MEM_RELEASE);
}
} // namespace
void test_dave_welcome(Check check) noexcept {
    auto* buffers =
        static_cast<u8*>(VirtualAlloc(nullptr, 131072, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(buffers != nullptr, "Welcome fixture buffers");
    if (!buffers)
        return;
    group_secrets_cases(check, buffers);
    u8 scalar[32], reference[32];
    test_unhex(welcome_init_scalar, scalar);
    test_unhex(welcome_package_reference, reference);
    P256Key init{};
    check(p256_import_private(&init, P256Kind::ecdh, {scalar, 32}).ok(),
          "Published Welcome init private key");
    KdfSha256Workspace workspace{};
    DaveWelcomeCandidate empty{};
    check(dave_welcome_decrypt(workspace, init, {reference, 32}, {}, &empty).error ==
                  DaveWelcomeError::not_initialized &&
              filled(&empty, sizeof(empty)),
          "Welcome needs initialized worker workspace");
    check(kdf_sha256_initialize(&workspace).ok(), "Welcome workspace");
    check(dave_welcome_decrypt(workspace, init, {reference, 31}, {}, &empty).error ==
                  DaveWelcomeError::invalid_input &&
              dave_welcome_decrypt(workspace, init, {reference, 32}, {nullptr, 1}, &empty).error ==
                  DaveWelcomeError::invalid_input &&
              filled(&empty, sizeof(empty)),
          "Welcome rejects malformed input/reference spans");
    crypto_cases(check, workspace, init, {reference, 32}, buffers, buffers + 65536);
    failures(check, workspace, init, {reference, 32}, buffers);
    guards(check, workspace, init, {reference, 32}, buffers);
    check(filled(workspace.storage.data, workspace.storage.capacity),
          "Welcome shared hash workspace erased");
    kdf_sha256_release(&workspace);
    p256_release(&init);
    SecureZeroMemory(buffers, 131072);
    VirtualFree(buffers, 0, MEM_RELEASE);
    dave_welcome_release(nullptr);
}
} // namespace maicord
