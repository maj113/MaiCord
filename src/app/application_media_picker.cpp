#include "application_reply_draft.hpp"
#include "application_content_point.hpp"
#include "text_edit_keys.hpp"
#include "application_media_picker.hpp"
#include "application_cursor.hpp"
#include "maicord/ui/scene_cursor.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_composer_text_input.hpp"
#include "application_friends.hpp"
#include "application_message_history.hpp"
#include "application_message_mutation_notice.hpp"
#include "application_message_reaction.hpp"
#include "application_profile.hpp"
#include "application_settings.hpp"
#include "application_gif_favorites.hpp"
#include "application_emoji_preferences.hpp"
#include "application_surface_motion.hpp"
#include "application_retained_projection.hpp"
#include "catalog_preview_log.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/app/account_media_catalog.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/cdn_url.hpp"
#include "maicord/ui/reaction_picker.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/splitter.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/model/forwarded_input.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/app/gateway_client.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/app/account_media.hpp"

#include <limits.h>

namespace maicord::app_detail {
namespace {
struct CatalogBank {
    MediaCatalogItem* items;
    char* text;
    u8* preview_mask;
    u64 first_id;
    u32 count, text_bytes, allocation_bytes, generation;
    MediaCatalogOperation operation;
};
bool preview_admitted(const CatalogBank& bank, u32 index) noexcept {
    return index < bank.count && bank.preview_mask && (bank.preview_mask[index >> 3] & (1u << (index & 7)));
}
bool favorite_contains(Utf8View text, Utf8View query) noexcept {
    const auto ignored = [](char c) noexcept { return c == '-' || c == '_' || c == ' '; };
    u32 first = 0;
    while (first < query.bytes && ignored(query.data[first]))
        ++first;
    if (first == query.bytes)
        return true;
    for (u32 start = 0; start < text.bytes; ++start) {
        if (ignored(text.data[start]))
            continue;
        u32 at = start, needle = first;
        for (;;) {
            while (needle < query.bytes && ignored(query.data[needle]))
                ++needle;
            if (needle == query.bytes)
                return true;
            while (at < text.bytes && ignored(text.data[at]))
                ++at;
            if (at == text.bytes || ascii_lower(text.data[at]) != ascii_lower(query.data[needle]))
                break;
            ++at;
            ++needle;
        }
    }
    return false;
}
// Server sticker lists from READY, GUILD_CREATE and GUILD_STICKERS_UPDATE (or
// the current server's REST list), verbatim JSON per server for the session.
constexpr u32 kStickerServerCapacity = 256;
constexpr u32 kStickerStoreMaximumBytes = 8 * 1024 * 1024;
struct StickerServerList {
    u64 guild_id;
    char* json; // Null for a server without stickers.
    u32 bytes;
};
struct StickerStore {
    StickerServerList lists[kStickerServerCapacity];
    u32 count, bytes;
    u64 revision;
} g_sticker_store;
const StickerServerList* sticker_list(u64 guild_id) noexcept {
    for (u32 i = 0; guild_id && i < g_sticker_store.count; ++i)
        if (g_sticker_store.lists[i].guild_id == guild_id)
            return &g_sticker_store.lists[i];
    return nullptr;
}
void sticker_list_remove(u64 guild_id) noexcept {
    auto& store = g_sticker_store;
    for (u32 i = 0; i < store.count; ++i)
        if (store.lists[i].guild_id == guild_id) {
            store.bytes -= store.lists[i].bytes;
            if (store.lists[i].json)
                HeapFree(GetProcessHeap(), 0, store.lists[i].json);
            store.lists[i] = store.lists[--store.count];
            ++store.revision;
            return;
        }
}
bool sticker_list_put(u64 guild_id, Utf8View json) noexcept {
    auto& store = g_sticker_store;
    if (!guild_id || !json.bytes || json.bytes > kMediaCatalogMaximumStickerResponseBytes)
        return false;
    sticker_list_remove(guild_id);
    if (store.count == kStickerServerCapacity || json.bytes > kStickerStoreMaximumBytes - store.bytes)
        return false;
    // Most servers have none; their "[]" stays a known empty list without a copy.
    const bool empty = json.bytes == 2 && json.data[0] == '[' && json.data[1] == ']';
    char* copy = nullptr;
    if (!empty) {
        copy = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, json.bytes));
        if (!copy)
            return false;
        CopyMemory(copy, json.data, json.bytes);
    }
    const u32 bytes = copy ? json.bytes : 0;
    store.lists[store.count++] = {guild_id, copy, bytes};
    store.bytes += bytes;
    ++store.revision;
    return true;
}
void sticker_store_clear() noexcept {
    auto& store = g_sticker_store;
    for (u32 i = 0; i < store.count; ++i)
        if (store.lists[i].json)
            HeapFree(GetProcessHeap(), 0, store.lists[i].json);
    store.count = store.bytes = 0;
    ++store.revision;
}
MediaAttachmentFormat format(const MediaCatalogItem& item) noexcept {
    // A synced Klipy favourite stores the official picker's gif_src (.gif); a
    // VIDEO favourite's MP4 plays as a muted looping tile.
    return item.kind == MediaCatalogItemKind::sticker
               ? (item.sticker_format == 4 ? MediaAttachmentFormat::gif
                  : item.sticker_format == 3 ? MediaAttachmentFormat::lottie : MediaAttachmentFormat::png)
               : catalog_favorite_format(item.preview_url);
}
} // namespace
struct ApplicationMediaPicker {
    MessageComposer edit;
    char query[256], submitted_query[256], tab_query[3][256];
    u16 tab_query_bytes[3];
    float tab_scroll[3];
    ReactionPickerModel emoji;
    u64 emoji_first_id;
    u64 emoji_state_revision;
    u32 emoji_guild_revision;
    // categories: the landing view's /gifs/trending page, kept for the
    // session like the official client's store; gif: the current results.
    CatalogBank *gif, *categories, *guild, *packs;
    CatalogBank saved, favorites_tile;
    MediaCatalogItem tile_item;
    u8* saved_preview_mask;
    u32 saved_mask_bytes, tile_source_index, favorite_generation;
    u64 favorites_revision, favorites_data_revision;
    u8 tile_preview_mask;
    UiMediaPickerScene scene;
    UiMediaPickerSource source;
    UiMediaPickerContent content;
    UiMediaPickerSection sections[kUiMediaPickerMaximumSections];
    u32 *filtered, filtered_count, filtered_capacity, section_count;
    u64 scope, account_id, channel_id, guild_id, submitted_id, submitted_generation;
    u64 due_at, blink_at, hover_at, edit_identity;
    u32 generation, content_revision, edit_revision, held_key, retries;
    MediaCatalogOperation operation, submitted_operation;
    UiMediaPickerKind kind;
    UiMediaPickerPhase phase;
    Utf8View status;
    bool active, focused, caret_visible, captured, dragging, wanted, admission_busy, sticker_guild_done;
    bool favorites_enabled, favorites_view, gif_favorites_view;
    u64 scrollbar_scope, scrollbar_revision;
    float scrollbar_grab;
    bool scrollbar_dragging, scrollbar_rail;
    float preferred_width, resize_fixed_edge, resize_pointer_offset;
    bool resizing;
    // Reaction popout: the Emoji component targeting one message instead of
    // the composer. Zero message ID is the composer's expression picker.
    u64 reaction_message_id;
    UiRect reaction_anchor;
    ApplicationMessageMutationScope reaction_scope;
    UiMediaPickerKind composer_kind;
    bool reaction_error;
    // Server sticker sections of the guild bank; labels live in its text.
    struct StickerServer {
        u64 id;
        Utf8View label;
        u32 icon_variant;
        bool animated;
    } sticker_servers[kStickerServerCapacity];
    u32 sticker_server_count;
    u64 sticker_store_revision;
    // Packs-bank index of each section's cover sticker; ~0 when none.
    u32 section_cover[kUiMediaPickerMaximumSections];
};
namespace {
constexpr auto kReactionHint = utf8_literal("Shift-click to add more reactions");
// Search-as-you-type waits for typing to pause before one request.
constexpr u32 kSearchDebounceMs = 250;
// Rate-limited catalog requests retry at most this often, never sooner than
// the server's Retry-After.
constexpr u32 kCatalogRateRetries = 3;
constexpr u64 kCatalogLocalRateRetryMs = 1000;
bool projection(ApplicationMediaPicker&) noexcept;
void revoke_capture(ApplicationMediaPicker&) noexcept;
// The emoji picker holds every emoji the chat state can.
static_assert(kChatStateEmojiCapacity <= kReactionPickerCustomEmojiCapacity &&
              kReactionPickerResultCapacity <= kUiMediaPickerMaximumItems);
bool emoji_projection(ApplicationMediaPicker& p) noexcept {
    revoke_capture(p);
    if (!reaction_picker_refresh_search(&p.emoji).ok() ||
        p.emoji.section_count > kUiMediaPickerMaximumSections ||
        g_app.media_picker_item_serial > ~u64{0} - p.emoji.result_count - p.emoji.section_count)
        return false;
    p.emoji_first_id = g_app.media_picker_item_serial + 1;
    g_app.media_picker_item_serial += p.emoji.result_count + p.emoji.section_count;
    p.source.count = p.emoji.result_count;
    p.section_count = p.emoji.section_count;
    for (u32 i = 0; i < p.section_count; ++i) {
        const auto& section = p.emoji.sections[i];
        p.sections[i] = {p.emoji_first_id + p.emoji.result_count + i, section.first, section.count,
                         reaction_picker_section_label(p.emoji, i), !section.collapsed};
        auto& projected = p.sections[i];
        if (section.guild_id) {
            projected.visual = UiMediaPickerSectionVisual::guild;
            projected.guild_id = section.guild_id;
            for (u32 guild = 0; guild < p.emoji.guild_count; ++guild) {
                const auto& source = p.emoji.guilds[guild];
                if (source.id != section.guild_id)
                    continue;
                projected.icon_variant = source.icon_variant;
                projected.icon_animated = source.animated;
                break;
            }
        } else if (section.local < kReactionPickerSectionSearch) {
            projected.visual = static_cast<UiMediaPickerSectionVisual>(section.local + 1);
        }
    }
    p.phase = p.source.count ? UiMediaPickerPhase::ready : UiMediaPickerPhase::empty;
    p.status = !p.source.count           ? utf8_literal("No emoji match your search.")
               : p.reaction_message_id ? kReactionHint
                                       : utf8_literal("Choose an emoji");
    p.reaction_error = false;
    ++p.source.revision;
    return true;
}
// The Emoji tab stays open (and the tabs usable) when its model cannot open.
void emoji_unavailable(ApplicationMediaPicker& p) noexcept {
    revoke_capture(p);
    ui_media_picker_scene_close(&p.scene);
    reaction_picker_release(&p.emoji);
    const auto& state = g_app.account_ui->chat_state;
    p.emoji_state_revision = (static_cast<u64>(state.emoji_mutations) << 32) | state.permission_mutations;
    p.emoji_guild_revision = state.guild_mutations;
    p.source.count = p.section_count = 0;
    p.phase = UiMediaPickerPhase::failed;
    p.status = utf8_literal("Couldn't load emoji.");
    ++p.source.revision;
    log_write(LogLevel::warning, "ui", "emoji_picker_open_failed");
}
bool open_emoji_model(ApplicationMediaPicker& p) noexcept {
    const bool reaction = p.reaction_message_id != 0;
    auto& ui = *g_app.account_ui;
    if (!reaction_picker_open(&p.emoji, reaction ? p.reaction_message_id : p.channel_id,
                              &ui.reaction_preferences, clock_unix_milliseconds(), &ui.chat_state, p.guild_id,
                              &p.edit, reaction ? ReactionPreferenceKind::reaction : ReactionPreferenceKind::message)
             .ok() ||
        !emoji_projection(p))
        return false;
    p.emoji_state_revision =
        (static_cast<u64>(ui.chat_state.emoji_mutations) << 32) | ui.chat_state.permission_mutations;
    p.emoji_guild_revision = ui.chat_state.guild_mutations;
    return true;
}
bool landing(const ApplicationMediaPicker& p) noexcept {
    return p.kind == UiMediaPickerKind::gif && !p.favorites_view &&
           p.operation == MediaCatalogOperation::gif_categories;
}
// The bank the GIF view shows: the cached categories on the landing view.
CatalogBank* gif_bank(const ApplicationMediaPicker& p) noexcept {
    return p.operation == MediaCatalogOperation::gif_categories ? p.categories : p.gif;
}
Utf8View favorite_detail(Utf8View value) noexcept {
    u32 bytes = minimum(value.bytes, 256u);
    while (bytes && !utf8_validate({value.data, bytes}).valid)
        --bytes;
    return {value.data, bytes};
}
void favorite_item(const GifFavoriteRecord& record, MediaCatalogItem* item) noexcept {
    reset_value(item);
    item->kind = MediaCatalogItemKind::gif;
    item->name = utf8_literal("Saved GIF");
    item->description = favorite_detail(record.url);
    item->preview_url = record.src;
    item->gif_url = record.src;
    item->send_url = record.url;
    item->width = record.width;
    item->height = record.height;
    item->available = record.url.bytes && record.url.bytes <= kMediaCatalogMaximumUrlBytes;
}
bool favorites_refresh(ApplicationMediaPicker& p) noexcept {
    if (!p.favorites_enabled)
        return true;
    const auto view = application_gif_favorites_view();
    if (p.favorites_data_revision == view.data_revision && p.favorites_revision == view.revision)
        return true;
    if (p.kind == UiMediaPickerKind::gif)
        revoke_capture(p);
    const bool data_changed = p.favorites_data_revision != view.data_revision;
    if (data_changed) {
        if (view.records.count > kUiMediaPickerMaximumItems || p.favorite_generation == ~u32{0} ||
            g_app.media_picker_item_serial > ~u64{0} - view.records.count)
            return false;
        const u32 mask_bytes = (view.records.count + 7) / 8;
        auto* mask = mask_bytes ? static_cast<u8*>(VirtualAlloc(nullptr, mask_bytes,
                                                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                                : nullptr;
        if (mask_bytes && !mask)
            return false;
        u32 first_preview = ~u32{0};
        u8 target[kCdnMaximumTargetBytes];
        for (u32 i = 0; i < view.records.count; ++i) {
            const auto& record = view.records[i];
            // Tenor/Giphy favourites preview as the provider's animated WebP,
            // a Klipy gif_src as the GIF itself, a VIDEO favourite's MP4 as a
            // muted looping video tile, and any other external image through
            // the signed media proxy.
            const auto preview_format = catalog_favorite_format(record.src);
            const auto mapped = parse_catalog_preview_url(record.src, {target, sizeof(target)},
                                                          preview_format, record.width, record.height);
            if (mapped.ok()) {
                mask[i >> 3] |= static_cast<u8>(1u << (i & 7));
                if (first_preview == ~u32{0})
                    first_preview = i;
            } else {
                // Logged once per favourites revision: the tile keeps its placeholder.
                CatalogPreviewLog("map")
                    .field("favorite", i)
                    .field("format", static_cast<u64>(preview_format))
                    .field("stored_format", static_cast<u64>(record.format))
                    .field("url_error", static_cast<u64>(mapped.error))
                    .source(record.src)
                    .write();
            }
        }
        if (p.saved_preview_mask)
            VirtualFree(p.saved_preview_mask, 0, MEM_RELEASE);
        p.saved_preview_mask = mask;
        p.saved_mask_bytes = mask_bytes;
        reset_value(&p.saved);
        p.saved.count = view.records.count;
        p.saved.first_id = g_app.media_picker_item_serial + 1;
        g_app.media_picker_item_serial += view.records.count;
        p.saved.generation = ++p.favorite_generation;
        p.saved.preview_mask = mask;
        p.saved.operation = MediaCatalogOperation::gif_search;
        p.favorites_data_revision = view.data_revision;
        p.tile_source_index = first_preview;
        p.favorites_tile.generation = p.favorite_generation;
    }
    reset_value(&p.tile_item);
    p.tile_item.kind = MediaCatalogItemKind::category;
    p.tile_item.name = utf8_literal("Favorites");
    p.tile_item.available = true;
    p.tile_preview_mask = 0;
    if (p.tile_source_index < view.records.count) {
        const auto& preview = view.records[p.tile_source_index];
        p.tile_item.preview_url = preview.src;
        p.tile_item.width = preview.width;
        p.tile_item.height = preview.height;
        p.tile_preview_mask = 1;
    }
    p.favorites_revision = view.revision;
    if (p.kind == UiMediaPickerKind::gif) {
        ++p.source.revision;
        ui_media_picker_scene_cancel_capture(&p.scene);
    }
    return !data_changed || p.kind != UiMediaPickerKind::gif || !p.favorites_view || projection(p);
}
bool enable_favorites(ApplicationMediaPicker& p) noexcept {
    if (p.favorites_enabled) {
        application_gif_favorites_set_visible(true);
        return favorites_refresh(p);
    }
    if (!g_app.account.api.gif_favorites || !application_gif_favorites_open())
        return true;
    if (g_app.media_picker_item_serial == ~u64{0})
        return false;
    p.favorites_enabled = true;
    p.favorite_generation = 1;
    p.tile_source_index = ~u32{0};
    p.favorites_tile.first_id = ++g_app.media_picker_item_serial;
    p.favorites_tile.count = 1;
    p.favorites_tile.generation = 1;
    p.favorites_tile.operation = MediaCatalogOperation::gif_categories;
    p.favorites_tile.preview_mask = &p.tile_preview_mask;
    p.favorites_revision = ~u64{0};
    p.favorites_data_revision = ~u64{0};
    return favorites_refresh(p);
}
bool account_ready() noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed && !g_app.login.visible &&
           !application_friends_active() &&
           g_app.account.status.required_action == AccountRequirement::none;
}
bool reaction_current(const ApplicationMediaPicker& p) noexcept {
    const auto& scope = p.reaction_scope;
    const auto* ui = g_app.account_ui;
    if (!ui || !scope.message_id || scope.message_id != p.reaction_message_id)
        return false;
    const auto* channel = chat_state_store_find_channel(ui->chat_state, scope.channel_id);
    const auto found = message_store_find(g_app.message_view.store, scope.message_id);
    const auto* message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    return scope.account_serial == g_app.plugin_account_scope_serial &&
           scope.profile_id == g_app.account.status.profile_id &&
           scope.self_user_id == ui->chat_state.self_user_id && scope.channel_id == ui->active_channel_id &&
           channel && channel->occupied && channel->generation == scope.channel_generation && message &&
           message->channel_id == scope.channel_id && message->delivery == MessageDeliveryState::authoritative;
}
// The composer's expression picker needs a sendable composer; the reaction
// popout needs only its current message (reactions need no send access).
bool available(const ApplicationMediaPicker* p = nullptr) noexcept {
    if (!account_ready())
        return false;
    if (p && p->reaction_message_id)
        return reaction_current(*p);
    return g_app.message_composer_scene.active &&
           g_app.message_composer.channel_id == g_app.account_ui->active_channel_id &&
           g_app.message_composer_scene.channel_allows_send && !g_app.message_edit.editor_active;
}
bool owns_current(const ApplicationMediaPicker& p) noexcept {
    return available(&p) && p.account_id == g_app.account_ui->chat_state.self_user_id &&
           p.channel_id == g_app.account_ui->active_channel_id;
}
bool unobstructed() noexcept {
    return !application_settings_active() && !g_app.text_form && !g_app.popup_scene.active &&
           !(g_app.profile && g_app.profile->scene.active) && !g_app.image_viewer.scene.active &&
           !g_app.reaction_viewer.active;
}
bool front(const ApplicationMediaPicker* p = nullptr) noexcept {
    return available(p) && unobstructed();
}
void focus(ApplicationMediaPicker& p, bool value) noexcept {
    if (p.focused != value)
        application_composer_text_input_suspend();
    if (value && !p.focused) {
        if (g_app.media_picker_scope_serial == ~u64{0})
            return;
        p.edit_identity = ++g_app.media_picker_scope_serial;
    }
    p.focused = p.edit.focused = value;
    p.caret_visible = value;
    p.blink_at = value ? deadline_after(clock_monotonic_milliseconds(), kDefaultCaretBlinkMilliseconds) : 0;
    if (value) {
        g_app.message_composer.focused = false;
        g_app.context_focus = &p.scene.widgets;
        SetFocus(g_app.window);
        ui_widget_manager_focus(&p.scene.widgets, p.scene.edit);
    } else if (g_app.context_focus == &p.scene.widgets)
        g_app.context_focus = nullptr;
}
void cancel(ApplicationMediaPicker& p) noexcept {
    if (p.submitted_id && g_app.account.instance && g_app.account.api.media_catalog)
        g_app.account.api.media_catalog(g_app.account.instance, AccountMediaCatalogOperation::cancel,
                                        nullptr, p.submitted_id);
    // Keep the response until the primary worker consumes it.
}
bool repaint(bool defer_text = false) noexcept {
    if (!application_media_picker_synchronize())
        return false;
    // Pure pointer/scroll changes update hit geometry immediately. The frame
    // boundary shapes the latest suffix once after native input dispatch.
    if (defer_text)
        return refresh_account_text() && application_request_frame();
    if (!refresh_retained_projection())
        return false;
    application_composer_text_input_notify_layout();
    application_composer_text_input_synchronize();
    return true;
}
const MediaCatalogItem* item_at(const ApplicationMediaPicker& p, u32 index,
                                const CatalogBank** bank = nullptr, u32* local = nullptr,
                                MediaCatalogItem* temporary = nullptr) noexcept {
    if (index >= p.source.count)
        return nullptr;
    if (landing(p) && p.favorites_enabled) {
        if (!index) {
            if (bank)
                *bank = &p.favorites_tile;
            if (local)
                *local = 0;
            return &p.tile_item;
        }
        --index;
    }
    if (p.kind == UiMediaPickerKind::gif && p.favorites_view) {
        const auto view = application_gif_favorites_view();
        if (!temporary || index >= p.filtered_count || !p.filtered)
            return nullptr;
        const u32 at = p.filtered[index];
        if (at >= p.saved.count || at >= view.records.count)
            return nullptr;
        favorite_item(view.records[at], temporary);
        if (bank)
            *bank = &p.saved;
        if (local)
            *local = at;
        return temporary;
    }
    const CatalogBank* owner = p.kind == UiMediaPickerKind::gif ? gif_bank(p) : p.guild;
    if (index >= p.source.count || (p.kind == UiMediaPickerKind::sticker && index >= p.filtered_count))
        return nullptr;
    u32 at = p.kind == UiMediaPickerKind::sticker && p.filtered ? p.filtered[index] : index;
    if (p.kind == UiMediaPickerKind::sticker && (!owner || at >= owner->count)) {
        at -= owner ? owner->count : 0;
        owner = p.packs;
    }
    if (!owner || at >= owner->count)
        return nullptr;
    if (bank)
        *bank = owner;
    if (local)
        *local = at;
    return &owner->items[at];
}
// Writes each sticker's CDN preview URL into `urls` and admits it.
bool sticker_previews(CatalogBank* bank, char* urls, u32 capacity) noexcept {
    ProtocolWriter writer{{reinterpret_cast<u8*>(urls), capacity}, 0};
    u8 target[kCdnMaximumTargetBytes];
    for (u32 i = 0; i < bank->count; ++i) {
        auto& item = bank->items[i];
        const u32 first = writer.used;
        if (!(item.sticker_format == 4
                  ? protocol_writer_append_literal(&writer, "https://media.discordapp.net/stickers/")
                  : protocol_writer_append_literal(&writer, "https://cdn.discordapp.com/stickers/")) ||
            !protocol_writer_append_u64(&writer, item.sticker_id) ||
            !(item.sticker_format == 4   ? protocol_writer_append_literal(&writer, ".gif")
              : item.sticker_format == 3 ? protocol_writer_append_literal(&writer, ".json")
                                         : protocol_writer_append_literal(&writer, ".png")))
            return false;
        item.preview_url = {urls + first, writer.used - first};
        if (item.available &&
            parse_catalog_preview_url(item.preview_url, {target, sizeof(target)}, format(item)).ok())
            bank->preview_mask[i >> 3] |= static_cast<u8>(1u << (i & 7));
    }
    return true;
}
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
        servers ? static_cast<u32*>(VirtualAlloc(nullptr, kMediaCatalogStructuralCapacity * sizeof(u32),
                                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
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
    auto* bank = items && g_app.media_picker_item_serial <= ~u64{0} - items
                     ? static_cast<CatalogBank*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
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
                auto item = bank->items[first + k];
                const auto sendable =
                    guild_sticker_sendability(order[i], channel_guild, item.available, external, premium);
                if (sendable == StickerSendability::hidden)
                    continue;
                item.guild_id = order[i];
                item.available = sendable == StickerSendability::sendable;
                bank->items[used++] = item;
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
            bank->allocation_bytes = bytes;
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
bool resolve(void* context, u32 index, UiMediaPickerItem* out) noexcept {
    const auto& p = *static_cast<ApplicationMediaPicker*>(context);
    if (!out || index >= p.source.count)
        return false;
    if (p.kind == UiMediaPickerKind::emoji) {
        ReactionPickerSelection selection{};
        Utf8View label{};
        if (!reaction_picker_result_at(p.emoji, index, &selection, &label))
            return false;
        reset_value(out);
        out->id = p.emoji_first_id + index;
        out->kind = UiMediaPickerItemKind::emoji;
        out->enabled = true;
        out->label = label;
        // The footer reads "from <server>" for custom emoji.
        out->detail = reaction_picker_result_guild_name(p.emoji, index);
        out->width = out->height = 32;
        if (selection.custom) {
            out->asset_id = selection.emoji_id;
            out->asset_flags = selection.animated ? 1 : 0;
        } else {
            out->glyph = selection.name;
            out->regional_flag = regional_flag_sequence(selection.name);
        }
        return true;
    }
    const CatalogBank* bank = nullptr;
    u32 at = 0;
    MediaCatalogItem temporary{};
    const auto* item = item_at(p, index, &bank, &at, &temporary);
    if (!item)
        return false;
    const bool categories = bank->operation == MediaCatalogOperation::gif_categories;
    const auto key = media_asset_catalog_preview(bank->first_id + at, bank->generation, format(*item),
                                                 (item->sticker_format == 2 || item->sticker_format == 3));
    reset_value(out);
    out->id = key.id;
    out->asset_id = preview_admitted(*bank, at) ? key.id : 0;
    out->variant = key.variant;
    out->asset_flags = key.flags;
    out->label =
        categories && item->kind == MediaCatalogItemKind::gif ? utf8_literal("Trending GIFs") : item->name;
    out->detail = item->description.bytes ? item->description : item->name;
    // Stickers name their source (server or pack) in the footer.
    if (p.kind == UiMediaPickerKind::sticker) {
        reset_value(&out->detail);
        for (u32 s = 0; s < p.section_count; ++s)
            if (index >= p.sections[s].first && index - p.sections[s].first < p.sections[s].count) {
                out->detail = p.sections[s].label;
                break;
            }
    }
    out->width = item->width;
    out->height = item->height;
    out->kind = p.kind == UiMediaPickerKind::sticker ? UiMediaPickerItemKind::sticker
                : categories                         ? UiMediaPickerItemKind::category
                                                     : UiMediaPickerItemKind::gif;
    out->enabled = item->available;
    if (out->kind == UiMediaPickerItemKind::gif && p.favorites_enabled) {
        const auto favorite = application_gif_favorites_state(item->send_url);
        out->favoritable = item->send_url.bytes && item->send_url.bytes <= kMediaCatalogMaximumUrlBytes;
        out->favorite = favorite.confirmed;
        out->favorite_pending = favorite.pending || !favorite.available;
        out->favorite_failed = favorite.failed;
    }
    return true;
}
bool projection(ApplicationMediaPicker& p) noexcept {
    revoke_capture(p);
    p.section_count = 0;
    if (p.kind == UiMediaPickerKind::gif) {
        if (!p.favorites_view) {
            p.source.count = (gif_bank(p) ? gif_bank(p)->count : 0) + (landing(p) && p.favorites_enabled ? 1 : 0);
            return true;
        }
    }
    const auto favorites = application_gif_favorites_view();
    const u32 count = p.favorites_view && p.kind == UiMediaPickerKind::gif
                          ? favorites.records.count
                          : (p.guild ? p.guild->count : 0) + (p.packs ? p.packs->count : 0);
    if (count > p.filtered_capacity) {
        auto* next = static_cast<u32*>(
            VirtualAlloc(nullptr, count * sizeof(u32), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!next)
            return false;
        if (p.filtered)
            VirtualFree(p.filtered, 0, MEM_RELEASE);
        p.filtered = next;
        p.filtered_capacity = count;
    }
    p.filtered_count = 0;
    if (p.kind == UiMediaPickerKind::gif && p.favorites_view) {
        const auto query = message_composer_text(p.edit);
        for (u32 i = 0; i < count; ++i)
            if (favorite_contains(favorites.records[i].url, query))
                p.filtered[p.filtered_count++] = i;
        p.source.count = p.filtered_count;
        return true;
    }
    const auto query = message_composer_text(p.edit);
    u64 previous = 0;
    for (u32 i = 0; i < count; ++i) {
        const auto* bank = p.guild && i < p.guild->count ? p.guild : p.packs;
        const u32 local = p.guild && i < p.guild->count ? i : i - (p.guild ? p.guild->count : 0);
        const auto& item = bank->items[local];
        if (!ascii_contains_ignore_case(item.name, query) &&
            !ascii_contains_ignore_case(item.tags, query) &&
            !ascii_contains_ignore_case(item.description, query))
            continue;
        const u64 section = item.guild_id ? item.guild_id : item.pack_id;
        if (section != previous && p.section_count < kUiMediaPickerMaximumSections) {
            auto& next = p.sections[p.section_count];
            next = {section, p.filtered_count, 0,
                    item.pack_name.bytes ? item.pack_name : utf8_literal("Sticker pack"), true};
            p.section_cover[p.section_count] = ~u32{0};
            if (item.guild_id) {
                next.visual = UiMediaPickerSectionVisual::guild;
                next.guild_id = item.guild_id;
                next.label = utf8_literal("Server stickers");
                for (u32 s = 0; s < p.sticker_server_count; ++s)
                    if (p.sticker_servers[s].id == item.guild_id) {
                        next.label = p.sticker_servers[s].label;
                        next.icon_variant = p.sticker_servers[s].icon_variant;
                        next.icon_animated = p.sticker_servers[s].animated;
                    }
            } else {
                // The pack's cover sticker, else its first, as in the official picker.
                next.visual = UiMediaPickerSectionVisual::media;
                u32 cover = ~u32{0};
                for (u32 k = 0; k < bank->count; ++k)
                    if (bank->items[k].pack_id == item.pack_id && (cover == ~u32{0} || bank->items[k].cover)) {
                        cover = k;
                        if (bank->items[k].cover)
                            break;
                    }
                if (cover != ~u32{0} && preview_admitted(*bank, cover)) {
                    const auto& art = bank->items[cover];
                    const auto key = media_asset_catalog_preview(bank->first_id + cover, bank->generation, format(art),
                                                                 art.sticker_format == 2 || art.sticker_format == 3);
                    p.section_cover[p.section_count] = cover;
                    next.asset_id = key.id;
                    next.icon_variant = key.variant;
                    next.asset_flags = key.flags;
                }
            }
            ++p.section_count;
            previous = section;
        }
        if (p.section_count)
            ++p.sections[p.section_count - 1].count;
        p.filtered[p.filtered_count++] = i;
    }
    p.source.count = p.filtered_count;
    return true;
}
void request(ApplicationMediaPicker& p, MediaCatalogOperation operation, bool delayed = false) noexcept {
    revoke_capture(p);
    cancel(p);
    if (p.kind == UiMediaPickerKind::gif)
        p.favorites_view = p.gif_favorites_view = false;
    if (p.generation == ~u32{0}) {
        p.wanted = false;
        p.phase = UiMediaPickerPhase::failed;
        return;
    }
    ++p.generation;
    p.operation = operation;
    p.retries = 0;
    p.admission_busy = false;
    ++p.source.revision;
    p.scene.scroll_dips = 0;
    // The landing categories load once per session; returning to them (Back,
    // a cleared search, reopening) reuses that page without a request.
    if (operation == MediaCatalogOperation::gif_categories && p.categories) {
        p.wanted = false;
        p.due_at = 0;
        p.phase = UiMediaPickerPhase::ready;
        p.status = utf8_literal("Choose an item");
        projection(p);
        return;
    }
    p.wanted = true;
    p.due_at = deadline_after(clock_monotonic_milliseconds(), delayed ? kSearchDebounceMs : 0);
    p.phase = UiMediaPickerPhase::loading;
    p.status = utf8_literal("Loading…");
}
bool switch_tab(ApplicationMediaPicker& p, UiMediaPickerKind kind) noexcept {
    if (kind > UiMediaPickerKind::emoji)
        return false;
    if (p.kind == kind && p.scene.active)
        return repaint();
    const u32 old = static_cast<u32>(p.kind), next = static_cast<u32>(kind);
    const auto query = message_composer_text(p.edit);
    p.tab_query_bytes[old] = static_cast<u16>(query.bytes);
    CopyMemory(p.tab_query[old], query.data, query.bytes);
    if (p.scene.active)
        p.tab_scroll[old] = p.scene.scroll_dips;
    if (p.kind == UiMediaPickerKind::gif)
        p.gif_favorites_view = p.favorites_view;
    focus(p, false);
    revoke_capture(p);
    cancel(p);
    if (p.kind == UiMediaPickerKind::emoji && kind != UiMediaPickerKind::emoji) {
        ui_media_picker_scene_close(&p.scene);
        reaction_picker_release(&p.emoji);
    }
    p.kind = kind;
    message_composer_restore(&p.edit, 0, {p.tab_query[next], p.tab_query_bytes[next]},
                             p.tab_query_bytes[next], p.tab_query_bytes[next]);
    if (kind == UiMediaPickerKind::emoji) {
        ui_media_picker_scene_close(&p.scene);
        application_gif_favorites_set_visible(false);
        p.favorites_view = false;
        p.wanted = false;
        p.due_at = 0;
        if (p.generation == ~u32{0})
            return false;
        ++p.generation;
        application_emoji_preferences_open();
        if (!open_emoji_model(p))
            emoji_unavailable(p);
    } else if (kind == UiMediaPickerKind::gif && p.gif_favorites_view) {
        if (p.generation == ~u32{0})
            return false;
        ++p.generation;
        p.favorites_view = true;
        p.wanted = false;
        if (!enable_favorites(p) || !projection(p))
            return false;
        ++p.source.revision;
    } else if (kind == UiMediaPickerKind::gif) {
        if (!enable_favorites(p))
            return false;
        request(p,
                p.edit.bytes ? MediaCatalogOperation::gif_search : MediaCatalogOperation::gif_categories);
    } else {
        p.favorites_view = false;
        application_gif_favorites_set_visible(false);
        if (p.generation == ~u32{0})
            return false;
        ++p.generation;
        const bool server_unknown = p.guild_id && !sticker_list(p.guild_id) && !p.sticker_guild_done;
        if (!build_server_stickers(p))
            return false;
        p.wanted = !p.packs || server_unknown;
        p.operation = server_unknown ? MediaCatalogOperation::guild_stickers : MediaCatalogOperation::sticker_packs;
        p.due_at = 1;
        p.phase = p.wanted ? UiMediaPickerPhase::loading : UiMediaPickerPhase::ready;
        p.status = p.wanted ? utf8_literal("Loading stickers…") : utf8_literal("Choose a sticker");
        if (!projection(p))
            return false;
        ++p.source.revision;
    }
    // Committing the new generation resets the scene's old layout.
    // Restore the tab's saved offset only after its new scroll bounds exist.
    if (!repaint())
        return false;
    const float delta = p.tab_scroll[next] - p.scene.scroll_dips;
    return !ui_media_picker_scene_scroll(&p.scene, delta) || repaint();
}
// One reaction per activation; Shift keeps the popout open for more, as in
// the official client. A refusal keeps the popout and shows the owner's reason.
bool react(ApplicationMediaPicker& p, const ReactionPickerSelection& selection) noexcept {
    const auto admission =
        reaction_current(p) ? application_queue_message_reaction_emoji(p.reaction_message_id, selection.emoji_id,
                                                                       selection.name, selection.animated)
                            : ApplicationMessageReactionEnqueueResult::stale;
    const bool accepted = admission == ApplicationMessageReactionEnqueueResult::accepted ||
                          admission == ApplicationMessageReactionEnqueueResult::coalesced;
    if (accepted && !(GetKeyState(VK_SHIFT) & 0x8000)) {
        application_media_picker_close();
        return true;
    }
    bool error = !accepted;
    auto label = accepted ? application_message_reaction_status(p.reaction_message_id, &error)
                          : application_message_reaction_admission_text(admission);
    if (!label.bytes)
        label = accepted ? kReactionHint : utf8_literal("Couldn't update the reaction. Try again.");
    p.status = label;
    p.reaction_error = error;
    return repaint();
}
bool activate(ApplicationMediaPicker& p, UiMediaPickerTarget target) noexcept {
    if (!owns_current(p) || !front(&p) || target.account_scope != p.scope ||
        target.query_generation != p.generation || target.source_revision != p.source.revision ||
        target.scene_generation != p.scene.generation || !ui_media_picker_scene_current(p.scene))
        return true;
    if (target.action == UiMediaPickerAction::tab) {
        if (switch_tab(p, target.tab))
            return true;
        application_media_picker_close();
        return false;
    }
    if (target.action == UiMediaPickerAction::edit) {
        focus(p, true);
        return repaint();
    }
    if (target.action == UiMediaPickerAction::back) {
        message_composer_clear(&p.edit);
        request(p, MediaCatalogOperation::gif_categories);
        return repaint();
    }
    if (target.action == UiMediaPickerAction::section_jump) {
        ui_media_picker_scene_scroll_section(&p.scene, target.item_id);
        return repaint();
    }
    if (target.action == UiMediaPickerAction::section) {
        for (u32 i = 0; i < p.section_count; ++i)
            if (p.sections[i].id == target.item_id) {
                if (p.kind == UiMediaPickerKind::emoji) {
                    if (!reaction_picker_toggle_section(&p.emoji, i).ok() || !emoji_projection(p))
                        return false;
                    break;
                }
                revoke_capture(p);
                p.sections[i].expanded = !p.sections[i].expanded;
                ++p.source.revision;
                break;
            }
        return repaint();
    }
    if (target.source_index >= p.source.count)
        return true;
    if (p.kind == UiMediaPickerKind::emoji) {
        if (target.action != UiMediaPickerAction::item ||
            target.item_id != p.emoji_first_id + target.source_index)
            return true;
        ReactionPickerSelection selection{};
        if (!reaction_picker_result_at(p.emoji, target.source_index, &selection))
            return true;
        if (GetKeyState(VK_MENU) & 0x8000) {
            if (!application_emoji_preferences_toggle_favorite(reaction_picker_selection_emoji(selection)) ||
                !reaction_picker_refresh_preferences(&p.emoji).ok() || !emoji_projection(p))
                return false;
            return repaint();
        }
        if (p.reaction_message_id)
            return react(p, selection);
        char insertion[128]{};
        Utf8View value = selection.name;
        if (selection.custom) {
            ProtocolWriter writer{{reinterpret_cast<u8*>(insertion), sizeof(insertion)}, 0};
            if (!protocol_writer_append_c_string(&writer, selection.animated ? "<a:" : "<:") ||
                !protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(selection.name.data),
                                              selection.name.bytes) ||
                !protocol_writer_append_literal(&writer, ":") ||
                !protocol_writer_append_u64(&writer, selection.emoji_id) ||
                !protocol_writer_append_literal(&writer, ">"))
                return false;
            value = {insertion, writer.used};
        }
        const u32 first = minimum(g_app.message_composer.anchor, g_app.message_composer.caret);
        const u32 last = maximum(g_app.message_composer.anchor, g_app.message_composer.caret);
        const auto inserted = message_composer_replace_range(&g_app.message_composer, first, last, value);
        if (inserted != MessageComposerResult::changed)
            return false;
        // Composer frecency is recorded when the message is sent.
        application_media_picker_close();
        focus_message_composer_control(MessageComposerControl::edit);
        return composer_apply_result(inserted, true);
    }
    if (target.action == UiMediaPickerAction::category && landing(p) && p.favorites_enabled &&
        target.item_id == p.favorites_tile.first_id && target.source_index == 0) {
        cancel(p);
        if (p.generation == ~u32{0})
            return false;
        ++p.generation;
        p.favorites_view = p.gif_favorites_view = true;
        p.wanted = false;
        p.due_at = 0;
        message_composer_clear(&p.edit);
        focus(p, true);
        p.scene.scroll_dips = 0;
        ++p.source.revision;
        return favorites_refresh(p) && projection(p) && repaint();
    }
    const CatalogBank* bank = nullptr;
    u32 local = 0;
    MediaCatalogItem catalog_item{};
    const auto* item = item_at(p, target.source_index, &bank, &local, &catalog_item);
    if (!item || bank->first_id + local != target.item_id || !item->available)
        return true;
    if (target.action == UiMediaPickerAction::favorite && p.kind == UiMediaPickerKind::gif) {
        const auto saved = application_gif_favorites_state(item->send_url);
        if (!saved.available || saved.pending)
            return true;
        const auto src = item->gif_url.bytes ? item->gif_url : item->preview_url;
        application_gif_favorites_toggle(item->send_url, src, item->width, item->height, application_gif_favorites_format(src));
        return repaint();
    }
    if (target.action == UiMediaPickerAction::category) {
        if (item->kind == MediaCatalogItemKind::gif) {
            message_composer_clear(&p.edit);
            request(p, MediaCatalogOperation::gif_trending);
        } else {
            message_composer_restore(&p.edit, 0, item->name, item->name.bytes, item->name.bytes);
            request(p, MediaCatalogOperation::gif_search);
        }
        return repaint();
    }
    if (target.action != UiMediaPickerAction::item)
        return true;
    focus(p, false);
    if (p.kind == UiMediaPickerKind::gif && (GetKeyState(VK_SHIFT) & 0x8000)) {
        const auto inserted = message_composer_insert_utf8(&g_app.message_composer, item->send_url);
        application_media_picker_close();
        return composer_apply_result(inserted, true);
    }
    // Ordinary selection uses the existing durable outbox; the current draft is
    // independent and is never cleared by a catalog selection.
    char text[kMediaCatalogMaximumUrlBytes];
    MessageComposer temporary{};
    if (!message_composer_initialize(&temporary, {text, sizeof(text)}))
        return false;
    message_composer_restore(&temporary, p.channel_id,
                             p.kind == UiMediaPickerKind::gif ? item->send_url : Utf8View{}, 0, 0);
    const u64 sticker = item->sticker_id;
    const auto reply = application_reply_draft();
    if (reply.message_id && !application_channel_readable(g_app, reply.channel_id)) {
        message_composer_release(&temporary);
        p.status = utf8_literal("You can no longer reply to this message.");
        return repaint();
    }
    const auto queued = message_composer_controller_queue_send(
        &g_app.account_ui->message_composer_controller, g_app.message_composer_scene.channel_allows_send,
        g_app.account_ui->chat_state.self_user_id, clock_unix_milliseconds(),
        clock_monotonic_milliseconds(), reply.message_id, {}, {}, &temporary,
        p.kind == UiMediaPickerKind::sticker ? ConstSlice<u64>{&sticker, 1} : ConstSlice<u64>{},
        reply.mention_reply_author, &reply);
    message_composer_release(&temporary);
    if (!queued.ok()) {
        p.status = utf8_literal("Couldn't send. Try again.");
        return repaint();
    }
    application_media_picker_close();
    return refresh_message_composer_ui() && pump_message_history();
}

ApplicationMediaPicker* ensure_owner() noexcept {
    if (g_app.media_picker)
        return g_app.media_picker;
    if (g_app.media_picker_scope_serial == ~u64{0})
        return nullptr;
    auto* p = static_cast<ApplicationMediaPicker*>(
        VirtualAlloc(nullptr, sizeof(ApplicationMediaPicker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p)
        return nullptr;
    if (!message_composer_initialize(&p->edit, {p->query, sizeof(p->query)})) {
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    p->scope = ++g_app.media_picker_scope_serial;
    p->account_id = g_app.account_ui->chat_state.self_user_id;
    p->channel_id = g_app.account_ui->active_channel_id;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, p->channel_id);
    p->guild_id = channel ? channel->guild_id : 0;
    p->source.context = p;
    p->source.resolve = resolve;
    p->source.revision = 1;
    p->preferred_width = g_app.preferences.expression_picker_width;
    g_app.media_picker = p;
    return p;
}
// A new account or channel retires every bank and saved tab of the old one.
bool rebind(ApplicationMediaPicker& p) noexcept {
    if (p.account_id == g_app.account_ui->chat_state.self_user_id &&
        p.channel_id == g_app.account_ui->active_channel_id)
        return true;
    revoke_capture(p);
    ui_media_picker_scene_close(&p.scene);
    reaction_picker_release(&p.emoji);
    cancel(p);
    focus(p, false);
    if (g_app.media_picker_scope_serial == ~u64{0})
        return false;
    // The session's category cache belongs to its account.
    if (p.account_id != g_app.account_ui->chat_state.self_user_id && p.categories) {
        VirtualFree(p.categories, 0, MEM_RELEASE);
        p.categories = nullptr;
    }
    p.scope = ++g_app.media_picker_scope_serial;
    p.favorites_enabled = p.favorites_view = p.gif_favorites_view = false;
    p.account_id = g_app.account_ui->chat_state.self_user_id;
    p.channel_id = g_app.account_ui->active_channel_id;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, p.channel_id);
    p.guild_id = channel ? channel->guild_id : 0;
    if (p.guild)
        VirtualFree(p.guild, 0, MEM_RELEASE);
    p.guild = nullptr;
    p.sticker_guild_done = false;
    p.section_count = p.filtered_count = p.source.count = 0;
    ZeroMemory(p.tab_query_bytes, sizeof(p.tab_query_bytes));
    ZeroMemory(p.tab_scroll, sizeof(p.tab_scroll));
    message_composer_clear(&p.edit);
    return true;
}
} // namespace
bool application_media_picker_open(UiMediaPickerKind kind) noexcept {
    // The composer launchers replace an open reaction popout.
    if (g_app.media_picker && g_app.media_picker->reaction_message_id)
        application_media_picker_close();
    if (!front())
        return false;
    auto* owner = ensure_owner();
    if (!owner)
        return false;
    auto& p = *owner;
    if (p.active && p.scene.active && owns_current(p) && p.kind == kind) {
        application_media_picker_close();
        return true;
    }
    if (!rebind(p))
        return false;
    if (kind == UiMediaPickerKind::gif && !enable_favorites(p))
        return false;
    p.active = true;
    message_action_bar_close(&g_app.message_action_bar);
    if (switch_tab(p, kind))
        return true;
    application_media_picker_close();
    return false;
}
bool application_media_picker_open_reaction(u64 message_id, UiRect anchor) noexcept {
    auto* ui = g_app.account_ui;
    if (!message_id || !ui_rect_valid(anchor) || !account_ready() || !unobstructed())
        return false;
    const auto found = message_store_find(g_app.message_view.store, message_id);
    const auto* message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    const auto* channel = message ? chat_state_store_find_channel(ui->chat_state, message->channel_id) : nullptr;
    if (!message || !channel || channel->id != ui->active_channel_id ||
        message->delivery != MessageDeliveryState::authoritative)
        return false;
    auto* owner = ensure_owner();
    if (!owner)
        return false;
    auto& p = *owner;
    // Add Reaction on the open popout's own message closes it, as in the official client.
    if (p.active && p.scene.active && p.reaction_message_id == message_id) {
        application_media_picker_close();
        return true;
    }
    if (p.active)
        application_media_picker_close();
    if (!rebind(p) || p.generation == ~u32{0})
        return false;
    // Keep the composer picker's query for its next open.
    const auto saved = message_composer_text(p.edit);
    p.composer_kind = p.kind;
    p.tab_query_bytes[static_cast<u32>(p.kind)] = static_cast<u16>(saved.bytes);
    CopyMemory(p.tab_query[static_cast<u32>(p.kind)], saved.data, saved.bytes);
    message_composer_clear(&p.edit);
    p.reaction_message_id = message_id;
    p.reaction_anchor = anchor;
    p.reaction_scope = {g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
                        ui->chat_state.self_user_id, channel->id, channel->generation, message_id};
    p.kind = UiMediaPickerKind::emoji;
    p.favorites_view = p.wanted = false;
    p.due_at = 0;
    ++p.generation;
    p.active = true;
    application_gif_favorites_set_visible(false);
    application_emoji_preferences_open();
    message_action_bar_close(&g_app.message_action_bar);
    if (open_emoji_model(p)) {
        // The official client focuses the popout's search so typing filters at once.
        focus(p, true);
        if (repaint())
            return true;
    }
    application_media_picker_close();
    return false;
}
void application_media_picker_reaction_changed() noexcept {
    auto* p = g_app.media_picker;
    if (p && p->active && p->reaction_message_id && !repaint())
        application_media_picker_close();
}
void application_media_picker_close() noexcept {
    auto* p = g_app.media_picker;
    if (!p)
        return;
    application_surface_motion_retire(RendererLayerSurface::gallery);
    // The scene's storage is released below; its tab keeps the scroll offset.
    if (!p->reaction_message_id && p->scene.active)
        p->tab_scroll[static_cast<u32>(p->kind)] = p->scene.scroll_dips;
    cancel(*p);
    p->active = p->wanted = false;
    focus(*p, false);
    if (p->reaction_message_id) {
        // Return the shared search field and kind to the composer picker.
        const u32 kind = static_cast<u32>(p->composer_kind);
        message_composer_restore(&p->edit, 0, {p->tab_query[kind], p->tab_query_bytes[kind]},
                                 p->tab_query_bytes[kind], p->tab_query_bytes[kind]);
        p->kind = p->composer_kind;
        p->reaction_message_id = 0;
        reset_value(&p->reaction_scope);
        p->reaction_error = false;
    }
    revoke_capture(*p);
    p->hover_at = 0;
    if (g_app.context_focus == &p->scene.widgets)
        g_app.context_focus = nullptr;
    // A closed picker keeps no widget, projection or masonry pages.
    g_app.renderer.media_picker_scene = nullptr;
    ui_media_picker_scene_release(&p->scene);
    reaction_picker_release(&p->emoji);
    application_gif_favorites_set_visible(false);
    application_request_frame();
}
void application_media_picker_release() noexcept {
    auto* p = g_app.media_picker;
    if (!p)
        return;
    application_media_picker_close();
    application_composer_text_input_suspend();
    g_app.media_picker = nullptr;
    ui_media_picker_scene_release(&p->scene);
    reaction_picker_release(&p->emoji);
    message_composer_release(&p->edit);
    CatalogBank* banks[]{p->gif, p->categories, p->guild, p->packs};
    for (auto* bank : banks)
        if (bank)
            VirtualFree(bank, 0, MEM_RELEASE);
    if (p->filtered)
        VirtualFree(p->filtered, 0, MEM_RELEASE);
    if (p->saved_preview_mask)
        VirtualFree(p->saved_preview_mask, 0, MEM_RELEASE);
    VirtualFree(p, 0, MEM_RELEASE);
    sticker_store_clear();
}
void application_media_picker_gateway(const GatewayClientEvent& event, Utf8View body) noexcept {
    if (event.kind == GatewayClientEventKind::ready) {
        sticker_store_clear();
        return;
    }
    if (event.kind != GatewayClientEventKind::dispatch)
        return;
    if (event.dispatch_kind == GatewayClientDispatchKind::guild_delete)
        sticker_list_remove(event.state.id);
    else if (event.dispatch_kind == GatewayClientDispatchKind::guild_stickers_update) {
        // An update without its list (too large) leaves the server to REST.
        if (!body.bytes || body.bytes != event.dispatch_payload_bytes ||
            !sticker_list_put(event.member_list.guild_id, body))
            sticker_list_remove(event.member_list.guild_id);
    } else
        return;
    const auto* p = g_app.media_picker;
    if (p && p->active && p->kind == UiMediaPickerKind::sticker)
        application_request_frame();
}
bool application_media_picker_active() noexcept {
    return g_app.media_picker && g_app.media_picker->active && owns_current(*g_app.media_picker) &&
           front(g_app.media_picker);
}
void application_media_picker_emoji_preferences_changed() noexcept {
    auto* p = g_app.media_picker;
    if (!p || !p->active || p->kind != UiMediaPickerKind::emoji || !p->emoji.message_id)
        return;
    if (!reaction_picker_refresh_preferences(&p->emoji).ok() || !emoji_projection(*p) || !repaint())
        application_media_picker_close();
}
void application_media_picker_favorites_changed() noexcept {
    auto* p = g_app.media_picker;
    if (!p || !p->favorites_enabled)
        return;
    if (!favorites_refresh(*p)) {
        application_media_picker_close();
        return;
    }
    if (p->active && p->kind == UiMediaPickerKind::gif && !repaint())
        application_media_picker_close();
}
bool application_media_picker_synchronize() noexcept {
    auto* p = g_app.media_picker;
    if (!p || !p->active) {
        g_app.renderer.media_picker_scene = nullptr;
        return true;
    }
    if (!front(p) || p->account_id != g_app.account_ui->chat_state.self_user_id ||
        p->channel_id != g_app.account_ui->active_channel_id) {
        application_media_picker_close();
        return true;
    }
    p->source.account_scope = p->scope;
    p->source.query_generation = p->generation;
    const auto& state = g_app.account_ui->chat_state;
    const u64 emoji_revision = (static_cast<u64>(state.emoji_mutations) << 32) | state.permission_mutations;
    if (p->kind == UiMediaPickerKind::emoji &&
        (p->emoji_state_revision != emoji_revision || p->emoji_guild_revision != state.guild_mutations)) {
        revoke_capture(*p);
        ui_media_picker_scene_close(&p->scene);
        if (!open_emoji_model(*p))
            emoji_unavailable(*p);
    }
    if (p->kind == UiMediaPickerKind::sticker && p->sticker_store_revision != g_sticker_store.revision) {
        if (!build_server_stickers(*p) || !projection(*p))
            return false;
        if (p->phase != UiMediaPickerPhase::loading && p->phase != UiMediaPickerPhase::failed)
            p->phase = p->source.count ? UiMediaPickerPhase::ready : UiMediaPickerPhase::empty;
    }
    p->source.sections = {p->sections, p->kind != UiMediaPickerKind::gif ? p->section_count : 0};
    if (p->kind == UiMediaPickerKind::gif && !favorites_refresh(*p))
        return false;
    if (p->kind == UiMediaPickerKind::gif && !p->favorites_view)
        p->source.count =
            (gif_bank(*p) ? gif_bank(*p)->count : 0) + (landing(*p) && p->favorites_enabled ? 1 : 0);
    UiMediaPickerContent content{};
    content.edit = {
        message_composer_text(p->edit), p->edit.anchor, p->edit.caret, true, p->focused, p->caret_visible};
    content.account_scope = p->scope;
    content.query_generation = p->generation;
    content.kind = p->kind;
    content.phase = p->phase;
    if (p->kind == UiMediaPickerKind::gif && p->favorites_view) {
        const auto saved = application_gif_favorites_view();
        content.phase = saved.authoritative
                            ? (p->source.count ? UiMediaPickerPhase::ready : UiMediaPickerPhase::empty)
                        : saved.failed ? UiMediaPickerPhase::failed
                                       : UiMediaPickerPhase::loading;
        p->phase = content.phase;
        p->status = saved.failed || saved.loading || !saved.authoritative ? saved.status
                    : !p->source.count
                        ? (p->edit.bytes ? utf8_literal("No favorites match your search.")
                                         : utf8_literal("Save GIFs with the star to find them here."))
                        : utf8_literal("Your saved GIFs");
    }
    const bool reaction = p->reaction_message_id != 0;
    if (reaction && !p->reaction_error && p->source.count) {
        bool error = false;
        const auto status = application_message_reaction_status(p->reaction_message_id, &error);
        p->status = status.bytes ? status : kReactionHint;
    }
    content.available_tabs = reaction ? ui_media_picker_tab_bit(UiMediaPickerKind::emoji) : 7;
    content.variant = reaction ? UiMediaPickerVariant::reaction : UiMediaPickerVariant::expression;
    content.placeholder =
        p->kind == UiMediaPickerKind::gif
            ? (p->favorites_view ? utf8_literal("Search favorites") : utf8_literal("Search Klipy"))
        : p->kind == UiMediaPickerKind::emoji
            ? (reaction ? utf8_literal("Find the perfect reaction") : utf8_literal("Find the perfect emoji"))
                                              : utf8_literal("Search stickers");
    content.status = p->status;
    content.back = p->kind == UiMediaPickerKind::gif &&
                   (p->favorites_view || p->operation != MediaCatalogOperation::gif_categories);
    auto prior = p->content;
    prior.revision = 0;
    content.revision =
        bytes_equal(&prior, &content, sizeof(content)) ? p->content.revision : p->content.revision + 1;
    p->content = content;
    auto viewport = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::content);
    viewport.right = static_cast<float>(g_app.renderer.width) * 96.f / maximum(g_app.renderer.dpi, 1u);
    const auto* composer = ui_widget_manager_layout_node(g_app.message_composer_scene.widgets,
                                                         g_app.message_composer_scene.surface);
    if (const auto* title =
            ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::title_bar)))
        viewport.top = maximum(viewport.top, title->rectangle.bottom);
    // The composer picker sits above the composer; the reaction popout may
    // use the whole client area below the title bar beside its message.
    if (reaction) {
        viewport.left = 0;
        viewport.bottom = static_cast<float>(g_app.renderer.height) * 96.f / maximum(g_app.renderer.dpi, 1u);
    } else if (composer)
        viewport.bottom = minimum(viewport.bottom, composer->rectangle.top);
    if (p->resizing && !ui_rect_equal(viewport, p->scene.viewport))
        revoke_capture(*p);
    const u32 prior_generation = p->scene.generation;
    if ((!reaction && !composer) ||
        !ui_media_picker_scene_update(&p->scene, &p->source, &p->content, viewport,
                                      reaction ? p->reaction_anchor : composer->rectangle, nullptr,
                                      p->preferred_width))
        return false;
    // A saved-state response rebuilds cell handles. Restore pointer state
    // once for that update so a stationary cursor keeps its action hint.
    if (prior_generation != p->scene.generation && !g_app.keyboard_focus_visible) {
        POINT pointer{};
        if (GetCursorPos(&pointer) && application_screen_to_content(&pointer)) {
            const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);
            ui_media_picker_scene_hover(&p->scene, pointer.x * scale, pointer.y * scale,
                                        clock_monotonic_milliseconds());
        }
    }
    g_app.renderer.media_picker_scene = &p->scene;
    return true;
}
bool application_media_picker_pending(u64 now) noexcept {
    const auto* p = g_app.media_picker;
    return p && p->active && owns_current(*p) && p->wanted && !p->submitted_id && now >= p->due_at;
}
bool application_media_picker_submit(u64 id, u64 now) noexcept {
    if (!id || !application_media_picker_pending(now) || !g_app.account.api.media_catalog)
        return false;
    auto& p = *g_app.media_picker;
    const auto text = message_composer_text(p.edit);
    CopyMemory(p.submitted_query, text.data, text.bytes);
    const bool search = p.operation == MediaCatalogOperation::gif_search ||
                        p.operation == MediaCatalogOperation::gif_suggestions;
    MediaCatalogQuery query{p.operation,
                            search ? Utf8View{p.submitted_query, text.bytes} : Utf8View{},
                            utf8_literal("en-US"),
                            p.operation == MediaCatalogOperation::guild_stickers ? p.guild_id : 0,
                            0u,
                            MediaCatalogFormat::tiny_webp};
    const auto submitted = g_app.account.api.media_catalog(
        g_app.account.instance, AccountMediaCatalogOperation::submit, &query, id);
    if (!submitted.ok()) {
        p.admission_busy = submitted.error == RestClientSubmitError::busy;
        if (!p.admission_busy) {
            p.wanted = false;
            p.phase = UiMediaPickerPhase::failed;
            p.status = utf8_literal("Couldn't load. Press Enter to retry.");
            repaint();
        }
        return false;
    }
    p.submitted_id = id;
    p.submitted_generation = p.generation;
    p.submitted_operation = p.operation;
    p.wanted = false;
    p.admission_busy = false;
    return repaint();
}
bool application_media_picker_response(const RestClientResponse& response, Utf8View body) noexcept {
    auto* p = g_app.media_picker;
    if (!p || !p->submitted_id || response.request_id != p->submitted_id)
        return false;
    const auto generation = p->submitted_generation;
    const auto operation = p->submitted_operation;
    p->submitted_id = 0;
    if (!p->active || !owns_current(*p) || p->generation != generation ||
        p->account_id != g_app.account_ui->chat_state.self_user_id)
        return true;
    // 429: wait out Retry-After (and an exhausted bucket's reset). A request
    // the local limiter refused waits its own floor. Either way the view keeps
    // a notice instead of failing, up to kCatalogRateRetries.
    const bool limited = response.transport_ok() && response.rate_limited && response.status_code == 429 &&
                         response.retry_at_ms;
    const bool refused = response.error == RestClientError::rate;
    if ((limited || refused) && p->retries < kCatalogRateRetries) {
        ++p->retries;
        const u64 now = clock_monotonic_milliseconds();
        u64 due = refused ? deadline_after(now, kCatalogLocalRateRetryMs) : response.retry_at_ms;
        if (limited && (response.rate.fields & rate_limit_field_remaining) && !response.rate.remaining &&
            response.rate.reset_after_ms)
            due = maximum(due, deadline_after(now, response.rate.reset_after_ms));
        p->wanted = true;
        p->due_at = maximum(due, deadline_after(now, 1));
        p->status = utf8_literal("Too many requests. Retrying…");
        return repaint();
    }
    if (limited || refused) {
        p->wanted = false;
        p->phase = UiMediaPickerPhase::failed;
        p->status = utf8_literal("Too many requests. Try again in a moment.");
        return repaint();
    }
    if (operation == MediaCatalogOperation::guild_stickers) {
        // The current server's list joins the session store like a Gateway list.
        p->sticker_guild_done = response.transport_ok() && response.status_code == 200 &&
                                sticker_list_put(p->guild_id, body);
        if (!build_server_stickers(*p))
            return false;
        if (!p->packs) {
            request(*p, MediaCatalogOperation::sticker_packs);
            return repaint();
        }
        if (!projection(*p))
            return false;
        p->phase = p->source.count ? UiMediaPickerPhase::ready : UiMediaPickerPhase::empty;
        p->status = p->source.count ? utf8_literal("Choose a sticker") : utf8_literal("No matches");
        return repaint();
    }
    CatalogBank* candidate = nullptr;
    if (response.transport_ok() && response.status_code == 200) {
        auto* scratch =
            static_cast<u32*>(VirtualAlloc(nullptr, kMediaCatalogStructuralCapacity * sizeof(u32),
                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (scratch) {
            const auto measured =
                media_catalog_measure(operation, body, {scratch, kMediaCatalogStructuralCapacity}, 32);
            // A page longer than the cap keeps its first records.
            if (measured.ok() &&
                g_app.media_picker_item_serial <= ~u64{0} - measured.item_count) {
                const bool stickers = operation == MediaCatalogOperation::guild_stickers ||
                                      operation == MediaCatalogOperation::sticker_packs;
                const u32 preview_bytes = stickers ? measured.item_count * 64 : 0;
                const u32 mask_bytes = (measured.item_count + 7) / 8;
                const u32 bytes = sizeof(CatalogBank) + measured.item_count * sizeof(MediaCatalogItem) +
                                  measured.text_bytes + preview_bytes + mask_bytes;
                candidate = static_cast<CatalogBank*>(
                    VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (candidate) {
                    candidate->items = reinterpret_cast<MediaCatalogItem*>(candidate + 1);
                    candidate->text = reinterpret_cast<char*>(candidate->items + measured.item_count);
                    candidate->preview_mask =
                        reinterpret_cast<u8*>(candidate->text + measured.text_bytes + preview_bytes);
                    const auto decoded = media_catalog_decode(
                        operation, body, {scratch, kMediaCatalogStructuralCapacity}, 32,
                        {candidate->items, measured.item_count}, {candidate->text, measured.text_bytes});
                    if (!decoded.ok()) {
                        VirtualFree(candidate, 0, MEM_RELEASE);
                        candidate = nullptr;
                    } else {
                        candidate->count = decoded.item_count;
                        // URL policy is checked once per accepted bank. Visibility and
                        // animation paths use one bit; the account worker revalidates.
                        u8 target[kCdnMaximumTargetBytes];
                        for (u32 i = 0; !stickers && i < decoded.item_count; ++i)
                            if (candidate->items[i].available &&
                                parse_catalog_preview_url(candidate->items[i].preview_url, {target, sizeof(target)},
                                                          format(candidate->items[i]))
                                    .ok())
                                candidate->preview_mask[i >> 3] |= static_cast<u8>(1u << (i & 7));
                        if (stickers &&
                            !sticker_previews(candidate, candidate->text + measured.text_bytes, preview_bytes)) {
                            VirtualFree(candidate, 0, MEM_RELEASE);
                            candidate = nullptr;
                        } else {
                            candidate->text_bytes = decoded.text_bytes + preview_bytes;
                            candidate->allocation_bytes = bytes;
                            candidate->generation = p->generation;
                            candidate->operation = operation;
                            candidate->first_id = g_app.media_picker_item_serial + 1;
                            g_app.media_picker_item_serial += decoded.item_count;
                        }
                    }
                }
            }
            VirtualFree(scratch, 0, MEM_RELEASE);
        }
    }
    if (candidate) {
        auto** slot = operation == MediaCatalogOperation::sticker_packs    ? &p->packs
                      : operation == MediaCatalogOperation::gif_categories ? &p->categories
                                                                           : &p->gif;
        auto* old = *slot;
        revoke_capture(*p);
        ui_media_picker_scene_close(&p->scene);
        *slot = candidate;
        if (old)
            VirtualFree(old, 0, MEM_RELEASE);
        ++p->source.revision;
        if (!projection(*p))
            return false;
        p->phase = p->source.count ? UiMediaPickerPhase::ready : UiMediaPickerPhase::empty;
        p->status = p->source.count ? utf8_literal("Choose an item") : utf8_literal("No matches");
    } else {
        p->phase = UiMediaPickerPhase::failed;
        p->status = utf8_literal("Couldn't load. Press Enter to retry.");
    }
    return repaint();
}
MessageComposer* application_media_picker_edit_model() noexcept {
    return g_app.media_picker ? &g_app.media_picker->edit : nullptr;
}
bool application_media_picker_edit_focused() noexcept {
    return application_media_picker_active() && g_app.media_picker->focused;
}
UiRect application_media_picker_edit_bounds() noexcept {
    return application_media_picker_edit_focused() ? g_app.media_picker->scene.text : UiRect{};
}
bool application_media_picker_edit_changed(bool content) noexcept {
    auto* p = g_app.media_picker;
    if (!p)
        return false;
    p->caret_visible = p->focused;
    p->blink_at =
        p->focused ? deadline_after(clock_monotonic_milliseconds(), kDefaultCaretBlinkMilliseconds) : 0;
    if (content) {
        revoke_capture(*p);
        if (p->kind == UiMediaPickerKind::gif && p->favorites_view) {
            if (!projection(*p))
                return false;
            ++p->source.revision;
            p->scene.scroll_dips = 0;
        } else if (p->kind == UiMediaPickerKind::gif)
            request(*p,
                    p->edit.bytes ? MediaCatalogOperation::gif_search
                                  : MediaCatalogOperation::gif_categories,
                    true);
        else if (p->kind == UiMediaPickerKind::emoji) {
            if (!p->emoji.storage.base || !emoji_projection(*p))
                emoji_unavailable(*p);
            p->scene.scroll_dips = 0;
        } else {
            if (!projection(*p))
                return false;
            ++p->source.revision;
            p->scene.scroll_dips = 0;
        }
    }
    return repaint();
}
u64 application_media_picker_next_wake() noexcept {
    const auto* p = g_app.media_picker;
    return p && p->active && p->wanted && !p->submitted_id && !p->admission_busy &&
                   g_app.message_history_request_owner == MessageHistoryRequestOwner::none
               ? p->due_at
               : 0;
}
u64 application_media_picker_caret_wake() noexcept {
    const auto* p = g_app.media_picker;
    return application_media_picker_active() ? earliest_deadline(p->focused ? p->blink_at : 0, p->hover_at)
                                             : 0;
}
void application_media_picker_tick(u64 now) noexcept {
    auto* p = g_app.media_picker;
    if (p && application_media_picker_edit_focused() && deadline_due(now, p->blink_at)) {
        p->caret_visible = !p->caret_visible;
        p->blink_at = deadline_after(now, kDefaultCaretBlinkMilliseconds);
        repaint();
    }
    if (p && application_media_picker_active() && deadline_due(now, p->hover_at)) {
        p->hover_at = 0;
        repaint();
    }
}
bool application_media_picker_preview_request(u32 cell, AccountMediaCatalogPreviewRequest* out) noexcept {
    const auto* p = g_app.media_picker;
    if (!out || !p || !application_media_picker_active() || p->kind == UiMediaPickerKind::emoji)
        return false;
    const CatalogBank* bank = nullptr;
    u32 local = 0;
    MediaCatalogItem temporary{};
    const MediaCatalogItem* item = nullptr;
    if (cell >= p->scene.cell_count) {
        // A sticker pack's rail icon: its cover sticker's preview.
        UiMediaPickerRailMediaSlot rail{};
        if (p->kind != UiMediaPickerKind::sticker || !p->packs ||
            !ui_media_picker_scene_rail_media(p->scene, cell - p->scene.cell_count, &rail) || !rail.asset_id)
            return false;
        for (u32 i = 0; !item && i < p->section_count; ++i)
            if (p->sections[i].asset_id == rail.asset_id && p->section_cover[i] < p->packs->count) {
                bank = p->packs;
                local = p->section_cover[i];
                item = &bank->items[local];
            }
    } else {
        UiMediaPickerMediaSlot slot{};
        if (!ui_media_picker_scene_media(p->scene, cell, &slot) || slot.target.source_index >= p->source.count)
            return false;
        item = item_at(*p, slot.target.source_index, &bank, &local, &temporary);
    }
    if (!item || !preview_admitted(*bank, local))
        return false;
    reset_value(out);
    out->url = item->preview_url;
    out->key = media_asset_catalog_preview(bank->first_id + local, bank->generation, format(*item),
                                           (item->sticker_format == 2 || item->sticker_format == 3));
    out->catalog_scope = p->scope;
    out->catalog_generation = bank->generation;
    // Fit within 512 px keeping the aspect ratio the proxy sizes from.
    u64 width = item->width ? item->width : 256, height = item->height ? item->height : 256;
    const u64 longest = maximum(width, height);
    if (longest > 512) {
        width = maximum<u64>(1, width * 512 / longest);
        height = maximum<u64>(1, height * 512 / longest);
    }
    out->width = static_cast<u16>(width);
    out->height = static_cast<u16>(height);
    out->format = format(*item);
    return true;
}
bool application_media_picker_preview_current(MediaAssetKey key, u64 scope, u32 generation) noexcept {
    const auto* p = g_app.media_picker;
    if (!p || p->scope != scope || key.variant != generation || !application_media_picker_active())
        return false;
    return application_media_picker_preview_visible(key);
}
bool application_media_picker_preview_visible(MediaAssetKey key) noexcept {
    const auto* p = g_app.media_picker;
    if (!p || !application_media_picker_active())
        return false;
    for (u32 i = 0; i < p->scene.cell_count + p->scene.rail_count; ++i) {
        AccountMediaCatalogPreviewRequest request{};
        if (application_media_picker_preview_request(i, &request) &&
            media_asset_key_equal(request.key, key))
            return true;
    }
    return false;
}
u32 application_media_picker_owner_bytes() noexcept {
    const auto* p = g_app.media_picker;
    if (!p)
        return 0;
    u32 bytes = sizeof(*p) + p->filtered_capacity * sizeof(u32);
    bytes += static_cast<u32>(p->emoji.storage.offset);
    CatalogBank* banks[]{p->gif, p->categories, p->guild, p->packs};
    for (auto* bank : banks)
        if (bank)
            bytes += bank->allocation_bytes;
    bytes += p->saved_mask_bytes;
    return bytes;
}

bool application_media_picker_preview_owner_current(MediaAssetKey key, u64 scope, u32 generation) noexcept {
    const auto* p = g_app.media_picker;
    if (!p || !application_media_picker_active() || p->kind == UiMediaPickerKind::emoji ||
        p->scope != scope || !key.id || !generation || key.variant != generation)
        return false;
    const CatalogBank* banks[]{p->kind == UiMediaPickerKind::gif ? (p->favorites_view ? &p->saved : gif_bank(*p))
                                                                 : p->guild,
                               p->kind == UiMediaPickerKind::sticker
                                   ? p->packs
                                   : (landing(*p) && p->favorites_enabled ? &p->favorites_tile : nullptr)};
    for (const auto* bank : banks) {
        if (!bank || bank->generation != generation || key.id < bank->first_id ||
            key.id - bank->first_id >= bank->count)
            continue;
        const u32 index = static_cast<u32>(key.id - bank->first_id);
        if (!preview_admitted(*bank, index))
            return false;
        MediaCatalogItem temporary{};
        if (bank == &p->saved) {
            const auto saved = application_gif_favorites_view();
            if (index >= saved.records.count)
                return false;
            favorite_item(saved.records[index], &temporary);
        } else if (bank == &p->favorites_tile)
            temporary = p->tile_item;
        const auto& item = bank == &p->saved || bank == &p->favorites_tile ? temporary : bank->items[index];
        return media_asset_key_equal(
            key, media_asset_catalog_preview(key.id, generation, format(item), (item.sticker_format == 2 || item.sticker_format == 3)));
    }
    return false;
}

bool application_media_picker_preview_rejected(MediaAssetKey key, u64 scope, u32 generation) noexcept {
    auto* p = g_app.media_picker;
    if (!application_media_picker_preview_owner_current(key, scope, generation))
        return false;
    CatalogBank* banks[]{p->gif, p->categories, p->guild, p->packs, &p->saved, &p->favorites_tile};
    for (auto* bank : banks) {
        if (!bank || bank->generation != generation || key.id < bank->first_id ||
            key.id - bank->first_id >= bank->count || !bank->preview_mask)
            continue;
        const u32 index = static_cast<u32>(key.id - bank->first_id);
        bank->preview_mask[index >> 3] &= static_cast<u8>(~(1u << (index & 7)));
        ++p->source.revision;
        application_request_frame();
        return true;
    }
    return false;
}
namespace {
bool picker_edit_ready(const TextInputTarget& target) noexcept {
    const auto* p = g_app.media_picker;
    return p && application_media_picker_edit_focused() && target.context == &p->edit &&
           target.identity == p->edit_identity;
}
using PickerEditInput = ForwardedInput<picker_edit_ready, text_input_single_line_forbidden>;
bool scroll_read(void* opaque, u64 scope, UiScrollRange* output, bool rail) noexcept {
    if (!output || opaque != g_app.media_picker || !application_media_picker_active())
        return false;
    const auto& p = *g_app.media_picker;
    if (p.scope != scope || !ui_media_picker_scene_current(p.scene))
        return false;
    const auto viewport = rail ? p.scene.rail : p.scene.body;
    if (!ui_rect_has_area(viewport))
        return false;
    const auto scrollbar = ui_media_picker_scene_scrollbar(p.scene, rail);
    if (!scrollbar.visible)
        return false;
    *output = {viewport,
               rail ? p.scene.rail_scroll_dips : p.scene.scroll_dips,
               scrollbar.maximum_scroll,
               kUiScrollLineDips,
               1,
               0,
               (static_cast<u64>(p.generation) << 32) | p.source.revision};
    return true;
}
bool body_scroll_read(void* opaque, u64 scope, UiScrollRange* out) noexcept {
    return scroll_read(opaque, scope, out, false);
}
bool rail_scroll_read(void* opaque, u64 scope, UiScrollRange* out) noexcept {
    return scroll_read(opaque, scope, out, true);
}
bool scroll_write(void* opaque, u64 scope, double offset, bool* changed, bool rail) noexcept {
    UiScrollRange range{};
    if (!changed || !scroll_read(opaque, scope, &range, rail))
        return false;
    auto& p = *g_app.media_picker;
    *changed = ui_media_picker_scene_scroll(&p.scene, static_cast<float>(offset - range.offset), rail);
    if (*changed) {
        p.hover_at = 0;
        if (p.focused)
            ui_widget_manager_focus(&p.scene.widgets, p.scene.edit);
        application_composer_text_input_notify_layout();
        application_request_frame();
    }
    return true;
}
bool body_scroll_write(void* opaque, u64 scope, double offset, bool* changed) noexcept {
    return scroll_write(opaque, scope, offset, changed, false);
}
bool rail_scroll_write(void* opaque, u64 scope, double offset, bool* changed) noexcept {
    return scroll_write(opaque, scope, offset, changed, true);
}
void revoke_capture(ApplicationMediaPicker& p) noexcept {
    const bool was_resizing = p.resizing;
    p.captured = p.dragging = p.scrollbar_dragging = p.resizing = false;
    ui_media_picker_scene_set_scrollbar_state(&p.scene, false, UiScrollbarVisualState::normal);
    ui_media_picker_scene_set_scrollbar_state(&p.scene, true, UiScrollbarVisualState::normal);
    ui_media_picker_scene_cancel_capture(&p.scene);
    if (GetCapture() == g_app.window)
        ReleaseCapture();
    if (was_resizing) {
        const u16 width = ui_splitter_persisted_width(p.preferred_width);
        if (g_app.preferences.expression_picker_width != width) {
            g_app.preferences.expression_picker_width = width;
            application_settings_commit_preferences();
        }
    }
}
UiMediaPickerTarget focused_target(const ApplicationMediaPicker& p) noexcept {
    const auto* node = ui_widget_manager_layout_node(p.scene.widgets, p.scene.widgets.focused);
    if (!node)
        return {};
    const auto r = node->rectangle;
    return ui_media_picker_scene_hit_test(p.scene, (r.left + r.right) * .5f, (r.top + r.bottom) * .5f);
}
void focus_selected(ApplicationMediaPicker& p) noexcept {
    const auto cells = ui_media_picker_scene_cells(p.scene);
    for (u32 i = 0; i < cells.count; ++i) {
        const auto& cell = cells[i];
        if (cell.source_index != p.scene.selected_index)
            continue;
        ui_widget_manager_focus(&p.scene.widgets, cell.widget);
        break;
    }
    g_app.context_focus = &p.scene.widgets;
}
// Moves keyboard focus from the search field to the first result.
void focus_results(ApplicationMediaPicker& p) noexcept {
    focus(p, false);
    ui_media_picker_scene_edge_selection(&p.scene, false);
    focus_selected(p);
}
bool edit_pointer(ApplicationMediaPicker& p, float x, float y, bool extend) noexcept {
    u32 position = 0, byte = 0;
    if (!text_edit_layout_hit_test(g_app.renderer.media_picker_layout, x, y, &position) ||
        !utf8_byte_offset_for_utf16_position(message_composer_text(p.edit), position, &byte))
        return false;
    message_composer_set_selection(&p.edit, extend ? p.edit.anchor : byte, byte);
    return application_media_picker_edit_changed(false);
}
} // namespace

TextInputTarget application_media_picker_edit_target() noexcept {
    const auto* p = g_app.media_picker;
    return p ? TextInputTarget{&g_app.media_picker->edit, &PickerEditInput::operations, p->edit_identity}
             : TextInputTarget{};
}
void application_media_picker_edit_cancel_native_group(const TextInputTarget& target) noexcept {
    const auto* p = g_app.media_picker;
    if (!p || target.operations != &PickerEditInput::operations || target.context != &p->edit ||
        target.identity != p->edit_identity)
        return;
    PickerEditInput::abandon(target);
}

bool application_media_picker_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    auto* p = g_app.media_picker;
    if (p && ui_input_consume_key_tail(&p->held_key, {kind, key, repeated}))
        return true;
    if (!application_media_picker_active())
        return false;
    const bool raw_control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool control = raw_control && !alt, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (kind == UiInputKind::character) {
        if (!p->focused || control || (alt && !raw_control) || key < 32 || (key >= 127 && key <= 159) ||
            key == 0x2028 || key == 0x2029 || key > 0xffff)
            return true;
        if (!p->edit.history)
            message_composer_enable_history(&p->edit, 2048);
        if (message_composer_insert_utf16(&p->edit, static_cast<u16>(key),
                                          clock_monotonic_milliseconds()) == MessageComposerResult::changed)
            application_media_picker_edit_changed(true);
        return true;
    }
    if (kind == UiInputKind::key_up)
        return true;
    if (kind != UiInputKind::key_down)
        return false;
    if (key == VK_ESCAPE) {
        p->held_key = key;
        application_media_picker_close();
        return true;
    }
    if (key == VK_TAB) {
        p->held_key = key;
        focus(*p, false);
        const auto widget = ui_widget_manager_move_focus(&p->scene.widgets, shift);
        if (widget.generation && widget.slot == p->scene.edit.slot)
            focus(*p, true);
        else
            g_app.context_focus = &p->scene.widgets;
        repaint();
        // Content focus changes keep the geometry generation, but the scene
        // reconciles selection focus. Preserve explicit Tab navigation after
        // that reconciliation, including Shift+Tab back to the tab strip.
        if (!p->focused && widget.generation)
            ui_widget_manager_focus(&p->scene.widgets, widget);
        return true;
    }
    if (key == VK_RETURN || (key == VK_SPACE && !p->focused)) {
        p->held_key = key;
        if (p->favorites_view && p->phase == UiMediaPickerPhase::failed) {
            application_gif_favorites_set_visible(false);
            application_gif_favorites_open();
            repaint();
        } else if (p->phase == UiMediaPickerPhase::failed) {
            request(*p, p->operation);
            repaint();
        } else if (p->focused) {
            if (p->kind == UiMediaPickerKind::gif && !p->favorites_view)
                request(*p, p->edit.bytes ? MediaCatalogOperation::gif_search
                                          : MediaCatalogOperation::gif_categories);
            else
                focus_results(*p);
            repaint();
        } else
            activate(*p, focused_target(*p));
        return true;
    }
    if (!p->focused) {
        if (key == 'F' && !control && !alt) {
            p->held_key = key;
            activate(*p, ui_media_picker_scene_selected_favorite_target(p->scene));
            return true;
        }
        bool changed = false;
        if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN)
            changed = ui_media_picker_scene_move_selection(&p->scene,
                                                           key == VK_LEFT    ? -1
                                                           : key == VK_RIGHT ? 1
                                                                             : 0,
                                                           key == VK_UP     ? -1
                                                           : key == VK_DOWN ? 1
                                                                            : 0);
        else if (key == VK_PRIOR || key == VK_NEXT)
            changed = ui_media_picker_scene_page_selection(&p->scene, key == VK_PRIOR);
        else if (key == VK_HOME || key == VK_END)
            // Home/End reach the row's ends; with Ctrl the grid's.
            changed = control ? ui_media_picker_scene_edge_selection(&p->scene, key == VK_END)
                              : ui_media_picker_scene_row_edge_selection(&p->scene, key == VK_END);
        if (changed) {
            focus_selected(*p);
            repaint();
        }
        return true;
    }
    if (!p->edit.history)
        message_composer_enable_history(&p->edit, 2048);
    const u32 revision = p->edit.text_revision;
    MessageComposerResult result = MessageComposerResult::unchanged;
    if (control && (key == 'C' || key == 'X')) {
        const u32 first = minimum(p->edit.anchor, p->edit.caret),
                  last = maximum(p->edit.anchor, p->edit.caret);
        if (last > first && clipboard_write_utf8(g_app.window, {p->edit.text + first, last - first}) &&
            key == 'X')
            result = message_composer_replace_range(&p->edit, first, last, {});
    } else if (control && key == 'V') {
        char text[kMediaCatalogMaximumQueryBytes];
        const auto read = clipboard_read_utf8(g_app.window, {text, sizeof(text)});
        bool valid = read.ok();
        for (u32 i = 0; valid && i < read.bytes;) {
            const auto scalar = utf8_decode_one(text + i, read.bytes - i);
            valid = scalar.valid && scalar.codepoint >= 32 &&
                    !(scalar.codepoint >= 127 && scalar.codepoint <= 159) && scalar.codepoint != 0x2028 &&
                    scalar.codepoint != 0x2029;
            if (valid)
                i += scalar.width;
        }
        if (valid)
            result = message_composer_insert_utf8(&p->edit, {text, read.bytes});
    } else if (key == VK_DOWN) {
        focus_results(*p);
        repaint();
        return true;
    } else {
        if (key == VK_BACK)
            p->held_key = key;
        message_composer_edit_key(&p->edit, key, control, shift, &result);
    }
    if (result == MessageComposerResult::changed)
        application_media_picker_edit_changed(revision != p->edit.text_revision);
    return true;
}

bool application_media_picker_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* p = g_app.media_picker;
    if (!p)
        return false;
    if (message == WM_CANCELMODE) {
        revoke_capture(*p);
        return false;
    }
    if (message == WM_KILLFOCUS) {
        focus(*p, false);
        revoke_capture(*p);
        p->hover_at = 0;
        ui_widget_manager_hover(&p->scene.widgets, ui_widget_invalid_handle());
        application_request_frame();
        return false;
    }
    if (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lparam) != g_app.window) {
        revoke_capture(*p);
        return false;
    }
    if (!application_media_picker_active())
        return false;
    if (message == WM_MOUSELEAVE) {
        p->hover_at = 0;
        const bool scrollbar_changed =
            !p->scrollbar_dragging &&
            (ui_media_picker_scene_set_scrollbar_state(&p->scene, false, UiScrollbarVisualState::normal) |
             ui_media_picker_scene_set_scrollbar_state(&p->scene, true, UiScrollbarVisualState::normal));
        if (ui_widget_manager_hover(&p->scene.widgets, ui_widget_invalid_handle()) || scrollbar_changed)
            repaint(true);
        return false;
    }
    POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    if (message == WM_SETCURSOR &&
        (LOWORD(lparam) != HTCLIENT || !GetCursorPos(&point) || !application_screen_to_content(&point)))
        return false;
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL || message == WM_CONTEXTMENU) {
        if (message == WM_CONTEXTMENU && lparam == -1)
            return true;
        if (!application_screen_to_content(&point))
            return false;
    }
    const float scale = g_app.renderer.dpi ? 96.f / g_app.renderer.dpi : 1;
    const float x = point.x * scale, y = point.y * scale;
    const bool inside = ui_rect_contains(ui_rect_intersection(p->scene.panel, p->scene.viewport), x, y);
    const bool resize_hit = !p->reaction_message_id &&
                            ui_rect_contains(p->scene.viewport, x, y) &&
                            ui_splitter_hit(p->scene.panel, UiSplitterEdge::left, x, y);
    if (p->resizing) {
        if (message == WM_SETCURSOR) {
            application_set_cursor(UiCursor::resize_we);
            return true;
        }
        if (message == WM_MOUSEMOVE || message == WM_LBUTTONUP) {
            const float maximum_width = p->resize_fixed_edge - p->scene.viewport.left -
                                        p->scene.style.metrics.viewport_inset;
            const UiSplitterLimits limits{minimum(p->scene.style.metrics.panel_width, maximum_width),
                                           maximum_width};
            p->preferred_width = ui_splitter_drag_width(
                p->resize_fixed_edge, UiSplitterEdge::left, x - p->resize_pointer_offset, limits);
            if (message == WM_LBUTTONUP)
                revoke_capture(*p);
            repaint(true);
            return true;
        }
    }
    if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) && resize_hit) {
        revoke_capture(*p);
        focus(*p, false);
        ui_scroll_host_cancel(&g_app.scroll_host);
        p->resize_fixed_edge = p->scene.panel.right;
        p->resize_pointer_offset = x - p->scene.panel.left;
        p->resizing = p->captured = true;
        p->hover_at = 0;
        SetCapture(g_app.window);
        application_set_cursor(UiCursor::resize_we);
        return true;
    }
    if (message == WM_SETCURSOR && resize_hit) {
        application_set_cursor(UiCursor::resize_we);
        return true;
    }
    const auto hit = inside ? ui_media_picker_scene_hit_test(p->scene, x, y) : UiMediaPickerTarget{};
    const auto body_scrollbar = ui_media_picker_scene_scrollbar(p->scene);
    // The category rail remains wheel/keyboard scrollable without an overlay
    // thumb stealing space and hits from its icons. Only the content has a thumb.
    const UiScrollbarGeometry rail_scrollbar{};
    if (p->scrollbar_dragging) {
        const u64 revision = (static_cast<u64>(p->generation) << 32) | p->source.revision;
        const auto geometry = p->scrollbar_rail ? rail_scrollbar : body_scrollbar;
        if (p->scrollbar_scope != p->scope || p->scrollbar_revision != revision || !geometry.visible) {
            revoke_capture(*p);
            application_request_frame();
            return true;
        }
        if (message == WM_MOUSEMOVE || message == WM_LBUTTONUP) {
            const float next = ui_scrollbar_scroll_from_pointer(geometry, y, p->scrollbar_grab);
            const float prior = p->scrollbar_rail ? p->scene.rail_scroll_dips : p->scene.scroll_dips;
            const bool changed = ui_media_picker_scene_scroll(&p->scene, next - prior, p->scrollbar_rail);
            if (message == WM_LBUTTONUP)
                revoke_capture(*p);
            if (changed)
                repaint(true);
            else
                application_request_frame();
            return true;
        }
    }
    const bool primary_press = message == WM_LBUTTONDOWN ||
                               (message == WM_LBUTTONDBLCLK && hit.action != UiMediaPickerAction::edit);
    if (primary_press && inside &&
        (ui_scrollbar_track_hit(body_scrollbar, x, y) || ui_scrollbar_track_hit(rail_scrollbar, x, y))) {
        focus(*p, false);
        ui_scroll_host_cancel(&g_app.scroll_host);
        p->scrollbar_rail = ui_scrollbar_track_hit(rail_scrollbar, x, y);
        const auto geometry = p->scrollbar_rail ? rail_scrollbar : body_scrollbar;
        p->scrollbar_grab = ui_scrollbar_thumb_hit(geometry, x, y) ? y - geometry.thumb.top
                                                                   : ui_rect_height(geometry.thumb) * .5f;
        p->scrollbar_scope = p->scope;
        p->scrollbar_revision = (static_cast<u64>(p->generation) << 32) | p->source.revision;
        p->scrollbar_dragging = p->captured = true;
        ui_media_picker_scene_set_scrollbar_state(&p->scene, p->scrollbar_rail,
                                                  UiScrollbarVisualState::pressed);
        const float next = ui_scrollbar_scroll_from_pointer(geometry, y, p->scrollbar_grab);
        const float prior = p->scrollbar_rail ? p->scene.rail_scroll_dips : p->scene.scroll_dips;
        ui_media_picker_scene_scroll(&p->scene, next - prior, p->scrollbar_rail);
        SetCapture(g_app.window);
        repaint(true);
        return true;
    }
    if (message == WM_SETCURSOR && inside) {
        application_set_cursor(ui_media_picker_scene_cursor(p->scene, x, y));
        return true;
    }
    if ((message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) && inside) {
        const bool rail = ui_rect_contains(p->scene.rail, x, y);
        if (message == WM_MOUSEWHEEL && (rail || ui_rect_contains(p->scene.body, x, y))) {
            u32 lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            ui_scroll_host_wheel(&g_app.scroll_host,
                                 {p, p->scope, rail ? rail_scroll_read : body_scroll_read,
                                  rail ? rail_scroll_write : body_scroll_write},
                                 {x, y}, static_cast<short>(HIWORD(wparam)), lines,
                                 lines == WHEEL_PAGESCROLL,
                                 clock_microseconds(g_app.clock, g_app.process_start, clock_now()), scale);
            application_request_frame();
        }
        return true;
    }
    if (primary_press) {
        if (!inside) {
            const auto control = message_composer_scene_hit_test(g_app.message_composer_scene, x, y);
            // The launcher activates on release. Keep the current picker alive
            // until then so its own same-kind toggle can close it, or a different
            // launcher can switch tabs without a close/reopen cycle.
            if (control == MessageComposerControl::gif || control == MessageComposerControl::sticker ||
                control == MessageComposerControl::emoji)
                return false;
            application_media_picker_close();
            return false;
        }
        SetFocus(g_app.window);
        revoke_capture(*p);
        if (hit.action == UiMediaPickerAction::edit) {
            focus(*p, true);
            edit_pointer(*p, x, y, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            p->dragging = true;
        } else {
            focus(*p, false);
            if (hit.action != UiMediaPickerAction::none) {
                const auto widget = ui_widget_manager_hit_test(p->scene.widgets, x, y);
                ui_widget_manager_focus(&p->scene.widgets, widget);
                ui_media_picker_scene_pointer_down(&p->scene, x, y);
            }
            g_app.context_focus = &p->scene.widgets;
            application_request_frame();
        }
        p->captured = true;
        SetCapture(g_app.window);
        return true;
    }
    if (message == WM_MOUSEMOVE && p->dragging) {
        edit_pointer(*p, x, y, true);
        return true;
    }
    if (message == WM_MOUSEMOVE) {
        const bool scrollbar_changed =
            ui_media_picker_scene_set_scrollbar_state(&p->scene, false,
                                                      ui_scrollbar_thumb_hit(body_scrollbar, x, y)
                                                          ? UiScrollbarVisualState::hovered
                                                          : UiScrollbarVisualState::normal) |
            ui_media_picker_scene_set_scrollbar_state(&p->scene, true,
                                                      ui_scrollbar_thumb_hit(rail_scrollbar, x, y)
                                                          ? UiScrollbarVisualState::hovered
                                                          : UiScrollbarVisualState::normal);
        if (scrollbar_changed)
            application_request_frame();
        if (inside) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, g_app.window, 0};
            TrackMouseEvent(&track);
            if (ui_media_picker_scene_hover(&p->scene, x, y, clock_monotonic_milliseconds())) {
                p->hover_at = hit.action == UiMediaPickerAction::item ||
                                      hit.action == UiMediaPickerAction::category ||
                                      hit.action == UiMediaPickerAction::section ||
                                      hit.action == UiMediaPickerAction::section_jump
                                  ? deadline_after(clock_monotonic_milliseconds(), 500)
                                  : 0;
                repaint(true);
            }
        } else {
            p->hover_at = 0;
            if (ui_widget_manager_hover(&p->scene.widgets, ui_widget_invalid_handle()))
                repaint(true);
        }
        return inside || p->captured;
    }
    if (message == WM_LBUTTONUP && (p->captured || inside)) {
        const bool drag = p->dragging;
        const auto selected =
            !drag && inside ? ui_media_picker_scene_pointer_up(&p->scene, x, y) : UiMediaPickerTarget{};
        revoke_capture(*p);
        if (selected.action != UiMediaPickerAction::none)
            activate(*p, selected);
        return true;
    }
    if (message == WM_LBUTTONDBLCLK && hit.action == UiMediaPickerAction::edit) {
        focus(*p, true);
        edit_pointer(*p, x, y, false);
        message_composer_select_word(&p->edit, p->edit.caret);
        application_media_picker_edit_changed(false);
        return true;
    }
    return inside && (message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
                      message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ||
                      message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
                      message == WM_XBUTTONDBLCLK || message == WM_LBUTTONDBLCLK);
}
} // namespace maicord::app_detail
