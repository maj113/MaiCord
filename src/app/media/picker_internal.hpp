#pragma once
// Shared by picker.cpp and its parts (picker_*.cpp); private to them.
#include "picker.hpp"
#include "../shell/state.hpp"
#include "../chat/messages/mutation_notice.hpp"
#include "maicord/app/media/account_media_catalog.hpp"
#include "maicord/ui/chat/messages/reaction_picker.hpp"


#include <limits.h>
#include "maicord/ui/media/picker.hpp"

namespace maicord::app_detail {
namespace picker_detail {}
using namespace picker_detail;
namespace picker_detail {

struct CatalogBank {
    MediaCatalogItem* items;
    char* text;
    u8* preview_mask;
    u64 first_id;
    u32 count, text_bytes, generation;
    MediaCatalogOperation operation;
};
// Keeps the most recent /gif queries' banks; a cached query is asked again only
// after a failure.
constexpr u32 kInlineGifCacheCapacity = 16;

bool preview_admitted(const CatalogBank& bank, u32 index) noexcept;

// Server sticker lists from READY, GUILD_CREATE and GUILD_STICKERS_UPDATE (or
// the current server's REST list), verbatim JSON per server for the session.
constexpr u32 kStickerServerCapacity = 256;

struct StickerServerList {
    u64 guild_id;
    char* json; // Null for a server without stickers.
    u32 bytes;
};
struct StickerStore {
    StickerServerList lists[kStickerServerCapacity];
    u32 count, bytes;
    u64 revision;
};

extern StickerStore g_sticker_store;
const StickerServerList* sticker_list(u64 guild_id) noexcept;
void sticker_list_remove(u64 guild_id) noexcept;
bool sticker_list_put(u64 guild_id, const Utf8View& json) noexcept;
void sticker_store_clear() noexcept;
MediaAttachmentFormat format(const MediaCatalogItem& item) noexcept;
MediaAssetKey catalog_key(u64 id, u32 generation, const MediaCatalogItem& item) noexcept;

} // namespace picker_detail

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
    u32 tile_source_index, favorite_generation;
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
    // A thumb drag's latest pointer row, applied once per presented frame.
    float scrollbar_grab, scrollbar_pointer_y;
    bool scrollbar_dragging, scrollbar_follow_pending;
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
    // A pack the Stickers tab scrolls to once its section exists (the
    // sticker popout's View More Stickers); zero for none.
    u64 jump_section;
    // Inline results: the composer's /gif strip or /sticker list. The query is
    // the command's option (kept in edit, never focused); the panel sits above
    // inline_anchor (the commands popup, else the composer) while the command's
    // option bar shows. The first result of each new set is selected. Entering
    // keeps the expression picker's tab
    // and query (composer_kind, tab_query). enter_pending: Enter came while
    // the results loaded; the result sends as they show, unless the query
    // changes or the results close first.
    bool inline_mode, inline_select, enter_pending;
    UiRect inline_anchor;
    u32 inline_heading_bytes, inline_heading_plain;
    char inline_heading[320];
    // Each shown sticker's "Similar to: <match>" line.
    u16 inline_note_bytes[kUiInlineResultsStickerLimit];
    char inline_notes[kUiInlineResultsStickerLimit][160];
    // The next frame of the strip's page animation; zero when none runs.
    u64 page_at;
    // This account's /gif results by query, most recent first; gif may borrow
    // one, so a bank is freed only once neither holds it.
    CatalogBank* inline_cache[kInlineGifCacheCapacity];
    u16 inline_cache_query_bytes[kInlineGifCacheCapacity];
    char inline_cache_query[kInlineGifCacheCapacity][kMediaCatalogMaximumQueryBytes];
};

namespace picker_detail {

constexpr auto kReactionHint = utf8_literal("Shift-click to add more reactions");

// Rate-limited catalog requests retry at most this often, never sooner than
// the server's Retry-After.
constexpr u32 kCatalogRateRetries = 3;
constexpr u64 kCatalogLocalRateRetryMs = 1000;

void inline_cache_keep(ApplicationMediaPicker& p, Utf8View query, CatalogBank* bank) noexcept;
CatalogBank* inline_cache_take(ApplicationMediaPicker& p, Utf8View query) noexcept;
void inline_cache_clear(ApplicationMediaPicker& p) noexcept;
bool show_bank(ApplicationMediaPicker& p, CatalogBank** slot, CatalogBank* bank) noexcept;
bool emoji_projection(ApplicationMediaPicker& p) noexcept;
u64 emoji_state_key(const ChatStateStore& state) noexcept;
void emoji_unavailable(ApplicationMediaPicker& p) noexcept;
bool open_emoji_model(ApplicationMediaPicker& p) noexcept;
bool landing(const ApplicationMediaPicker& p) noexcept;
CatalogBank* gif_bank(const ApplicationMediaPicker& p) noexcept;
void favorite_item(const GifFavoriteRecord& record, MediaCatalogItem* item) noexcept;
bool favorites_refresh(ApplicationMediaPicker& p) noexcept;
bool enable_favorites(ApplicationMediaPicker& p) noexcept;
bool account_ready() noexcept;
bool available(const ApplicationMediaPicker* p = nullptr) noexcept;
bool owns_current(const ApplicationMediaPicker& p) noexcept;
bool unobstructed(bool inline_results = false) noexcept;
bool front(const ApplicationMediaPicker* p = nullptr) noexcept;
bool shown(const ApplicationMediaPicker* p) noexcept;
void focus(ApplicationMediaPicker& p, bool value) noexcept;
void cancel(ApplicationMediaPicker& p) noexcept;
bool repaint(bool defer_text = false) noexcept;
const MediaCatalogItem* item_at(const ApplicationMediaPicker& p, u32 index,
                                const CatalogBank** bank = nullptr, u32* local = nullptr,
                                MediaCatalogItem* temporary = nullptr) noexcept;
bool sticker_previews(CatalogBank* bank, char* urls, u32 capacity) noexcept;
bool build_server_stickers(ApplicationMediaPicker& p) noexcept;
Utf8View sticker_source(const ApplicationMediaPicker& p, const MediaCatalogItem& item) noexcept;
bool inline_stickers(ApplicationMediaPicker& p, u32 count, Utf8View query) noexcept;
bool resolve(void* context, u32 index, UiMediaPickerItem* out) noexcept;
bool projection(ApplicationMediaPicker&) noexcept;
void request(ApplicationMediaPicker& p, MediaCatalogOperation operation, bool delayed = false) noexcept;
bool inline_sticker_search(ApplicationMediaPicker& p) noexcept;
bool switch_tab(ApplicationMediaPicker& p, UiMediaPickerKind kind) noexcept;
bool activate(ApplicationMediaPicker& p, const UiMediaPickerTarget& target, bool insert = false) noexcept;
void revoke_capture(ApplicationMediaPicker&) noexcept;

} // namespace picker_detail
} // namespace maicord::app_detail
