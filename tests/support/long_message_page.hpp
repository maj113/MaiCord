#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// The sizes of a generated message: characters of content and of the embed's title and description, and attachments.
struct LongMessageShape {
    u32 content;
    u32 attachments;
    u32 title;
    u32 description;
    bool pinned;
};
// Pins of 1,500 characters, four attachments and a long embed.
inline constexpr LongMessageShape kLongPinnedMessage{1500, 4, 200, 2500, true};
// Discord's limits: 4,000 characters of content, 10 attachments, and an embed of 5,913 characters (title 256,
// description 4,096, ten fields and the footer) of the 6,000 an embed may hold.
inline constexpr LongMessageShape kMaximalMessage{4000, 10, 256, 4096, false};

// Writes messages as GET /channels/{c}/messages and its pins send them into `data`, which the caller sizes. Each
// message has three mentions, a rich embed with ten fields and five buttons, and can reply to one like it.
struct LongMessageWriter {
    char* data;
    u32 used;
    LongMessageShape shape;
    u64 channel_id;
    void put(const char* text) noexcept { while (*text) data[used++] = *text++; }
    void run(char c, u32 count) noexcept { while (count--) data[used++] = c; }
    void id(u64 value) noexcept {
        char digits[20];u32 count = 0;
        do { digits[count++] = static_cast<char>('0' + value % 10);value /= 10; } while (value);
        while (count) data[used++] = digits[--count];
    }
    void user(u64 id_value) noexcept {
        put(R"({"id":")");id(id_value);
        put(R"(","username":"alien","avatar":"14733482e560d9267c0a414b21b2fb8d","discriminator":"0",)"
            R"("public_flags":64,"flags":64,"banner":null,"accent_color":null,"global_name":"Alien",)"
            R"("avatar_decoration_data":null,"collectibles":null,"display_name_styles":null,"banner_color":null,)"
            R"("clan":null,"primary_guild":null})");
    }
    void message(u64 message_id, u64 replied) noexcept {
        put(R"({"type":)");put(replied ? "19" : "0");put(R"(,"content":")");run('x', shape.content);
        put(R"(","mentions":[)");
        for (u32 k = 0; k < 3; ++k) { if (k) put(",");user(message_id * 4 + k); }
        put(R"(],"mention_roles":[],"attachments":[)");
        for (u32 k = 0; k < shape.attachments; ++k) {
            if (k) put(",");
            put(R"({"id":")");id(message_id * 16 + k);
            put(R"(","filename":"screenshot.png","size":123456,"url":"https://cdn.discordapp.com/attachments/70/1/)"
                R"(screenshot.png?ex=6700a1b2&is=66ff5032&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123)"
                R"(456789abcdef&","proxy_url":"https://media.discordapp.net/attachments/70/1/screenshot.png?ex=6700a)"
                R"(1b2&is=66ff5032&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef&","width":19)"
                R"(20,"height":1080,"content_type":"image/png","placeholder":"abcdefghijklmnopqrstuvwxyz",)"
                R"("placeholder_version":1})");
        }
        put(R"(],"embeds":[{"type":"rich","title":")");run('T', shape.title);put(R"(","description":")");
        run('d', shape.description);
        put(R"(","color":3447003,"fields":[)");
        for (u32 k = 0; k < 10; ++k) {
            if (k) put(",");
            put(R"({"name":"Field","value":")");run('v', 150);put(R"(","inline":true})");
        }
        put(R"(],"footer":{"text":"footer text","icon_url":"https://cdn.discordapp.com/x.png"}}],)"
            R"("timestamp":"2026-10-05T12:00:00.000000+00:00","edited_timestamp":null,"flags":0,"components":[)"
            R"({"type":1,"id":1,"components":[)");
        for (u32 k = 0; k < 5; ++k) {
            if (k) put(",");
            put(R"({"type":2,"id":)");id(2 + k);put(R"(,"style":1,"label":"Button","custom_id":"button_)");id(k);
            put(R"("})");
        }
        put(R"(]}],"id":")");id(message_id);put(R"(","channel_id":")");id(channel_id);put(R"(","author":)");user(3);
        put(shape.pinned ? R"(,"pinned":true)" : R"(,"pinned":false)");
        put(R"(,"mention_everyone":false,"tts":false)");
        if (replied) {
            put(R"(,"message_reference":{"type":0,"channel_id":")");id(channel_id);put(R"(","message_id":")");
            id(replied);
            put(R"(","guild_id":"1"},"referenced_message":)");message(replied, 0);
        }
        put("}");
    }
};

} // namespace maicord
