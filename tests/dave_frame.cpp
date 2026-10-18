#include "maicord/protocol/dave_frame.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/dave_frame_vectors.hpp"
bool filled(const void* bytes, u32 count, u8 value) noexcept {
    const auto* p = static_cast<const u8*>(bytes);
    for (u32 i = 0; i != count; ++i)
        if (p[i] != value)
            return false;
    return true;
}
u32 fixture(u8* frame, u32 body, ConstSlice<u8> metadata) noexcept {
    memset(frame, 0x55, body);
    for (u32 i = 0; i != 8; ++i)
        frame[body + i] = static_cast<u8>(0xA0 + i);
    copy_bytes(frame + body + 8, metadata.data, metadata.count);
    frame[body + 8 + metadata.count] = static_cast<u8>(metadata.count + 11);
    frame[body + 9 + metadata.count] = frame[body + 10 + metadata.count] = 0xFA;
    return body + metadata.count + 11;
}
void vectors(Check check) noexcept {
    u8 frame[768], original[768], aad[768], cipher[768], expected_aad[768], expected_cipher[768],
        output[768], expected[768];
    DaveClearRange ranges[8];
    for (const auto& vector : dave_frames) {
        const u32 bytes = test_unhex(vector.wire, frame);
        copy_bytes(original, frame, bytes);
        const auto clear_bytes = test_unhex(vector.aad, expected_aad);
        const auto encrypted = test_unhex(vector.cipher, expected_cipher);
        const auto body = test_unhex(vector.reconstructed, expected);
        DaveFrameView view{};
        view.nonce = 123;
        for (u32 capacity = 0; capacity < vector.ranges; ++capacity) {
            memset(ranges, 0xCC, sizeof(ranges));
            const auto parsed = dave_frame_parse({frame, bytes}, {ranges, capacity}, &view);
            check(parsed.error == DaveFrameError::range_capacity && parsed.range_count == vector.ranges &&
                      view.nonce == 123 && filled(ranges, sizeof(ranges), 0xCC),
                  "DAVE frame range capacity reports complete requirement without partial output");
        }
        memset(ranges, 0xCC, sizeof(ranges));
        const auto parsed = dave_frame_parse({frame, bytes}, {ranges, 8}, &view);
        check(parsed.ok() && view.nonce == vector.nonce && view.clear_ranges.count == vector.ranges &&
                  view.body_bytes == body && view.clear_bytes == clear_bytes && view.frame.data == frame &&
                  filled(ranges + vector.ranges, (8 - vector.ranges) * sizeof(DaveClearRange), 0xCC),
              "DAVE parser matches independently encoded metadata and borrows original frame");
        if (!parsed.ok())
            continue;
        memset(aad, 0xCC, sizeof(aad));
        memset(cipher, 0xCC, sizeof(cipher));
        for (u32 capacity = 0; capacity < clear_bytes; ++capacity)
            check(dave_frame_gather(view, {aad, capacity}, {cipher, sizeof(cipher)}) ==
                      DaveFrameError::output_capacity,
                  "DAVE gather rejects every short AAD output");
        for (u32 capacity = 0; capacity < encrypted; ++capacity)
            check(dave_frame_gather(view, {aad, sizeof(aad)}, {cipher, capacity}) ==
                      DaveFrameError::output_capacity,
                  "DAVE gather rejects every short cipher output");
        check(filled(aad, sizeof(aad), 0xCC) && filled(cipher, sizeof(cipher), 0xCC),
              "DAVE gather capacity failures preserve both complete output buffers");
        check(dave_frame_gather(view, {aad, sizeof(aad)}, {cipher, sizeof(cipher)}) ==
                      DaveFrameError::none &&
                  bytes_equal(aad, expected_aad, clear_bytes) &&
                  bytes_equal(cipher, expected_cipher, encrypted) &&
                  filled(aad + clear_bytes, sizeof(aad) - clear_bytes, 0xCC) &&
                  filled(cipher + encrypted, sizeof(cipher) - encrypted, 0xCC),
              "DAVE gather matches independent AAD/cipher partitions and preserves capacity tails");
        if (!clear_bytes)
            check(dave_frame_gather(view, {}, {frame, body}) == DaveFrameError::none &&
                      bytes_equal(frame, original, bytes),
                  "DAVE wholly encrypted frame supports direct body/cipher alias without copying");
        for (u32 i = 0; i != encrypted; ++i)
            cipher[i] ^= 0xA5; // Stand-in for authenticated plaintext.
        memset(output, 0xCC, sizeof(output));
        for (u32 capacity = 0; capacity < body; ++capacity)
            check(dave_frame_reconstruct(view, {cipher, encrypted}, {output, capacity}) ==
                      DaveFrameError::output_capacity,
                  "DAVE reconstruction rejects every short output");
        check(filled(output, sizeof(output), 0xCC) &&
                  dave_frame_reconstruct(view, {cipher, encrypted}, {output, sizeof(output)}) ==
                      DaveFrameError::none &&
                  bytes_equal(output, expected, body) && filled(output + body, sizeof(output) - body, 0xCC),
              "DAVE reconstructed frame matches independent mixed clear/plaintext fixture");
        check(dave_frame_reconstruct(view, {cipher, encrypted}, {frame, body}) == DaveFrameError::none &&
                  bytes_equal(frame, expected, body) &&
                  bytes_equal(frame + body, original + body, bytes - body),
              "DAVE reassembly can reuse original body while preserving tag and supplemental data");
        if (!clear_bytes)
            check(dave_frame_reconstruct(view, {frame, body}, {frame, body}) == DaveFrameError::none &&
                      bytes_equal(frame, expected, body),
                  "DAVE fully encrypted in-place plaintext needs no separate reconstruction copy");
    }
}
void invalid(Check check) noexcept {
    struct Case {
        const char* metadata;
        bool valid;
    };
    const Case cases[]{{"", false},
                       {"80", false},
                       {"00", true},
                       {"8000", true},
                       {"80808080808080808000", true},
                       {"8080808010", false},
                       {"ffffffffffffffffff01", false},
                       {"80808080808080808080", false},
                       {"0000", false},
                       {"000001", true},
                       {"000500", true},
                       {"000501", false},
                       {"000600", false},
                       {"000006", false},
                       {"0002020101", false},
                       {"0000040300", false},
                       {"00ffffffffffffffffff0100", false},
                       {"0000ffffffffffffffffff01", false}};
    u8 frame[512], metadata[256], aad[64], cipher[64], output[64];
    DaveClearRange ranges[4];
    for (const auto& c : cases) {
        const auto n = test_unhex(c.metadata, metadata);
        const auto bytes = fixture(frame, 5, {metadata, n});
        DaveFrameView view{};
        view.nonce = 123;
        memset(ranges, 0xCC, sizeof(ranges));
        const auto parsed = dave_frame_parse({frame, bytes}, {ranges, 4}, &view);
        check(parsed.ok() == c.valid &&
                  (c.valid || (view.nonce == 123 && filled(ranges, sizeof(ranges), 0xCC))),
              "DAVE malformed ULEB/range/footer boundaries preserve outputs");
        if (c.valid) {
            for (u32 prefix = 0; prefix < bytes; ++prefix) {
                DaveFrameView preserved{};
                preserved.nonce = 234;
                check(!dave_frame_parse({frame, prefix}, {ranges, 4}, &preserved).ok() &&
                          preserved.nonce == 234,
                      "DAVE incomplete frame prefixes never publish a view");
            }
        }
    }
    const u8 ranged[]{0, 0, 1, 4, 1};
    const auto bytes = fixture(frame, 5, {ranged, sizeof(ranged)});
    DaveFrameView view{};
    check(dave_frame_parse({frame, bytes}, {ranges, 4}, &view).ok(), "DAVE alias fixture parses");
    memset(output, 0xCC, sizeof(output));
    check(dave_frame_gather(view, {frame, 2}, {cipher, sizeof(cipher)}) == DaveFrameError::invalid_input &&
              dave_frame_gather(view, {aad, 2}, {frame, 3}) == DaveFrameError::invalid_input &&
              dave_frame_gather(view, {output, 2}, {output + 1, 3}) == DaveFrameError::invalid_input &&
              dave_frame_reconstruct(view, {output, 3}, {output, 5}) == DaveFrameError::invalid_input &&
              dave_frame_reconstruct(view, {cipher, 3}, {frame + 1, 5}) == DaveFrameError::invalid_input &&
              filled(output, sizeof(output), 0xCC),
          "DAVE gather/reassembly reject unsafe partial and header-insertion aliases");
    ranges[0].offset = 99;
    check(dave_frame_gather(view, {aad, sizeof(aad)}, {output, sizeof(output)}) ==
                  DaveFrameError::invalid_input &&
              dave_frame_reconstruct(view, {cipher, 3}, {output, sizeof(output)}) ==
                  DaveFrameError::invalid_input &&
              filled(output, sizeof(output), 0xCC),
          "DAVE helpers revalidate borrowed range bounds before writing");
    check(
        dave_frame_parse({frame, bytes}, {reinterpret_cast<DaveClearRange*>(frame), 1}, &view).error ==
                DaveFrameError::invalid_input &&
            dave_frame_parse({frame, bytes}, {reinterpret_cast<DaveClearRange*>(&view), 1}, &view).error ==
                DaveFrameError::invalid_input &&
            dave_frame_parse({reinterpret_cast<const u8*>(~usize{0} - 4), 12}, {}, &view).error ==
                DaveFrameError::invalid_input,
        "DAVE parser rejects aliased metadata outputs and wrapping input ranges");
    const u8 silence[]{0xF8, 0xFF, 0xFE};
    check(dave_frame_parse({silence, 3}, {}, &view).error == DaveFrameError::not_encrypted &&
              dave_frame_parse({}, {}, &view).error == DaveFrameError::not_encrypted,
          "DAVE parser classifies unmarked input without authorizing passthrough");
    frame[bytes - 3] = 255;
    check(dave_frame_parse({frame, bytes}, {ranges, 4}, &view).error == DaveFrameError::malformed,
          "DAVE supplemental size cannot precede the supplied frame");
    // Maximal count of zero-sized ranges reaches the actual one-byte suffix limit.
    clear_bytes(metadata, 243);
    const auto maximum_bytes = fixture(frame, 0, {metadata, 243});
    DaveClearRange maximum[kDaveMaximumClearRanges];
    check(
        dave_frame_parse({frame, maximum_bytes}, {maximum, kDaveMaximumClearRanges}, &view).ok() &&
            view.clear_ranges.count == kDaveMaximumClearRanges && !view.body_bytes &&
            dave_frame_gather(view, {}, {}) == DaveFrameError::none,
        "DAVE supports the complete wire-format clear-range count without a smaller implementation limit");
}
void guarded(Check check) noexcept {
    Win32SystemInfo info{};
    GetSystemInfo(info.native());
    const u32 page = info.dwPageSize;
    auto* memory =
        static_cast<u8*>(VirtualAlloc(nullptr, page * 3, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
    DWORD previous = 0;
    const bool allocated = memory && VirtualProtect(memory + page, page, PAGE_READWRITE, &previous);
    check(allocated, "DAVE guard-page fixture allocation");
    if (allocated) {
        DaveClearRange ranges[8];
        DaveFrameView view{};
        for (u32 bytes = 0; bytes < 12; ++bytes) {
            auto* frame = memory + page;
            clear_bytes(frame, bytes);
            if (bytes >= 2)
                frame[bytes - 1] = frame[bytes - 2] = 0xFA;
            if (bytes >= 3)
                frame[bytes - 3] = 12;
            check(!dave_frame_parse({frame, bytes}, {ranges, 8}, &view).ok(),
                  "DAVE short marked frame cannot read before its guarded start");
        }
        u8 source[768], aad[768], cipher[768];
        for (const auto& vector : dave_frames) {
            const auto bytes = test_unhex(vector.wire, source);
            auto* frame = memory + page * 2 - bytes;
            copy_bytes(frame, source, bytes);
            check(dave_frame_parse({frame, bytes}, {ranges, 8}, &view).ok() &&
                      dave_frame_gather(view, {aad, sizeof(aad)}, {cipher, sizeof(cipher)}) ==
                          DaveFrameError::none,
                  "DAVE frame ending at a guard page parses and gathers without overread");
        }
    }
    if (memory)
        VirtualFree(memory, 0, MEM_RELEASE);
}
} // namespace
void test_dave_frame(Check check) noexcept {
    vectors(check);
    invalid(check);
    guarded(check);
}
} // namespace maicord
