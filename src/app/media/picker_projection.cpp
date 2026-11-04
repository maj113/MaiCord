#include "picker_internal.hpp"
#include "../chat/composer/reply_draft.hpp"
#include "../shell/overlay_input.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/composer/text_input.hpp"
#include "../account/friends.hpp"
#include "../account/profile.hpp"
#include "../settings/settings.hpp"
#include "../settings/emoji_preferences.hpp"
#include "../shell/retained_projection.hpp"
#include "catalog_preview_log.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/ui/text/regional_flag.hpp"
#include "maicord/app/media/account_media.hpp"

namespace maicord::app_detail {
namespace picker_detail {

bool preview_admitted(const CatalogBank& bank, u32 index) noexcept {
    return index < bank.count && bank.preview_mask && (bank.preview_mask[index >> 3] & (1u << (index & 7)));
}

} // namespace picker_detail
namespace {

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

constexpr u32 kStickerStoreMaximumBytes = 8 * 1024 * 1024;

} // namespace
namespace picker_detail {

StickerStore g_sticker_store;
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
bool sticker_list_put(u64 guild_id, const Utf8View& json) noexcept {
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
MediaAssetKey catalog_key(u64 id, u32 generation, const MediaCatalogItem& item) noexcept {
    return media_asset_catalog_preview(id, generation, format(item),
                                       item.sticker_format == 2 || item.sticker_format == 3);
}

} // namespace picker_detail
namespace {

// Search-as-you-type waits for typing to pause before one request.
constexpr u32 kSearchDebounceMs = 250;
// The inline /gif search waits 300 ms (a trailing debounce).
constexpr u32 kInlineSearchDebounceMs = 300;

// Frees a bank the /gif cache does not hold.
void release_bank(const ApplicationMediaPicker& p, CatalogBank* bank) noexcept {
    for (const auto* cached : p.inline_cache)
        if (bank == cached)
            return;
    if (bank)
        pages_release(bank);
}
// Removes a cached query's bank, freed unless the current results borrow it.
void inline_cache_drop(ApplicationMediaPicker& p, u32 at) noexcept {
    auto* bank = p.inline_cache[at];
    for (u32 i = at; i + 1 < kInlineGifCacheCapacity; ++i) {
        p.inline_cache[i] = p.inline_cache[i + 1];
        p.inline_cache_query_bytes[i] = p.inline_cache_query_bytes[i + 1];
        CopyMemory(p.inline_cache_query[i], p.inline_cache_query[i + 1], p.inline_cache_query_bytes[i]);
    }
    p.inline_cache[kInlineGifCacheCapacity - 1] = nullptr;
    p.inline_cache_query_bytes[kInlineGifCacheCapacity - 1] = 0;
    if (bank != p.gif)
        release_bank(p, bank);
}
u32 inline_cache_index(const ApplicationMediaPicker& p, Utf8View query) noexcept {
    for (u32 i = 0; i < kInlineGifCacheCapacity && p.inline_cache[i]; ++i)
        if (utf8_equal({p.inline_cache_query[i], p.inline_cache_query_bytes[i]}, query))
            return i;
    return kInlineGifCacheCapacity;
}
// Makes `bank` the first entry, shifting the entries before `last` down over slot `last`.
void inline_cache_front(ApplicationMediaPicker& p, u32 last, const Utf8View& query,
                        CatalogBank* bank) noexcept {
    for (u32 i = last; i; --i) {
        p.inline_cache[i] = p.inline_cache[i - 1];
        p.inline_cache_query_bytes[i] = p.inline_cache_query_bytes[i - 1];
        CopyMemory(p.inline_cache_query[i], p.inline_cache_query[i - 1], p.inline_cache_query_bytes[i]);
    }
    p.inline_cache[0] = bank;
    p.inline_cache_query_bytes[0] = static_cast<u16>(query.bytes);
    CopyMemory(p.inline_cache_query[0], query.data, query.bytes);
}

} // namespace
namespace picker_detail {

// Keeps a query's results first, evicting the least recently used.
void inline_cache_keep(ApplicationMediaPicker& p, Utf8View query, CatalogBank* bank) noexcept {
    if (!bank || query.bytes > kMediaCatalogMaximumQueryBytes)
        return;
    u32 at = inline_cache_index(p, query);
    if (at < kInlineGifCacheCapacity && p.inline_cache[at] == bank)
        return;
    if (at < kInlineGifCacheCapacity)
        inline_cache_drop(p, at);
    if (p.inline_cache[kInlineGifCacheCapacity - 1])
        inline_cache_drop(p, kInlineGifCacheCapacity - 1);
    inline_cache_front(p, kInlineGifCacheCapacity - 1, query, bank);
}
// A cached query's bank, moved first; null when the query was never answered.
CatalogBank* inline_cache_take(ApplicationMediaPicker& p, Utf8View query) noexcept {
    const u32 at = inline_cache_index(p, query);
    if (at == kInlineGifCacheCapacity)
        return nullptr;
    auto* bank = p.inline_cache[at];
    inline_cache_front(p, at, query, bank);
    return bank;
}
// The account's cache goes with it; current results keep their own bank.
void inline_cache_clear(ApplicationMediaPicker& p) noexcept {
    while (p.inline_cache[0])
        inline_cache_drop(p, 0);
}
// Installs a bank as its slot's current results and projects them.
bool show_bank(ApplicationMediaPicker& p, CatalogBank** slot, CatalogBank* bank) noexcept {
    auto* old = *slot;
    revoke_capture(p);
    ui_media_picker_scene_close(&p.scene);
    *slot = bank;
    if (old != bank)
        release_bank(p, old);
    ++p.source.revision;
    if (!projection(p))
        return false;
    p.phase = p.source.count ? UiMediaPickerPhase::ready : UiMediaPickerPhase::empty;
    p.status = p.source.count ? utf8_literal("Choose an item") : utf8_literal("No matches");
    return true;
}

} // namespace picker_detail
namespace {

// The emoji picker holds every emoji the chat state can.
static_assert(kChatStateEmojiCapacity <= kReactionPickerCustomEmojiCapacity &&
              kReactionPickerResultCapacity <= kUiMediaPickerMaximumItems);

} // namespace
namespace picker_detail {

bool emoji_projection(ApplicationMediaPicker& p) noexcept {
    revoke_capture(p);
    if (!reaction_picker_refresh_search(&p.emoji).ok() ||
        p.emoji.section_count > kUiMediaPickerMaximumSections)
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
u64 emoji_state_key(const ChatStateStore& state) noexcept {
    return (static_cast<u64>(state.emoji_mutations) << 32) | state.permission_mutations;
}

} // namespace picker_detail
namespace {

void emoji_state_current(ApplicationMediaPicker& p) noexcept {
    const auto& state = g_app.account_ui->chat_state;
    p.emoji_state_revision = emoji_state_key(state);
    p.emoji_guild_revision = state.guild_mutations;
}

} // namespace
namespace picker_detail {

// The Emoji tab stays open (and the tabs usable) when its model cannot open.
void emoji_unavailable(ApplicationMediaPicker& p) noexcept {
    revoke_capture(p);
    ui_media_picker_scene_close(&p.scene);
    reaction_picker_release(&p.emoji);
    emoji_state_current(p);
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
    emoji_state_current(p);
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

} // namespace picker_detail
namespace {

Utf8View favorite_detail(Utf8View value) noexcept {
    u32 bytes = minimum(value.bytes, 256u);
    while (bytes && !utf8_validate({value.data, bytes}).valid)
        --bytes;
    return {value.data, bytes};
}

} // namespace
namespace picker_detail {

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
        if (view.records.count > kUiMediaPickerMaximumItems || p.favorite_generation == ~u32{0})
            return false;
        const u32 mask_bytes = (view.records.count + 7) / 8;
        auto* mask = mask_bytes ? static_cast<u8*>(commit_pages(mask_bytes))
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
            pages_release(p.saved_preview_mask);
        p.saved_preview_mask = mask;
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

} // namespace picker_detail
namespace {

bool reaction_current(const ApplicationMediaPicker& p) noexcept {
    const auto& scope = p.reaction_scope;
    const auto* ui = g_app.account_ui;
    if (!ui || !scope.message_id || scope.message_id != p.reaction_message_id)
        return false;
    const auto* channel = chat_state_store_find_channel(ui->chat_state, scope.channel_id);
    const auto* message = application_message_record(scope.message_id);
    return scope.account_serial == g_app.plugin_account_scope_serial &&
           scope.profile_id == g_app.account.status.profile_id &&
           scope.self_user_id == ui->chat_state.self_user_id && scope.channel_id == ui->active_channel_id &&
           channel && channel->occupied && channel->generation == scope.channel_generation && message &&
           message->channel_id == scope.channel_id && message->delivery == MessageDeliveryState::authoritative;
}

} // namespace
namespace picker_detail {

// The composer's expression picker needs a sendable composer; the reaction
// popout needs only its current message (reactions need no send access).
bool available(const ApplicationMediaPicker* p) noexcept {
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
// Inline results belong to the commands popup's option bar and show beside it.
bool unobstructed(bool inline_results) noexcept {
    return !application_settings_active() && !g_app.text_form &&
           (!g_app.popup_scene.active || (inline_results && g_app.popup_owner_kind == PopupOwnerKind::commands)) &&
           !(g_app.profile && g_app.profile->scene.active) && !g_app.image_viewer.scene.active &&
           !g_app.reaction_viewer.active;
}
bool front(const ApplicationMediaPicker* p) noexcept {
    return available(p) && unobstructed(p && p->inline_mode);
}
// Open and current on screen, the inline results included (these never take
// the keyboard: application_media_picker_active is the expression picker).
bool shown(const ApplicationMediaPicker* p) noexcept {
    return p && p->active && owns_current(*p) && front(p);
}
void focus(ApplicationMediaPicker& p, bool value) noexcept {
    if (p.focused != value)
        application_composer_text_input_suspend();
    if (value && !p.focused)
        p.edit_identity = ++g_app.media_picker_scope_serial;
    p.focused = p.edit.focused = value;
    p.caret_visible = value;
    p.blink_at = value ? deadline_after(clock_monotonic_milliseconds(), kDefaultCaretBlinkMilliseconds) : 0;
    if (value) {
        g_app.message_composer.focused = false;
        g_app.context_focus = &p.scene.widgets;
        application_focus_window();
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
bool repaint(bool defer_text) noexcept {
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
                                const CatalogBank** bank, u32* local,
                                MediaCatalogItem* temporary) noexcept {
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
    if (p.kind == UiMediaPickerKind::sticker && index >= p.filtered_count)
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
        const bool animated = item.sticker_format == 4;
        if (!protocol_writer_append_c_string(&writer, animated ? "https://media.discordapp.net/stickers/"
                                                               : "https://cdn.discordapp.com/stickers/") ||
            !protocol_writer_append_u64(&writer, item.sticker_id) ||
            !protocol_writer_append_c_string(&writer, animated                  ? ".gif"
                                                      : item.sticker_format == 3 ? ".json"
                                                                                 : ".png"))
            return false;
        item.preview_url = {urls + first, writer.used - first};
        if (item.available &&
            parse_catalog_preview_url(item.preview_url, {target, sizeof(target)}, format(item)).ok())
            bank->preview_mask[i >> 3] |= static_cast<u8>(1u << (i & 7));
    }
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
    const auto key = catalog_key(bank->first_id + at, bank->generation, *item);
    reset_value(out);
    out->id = key.id;
    out->asset_id = preview_admitted(*bank, at) ? key.id : 0;
    out->variant = key.variant;
    out->asset_flags = key.flags;
    out->label =
        categories && item->kind == MediaCatalogItemKind::gif ? utf8_literal("Trending GIFs") : item->name;
    out->detail = item->description.bytes ? item->description : item->name;
    // Stickers name their source (server or pack) in the footer.
    if (p.kind == UiMediaPickerKind::sticker && p.inline_mode) {
        // Inline rows end with the sticker's server or pack name.
        out->detail = sticker_source(p, *item);
        if (index < kUiInlineResultsStickerLimit)
            out->note = {p.inline_notes[index], p.inline_note_bytes[index]};
    } else if (p.kind == UiMediaPickerKind::sticker) {
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
    if (out->kind == UiMediaPickerItemKind::gif && p.favorites_enabled && !p.inline_mode) {
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
            p.inline_select = p.inline_mode;
            return true;
        }
    }
    const auto favorites = application_gif_favorites_view();
    const u32 count = p.favorites_view && p.kind == UiMediaPickerKind::gif
                          ? favorites.records.count
                          : (p.guild ? p.guild->count : 0) + (p.packs ? p.packs->count : 0);
    if (count > p.filtered_capacity) {
        auto* next = static_cast<u32*>(
            commit_pages(count * sizeof(u32)));
        if (!next)
            return false;
        if (p.filtered)
            pages_release(p.filtered);
        p.filtered = next;
        p.filtered_capacity = count;
    }
    p.filtered_count = 0;
    const auto query = message_composer_text(p.edit);
    if (p.kind == UiMediaPickerKind::gif && p.favorites_view) {
        for (u32 i = 0; i < count; ++i)
            if (favorite_contains(favorites.records[i].url, query))
                p.filtered[p.filtered_count++] = i;
        p.source.count = p.filtered_count;
        return true;
    }
    if (p.inline_mode)
        return inline_stickers(p, count, query);
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
                    const auto key = catalog_key(bank->first_id + cover, bank->generation, art);
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
void request(ApplicationMediaPicker& p, MediaCatalogOperation operation, bool delayed) noexcept {
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
    p.due_at = deadline_after(clock_monotonic_milliseconds(),
                              !delayed ? 0 : p.inline_mode ? kInlineSearchDebounceMs : kSearchDebounceMs);
    p.phase = UiMediaPickerPhase::loading;
    p.status = utf8_literal("Loading…");
}
// /sticker results for the query: the session's server stickers and the
// standard packs (the current server's list and the packs fetched once when
// missing, the results scored again as they land).
bool inline_sticker_search(ApplicationMediaPicker& p) noexcept {
    revoke_capture(p);
    cancel(p);
    if (p.generation == ~u32{0})
        return false;
    ++p.generation;
    if ((!p.guild || p.sticker_store_revision != g_sticker_store.revision) && !build_server_stickers(p))
        return false;
    const bool server_unknown = p.guild_id && !sticker_list(p.guild_id) && !p.sticker_guild_done;
    p.wanted = !p.packs || server_unknown;
    p.operation = server_unknown ? MediaCatalogOperation::guild_stickers : MediaCatalogOperation::sticker_packs;
    p.due_at = 1;
    if (!projection(p))
        return false;
    p.phase = p.source.count ? UiMediaPickerPhase::ready
              : p.wanted     ? UiMediaPickerPhase::loading
                             : UiMediaPickerPhase::empty;
    p.scene.scroll_dips = 0;
    ++p.source.revision;
    return true;
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

} // namespace picker_detail
namespace {

// One reaction per activation; Shift keeps the popout open for more, as in
// the official client. A refusal keeps the popout and shows the owner's reason.
bool react(ApplicationMediaPicker& p, const ReactionPickerSelection& selection) noexcept {
    const auto admission =
        reaction_current(p) ? application_queue_message_reaction_emoji(p.reaction_message_id, selection.emoji_id,
                                                                       selection.name, selection.animated)
                            : ApplicationMessageReactionEnqueueResult::stale;
    const bool accepted = admission == ApplicationMessageReactionEnqueueResult::accepted ||
                          admission == ApplicationMessageReactionEnqueueResult::coalesced;
    if (accepted && !application_shift_down()) {
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

} // namespace
namespace picker_detail {

// insert: the inline strip's Tab (a GIF's link replaces the draft).
bool activate(ApplicationMediaPicker& p, const UiMediaPickerTarget& target, bool insert) noexcept {
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
    if (target.action == UiMediaPickerAction::page_back || target.action == UiMediaPickerAction::page_forward) {
        // A smooth scroll, ticked each frame only while it runs.
        const u64 now = clock_monotonic_milliseconds();
        if (ui_media_picker_scene_page(&p.scene, target.action == UiMediaPickerAction::page_forward, now,
                                       static_cast<float>(maximum(g_app.renderer.dpi, 1u)) / 96.f,
                                       g_app.preferences.reduced_motion) &&
            ui_media_picker_scene_paging(p.scene))
            p.page_at = deadline_after(now, kUiTransitionFrameMs);
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
        if (application_alt_down()) {
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
    // The inline strip's Tab: the GIF's link replaces the draft, which counts as a use of /gif.
    if (p.inline_mode && insert && p.kind == UiMediaPickerKind::gif) {
        const auto url = item->send_url;
        application_media_picker_close();
        return application_commands_builtin_used(url);
    }
    if (!p.inline_mode && p.kind == UiMediaPickerKind::gif && application_shift_down()) {
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
    // A result sent from the inline strip or list ends its command: the draft clears.
    const bool inline_results = p.inline_mode;
    application_media_picker_close();
    if (inline_results && !application_commands_builtin_used({}))
        return false;
    return refresh_message_composer_ui() && pump_message_history();
}

} // namespace picker_detail
} // namespace maicord::app_detail
