#include "maicord/app/mls_signature.hpp"
#include "maicord/protocol/mls_encoding.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/crypto.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/mls_signature_vectors.hpp"
bool filled(const u8* p, u32 n, u8 v = 0xa5) noexcept {
    for (u32 i = 0; i != n; ++i)
        if (p[i] != v)
            return false;
    return true;
}
void* aggregate(Check check) noexcept {
    void* hash = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
          "MLS signature aggregate init");
    return hash;
}
void finish(Check check, void* hash, const char* hex) noexcept {
    u8 expected[32], actual[32];
    test_unhex(hex, expected);
    check(BCryptFinishHash(hash, actual, 32, 0) >= 0 && bytes_equal(expected, actual, 32),
          "MLS independent aggregate matches");
    BCryptDestroyHash(hash);
}
void codec(Check check) noexcept {
    void* hash = aggregate(check);
    if (!hash)
        return;
    constexpr u8 values[]{1, 127, 128, 255};
    for (u32 pos = 0; pos != 32; ++pos) {
        for (u32 j = 0; j != 4; ++j) {
            u8 raw[64]{}, der[80], decoded[80];
            raw[pos] = values[j];
            raw[32 + (pos * 7 + j) % 32] = static_cast<u8>((values[j] ^ 0x55) | 1);
            memset(der, 0xa5, sizeof(der));
            memset(decoded, 0xa5, sizeof(decoded));
            auto encoded = p256_signature_to_der({raw, 64}, {der, 80});
            check(encoded.ok() && encoded.bytes >= 8 && encoded.bytes <= 72 &&
                      filled(der + encoded.bytes, 80 - encoded.bytes),
                  "DER significant lengths/sign padding/tails");
            if (!encoded.ok())
                continue;
            check(BCryptHashData(hash, der, encoded.bytes, 0) >= 0, "DER independent corpus feed");
            check(p256_signature_from_der({der, encoded.bytes}, {decoded, 80}).ok() &&
                      bytes_equal(raw, decoded, 64) && filled(decoded + 64, 16),
                  "DER strict round trip/tail");
            for (u32 cap = 0; cap < encoded.bytes; ++cap) {
                memset(decoded, 0xa5, sizeof(decoded));
                check(p256_signature_to_der({raw, 64}, {decoded, cap}).error ==
                              SignatureEncodingError::output_capacity &&
                          filled(decoded, 80),
                      "DER encode all short capacities preserve output");
            }
            for (u32 cut = 0; cut != encoded.bytes; ++cut) {
                memset(decoded, 0xa5, sizeof(decoded));
                check(!p256_signature_from_der({der, cut}, {decoded, 80}).ok() && filled(decoded, 80),
                      "DER all truncations preserve output");
            }
            copy_bytes(decoded, raw, 64);
            const auto inplace = p256_signature_to_der({decoded, 64}, {decoded + 1, 79});
            check(inplace.ok() && bytes_equal(der, decoded + 1, encoded.bytes),
                  "DER overlapping encoding staged");
            check(p256_signature_from_der({decoded + 1, inplace.bytes}, {decoded, 64}).ok() &&
                      bytes_equal(decoded, raw, 64),
                  "DER overlapping decoding staged");
        }
    }
    finish(check, hash, signature_der_matrix);
    const char* malformed[]{"3006020100020101",     "3006020180020101",   "300702020001020101",
                            "308106020101020101",   "300602010102010100", "3006020101020100",
                            "3006020101020180",     "3006020001020101",   "3006020101040101",
                            "30080201010201010200", "3006020101020201",   "3006020101028001",
                            "300702018102020001"};
    for (auto hex : malformed) {
        u8 der[80], output[80];
        u32 n = test_unhex(hex, der);
        memset(output, 0xa5, sizeof(output));
        check(p256_signature_from_der({der, n}, {output, 80}).error == SignatureEncodingError::malformed &&
                  filled(output, 80),
              "DER rejects zero/negative/nonminimal/indefinite/trailing/tag/length");
    }
    u8 raw[64]{}, output[80];
    memset(output, 0xa5, sizeof(output));
    check(!p256_signature_to_der({raw, 64}, {output, 80}).ok() && filled(output, 80),
          "DER rejects raw zero");
    raw[31] = 1;
    raw[63] = 1;
    const u8 valid[]{0x30, 6, 2, 1, 1, 2, 1, 1};
    for (u32 cap = 0; cap != 64; ++cap)
        check(p256_signature_from_der({valid, 8}, {output, cap}).error ==
                      SignatureEncodingError::output_capacity &&
                  filled(output, 80),
              "DER decode all short capacities preserve output");
    // Guard every possible bounded DER input length, including a fully populated
    // maximum signature and its truncations; no read may cross the last byte.
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const u32 page = info.dwPageSize;
    auto* allocation =
        static_cast<u8*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(allocation != nullptr, "DER input guard allocation");
    if (!allocation)
        return;
    DWORD old = 0;
    check(VirtualProtect(allocation + page, page, PAGE_NOACCESS, &old) != 0, "DER input guard protection");
    memset(raw, 0xff, 64);
    u8 max_der[72];
    check(p256_signature_to_der({raw, 64}, {max_der, 72}).bytes == 72, "DER 72-byte maximum");
    for (u32 n = 0; n <= 72; ++n) {
        copy_bytes(allocation + page - n, max_der, n);
        check(VirtualProtect(allocation, page, PAGE_READONLY, &old) != 0, "DER immutable guard input");
        memset(output, 0xa5, 80);
        const auto r = p256_signature_from_der({allocation + page - n, n}, {output, 80});
        check(n == 72 ? r.ok() && bytes_equal(output, raw, 64) : !r.ok() && filled(output, 80),
              "DER exact input guard boundary");
        check(VirtualProtect(allocation, page, PAGE_READWRITE, &old) != 0, "DER restore input page");
    }
    // Every single-byte mutation of the maximum encoding is either rejected
    // atomically or re-encodes identically, including integer/sequence headers.
    auto* mutated = allocation + page - 72;
    copy_bytes(mutated, max_der, 72);
    for (u32 i = 0; i != 72; ++i) {
        const u8 saved = mutated[i];
        for (u32 v = 0; v != 256; ++v) {
            mutated[i] = static_cast<u8>(v);
            memset(output, 0xa5, 80);
            const auto decoded = p256_signature_from_der({mutated, 72}, {output, 80});
            if (!decoded.ok())
                check(filled(output, 80), "DER mutation failure is atomic");
            else {
                u8 canonical[72];
                const auto encoded = p256_signature_to_der({output, 64}, {canonical, 72});
                check(encoded.ok() && encoded.bytes == 72 && bytes_equal(canonical, mutated, 72),
                      "DER accepted mutations are canonical");
            }
        }
        mutated[i] = saved;
    }
    check(p256_signature_to_der({raw, 64}, {allocation + page - 72, 72}).ok(), "DER exact output guard");
    check(p256_signature_from_der({max_der, 72}, {allocation + page - 64, 64}).ok(),
          "P1363 exact output guard");
    VirtualFree(allocation, 0, MEM_RELEASE);
}
void record(Check check, HANDLE file, const P256Key& key, ConstSlice<u8> label, ConstSlice<u8> content,
            ConstSlice<u8> signature) noexcept {
    if (file == INVALID_HANDLE_VALUE)
        return;
    u8 header[77];
    check(p256_export_public(key, {header, 65}).ok(), "MLS signature QA public point");
    u32 lengths[]{label.count, content.count, signature.count};
    for (u32 i = 0; i != 3; ++i)
        for (u32 j = 0; j != 4; ++j)
            header[65 + i * 4 + j] = static_cast<u8>(lengths[i] >> (j * 8));
    ConstSlice<u8> parts[]{{header, 77}, label, content, signature};
    for (auto part : parts) {
        DWORD written = 0;
        check(WriteFile(file, part.data, part.count, &written, nullptr) != 0 && written == part.count,
              "MLS signature public QA record");
    }
}
void framed(Check check, KdfSha256Workspace& workspace, u8* label, u8* content, HANDLE file) noexcept {
    u8 scalar[32], point[65], output[96], der[72];
    test_unhex(signature_scalar, scalar);
    test_unhex(signature_point, point);
    P256Key key{}, pub{};
    check(p256_import_private(&key, P256Kind::ecdsa, {scalar, 32}).ok() &&
              p256_import_public(&pub, P256Kind::ecdsa, {point, 65}).ok(),
          "MLS signature keys");
    if (!key.handle || !pub.handle) {
        p256_release(&key);
        p256_release(&pub);
        return;
    }
    for (const auto& v : mls_sign_vectors) {
        const u32 nl = test_unhex(v.label, label), nc = test_unhex(v.content, content), ns = test_unhex(v.signature, der);
        const u32 splits[]{0, nc / 2, nc};
        for (auto split : splits)
            check(mls_verify_with_label(workspace, pub, {label, nl}, {content, split},
                                        {content + split, nc - split}, {der, ns})
                      .ok(),
                  "MLS verifies published/independent signature across content splits");
        memset(output, 0xa5, sizeof(output));
        const auto signed_value = mls_sign_with_label(workspace, key, {label, nl}, {content, nc / 2},
                                                      {content + nc / 2, nc - nc / 2}, {output, 96});
        check(signed_value.ok() && filled(output + signed_value.bytes, 96 - signed_value.bytes),
              "MLS signs DER and preserves tail");
        if (signed_value.ok()) {
            check(mls_verify_with_label(workspace, pub, {label, nl}, {content, nc}, {},
                                        {output, signed_value.bytes})
                      .ok(),
                  "MLS native signed value verifies");
            record(check, file, key, {label, nl}, {content, nc}, {output, signed_value.bytes});
        }
        for (u32 i = 0; i < ns; ++i) {
            der[i] ^= 1;
            check(!mls_verify_with_label(workspace, pub, {label, nl}, {content, nc}, {}, {der, ns}).ok(),
                  "MLS rejects each corrupted DER signature byte");
            der[i] ^= 1;
        }
        u8 wrong = 0x5a;
        check(!mls_verify_with_label(workspace, pub, {&wrong, 1}, {content, nc}, {}, {der, ns}).ok(),
              "MLS label is authenticated");
        check(
            !mls_verify_with_label(workspace, pub, {label, nl}, {content, nc}, {&wrong, 1}, {der, ns}).ok(),
            "MLS complete content is authenticated");
    }
    // Full label/vector and SHA padding boundary corpus, independently hashed.
    for (u32 i = 0; i != 16384; ++i) {
        label[i] = static_cast<u8>(i * 7 + 9);
        content[i] = static_cast<u8>(i * 13 + 5);
    }
    void* hash = aggregate(check);
    if (hash) {
        for (u32 sign = 0; sign != 2; ++sign)
            for (auto nl : signature_label_lengths)
                for (auto nc : signature_content_lengths) {
                    memset(output, 0xa5, 96);
                    const auto r =
                        sign ? mls_signature_hash(workspace, {label, nl}, {content, nc / 2},
                                                  {content + nc / 2, nc - nc / 2}, {output, 96})
                             : mls_reference_hash(workspace, {label, nl}, {content, nc}, {output, 96});
                    check(r.ok() && filled(output + 32, 64), "MLS framed hash boundary/tail");
                    check(BCryptHashData(hash, output, 32, 0) >= 0, "MLS hash matrix feed");
                }
        finish(check, hash, signature_hash_matrix);
    }
    u8 expected[32];
    const u32 nl = test_unhex(ref_label, label), nc = test_unhex(ref_value, content);
    test_unhex(ref_expected, expected);
    check(mls_reference_hash(workspace, {label, nl}, {content, nc}, {output, 96}).ok() &&
              bytes_equal(expected, output, 32),
          "Published MLSWG RefHash");
    // All external preflight failures must preserve outputs and key/workspace.
    memset(output, 0xa5, 96);
    for (u32 cap = 0; cap != 72; ++cap)
        check(mls_sign_with_label(workspace, key, {label, nl}, {content, nc}, {}, {output, cap}).error ==
                      MlsSignatureError::output_capacity &&
                  filled(output, 96),
              "MLS signing rejects capacity before crypto");
    const ConstSlice<u8> huge{label, 0x3fffffff};
    check(!mls_signature_hash(workspace, huge, {}, {}, {output, 96}).ok() && filled(output, 96),
          "MLS label overflow before read");
    check(!mls_signature_hash(workspace, {}, huge, {content, 1}, {output, 96}).ok() && filled(output, 96),
          "MLS content sum overflow before read");
    check(!mls_sign_with_label(workspace, key, {workspace.storage.data, 1}, {}, {}, {output, 96}).ok() &&
              filled(output, 96),
          "MLS input cannot alias erased hash workspace");
    check(!mls_sign_with_label(workspace, key, {reinterpret_cast<const u8*>(&key), 1}, {}, {}, {output, 96})
                  .ok() &&
              filled(output, 96),
          "MLS input cannot alias key owner");
    check(!mls_sign_with_label(workspace, key, {}, {}, {}, {reinterpret_cast<u8*>(&key), 72}).ok() &&
              key.handle,
          "MLS output cannot corrupt key owner");
    check(!mls_sign_with_label(workspace, key, {}, {}, {}, {workspace.storage.data, 72}).ok(),
          "MLS output cannot alias hash workspace");
    check(!mls_sign_with_label(workspace, pub, {label, nl}, {}, {}, {output, 96}).ok() &&
              filled(output, 96),
          "MLS public key cannot sign");
    check(!mls_signature_hash(workspace, {output, 1}, {}, {}, {output, 96}).ok() && filled(output, 96),
          "MLS hash output cannot alias input");
    const u32 capacity = workspace.storage.capacity;
    workspace.storage.capacity = 1;
    check(mls_signature_hash(workspace, {label, nl}, {content, nc}, {}, {output, 96}).error ==
                  KdfError::platform_failure &&
              filled(output, 96),
          "MLS hash provider failure preserves output");
    check(mls_sign_with_label(workspace, key, {label, nl}, {content, nc}, {}, {output, 96}).error ==
                  MlsSignatureError::hash_failure &&
              filled(output, 96),
          "MLS sign provider failure preserves output");
    workspace.storage.capacity = capacity;
    check(mls_sign_with_label(workspace, key, {label, nl}, {content, nc}, {}, {output, 96}).ok(),
          "MLS retry after hash failure");
    check(filled(workspace.storage.data, workspace.storage.capacity, 0), "MLS shared workspace erased");
    P256Key wrong_kind{};
    check(p256_import_private(&wrong_kind, P256Kind::ecdh, {scalar, 32}).ok(), "MLS wrong-purpose fixture");
    check(mls_sign_with_label(workspace, wrong_kind, {}, {}, {}, {output, 96}).error ==
              MlsSignatureError::key_failure,
          "MLS refuses ECDH keys for signing");
    p256_release(&wrong_kind);
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const u32 page = info.dwPageSize;
    auto* guard =
        static_cast<u8*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(guard != nullptr, "MLS framing guard allocation");
    if (guard) {
        DWORD old = 0;
        auto* input = guard + page - 64;
        auto* end = guard + page * 3;
        for (u32 i = 0; i != 64; ++i)
            input[i] = static_cast<u8>(i + 1);
        check(VirtualProtect(guard, page, PAGE_READONLY, &old) &&
                  VirtualProtect(guard + page, page, PAGE_NOACCESS, &old) &&
                  VirtualProtect(end, page, PAGE_NOACCESS, &old),
              "MLS immutable inputs and inaccessible output boundaries");
        check(mls_signature_hash(workspace, {input, 32}, {input + 32, 16}, {input + 48, 16}, {end - 32, 32})
                  .ok(),
              "MLS exact digest output boundary");
        const auto signed_guard = mls_sign_with_label(workspace, key, {input, 32}, {input + 32, 16},
                                                      {input + 48, 16}, {end - 72, 72});
        check(signed_guard.ok() && mls_verify_with_label(workspace, pub, {input, 32}, {input + 32, 32}, {},
                                                         {end - 72, signed_guard.bytes})
                                       .ok(),
              "MLS exact signature output boundary and immutable segmented input");
        VirtualFree(guard, 0, MEM_RELEASE);
    }
    p256_release(&key);
    p256_release(&pub);
    SecureZeroMemory(scalar, 32);
}
} // namespace
void test_mls_signature(Check check) noexcept {
    codec(check);
    KdfSha256Workspace workspace{};
    check(kdf_sha256_initialize(&workspace).ok(), "MLS signature workspace init");
    auto* buffers =
        static_cast<u8*>(VirtualAlloc(nullptr, 32768, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(buffers != nullptr, "MLS signature fixture buffers");
    wchar_t path[1024];
    const DWORD n = GetEnvironmentVariableW(L"MAICORD_MLS_SIGNATURE_OUTPUT", path, 1024);
    HANDLE file = INVALID_HANDLE_VALUE;
    if (n && n < 1024) {
        file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(file != INVALID_HANDLE_VALUE, "MLS signature QA file open");
    }
    if (buffers && workspace.storage.data)
        framed(check, workspace, buffers, buffers + 16384, file);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (buffers)
        VirtualFree(buffers, 0, MEM_RELEASE);
    kdf_sha256_release(&workspace);
    u8 length[4]{0xa5, 0xa5, 0xa5, 0xa5};
    check(mls_vector_length(0x40000000, length) == 0 && filled(length, 4),
          "MLS vector length overflow preserves output");
}
} // namespace maicord
