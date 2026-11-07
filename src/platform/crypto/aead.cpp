#include "aead_internal.hpp"

#if defined(_WIN32)
#include "maicord/sys/crypto.hpp"
#endif

namespace maicord {

// Runs on every packet. Ranges are normalized once (an empty one overlaps nothing) so each alias test is two
// compares and the tests combine without branches: the two dozen inlined, branching copies this replaced were
// twice the size and slower.
AeadResult aead_validate(ConstSlice<u8> owner, ConstSlice<u8> storage, u32 nonce_bytes,
                         ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input, Slice<u8> output,
                         ConstSlice<u8> tag, bool encrypt, u32 tag_bytes) noexcept {
    const auto valid = [](const void* data, usize bytes) noexcept {
        return bytes == 0 || (data && reinterpret_cast<usize>(data) <= ~usize{0} - bytes);
    };
    if (nonce.count != nonce_bytes || tag.count != tag_bytes || !valid(nonce.data, nonce.count) ||
        !valid(aad.data, aad.count) || !valid(input.data, input.count) || !valid(tag.data, tag.count) ||
        !valid(output.data, output.count))
        return aead_result(AeadError::invalid_input);
    if (output.count < input.count)
        return aead_result(AeadError::output_too_small);
    struct Range {
        usize first, last;
    };
    // An empty range is the one that overlaps nothing: its first is above every last, its last below every first.
    const auto range = [](const u8* data, usize bytes) noexcept {
        const usize first = reinterpret_cast<usize>(data);
        return bytes ? Range{first, first + bytes} : Range{~usize{0}, 0};
    };
    const auto overlap = [](const Range& a, const Range& b) noexcept { return (a.first < b.last) & (b.first < a.last); };
    const Range n = range(nonce.data, nonce.count), a = range(aad.data, aad.count),
                i = range(input.data, input.count), w = range(output.data, input.count),
                t = range(tag.data, tag.count), o = range(owner.data, owner.count),
                s = range(storage.data, storage.count);
    bool conflict = (input.data != output.data) & overlap(i, w);
    conflict |= overlap(w, n) | overlap(w, a) | overlap(w, t);
    conflict |= encrypt & (overlap(t, i) | overlap(t, n) | overlap(t, a));
    conflict |= overlap(n, o) | overlap(n, s) | overlap(a, o) | overlap(a, s) | overlap(i, o) | overlap(i, s) |
                overlap(w, o) | overlap(w, s) | overlap(t, o) | overlap(t, s);
    return aead_result(conflict ? AeadError::invalid_input : AeadError::none);
}

#if defined(_WIN32)
AeadResult aead_cng_crypt(void* handle, ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input,
                          Slice<u8> output, u8* tag, bool encrypt) noexcept {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = const_cast<u8*>(nonce.data);
    auth.cbNonce = nonce.count;
    auth.pbAuthData = aad.count ? const_cast<u8*>(aad.data) : nullptr;
    auth.cbAuthData = aad.count;
    auth.pbTag = tag;
    auth.cbTag = 16;
    // Both null pointers mean authenticated empty data. Only a null output
    // would instead ask CNG for a size and skip authentication altogether.
    u8* source = input.count ? const_cast<u8*>(input.data) : nullptr;
    u8* destination = input.count ? output.data : nullptr;
    ULONG bytes = 0;
    const NTSTATUS status = encrypt ? BCryptEncrypt(handle, source, input.count, &auth, nullptr, 0,
                                                    destination, input.count, &bytes, 0)
                                    : BCryptDecrypt(handle, source, input.count, &auth, nullptr, 0,
                                                    destination, input.count, &bytes, 0);
    if (status < 0 || bytes != input.count) {
        if (input.count)
            SecureZeroMemory(output.data, input.count);
        if (encrypt)
            SecureZeroMemory(tag, 16);
        constexpr NTSTATUS auth_tag_mismatch = static_cast<NTSTATUS>(0xc000a002u);
        return aead_result(status == auth_tag_mismatch ? AeadError::authentication_failed
                                                       : AeadError::platform_failure,
                           0, status);
    }
    return aead_result(AeadError::none, bytes);
}

#endif

} // namespace maicord
