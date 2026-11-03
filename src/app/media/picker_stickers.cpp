#include "picker_internal.hpp"
#include "../settings/emoji_preferences.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/protocol/user/emoji_preferences.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "maicord/model/chat/asset_hash.hpp"

namespace maicord::app_detail {
namespace picker_detail {

// One bank of every server's stickers usable here: this server first, then
// the server list order. Locked (Nitro) and unavailable ones stay visible
// but disabled; stickers this channel cannot use at all are left out.
bool build_server_stickers(ApplicationMediaPicker& p) noexcept {
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, p.channel_id);
    const u64 channel_guild = channel ? channel->guild_id : 0;
    const bool external = channel && channel->permissions_resolved &&
                          permission_has(channel->effective_permissions, kPermissionUseExternalStickers);
    u64 order[kStickerServerCapacity];
    u32 servers = 0;
    const auto stocked = [](u64 guild_id) noexcept {
        const auto* list = sticker_list(guild_id);
        return list && list->json;
    };
    if (stocked(p.guild_id))
        order[servers++] = p.guild_id;
    for (u32 i = 0; i < state.guild_count && servers < kStickerServerCapacity; ++i) {
        const auto& guild = state.guilds[state.guild_order[i]];
        if (!guild.unavailable && guild.id != p.guild_id && stocked(guild.id))
            order[servers++] = guild.id;
    }
    p.sticker_store_revision = g_sticker_store.revision;
    auto* scratch =
        servers ? static_cast<u32*>(commit_pages(kMediaCatalogStructuralCapacity * sizeof(u32)))
                : nullptr;
    if (servers && !scratch)
        return false;
    constexpr u32 kNameBytes = 128;
    u32 items = 0, text = 0;
    for (u32 i = 0; i < servers; ++i) {
        const auto* list = sticker_list(order[i]);
        const auto measured = media_catalog_measure(MediaCatalogOperation::guild_stickers, {list->json, list->bytes},
                                                    {scratch, kMediaCatalogStructuralCapacity}, 32);
        if (!measured.ok() || measured.truncated || measured.item_count > kUiMediaPickerMaximumItems - items) {
            order[i] = 0;
            continue;
        }
        items += measured.item_count;
        text += measured.text_bytes;
    }
    const u32 bytes = sizeof(CatalogBank) + items * sizeof(MediaCatalogItem) + text + items * 64 +
                      servers * kNameBytes + (items + 7) / 8;
    auto* bank = items ? static_cast<CatalogBank*>(
                             commit_pages(bytes))
                       : nullptr;
    p.sticker_server_count = 0;
    if (bank) {
        bank->items = reinterpret_cast<MediaCatalogItem*>(bank + 1);
        bank->text = reinterpret_cast<char*>(bank->items + items);
        char* const names = bank->text + text + items * 64;
        bank->preview_mask = reinterpret_cast<u8*>(names + servers * kNameBytes);
        u32 used = 0, text_used = 0;
        const u8 premium = state.self_premium_type;
        for (u32 i = 0; i < servers; ++i) {
            const auto* list = order[i] ? sticker_list(order[i]) : nullptr;
            if (!list)
                continue;
            const auto decoded = media_catalog_decode(
                MediaCatalogOperation::guild_stickers, {list->json, list->bytes},
                {scratch, kMediaCatalogStructuralCapacity}, 32, {bank->items + used, items - used},
                {bank->text + text_used, text - text_used});
            if (!decoded.ok() || decoded.truncated)
                continue;
            text_used += decoded.text_bytes;
            const u32 first = used;
            for (u32 k = 0; k < decoded.item_count; ++k) {
                const auto& source = bank->items[first + k];
                const auto sendable =
                    guild_sticker_sendability(order[i], channel_guild, source.available, external, premium);
                if (sendable == StickerSendability::hidden)
                    continue;
                auto& item = bank->items[used++];
                if (&item != &source)
                    item = source;
                item.guild_id = order[i];
                item.available = sendable == StickerSendability::sendable;
            }
            if (used == first)
                continue;
            const auto* guild = chat_state_store_find_guild(state, order[i]);
            auto& server = p.sticker_servers[p.sticker_server_count];
            server = {order[i], utf8_literal("Server stickers"), 0, false};
            if (guild) {
                char* const label = names + p.sticker_server_count * kNameBytes;
                const u32 label_bytes =
                    utf8_copy_truncated(label, kNameBytes, chat_state_store_guild_name(state, *guild));
                if (label_bytes)
                    server.label = {label, label_bytes};
                server.icon_variant = guild->icon_variant;
                server.animated = asset_hash_animated(chat_state_store_guild_icon_hash(*guild));
            }
            ++p.sticker_server_count;
        }
        bank->count = used;
        if (!sticker_previews(bank, bank->text + text, items * 64)) {
            VirtualFree(bank, 0, MEM_RELEASE);
            bank = nullptr;
            p.sticker_server_count = 0;
        } else {
            bank->text_bytes = text + items * 64 + servers * kNameBytes;
            bank->generation = p.generation;
            bank->operation = MediaCatalogOperation::guild_stickers;
            bank->first_id = g_app.media_picker_item_serial + 1;
            g_app.media_picker_item_serial += used;
        }
    }
    if (scratch)
        VirtualFree(scratch, 0, MEM_RELEASE);
    revoke_capture(p);
    ui_media_picker_scene_close(&p.scene);
    if (p.guild)
        VirtualFree(p.guild, 0, MEM_RELEASE);
    p.guild = bank;
    ++p.source.revision;
    return true;
}
// A server sticker's server name, else its pack's name: the inline row's
// trailing column.
Utf8View sticker_source(const ApplicationMediaPicker& p, const MediaCatalogItem& item) noexcept {
    if (!item.guild_id)
        return item.pack_name;
    for (u32 s = 0; s < p.sticker_server_count; ++s)
        if (p.sticker_servers[s].id == item.guild_id)
            return p.sticker_servers[s].label;
    return {};
}

} // namespace picker_detail
namespace {

// The /sticker results: every
// sendable sticker scores its best field, a field's weight (name 11, server or
// pack name 8, a server sticker's correlated emoji 6 and tag 1) times 10 when
// it equals the query, 7 when it starts with it, or 5 when a name contains it.
// The correlated emoji is the tag's emoji and its skin-tone variants
// (guild sticker metadata). A sticker with synced frecency then weighs by its
// score / 100. The eight best show, equal scores in catalog order (server
// stickers, then packs). A sticker found by another field than its name notes
// that field in lowercase. Not kept: this session's unsaved sticker uses.
constexpr u32 kStickerFrecencyCapacity = 256;
struct StickerFrecency {
    u64 id[kStickerFrecencyCapacity];
    u32 score[kStickerFrecencyCapacity];
    u32 count;
    u64 now_ms;
};
bool sticker_frecency_entry(void* context, Utf8View key, const EmojiFrecencyItem& item) noexcept {
    auto& table = *static_cast<StickerFrecency*>(context);
    u64 id = 0;
    if (!snowflake_parse(key, &id))
        return true;
    u32 score = 0;
    for (u32 i = 0; i < item.recent_count; ++i)
        if (item.recent_uses[i])
            score += frecency_age_weight(table.now_ms, item.recent_uses[i]);
    u32 at = 0;
    while (at < table.count && table.id[at] != id)
        ++at;
    // The last duplicate is authoritative; past capacity an entry is unweighted.
    if (at == table.count) {
        if (table.count == kStickerFrecencyCapacity)
            return true;
        ++table.count;
    }
    table.id[at] = id;
    table.score[at] = score;
    return true;
}

} // namespace
namespace picker_detail {

bool inline_stickers(ApplicationMediaPicker& p, u32 count, Utf8View query) noexcept {
    u32 scores[kUiInlineResultsStickerLimit]{};
    Utf8View matches[kUiInlineResultsStickerLimit]{};
    u32 shown = 0;
    StickerFrecency frecency;
    frecency.count = 0;
    frecency.now_ms = clock_unix_milliseconds();
    if (query.bytes)
        emoji_preferences_visit_frecency(application_emoji_preferences_sticker_frecency(), sticker_frecency_entry,
                                         &frecency);
    for (u32 i = 0; query.bytes && i < count; ++i) {
        const bool guild = p.guild && i < p.guild->count;
        const auto& item = guild ? p.guild->items[i] : p.packs->items[i - (p.guild ? p.guild->count : 0)];
        if (!item.available)
            continue;
        u32 score = 0;
        Utf8View match{};
        const auto consider = [&](Utf8View value, u32 weight, bool contains) noexcept {
            const u32 points = value.bytes == query.bytes && ascii_equal_ignore_case(value, query) ? 10 * weight
                               : value.bytes >= query.bytes &&
                                       ascii_equal_ignore_case({value.data, query.bytes}, query)
                                   ? 7 * weight
                               : contains && ascii_contains_ignore_case(value, query) ? 5 * weight
                                                                                      : 0;
            if (points > score) {
                score = points;
                match = value;
            }
        };
        consider(item.name, 11, true);
        consider(sticker_source(p, item), 8, true);
        if (guild && item.tags.bytes) {
            consider(item.tags, 1, false);
            consider(emoji_shortcode_glyph(item.tags), 6, false);
            char toned[128];
            if (item.tags.bytes + 6 <= sizeof(toned)) {
                copy_bytes(toned, item.tags.data, item.tags.bytes);
                copy_bytes(toned + item.tags.bytes, "_tone", 5);
                for (char tone = '1'; tone <= '5'; ++tone) {
                    toned[item.tags.bytes + 5] = tone;
                    consider(emoji_shortcode_glyph({toned, item.tags.bytes + 6}), 6, false);
                }
            }
        }
        if (!score)
            continue;
        // Integer form of score * frecency / 100: unweighted stickers count 100.
        u32 weight = 100;
        for (u32 f = 0; f < frecency.count; ++f)
            if (frecency.id[f] == item.sticker_id) {
                weight = frecency.score[f] ? frecency.score[f] : 100;
                break;
            }
        score *= weight;
        u32 at = shown < kUiInlineResultsStickerLimit ? shown++ : kUiInlineResultsStickerLimit;
        while (at && scores[at - 1] < score) {
            if (at < kUiInlineResultsStickerLimit) {
                scores[at] = scores[at - 1];
                matches[at] = matches[at - 1];
                p.filtered[at] = p.filtered[at - 1];
            }
            --at;
        }
        if (at < kUiInlineResultsStickerLimit) {
            scores[at] = score;
            matches[at] = match;
            p.filtered[at] = i;
        }
    }
    for (u32 n = 0; n < shown; ++n) {
        const u32 i = p.filtered[n];
        const auto& item = p.guild && i < p.guild->count ? p.guild->items[i]
                                                          : p.packs->items[i - (p.guild ? p.guild->count : 0)];
        u32 bytes = 0;
        if (matches[n].bytes != item.name.bytes || !ascii_equal_ignore_case(matches[n], item.name)) {
            auto* note = p.inline_notes[n];
            bytes = utf8_copy_truncated(note, sizeof(p.inline_notes[n]), utf8_literal("Similar to: "));
            const u32 start = bytes;
            bytes += utf8_copy_truncated(note + bytes, sizeof(p.inline_notes[n]) - bytes, matches[n]);
            for (u32 k = start; k < bytes; ++k)
                note[k] = ascii_lower(note[k]);
        }
        p.inline_note_bytes[n] = static_cast<u16>(bytes);
    }
    p.filtered_count = shown;
    p.source.count = shown;
    p.inline_select = true;
    return true;
}

} // namespace picker_detail
} // namespace maicord::app_detail
