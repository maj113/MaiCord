#include "maicord/app/mls_hpke.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/mls_hpke_vectors.hpp"
bool filled(const void* ptr, u32 n, u8 v = 0xa5) noexcept {
    const auto* p = static_cast<const u8*>(ptr);
    for (u32 i = 0; i != n; ++i)
        if (p[i] != v)
            return false;
    return true;
}
void record(Check check, HANDLE file, ConstSlice<u8> enc, ConstSlice<u8> label, ConstSlice<u8> context,
            ConstSlice<u8> pt, ConstSlice<u8> ct) noexcept {
    if (file == INVALID_HANDLE_VALUE)
        return;
    u8 header[81];
    copy_bytes(header, enc.data, 65);
    const u32 lengths[]{label.count, context.count, pt.count, ct.count};
    for (u32 i = 0; i != 4; ++i)
        for (u32 j = 0; j != 4; ++j)
            header[65 + i * 4 + j] = static_cast<u8>(lengths[i] >> (j * 8));
    const ConstSlice<u8> spans[]{{header, 81}, label, context, pt, ct};
    for (auto span : spans) {
        DWORD written = 0;
        check(WriteFile(file, span.data, span.count, &written, nullptr) && written == span.count,
              "MLS HPKE public test record");
    }
}
void published(Check check, KdfSha256Workspace& workspace, const P256Key& key) noexcept {
    u8 label[64], context[64], enc[65], ct[64], pt[64], out[80];
    const u32 nl = test_unhex(mls_hpke_label, label), nx = test_unhex(mls_hpke_context, context),
              nc = test_unhex(mls_hpke_ct, ct), np = test_unhex(mls_hpke_pt, pt);
    test_unhex(mls_hpke_enc, enc);
    memset(out, 0xa5, 80);
    check(mls_decrypt_with_label(workspace, key, {label, nl}, {context, nx}, {enc, 65}, {ct, nc}, {out, 80})
                  .ok() &&
              bytes_equal(pt, out, np) && filled(out + np, 80 - np),
          "MLSWG published EncryptWithLabel ciphertext decrypts natively");
    for (u32 i = 0; i != nc; ++i) {
        ct[i] ^= 1;
        memset(out, 0xa5, 80);
        check(mls_decrypt_with_label(workspace, key, {label, nl}, {context, nx}, {enc, 65}, {ct, nc},
                                     {out, 80})
                          .error == HpkeError::authentication_failed &&
                  filled(out, np, 0) && filled(out + np, 80 - np),
              "MLS HPKE rejects every corrupted ciphertext/tag byte and erases only payload");
        ct[i] ^= 1;
    }
    label[0] ^= 1;
    check(mls_decrypt_with_label(workspace, key, {label, nl}, {context, nx}, {enc, 65}, {ct, nc}, {out, 80})
                  .error == HpkeError::authentication_failed,
          "MLS HPKE label is cryptographically bound");
    label[0] ^= 1;
    context[0] ^= 1;
    check(mls_decrypt_with_label(workspace, key, {label, nl}, {context, nx}, {enc, 65}, {ct, nc}, {out, 80})
                  .error == HpkeError::authentication_failed,
          "MLS HPKE context is cryptographically bound");
    context[0] ^= 1;
    copy_bytes(out, ct, nc);
    check(
        mls_decrypt_with_label(workspace, key, {label, nl}, {context, nx}, {enc, 65}, {out, nc}, {out, 80})
                .ok() &&
            bytes_equal(out, pt, np),
        "MLS HPKE exact in-place decryption");
}
void boundaries(Check check, KdfSha256Workspace& workspace, const P256Key& key, HANDLE file, u8* label,
                u8* context) noexcept {
    u8 point[65], enc[80], ct[1056], pt[1024], out[1056];
    test_unhex(mls_hpke_point, point);
    for (u32 i = 0; i != 16384; ++i) {
        label[i] = static_cast<u8>(i * 7 + 9);
        context[i] = static_cast<u8>(i * 13 + 5);
    }
    for (u32 i = 0; i != 1024; ++i)
        pt[i] = static_cast<u8>(i * 17 + 3);
    for (const auto& v : mls_hpke_vectors) {
        test_unhex(v.enc, enc);
        const u32 nc = test_unhex(v.cipher, ct);
        memset(out, 0xa5, sizeof(out));
        check(mls_decrypt_with_label(workspace, key, {label, v.label}, {context, v.context}, {enc, 65},
                                     {ct, nc}, {out, sizeof(out)})
                      .ok() &&
                  bytes_equal(out, pt, v.plain) && filled(out + v.plain, sizeof(out) - v.plain),
              "MLS independent ciphertext boundary vector");
        memset(enc, 0xa5, 80);
        memset(ct, 0xa5, sizeof(ct));
        copy_bytes(ct, pt, v.plain);
        const auto result =
            mls_encrypt_with_label(workspace, {point, 65}, {label, v.label}, {context, v.context},
                                   {ct, v.plain}, {enc, 80}, {ct, sizeof(ct)});
        check(result.ok() && result.bytes == v.plain + 16 && filled(enc + 65, 15) &&
                  filled(ct + result.bytes, sizeof(ct) - result.bytes),
              "MLS native in-place encryption boundary/tails");
        if (result.ok()) {
            record(check, file, {enc, 65}, {label, v.label}, {context, v.context}, {pt, v.plain},
                   {ct, result.bytes});
            check(mls_decrypt_with_label(workspace, key, {label, v.label}, {context, v.context}, {enc, 65},
                                         {ct, result.bytes}, {out, sizeof(out)})
                          .ok() &&
                      bytes_equal(out, pt, v.plain),
                  "MLS native labeled encrypt/decrypt roundtrip");
        }
    }
    memset(enc, 0xa5, 80);
    memset(out, 0xa5, sizeof(out));
    for (u32 cap = 0; cap != 65; ++cap)
        check(
            mls_encrypt_with_label(workspace, {point, 65}, {}, {}, {pt, 33}, {enc, cap}, {out, sizeof(out)})
                        .error == HpkeError::output_capacity &&
                filled(enc, 80) && filled(out, sizeof(out)),
            "MLS short encapsulation capacity preserves both outputs");
    for (u32 cap = 0; cap != 49; ++cap)
        check(
            mls_encrypt_with_label(workspace, {point, 65}, {}, {}, {pt, 33}, {enc, 80}, {out, cap}).error ==
                    HpkeError::output_capacity &&
                filled(enc, 80) && filled(out, sizeof(out)),
            "MLS short ciphertext capacity preserves both outputs");
    check(!mls_encrypt_with_label(workspace, {point, 65}, {label, 0x3fffffff}, {}, {}, {enc, 80},
                                  {out, sizeof(out)})
                  .ok() &&
              filled(enc, 80) && filled(out, sizeof(out)),
          "MLS label vector overflow before input access");
    check(!mls_encrypt_with_label(workspace, {point, 65}, {}, {context, 0x40000000}, {}, {enc, 80},
                                  {out, sizeof(out)})
               .ok(),
          "MLS context vector overflow before input access");
    check(!mls_encrypt_with_label(workspace, {point, 65}, {}, {}, {pt, ~u32{0}}, {enc, 80},
                                  {out, sizeof(out)})
               .ok(),
          "MLS ciphertext length overflow before input access");
    check(!mls_encrypt_with_label(workspace, {point, 65}, {}, {}, {pt, 33}, {out, 65},
                                  {out + 1, sizeof(out) - 1})
                  .ok() &&
              filled(out, sizeof(out)),
          "MLS coupled output overlap rejected");
    check(!mls_encrypt_with_label(workspace, {point, 65}, {out, 1}, {}, {pt, 33}, {enc, 80},
                                  {out, sizeof(out)})
                  .ok() &&
              filled(out, sizeof(out)),
          "MLS label/output alias rejected");
    check(!mls_encrypt_with_label(workspace, {point, 65}, {workspace.storage.data, 1}, {}, {pt, 33},
                                  {enc, 80}, {out, sizeof(out)})
                  .ok() &&
              filled(enc, 80),
          "MLS protected KDF input alias rejected");
    const u32 capacity = workspace.storage.capacity;
    workspace.storage.capacity = 1;
    check(mls_encrypt_with_label(workspace, {point, 65}, {}, {}, {pt, 33}, {enc, 80}, {out, sizeof(out)})
                      .error == HpkeError::derivation_failure &&
              filled(enc, 80) && filled(out, sizeof(out)),
          "MLS failed setup preserves both outputs");
    workspace.storage.capacity = capacity;
    check(mls_encrypt_with_label(workspace, {point, 65}, {}, {}, {pt, 33}, {enc, 80}, {out, sizeof(out)})
              .ok(),
          "MLS setup retry after provider failure");
    // Validate descriptor arrays before following pointers, including empty and
    // arbitrarily fragmented input. Existing two-part HPKE calls remain covered.
    HpkeContext hpke{};
    const ConstSlice<u8> bad[]{{nullptr, 1}};
    check(hpke_setup_sender_parts(&hpke, workspace, {point, 65}, {bad, 1}, {enc, 80}).error ==
                  HpkeError::invalid_input &&
              !hpke.cipher.handle,
          "HPKE invalid segment rejected before setup");
    check(hpke_setup_sender_parts(&hpke, workspace, {point, 65}, {nullptr, 1}, {enc, 80}).error ==
              HpkeError::invalid_input,
          "HPKE missing descriptors rejected");
    check(hpke_setup_recipient_parts(&hpke, workspace, key, {point, 65},
                                     {reinterpret_cast<const ConstSlice<u8>*>(&key), 2})
                      .error == HpkeError::invalid_input &&
              !hpke.cipher.handle,
          "HPKE recipient owner alias rejected before descriptor reads");
    check(hpke_setup_sender_parts(&hpke, workspace, {point, 65}, {bad, ~u32{0}}, {enc, 80}).error ==
              HpkeError::invalid_input,
          "HPKE descriptor byte count overflow rejected");
    check(hpke_setup_sender_parts(&hpke, workspace, {point, 65},
                                  {reinterpret_cast<const ConstSlice<u8>*>(workspace.storage.data), 1},
                                  {enc, 80})
                  .error == HpkeError::invalid_input,
          "HPKE descriptors cannot alias mutable hash workspace");
    const ConstSlice<u8> parts[]{{label, 1}, {context, 1}};
    memset(out, 0xa5, sizeof(out));
    check(hmac_sha256_prefixed(workspace, {}, {out, 1}, {parts, 2}, {out, 32}).error ==
                  KdfError::invalid_input &&
              filled(out, sizeof(out)),
          "Prefixed HMAC output cannot alias prefix");
    check(hmac_sha256_prefixed(workspace, {}, {nullptr, 1}, {parts, 2}, {out, 32}).error ==
                  KdfError::invalid_input &&
              filled(out, sizeof(out)),
          "Prefixed HMAC invalid prefix is atomic");
}
void guards(Check check, KdfSha256Workspace& workspace, const P256Key& key) noexcept {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u32 page = system.dwPageSize;
    auto* mem = static_cast<u8*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(mem != nullptr, "MLS HPKE guard allocation");
    if (!mem)
        return;
    u8 point[65], enc[65], pt[33], ct[49];
    test_unhex(mls_hpke_point, point);
    auto* input = mem + page - 33;
    auto* end = mem + page * 3;
    for (u32 i = 0; i != 33; ++i)
        input[i] = static_cast<u8>(i + 1);
    copy_bytes(pt, input, 33);
    DWORD old = 0;
    check(VirtualProtect(mem, page, PAGE_READONLY, &old) &&
              VirtualProtect(mem + page, page, PAGE_NOACCESS, &old) &&
              VirtualProtect(end, page, PAGE_NOACCESS, &old),
          "MLS HPKE immutable and inaccessible pages");
    check(mls_encrypt_with_label(workspace, {point, 65}, {input, 1}, {input + 1, 32}, {input, 33},
                                 {end - 114, 65}, {end - 49, 49})
              .ok(),
          "MLS exact coupled output boundary and immutable input");
    copy_bytes(enc, end - 114, 65);
    copy_bytes(ct, end - 49, 49);
    check(mls_decrypt_with_label(workspace, key, {input, 1}, {input + 1, 32}, {enc, 65}, {ct, 49},
                                 {end - 33, 33})
                  .ok() &&
              bytes_equal(end - 33, pt, 33),
          "MLS exact plaintext output boundary");
    // Place descriptors immediately before a guard; validate and stream every
    // empty/nonempty element without requiring concatenation storage.
    check(VirtualProtect(mem, page, PAGE_READWRITE, &old) != 0, "MLS descriptor guard write");
    auto* descriptors = reinterpret_cast<ConstSlice<u8>*>(mem + page - sizeof(ConstSlice<u8>) * 3);
    descriptors[0] = {pt, 1};
    descriptors[1] = {};
    descriptors[2] = {pt + 1, 32};
    check(VirtualProtect(mem, page, PAGE_READONLY, &old) != 0, "MLS descriptor immutable page");
    HpkeContext sender{}, receiver{};
    check(hpke_setup_sender_parts(&sender, workspace, {point, 65}, {descriptors, 3}, {enc, 65}).ok() &&
              hpke_setup_recipient_parts(&receiver, workspace, key, {enc, 65}, {descriptors, 3}).ok(),
          "HPKE descriptor guard boundary");
    hpke_release(&sender);
    hpke_release(&receiver);
    VirtualFree(mem, 0, MEM_RELEASE);
}
} // namespace
void test_mls_hpke(Check check) noexcept {
    KdfSha256Workspace workspace{};
    P256Key key{};
    u8 scalar[32];
    test_unhex(mls_hpke_scalar, scalar);
    check(kdf_sha256_initialize(&workspace).ok() &&
              p256_import_private(&key, P256Kind::ecdh, {scalar, 32}).ok(),
          "MLS HPKE setup fixtures");
    wchar_t path[1024];
    const DWORD n = GetEnvironmentVariableW(L"MAICORD_MLS_HPKE_OUTPUT", path, 1024);
    HANDLE file = INVALID_HANDLE_VALUE;
    if (n && n < 1024) {
        file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(file != INVALID_HANDLE_VALUE, "MLS HPKE QA output");
    }
    auto* buffers =
        static_cast<u8*>(VirtualAlloc(nullptr, 32768, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(buffers != nullptr, "MLS HPKE boundary buffers");
    if (workspace.storage.data && key.handle) {
        published(check, workspace, key);
        if (buffers)
            boundaries(check, workspace, key, file, buffers, buffers + 16384);
        guards(check, workspace, key);
    }
    if (buffers)
        VirtualFree(buffers, 0, MEM_RELEASE);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    p256_release(&key);
    kdf_sha256_release(&workspace);
    SecureZeroMemory(scalar, 32);
}
} // namespace maicord
