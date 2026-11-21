#include "aead_internal.hpp"

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

} // namespace maicord
