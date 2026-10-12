#include "maicord/app/hpke_derivation.hpp"
#include "maicord/platform/aes_gcm.hpp"
#include "maicord/core/memory.hpp"
#include <bcrypt.h>
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/hpke_derivation_vectors.hpp"
bool filled(const void* pointer, u32 n, u8 value = 0xa5) noexcept {
    const auto* p = static_cast<const u8*>(pointer);
    for (u32 i = 0; i != n; ++i)
        if (p[i] != value)
            return false;
    return true;
}
void pair(Check check, KdfSha256Workspace& workspace, ConstSlice<u8> ikm, const char* sk,
          const char* pk) noexcept {
    u8 scalar[48], point[80], expected_scalar[32], expected_point[65];
    test_unhex(sk, expected_scalar);
    test_unhex(pk, expected_point);
    memset(scalar, 0xa5, sizeof(scalar));
    memset(point, 0xa5, sizeof(point));
    P256Key key{};
    check(hpke_derive_key_pair(workspace, ikm, &key).ok(), "HPKE RFC/MLSWG/independent key derivation");
    check(p256_export_private(key, {scalar, 48}).ok() && bytes_equal(scalar, expected_scalar, 32) &&
              filled(scalar + 32, 16),
          "HPKE derived scalar matches independent answer");
    check(p256_export_public(key, {point, 80}).ok() && bytes_equal(point, expected_point, 65) &&
              filled(point + 65, 15),
          "HPKE derived public point matches independent answer");
    const auto handle = key.handle;
    check(!hpke_derive_key_pair(workspace, ikm, &key).ok() && key.handle == handle,
          "HPKE derivation preserves live key owner");
    p256_release(&key);
    SecureZeroMemory(scalar, sizeof(scalar));
    SecureZeroMemory(expected_scalar, 32);
}
void kem(Check check, KdfSha256Workspace& workspace) noexcept {
    for (const auto& v : hpke_kems) {
        u8 ske[32], skr[32], pk[65], enc[80], shared[48], received[48], expected_enc[65],
            expected_shared[32];
        test_unhex(v.ephemeral, ske);
        test_unhex(v.recipient, skr);
        test_unhex(v.recipient_point, pk);
        test_unhex(v.enc, expected_enc);
        test_unhex(v.shared, expected_shared);
        P256Key e{}, r{};
        check(p256_import_private(&e, P256Kind::ecdh, {ske, 32}).ok() &&
                  p256_import_private(&r, P256Kind::ecdh, {skr, 32}).ok(),
              "HPKE KEM fixture imports");
        memset(enc, 0xa5, 80);
        memset(shared, 0xa5, 48);
        memset(received, 0xa5, 48);
        check(hpke_encap(workspace, e, {pk, 65}, {enc, 80}, {shared, 48}).ok() &&
                  bytes_equal(enc, expected_enc, 65) && bytes_equal(shared, expected_shared, 32) &&
                  filled(enc + 65, 15) && filled(shared + 32, 16),
              "HPKE encap matches published KEM output/shared secret and tails");
        check(hpke_decap(workspace, r, {enc, 65}, {received, 48}).ok() &&
                  bytes_equal(received, expected_shared, 32) && filled(received + 32, 16),
              "HPKE decap binds both public keys and matches sender");
        memset(shared, 0xa5, 48);
        memset(enc, 0xa5, 80);
        for (u32 cap = 0; cap != 65; ++cap)
            check(hpke_encap(workspace, e, {pk, 65}, {enc, cap}, {shared, 48}).error ==
                          HpkeDerivationError::output_capacity &&
                      filled(enc, 80) && filled(shared, 48),
                  "HPKE short encapsulation output is atomic");
        for (u32 cap = 0; cap != 32; ++cap) {
            check(hpke_encap(workspace, e, {pk, 65}, {enc, 80}, {shared, cap}).error ==
                          HpkeDerivationError::output_capacity &&
                      filled(enc, 80) && filled(shared, 48),
                  "HPKE short shared output preserves both destinations");
            check(hpke_decap(workspace, r, {pk, 65}, {shared, cap}).error ==
                          HpkeDerivationError::output_capacity &&
                      filled(shared, 48),
                  "HPKE decap short capacity preflight");
        }
        u8 invalid[65]{};
        invalid[0] = 4;
        check(!hpke_encap(workspace, e, {invalid, 65}, {enc, 80}, {shared, 48}).ok() && filled(enc, 80) &&
                  filled(shared, 48),
              "HPKE invalid recipient is atomic");
        check(!hpke_decap(workspace, r, {invalid, 65}, {shared, 48}).ok() && filled(shared, 48),
              "HPKE invalid encapsulation is atomic");
        check(!hpke_encap(workspace, e, {pk, 65}, {enc, 80}, {enc + 1, 32}).ok() && filled(enc, 80),
              "HPKE coupled outputs cannot overlap");
        check(!hpke_encap(workspace, e, {pk, 65}, {enc, 80}, {workspace.storage.data, 32}).ok() &&
                  filled(enc, 80),
              "HPKE output cannot alias erased KDF storage");
        const u32 capacity = workspace.storage.capacity;
        workspace.storage.capacity = 1;
        check(hpke_encap(workspace, e, {pk, 65}, {enc, 80}, {shared, 48}).error ==
                      HpkeDerivationError::kdf_failure &&
                  filled(enc, 80) && filled(shared, 48),
              "HPKE failed KEM KDF preserves both outputs");
        check(hpke_decap(workspace, r, {expected_enc, 65}, {shared, 48}).error ==
                      HpkeDerivationError::kdf_failure &&
                  filled(shared, 48),
              "HPKE failed decap KDF preserves output");
        workspace.storage.capacity = capacity;
        check(hpke_decap(workspace, r, {expected_enc, 65}, {shared, 48}).ok() &&
                  bytes_equal(shared, expected_shared, 32),
              "HPKE decap retries after KDF failure");
        p256_release(&e);
        p256_release(&r);
        SecureZeroMemory(ske, 32);
        SecureZeroMemory(skr, 32);
        SecureZeroMemory(shared, 48);
        SecureZeroMemory(received, 48);
    }
}
void schedule(Check check, KdfSha256Workspace& workspace, u8* data) noexcept {
    u8 shared[32], info[128];
    test_unhex(hpke_base_shared, shared);
    const u32 n = test_unhex(hpke_base_info, info);
    HpkeBaseSecrets actual{}, expected{};
    test_unhex(hpke_base_material, reinterpret_cast<u8*>(&expected));
    check(hpke_base_key_schedule(workspace, {shared, 32}, {info, n}, {}, &actual).ok() &&
              bytes_equal(&actual, &expected, sizeof(actual)),
          "HPKE published Base key, nonce and exporter secret");
    AesGcmKey cipher{};
    check(aes_gcm_initialize(&cipher, {actual.key, 16}).ok(), "HPKE derived AES key initialization");
    for (const auto& v : hpke_encryptions) {
        u8 aad[128], pt[128], ct[144], nonce[12], out[160], tag[16];
        const u32 na = test_unhex(v.aad, aad), np = test_unhex(v.pt, pt), nc = test_unhex(v.ct, ct);
        test_unhex(v.nonce, nonce);
        memset(out, 0xa5, 160);
        check(aes_gcm_encrypt(cipher, {nonce, 12}, {aad, na}, {pt, np}, {out, 160}, {tag, 16}).ok() &&
                  nc == np + 16 && bytes_equal(out, ct, np) && bytes_equal(tag, ct + np, 16) &&
                  filled(out + np, 160 - np),
              "HPKE derived key opens RFC AEAD ciphertext path");
        memset(out, 0xa5, 160);
        check(aes_gcm_decrypt(cipher, {nonce, 12}, {aad, na}, {ct, nc - 16}, {ct + nc - 16, 16}, {out, 160})
                      .ok() &&
                  bytes_equal(out, pt, np),
              "HPKE derived key decrypts published ciphertext");
    }
    aes_gcm_release(&cipher);
    u8 context[64], expected_export[32], output[8168];
    for (const auto& v : hpke_exports) {
        const u32 count = test_unhex(v.context, context);
        test_unhex(v.output, expected_export);
        memset(output, 0xa5, sizeof(output));
        check(hpke_export_secret(workspace, {actual.exporter_secret, 32}, {context, count}, {output, 32})
                      .ok() &&
                  bytes_equal(output, expected_export, 32) && filled(output + 32, sizeof(output) - 32),
              "HPKE exporter published vector/tails");
    }
    void* hash = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
          "HPKE export matrix digest init");
    if (hash) {
        for (auto nc : hpke_export_contexts)
            for (auto no : hpke_export_lengths) {
                memset(output, 0xa5, sizeof(output));
                check(hpke_export_secret(workspace, {actual.exporter_secret, 32}, {data, nc}, {output, no})
                              .ok() &&
                          filled(output + no, sizeof(output) - no),
                      "HPKE export context/length boundaries");
                check(BCryptHashData(hash, output, no, 0) >= 0, "HPKE export matrix aggregation");
            }
        u8 answer[32], digest[32];
        test_unhex(hpke_export_digest, answer);
        check(BCryptFinishHash(hash, digest, 32, 0) >= 0 && bytes_equal(answer, digest, 32),
              "HPKE exporter matrix independent oracle");
        BCryptDestroyHash(hash);
    }
    for (const auto& v : hpke_info_boundaries) {
        test_unhex(v.material, reinterpret_cast<u8*>(&expected));
        const u32 splits[]{0, v.length / 2, v.length};
        for (auto split : splits) {
            memset(&actual, 0xa5, sizeof(actual));
            check(hpke_base_key_schedule(workspace, {shared, 32}, {data, split},
                                         {data + split, v.length - split}, &actual)
                          .ok() &&
                      bytes_equal(&actual, &expected, sizeof(actual)),
                  "HPKE arbitrary split info including beyond reference setup limit");
        }
        // Replace borrowed shared-secret storage only after all derivation succeeds.
        copy_bytes(&actual, shared, 32);
        check(hpke_base_key_schedule(workspace, {reinterpret_cast<const u8*>(&actual), 32},
                                     {data, v.length}, {}, &actual)
                      .ok() &&
                  bytes_equal(&actual, &expected, sizeof(actual)),
              "HPKE schedule stages aliased input/output");
    }
    memset(&actual, 0xa5, sizeof(actual));
    check(!hpke_base_key_schedule(workspace, {shared, 31}, {}, {}, &actual).ok() &&
              filled(&actual, sizeof(actual)),
          "HPKE invalid shared-secret length is atomic");
    check(!hpke_base_key_schedule(workspace, {shared, 32}, {workspace.storage.data, 1}, {}, &actual).ok() &&
              filled(&actual, sizeof(actual)),
          "HPKE schedule input cannot alias KDF storage");
    const u32 capacity = workspace.storage.capacity;
    workspace.storage.capacity = 1;
    check(hpke_base_key_schedule(workspace, {shared, 32}, {info, n}, {}, &actual).error ==
                  HpkeDerivationError::kdf_failure &&
              filled(&actual, sizeof(actual)),
          "HPKE schedule provider failure is atomic");
    P256Key failed{};
    check(hpke_derive_key_pair(workspace, {shared, 32}, &failed).error ==
                  HpkeDerivationError::kdf_failure &&
              !failed.handle,
          "HPKE key derivation failure keeps empty owner");
    memset(output, 0xa5, sizeof(output));
    check(hpke_export_secret(workspace, {shared, 32}, {}, {output, 33}).error ==
                  KdfError::platform_failure &&
              filled(output, 33, 0) && filled(output + 33, sizeof(output) - 33),
          "HPKE exporter native failure erases only requested bytes");
    check(hpke_export_secret(workspace, {shared, 32}, {}, {output, 0}).ok(),
          "HPKE zero-length export skips failed provider");
    workspace.storage.capacity = capacity;
    check(!hpke_derive_key_pair(workspace, {reinterpret_cast<const u8*>(&failed), 1}, &failed).ok() &&
              !failed.handle,
          "HPKE derived owner cannot overlap IKM");
    check(!hpke_derive_key_pair(workspace, {workspace.storage.data, 1}, &failed).ok() && !failed.handle,
          "HPKE derived IKM cannot alias KDF storage");
    check(hpke_base_key_schedule(workspace, {shared, 32}, {info, n}, {}, &actual).ok(),
          "HPKE schedule retry after failure");
    hpke_base_secrets_clear(&actual);
    hpke_base_secrets_clear(&expected);
    check(filled(&actual, sizeof(actual), 0) &&
              filled(workspace.storage.data, workspace.storage.capacity, 0),
          "HPKE candidate and KDF retirement erasure");
    SecureZeroMemory(shared, 32);
    SecureZeroMemory(output, sizeof(output));
}
void guards(Check check, KdfSha256Workspace& workspace) noexcept {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u32 page = system.dwPageSize;
    auto* allocation =
        static_cast<u8*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(allocation != nullptr, "HPKE guards allocation");
    if (!allocation)
        return;
    auto* input = allocation + page - 65;
    auto* end = allocation + page * 3;
    test_unhex(hpke_kems[0].recipient_point, input);
    u8 scalar[32], ikm[32];
    test_unhex(hpke_kems[0].ephemeral, scalar);
    test_unhex(hpke_pairs[0].ikm, ikm);
    DWORD old = 0;
    check(VirtualProtect(allocation, page, PAGE_READONLY, &old) &&
              VirtualProtect(allocation + page, page, PAGE_NOACCESS, &old) &&
              VirtualProtect(end, page, PAGE_NOACCESS, &old),
          "HPKE guard pages and immutable point");
    P256Key e{};
    check(p256_import_private(&e, P256Kind::ecdh, {scalar, 32}).ok(), "HPKE guard ephemeral");
    check(hpke_encap(workspace, e, {input, 65}, {end - 97, 65}, {end - 32, 32}).ok(),
          "HPKE exact KEM output boundaries");
    p256_release(&e);
    check(VirtualProtect(allocation, page, PAGE_READWRITE, &old) != 0, "HPKE restore input guard page");
    copy_bytes(allocation + page - 32, ikm, 32);
    check(VirtualProtect(allocation, page, PAGE_READONLY, &old) != 0, "HPKE immutable IKM");
    auto* key = reinterpret_cast<P256Key*>(end - sizeof(P256Key));
    *key = {};
    check(hpke_derive_key_pair(workspace, {allocation + page - 32, 32}, key).ok(),
          "HPKE exact derived owner boundary");
    p256_release(key);
    auto* secrets = reinterpret_cast<HpkeBaseSecrets*>(end - sizeof(HpkeBaseSecrets));
    check(hpke_base_key_schedule(workspace, {allocation + page - 32, 32}, {allocation + page - 31, 15},
                                 {allocation + page - 16, 16}, secrets)
              .ok(),
          "HPKE exact schedule output and segmented input boundaries");
    hpke_base_secrets_clear(secrets);
    SecureZeroMemory(scalar, 32);
    SecureZeroMemory(ikm, 32);
    VirtualFree(allocation, 0, MEM_RELEASE);
}
} // namespace
void test_hpke_derivation(Check check) noexcept {
    KdfSha256Workspace workspace{};
    check(kdf_sha256_initialize(&workspace).ok(), "HPKE workspace init");
    if (!workspace.storage.data)
        return;
    auto* data = static_cast<u8*>(VirtualAlloc(nullptr, 4194305, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(data != nullptr, "HPKE large info fixture buffer");
    if (data) {
        for (u32 i = 0; i != 4194305; ++i)
            data[i] = static_cast<u8>(i * 13 + 7);
        for (const auto& v : hpke_pairs) {
            u8 ikm[32];
            const u32 n = test_unhex(v.ikm, ikm);
            pair(check, workspace, {ikm, n}, v.scalar, v.point);
            SecureZeroMemory(ikm, 32);
        }
        for (const auto& v : hpke_ikm_boundaries)
            pair(check, workspace, {data, v.length}, v.scalar, v.point);
        kem(check, workspace);
        schedule(check, workspace, data);
        guards(check, workspace);
        VirtualFree(data, 0, MEM_RELEASE);
    }
    kdf_sha256_release(&workspace);
}
} // namespace maicord
