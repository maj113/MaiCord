#include "maicord/platform/aes_gcm64.hpp"
#include "maicord/platform/aes_gcm.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/crypto.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
void known_answers(Check check, Gcm64Backend backend) noexcept {
    // NIST zero-key/96-bit-IV GCM examples, with the specified MSB64 tag.
    constexpr u8 encrypted[]{0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92,
                             0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78};
    constexpr u8 full_tag[]{0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd};
    constexpr u8 empty_tag[]{0x58, 0xe2, 0xfc, 0xce, 0xfa, 0x7e, 0x30, 0x61};
    u8 secret[16]{}, nonce[12]{}, output[32];
    AesGcm64Key key{};
    check(aes_gcm64_initialize(&key, {secret, 16}, backend).ok(), "GCM64 NIST key initialization");
    if (!key.handle)
        return;
    check(aes_gcm64_decrypt(key, {nonce, 12}, {}, {encrypted, 16}, {full_tag, 8}, {output, 32}).ok() &&
              test_filled(output, 16, 0),
          "GCM64 decrypts published NIST zero-key block");
    const auto empty = aes_gcm64_decrypt(key, {nonce, 12}, {}, {}, {empty_tag, 8}, {});
    check(empty.ok() && !empty.bytes, "GCM64 authenticates null empty payload");
    const auto rejected = aes_gcm64_decrypt(key, {nonce, 12}, {}, {}, {full_tag, 8}, {});
    check(rejected.error == AeadError::authentication_failed && !rejected.bytes,
          "GCM64 does not mistake null output for an unauthenticated size query");
    aes_gcm64_release(&key);
    // Distinct subkeys exercise GHASH beyond one fixed matrix key.
    u8 plain[273]{}, aad[31]{}, cipher[289], tag[8];
    for (u32 k = 0; k != 64; ++k) {
        for (u32 i = 0; i != 16; ++i)
            secret[i] = static_cast<u8>(k * 37 + i * 17 + (k ^ i));
        for (u32 i = 0; i != sizeof(plain); ++i)
            plain[i] = static_cast<u8>(k + i * 11);
        for (u32 i = 0; i != sizeof(aad); ++i)
            aad[i] = static_cast<u8>(k * 3 + i);
        AesGcmKey reference{};
        const bool initialized = aes_gcm64_initialize(&key, {secret, 16}, backend).ok() &&
                                 aes_gcm_initialize(&reference, {secret, 16}).ok();
        check(initialized, "GCM64 varied-key initialization");
        if (initialized) {
            check(aes_gcm_encrypt(reference, {nonce, 12}, {aad, 31}, {plain, 273}, {cipher, 273},
                                  {cipher + 273, 16})
                      .ok(),
                  "GCM64 varied-key native oracle");
            u8 result[273];
            check(aes_gcm64_encrypt(key, {nonce, 12}, {aad, 31}, {plain, 273}, {result, 273}, {tag, 8})
                          .ok() &&
                      bytes_equal(result, cipher, 273) && bytes_equal(tag, cipher + 273, 8),
                  "GCM64 varied-key ciphertext and tag match native oracle");
            check(aes_gcm64_decrypt(key, {nonce, 12}, {aad, 31}, {cipher, 273}, {cipher + 273, 8},
                                    {result, 273})
                          .ok() &&
                      bytes_equal(result, plain, 273),
                  "GCM64 varied-key decryption matches native oracle");
        }
        aes_gcm64_release(&key);
        aes_gcm_release(&reference);
    }
}
void matrix(Check check, Gcm64Backend backend) noexcept {
    constexpr u32 sizes[]{0,    1,    2,    7,    15,   16,   17,   31,   32,   33,   63,    64,    65,
                          127,  128,  129,  255,  256,  257,  511,  512,  513,  1007, 1008,  1009,  1023,
                          1024, 1025, 1200, 1400, 2047, 2048, 2049, 4095, 4096, 4097, 16383, 16384, 16385};
    constexpr u32 aad_sizes[]{0,  1,   12,  15,  16,  17,  31,  32,   33,   64,
                              65, 127, 128, 129, 255, 256, 257, 1023, 1024, 1025};
    constexpr u8 expected_digest[]{0xd3, 0xef, 0x0a, 0x06, 0x02, 0xba, 0xf0, 0xa0, 0xe8, 0x1f, 0x57,
                                   0x8b, 0xe4, 0x0e, 0xab, 0x11, 0x6b, 0x0e, 0xbf, 0x44, 0x9b, 0x8d,
                                   0xcf, 0x89, 0x9a, 0x50, 0x8f, 0x35, 0x39, 0xd9, 0x9a, 0x12};
    u8 secret[16], nonce[12]{}, aad[1025], plain[16385], packet[16402], oracle[16402], digest[32];
    for (u32 i = 0; i != 16; ++i)
        secret[i] = static_cast<u8>(i * 13 + 23);
    for (u32 i = 0; i != 8; ++i)
        nonce[4 + i] = static_cast<u8>(i);
    for (u32 i = 0; i != sizeof(plain); ++i)
        plain[i] = static_cast<u8>(i * 37 + (i >> 3) + 11);
    for (u32 i = 0; i != sizeof(aad); ++i)
        aad[i] = static_cast<u8>(i * 19 + 5);
    AesGcm64Key key{};
    AesGcmKey reference{};
    check(aes_gcm64_initialize(&key, {secret, 16}, backend).ok(), "GCM64 matrix key initialization");
    check(aes_gcm_initialize(&reference, {secret, 16}).ok(), "GCM64 CNG reference key initialization");
    BCRYPT_HASH_HANDLE hash = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
          "GCM64 independent matrix hash initialization");
    if (!key.handle || !reference.handle || !hash) {
        aes_gcm64_release(&key);
        aes_gcm_release(&reference);
        if (hash)
            BCryptDestroyHash(hash);
        return;
    }
    void* retained_handle = key.handle;
    const auto* retained_storage = key.storage.data;
    u32 counter = 0;
    for (u32 size : sizes) {
        for (u32 aad_size : aad_sizes) {
            ++counter;
            for (u32 i = 0; i != 4; ++i)
                nonce[i] = static_cast<u8>(counter >> (24 - i * 8));
            const auto ref = aes_gcm_encrypt(reference, {nonce, 12}, {aad, aad_size}, {plain, size},
                                             {oracle, sizeof(oracle)}, {oracle + size, 16});
            check(ref.ok(), "GCM64 native full-tag reference encryption");
            copy_bytes(packet, plain, size);
            memset(packet + size, 0xa5, sizeof(packet) - size);
            const auto enc = aes_gcm64_encrypt(key, {nonce, 12}, {aad, aad_size}, {packet, size},
                                               {packet, sizeof(packet)}, {packet + size, 8});
            check(enc.ok() && enc.bytes == size && bytes_equal(packet, oracle, size + 8) &&
                      test_filled(packet + size + 8, sizeof(packet) - size - 8, 0xa5),
                  "GCM64 matches native ciphertext and truncated tag with untouched tail");
            check(BCryptHashData(hash, packet, size + 8, 0) >= 0, "GCM64 matrix hash update");
            memset(packet, 0xa5, sizeof(packet));
            const auto disjoint = aes_gcm64_encrypt(key, {nonce, 12}, {aad, aad_size}, {plain, size},
                                                    {packet, sizeof(packet)}, {packet + size, 8});
            check(disjoint.ok() && disjoint.bytes == size && bytes_equal(packet, oracle, size + 8) &&
                      test_filled(packet + size + 8, sizeof(packet) - size - 8, 0xa5),
                  "GCM64 disjoint encryption matches native oracle and preserves tails");
            // Decrypt the independent provider's ciphertext/tag, not our own result.
            const auto dec = aes_gcm64_decrypt(key, {nonce, 12}, {aad, aad_size}, {oracle, size},
                                               {oracle + size, 8}, {packet, sizeof(packet)});
            check(dec.ok() && dec.bytes == size && bytes_equal(packet, plain, size),
                  "GCM64 decrypts independent provider ciphertext");
            copy_bytes(packet, oracle, size + 8);
            const auto in_place = aes_gcm64_decrypt(key, {nonce, 12}, {aad, aad_size}, {packet, size},
                                                    {packet + size, 8}, {packet, sizeof(packet)});
            check(in_place.ok() && in_place.bytes == size && bytes_equal(packet, plain, size) &&
                      bytes_equal(packet + size, oracle + size, 8),
                  "GCM64 supports in-place authenticated decryption with adjacent tag");
            check(key.handle == retained_handle && key.storage.data == retained_storage &&
                      key.storage.locked,
                  "GCM64 operations retain the original locked owner without allocations");
        }
    }
    check(BCryptFinishHash(hash, digest, 32, 0) >= 0 && bytes_equal(digest, expected_digest, 32),
          "GCM64 780-case ciphertext/tag digest matches independent OpenSSL oracle");
    BCryptDestroyHash(hash);
    aes_gcm64_release(&key);
    aes_gcm_release(&reference);
    check(!key.handle && !key.storage.data && !key.storage.locked && !key.storage.capacity,
          "GCM64 release clears owner");
    aes_gcm64_release(&key);
}
void boundaries(Check check, Gcm64Backend backend) noexcept {
    u8 secret[32]{}, nonce[12]{}, aad[33]{}, cipher[65]{}, tag[16]{}, output[160], original[160];
    AesGcm64Key key{};
    check(aes_gcm64_encrypt(key, {nonce, 12}, {}, {}, {}, {tag, 8}).error == AeadError::not_initialized,
          "GCM64 rejects operations without a key");
    constexpr u32 invalid_sizes[]{0, 1, 15, 17, 24, 32};
    for (u32 size : invalid_sizes)
        check(aes_gcm64_initialize(&key, {secret, size}, backend).error == AeadError::invalid_input &&
                  !key.handle,
              "GCM64 accepts only AES128 key material");
    check(aes_gcm64_initialize(&key, {secret, 16}, backend).ok(), "GCM64 boundary key initialization");
    if (!key.handle)
        return;
    const auto handle = key.handle;
    check(aes_gcm64_initialize(&key, {secret, 16}, backend).error == AeadError::invalid_input &&
              key.handle == handle,
          "GCM64 cannot replace a live key owner");
    constexpr u32 sizes[]{0, 1, 15, 16, 17, 65};
    for (u32 size : sizes) {
        ++nonce[11];
        memset(cipher, 0, sizeof(cipher));
        check(aes_gcm64_encrypt(key, {nonce, 12}, {aad, 33}, {cipher, size}, {cipher, 65}, {tag, 8}).ok(),
              "GCM64 tamper fixture encryption");
        u8* fields[]{nonce, aad, cipher, tag};
        const u32 lengths[]{12, 33, size, 8};
        for (u32 part = 0; part != 4; ++part) {
            for (u32 i = 0; i != lengths[part]; ++i) {
                for (u32 bit = 0; bit != 8; ++bit) {
                    fields[part][i] ^= static_cast<u8>(1u << bit);
                    memset(output, 0xa5, sizeof(output));
                    const auto result = aes_gcm64_decrypt(key, {nonce, 12}, {aad, 33}, {cipher, size},
                                                          {tag, 8}, {output, sizeof(output)});
                    check(result.error == AeadError::authentication_failed && !result.bytes &&
                              test_filled(output, size, 0) &&
                              test_filled(output + size, sizeof(output) - size, 0xa5),
                          "GCM64 rejects every nonce/AAD/cipher/tag bit change and wipes attempted output "
                          "only");
                    fields[part][i] ^= static_cast<u8>(1u << bit);
                }
            }
        }
        for (u32 capacity = 0; capacity < size; ++capacity) {
            memset(output, 0xa5, sizeof(output));
            const auto result = aes_gcm64_decrypt(key, {nonce, 12}, {aad, 33}, {cipher, size}, {tag, 8},
                                                  {output, capacity});
            check(result.error == AeadError::output_too_small && test_filled(output, sizeof(output), 0xa5),
                  "GCM64 rejects every insufficient capacity without writes");
        }
    }
    memset(output, 0xa5, sizeof(output));
    copy_bytes(original, output, sizeof(output));
    for (u32 size = 0; size != 17; ++size) {
        if (size == 8)
            continue;
        check(aes_gcm64_decrypt(key, {nonce, 12}, {}, {cipher, 16}, {tag, size}, {output, 160}).error ==
                      AeadError::invalid_input &&
                  bytes_equal(output, original, 160),
              "GCM64 rejects every other tag length without writes");
    }
    check(aes_gcm64_encrypt(key, {nonce, 11}, {}, {cipher, 16}, {output, 160}, {tag, 8}).error ==
                  AeadError::invalid_input &&
              bytes_equal(output, original, 160),
          "GCM64 rejects wrong nonce length");
    check(aes_gcm64_encrypt(key, {nullptr, 12}, {}, {cipher, 16}, {output, 160}, {tag, 8}).error ==
              AeadError::invalid_input,
          "GCM64 rejects null nonce span");
    check(aes_gcm64_encrypt(key, {nonce, 12}, {reinterpret_cast<const u8*>(~usize{0} - 7), 16},
                            {cipher, 16}, {output, 160}, {tag, 8})
                  .error == AeadError::invalid_input,
          "GCM64 rejects wrapping spans before reading input");
    u8 saved_tag[8];
    copy_bytes(saved_tag, tag, 8);
    check(aes_gcm64_encrypt(key, {nonce, 12}, {}, {cipher, 16}, {output, 15}, {tag, 8}).error ==
                  AeadError::output_too_small &&
              bytes_equal(output, original, 160) && bytes_equal(tag, saved_tag, 8),
          "GCM64 short encryption output preserves ciphertext and tag");
    check(aes_gcm64_encrypt(key, {nonce, 12}, {}, {output, 16}, {output + 1, 159}, {tag, 8}).error ==
                  AeadError::invalid_input &&
              bytes_equal(output, original, 160),
          "GCM64 rejects partial in-place alias");
    check(aes_gcm64_encrypt(key, {output, 12}, {}, {cipher, 16}, {output, 160}, {tag, 8}).error ==
              AeadError::invalid_input,
          "GCM64 rejects output/nonce alias");
    check(aes_gcm64_encrypt(key, {nonce, 12}, {output, 33}, {cipher, 16}, {output, 160}, {tag, 8}).error ==
              AeadError::invalid_input,
          "GCM64 rejects output/AAD alias");
    check(aes_gcm64_encrypt(key, {nonce, 12}, {}, {cipher, 16}, {output, 160}, {output, 8}).error ==
              AeadError::invalid_input,
          "GCM64 rejects output/tag alias");
    check(aes_gcm64_encrypt(key, {nonce, 12}, {}, {cipher, 16}, {output, 160}, {cipher, 8}).error ==
              AeadError::invalid_input,
          "GCM64 rejects encryption tag/input alias");
    check(aes_gcm64_encrypt(key, {nonce, 12}, {}, {cipher, 16}, {key.storage.data, 16}, {tag, 8}).error ==
              AeadError::invalid_input,
          "GCM64 rejects writes to locked key storage");
    check(aes_gcm64_encrypt(key, {nonce, 12}, {}, {reinterpret_cast<const u8*>(&key), 16}, {output, 160},
                            {tag, 8})
                  .error == AeadError::invalid_input,
          "GCM64 rejects reading the key owner as payload");
    // Guard-page inputs expose any full-block read of a partial final block.
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto* pages = static_cast<u8*>(
        VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(pages != nullptr, "GCM64 guard allocation");
    if (pages) {
        DWORD previous = 0;
        const bool guarded =
            VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous) != 0;
        check(guarded, "GCM64 guard protection");
        if (guarded) {
            for (u32 size = 0; size != 130; ++size) {
                auto* tail = pages + info.dwPageSize - size;
                ++nonce[11];
                const auto enc = aes_gcm64_encrypt(key, {nonce, 12}, {tail, size}, {tail, size},
                                                   {output, 160}, {tag, 8});
                check(enc.ok() && enc.bytes == size,
                      "GCM64 partial blocks respect inaccessible trailing page");
            }
            // Exact-sized destinations end at the guard page, including the
            // disjoint whole-buffer path's threshold and partial final blocks.
            u8 plain[2049]{}, expected[2065];
            AesGcmKey reference{};
            const bool initialized = aes_gcm_initialize(&reference, {secret, 16}).ok();
            check(initialized, "GCM64 guarded-output reference key");
            if (initialized) {
                constexpr u32 guarded_sizes[]{1023, 1024, 1025, 2047, 2048, 2049};
                for (u32 size : guarded_sizes) {
                    auto* tail = pages + info.dwPageSize - size;
                    ++nonce[11];
                    check(aes_gcm_encrypt(reference, {nonce, 12}, {aad, 33}, {plain, size},
                                          {expected, size}, {expected + size, 16})
                              .ok(),
                          "GCM64 guarded-output native oracle");
                    check(aes_gcm64_encrypt(key, {nonce, 12}, {aad, 33}, {plain, size}, {tail, size},
                                            {tag, 8})
                                  .ok() &&
                              bytes_equal(tail, expected, size) && bytes_equal(tag, expected + size, 8),
                          "GCM64 disjoint encryption writes exactly through the guard boundary");
                    check(
                        aes_gcm64_decrypt(key, {nonce, 12}, {aad, 33}, {tail, size}, {tag, 8}, {tail, size})
                                .ok() &&
                            test_filled(tail, size, 0),
                        "GCM64 in-place decryption respects the guard boundary");
                    check(aes_gcm64_decrypt(key, {nonce, 12}, {aad, 33}, {expected, size}, {tag, 8},
                                            {tail, size})
                                  .ok() &&
                              test_filled(tail, size, 0),
                          "GCM64 disjoint decryption writes exactly through the guard boundary");
                    tag[7] ^= 1;
                    memset(tail, 0xa5, size);
                    const auto rejected = aes_gcm64_decrypt(key, {nonce, 12}, {aad, 33}, {expected, size},
                                                            {tag, 8}, {tail, size});
                    check(rejected.error == AeadError::authentication_failed && !rejected.bytes &&
                              test_filled(tail, size, 0),
                          "GCM64 rejects a bad multi-block tag and wipes exactly to the guard boundary");
                }
            }
            aes_gcm_release(&reference);
        }
        VirtualFree(pages, 0, MEM_RELEASE);
    }
    aes_gcm64_release(&key);
}
} // namespace
void test_aes_gcm64(Check check) noexcept {
    known_answers(check, Gcm64Backend::automatic);
    known_answers(check, Gcm64Backend::portable);
    matrix(check, Gcm64Backend::automatic);
    matrix(check, Gcm64Backend::portable);
    boundaries(check, Gcm64Backend::automatic);
    boundaries(check, Gcm64Backend::portable);
}
} // namespace maicord
