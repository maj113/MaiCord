#include "maicord/media/deflate.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/writer.hpp"

#include <Windows.h>

namespace maicord {
namespace {
int run() noexcept {
    wchar_t path[1024]{};
    const DWORD n = GetEnvironmentVariableW(L"MAICORD_COMPRESSION_CORPUS", path, 1024);
    if (!n || n >= 1024)
        return 2;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    if (file == INVALID_HANDLE_VALUE)
        return 3;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 128 * 1024 * 1024) {
        CloseHandle(file);
        return 4;
    }
    auto* bytes = static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(size.QuadPart),
                                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* output =
        static_cast<u8*>(VirtualAlloc(nullptr, 2 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD got = 0;
    const bool loaded = bytes && output &&
                        ReadFile(file, bytes, static_cast<DWORD>(size.QuadPart), &got, nullptr) &&
                        got == size.QuadPart;
    CloseHandle(file);
    if (!loaded)
        return 5;
    const Clock clock = clock_initialize();
    u32 offset = 0, index = 0;
    DeflateWorkspace workspace{};
    while (got - offset >= 12) {
        u32 header[3]{};
        copy_bytes(header, bytes + offset, 12);
        offset += 12;
        const u32 raw_size = header[0], encoded_size = header[1], kind = header[2];
        if (raw_size > 2 * 1024 * 1024 || raw_size > got - offset || encoded_size > got - offset - raw_size ||
            kind > 1)
            return 6;
        const auto* expected = bytes + offset;
        const auto* encoded = expected + raw_size;
        const DeflateInputSpan span{encoded, encoded_size};
        const auto decode = [&]() noexcept {
            return kind ? zlib_inflate({&span, 1}, {output, 2 * 1024 * 1024}, ~0u, &workspace)
                        : gzip_inflate({encoded, encoded_size}, {output, 2 * 1024 * 1024}, &workspace);
        };
        const auto first = decode();
        if (!first.ok() || first.output_bytes != raw_size || !bytes_equal(expected, output, raw_size))
            return 7;
        const i64 begin = clock_now();
        constexpr u32 iterations = 40;
        for (u32 pass = 0; pass < iterations; ++pass)
            if (!decode().ok())
                return 8;
        const u64 elapsed = clock_microseconds(clock, begin, clock_now());
        u8 line[256]{};
        ProtocolWriter w{{line, sizeof(line)}, 0};
        protocol_writer_append_literal(&w, "{\"index\":");
        protocol_writer_append_u64(&w, index++);
        protocol_writer_append_literal(&w, ",\"raw_bytes\":");
        protocol_writer_append_u64(&w, raw_size);
        protocol_writer_append_literal(&w, ",\"encoded_bytes\":");
        protocol_writer_append_u64(&w, encoded_size);
        protocol_writer_append_literal(&w, ",\"kind\":");
        protocol_writer_append_u64(&w, kind);
        protocol_writer_append_literal(&w, ",\"iterations\":40,\"elapsed_us\":");
        protocol_writer_append_u64(&w, elapsed);
        protocol_writer_append_literal(&w, "}\n");
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, w.used, &written, nullptr);
        offset += raw_size + encoded_size;
    }
    VirtualFree(bytes, 0, MEM_RELEASE);
    VirtualFree(output, 0, MEM_RELEASE);
    return offset == got ? 0 : 9;
}
} // namespace
} // namespace maicord
extern "C" void maicord_http_compression_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
