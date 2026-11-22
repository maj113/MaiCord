// A big bot's application command index, synthetic and without account data: its 100 global slash commands
// (Discord's limit for one app), each with its name and description in Discord's 31 locales and one option of 25
// choices. kLargeCommandIndexBytes of JSON with 80,048 structurals: past the 256 KiB response a REST client is
// configured for and past 64 Ki structurals.
#pragma once
#include "maicord/protocol/encoding/writer.hpp"

namespace maicord {
inline constexpr u32 kLargeCommandIndexBytes = 299'173;
// The bytes written, 0 when output is short.
inline u32 write_large_command_index(Slice<u8> output) noexcept {
    static constexpr const char* locales[]{"da", "de",    "en-GB", "en-US", "es-ES", "es-419", "fr", "hr",
                                           "it", "lt",    "hu",    "nl",    "no",    "pl",     "pt-BR", "ro",
                                           "fi", "sv-SE", "vi",    "tr",    "cs",    "el",     "bg", "ru",
                                           "uk", "hi",    "th",    "zh-CN", "ja",    "zh-TW",  "ko"};
    ProtocolWriter w{output, 0};
    bool ok = protocol_write(
        &w, R"({"applications":[{"id":"10","name":"Music","bot":{"id":"11","username":"music"}}],"application_commands":[)");
    for (u32 i = 0; ok && i < 100; ++i) {
        ok = protocol_write(&w, i ? ",{\"id\":\"" : "{\"id\":\"", 1000 + i, R"(","application_id":"10","version":")",
                            i + 1, R"(","type":1,"name":"play)", i, R"(","name_localizations":{)");
        for (u32 l = 0; ok && l < 31; ++l)
            ok = protocol_write(&w, l ? ",\"" : "\"", locales[l], "\":\"play", i, '-', locales[l], '"');
        ok = ok && protocol_write(&w, R"(},"description":"Plays track )", i,
                                  R"( of the queue","description_localizations":{)");
        for (u32 l = 0; ok && l < 31; ++l)
            ok = protocol_write(&w, l ? ",\"" : "\"", locales[l], "\":\"Plays track ", i, " of the queue (", locales[l],
                                ")\"");
        ok = ok && protocol_write(
                       &w, R"(},"options":[{"type":3,"name":"source","description":"Where to play from","required":true,"choices":[)");
        for (u32 k = 0; ok && k < 25; ++k)
            ok = protocol_write(&w, k ? ",{\"name\":\"Source " : "{\"name\":\"Source ", k, "\",\"value\":\"s", k, "\"}");
        ok = ok && protocol_write(&w, "]}]}");
    }
    return ok && protocol_write(&w, R"(],"version":"1"})") ? w.used : 0;
}
} // namespace maicord
