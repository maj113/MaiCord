#include "game_search.hpp"
#include "composer.hpp"
#include "../game_lookup.hpp"
#include "../../account/account_presentation.hpp"
#include "../../shell/state.hpp"
#include "../../settings/user_settings.hpp"

#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/ui/chat/game_media.hpp"
#include "maicord/ui/shell/popup_scene.hpp"

namespace maicord::app_detail {
namespace {
constexpr u32 kRows = kGameSearchResultMaximum;
constexpr Utf8View kTitle = utf8_literal("Games matching @");

struct Search {
    // The listed games and their names (the popup borrows the labels).
    u64 shown[kRows];
    u32 shown_count;
    u8 label_bytes[kRows];
    char labels[kRows][kGuildAdminGameNameBytes];
    char title[kTitle.bytes + kGameSearchQueryBytes];
    // The draft as last seen: a typed space after "@game" starts a token.
    u64 document;
    u32 bytes, caret;
};
Search g_search;

bool composer_popup() noexcept {
    return g_app.popup_owner_kind == PopupOwnerKind::autocomplete ||
           g_app.popup_owner_kind == PopupOwnerKind::commands;
}
void close_popup() noexcept {
    if (composer_popup()) {
        popup_scene_close(&g_app.popup_scene);
        set_popup_owner(PopupOwnerKind::none);
    }
    g_search.shown_count = 0;
}

bool has_marks(Utf8View text) noexcept {
    for (u32 i = 0; i + 2 < text.bytes; ++i)
        if (static_cast<u8>(text.data[i]) == 0xE2 && static_cast<u8>(text.data[i + 1]) == 0x81 &&
            (static_cast<u8>(text.data[i + 2]) == 0xA4 || static_cast<u8>(text.data[i + 2]) == 0xA2))
            return true;
    return false;
}

// Removes what is left of a token whose head or end was deleted.
bool repair(MessageComposer& composer) noexcept {
    const Utf8View text = message_composer_text(composer);
    if (!has_marks(text))
        return true;
    ScratchPages<char[kMessageComposerMaximumBytes]> scratch;
    if (!scratch.data)
        return false;
    u32 bytes = 0, caret = composer.caret;
    if (!game_tokens_rewrite(text, false, {*scratch.data, kMessageComposerMaximumBytes}, &bytes, &caret))
        return true;
    return message_composer_replace_range(&composer, 0, composer.bytes, {*scratch.data, bytes}) !=
               MessageComposerResult::invalid_input &&
           message_composer_set_selection(&composer, caret, caret) != MessageComposerResult::invalid_input;
}

// The official client turns "@game" into the token when a space is typed after it.
bool typed_game_space(MessageComposer& composer) noexcept {
    const Utf8View text = message_composer_text(composer);
    const u32 caret = composer.caret;
    const bool typed = composer.document_identity == g_search.document && composer.bytes == g_search.bytes + 1 &&
                       caret == g_search.caret + 1 && composer.anchor == caret;
    if (!typed || caret < 6 || !bytes_equal(text.data + caret - 6, "@game ", 6) ||
        (caret > 6 && !ascii_is_space(text.data[caret - 7])))
        return true;
    char token[kGameTokenHeadBytes + kGameTokenEndBytes];
    copy_bytes(token, kGameTokenHead, kGameTokenHeadBytes);
    copy_bytes(token + kGameTokenHeadBytes, kGameTokenEnd, kGameTokenEndBytes);
    const u32 inside = caret - 6 + kGameTokenHeadBytes;
    return message_composer_replace_range(&composer, caret - 6, caret, {token, sizeof(token)}) !=
               MessageComposerResult::invalid_input &&
           message_composer_set_selection(&composer, inside, inside) != MessageComposerResult::invalid_input;
}

// "Games matching @query" and a row per game: its icon (the placeholder glyph
// without one) and name.
bool show(const GameSearchResults& results, Utf8View query) noexcept {
    auto& s = g_search;
    const GameStore* games = application_games();
    PopupItem items[kRows + 1];
    u32 count = 0;
    s.shown_count = 0;
    for (u32 i = 0; games && i < results.count && s.shown_count < kRows; ++i) {
        const GameRecord* record = game_store_find(*games, results.ids[i]);
        if (!record || record->state != GameState::known || !record->game.name_bytes)
            continue;
        const u32 row = s.shown_count++;
        s.shown[row] = record->game.id;
        s.label_bytes[row] = record->game.name_bytes;
        copy_bytes(s.labels[row], record->game.name, record->game.name_bytes);
    }
    if (!s.shown_count) {
        close_popup();
        return true;
    }
    copy_bytes(s.title, kTitle.data, kTitle.bytes);
    copy_bytes(s.title + kTitle.bytes, query.data, query.bytes);
    popup_item_set(&items[count++], {s.title, kTitle.bytes + query.bytes}, popup_action_game_first + kRows,
                   popup_item_command_section);
    for (u32 row = 0; row < s.shown_count; ++row) {
        PopupItem& item = items[count++];
        popup_item_set(&item, {s.labels[row], s.label_bytes[row]}, popup_action_game_first + row,
                       popup_item_enabled);
        GameImageMedia icon;
        if (game_chip_icon_media(games, s.shown[row], &icon)) {
            item.flags |= popup_item_avatar;
            item.media_id = icon.key.id;
            item.media_variant = static_cast<u32>(media_asset_embed_format(icon.key));
            item.media_flags = kPopupItemMediaUrl;
        } else {
            item.presentation = ui_menu_row_presentation(UiIcon::image);
        }
    }
    const auto* surface =
        ui_widget_manager_layout_node(g_app.message_composer_scene.widgets, g_app.message_composer_scene.surface);
    if (!surface)
        return false;
    const UiRect anchor = surface->rectangle;
    const UiRect viewport = application_viewport_dips();
    const u64 owner_id = g_app.message_composer.channel_id ? g_app.message_composer.channel_id : 1;
    auto& popup = g_app.popup_scene;
    if (popup.active && g_app.popup_owner_kind == PopupOwnerKind::autocomplete && popup.kind == PopupKind::autocomplete &&
        popup.owner_id == owner_id && popup.item_count == count && popup.items[0].action == items[0].action &&
        ui_rect_equal(popup.anchor, anchor) && ui_rect_equal(popup.viewport, viewport)) {
        // Same rows: new views without losing the selection or the scroll.
        for (u32 i = 0; i < count; ++i)
            popup.items[i] = items[i];
        ++popup.revision;
        return true;
    }
    if (!popup_scene_open(&popup, PopupKind::autocomplete, owner_id, {items, count}, anchor, viewport,
                          ui_rect_width(anchor), true)
             .ok())
        return false;
    set_popup_owner(PopupOwnerKind::autocomplete);
    // The first game is selected, as the official list selects it.
    popup.selected_index = 1;
    return true;
}

} // namespace

bool application_game_search_sync(bool* handled) noexcept {
    *handled = false;
    MessageComposer& composer = g_app.message_composer;
    // "@game " becomes the token only while games show in autocomplete results (the setting's autoInsertOnSpace).
    if (!repair(composer) || (application_user_settings_game_mentions_autocomplete() && !typed_game_space(composer)))
        return false;
    g_search.document = composer.document_identity;
    g_search.bytes = composer.bytes;
    g_search.caret = composer.caret;
    const Utf8View text = message_composer_text(composer);
    GameToken token{};
    if (composer.anchor != composer.caret || !game_token_around(text, composer.caret, &token)) {
        if (g_search.shown_count)
            close_popup();
        return true;
    }
    *handled = true;
    // The official list: nothing for an empty query, else the query's games
    // (the longest searched prefix's while they load).
    const Utf8View query{text.data + token.query_first, token.query_last - token.query_first};
    game_lookup_search(query);
    return show(game_lookup_results(query), query);
}

bool application_game_search_started() noexcept {
    MessageComposer& composer = g_app.message_composer;
    if (composer.caret < kGameTokenEndBytes)
        return true;
    const u32 inside = composer.caret - kGameTokenEndBytes;
    return composer_apply_result(message_composer_set_selection(&composer, inside, inside), true);
}

bool application_game_search_pick(u32 row) noexcept {
    MessageComposer& composer = g_app.message_composer;
    GameToken token{};
    if (row >= g_search.shown_count || !game_token_around(message_composer_text(composer), composer.caret, &token))
        return false;
    char mention[32];
    const u32 bytes = game_mention_write(g_search.shown[row], {mention, sizeof(mention) - 1});
    if (!bytes)
        return false;
    mention[bytes] = ' ';
    close_popup();
    return composer_apply_result(
        message_composer_replace_range(&composer, token.first, token.last, {mention, bytes + 1}), true);
}

u32 application_game_search_icon_url(u64 media_id, Slice<char> url) noexcept {
    for (u32 row = 0; media_id && row < g_search.shown_count; ++row) {
        GameImageMedia icon;
        if (game_chip_icon_media(application_games(), g_search.shown[row], &icon) && icon.key.id == media_id &&
            icon.url_bytes <= url.count) {
            copy_bytes(url.data, icon.url, icon.url_bytes);
            return icon.url_bytes;
        }
    }
    return 0;
}

} // namespace maicord::app_detail
