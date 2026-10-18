#include "maicord/platform/xchacha20_poly1305.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/crypto.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool workspace_erased(const XChaChaKey& key) noexcept {
    return key.storage.data && key.storage.locked && test_filled(key.storage.data, key.object_bytes, 0) &&
           test_filled(key.storage.data + key.object_bytes + 32, 32, 0);
}

void draft_vector(Check check, XChaChaBackend backend) noexcept {
    // draft-irtf-cfrg-xchacha-03 A.3.1, independently published ciphertext/tag.
    u8 secret[32], nonce[24], aad[12], plaintext[114], expected[114], tag[16], packet[131];
    test_unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", secret);
    test_unhex("404142434445464748494a4b4c4d4e4f5051525354555657", nonce);
    test_unhex("50515253c0c1c2c3c4c5c6c7", aad);
    test_unhex("4c616469657320616e642047656e746c656d656e206f662074686520636c6173"
          "73206f66202739393a204966204920636f756c64206f6666657220796f75206f"
          "6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73"
          "637265656e20776f756c642062652069742e",
          plaintext);
    test_unhex("bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb"
          "731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b452"
          "2f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff9"
          "21f9664c97637da9768812f615c68b13b52e",
          expected);
    test_unhex("c0875924c1c7987947deafd8780acf49", tag);
    XChaChaKey key{};
    check(xchacha_initialize(&key, {secret, 32}, backend).ok(), "XChaCha draft-vector initialization");
    if (!key.storage.data)
        return;
    memset(packet, 0xa5, sizeof(packet));
    const auto enc =
        xchacha_encrypt(key, {nonce, 24}, {aad, 12}, {plaintext, 114}, {packet, 131}, {packet + 114, 16});
    check(enc.ok() && enc.bytes == 114 && bytes_equal(packet, expected, 114) &&
              bytes_equal(packet + 114, tag, 16) && packet[130] == 0xa5 && workspace_erased(key),
          "XChaCha matches draft ciphertext/tag and erases derived-key workspace");
    const auto dec =
        xchacha_decrypt(key, {nonce, 24}, {aad, 12}, {expected, 114}, {tag, 16}, {packet, 131});
    check(dec.ok() && dec.bytes == 114 && bytes_equal(packet, plaintext, 114) && workspace_erased(key),
          "XChaCha decrypts independently published draft ciphertext");
    u8* fields[]{nonce, aad, expected, tag};
    constexpr u32 lengths[]{24, 12, 114, 16};
    for (u32 part = 0; part != 4; ++part) {
        for (u32 i = 0; i != lengths[part]; ++i) {
            fields[part][i] ^= 1;
            memset(packet, 0xa5, sizeof(packet));
            const auto bad =
                xchacha_decrypt(key, {nonce, 24}, {aad, 12}, {expected, 114}, {tag, 16}, {packet, 131});
            check(bad.error == AeadError::authentication_failed && bad.bytes == 0 &&
                      test_filled(packet, 114, 0) && test_filled(packet + 114, 17, 0xa5) &&
                      workspace_erased(key),
                  "XChaCha rejects each altered nonce/AAD/ciphertext/tag byte and erases plaintext/key "
                  "workspace");
            fields[part][i] ^= 1;
        }
    }
    check(bytes_equal(key.storage.data + key.object_bytes, secret, 32),
          "XChaCha retains only its original key");
    xchacha_release(&key);
}

void matrix(Check check, XChaChaBackend backend) noexcept {
    // Every ciphertext and tag is checked against the libsodium oracle digest.
    // Regenerate independently with tools/xchacha_oracle.py, never MaiCord code.
    constexpr u32 sizes[]{0,   1,   2,    7,    15,   16,   17,   31,    32,    33,
                          63,  64,  65,   127,  128,  129,  255,  256,   257,   511,
                          512, 513, 1200, 1400, 4095, 4096, 4097, 16383, 16384, 16385};
    constexpr u32 aad_sizes[]{0, 1, 12, 15, 16, 17, 31, 32, 33, 64, 65};
    const char* digests[]{"d6481325d750ec73d176b256e2cdc5e3a9e852ce321cf963984363804dac79ab",
                          "9e6fecd7e5157f2d0495c7a3515ab6304c3eb522b2eacca67d89be7107e59f94",
                          "2c98569497b7a9f4ebd12d530b2fbb5d15e63659fe4fe40bb02c74c9cc827758",
                          "8a6af968b36be9e3be94ab30620baa9893041b307562c1c2398144e278ac5c11"};
    u8 plain[16385], packet[16385 + 17], aad[65], secret[32], nonce[24], digest[32], expected[32];
    for (u32 i = 0; i != sizeof(plain); ++i)
        plain[i] = static_cast<u8>(i * 37 + (i >> 3) + 11);
    for (u32 i = 0; i != sizeof(aad); ++i)
        aad[i] = static_cast<u8>(i * 19 + 5);
    for (u32 k = 0; k != 4; ++k) {
        for (u32 i = 0; i != 32; ++i)
            secret[i] = static_cast<u8>(k == 0 ? 0 : k == 1 ? 255 : k == 2 ? i : i * 13 + 39);
        XChaChaKey key{};
        check(xchacha_initialize(&key, {secret, 32}, backend).ok(), "XChaCha matrix initialization");
        if (!key.storage.data)
            continue;
        const u8* retained_storage = key.storage.data;
        BCRYPT_HASH_HANDLE hash = nullptr;
        check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
              "XChaCha oracle comparison hash initialization");
        if (!hash) {
            xchacha_release(&key);
            continue;
        }
        u32 counter = 0;
        for (u32 size : sizes) {
            for (u32 aad_size : aad_sizes) {
                ++counter;
                for (u32 i = 0; i != 4; ++i)
                    nonce[i] = static_cast<u8>(counter >> (24 - i * 8));
                for (u32 i = 4; i != 24; ++i)
                    nonce[i] = static_cast<u8>(i * 23 + counter * (i + 1));
                copy_bytes(packet, plain, size);
                packet[size + 16] = 0xa5;
                const auto enc = xchacha_encrypt(key, {nonce, 24}, {aad, aad_size}, {packet, size},
                                                 {packet, sizeof(packet)}, {packet + size, 16});
                check(enc.ok() && enc.bytes == size && packet[size + 16] == 0xa5 && workspace_erased(key),
                      "XChaCha matrix in-place encryption, adjacent tag and workspace erasure");
                check(BCryptHashData(hash, packet, size + 16, 0) >= 0, "XChaCha matrix digest update");
                const auto dec = xchacha_decrypt(key, {nonce, 24}, {aad, aad_size}, {packet, size},
                                                 {packet + size, 16}, {packet, sizeof(packet)});
                check(
                    dec.ok() && dec.bytes == size && bytes_equal(packet, plain, size) &&
                        packet[size + 16] == 0xa5 && workspace_erased(key) &&
                        key.storage.data == retained_storage,
                    "XChaCha matrix in-place authenticated round trip retains only original key/workspace");
            }
        }
        check(BCryptFinishHash(hash, digest, 32, 0) >= 0, "XChaCha matrix digest finish");
        test_unhex(digests[k], expected);
        check(bytes_equal(digest, expected, 32),
              "XChaCha ciphertexts/tags match independent libsodium matrix");
        check(bytes_equal(key.storage.data + key.object_bytes, secret, 32),
              "XChaCha matrix original key unchanged");
        BCryptDestroyHash(hash);
        xchacha_release(&key);
        check(!key.storage.data && !key.storage.locked && !key.storage.capacity && !key.object_bytes,
              "XChaCha releases all retained key storage");
        xchacha_release(&key);
    }
}

void boundaries(Check check, XChaChaBackend backend) noexcept {
    u8 secret[32]{}, nonce[24]{}, plain[33]{}, tag[16], output[64];
    XChaChaKey key{};
    check(xchacha_encrypt(key, {nonce, 24}, {}, {}, {}, {tag, 16}).error == AeadError::not_initialized,
          "XChaCha rejects operation before initialization");
    constexpr u32 invalid_key_sizes[]{0, 16, 24, 31, 33};
    for (u32 size : invalid_key_sizes)
        check(xchacha_initialize(&key, {secret, size}).error == AeadError::invalid_input &&
                  !key.storage.data,
              "XChaCha requires exactly 32 key bytes");
    check(xchacha_initialize(nullptr, {secret, 32}).error == AeadError::invalid_input &&
              xchacha_initialize(&key, {nullptr, 32}).error == AeadError::invalid_input,
          "XChaCha rejects null owner/secret");
    check(xchacha_initialize(&key, {secret, 32}, backend).ok(), "XChaCha boundary initialization");
    if (!key.storage.data)
        return;
    const u8* retained_storage = key.storage.data;
    check(xchacha_initialize(&key, {secret, 32}, backend).error == AeadError::invalid_input &&
              key.storage.data == retained_storage,
          "XChaCha rejects overwriting live key storage");
    memset(output, 0xa5, sizeof(output));
    memset(tag, 0xa5, sizeof(tag));
    const auto short_result = xchacha_encrypt(key, {nonce, 24}, {}, {plain, 33}, {output, 32}, {tag, 16});
    check(short_result.error == AeadError::output_too_small && short_result.bytes == 0 &&
              test_filled(output, 64, 0xa5) && test_filled(tag, 16, 0xa5) && workspace_erased(key),
          "XChaCha short output preserves payload and tag without deriving a key");
    constexpr u32 invalid_nonce_sizes[]{0, 12, 23, 25};
    for (u32 size : invalid_nonce_sizes)
        check(xchacha_encrypt(key, {nonce, size}, {}, {plain, 33}, {output, 64}, {tag, 16}).error ==
                      AeadError::invalid_input &&
                  test_filled(output, 64, 0xa5) && workspace_erased(key),
              "XChaCha rejects wrong nonce sizes before key derivation");
    constexpr u32 invalid_tag_sizes[]{0, 15, 17};
    for (u32 size : invalid_tag_sizes)
        check(xchacha_decrypt(key, {nonce, 24}, {}, {plain, 33}, {tag, size}, {output, 64}).error ==
                      AeadError::invalid_input &&
                  test_filled(output, 64, 0xa5),
              "XChaCha rejects wrong tag sizes");
    check(xchacha_encrypt(key, {nonce, 24}, {nullptr, 1}, {plain, 33}, {output, 64}, {tag, 16}).error ==
                  AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "XChaCha rejects null input spans");
    check(xchacha_encrypt(key, {nonce, 24}, {}, {output, 33}, {output + 1, 33}, {tag, 16}).error ==
                  AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "XChaCha rejects partial payload alias");
    check(xchacha_encrypt(key, {output, 24}, {}, {plain, 33}, {output, 64}, {tag, 16}).error ==
                  AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "XChaCha rejects nonce/output alias");
    check(xchacha_decrypt(key, {nonce, 24}, {output, 12}, {plain, 33}, {tag, 16}, {output, 64}).error ==
                  AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "XChaCha rejects AAD/output alias");
    check(xchacha_encrypt(key, {nonce, 24}, {}, {plain, 33}, {output, 64}, {output + 32, 16}).error ==
                  AeadError::invalid_input &&
              test_filled(output, 64, 0xa5),
          "XChaCha rejects payload/tag alias");
    const auto storage = key.storage.data;
    const u32 offset = key.object_bytes;
    check(
        xchacha_encrypt(key, {storage, 24}, {}, {plain, 33}, {output, 64}, {tag, 16}).error ==
                AeadError::invalid_input &&
            xchacha_encrypt(key, {nonce, 24}, {storage, 12}, {plain, 33}, {output, 64}, {tag, 16}).error ==
                AeadError::invalid_input &&
            xchacha_encrypt(key, {nonce, 24}, {}, {storage + offset, 32}, {output, 64}, {tag, 16}).error ==
                AeadError::invalid_input &&
            xchacha_encrypt(key, {nonce, 24}, {}, {plain, 33}, {storage, 33}, {tag, 16}).error ==
                AeadError::invalid_input &&
            xchacha_encrypt(key, {nonce, 24}, {}, {}, {}, {storage, 16}).error ==
                AeadError::invalid_input &&
            workspace_erased(key) && bytes_equal(storage + offset, secret, 32) &&
            test_filled(output, 64, 0xa5),
        "XChaCha rejects all key-workspace aliases before it imports or erases derived state");
    check(xchacha_encrypt(key, {nonce, 24}, {}, {}, {}, {tag, 16}).ok(),
          "XChaCha authenticates empty data with null payload pointers");
    tag[0] ^= 1;
    const auto bad_empty = xchacha_decrypt(key, {nonce, 24}, {}, {}, {tag, 16}, {});
    check(bad_empty.error == AeadError::authentication_failed && bad_empty.bytes == 0 &&
              workspace_erased(key),
          "XChaCha rejects wrong tag for empty data");
    check(xchacha_encrypt(key, {nonce, 24}, {}, {plain, 33}, {output, 64}, {tag, 16}).ok(),
          "XChaCha failure fixture encryption");
    tag[0] ^= 1;
    const auto bad_inplace = xchacha_decrypt(key, {nonce, 24}, {}, {output, 33}, {tag, 16}, {output, 64});
    check(bad_inplace.error == AeadError::authentication_failed && test_filled(output, 33, 0) &&
              workspace_erased(key),
          "XChaCha failed in-place authentication erases plaintext and key workspace");
    xchacha_release(&key);
    check(xchacha_decrypt(key, {nonce, 24}, {}, {}, {tag, 16}, {}).error == AeadError::not_initialized,
          "XChaCha rejects use after release");
    check(xchacha_initialize(&key, {secret, 32}, backend).ok(), "XChaCha reinitializes released owner");
    xchacha_release(&key);
    xchacha_release(nullptr);
}
} // namespace

void test_xchacha20_poly1305(Check check) noexcept {
    // Exercise the selected native backend where available, and always force
    // the portable implementation. Older Windows 10 can run this same suite
    // without requiring a provider it does not ship.
    constexpr XChaChaBackend backends[]{XChaChaBackend::automatic, XChaChaBackend::portable};
    for (auto backend : backends) {
        draft_vector(check, backend);
        matrix(check, backend);
        boundaries(check, backend);
    }
    XChaChaKey automatic{};
    u8 key[32]{};
    check(xchacha_initialize(&automatic, {key, 32}).ok() && automatic.backend != XChaChaBackend::automatic,
          "XChaCha resolves automatic backend once");
    xchacha_release(&automatic);
}
} // namespace maicord
