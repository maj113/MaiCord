#include "maicord/platform/crypto/hkdf_sha256.hpp"
#include "maicord/core/memory.hpp"
#include "../../support/test_output.hpp"

// SHA-256, HMAC-SHA-256 and HKDF-SHA-256 against the published vectors (FIPS 180-4 examples, RFC 4231, RFC 5869) and the
// argument and alias rules of the API. Values that no RFC lists (the empty-key HMAC, a 16-byte pseudorandom key, the
// longest expansion) come from Python's hashlib and hmac.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

ConstSlice<u8> text(const char* value) noexcept {
    return {reinterpret_cast<const u8*>(value), c_string_length(value)};
}

bool equals_hex(const u8* actual, const char* hex) noexcept {
    u8 expected[256];
    const u32 bytes = test_unhex(hex, expected);
    return bytes_equal(actual, expected, bytes);
}

void sha256_vectors(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 out[40];
    memset(out, 0xa5, sizeof(out));
    const auto empty = sha256(workspace, {}, {out, 40});
    check(empty.ok() && empty.bytes == 32 &&
              equals_hex(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") &&
              test_filled(out + 32, 8, 0xa5),
          "SHA-256 of nothing, with the excess capacity preserved");
    const ConstSlice<u8> abc[]{text("abc")};
    check(sha256(workspace, {abc, 1}, {out, 32}).ok() &&
              equals_hex(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "SHA-256 of abc");
    const ConstSlice<u8> split[]{text("a"), text("b"), {}, text("c")};
    check(sha256(workspace, {split, 4}, {out, 32}).ok() &&
              equals_hex(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "SHA-256 over segments, an empty one included");
    const ConstSlice<u8> two_blocks[]{text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")};
    check(sha256(workspace, {two_blocks, 1}, {out, 32}).ok() &&
              equals_hex(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
          "SHA-256 of the two-block FIPS message");
    static u8 thousand[1000];
    memset(thousand, 'a', sizeof(thousand));
    ConstSlice<u8> million[1000];
    for (auto& part : million)
        part = {thousand, 1000};
    check(sha256(workspace, {million, 1000}, {out, 32}).ok() &&
              equals_hex(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
          "SHA-256 of a million a in a thousand segments");
}

void hmac_vectors(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 key[131], data[152], out[32];
    // RFC 4231 test cases 1 to 4, 6 and 7.
    memset(key, 0x0b, 20);
    const ConstSlice<u8> hi[]{text("Hi There")};
    check(hmac_sha256(workspace, {key, 20}, {hi, 1}, {out, 32}).ok() &&
              equals_hex(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
          "HMAC-SHA-256 RFC 4231 case 1");
    const ConstSlice<u8> jefe[]{text("what do ya want for nothing?")};
    check(hmac_sha256(workspace, text("Jefe"), {jefe, 1}, {out, 32}).ok() &&
              equals_hex(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
          "HMAC-SHA-256 RFC 4231 case 2");
    memset(key, 0xaa, 20);
    memset(data, 0xdd, 50);
    const ConstSlice<u8> dd[]{{data, 50}};
    check(hmac_sha256(workspace, {key, 20}, {dd, 1}, {out, 32}).ok() &&
              equals_hex(out, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"),
          "HMAC-SHA-256 RFC 4231 case 3");
    for (u32 i = 0; i != 25; ++i)
        key[i] = static_cast<u8>(i + 1);
    memset(data, 0xcd, 50);
    check(hmac_sha256(workspace, {key, 25}, {dd, 1}, {out, 32}).ok() &&
              equals_hex(out, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"),
          "HMAC-SHA-256 RFC 4231 case 4");
    memset(key, 0xaa, 131);
    const ConstSlice<u8> first_hash[]{text("Test Using Larger Than Block-Size Key - Hash Key First")};
    check(hmac_sha256(workspace, {key, 131}, {first_hash, 1}, {out, 32}).ok() &&
              equals_hex(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"),
          "HMAC-SHA-256 RFC 4231 case 6, a key longer than the block");
    const ConstSlice<u8> long_data[]{text("This is a test using a larger than block-size key and a larger than block-size data. "),
                                     text("The key needs to be hashed before being used by the HMAC algorithm.")};
    check(hmac_sha256(workspace, {key, 131}, {long_data, 2}, {out, 32}).ok() &&
              equals_hex(out, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"),
          "HMAC-SHA-256 RFC 4231 case 7, in two segments");
    const ConstSlice<u8> abc[]{text("abc")};
    check(hmac_sha256(workspace, {}, {abc, 1}, {out, 32}).ok() &&
              equals_hex(out, "fd7adb152c05ef80dccf50a1fa4c05d5a3ec6da95575fc312ae7c5d091836351"),
          "an empty HMAC key is a valid key, not an unkeyed hash");
    memset(key, 0x0b, 20);
    const ConstSlice<u8> there[]{text("There")};
    check(hmac_sha256_prefixed(workspace, {key, 20}, text("Hi "), {there, 1}, {out, 32}).ok() &&
              equals_hex(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
          "a prefixed HMAC equals the HMAC of the concatenation");
}

void hkdf_vectors(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 ikm[80], salt[80], info[80], prk[32], okm[255 * 32];
    // RFC 5869 A.1.
    memset(ikm, 0x0b, 22);
    for (u32 i = 0; i != 13; ++i)
        salt[i] = static_cast<u8>(i);
    for (u32 i = 0; i != 10; ++i)
        info[i] = static_cast<u8>(0xf0 + i);
    ConstSlice<u8> info_parts[]{{info, 10}};
    check(hkdf_sha256_extract(workspace, {salt, 13}, {ikm, 22}, {prk, 32}).ok() &&
              equals_hex(prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"),
          "HKDF RFC 5869 A.1 extract");
    check(hkdf_sha256_expand(workspace, {prk, 32}, {info_parts, 1}, {okm, 42}).ok() &&
              equals_hex(okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
          "HKDF RFC 5869 A.1 expand");
    // A.2: 80-byte inputs and a 82-byte output, the info in two segments.
    u8 long_salt[80], long_info[80];
    for (u32 i = 0; i != 80; ++i) {
        ikm[i] = static_cast<u8>(i);
        long_salt[i] = static_cast<u8>(0x60 + i);
        long_info[i] = static_cast<u8>(0xb0 + i);
    }
    const ConstSlice<u8> long_info_parts[]{{long_info, 30}, {long_info + 30, 50}};
    check(hkdf_sha256_extract(workspace, {long_salt, 80}, {ikm, 80}, {prk, 32}).ok() &&
              equals_hex(prk, "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244"),
          "HKDF RFC 5869 A.2 extract");
    check(hkdf_sha256_expand(workspace, {prk, 32}, {long_info_parts, 2}, {okm, 82}).ok() &&
              equals_hex(okm, "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09"
                              "da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87"),
          "HKDF RFC 5869 A.2 expand");
    // A.3: empty salt and info.
    memset(ikm, 0x0b, 22);
    check(hkdf_sha256_extract(workspace, {}, {ikm, 22}, {prk, 32}).ok() &&
              equals_hex(prk, "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04"),
          "HKDF RFC 5869 A.3 extract with an empty salt");
    check(hkdf_sha256_expand(workspace, {prk, 32}, {}, {okm, 42}).ok() &&
              equals_hex(okm, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"),
          "HKDF RFC 5869 A.3 expand with no info");
    // A 16-byte pseudorandom key is accepted (the DAVE ratchet seed).
    u8 short_prk[16];
    for (u32 i = 0; i != 16; ++i)
        short_prk[i] = static_cast<u8>(i);
    const ConstSlice<u8> word[]{text("info")};
    check(hkdf_sha256_expand(workspace, {short_prk, 16}, {word, 1}, {okm, 48}).ok() &&
              equals_hex(okm, "80fead2937e205140e296c7af7d6ebaecc6f0e7f105ce1d370eeefc9676e855145da215d495b0edf0f89b8e6b98232d8"),
          "HKDF expands a 16-byte key");
    // The longest output, 255 blocks.
    for (u32 i = 0; i != 32; ++i)
        prk[i] = static_cast<u8>(i);
    const ConstSlice<u8> x[]{text("x")};
    const auto longest = hkdf_sha256_expand(workspace, {prk, 32}, {x, 1}, {okm, sizeof(okm)});
    check(longest.ok() && longest.bytes == sizeof(okm) &&
              equals_hex(okm, "ef108b0fc1288bc0f8038607515b1cd648deb5014f8de180e9f1813f258bdd6e") &&
              equals_hex(okm + sizeof(okm) - 32, "108c2f9b6be2f67fccbc845d5c9783cc78e04eb47691830d2eec2b7229ba2954"),
          "HKDF expands 255 blocks");
    u8 guard[8200];
    memset(guard, 0xa5, sizeof(guard));
    check(hkdf_sha256_expand(workspace, {prk, 32}, {x, 1}, {guard, 255 * 32 + 1}).error == KdfError::invalid_input &&
              test_filled(guard, sizeof(guard), 0xa5),
          "HKDF rejects an output beyond 255 blocks without touching it");
    check(hkdf_sha256_expand(workspace, {}, {x, 1}, {guard, 32}).error == KdfError::invalid_input &&
              test_filled(guard, 32, 0xa5),
          "HKDF expand requires a nonempty key");
    const auto none = hkdf_sha256_expand(workspace, {prk, 32}, {x, 1}, {guard, 0});
    check(none.ok() && none.bytes == 0, "HKDF expands zero bytes");
}

void boundaries(Check check, KdfSha256Workspace& workspace) noexcept {
    u8 key[16]{}, out[64], other[64];
    const ConstSlice<u8> parts[]{{other, 8}};
    memset(out, 0xa5, sizeof(out));
    KdfSha256Workspace unused{};
    check(sha256(unused, {parts, 1}, {out, 32}).error == KdfError::not_initialized &&
              hmac_sha256(unused, {key, 16}, {parts, 1}, {out, 32}).error == KdfError::not_initialized &&
              test_filled(out, 64, 0xa5),
          "hashing before initialization is rejected");
    check(sha256(workspace, {parts, 1}, {out, 31}).error == KdfError::output_capacity && test_filled(out, 64, 0xa5),
          "a short output is rejected unchanged");
    check(hmac_sha256(workspace, {out, 16}, {parts, 1}, {out + 8, 32}).error == KdfError::invalid_input &&
              test_filled(out, 64, 0xa5),
          "an output overlapping the key is rejected");
    const ConstSlice<u8> overlapping[]{{out + 16, 8}};
    check(sha256(workspace, {overlapping, 1}, {out, 32}).error == KdfError::invalid_input &&
              test_filled(out, 64, 0xa5),
          "an output overlapping a segment is rejected");
    const ConstSlice<u8> storage[]{{workspace.storage.data, 16}};
    check(sha256(workspace, {storage, 1}, {out, 32}).error == KdfError::invalid_input &&
              test_filled(out, 64, 0xa5),
          "an input inside the workspace storage is rejected");
    const ConstSlice<u8> owner[]{{reinterpret_cast<const u8*>(&workspace), 8}};
    check(sha256(workspace, {owner, 1}, {out, 32}).error == KdfError::invalid_input,
          "an input inside the workspace owner is rejected");
    check(sha256(workspace, {parts, 1}, {workspace.storage.data, 32}).error == KdfError::invalid_input,
          "an output inside the workspace storage is rejected");
    const ConstSlice<u8> invalid[]{{nullptr, 4}};
    check(sha256(workspace, {invalid, 1}, {out, 32}).error == KdfError::invalid_input && test_filled(out, 64, 0xa5),
          "a null segment with a length is rejected");
    check(kdf_sha256_initialize(&workspace).error == KdfError::invalid_input,
          "a live workspace cannot be initialized again");
}

} // namespace

void test_kdf_sha256(Check check) noexcept {
    KdfSha256Workspace workspace{};
    check(kdf_sha256_initialize(&workspace).ok() && workspace.storage.data && workspace.storage.locked,
          "KDF workspace initialization");
    if (!workspace.storage.data)
        return;
    sha256_vectors(check, workspace);
    hmac_vectors(check, workspace);
    hkdf_vectors(check, workspace);
    boundaries(check, workspace);
    kdf_sha256_release(&workspace);
    check(!workspace.storage.data && !workspace.storage.locked, "KDF workspace release clears the owner");
    kdf_sha256_release(&workspace);
    kdf_sha256_release(nullptr);
}

} // namespace maicord
