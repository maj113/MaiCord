#pragma once

#include "maicord/protocol/encoding/writer.hpp"

namespace maicord::perf {
namespace perf_account_detail {

// Memory
struct Region {
    u8* base;
    u64 reserved;
    u64 used;
    u64 committed;
};

extern Region g_region;

void* region_take(u64 bytes) noexcept;

// JSON text
struct Json {
    ProtocolWriter writer;
    bool ok = true;
    Json(char* data, u32 capacity) noexcept : writer{{reinterpret_cast<u8*>(data), capacity}, 0} {}
    Json& s(const char* text) noexcept {
        ok = ok && protocol_writer_append_c_string(&writer, text);
        return *this;
    }
    Json& n(u64 value) noexcept {
        ok = ok && protocol_writer_append_u64(&writer, value);
        return *this;
    }
    Json& id(u64 value) noexcept { return s("\"").n(value).s("\""); }
    Json& c(u32 index) noexcept { return index ? s(",") : *this; }
    Json& hex(u64 value, u32 digits) noexcept {
        char text[17]{};
        for (u32 i = 0; i < digits && i < 16; ++i)
            text[digits - 1 - i] = "0123456789abcdef"[(value >> (i * 4)) & 15u];
        return s(text);
    }
    u32 used() const noexcept { return writer.used; }
};

// 2026-10-11T06:00:00Z: the newest synthetic message.
constexpr u64 kLatestMessageMs = 1'792'994'400'000ull;
constexpr u64 kSnowflakeEpochMs = 1'420'070'400'000ull;
constexpr u32 kMessagesPerChannel = 4'000;

u64 message_id(u64 channel, u32 index) noexcept;

u32 message_index_at(u64 id) noexcept;

void append_user(Json& j, u32 user) noexcept;

void append_activity(Json& j, u32 user) noexcept;

u32 build_ready(char* output, u32 capacity) noexcept;

void append_message(Json& j, u64 guild, u64 channel, u32 index, bool reference_depth) noexcept;

} // namespace perf_account_detail

using namespace perf_account_detail;
} // namespace maicord::perf
