#include "maicord/media/deflate.hpp"
#include "fixtures/deflate_matches.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
u8 output[65538];
u8 expected(u32 pattern, u32 i) noexcept {
    return static_cast<u8>(pattern == 0   ? 65
                           : pattern == 1 ? 65 + i % 3
                           : pattern == 2 ? (i % 257) * 17
                                          : (i % 1021) * 31 + ((i % 1021) >> 3));
}
} // namespace
void test_deflate_differential(void (*check)(bool, const char*) noexcept) noexcept;
void test_deflate_matches(void (*check)(bool, const char*) noexcept) noexcept {
    DeflateWorkspace workspace{};
    for (const auto& fixture : kDeflateMatchFixtures) {
        const u32 first = fixture.bytes / 3, second = fixture.bytes / 2;
        const DeflateInputSpan spans[]{{fixture.data, first},
                                       {nullptr, 0},
                                       {fixture.data + first, second - first},
                                       {fixture.data + second, fixture.bytes - second}};
        output[0] = 0xA5;
        output[fixture.size + 1] = 0x5A;
        const auto result = zlib_inflate({spans, 4}, {output + 1, fixture.size}, fixture.size, &workspace);
        bool exact = result.ok() && result.output_bytes == fixture.size;
        for (u32 i = 0; exact && i < fixture.size; ++i)
            exact = output[i + 1] == expected(fixture.pattern, i);
        check(exact && output[0] == 0xA5 && output[fixture.size + 1] == 0x5A,
              "DEFLATE stored/fixed/dynamic match expansion is exact across fragmented input");
        const DeflateInputSpan truncated{fixture.data, fixture.bytes - 1};
        check(!zlib_inflate({&truncated, 1}, {output + 1, fixture.size}, fixture.size, &workspace).ok(),
              "DEFLATE truncated stream is rejected");
        if (fixture.size) {
            output[fixture.size] = 0x5A;
            check(
                !zlib_inflate({spans, 4}, {output + 1, fixture.size - 1}, fixture.size, &workspace).ok() &&
                    output[fixture.size] == 0x5A,
                "DEFLATE output capacity remains guarded");
        }
    }
    test_deflate_differential(check);
}
} // namespace maicord
