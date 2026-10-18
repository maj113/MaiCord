#include "aead_internal.hpp"

#include "maicord/sys/crypto.hpp"

namespace maicord {

AeadResult aead_validate(ConstSlice<u8> owner, ConstSlice<u8> storage, u32 nonce_bytes,
                         ConstSlice<u8> nonce, ConstSlice<u8> aad, ConstSlice<u8> input, Slice<u8> output,
                         ConstSlice<u8> tag, bool encrypt, u32 tag_bytes) noexcept {
    if (nonce.count != nonce_bytes || tag.count != tag_bytes || !byte_range_valid(nonce) || !byte_range_valid(aad) ||
        !byte_range_valid(input) || !byte_range_valid(tag) || !byte_range_valid({output.data, output.count}))
        return aead_result(AeadError::invalid_input);
    if (output.count < input.count)
        return aead_result(AeadError::output_too_small);
    const ConstSlice<u8> written{output.data, input.count};
    if ((input.data != output.data && byte_ranges_overlap(input, written)) || byte_ranges_overlap(written, nonce) ||
        byte_ranges_overlap(written, aad) || byte_ranges_overlap(written, tag) ||
        (encrypt && (byte_ranges_overlap(tag, input) || byte_ranges_overlap(tag, nonce) || byte_ranges_overlap(tag, aad))))
        return aead_result(AeadError::invalid_input);
    const ConstSlice<u8> spans[]{nonce, aad, input, written, tag};
    for (const auto span : spans)
        if (byte_ranges_overlap(span, owner) || byte_ranges_overlap(span, storage))
            return aead_result(AeadError::invalid_input);
    return aead_result(AeadError::none);
}

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

} // namespace maicord
