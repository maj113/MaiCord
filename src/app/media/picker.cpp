#include "picker_internal.hpp"
#include "../shell/overlay_input.hpp"
#include "../chat/composer/text_input.hpp"
#include "../settings/emoji_preferences.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/platform/window/key.hpp"
#include "../chat/composer/commands.hpp"
#include "../chat/composer/composer.hpp"
#include "../account/account_ui_runtime.hpp"
#include "maicord/ui/media/picker.hpp"

namespace maicord::app_detail {
namespace {

ApplicationMediaPicker* ensure_owner() noexcept {
    if (g_app.media_picker)
        return g_app.media_picker;
    auto* p = static_cast<ApplicationMediaPicker*>(
        commit_pages(sizeof(ApplicationMediaPicker)));
    if (!p)
        return nullptr;
    if (!message_composer_initialize(&p->edit, {p->query, sizeof(p->query)})) {
        pages_release(p);
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
void rebind(ApplicationMediaPicker& p) noexcept {
    if (p.account_id == g_app.account_ui->chat_state.self_user_id &&
        p.channel_id == g_app.account_ui->active_channel_id)
        return;
    revoke_capture(p);
    ui_media_picker_scene_close(&p.scene);
    reaction_picker_release(&p.emoji);
    cancel(p);
    focus(p, false);
    // The session's category and /gif caches belong to its account.
    if (p.account_id != g_app.account_ui->chat_state.self_user_id) {
        if (p.categories)
            pages_release(p.categories);
        p.categories = nullptr;
        inline_cache_clear(p);
    }
    p.scope = ++g_app.media_picker_scope_serial;
    p.favorites_enabled = p.favorites_view = p.gif_favorites_view = false;
    p.account_id = g_app.account_ui->chat_state.self_user_id;
    p.channel_id = g_app.account_ui->active_channel_id;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, p.channel_id);
    p.guild_id = channel ? channel->guild_id : 0;
    if (p.guild)
        pages_release(p.guild);
    p.guild = nullptr;
    p.sticker_guild_done = false;
    p.section_count = p.filtered_count = p.source.count = 0;
    ZeroMemory(p.tab_query_bytes, sizeof(p.tab_query_bytes));
    ZeroMemory(p.tab_scroll, sizeof(p.tab_scroll));
    message_composer_clear(&p.edit);
}
// Enter's result among the inline results: the keyboard-selected one, else the
// one the pointer rests on, else the first sendable one.
UiMediaPickerTarget inline_enter_target(const ApplicationMediaPicker& p) noexcept {
    if (!ui_media_picker_scene_current(p.scene))
        return {};
    const auto selected = ui_media_picker_scene_selected_target(p.scene);
    if (selected.action == UiMediaPickerAction::item)
        return selected;
    u32 index = ~u32{0};
    for (u32 i = 0; i < p.scene.cell_count && index == ~u32{0}; ++i) {
        const auto& cell = p.scene.projection->cells[i];
        if (cell.item.enabled && ui_widget_handle_equal(p.scene.widgets.hovered, cell.widget))
            index = cell.source_index;
    }
    const CatalogBank* bank = nullptr;
    u32 local = 0;
    MediaCatalogItem temporary{};
    for (u32 i = 0; index == ~u32{0} && i < p.source.count; ++i) {
        const auto* item = item_at(p, i, &bank, &local, &temporary);
        if (item && item->available)
            index = i;
    }
    if (!item_at(p, index, &bank, &local, &temporary) || !bank)
        return {};
    return {p.scope,  p.generation,        bank->first_id + local, p.source.revision,
            index,    p.scene.generation,  UiMediaPickerAction::item, p.kind};
}
// The results an Enter waited for have settled (or are still loading).
bool inline_enter_resume(ApplicationMediaPicker& p) noexcept {
    if (!p.enter_pending || !p.inline_mode || p.phase == UiMediaPickerPhase::loading)
        return true;
    p.enter_pending = false;
    const auto target = p.phase == UiMediaPickerPhase::ready ? inline_enter_target(p) : UiMediaPickerTarget{};
    return target.action == UiMediaPickerAction::item ? activate(p, target)
                                                       : application_commands_results_refused();
}

} // namespace

bool application_media_picker_open(UiMediaPickerKind kind) noexcept {
    // The composer launchers replace an open reaction or field popout.
    if (g_app.media_picker && (g_app.media_picker->reaction_message_id || g_app.media_picker->field_mode))
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
    rebind(p);
    if (kind == UiMediaPickerKind::gif && !enable_favorites(p))
        return false;
    p.active = true;
    message_action_bar_close(&g_app.message_action_bar);
    if (switch_tab(p, kind))
        return true;
    application_media_picker_close();
    return false;
}
bool application_media_picker_open_sticker_pack(u64 pack_id) noexcept {
    if (!pack_id)
        return false;
    // Already on the Stickers tab: stay open (a reopen would toggle it shut).
    auto* p = g_app.media_picker;
    const bool shown = p && p->active && p->scene.active && owns_current(*p) && p->kind == UiMediaPickerKind::sticker &&
                       !p->reaction_message_id && !p->field_mode;
    if (!shown && !application_media_picker_open(UiMediaPickerKind::sticker))
        return false;
    if (!g_app.media_picker)
        return false;
    g_app.media_picker->jump_section = pack_id;
    return repaint();
}
bool application_media_picker_inline(UiMediaPickerKind kind, Utf8View query, UiRect anchor, bool autocomplete) noexcept {
    auto* open = g_app.media_picker;
    // Inline results hide while the expression picker is open.
    if (open && open->active && !open->inline_mode)
        return true;
    if (!query.bytes || kind == UiMediaPickerKind::emoji || !ui_rect_valid(anchor) || !available()) {
        application_media_picker_inline_close();
        return true;
    }
    auto* owner = ensure_owner();
    if (!owner)
        return false;
    auto& p = *owner;
    rebind(p);
    if (p.generation == ~u32{0})
        return false;
    const bool fresh = !p.active || !p.inline_mode || p.kind != kind || p.inline_autocomplete != autocomplete;
    if (fresh) {
        if (p.active)
            application_media_picker_close();
        // Keep the composer picker's tab and query for its next open.
        const auto saved = message_composer_text(p.edit);
        p.composer_kind = p.kind;
        p.tab_query_bytes[static_cast<u32>(p.kind)] = static_cast<u16>(saved.bytes);
        CopyMemory(p.tab_query[static_cast<u32>(p.kind)], saved.data, saved.bytes);
        message_composer_clear(&p.edit);
        p.kind = kind;
        p.favorites_view = p.wanted = false;
        p.due_at = 0;
        p.inline_mode = p.active = true;
    }
    p.inline_anchor = anchor;
    p.inline_autocomplete = autocomplete;
    // The search field's capacity bounds the query, as the catalog does.
    char bounded[kMediaCatalogMaximumQueryBytes];
    query = {bounded, utf8_copy_truncated(bounded, sizeof(bounded), query)};
    if (fresh || !utf8_equal(message_composer_text(p.edit), query)) {
        p.enter_pending = false;
        if (!message_composer_restore(&p.edit, 0, query, query.bytes, query.bytes))
            return false;
        // "Gif content matching **q**" / "Stickers matching **q**".
        const Utf8View lead = kind == UiMediaPickerKind::gif ? utf8_literal("Gif content matching ")
                                                             : utf8_literal("Stickers matching ");
        p.inline_heading_plain = utf8_copy_truncated(p.inline_heading, sizeof(p.inline_heading), lead);
        p.inline_heading_bytes =
            p.inline_heading_plain + utf8_copy_truncated(p.inline_heading + p.inline_heading_plain,
                                                         sizeof(p.inline_heading) - p.inline_heading_plain, query);
        if (kind == UiMediaPickerKind::gif) {
            request(p, MediaCatalogOperation::gif_search, true);
            // A query answered this session shows at once without asking again.
            if (auto* cached = inline_cache_take(p, query)) {
                p.wanted = false;
                p.due_at = 0;
                if (!show_bank(p, &p.gif, cached))
                    return false;
            }
        } else if (!inline_sticker_search(p)) {
            return false;
        }
    }
    return application_request_frame();
}
void application_media_picker_inline_close() noexcept {
    if (g_app.media_picker && g_app.media_picker->inline_mode)
        application_media_picker_close();
}
bool application_media_picker_inline_key(u32 key, bool* handled) noexcept {
    *handled = false;
    auto* p = g_app.media_picker;
    if (!p || !p->inline_mode)
        return true;
    // Enter before the results arrive waits for them (inline_enter_resume).
    if (key == key::enter && p->phase == UiMediaPickerPhase::loading && shown(p)) {
        *handled = p->enter_pending = true;
        return true;
    }
    // Results that arrived since the last frame show their scene first.
    if (key == key::enter && p->phase == UiMediaPickerPhase::ready && p->source.count && shown(p) &&
        !ui_media_picker_scene_current(p->scene) && !application_media_picker_synchronize())
        return false;
    if (!shown(p) || !ui_media_picker_scene_current(p->scene) || p->phase != UiMediaPickerPhase::ready)
        return true;
    if (key == key::up || key == key::down) {
        *handled = true;
        ui_media_picker_scene_step_selection(&p->scene, key == key::up ? -1 : 1);
        return repaint();
    }
    if (key != key::enter && key != key::tab)
        return true;
    // Enter sends the selected result, else the hovered one, else the first;
    // Tab inserts a GIF's link and keeps its composer meaning without a selection.
    const auto target = key == key::enter ? inline_enter_target(*p) : ui_media_picker_scene_selected_target(p->scene);
    if (target.action != UiMediaPickerAction::item)
        return true;
    *handled = true;
    return activate(*p, target, key == key::tab);
}
Utf8View application_media_picker_inline_message() noexcept {
    const auto* p = g_app.media_picker;
    return p && p->inline_mode && p->phase == UiMediaPickerPhase::failed
               ? utf8_literal("Couldn't load results. Edit your search to try again.")
               : utf8_literal("No results found. Try a different search.");
}
u64 application_media_picker_sticker_server(u64 sticker_id) noexcept {
    if (!sticker_id)
        return 0;
    // Server lists are the API's JSON: an ID appears as "<id>".
    char needle[24];
    u32 bytes = 0;
    needle[bytes++] = '"';
    char digits[20];
    u32 count = 0;
    for (u64 value = sticker_id; value; value /= 10)
        digits[count++] = static_cast<char>('0' + value % 10);
    while (count)
        needle[bytes++] = digits[--count];
    needle[bytes++] = '"';
    for (u32 i = 0; i < g_sticker_store.count; ++i) {
        const auto& list = g_sticker_store.lists[i];
        for (u32 at = 0; list.json && at + bytes <= list.bytes; ++at)
            if (list.json[at] == '"' && bytes_equal(list.json + at, needle, bytes))
                return list.guild_id;
    }
    return 0;
}
bool application_media_picker_open_reaction(u64 message_id, UiRect anchor) noexcept {
    auto* ui = g_app.account_ui;
    if (!message_id || !ui_rect_valid(anchor) || !account_ready() || !unobstructed())
        return false;
    const auto* message = application_message_record(message_id);
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
    rebind(p);
    if (p.generation == ~u32{0})
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
bool application_media_picker_open_field(UiRect anchor, u64 owner_id) noexcept {
    if (!owner_id || !ui_rect_valid(anchor) || !account_ready() || !unobstructed(false, true))
        return false;
    auto* owner = ensure_owner();
    if (!owner)
        return false;
    auto& p = *owner;
    // The button on the open field popout toggles it shut.
    if (p.active && p.scene.active && p.field_mode) {
        application_media_picker_close();
        return true;
    }
    if (p.active)
        application_media_picker_close();
    rebind(p);
    if (p.generation == ~u32{0})
        return false;
    // Keep the composer picker's query for its next open.
    const auto saved = message_composer_text(p.edit);
    p.composer_kind = p.kind;
    p.tab_query_bytes[static_cast<u32>(p.kind)] = static_cast<u16>(saved.bytes);
    CopyMemory(p.tab_query[static_cast<u32>(p.kind)], saved.data, saved.bytes);
    message_composer_clear(&p.edit);
    p.field_mode = true;
    p.field_owner = owner_id;
    p.reaction_anchor = anchor;
    p.kind = UiMediaPickerKind::emoji;
    p.favorites_view = p.wanted = false;
    p.due_at = 0;
    ++p.generation;
    p.active = true;
    application_gif_favorites_set_visible(false);
    application_emoji_preferences_open();
    if (open_emoji_model(p)) {
        focus(p, true);
        if (repaint())
            return true;
    }
    application_media_picker_close();
    return false;
}
void application_media_picker_close_field() noexcept {
    if (g_app.media_picker && g_app.media_picker->field_mode)
        application_media_picker_close();
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
    if (!p->reaction_message_id && !p->field_mode && !p->inline_mode && p->scene.active)
        p->tab_scroll[static_cast<u32>(p->kind)] = p->scene.scroll_dips;
    cancel(*p);
    p->active = p->wanted = false;
    p->jump_section = 0;
    focus(*p, false);
    if (p->reaction_message_id || p->field_mode || p->inline_mode) {
        // Return the shared search field and kind to the composer picker.
        const u32 kind = static_cast<u32>(p->composer_kind);
        message_composer_restore(&p->edit, 0, {p->tab_query[kind], p->tab_query_bytes[kind]},
                                 p->tab_query_bytes[kind], p->tab_query_bytes[kind]);
        p->kind = p->composer_kind;
        p->reaction_message_id = p->field_owner = 0;
        reset_value(&p->reaction_scope);
        p->reaction_error = p->field_mode = p->inline_mode = p->inline_select = p->enter_pending =
            p->inline_autocomplete = false;
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
    // Emptying the /gif cache frees every cached bank but the current results'.
    inline_cache_clear(*p);
    CatalogBank* banks[]{p->gif, p->categories, p->guild, p->packs};
    for (auto* bank : banks)
        if (bank)
            pages_release(bank);
    if (p->filtered)
        pages_release(p->filtered);
    if (p->saved_preview_mask)
        pages_release(p->saved_preview_mask);
    pages_release(p);
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
    return shown(g_app.media_picker) && !g_app.media_picker->inline_mode;
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
    // Inline results go with the command (its option bar), or an autocomplete's with its ':' query; another popup
    // obstructs them (front).
    if (!front(p) || p->account_id != g_app.account_ui->chat_state.self_user_id ||
        p->channel_id != g_app.account_ui->active_channel_id ||
        (p->inline_mode && !(p->inline_autocomplete ? application_composer_emoji_query_active()
                                                    : application_commands_bar_visible()))) {
        application_media_picker_close();
        return true;
    }
    p->source.account_scope = p->scope;
    p->source.query_generation = p->generation;
    const auto& state = g_app.account_ui->chat_state;
    const u64 emoji_revision = emoji_state_key(state);
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
    // Inline results show only while loading or with results.
    if (p->inline_mode && (p->phase == UiMediaPickerPhase::empty || p->phase == UiMediaPickerPhase::failed ||
                           (p->phase == UiMediaPickerPhase::ready && !p->source.count))) {
        revoke_capture(*p);
        ui_media_picker_scene_close(&p->scene);
        g_app.renderer.media_picker_scene = nullptr;
        return true;
    }
    // The field popout takes the reaction popout's shape (Emoji alone, beside its anchor).
    const bool popout = reaction || p->field_mode;
    content.available_tabs = popout ? ui_media_picker_tab_bit(UiMediaPickerKind::emoji)
                             : p->inline_mode ? ui_media_picker_tab_bit(p->kind)
                                              : 7;
    content.variant = popout           ? UiMediaPickerVariant::reaction
                      : p->inline_mode ? UiMediaPickerVariant::inline_results
                                       : UiMediaPickerVariant::expression;
    if (p->inline_mode) {
        reset_value(&content.edit);
        content.heading = {p->inline_heading, p->inline_heading_bytes};
        content.heading_plain = p->inline_heading_plain;
    }
    content.placeholder =
        p->inline_mode ? Utf8View{}
        : p->kind == UiMediaPickerKind::gif
            ? (p->favorites_view ? utf8_literal("Search favorites") : utf8_literal("Search Klipy"))
        : p->kind == UiMediaPickerKind::emoji
            ? (reaction ? utf8_literal("Find the perfect reaction") : utf8_literal("Find the perfect emoji"))
                                              : utf8_literal("Search stickers");
    content.status = p->status;
    content.back = p->kind == UiMediaPickerKind::gif && !p->inline_mode &&
                   (p->favorites_view || p->operation != MediaCatalogOperation::gif_categories);
    auto& prior = p->content;
    const auto revision = prior.revision;
    prior.revision = 0;
    const bool content_same = bytes_equal(&prior, &content, sizeof(content));
    prior.revision = revision;
    content.revision = content_same ? revision : revision + 1;
    p->content = content;
    auto viewport = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::content);
    viewport.right = static_cast<float>(g_app.renderer.width) * 96.f / maximum(g_app.renderer.dpi, 1u);
    const auto* composer = ui_widget_manager_layout_node(g_app.message_composer_scene.widgets,
                                                         g_app.message_composer_scene.surface);
    if (const auto* title =
            ui_scene_node(g_app.shell_scene, UiSemantic::title_bar))
        viewport.top = maximum(viewport.top, title->rectangle.bottom);
    // The composer picker sits above the composer; the reaction popout may
    // use the whole client area below the title bar beside its message;
    // inline results sit above their anchor (the commands popup).
    const UiRect anchor = popout           ? p->reaction_anchor
                          : p->inline_mode ? p->inline_anchor
                          : composer       ? composer->rectangle
                                           : UiRect{};
    if (popout) {
        viewport.left = 0;
        viewport.bottom = static_cast<float>(g_app.renderer.height) * 96.f / maximum(g_app.renderer.dpi, 1u);
    } else if (p->inline_mode)
        viewport.bottom = minimum(viewport.bottom, anchor.top);
    else if (composer)
        viewport.bottom = minimum(viewport.bottom, composer->rectangle.top);
    if (p->resizing && !ui_rect_equal(viewport, p->scene.viewport))
        revoke_capture(*p);
    const u32 prior_generation = p->scene.generation;
    if ((!popout && !p->inline_mode && !composer) ||
        !ui_media_picker_scene_update(&p->scene, &p->source, &p->content, viewport, anchor,
                                      p->preferred_width))
        return false;
    // A new result set starts with its first result selected.
    if (p->inline_mode && p->inline_select && p->phase == UiMediaPickerPhase::ready) {
        p->inline_select = false;
        ui_media_picker_scene_select(&p->scene, 0);
    }
    // A saved-state response rebuilds cell handles. Restore pointer state
    // once for that update so a stationary cursor keeps its action hint.
    if (prior_generation != p->scene.generation && !g_app.keyboard_focus_visible) {
        UiPoint pointer{};
        if (application_pointer_dip(&pointer))
            ui_media_picker_scene_hover(&p->scene, pointer.x, pointer.y, clock_monotonic_milliseconds());
    }
    // View More Stickers: the pack's section, once the packs have arrived.
    if (p->jump_section && p->kind == UiMediaPickerKind::sticker && p->phase == UiMediaPickerPhase::ready) {
        for (u32 i = 0; i < p->section_count; ++i)
            if (p->sections[i].id == p->jump_section) {
                ui_media_picker_scene_scroll_section(&p->scene, p->jump_section);
                break;
            }
        p->jump_section = 0;
    }
    g_app.renderer.media_picker_scene = &p->scene;
    return true;
}
bool application_media_picker_pending(u64 now) noexcept {
    const auto* p = g_app.media_picker;
    return p && p->active && owns_current(*p) && p->wanted && !p->submitted_id && now >= p->due_at;
}
bool application_media_picker_submit(u64 id, u64 now) noexcept {
    if (!id || !application_media_picker_pending(now))
        return false;
    auto& p = *g_app.media_picker;
    const auto text = message_composer_text(p.edit);
    CopyMemory(p.submitted_query, text.data, text.bytes);
    const bool search = p.operation == MediaCatalogOperation::gif_search ||
                        p.operation == MediaCatalogOperation::gif_suggestions;
    // The inline /gif strip asks for animated WebP, 20 results.
    const bool strip = p.inline_mode && p.operation == MediaCatalogOperation::gif_search;
    MediaCatalogQuery query{p.operation,
                            search ? Utf8View{p.submitted_query, text.bytes} : Utf8View{},
                            utf8_literal("en-US"),
                            p.operation == MediaCatalogOperation::guild_stickers ? p.guild_id : 0,
                            strip ? kMediaCatalogMinimumGifPageItems : 0u,
                            strip ? MediaCatalogFormat::webp : MediaCatalogFormat::tiny_webp};
    const auto submitted = account_module_media_catalog(
        g_app.account.instance, AccountMediaCatalogOperation::submit, &query, id);
    if (!submitted.ok()) {
        p.admission_busy = submitted.error == RestClientSubmitError::busy;
        if (!p.admission_busy) {
            p.wanted = false;
            p.phase = UiMediaPickerPhase::failed;
            p.status = utf8_literal("Couldn't load. Press Enter to retry.");
            if (repaint())
                inline_enter_resume(p);
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
        return repaint() && inline_enter_resume(*p);
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
        return repaint() && inline_enter_resume(*p);
    }
    CatalogBank* candidate = nullptr;
    if (response.transport_ok() && response.status_code == 200) {
        auto* scratch =
            static_cast<u32*>(commit_pages(kMediaCatalogStructuralCapacity * sizeof(u32)));
        if (scratch) {
            const auto measured =
                media_catalog_measure(operation, body, {scratch, kMediaCatalogStructuralCapacity}, 32);
            // A page longer than the cap keeps its first records.
            if (measured.ok()) {
                const bool stickers = operation == MediaCatalogOperation::guild_stickers ||
                                      operation == MediaCatalogOperation::sticker_packs;
                const u32 preview_bytes = stickers ? measured.item_count * 64 : 0;
                const u32 mask_bytes = (measured.item_count + 7) / 8;
                const u32 bytes = sizeof(CatalogBank) + measured.item_count * sizeof(MediaCatalogItem) +
                                  measured.text_bytes + preview_bytes + mask_bytes;
                candidate = static_cast<CatalogBank*>(
                    commit_pages(bytes));
                if (candidate) {
                    candidate->items = reinterpret_cast<MediaCatalogItem*>(candidate + 1);
                    candidate->text = reinterpret_cast<char*>(candidate->items + measured.item_count);
                    candidate->preview_mask =
                        reinterpret_cast<u8*>(candidate->text + measured.text_bytes + preview_bytes);
                    const auto decoded = media_catalog_decode(
                        operation, body, {scratch, kMediaCatalogStructuralCapacity}, 32,
                        {candidate->items, measured.item_count}, {candidate->text, measured.text_bytes});
                    if (!decoded.ok()) {
                        pages_release(candidate);
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
                            pages_release(candidate);
                            candidate = nullptr;
                        } else {
                            candidate->text_bytes = decoded.text_bytes + preview_bytes;
                            candidate->generation = p->generation;
                            candidate->operation = operation;
                            candidate->first_id = g_app.media_picker_item_serial + 1;
                            g_app.media_picker_item_serial += decoded.item_count;
                        }
                    }
                }
            }
            pages_release(scratch);
        }
    }
    if (candidate) {
        auto** slot = operation == MediaCatalogOperation::sticker_packs    ? &p->packs
                      : operation == MediaCatalogOperation::gif_categories ? &p->categories
                                                                           : &p->gif;
        if (!show_bank(*p, slot, candidate))
            return false;
        // The query is unchanged since the request, or its generation would differ.
        if (p->inline_mode && operation == MediaCatalogOperation::gif_search)
            inline_cache_keep(*p, message_composer_text(p->edit), candidate);
    } else {
        p->phase = UiMediaPickerPhase::failed;
        p->status = utf8_literal("Couldn't load. Press Enter to retry.");
    }
    return repaint() && inline_enter_resume(*p);
}

} // namespace maicord::app_detail
