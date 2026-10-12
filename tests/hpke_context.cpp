#include "maicord/app/hpke_context.hpp"
#include "maicord/core/memory.hpp"
#include <bcrypt.h>
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/hpke_derivation_vectors.hpp"
bool filled(const void* ptr, u32 n, u8 value = 0xa5) noexcept {
    const auto* p = static_cast<const u8*>(ptr);
    for (u32 i = 0; i != n; ++i)
        if (p[i] != value)
            return false;
    return true;
}
void record(Check check, HANDLE file, ConstSlice<u8> enc, ConstSlice<u8> seq, ConstSlice<u8> info,
            ConstSlice<u8> aad, ConstSlice<u8> pt, ConstSlice<u8> ct) noexcept {
    if (file == INVALID_HANDLE_VALUE)
        return;
    u8 header[93];
    copy_bytes(header, enc.data, 65);
    copy_bytes(header + 65, seq.data, 12);
    const u32 lengths[]{info.count, aad.count, pt.count, ct.count};
    for (u32 i = 0; i != 4; ++i)
        for (u32 j = 0; j != 4; ++j)
            header[77 + i * 4 + j] = static_cast<u8>(lengths[i] >> (j * 8));
    const ConstSlice<u8> spans[]{{header, 93}, info, aad, pt, ct};
    for (auto span : spans) {
        DWORD written = 0;
        check(WriteFile(file, span.data, span.count, &written, nullptr) && written == span.count,
              "HPKE native sender public QA record");
    }
}
void published(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 scalar[32], enc[65], info[128];
    test_unhex(hpke_kems[0].recipient, scalar);
    test_unhex(hpke_kems[0].enc, enc);
    const u32 ni = test_unhex(hpke_base_info, info);
    P256Key recipient{};
    HpkeContext context{};
    check(p256_import_private(&recipient, P256Kind::ecdh, {scalar, 32}).ok(), "HPKE RFC recipient key");
    check(hpke_setup_recipient(&context, workspace, recipient, {enc, 65}, {info, ni / 2},
                               {info + ni / 2, ni - ni / 2})
              .ok(),
          "HPKE RFC recipient setup");
    const auto auxiliary = aes_gcm_auxiliary(context.cipher);
    u8 material[60];
    test_unhex(hpke_base_material, material);
    check(auxiliary.count == 32 && context.cipher.object.locked &&
              auxiliary.data == context.cipher.object.data + context.cipher.object.bytes &&
              bytes_equal(auxiliary.data, material + 28, 32) &&
              bytes_equal(context.base_nonce, material + 16, 12),
          "HPKE exporter shares locked provider allocation");
    u32 seq = 0;
    for (const auto& v : hpke_encryptions) {
        u8 aad[128], ct[144], pt[128], output[160];
        const u32 na = test_unhex(v.aad, aad), nc = test_unhex(v.ct, ct), np = test_unhex(v.pt, pt);
        memset(output, 0xa5, 160);
        check(hpke_seal(context, {}, {}, {output, 160}).error == HpkeError::wrong_role &&
                  filled(output, 160),
              "HPKE recipient cannot encrypt");
        ct[nc - 1] ^= 1;
        check(hpke_open(context, {aad, na}, {ct, nc}, {output, 160}).error ==
                      HpkeError::authentication_failed &&
                  filled(output, np, 0) && !context.poisoned,
              "HPKE bad tag does not advance/poison recipient");
        ct[nc - 1] ^= 1;
        check(hpke_open(context, {aad, na}, {ct, nc}, {output, 160}).ok() && bytes_equal(output, pt, np) &&
                  filled(output + np, 160 - np),
              "HPKE context opens all RFC sequential ciphertexts after tamper retry");
        ++seq;
        check(filled(context.sequence, 10, 0) && context.sequence[10] == static_cast<u8>(seq >> 8) &&
                  context.sequence[11] == static_cast<u8>(seq),
              "HPKE receive sequence carry at 255/256");
        check(hpke_open(context, {aad, na}, {ct, nc}, {output, 160}).error ==
                  HpkeError::authentication_failed,
              "HPKE immediate replay rejected");
    }
    for (const auto& v : hpke_exports) {
        u8 label[64], expected[32], out[40];
        const u32 n = test_unhex(v.context, label);
        test_unhex(v.output, expected);
        memset(out, 0xa5, 40);
        check(hpke_export(context, workspace, {label, n}, {out, 32}).ok() &&
                  bytes_equal(out, expected, 32) && filled(out + 32, 8),
              "HPKE retained exporter matches RFC after receive traffic");
    }
    hpke_release(&context);
    hpke_release(&context);
    p256_release(&recipient);
    check(filled(&context, sizeof(context), 0), "HPKE release clears complete owner");
    SecureZeroMemory(scalar, 32);
    SecureZeroMemory(material, 60);
}
void sender(Check check, KdfSha256Workspace& workspace, HANDLE file, u8* buffer) noexcept {
    auto* plain = buffer;
    auto* cipher = buffer + 65536;
    auto* output = cipher + 65552;
    for (u32 i = 0; i != 65536; ++i)
        plain[i] = static_cast<u8>(i * 17 + 3);
    u8 scalar[32], point[65], enc[80], info[128], aad[17];
    test_unhex(hpke_kems[0].recipient, scalar);
    test_unhex(hpke_kems[0].recipient_point, point);
    const u32 ni = test_unhex(hpke_base_info, info);
    for (u32 i = 0; i != 17; ++i)
        aad[i] = static_cast<u8>(i * 7 + 1);
    P256Key key{};
    HpkeContext send{}, receive{};
    check(p256_import_private(&key, P256Kind::ecdh, {scalar, 32}).ok(), "HPKE roundtrip key");
    memset(enc, 0xa5, 80);
    check(hpke_setup_sender(&send, workspace, {point, 65}, {info, ni}, {}, {enc, 80}).ok() &&
              filled(enc + 65, 15),
          "HPKE normal sender generates fresh ephemeral and preserves output tail");
    check(hpke_setup_recipient(&receive, workspace, key, {enc, 65}, {info, ni}, {}).ok(),
          "HPKE normal generated encapsulation receiver");
    check(send.cipher.object.capacity == send.cipher.object.bytes + 32,
          "HPKE retains just 32 auxiliary secret bytes");
    u8 sender_export[32], receiver_export[32];
    check(hpke_export(send, workspace, {aad, 17}, {sender_export, 32}).ok() &&
              hpke_export(receive, workspace, {aad, 17}, {receiver_export, 32}).ok() &&
              bytes_equal(sender_export, receiver_export, 32),
          "HPKE setup sender/recipient exporter agreement");
    constexpr u32 lengths[]{0, 1, 15, 16, 17, 31, 32, 33, 1024, 8192, 65536};
    for (auto n : lengths) {
        u8 seq[12];
        copy_bytes(seq, send.sequence, 12);
        memset(cipher, 0xa5, 65552);
        memset(output, 0xa5, 65552);
        check(hpke_open(send, {}, {cipher, 16}, {output, 65552}).error == HpkeError::wrong_role &&
                  filled(output, 65552),
              "HPKE sender cannot decrypt");
        check(hpke_seal(send, {aad, 17}, {plain, n}, {cipher, n + 15}).error ==
                      HpkeError::output_capacity &&
                  bytes_equal(seq, send.sequence, 12) && filled(cipher, 65552),
              "HPKE short seal output preserves sequence/output");
        const auto sealed = hpke_seal(send, {aad, 17}, {plain, n}, {cipher, 65552});
        check(sealed.ok() && sealed.bytes == n + 16 && filled(cipher + n + 16, 65552 - n - 16),
              "HPKE native sender lengths/tail");
        if (sealed.ok())
            record(check, file, {enc, 65}, {seq, 12}, {info, ni}, {aad, 17}, {plain, n}, {cipher, n + 16});
        if (n)
            check(hpke_open(receive, {aad, 17}, {cipher, n + 16}, {output, n - 1}).error ==
                          HpkeError::output_capacity &&
                      bytes_equal(seq, receive.sequence, 12),
                  "HPKE short open output preserves sequence");
        const auto opened = hpke_open(receive, {aad, 17}, {cipher, n + 16}, {output, 65552});
        check(opened.ok() && opened.bytes == n && bytes_equal(output, plain, n) &&
                  filled(output + n, 65552 - n) && bytes_equal(send.sequence, receive.sequence, 12),
              "HPKE generated sender recipient independent ownership roundtrip");
    }
    // Explicit QA-only positioning covers the full 96-bit counter, including
    // carries beyond the reference implementation's 64-bit sequence and the
    // last usable value.
    const char* counters[]{"0000000000000000000000ff", "00000000000000000000ffff",
                           "0000000000000000ffffffff", "00000000ffffffffffffffff",
                           "fffffffffffffffffffffffe"};
    for (auto hex : counters) {
        test_unhex(hex, send.sequence);
        copy_bytes(receive.sequence, send.sequence, 12);
        u8 seq[12];
        copy_bytes(seq, send.sequence, 12);
        copy_bytes(cipher, plain, 33);
        check(hpke_seal(send, {aad, 17}, {cipher, 33}, {cipher, 65552}).ok(),
              "HPKE exact in-place seal across counter carries");
        record(check, file, {enc, 65}, {seq, 12}, {info, ni}, {aad, 17}, {plain, 33}, {cipher, 49});
        check(hpke_open(receive, {aad, 17}, {cipher, 49}, {cipher, 65552}).ok() &&
                  bytes_equal(cipher, plain, 33) && bytes_equal(send.sequence, receive.sequence, 12),
              "HPKE exact in-place open across counter carries");
    }
    check(filled(send.sequence, 12, 0xff) && filled(receive.sequence, 12, 0xff),
          "HPKE counter reaches terminal without wrapping");
    memset(output, 0xa5, 65552);
    check(hpke_seal(send, {}, {plain, 1}, {output, 65552}).error == HpkeError::sequence_exhausted &&
              filled(output, 65552),
          "HPKE terminal sender fails before encryption");
    check(hpke_open(receive, {}, {cipher, 49}, {output, 65552}).error == HpkeError::sequence_exhausted &&
              filled(output, 65552),
          "HPKE terminal receiver fails before authentication");
    check(hpke_export(send, workspace, {aad, 17}, {sender_export, 32}).ok() &&
              bytes_equal(sender_export, receiver_export, 32),
          "HPKE export remains stable after sequence exhaustion");
    // Failed replacement must preserve not merely an empty context, but its
    // live keys, sequence, role and exporter allocation.
    u8 previous[sizeof(HpkeContext)];
    copy_bytes(previous, &send, sizeof(send));
    u8 invalid[65]{};
    invalid[0] = 4;
    memset(output, 0xa5, 80);
    check(!hpke_setup_sender(&send, workspace, {invalid, 65}, {}, {}, {output, 80}).ok() &&
              bytes_equal(previous, &send, sizeof(send)) && filled(output, 80),
          "HPKE invalid replacement preserves live exhausted context");
    const u32 capacity = workspace.storage.capacity;
    workspace.storage.capacity = 1;
    check(hpke_setup_sender(&send, workspace, {point, 65}, {info, ni}, {}, {output, 80}).error ==
                  HpkeError::derivation_failure &&
              bytes_equal(previous, &send, sizeof(send)) && filled(output, 80),
          "HPKE provider failure preserves live sender and encapsulation");
    u8 prior_receive[sizeof(HpkeContext)];
    copy_bytes(prior_receive, &receive, sizeof(receive));
    check(hpke_setup_recipient(&receive, workspace, key, {enc, 65}, {info, ni}, {}).error ==
                  HpkeError::derivation_failure &&
              bytes_equal(prior_receive, &receive, sizeof(receive)),
          "HPKE failed replacement preserves recipient owner");
    workspace.storage.capacity = capacity;
    check(!hpke_setup_sender(&send, workspace, {point, 65}, {}, {}, {send.base_nonce, 65}).ok() &&
              bytes_equal(previous, &send, sizeof(send)),
          "HPKE setup cannot overwrite context owner");
    const auto auxiliary = aes_gcm_auxiliary(send.cipher);
    check(!hpke_setup_sender(&send, workspace, {point, 65}, {auxiliary.data, 32}, {}, {output, 80}).ok() &&
              bytes_equal(previous, &send, sizeof(send)),
          "HPKE setup cannot borrow old protected exporter");
    check(!hpke_export(send, workspace, {}, {auxiliary.data, 32}).ok(),
          "HPKE export cannot overwrite retained secret");
    u8 old_enc[65];
    copy_bytes(old_enc, enc, 65);
    check(hpke_setup_sender(&send, workspace, {point, 65}, {info, ni}, {}, {enc, 80}).ok() &&
              !bytes_equal(old_enc, enc, 65) && filled(send.sequence, 12, 0),
          "HPKE fresh replacement resets state with new ephemeral");
    check(hpke_setup_recipient(&receive, workspace, key, {enc, 65}, {info, ni}, {}).ok(),
          "HPKE recipient successful replacement");
    // Use a valid CNG hash handle of the wrong object type to exercise a real
    // provider error, then restore ownership before teardown/replacement.
    void* hash = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
          "HPKE failure provider fixture");
    if (hash) {
        void* original = send.cipher.handle;
        send.cipher.handle = hash;
        memset(output, 0xa5, 80);
        const auto failed = hpke_seal(send, {}, {plain, 17}, {output, 80});
        send.cipher.handle = original;
        check(failed.error == HpkeError::platform_failure && send.poisoned && filled(output, 33, 0) &&
                  filled(output + 33, 47) && filled(send.sequence, 12, 0),
              "HPKE unexpected provider error poisons sender and erases ciphertext/tag");
        check(hpke_seal(send, {}, {plain, 17}, {output, 80}).error == HpkeError::poisoned,
              "HPKE poisoned sender cannot reuse nonce");
        BCryptDestroyHash(hash);
        check(hpke_setup_sender(&send, workspace, {point, 65}, {info, ni}, {}, {enc, 80}).ok() &&
                  !send.poisoned,
              "HPKE fresh setup recovers poisoned context");
    }
    hpke_release(&send);
    hpke_release(&receive);
    p256_release(&key);
    SecureZeroMemory(scalar, 32);
    SecureZeroMemory(sender_export, 32);
    SecureZeroMemory(receiver_export, 32);
}
void auxiliary(Check check) noexcept {
    AesGcmKey key{};
    u8 secret[16]{}, nonce[12]{}, plain[33]{}, out[64], tag[16];
    check(aes_gcm_initialize_with_auxiliary(&key, {secret, 16}, ~u32{0}).error ==
                  AeadError::invalid_input &&
              !key.handle && !key.object.data,
          "AES auxiliary allocation overflow rejected");
    check(aes_gcm_initialize_with_auxiliary(&key, {secret, 16}, 32).ok(), "AES auxiliary key init");
    auto aux = aes_gcm_auxiliary(key);
    check(aux.count == 32 && filled(aux.data, 32, 0), "AES auxiliary starts zeroed");
    if (aux.count == 32) {
        memset(aux.data, 0x37, 32);
        check(aes_gcm_encrypt(key, {nonce, 12}, {}, {plain, 33}, {out, 64}, {tag, 16}).ok() &&
                  filled(aux.data, 32, 0x37),
              "CNG encryption preserves auxiliary bytes");
        check(aes_gcm_decrypt(key, {nonce, 12}, {}, {out, 33}, {tag, 16}, {out, 64}).ok() &&
                  filled(aux.data, 32, 0x37),
              "CNG decryption preserves auxiliary bytes");
        check(!aes_gcm_encrypt(key, {nonce, 12}, {aux.data, 32}, {}, {out, 64}, {tag, 16}).ok(),
              "AES rejects auxiliary input alias");
        check(!aes_gcm_encrypt(key, {nonce, 12}, {}, {plain, 17}, {aux.data, 32}, {tag, 16}).ok() &&
                  filled(aux.data, 32, 0x37),
              "AES rejects auxiliary output alias");
    }
    aes_gcm_release(&key);
    check(!aes_gcm_auxiliary(key).count, "Released AES exposes no auxiliary storage");
}
void guards(Check check, KdfSha256Workspace& workspace) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const u32 page = info.dwPageSize;
    auto* allocation =
        static_cast<u8*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(allocation != nullptr, "HPKE context guard allocation");
    if (!allocation)
        return;
    auto* input = allocation + page - 65;
    auto* end = allocation + page * 3;
    test_unhex(hpke_kems[0].recipient_point, input);
    u8 scalar[32], enc[65], saved[49], plaintext[33], output[80];
    test_unhex(hpke_kems[0].recipient, scalar);
    P256Key key{};
    HpkeContext sender{}, receiver{};
    check(p256_import_private(&key, P256Kind::ecdh, {scalar, 32}).ok(), "HPKE guard key");
    DWORD old = 0;
    check(VirtualProtect(allocation, page, PAGE_READONLY, &old) &&
              VirtualProtect(allocation + page, page, PAGE_NOACCESS, &old) &&
              VirtualProtect(end, page, PAGE_NOACCESS, &old),
          "HPKE context immutable inputs and output guard");
    check(hpke_setup_sender(&sender, workspace, {input, 65}, {}, {}, {end - 65, 65}).ok(),
          "HPKE sender exact encapsulation boundary");
    copy_bytes(enc, end - 65, 65);
    check(hpke_setup_recipient(&receiver, workspace, key, {enc, 65}, {}, {}).ok(),
          "HPKE guard receiver setup");
    copy_bytes(plaintext, input + 32, 33);
    memset(output, 0xa5, 80);
    check(hpke_seal(sender, {}, {input, ~u32{0}}, {output, 80}).error == HpkeError::invalid_input &&
              filled(output, 80) && filled(sender.sequence, 12, 0),
          "HPKE ciphertext length overflow before reading input");
    copy_bytes(output, plaintext, 33);
    check(hpke_seal(sender, {}, {output, 33}, {output + 1, 79}).error == HpkeError::invalid_input &&
              bytes_equal(output, plaintext, 33) && filled(sender.sequence, 12, 0),
          "HPKE partial payload overlap does not consume sequence");
    check(hpke_seal(sender, {}, {input + 32, 33}, {end - 49, 49}).ok(),
          "HPKE exact ciphertext/tag output and immutable plaintext boundary");
    copy_bytes(saved, end - 49, 49);
    check(VirtualProtect(allocation, page, PAGE_READWRITE, &old) != 0, "HPKE replace guard input");
    copy_bytes(allocation + page - 49, saved, 49);
    check(VirtualProtect(allocation, page, PAGE_READONLY, &old) != 0, "HPKE immutable ciphertext");
    check(hpke_open(receiver, {}, {allocation + page - 49, 49}, {end - 33, 33}).ok() &&
              bytes_equal(end - 33, plaintext, 33),
          "HPKE exact open plaintext/ciphertext guard boundaries");
    const auto protected_secret = aes_gcm_auxiliary(sender.cipher);
    memset(output, 0xa5, 80);
    check(hpke_seal(sender, {protected_secret.data, 32}, {}, {output, 80}).error ==
                  HpkeError::invalid_input &&
              filled(output, 80),
          "HPKE AEAD cannot borrow exporter as AAD");
    check(hpke_seal(sender, {}, {plaintext, 1}, {sender.base_nonce, 17}).error == HpkeError::invalid_input,
          "HPKE ciphertext cannot overwrite sequence/nonce owner");
    // Empty in-place payloads still authenticate the tag and advance once.
    check(hpke_seal(sender, {}, {}, {end - 16, 16}).ok(), "HPKE empty seal at tag guard boundary");
    u8 tag[16];
    copy_bytes(tag, end - 16, 16);
    tag[0] ^= 1;
    check(hpke_open(receiver, {}, {tag, 16}, {}).error == HpkeError::authentication_failed &&
              receiver.sequence[11] == 1,
          "HPKE empty payload verifies tag before sequence advance");
    tag[0] ^= 1;
    check(hpke_open(receiver, {}, {tag, 16}, {}).ok() && receiver.sequence[11] == 2,
          "HPKE valid empty ciphertext retries with no output allocation");
    hpke_release(&sender);
    hpke_release(&receiver);
    p256_release(&key);
    SecureZeroMemory(scalar, 32);
    VirtualFree(allocation, 0, MEM_RELEASE);
}
} // namespace
void test_hpke_context(Check check) noexcept {
    auxiliary(check);
    KdfSha256Workspace workspace{};
    check(kdf_sha256_initialize(&workspace).ok(), "HPKE context workspace init");
    if (!workspace.storage.data)
        return;
    published(check, workspace);
    guards(check, workspace);
    auto* buffers = static_cast<u8*>(
        VirtualAlloc(nullptr, 65536 + 65552 * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(buffers != nullptr, "HPKE sender fixture buffers");
    wchar_t path[1024];
    const DWORD n = GetEnvironmentVariableW(L"MAICORD_HPKE_CONTEXT_OUTPUT", path, 1024);
    HANDLE file = INVALID_HANDLE_VALUE;
    if (n && n < 1024) {
        file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(file != INVALID_HANDLE_VALUE, "HPKE context QA file");
    }
    if (buffers) {
        sender(check, workspace, file, buffers);
        VirtualFree(buffers, 0, MEM_RELEASE);
    }
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    kdf_sha256_release(&workspace);
}
} // namespace maicord
