#include "maicord/platform/aes_gcm.hpp"
#include "maicord/core/memory.hpp"

#include <bcrypt.h>
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;


bool equal(const u8* a, const u8* b, u32 count) noexcept {
    for (u32 i = 0; i != count; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

void known_answers(Check check) noexcept {
    // NIST AES_GCM.pdf, AES128/AES256 examples 1-5 (96-bit IV, 128-bit tag).
    // https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Standards-and-Guidelines/documents/examples/AES_GCM.pdf
    u8 secret[32], nonce[12], aad[64], plain[64], expected[64], tag_expected[16];
    test_unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", secret);
    test_unhex("cafebabefacedbaddecaf888", nonce);
    test_unhex("3ad77bb40d7a3660a89ecaf32466ef97f5d3d58503b9699de785895a96fdbaaf"
          "43b1cd7f598ece23881b00e3ed0306887b0c785e27e8ad3f8223207104725dd4",
          aad);
    test_unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
          "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
          plain);
    const char* ciphertexts[]{"42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
                              "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985",
                              "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
                              "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad"};
    const char* tags[2][5]{{"3247184b3c4f69a44dbcd22887bbb418", "4d5c2af327cd64a62cf35abd2ba6fab4",
                            "5f91d77123ef5eb9997913849b8dc1e9", "64c0232904af398a5b67c10b53a5024d",
                            "f07c2528eea2fca1211f905e1b6a881b"},
                           {"fd2caa16a5832e76aa132c1453eeda7e", "b094dac5d93471bdec1a502270e3cc6c",
                            "de34b6dcd4cee2fdbec3cea01af1ee44", "c06d76f31930fef37acae23ed465ae62",
                            "e097195f4532da895fb917a5a55c6aa0"}};
    constexpr u32 sizes[]{0, 64, 0, 64, 60};
    constexpr u32 aad_sizes[]{0, 0, 64, 64, 20};
    for (u32 k = 0; k != 2; ++k) {
        AesGcmKey key{};
        check(aes_gcm_initialize(&key, {secret, k ? 32u : 16u}).ok(), "AES NIST key initialization");
        if (!key.handle)
            continue;
        test_unhex(ciphertexts[k], expected);
        for (u32 v = 0; v != 5; ++v) {
            u8 encrypted[80], decrypted[80], tag[17];
            memset(encrypted, 0xa5, sizeof(encrypted));
            memset(decrypted, 0xa5, sizeof(decrypted));
            memset(tag, 0xa5, sizeof(tag));
            test_unhex(tags[k][v], tag_expected);
            const auto enc = aes_gcm_encrypt(key, {nonce, 12}, {aad, aad_sizes[v]}, {plain, sizes[v]},
                                             {encrypted, 80}, {tag, 16});
            check(enc.ok() && enc.bytes == sizes[v] && equal(encrypted, expected, sizes[v]) &&
                      equal(tag, tag_expected, 16) &&
                      test_filled(encrypted + sizes[v], 80 - sizes[v], 0xa5) && tag[16] == 0xa5,
                  "AES matches NIST ciphertext/tag and preserves output tails");
            // Decrypt the independently published ciphertext/tag, not our encryption result.
            const auto dec = aes_gcm_decrypt(key, {nonce, 12}, {aad, aad_sizes[v]}, {expected, sizes[v]},
                                             {tag_expected, 16}, {decrypted, 80});
            check(dec.ok() && dec.bytes == sizes[v] && equal(decrypted, plain, sizes[v]) &&
                      test_filled(decrypted + sizes[v], 80 - sizes[v], 0xa5),
                  "AES decrypts NIST known answer");
            tag_expected[0] ^= 1;
            const auto bad = aes_gcm_decrypt(key, {nonce, 12}, {aad, aad_sizes[v]}, {expected, sizes[v]},
                                             {tag_expected, 16}, {decrypted, 80});
            check(bad.error == AeadError::authentication_failed && bad.bytes == 0 &&
                      test_filled(decrypted, sizes[v], 0) &&
                      test_filled(decrypted + sizes[v], 80 - sizes[v], 0xa5),
                  "AES rejects modified NIST tag including empty payload and clears attempted plaintext");
        }
        aes_gcm_release(&key);
    }
}

void matrix(Check check) noexcept {
    // Independent cryptography/OpenSSL oracle; regenerate with tools/aes_gcm_oracle.py.
    // Hash every ciphertext and tag to keep the fixture compact, with no AES oracle
    // dependency in the native executable. CNG SHA-256 is only the comparison sink.
    constexpr u32 sizes[]{0,   1,   2,    7,    15,   16,   17,   31,    32,    33,
                          63,  64,  65,   127,  128,  129,  255,  256,   257,   511,
                          512, 513, 1200, 1400, 4095, 4096, 4097, 16383, 16384, 16385};
    constexpr u32 aad_sizes[]{0, 1, 12, 15, 16, 17, 31, 32, 33, 64, 65};
    const char* digests[]{"9f7c83cc1f21f923b3c46c48ef40bce8e6a3cbe82777bc381c0edc9c92b31b8e",
                          "a505df1d67f2fa28ea2315cf54e31dfe36f8445afd363bd88e5638439e1a847f"};
    u8 plain[16385], packet[16385 + 17], aad[65], secret[32], nonce[12], digest[32], expected[32];
    for (u32 i = 0; i != sizeof(plain); ++i)
        plain[i] = static_cast<u8>(i * 37 + (i >> 3) + 11);
    for (u32 i = 0; i != sizeof(aad); ++i)
        aad[i] = static_cast<u8>(i * 19 + 5);
    for (u32 i = 4; i != 12; ++i)
        nonce[i] = static_cast<u8>(i - 4);
    for (u32 k = 0; k != 2; ++k) {
        const u32 key_bytes = k ? 32 : 16;
        for (u32 i = 0; i != key_bytes; ++i)
            secret[i] = static_cast<u8>(i * 13 + 7 + key_bytes);
        AesGcmKey key{};
        check(aes_gcm_initialize(&key, {secret, key_bytes}).ok(), "AES matrix key initialization");
        if (!key.handle)
            continue;
        const void* retained_handle = key.handle;
        const void* retained_object = key.object.data;
        BCRYPT_HASH_HANDLE hash = nullptr;
        check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
              "AES oracle comparison hash initialization");
        if (!hash) {
            aes_gcm_release(&key);
            continue;
        }
        u32 counter = 0;
        for (u32 size : sizes) {
            for (u32 aad_size : aad_sizes) {
                ++counter;
                for (u32 i = 0; i != 4; ++i)
                    nonce[i] = static_cast<u8>(counter >> (24 - i * 8));
                copy_bytes(packet, plain, size);
                packet[size + 16] = 0xa5;
                const auto enc = aes_gcm_encrypt(key, {nonce, 12}, {aad, aad_size}, {packet, size},
                                                 {packet, sizeof(packet)}, {packet + size, 16});
                check(enc.ok() && enc.bytes == size && packet[size + 16] == 0xa5,
                      "AES matrix in-place encryption and adjacent tag");
                check(BCryptHashData(hash, packet, size + 16, 0) >= 0, "AES matrix hash update");
                const auto dec = aes_gcm_decrypt(key, {nonce, 12}, {aad, aad_size}, {packet, size},
                                                 {packet + size, 16}, {packet, sizeof(packet)});
                check(dec.ok() && dec.bytes == size && equal(packet, plain, size) &&
                          packet[size + 16] == 0xa5,
                      "AES matrix in-place authenticated round trip");
                check(key.handle == retained_handle && key.object.data == retained_object &&
                          key.object.locked,
                      "AES packets retain the initialized locked key object");
            }
        }
        check(BCryptFinishHash(hash, digest, 32, 0) >= 0, "AES matrix hash finalization");
        test_unhex(digests[k], expected);
        check(equal(digest, expected, 32), "AES ciphertexts and tags match independent OpenSSL matrix");
        BCryptDestroyHash(hash);
        aes_gcm_release(&key);
        check(!key.handle && !key.object.data && !key.object.locked && !key.object.capacity,
              "AES release retires handle and locked storage");
        aes_gcm_release(&key);
    }
}

void boundaries(Check check) noexcept {
    u8 secret[32]{}, nonce[12]{}, aad[20]{}, plain[33]{}, ciphertext[33], tag[16], output[64];
    AesGcmKey key{};
    check(aes_gcm_encrypt(key, {nonce, 12}, {}, {}, {}, {tag, 16}).error == AeadError::not_initialized,
          "AES rejects encryption before initialization");
    constexpr u32 invalid_key_sizes[]{0, 1, 15, 17, 24, 31, 33};
    for (u32 length : invalid_key_sizes)
        check(aes_gcm_initialize(&key, {secret, length}).error == AeadError::invalid_input && !key.handle,
              "AES rejects unsupported key lengths without allocating");
    check(aes_gcm_initialize(&key, {secret, 32}).ok(), "AES boundary key initialization");
    if (!key.handle)
        return;
    const auto* storage = key.object.data;
    check(aes_gcm_initialize(&key, {secret, 32}).error == AeadError::invalid_input &&
              key.object.data == storage,
          "AES cannot overwrite a live owner");
    check(aes_gcm_encrypt(key, {nonce, 12}, {aad, 20}, {plain, 33}, {ciphertext, 33}, {tag, 16}).ok(),
          "AES tamper fixture encryption");
    for (u32 part = 0; part != 4; ++part) {
        u8* fields[]{nonce, aad, ciphertext, tag};
        constexpr u32 lengths[]{12, 20, 33, 16};
        for (u32 i = 0; i != lengths[part]; ++i) {
            fields[part][i] ^= 1;
            memset(output, 0xa5, sizeof(output));
            const auto dec =
                aes_gcm_decrypt(key, {nonce, 12}, {aad, 20}, {ciphertext, 33}, {tag, 16}, {output, 64});
            check(dec.error == AeadError::authentication_failed && dec.bytes == 0 &&
                      test_filled(output, 33, 0) && test_filled(output + 33, 31, 0xa5),
                  "AES rejects tampering at every nonce/AAD/ciphertext/tag byte and erases plaintext");
            fields[part][i] ^= 1;
        }
    }
    memset(output, 0xa5, sizeof(output));
    const auto short_output =
        aes_gcm_decrypt(key, {nonce, 12}, {aad, 20}, {ciphertext, 33}, {tag, 16}, {output, 32});
    check(short_output.error == AeadError::output_too_small && short_output.bytes == 0 &&
              test_filled(output, 64, 0xa5),
          "AES short output is unchanged");
    u8 saved_tag[16];
    copy_bytes(saved_tag, tag, 16);
    check(aes_gcm_encrypt(key, {nonce, 12}, {aad, 20}, {plain, 33}, {output, 32}, {tag, 16}).error ==
                  AeadError::output_too_small &&
              test_filled(output, 64, 0xa5) && equal(tag, saved_tag, 16),
          "AES short encryption output preserves ciphertext and tag");
    const auto truncated =
        aes_gcm_decrypt(key, {nonce, 12}, {aad, 20}, {ciphertext, 32}, {tag, 16}, {output, 64});
    check(truncated.error == AeadError::authentication_failed && truncated.bytes == 0 &&
              test_filled(output, 32, 0) && test_filled(output + 32, 32, 0xa5),
          "AES rejects truncated ciphertext without exposing plaintext");
    memset(output, 0xa5, sizeof(output));
    constexpr u32 invalid_nonce_sizes[]{0, 11, 13};
    for (u32 length : invalid_nonce_sizes)
        check(aes_gcm_encrypt(key, {nonce, length}, {}, {plain, 33}, {output, 64}, {tag, 16}).error ==
                      AeadError::invalid_input &&
                  test_filled(output, 64, 0xa5),
              "AES rejects wrong nonce lengths");
    constexpr u32 invalid_tag_sizes[]{0, 15, 17};
    for (u32 length : invalid_tag_sizes)
        check(aes_gcm_decrypt(key, {nonce, 12}, {}, {ciphertext, 33}, {tag, length}, {output, 64}).error ==
                      AeadError::invalid_input &&
                  test_filled(output, 64, 0xa5),
              "AES rejects wrong tag lengths");
    const ConstSlice<u8> invalid[]{{nullptr, 1}, {reinterpret_cast<const u8*>(~usize{0} - 1), 4}};
    for (const auto span : invalid)
        check(aes_gcm_encrypt(key, {nonce, 12}, span, {plain, 33}, {output, 64}, {tag, 16}).error ==
                      AeadError::invalid_input &&
                  test_filled(output, 64, 0xa5),
              "AES rejects null/wrapping spans");
    check(aes_gcm_encrypt(key, {nonce, 12}, {}, {output, 33}, {output + 1, 33}, {tag, 16}).error ==
                  AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "AES rejects forward partial payload alias");
    check(aes_gcm_encrypt(key, {nonce, 12}, {}, {output + 1, 33}, {output, 33}, {tag, 16}).error ==
                  AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "AES rejects backward partial payload alias");
    check(aes_gcm_encrypt(key, {output, 12}, {}, {plain, 33}, {output, 64}, {tag, 16}).error ==
                  AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "AES rejects nonce/output alias");
    check(
        aes_gcm_decrypt(key, {nonce, 12}, {output, 20}, {ciphertext, 33}, {tag, 16}, {output, 64}).error ==
                AeadError::invalid_input &&
            test_filled(output, 64, 0xa5),
        "AES rejects AAD/output alias");
    check(aes_gcm_encrypt(key, {nonce, 12}, {}, {plain, 33}, {output, 64}, {output + 32, 16}).error ==
                  AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "AES rejects overlapping payload/tag");
    check(aes_gcm_encrypt(key, {nonce, 12}, {}, {plain, 33}, {key.object.data, 33}, {tag, 16}).error ==
              AeadError::invalid_input,
          "AES rejects overwriting retained key storage");
    check(aes_gcm_encrypt(key, {output, 12}, {}, {}, {}, {output, 16}).error == AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "AES rejects encryption tag/nonce alias on empty data");
    check(aes_gcm_encrypt(key, {nonce, 12}, {output, 20}, {}, {}, {output, 16}).error ==
                  AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "AES rejects encryption tag/AAD alias on empty data");
    check(aes_gcm_encrypt(key, {nonce, 12}, {}, {output, 33}, {ciphertext, 33}, {output, 16}).error ==
                  AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "AES rejects encryption tag/source alias");
    check(
        aes_gcm_decrypt(key, {nonce, 12}, {aad, 20}, {ciphertext, 33}, {output, 16}, {output, 64}).error ==
                AeadError::invalid_input &&
            test_filled(output, 64, 0xa5),
        "AES rejects decryption tag/destination alias");
    check(aes_gcm_encrypt(key, {nonce, 12}, {}, {}, {}, {reinterpret_cast<u8*>(&key), 16}).error ==
              AeadError::invalid_input,
          "AES rejects overwriting the key owner with tag");
    copy_bytes(output, ciphertext, 33);
    tag[0] ^= 1;
    const auto inplace_failure =
        aes_gcm_decrypt(key, {nonce, 12}, {aad, 20}, {output, 33}, {tag, 16}, {output, 64});
    check(inplace_failure.error == AeadError::authentication_failed && test_filled(output, 33, 0),
          "AES failed in-place authentication leaves no plaintext");
    aes_gcm_release(&key);
    check(aes_gcm_decrypt(key, {nonce, 12}, {}, {}, {tag, 16}, {}).error == AeadError::not_initialized,
          "AES rejects decryption after release");
    check(aes_gcm_initialize(&key, {secret, 16}).ok(), "AES released owner can initialize a new key");
    aes_gcm_release(&key);
    aes_gcm_release(nullptr);
}
} // namespace

void test_aes_gcm(Check check) noexcept {
    known_answers(check);
    matrix(check);
    boundaries(check);
}
} // namespace maicord
