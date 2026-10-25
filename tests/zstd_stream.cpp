// zstd-stream decoding: reference-encoder streams (one frame across many
// flushed messages, as the Gateway sends them), hand-built blocks for the
// modes the reference encoder rarely picks, malformed input and the window.
#include "maicord/media/zstd.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

#include "zstd_stream_fixtures.inc"

u32 checks = 0;
bool print_ok = true;
void print(const char* text) noexcept {
    u32 n = 0;
    while (text[n])
        ++n;
    DWORD written = 0;
    print_ok = print_ok && WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, n, &written, nullptr) && written == n;
}
void number(u64 v) noexcept {
    char digits[21];
    u32 n = 0;
    do {
        digits[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        const char text[]{digits[--n], 0};
        print(text);
    }
}
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            print("FAIL line=");                                                                                       \
            number(__LINE__);                                                                                          \
            print("\n");                                                                                               \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

// The text generator of tools/generate_zstd_stream_fixtures.py.
const char* const kWords[] = {"alpha", "stream", "window", "block", "literal", "offset", "match", "sequence",
                              "\"id\":\"", "\",\"name\":\"", "{\"op\":0,\"d\":{", "}}", "guild", "channel",
                              "message", "\\u00e9", "\n", " ", "0", "[", "]", ",", ":", "true", "null"};
constexpr u32 kWordCount = sizeof(kWords) / sizeof(kWords[0]);

struct Out {
    u8* data;
    u32 size, used;
    void add(const u8* p, u32 n) noexcept {
        for (u32 i = 0; i < n && used < size; ++i)
            data[used++] = p[i];
    }
    void add(const char* p) noexcept {
        while (*p && used < size)
            data[used++] = static_cast<u8>(*p++);
    }
};

void fixture_text(u32 seed, u32 size, u32 mode, u8* output) noexcept {
    if (mode == 5) {
        fixture_text(seed, size < 3000 ? size : 3000, 0, output);
        for (u32 i = 3000; i < size; ++i)
            output[i] = output[i - 3000];
        return;
    }
    u64 state = seed * 0x9E3779B97F4A7C15ull + 1;
    Out out{output, size, 0};
    while (out.used < size) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const u64 r = state * 2685821657736338717ull;
        if (mode == 1) {
            const char* word = kWords[(r >> 40) % 4];
            for (u64 k = 0; k < 1 + (r >> 20) % 12; ++k)
                out.add(word);
        } else if (mode == 2) {
            u8 bytes[7];
            for (u32 i = 0; i < 7; ++i)
                bytes[i] = static_cast<u8>(r >> (8 + 8 * i));
            out.add(bytes, 7);
        } else if (mode == 3) {
            for (u32 i = 0; i < 64; ++i)
                out.add("z");
        } else if (mode == 4) {
            const u8 bytes[]{static_cast<u8>(97 + (r >> 60) % 5), static_cast<u8>(97 + (r >> 50) % 3)};
            out.add(bytes, 2);
        } else if (mode == 6) {
            const u8 bytes[]{static_cast<u8>(97 + (r >> 59) % 26), static_cast<u8>(97 + (r >> 40) % 26),
                             static_cast<u8>((r >> 30) % 5 == 0 ? 32 : 97 + (r >> 20) % 26)};
            out.add(bytes, 3);
        } else {
            const u64 pick = (r >> 32) % (kWordCount + 3);
            if (pick < kWordCount) {
                out.add(kWords[pick]);
            } else {
                char digits[21];
                u32 n = 0;
                u64 v = r % 1000003;
                do {
                    digits[n++] = static_cast<char>('0' + v % 10);
                    v /= 10;
                } while (v);
                while (n)
                    out.add(reinterpret_cast<const u8*>(&digits[--n]), 1);
            }
        }
    }
}

// Inputs end at a no-access page, so any read past a message faults.
constexpr u32 kPage = 4096;
constexpr u32 kGuardedBytes = 16 * kPage;
u8* guarded;
const u8* place(const u8* data, u32 bytes) noexcept {
    u8* at = guarded + kGuardedBytes - bytes;
    copy_bytes(at, data, bytes);
    return at;
}

constexpr u32 kStorageReserve = 4u << 20;
u8* storage;
u32 storage_committed;
u32 grow_calls;
bool grow(void*, u32 required, Slice<u8>* backed) noexcept {
    if (required > kStorageReserve)
        return false;
    ++grow_calls;
    storage_committed = required > storage_committed ? required : storage_committed;
    *backed = {storage, storage_committed};
    return true;
}

u8 expected[320 * 1024];
ZstdStreamDecoder decoder;

ZstdDecodeResult decode(const u8* data, u32 bytes, ConstSlice<u8>* output, u32 maximum = 1u << 20) noexcept {
    return zstd_stream_decompress(&decoder, {place(data, bytes), bytes}, maximum, output);
}

bool begin(u32 initial, bool growable) noexcept {
    storage_committed = initial;
    return zstd_stream_initialize(&decoder, {storage, initial}, growable ? kStorageReserve : initial,
                                  growable ? grow : nullptr, nullptr);
}

template <u32 N> bool run_case(const FixtureMessage (&messages)[N], u32 initial, bool growable) noexcept {
    CHECK(begin(initial, growable));
    for (const FixtureMessage& m : messages) {
        ConstSlice<u8> output{};
        const auto result = decode(m.data, m.bytes, &output);
        CHECK(result.ok());
        CHECK(result.output_bytes == m.size && output.count == m.size);
        fixture_text(m.seed, m.size, m.mode, expected);
        CHECK(bytes_equal(output.data, expected, m.size));
        CHECK(output.data >= storage && output.data + output.count <= storage + decoder.storage.count);
    }
    return true;
}

bool reference_streams() noexcept {
    // Ample storage, storage that has to grow, and fixed storage that slides
    // the window to the front between messages.
    const u32 room = 1u << 20;
    CHECK(run_case(k_gateway_like, room, false));
    CHECK(decoder.window_bytes == 64 * 1024 && decoder.in_frame);
    grow_calls = 0;
    CHECK(run_case(k_gateway_like, kZstdMessageSlackBytes, true));
    CHECK(grow_calls != 0);
    CHECK(run_case(k_gateway_like, kZstdMessageSlackBytes + 150 * 1024, false));
    CHECK(run_case(k_small_window_checksum, room, false));
    CHECK(decoder.window_bytes == 1024 && decoder.has_checksum);
    CHECK(run_case(k_small_window_checksum, kZstdMessageSlackBytes, false));
    CHECK(run_case(k_fast_raw, room, false));
    CHECK(run_case(k_runs, room, false));
    CHECK(run_case(k_large_window, room, false));
    CHECK(decoder.window_bytes == 1024 * 1024);
    // Each message is a whole frame with a checksum; the last is empty.
    CHECK(run_case(k_frames, room, false));
    CHECK(!decoder.in_frame);

    // A single-segment frame: its content size is the window.
    CHECK(begin(room, false));
    ConstSlice<u8> output{};
    auto result = decode(k_single_frame, sizeof(k_single_frame), &output);
    CHECK(result.ok() && output.count == 1200 && decoder.window_bytes == 1200 && !decoder.in_frame);
    fixture_text(51, 1200, 0, expected);
    CHECK(bytes_equal(output.data, expected, 1200));

    // Two whole frames and a skippable frame in one message.
    u8 joined[2048];
    u32 at = 0;
    const u8 skippable[]{0x5e, 0x2a, 0x4d, 0x18, 3, 0, 0, 0, 1, 2, 3};
    copy_bytes(joined, skippable, sizeof(skippable));
    at += sizeof(skippable);
    copy_bytes(joined + at, k_frames_1, sizeof(k_frames_1));
    at += sizeof(k_frames_1);
    copy_bytes(joined + at, k_single_frame, sizeof(k_single_frame));
    at += sizeof(k_single_frame);
    CHECK(begin(room, false));
    result = decode(joined, at, &output);
    CHECK(result.ok() && output.count == 400 + 1200);
    fixture_text(32, 400, 0, expected);
    fixture_text(51, 1200, 0, expected + 400);
    CHECK(bytes_equal(output.data, expected, 1600));
    return true;
}

// Frame header, window 1 KiB, no checksum; blocks follow.
constexpr u8 kHeader[]{0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x00};
u32 framed(u8* out, const u8* blocks, u32 bytes) noexcept {
    copy_bytes(out, kHeader, sizeof(kHeader));
    copy_bytes(out + sizeof(kHeader), blocks, bytes);
    return static_cast<u32>(sizeof(kHeader)) + bytes;
}
void block_header(u8* out, bool last, u32 type, u32 size) noexcept {
    const u32 v = (last ? 1u : 0u) | type << 1 | size << 3;
    out[0] = static_cast<u8>(v);
    out[1] = static_cast<u8>(v >> 8);
    out[2] = static_cast<u8>(v >> 16);
}

bool handmade_blocks() noexcept {
    u8 message[256];
    u8 blocks[200];
    ConstSlice<u8> output{};
    // Raw block, RLE block, then a compressed block with RLE literals and no sequences.
    u32 n = 0;
    block_header(blocks + n, false, 0, 3);
    blocks[n + 3] = 'r';
    blocks[n + 4] = 'a';
    blocks[n + 5] = 'w';
    n += 6;
    block_header(blocks + n, false, 1, 5);
    blocks[n + 3] = '-';
    n += 4;
    block_header(blocks + n, false, 2, 3);
    blocks[n + 3] = (4 << 3) | 1; // RLE literals, 4 of them
    blocks[n + 4] = 'q';
    blocks[n + 5] = 0; // no sequences
    n += 6;
    CHECK(begin(1u << 20, false));
    auto result = decode(message, framed(message, blocks, n), &output);
    CHECK(result.ok() && result.blocks == 3 && output.count == 12 && bytes_equal(output.data, "raw-----qqqq", 12));

    // Huffman literals with directly stored weights ('a' weight 1, 'b' implied):
    // 1-bit codes, a = 0, b = 1; then a treeless block reusing the table.
    n = 0;
    const u32 tree = 1 + 49;
    block_header(blocks + n, false, 2, 3 + tree + 1 + 1);
    u32 v = 2 | 4u << 4 | (tree + 1) << 14;
    blocks[n + 3] = static_cast<u8>(v);
    blocks[n + 4] = static_cast<u8>(v >> 8);
    blocks[n + 5] = static_cast<u8>(v >> 16);
    blocks[n + 6] = 127 + 98; // 98 weights stored, for symbols 0-97
    for (u32 i = 0; i < 49; ++i)
        blocks[n + 7 + i] = i == 48 ? 0x01 : 0x00; // symbol 97 ('a') in the low nibble of the last byte
    blocks[n + 7 + 49] = 0x16; // marker, then 0110: "abba"
    blocks[n + 7 + 50] = 0;
    n += 3 + 3 + tree + 1 + 1;
    block_header(blocks + n, true, 2, 3 + 1 + 1);
    v = 3 | 4u << 4 | 1u << 14;
    blocks[n + 3] = static_cast<u8>(v);
    blocks[n + 4] = static_cast<u8>(v >> 8);
    blocks[n + 5] = static_cast<u8>(v >> 16);
    blocks[n + 6] = 0x19; // marker, then 1001: "baab"
    blocks[n + 7] = 0;
    n += 3 + 5;
    CHECK(begin(1u << 20, false));
    result = decode(message, framed(message, blocks, n), &output);
    CHECK(result.ok() && output.count == 8 && bytes_equal(output.data, "abbabaab", 8) && !decoder.in_frame);

    // One sequence with RLE tables: 3 literals, then offset 3 (offset code 2,
    // extra bits 10) for 4 bytes.
    n = 0;
    const u8 sequence_block[]{0x18, 'a', 'b', 'c', 0x01, 0x54, 3, 2, 1, 0x06};
    block_header(blocks, true, 2, sizeof(sequence_block));
    copy_bytes(blocks + 3, sequence_block, sizeof(sequence_block));
    n = 3 + sizeof(sequence_block);
    CHECK(begin(1u << 20, false));
    result = decode(message, framed(message, blocks, n), &output);
    CHECK(result.ok() && output.count == 7 && bytes_equal(output.data, "abcabca", 7));
    CHECK(decoder.repeat[0] == 3 && decoder.repeat[1] == 1 && decoder.repeat[2] == 4);

    // The same sequence with offset 4 reaches before the frame.
    u8 bad[sizeof(sequence_block)];
    copy_bytes(bad, sequence_block, sizeof(bad));
    bad[9] = 0x07; // extra bits 11: offset value 7, offset 4
    bad[1] = 'a';
    block_header(blocks, true, 2, sizeof(bad));
    copy_bytes(blocks + 3, bad, sizeof(bad));
    CHECK(begin(1u << 20, false));
    result = decode(message, framed(message, blocks, 3 + sizeof(bad)), &output);
    CHECK(result.error == ZstdError::invalid_sequences && decoder.failed);
    // A poisoned stream refuses further input.
    result = decode(message, framed(message, blocks, 3 + sizeof(sequence_block)), &output);
    CHECK(result.error == ZstdError::poisoned);
    return true;
}

bool malformed_input() noexcept {
    ConstSlice<u8> output{};
    const struct {
        u8 bytes[16];
        u32 count;
        ZstdError error;
    } headers[] = {
        {{0x28, 0xb5, 0x2f, 0xfe, 0x00, 0x30}, 6, ZstdError::bad_magic},
        {{0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x70}, 6, ZstdError::window_too_large},    // windowLog 24
        {{0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x69}, 6, ZstdError::window_too_large},    // 8 MiB + 1/8
        {{0x28, 0xb5, 0x2f, 0xfd, 0x08, 0x30}, 6, ZstdError::invalid_frame_header}, // reserved bit
        {{0x28, 0xb5, 0x2f, 0xfd, 0x01, 0x30, 0x05}, 7, ZstdError::unsupported_dictionary},
        {{0x28, 0xb5, 0x2f, 0xfd, 0x00}, 5, ZstdError::truncated},
        {{0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x30, 0x07, 0x00, 0x00}, 9, ZstdError::invalid_block}, // reserved type
        {{0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x30, 0x20, 0x00, 0x00, 'a'}, 10, ZstdError::truncated},
        {{0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x00, 0x00, 0x21, 0x00}, 9, ZstdError::invalid_block}, // > window
        {{0x5a, 0x2a, 0x4d, 0x18, 9, 0, 0, 0, 1}, 9, ZstdError::truncated},
    };
    for (const auto& h : headers) {
        CHECK(begin(1u << 20, false));
        CHECK(decode(h.bytes, h.count, &output).error == h.error);
    }
    // The 8 MiB window itself is accepted.
    CHECK(begin(1u << 20, false));
    const u8 largest[]{0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x68, 0x01, 0x00, 0x00};
    CHECK(decode(largest, sizeof(largest), &output).ok() && decoder.window_bytes == kZstdMaximumWindowBytes);

    // A message cut inside a block, and a frame checksum that does not match.
    CHECK(begin(1u << 20, false));
    CHECK(decode(k_gateway_like_0, sizeof(k_gateway_like_0) - 1, &output).error == ZstdError::truncated);
    u8 copy[sizeof(k_frames_1)];
    copy_bytes(copy, k_frames_1, sizeof(copy));
    copy[sizeof(copy) - 1] ^= 1;
    CHECK(begin(1u << 20, false));
    CHECK(decode(copy, sizeof(copy), &output).error == ZstdError::checksum);
    // The output bound covers the whole message.
    CHECK(begin(1u << 20, false));
    CHECK(decode(k_gateway_like_0, sizeof(k_gateway_like_0), &output, 299).error == ZstdError::output_capacity);
    // Compaction keeps the window; the storage keeps a message's room past it.
    CHECK(begin(kZstdMessageSlackBytes, true));
    CHECK(decode(k_gateway_like_0, sizeof(k_gateway_like_0), &output).ok());
    CHECK(zstd_stream_compact(&decoder) == 300 && decoder.position == 300);
    CHECK(!zstd_stream_set_storage(&decoder, {storage, kZstdMessageSlackBytes}));
    CHECK(zstd_stream_set_storage(&decoder, {storage, kZstdMessageSlackBytes + 300}));

    // Random corruption of a real stream never reads outside its input or
    // writes outside the storage; most of it is detected.
    u8 mutated[4096];
    u64 rng = 0x2545F4914F6CDD1Dull;
    u32 detected = 0;
    constexpr u32 kRounds = 1500;
    for (u32 round = 0; round < kRounds; ++round) {
        CHECK(begin(kZstdMessageSlackBytes, true));
        bool failed = false;
        for (u32 m = 0; m < 4 && !failed; ++m) {
            const FixtureMessage& message = k_gateway_like[m];
            copy_bytes(mutated, message.data, message.bytes);
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            if ((rng & 3) == static_cast<u64>(m & 3)) {
                const u32 flips = 1 + (rng >> 8) % 3;
                for (u32 f = 0; f < flips; ++f)
                    mutated[(rng >> (16 + 10 * f)) % message.bytes] ^= static_cast<u8>(1u << ((rng >> (40 + f)) & 7));
            }
            const auto r = decode(mutated, message.bytes, &output);
            failed = !r.ok();
            if (!failed)
                CHECK(output.data >= storage && output.data + output.count <= storage + storage_committed);
        }
        detected += failed;
    }
    CHECK(detected > kRounds / 4);
    zstd_stream_clear(&decoder);
    CHECK(bytes_zero(storage, storage_committed) && !decoder.initialized);
    return true;
}

bool run() noexcept {
    guarded = static_cast<u8*>(VirtualAlloc(nullptr, kGuardedBytes + kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    storage = static_cast<u8*>(VirtualAlloc(nullptr, kStorageReserve, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD old = 0;
    CHECK(guarded && storage && VirtualProtect(guarded + kGuardedBytes, kPage, PAGE_NOACCESS, &old));
    CHECK(reference_streams());
    CHECK(handmade_blocks());
    CHECK(malformed_input());
    return true;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_zstd_stream_entry() noexcept {
    using namespace maicord;
    const bool ok = run();
    print("checks=");
    number(checks);
    print(ok ? " PASS zstd-stream decoding\n" : " FAIL zstd-stream decoding\n");
    ExitProcess(ok && print_ok ? 0 : 1);
}
