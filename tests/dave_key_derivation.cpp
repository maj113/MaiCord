#include "maicord/app/dave_key_derivation.hpp"
#include "maicord/app/dave_frame_crypto.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/crypto.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/dave_key_vectors.hpp"
void* begin_hash(Check check) noexcept {
    void* hash = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
          "KDF independent matrix digest initialization");
    return hash;
}
void feed_hash(Check check, void* hash, u8* bytes, u32 count) noexcept {
    if (count)
        check(BCryptHashData(hash, bytes, count, 0) >= 0, "KDF matrix digest update");
}
void finish_hash(Check check, void* hash, const char* expected_hex) noexcept {
    u8 digest[32], expected[32];
    test_unhex(expected_hex, expected);
    check(BCryptFinishHash(hash, digest, 32, 0) >= 0 && bytes_equal(digest, expected, 32),
          "KDF matrix matches independently generated digest");
    BCryptDestroyHash(hash);
}
void rfc_vectors(Check check, KdfSha256Workspace& workspace) noexcept {
    constexpr const char* prks[]{"077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
                                 "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244",
                                 "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04"};
    constexpr const char* okms[]{
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
        ("b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da"
         "3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87"),
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"};
    for (u32 test = 0; test != 3; ++test) {
        u8 input[80], salt[80], info[80], prk[40], output[96], expected[96];
        const u32 ikm_bytes = test == 1 ? 80u : 22u;
        const u32 salt_bytes = test == 2 ? 0u : test == 1 ? 80u : 13u;
        const u32 info_bytes = test == 2 ? 0u : test == 1 ? 80u : 10u;
        for (u32 i = 0; i != 80; ++i) {
            input[i] = test == 1 ? static_cast<u8>(i) : 0x0b;
            salt[i] = static_cast<u8>(i + (test == 1 ? 0x60 : 0));
            info[i] = static_cast<u8>(i + (test == 1 ? 0xb0 : 0xf0));
        }
        memset(prk, 0xa5, sizeof(prk));
        const auto extracted = hkdf_sha256_extract(workspace, {salt_bytes ? salt : nullptr, salt_bytes},
                                                   {input, ikm_bytes}, {prk, sizeof(prk)});
        test_unhex(prks[test], expected);
        check(extracted.ok() && extracted.bytes == 32 && bytes_equal(prk, expected, 32) &&
                  test_filled(prk + 32, 8, 0xa5),
              "HKDF extract matches RFC 5869 SHA256 vectors including empty salt");
        const u32 length = test_unhex(okms[test], expected);
        for (u32 split = 0; split <= info_bytes; ++split) {
            const ConstSlice<u8> parts[]{{info, split}, {}, {info + split, info_bytes - split}};
            memset(output, 0xa5, sizeof(output));
            const auto expanded = hkdf_sha256_expand(workspace, {prk, 32}, {parts, 3}, {output, length});
            check(expanded.ok() && expanded.bytes == length && bytes_equal(output, expected, length) &&
                      test_filled(output + length, sizeof(output) - length, 0xa5),
                  "HKDF expand matches RFC 5869 with every info split and preserves output tail");
        }
    }
    u8 key[20], output[32], expected[32];
    memset(key, 0x0b, sizeof(key));
    const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>("Hi "), 3},
                                 {reinterpret_cast<const u8*>("There"), 5}};
    test_unhex("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", expected);
    check(hmac_sha256(workspace, {key, 20}, {parts, 2}, {output, 32}).ok() &&
              bytes_equal(output, expected, 32),
          "HMAC matches RFC 4231 SHA256 example");
    test_unhex("b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad", expected);
    check(hmac_sha256(workspace, {}, {}, {output, 32}).ok() && bytes_equal(output, expected, 32),
          "HMAC empty key and message do not become an unkeyed SHA256 hash");
}
void hkdf_matrix(Check check, KdfSha256Workspace& workspace) noexcept {
    constexpr u32 sizes[]{0, 1, 16, 31, 32, 33, 63, 64, 65, 255, 8160};
    constexpr u32 info_sizes[]{0, 1, 31, 32, 33, 63, 64, 65, 255, 1025};
    constexpr u32 key_sizes[]{16, 32, 80};
    u8 key[80], info[1025], output[8161];
    for (u32 i = 0; i != sizeof(info); ++i)
        info[i] = static_cast<u8>(i * 17 + 5);
    void* hash = begin_hash(check);
    if (!hash)
        return;
    const auto* storage = workspace.storage.data;
    for (u32 key_size : key_sizes) {
        for (u32 i = 0; i != key_size; ++i)
            key[i] = static_cast<u8>(i * 13 + key_size);
        for (u32 info_size : info_sizes) {
            const u32 split = info_size / 2;
            const ConstSlice<u8> parts[]{{info, split}, {}, {info + split, info_size - split}};
            for (u32 size : sizes) {
                memset(output, 0xa5, sizeof(output));
                const auto result =
                    hkdf_sha256_expand(workspace, {key, key_size}, {parts, 3}, {output, size});
                check(result.ok() && result.bytes == size &&
                          test_filled(output + size, sizeof(output) - size, 0xa5),
                      "HKDF output/block-counter boundaries preserve excess capacity");
                feed_hash(check, hash, output, size);
                check(workspace.storage.data == storage && workspace.storage.locked &&
                          test_filled(storage, workspace.storage.capacity, 0),
                      "HKDF reuses and erases its locked CNG workspace after every operation");
            }
        }
    }
    finish_hash(check, hash, hkdf_matrix_digest);
}
void mls_matrix(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 secret[32], labels[16377], context[16385], output[80];
    for (u32 i = 0; i != 32; ++i)
        secret[i] = static_cast<u8>(i);
    for (u32 i = 0; i != sizeof(labels); ++i)
        labels[i] = static_cast<u8>(97 + i % 26);
    for (u32 i = 0; i != sizeof(context); ++i)
        context[i] = static_cast<u8>(i * 29 + 7);
    constexpr u32 label_sizes[]{0, 1, 55, 56, 57, 16375, 16376, 16377};
    constexpr u32 context_sizes[]{0, 1, 63, 64, 65, 16383, 16384, 16385};
    constexpr u32 sizes[]{16, 32, 65};
    void* hash = begin_hash(check);
    if (!hash)
        return;
    for (u32 label_size : label_sizes)
        for (u32 context_size : context_sizes)
            for (u32 size : sizes) {
                memset(output, 0xa5, sizeof(output));
                const auto result = mls_expand_label(workspace, {secret, 32}, {labels, label_size},
                                                     {context, context_size}, {output, size});
                check(result.ok() && result.bytes == size &&
                          test_filled(output + size, sizeof(output) - size, 0xa5),
                      "MLS label expansion handles canonical one/two/four-byte vector lengths");
                feed_hash(check, hash, output, size);
            }
    finish_hash(check, hash, mls_label_digest);
}
void ratchet(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 secret[48], key[32];
    memset(secret, 0xa5, sizeof(secret));
    for (u32 i = 0; i != 16; ++i)
        secret[i] = static_cast<u8>(i);
    void* hash = begin_hash(check);
    if (!hash)
        return;
    for (u32 generation = 0; generation != 257; ++generation) {
        memset(key, 0xa5, sizeof(key));
        const auto step =
            dave_ratchet_step(workspace, {secret, generation ? 32u : 16u}, generation, key, secret);
        check(step.ok() && step.bytes == 48 && test_filled(key + 16, 16, 0xa5) &&
                  test_filled(secret + 32, 16, 0xa5),
              "DAVE ratchet atomically replaces its seed and preserves destination tails");
        feed_hash(check, hash, key, 16);
        feed_hash(check, hash, secret, 32);
    }
    finish_hash(check, hash, dave_chain_digest);
    AesGcm64Key frame_key{};
    check(aes_gcm64_initialize(&frame_key, {key, 16}).ok(),
          "DAVE derived generation key imports into frame cipher");
    if (frame_key.storage.data) {
        u8 wire[128], output[64], scratch[64];
        const u32 wire_bytes = test_unhex(dave_derived_wire, wire);
        DaveClearRange ranges[4];
        DaveFrameView view{};
        DaveReplay replay{};
        DaveFrameCounter counter{};
        check(dave_frame_parse({wire, wire_bytes}, {ranges, 4}, &view).ok() &&
                  dave_frame_counter(256, view.nonce, &counter) && counter.generation == 256 &&
                  counter.value == (u64{256} << 24) + 42 && dave_replay_can_accept(replay, counter.value),
              "DAVE derived-key frame parses");
        const auto opened = dave_frame_open(view, frame_key, replay, counter.value, scratch, output);
        bool correct = opened == AeadError::none && view.body_bytes == 64;
        for (u32 i = 0; i != 64 && correct; ++i)
            correct = output[i] == static_cast<u8>(i * 11 + 3);
        check(correct && replay.initialized && test_filled(scratch, 64, 0),
              "DAVE protocol-derived key authenticates independent encrypted frame after generation-byte "
              "wrap");
    }
    aes_gcm64_release(&frame_key);
    for (const auto& vector : dave_step_vectors) {
        for (u32 i = 0; i != 32; ++i)
            secret[i] = static_cast<u8>(i);
        u8 next[32], expected_key[16], expected_next[32];
        test_unhex(vector.key, expected_key);
        test_unhex(vector.next, expected_next);
        check(dave_ratchet_step(workspace, {secret, 32}, vector.generation, key, next).ok() &&
                  bytes_equal(key, expected_key, 16) && bytes_equal(next, expected_next, 32),
              "DAVE derivation preserves the complete big-endian uint32 generation");
    }
}
void boundaries(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 secret[32]{}, message[80]{}, output[80], key[32], next[48];
    const ConstSlice<u8> parts[]{{message, sizeof(message)}};
    memset(output, 0xa5, sizeof(output));
    for (u32 size = 0; size != 32; ++size)
        check(hkdf_sha256_extract(workspace, {}, {secret, 32}, {output, size}).error ==
                      KdfError::output_capacity &&
                  test_filled(output, sizeof(output), 0xa5),
              "HKDF extract rejects every short destination before writes");
    check(hkdf_sha256_expand(workspace, {secret, 32}, {parts, 1}, {output, 8161}).error ==
                  KdfError::invalid_input &&
              test_filled(output, sizeof(output), 0xa5),
          "HKDF rejects a 256th output block before accessing output");
    check(hkdf_sha256_expand(workspace, {}, {}, {}).error == KdfError::invalid_input,
          "HKDF expand requires a PRK");
    check(hkdf_sha256_expand(workspace, {secret, 32}, {}, {}).ok(), "HKDF permits null zero-length output");
    check(hmac_sha256(workspace, {nullptr, 1}, {}, {output, 32}).error == KdfError::invalid_input,
          "HMAC rejects invalid key spans");
    const ConstSlice<u8> invalid_parts[]{{message, 16}, {nullptr, 1}};
    check(hmac_sha256(workspace, {secret, 32}, {invalid_parts, 2}, {output, 32}).error ==
                  KdfError::invalid_input &&
              test_filled(output, sizeof(output), 0xa5),
          "HMAC validates all segments before processing the first");
    check(hmac_sha256(workspace, {secret, 32}, {parts, 1}, {message + 1, 32}).error ==
              KdfError::invalid_input,
          "HMAC rejects partial output/input alias");
    check(hmac_sha256(workspace, {secret, 32}, {}, {secret, 32}).error == KdfError::invalid_input,
          "HMAC rejects key/output alias");
    check(hmac_sha256(workspace, {secret, 32}, {parts, 1}, {workspace.storage.data, 32}).error ==
              KdfError::invalid_input,
          "HMAC protects CNG workspace from output aliases");
    check(hmac_sha256(workspace, {secret, 32},
                      {reinterpret_cast<const ConstSlice<u8>*>(workspace.storage.data), 1}, {output, 32})
                  .error == KdfError::invalid_input,
          "HMAC rejects segment descriptors inside workspace before reading them");
    check(mls_expand_label(workspace, {secret, 32}, {message, 0x3ffffff8}, {}, {output, 16}).error ==
                  KdfError::invalid_input &&
              mls_expand_label(workspace, {secret, 32}, {}, {message, 0x40000000}, {output, 16}).error ==
                  KdfError::invalid_input,
          "MLS rejects vector length overflow without reading label/context");
    memset(key, 0xa5, sizeof(key));
    memset(next, 0xa5, sizeof(next));
    // A real provider error: advertise insufficient CNG object storage. The
    // allocation itself stays valid; restore capacity before releasing it.
    const u32 capacity = workspace.storage.capacity;
    workspace.storage.capacity = 1;
    const auto failure = hmac_sha256(workspace, {secret, 32}, {parts, 1}, {output, 80});
    check(failure.error == KdfError::platform_failure && !failure.bytes && failure.native_status < 0 &&
              test_filled(output, 32, 0) && test_filled(output + 32, 48, 0xa5),
          "HMAC native failure wipes only attempted output and reports native status");
    const auto step = dave_ratchet_step(workspace, {secret, 32}, 0, key, next);
    workspace.storage.capacity = capacity;
    check(step.error == KdfError::platform_failure && !step.bytes && test_filled(key, 32, 0xa5) &&
              test_filled(next, 48, 0xa5) && test_filled(secret, 32, 0),
          "DAVE native derivation failure preserves both outputs and seed");
    check(hmac_sha256(workspace, {secret, 32}, {parts, 1}, {output, 80}).ok(),
          "KDF workspace remains usable after a provider rejects insufficient object storage");
}
void guards(Check check, KdfSha256Workspace& workspace) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const u32 usable = ((8160 + info.dwPageSize - 1) / info.dwPageSize) * info.dwPageSize;
    auto* pages = static_cast<u8*>(
        VirtualAlloc(nullptr, usable + info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(pages != nullptr, "KDF guard allocation");
    if (!pages)
        return;
    DWORD previous = 0;
    const bool protected_tail = VirtualProtect(pages + usable, info.dwPageSize, PAGE_NOACCESS, &previous);
    check(protected_tail, "KDF guard protection");
    if (protected_tail) {
        u8 key[32]{}, expected[8160], label[65], context[65];
        constexpr u32 lengths[]{0, 1, 31, 32, 33, 63, 64, 65};
        for (u32 length : lengths) {
            auto* input = pages + usable - length;
            for (u32 i = 0; i != length; ++i)
                input[i] = label[i] = context[i] = static_cast<u8>(i * 19 + 5);
            check(VirtualProtect(pages, usable, PAGE_READONLY, &previous) != 0,
                  "KDF guarded inputs are immutable");
            u8 output[65];
            check(mls_expand_label(workspace, {key, 32}, {input, length}, {input, length}, {output, 65})
                          .ok() &&
                      mls_expand_label(workspace, {key, 32}, {label, length}, {context, length},
                                       {expected, 65})
                          .ok() &&
                      bytes_equal(output, expected, 65),
                  "MLS segmented label/context never read beyond exact guarded input");
            check(VirtualProtect(pages, usable, PAGE_READWRITE, &previous) != 0,
                  "KDF guarded output protection restore");
        }
        constexpr u32 output_lengths[]{0, 1, 16, 31, 32, 33, 63, 64, 65, 8160};
        const ConstSlice<u8> parts[]{{label, 65}};
        for (u32 length : output_lengths) {
            auto* output = pages + usable - length;
            check(hkdf_sha256_expand(workspace, {key, 32}, {parts, 1}, {expected, length}).ok() &&
                      hkdf_sha256_expand(workspace, {key, 32}, {parts, 1}, {output, length}).ok() &&
                      bytes_equal(output, expected, length),
                  "HKDF final partial block and maximum output respect exact guarded capacity");
        }
    }
    VirtualFree(pages, 0, MEM_RELEASE);
}
} // namespace
void test_dave_key_derivation(Check check) noexcept {
    KdfSha256Workspace workspace{};
    u8 output[32];
    check(hmac_sha256(workspace, {}, {}, {output, 32}).error == KdfError::not_initialized,
          "KDF rejects use before workspace initialization");
    check(kdf_sha256_initialize(&workspace).ok(), "KDF locked workspace initialization");
    if (!workspace.storage.data)
        return;
    check(kdf_sha256_initialize(&workspace).error == KdfError::invalid_input,
          "KDF cannot overwrite a live workspace");
    rfc_vectors(check, workspace);
    hkdf_matrix(check, workspace);
    mls_matrix(check, workspace);
    ratchet(check, workspace);
    boundaries(check, workspace);
    guards(check, workspace);
    kdf_sha256_release(&workspace);
    check(!workspace.storage.data && !workspace.storage.locked && !workspace.storage.capacity,
          "KDF release retires locked workspace");
    kdf_sha256_release(&workspace);
}
} // namespace maicord
