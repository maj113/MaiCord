#include "maicord/media/deflate.hpp"
#include "fixtures/deflate_matches.hpp"
#include "fixtures/gateway_growth.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
u8 output[65538];
enum class GrowthMode : u8 { normal, refused, moved, shrunk, over_limit };
struct Growth {
    StableStorage storage;
    u32 calls;
    GrowthMode mode;
};
struct AliasedGrowth { u8* base; u32 capacity; };
bool overlapping_growth(void* opaque, u32, Slice<u8>* backed) noexcept {
    const auto& owner = *static_cast<AliasedGrowth*>(opaque);
    *backed = {owner.base, owner.capacity};
    return true;
}
bool grow(void* opaque, u32 required, Slice<u8>* backed) noexcept {
    auto& owner = *static_cast<Growth*>(opaque);
    ++owner.calls;
    u8* base = stable_storage_data<u8>(owner.storage);
    if (owner.mode == GrowthMode::refused) return false;
    if (owner.mode == GrowthMode::moved) {
        *backed = {base + 1, required};
        return true;
    }
    if (owner.mode == GrowthMode::shrunk) {
        *backed = {base, owner.storage.capacity / 2};
        return true;
    }
    if (owner.mode == GrowthMode::over_limit) {
        *backed = {base, owner.storage.reserved_capacity + 1};
        return true;
    }
    if (!stable_storage_grow(&owner.storage, required)) return false;
    *backed = {base, owner.storage.capacity};
    return true;
}
void clear(Growth& owner) noexcept {
    if (owner.storage.arena.base) SecureZeroMemory(owner.storage.arena.base, owner.storage.arena.committed);
    stable_storage_release(&owner.storage);
    owner = {};
}
void stream_growth(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr u32 maximum = 8 * 1024 * 1024;
    Growth owner{};
    ZlibStreamInflater stream{};
    check(stable_storage_initialize(&owner.storage, maximum, 4096, 1, 1), "inflate growth: stable output reserves bounded 8 MiB with 4 KiB commit");
    if (!owner.storage.arena.base) return;
    Slice<u8> view{owner.storage.arena.base, owner.storage.capacity};
    const auto* base = view.data;
    const DeflateInputSpan hello{kGatewayGrowthHello, sizeof(kGatewayGrowthHello)};
    const DeflateInputSpan ready_parts[]{
        {kGatewayGrowthReady, 17}, {nullptr, 0}, {kGatewayGrowthReady + 17, sizeof(kGatewayGrowthReady) - 17}};
    const DeflateInputSpan follow{kGatewayGrowthFollow, sizeof(kGatewayGrowthFollow)};
    const DeflateInputSpan bound{kGatewayGrowthBound, sizeof(kGatewayGrowthBound)};
    zlib_stream_initialize(&stream);
    auto result = zlib_stream_inflate_flush_growing(&stream, {&hello, 1}, &view, maximum, grow, &owner);
    check(result.ok() && result.output_bytes == kGatewayGrowthHelloOutputBytes && !owner.calls &&
        owner.storage.arena.committed == 4096, "inflate growth: small persistent HELLO needs no growth callback or extra commit");
    result = zlib_stream_inflate_flush_growing(&stream, {ready_parts, 3}, &view, maximum, grow, &owner);
    check(result.ok() && result.output_bytes == kGatewayGrowthReadyOutputBytes && result.output_bytes > 128 * 1024 &&
        view.data == base && view.count == 512 * 1024 && owner.storage.arena.committed == view.count &&
        owner.calls == 7 && !stream.failed && bytes_equal(view.data, "{\"op\":0,\"t\":\"READY\"", 19),
        "inflate growth: fragmented dynamic READY expands once beyond 128 KiB at stable geometric write boundaries");
    const auto calls = owner.calls;
    result = zlib_stream_inflate_flush_growing(&stream, {&follow, 1}, &view, maximum, grow, &owner);
    check(result.ok() && result.output_bytes == kGatewayGrowthFollowOutputBytes && owner.calls == calls &&
        view.data == base && !stream.failed, "inflate growth: dictionary persists across flushes without new backing or replay");
    u8 no_history[sizeof(kGatewayGrowthFollow) + 2]{};
    no_history[0] = 0x78; no_history[1] = 0x9c;
    copy_bytes(no_history + 2, kGatewayGrowthFollow, sizeof(kGatewayGrowthFollow));
    const DeflateInputSpan dictionary_required{no_history, sizeof(no_history)};
    zlib_stream_initialize(&stream);
    check(zlib_stream_inflate_flush(&stream, {&dictionary_required, 1}, view).error == ZlibInflateError::invalid_distance,
        "inflate growth: independent continuation requires the preceding flush dictionary");
    zlib_stream_initialize(&stream);
    result = zlib_stream_inflate_flush_growing(&stream, {&bound, 1}, &view, maximum, grow, &owner);
    bool exact = result.ok() && result.output_bytes == maximum && view.count == maximum && view.data == base;
    for (u32 i = 0; exact && i < maximum; ++i) exact = view[i] == 'A';
    check(exact && owner.storage.arena.committed == maximum, "inflate growth: exact 8 MiB output bound is complete with no truncation");
    clear(owner);

    for (u32 mode = 0; mode < 5; ++mode) {
        check(stable_storage_initialize(&owner.storage, maximum, 4096, 1, 1), "inflate growth: terminal failure owner initializes");
        if (!owner.storage.arena.base) return;
        owner.mode = static_cast<GrowthMode>(mode);
        view = {owner.storage.arena.base, owner.storage.capacity};
        zlib_stream_initialize(&stream);
        result = zlib_stream_inflate_flush_growing(&stream, {&bound, 1}, &view,
            mode ? maximum : maximum - 1, grow, &owner);
        check(result.error == ZlibInflateError::output_capacity && stream.failed && view.data == owner.storage.arena.base &&
            result.output_bytes <= (mode ? 4096 : maximum - 1) &&
            zlib_stream_inflate_flush(&stream, {&bound, 1}, view).error == ZlibInflateError::stream_failed,
            "inflate growth: bound, denied commit, moved/shrunk/over-limit backing poison stream without a replay");
        clear(owner);
    }
    // Stored blocks preflight their complete output run, preserving the fixed
    // API and the same streaming dictionary update semantics.
    check(stable_storage_initialize(&owner.storage, 8192, 16, 1, 1), "inflate growth: stored-block owner initializes");
    if (!owner.storage.arena.base) return;
    view = {owner.storage.arena.base, owner.storage.capacity};
    const DeflateInputSpan stored{kGatewayGrowthStored, sizeof(kGatewayGrowthStored)};
    zlib_stream_initialize(&stream);
    result = zlib_stream_inflate_flush_growing(&stream, {&stored, 1}, &view, 8192, grow, &owner);
    exact = result.ok() && result.output_bytes == 4096 && owner.calls == 1;
    for (u32 i = 0; exact && i < 4096; ++i) exact = view[i] == i % 251;
    check(exact, "inflate growth: stored output growth preserves exact bytes and window updates");
    clear(owner);
    zlib_stream_initialize(&stream);
    output[0] = 0xa5;
    check(zlib_stream_inflate_flush(&stream, {&stored, 1}, {output + 1, 4096}).ok() && output[0] == 0xa5,
        "inflate growth: old fixed streaming API retains exact successful stored behavior");
    zlib_stream_initialize(&stream);
    check(zlib_stream_inflate_flush(&stream, {&stored, 1}, {output + 1, 4095}).error == ZlibInflateError::output_capacity &&
        stream.failed, "inflate growth: old fixed streaming API retains terminal output-capacity behavior");
    zlib_stream_initialize(&stream);
    Slice<u8> aliased{reinterpret_cast<u8*>(&stream), sizeof(stream)};
    check(zlib_stream_inflate_flush_growing(&stream, {&hello, 1}, &aliased, sizeof(stream), grow, &owner).error ==
        ZlibInflateError::invalid_input && !stream.failed && !stream.initialized,
        "inflate growth: stream/output alias is rejected before consuming header or dictionary");
    aliased = {const_cast<u8*>(kGatewayGrowthHello), sizeof(kGatewayGrowthHello)};
    check(zlib_stream_inflate_flush_growing(&stream, {&hello, 1}, &aliased, 8192, grow, &owner).error ==
        ZlibInflateError::invalid_input && !stream.initialized,
        "inflate growth: compressed input/output alias rejected before any write");
    static struct OutputWithMetadata { u8 bytes[4096]; DeflateInputSpan span; } shared{};
    shared.span = bound;
    AliasedGrowth overlap{shared.bytes, 8192};
    view = {shared.bytes, sizeof(shared.bytes)};
    zlib_stream_initialize(&stream);
    result = zlib_stream_inflate_flush_growing(&stream, {&shared.span, 1}, &view, 8192, overlapping_growth, &overlap);
    check(result.error == ZlibInflateError::output_capacity && stream.failed && view.count == 4096 &&
        shared.span.data == bound.data && shared.span.bytes == bound.bytes,
        "inflate growth: same-base expansion across compressed span metadata rejected before overwrite");
    static u8 shared_input[16384];
    copy_bytes(shared_input + 4096, kGatewayGrowthBound, sizeof(kGatewayGrowthBound));
    const DeflateInputSpan borrowed{shared_input + 4096, sizeof(kGatewayGrowthBound)};
    overlap = {shared_input, 8192};
    view = {shared_input, 4096};
    zlib_stream_initialize(&stream);
    result = zlib_stream_inflate_flush_growing(&stream, {&borrowed, 1}, &view, 8192, overlapping_growth, &overlap);
    check(result.error == ZlibInflateError::output_capacity && stream.failed && view.count == 4096 &&
        bytes_equal(shared_input + 4096, kGatewayGrowthBound, sizeof(kGatewayGrowthBound)),
        "inflate growth: same-base expansion into compressed bytes rejected before input corruption");
    zlib_stream_clear(&stream);
}
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
    stream_growth(check);
    test_deflate_differential(check);
}
} // namespace maicord
