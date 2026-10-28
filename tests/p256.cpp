#include "maicord/platform/p256.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/crypto.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/p256_vectors.hpp"
void scalar_corpus(Check check) noexcept {
    constexpr P256Kind kinds[]{P256Kind::ecdh, P256Kind::ecdsa};
    for (auto kind : kinds) {
        void* hash = nullptr;
        check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
              "P256 point corpus digest init");
        if (!hash)
            return;
        for (const auto* hex : p256_scalars) {
            u8 scalar[32], exported[48], point[80];
            test_unhex(hex, scalar);
            memset(exported, 0xa5, sizeof(exported));
            memset(point, 0xa5, sizeof(point));
            P256Key key{};
            check(p256_import_private(&key, kind, {scalar, 32}).ok(),
                  "CNG derives P256 public point directly from scalar");
            if (key.handle) {
                check(p256_export_public(key, {point, 80}).ok() && test_filled(point + 65, 15, 0xa5),
                      "P256 public export preserves tail");
                check(p256_export_private(key, {exported, 48}).ok() && bytes_equal(scalar, exported, 32) &&
                          test_filled(exported + 32, 16, 0xa5),
                      "P256 scalar import/export preserves exact full-width big-endian value");
                check(BCryptHashData(hash, point, 65, 0) >= 0, "P256 public corpus aggregation");
            }
            p256_release(&key);
            SecureZeroMemory(scalar, sizeof(scalar));
            SecureZeroMemory(exported, sizeof(exported));
        }
        u8 expected[32], actual[32];
        test_unhex(p256_public_digest, expected);
        check(BCryptFinishHash(hash, actual, 32, 0) >= 0 && bytes_equal(actual, expected, 32),
              "P256 all scalar bits, order boundary and random scalars match independent OpenSSL points");
        BCryptDestroyHash(hash);
    }
}
void vectors(Check check) noexcept {
    for (const auto& v : p256_vectors) {
        u8 scalar[32], point[65], peer[65], shared[32], output[80], digest[32], signature[64];
        test_unhex(v.scalar, scalar);
        test_unhex(v.point, point);
        test_unhex(v.peer, peer);
        test_unhex(v.shared, shared);
        test_unhex(v.digest, digest);
        test_unhex(v.signature, signature);
        P256Key dh{}, other{}, signing{}, verifier{};
        check(p256_import_private(&dh, P256Kind::ecdh, {scalar, 32}, {point, 65}).ok() &&
                  p256_import_public(&other, P256Kind::ecdh, {peer, 65}).ok(),
              "P256 independent ECDH key imports");
        memset(output, 0xa5, sizeof(output));
        check(p256_ecdh(dh, other, {output, 80}).ok() && bytes_equal(output, shared, 32) &&
                  test_filled(output + 32, 48, 0xa5),
              "P256 raw ECDH matches OpenSSL including leading-zero big-endian shared secret");
        check(p256_import_private(&signing, P256Kind::ecdsa, {scalar, 32}).ok() &&
                  p256_import_public(&verifier, P256Kind::ecdsa, {point, 65}).ok(),
              "P256 signature key imports");
        check(p256_verify_digest(verifier, {digest, 32}, {signature, 64}).ok(),
              "CNG verifies independent OpenSSL P1363 signature");
        for (u32 i = 0; i != 64; ++i) {
            signature[i] ^= 1;
            check(!p256_verify_digest(verifier, {digest, 32}, {signature, 64}).ok(),
                  "P256 signature tampering is rejected");
            signature[i] ^= 1;
        }
        digest[0] ^= 1;
        check(p256_verify_digest(verifier, {digest, 32}, {signature, 64}).error ==
                  P256Error::invalid_signature,
              "P256 wrong digest has distinct authentication failure");
        digest[0] ^= 1;
        memset(output, 0xa5, sizeof(output));
        check(p256_sign_digest(signing, {digest, 32}, {output, 80}).ok() && test_filled(output + 64, 16, 0xa5) &&
                  p256_verify_digest(verifier, {digest, 32}, {output, 64}).ok(),
              "P256 native signature verifies and preserves tail");
        p256_release(&dh);
        p256_release(&other);
        p256_release(&signing);
        p256_release(&verifier);
        SecureZeroMemory(scalar, sizeof(scalar));
        SecureZeroMemory(shared, sizeof(shared));
        SecureZeroMemory(output, sizeof(output));
    }
}
void boundaries(Check check) noexcept {
    P256Key key{}, pub{}, wrong{};
    u8 scalar[32]{}, point[65], output[80], digest[32]{}, signature[64];
    scalar[31] = 1;
    check(p256_import_private(&key, P256Kind::ecdsa, {scalar, 32}).ok(), "P256 boundary signing key");
    check(p256_export_public(key, {point, 65}).ok() &&
              p256_import_public(&pub, P256Kind::ecdsa, {point, 65}).ok(),
          "P256 boundary verifier");
    check(p256_generate(&key, P256Kind::ecdsa).error == P256Error::invalid_input,
          "P256 cannot replace a live key owner");
    memset(output, 0xa5, sizeof(output));
    for (u32 n = 0; n != 65; ++n)
        check(p256_export_public(key, {output, n}).error == P256Error::output_capacity &&
                  test_filled(output, 80, 0xa5),
              "P256 short public output unchanged");
    for (u32 n = 0; n != 64; ++n)
        check(p256_sign_digest(key, {digest, 32}, {output, n}).error == P256Error::output_capacity &&
                  test_filled(output, 80, 0xa5),
              "P256 short signature output unchanged");
    check(p256_export_private(pub, {output, 32}).error == P256Error::private_required &&
              p256_sign_digest(pub, {digest, 32}, {output, 64}).error == P256Error::private_required,
          "P256 public owner cannot expose private material or sign");
    check(p256_export_public(key, {reinterpret_cast<u8*>(&key), 65}).error == P256Error::invalid_input &&
              p256_sign_digest(key, {output, 32}, {output, 64}).error == P256Error::invalid_input,
          "P256 protects owner/input aliases");
    scalar[31] = 2;
    check(!p256_import_private(&wrong, P256Kind::ecdh, {scalar, 32}, {point, 65}).ok() && !wrong.handle,
          "P256 rejects a mismatched public/private key pair");
    memset(scalar, 0, sizeof(scalar));
    check(!p256_import_private(&wrong, P256Kind::ecdh, {scalar, 32}).ok(), "P256 rejects zero scalar");
    test_unhex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", scalar);
    check(!p256_import_private(&wrong, P256Kind::ecdh, {scalar, 32}).ok(),
          "P256 rejects group order instead of reducing it");
    memset(scalar, 0xff, sizeof(scalar));
    check(!p256_import_private(&wrong, P256Kind::ecdh, {scalar, 32}).ok(),
          "P256 rejects scalar beyond order");
    memset(point, 0, sizeof(point));
    point[0] = 4;
    check(!p256_import_public(&wrong, P256Kind::ecdh, {point, 65}).ok(),
          "P256 rejects point at infinity/zero coordinates");
    memset(point + 1, 0xff, 64);
    check(!p256_import_public(&wrong, P256Kind::ecdh, {point, 65}).ok(),
          "P256 CNG rejects off-curve coordinates");
    P256Key generated{};
    check(p256_generate(&generated, P256Kind::ecdsa).ok() &&
              p256_sign_digest(generated, {digest, 32}, {signature, 64}).ok(),
          "P256 generates fresh native signing key");
    p256_release(&generated);
    check(p256_generate(&wrong, P256Kind::ecdh).ok() &&
              p256_sign_digest(wrong, {digest, 32}, {output, 64}).error == P256Error::wrong_kind &&
              p256_ecdh(key, wrong, {output, 32}).error == P256Error::wrong_kind,
          "P256 purpose separation prevents ECDH/ECDSA misuse");
    p256_release(&key);
    p256_release(&pub);
    p256_release(&wrong);
    p256_release(&key);
    check(!key.handle && !key.has_private, "P256 release retires handle and private capability");
    SecureZeroMemory(scalar, sizeof(scalar));
}
void guards(Check check) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto* input = static_cast<u8*>(
        VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* output = static_cast<u8*>(
        VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD previous = 0;
    const bool ready = input && output &&
                       VirtualProtect(input + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous) &&
                       VirtualProtect(output + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous);
    check(ready, "P256 guarded input and output pages");
    P256Key signing{}, verifying{}, dh{}, peer{};
    if (ready) {
        auto* scalar = input + info.dwPageSize - 32;
        test_unhex(p256_vectors[0].scalar, scalar);
        check(VirtualProtect(input, info.dwPageSize, PAGE_READONLY, &previous) != 0,
              "P256 immutable scalar input");
        check(p256_import_private(&signing, P256Kind::ecdsa, {scalar, 32}).ok() &&
                  p256_import_private(&dh, P256Kind::ecdh, {scalar, 32}).ok(),
              "P256 scalar imports respect inaccessible following page");
        check(p256_export_private(signing, {output + info.dwPageSize - 32, 32}).ok() &&
                  bytes_equal(scalar, output + info.dwPageSize - 32, 32),
              "P256 private export respects exact guard capacity");
        check(
            p256_export_public(signing, {output + info.dwPageSize - 65, 65}).ok() &&
                p256_import_public(&verifying, P256Kind::ecdsa, {output + info.dwPageSize - 65, 65}).ok() &&
                p256_import_public(&peer, P256Kind::ecdh, {output + info.dwPageSize - 65, 65}).ok(),
            "P256 public import/export respects exact SEC1 guarded boundary");
        check(VirtualProtect(input, info.dwPageSize, PAGE_READWRITE, &previous) != 0,
              "P256 guarded input restore");
        auto* digest = input + info.dwPageSize - 32;
        test_unhex(p256_vectors[0].digest, digest);
        check(VirtualProtect(input, info.dwPageSize, PAGE_READONLY, &previous) != 0,
              "P256 immutable digest input");
        check(p256_sign_digest(signing, {digest, 32}, {output + info.dwPageSize - 64, 64}).ok() &&
                  p256_verify_digest(verifying, {digest, 32}, {output + info.dwPageSize - 64, 64}).ok(),
              "P256 signature sign/verify respects exact digest and signature boundaries");
        u8 unchanged[32];
        memset(unchanged, 0xa5, sizeof(unchanged));
        for (u32 n = 0; n != 32; ++n)
            check(p256_ecdh(dh, peer, {unchanged, n}).error == P256Error::output_capacity &&
                      p256_export_private(signing, {unchanged, n}).error == P256Error::output_capacity &&
                      test_filled(unchanged, 32, 0xa5),
                  "P256 short private/shared-secret outputs remain untouched");
        check(p256_ecdh(dh, peer, {output + info.dwPageSize - 32, 32}).ok(),
              "P256 raw shared-secret output respects guard");
    }
    p256_release(&signing);
    p256_release(&verifying);
    p256_release(&dh);
    p256_release(&peer);
    if (input)
        VirtualFree(input, 0, MEM_RELEASE);
    if (output)
        VirtualFree(output, 0, MEM_RELEASE);
}
} // namespace
void test_p256(Check check) noexcept {
    scalar_corpus(check);
    vectors(check);
    boundaries(check);
    guards(check);
}
} // namespace maicord
