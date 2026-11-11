#include "search_internal.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "../composer/text_input.hpp"
#include "../messages/pinned_messages.hpp"
#include "../../media/downloads.hpp"
#include "../../media/policy.hpp"
#include "../../media/video.hpp"
#include "content.hpp"
#include "navigation.hpp"
#include "maicord/ui/chat/search/query.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/name_list.hpp"

namespace maicord::app_detail {
namespace search_detail {

bool commit(ApplicationSearch& s, bool tab_switch) noexcept {
    if (thread_scope(s, nullptr)) {
        // Thread names, not messages: the browser lists the matches in place
        // and opens the chosen thread. The field keeps the query and focus.
        menus_close(s);
        reset_value(&s.validation);
        s.committed_edit_revision = s.edit.text_revision;
        application_thread_browser_search(message_composer_text(s.edit));
        return repaint();
    }
    // A widened search leaves the thread browser for the results pane.
    if (application_thread_browser_active())
        application_thread_browser_close();
    application_pinned_messages_close();
    current_scope(s);
    s.panel = true;
    if (!context_current(s))
        return repaint();
    cancel_submitted(s);
    message_search_cancel(&s.controller);
    s.admission_busy = false;
    const bool keep_editor_focus = s.focused;
    const UiSearchQueryContext context{&s, resolve_user, resolve_channel, local_date};
    auto parsed =
        ui_search_query_parse(message_composer_text(s.edit), s.scope, s.scope_id, s.sort, &context);
    parsed.query.tab = s.tab;
    parsed.query.endpoint =
        s.tabs_unavailable ? MessageSearchEndpoint::legacy : MessageSearchEndpoint::tabs;
    parsed.query.tab_counts = !tab_switch;
    menus_close(s);
    if (!parsed.ok()) {
        s.validation = parsed.error == UiSearchQueryError::invalid_date
                           ? utf8_literal("Use a valid YYYY-MM-DD date (2015-2100).")
                       : parsed.error == UiSearchQueryError::invalid_filter
                           ? utf8_literal("Couldn't find a match. Try an ID.")
                           : utf8_literal("Search text is invalid or too long.");
        return repaint();
    }
    if (!message_search_query_valid(parsed.query)) {
        s.validation = utf8_literal("Enter search text or choose a filter.");
        return repaint();
    }
    for (u8 i = 0; i < parsed.query.channel_count; ++i) {
        const auto* channel =
            chat_state_store_find_channel(g_app.account_ui->chat_state, parsed.query.channel_ids[i]);
        if (!channel || !channel->occupied || !channel_is_message_capable(channel->type) ||
            (s.scope == MessageSearchScope::guild ? channel->guild_id != s.scope_id
                                                         : channel->id != s.scope_id)) {
            s.validation = utf8_literal("Choose a channel in the current search scope.");
            return repaint();
        }
    }
    reset_value(&s.validation);
    if (!message_search_begin(&s.controller, s.account_id, parsed.query)) {
        s.validation = utf8_literal("Search could not be queued.");
        return repaint();
    }
    retire_actions(s);
    s.admission_busy = false;
    s.committed_edit_revision = s.edit.text_revision;
    if (!tab_switch)
        s.tab_known = 0;
    recent_load(s);
    if (s.recent && search_recent_push(s.recent, message_composer_text(s.edit)))
        recent_store(s);
    if (tab_switch) {
        if (!repaint())
            return false;
        if (focus_target(s, UiSearchAction::tab, static_cast<u32>(s.tab)))
            g_app.context_focus = &s.scene.widgets;
        application_request_frame();
        return true;
    }
    if (keep_editor_focus)
        return repaint();
    focus(s, false);
    if (!repaint())
        return false;
    g_app.context_focus = &s.scene.widgets;
    ui_widget_manager_focus(&s.scene.widgets, s.scene.buttons[0]);
    application_request_frame();
    return true;
}
void close_panel(ApplicationSearch& s) noexcept {
    cancel_submitted(s);
    message_search_cancel(&s.controller);
    s.panel = false;
    retire_actions(s);
    choices_release(s);
    menus_close(s);
    focus(s, false);
}
void decorate(SearchBank& bank) noexcept {
    bank.measured_native_text = nullptr;
    bank.measured_width = 0;
    clear_bytes(bank.body_heights, sizeof(bank.body_heights));
    for (u32 i = 0; i < bank.page.message_count; ++i) {
        u32 group = 0;
        for (u32 g = 0; g < bank.page.group_count; ++g)
            if (i >= bank.groups[g].first_message)
                group = g;
        const auto& m = bank.messages[i].message;
        const auto* channel =
            chat_state_store_find_channel(g_app.account_ui->chat_state, m.channel_id);
        const auto name = channel
                              ? chat_state_store_channel_name(g_app.account_ui->chat_state, *channel)
                              : Utf8View{};
        auto* label = bank.channels[group];
        u32 used = 0;
        label[used++] = '#';
        if (name.bytes)
            used += utf8_copy_truncated(label + used, sizeof(bank.channels[group]) - 1, name);
        else
            used += decimal_write(label + used, m.channel_id);
        bank.channel_bytes[group] = static_cast<u8>(used);
        auto* detail = bank.details[i];
        used = 0;
        if (m.timestamp_ms) {
            // The official client's calendar label ("Today at 3:45 PM", "10/5/2026 3:45 PM");
            // the short date alone when a long locale format overflows the slot.
            const u64 now_seconds = clock_unix_milliseconds() / 1000u;
            auto formatted = format_timestamp_calendar(m.timestamp_ms / 1000u, now_seconds,
                                                       Slice<char>{detail, sizeof(bank.details[i])});
            if (!formatted.ok)
                formatted = format_timestamp_local(m.timestamp_ms / 1000u, 'd', now_seconds,
                                                   Slice<char>{detail, sizeof(bank.details[i])});
            used = formatted.ok ? formatted.bytes : 0;
        }
        bank.detail_bytes[i] = static_cast<u8>(used);
    }
}
bool activate(ApplicationSearch& s, UiSearchTarget target) noexcept {
    switch (target.action) {
    case UiSearchAction::filter_value:
        return choice_accept(s, target.source_revision, target.source_index);
    case UiSearchAction::edit:
        suggestions_open(s);
        focus(s, true);
        return repaint();
    case UiSearchAction::submit:
        return commit(s);
    case UiSearchAction::scope: {
        // Removing the chip widens this browsing session to the whole server.
        ApplicationThreadBrowserSearchScope threads{};
        if (application_thread_browser_search_scope(&threads))
            s.widened_thread_session = threads.session;
        suggestions_open(s);
        focus(s, true);
        return repaint();
    }
    case UiSearchAction::tab:
        if (target.source_index >= kMessageSearchTabCount)
            return true;
        menus_close(s);
        s.tab = static_cast<MessageSearchTab>(target.source_index);
        focus(s, false);
        return commit(s, true);
    case UiSearchAction::recent: {
        const auto value = s.recent ? search_recent_entry(*s.recent, target.source_index) : Utf8View{};
        if (!value.bytes || application_composer_text_input_composing())
            return true;
        char text[kSearchRecentTextBytes];
        copy_bytes(text, value.data, value.bytes);
        message_composer_restore(&s.edit, 0, {text, value.bytes}, value.bytes, value.bytes);
        s.suggestions = false;
        return commit(s);
    }
    case UiSearchAction::clear_recent:
        if (s.recent && s.recent->count) {
            search_recent_clear(s.recent);
            recent_store(s);
        }
        s.suggestions = true;
        focus(s, true);
        return repaint();
    case UiSearchAction::close:
        close_panel(s);
        return repaint();
    case UiSearchAction::filters:
    case UiSearchAction::more_filters:
        s.filters = !s.filters;
        s.suggestions = s.filters;
        s.sort_menu = false;
        return menu_focus(s, s.filters, UiSearchAction::filter_from);
    case UiSearchAction::sort:
        s.sort_menu = !s.sort_menu;
        s.filters = s.suggestions = false;
        return menu_focus(s, s.sort_menu, UiSearchAction::newest);
    case UiSearchAction::result:
        if (s.filters || s.suggestions || s.sort_menu || !context_current(s))
            return true;
        focus(s, false);
        menus_close(s);
        {
            ApplicationSearchSourceTicket ticket{};
            if (!ui_search_scene_target_current(s.scene, target) ||
                !application_search_source_ticket(target.source_index, &ticket) ||
                ticket.message_id != target.message_id)
                return true;
            return application_search_navigation_begin(ticket) && repaint();
        }
    case UiSearchAction::rich_item: {
        if (s.filters || s.suggestions || s.sort_menu || !ui_search_scene_target_current(s.scene, target))
            return true;
        ApplicationSearchSourceTicket ticket{};
        ApplicationSearchSourceView view{};
        if (!application_search_source_ticket(target.source_index, &ticket) ||
            ticket.message_id != target.message_id ||
            !application_search_source_resolve(ticket, target.snapshot, &view))
            return true;
        const bool attachment_media = target.rich_kind == MessageSceneRichItemKind::attachment;
        const bool embed_media = target.rich_kind == MessageSceneRichItemKind::embed &&
                                 target.resource != MediaEmbedResource::none;
        if (attachment_media || embed_media) {
            ApplicationSearchSourceView outer{};
            auto* media_scene = ui_search_scene_message_scene(&s.scene);
            if (!view.rich || !view.rich_store || !media_scene ||
                !application_search_source_resolve(ticket, false, &outer))
                return true;
            bool spoiler = false, explicit_media = false;
            if (attachment_media) {
                if (target.rich_index >= view.rich->attachment_count)
                    return true;
                const auto& item = view.rich->attachments[target.rich_index];
                spoiler = message_rich_attachment_spoiler(*view.rich_store, item);
                explicit_media = message_rich_attachment_explicit(item);
            } else {
                if (target.rich_index >= view.rich->embed_count)
                    return true;
                explicit_media = message_rich_embed_explicit(view.rich->embeds[target.rich_index]);
            }
            const auto key = message_media_reveal_key(ticket.message_id,
                attachment_media ? MessageMediaSource::attachment : MessageMediaSource::embed,
                target.rich_index, target.snapshot);
            if (application_media_activation_consumed(*media_scene,
                    message_media_owner(*outer.message, outer.rich), ticket.message_id, key, spoiler, explicit_media))
                return true;
        }
        if (target.save) {
            if (target.rich_kind != MessageSceneRichItemKind::attachment || !view.rich ||
                target.rich_index >= view.rich->attachment_count)
                return true;
            return application_downloads_begin_search(ticket, view.rich->attachments[target.rich_index].id,
                                                      target.snapshot);
        }
        if (application_video_open_search(target))
            return true;
        UiRect rectangle{}, clip{};
        if (ui_search_scene_rich_preview_geometry(s.scene, target, &rectangle, &clip) &&
            ui_rect_has_area(clip) &&
            application_image_viewer_open_search(ticket, target.rich_kind, target.rich_index,
                                                 target.snapshot, rectangle, target.resource))
            return true;
        return application_search_content_activate(ticket, target);
    }
    case UiSearchAction::previous:
    case UiSearchAction::next:
    case UiSearchAction::page: {
        if (!context_current(s))
            return repaint();
        const auto offset = s.controller.query.offset;
        const auto next = target.action == UiSearchAction::page       ? target.page_offset
                          : target.action == UiSearchAction::previous ? (offset >= 25 ? offset - 25 : 0)
                                                                      : offset + 25;
        if (message_search_page(&s.controller, next)) {
            retire_actions(s);
            return repaint();
        }
        return true;
    }
    case UiSearchAction::newest:
    case UiSearchAction::oldest:
    case UiSearchAction::relevance:
        s.sort_menu = false;
        s.sort = target.action == UiSearchAction::newest   ? MessageSearchSort::newest
                 : target.action == UiSearchAction::oldest ? MessageSearchSort::oldest
                                                           : MessageSearchSort::relevance;
        return commit(s);
    default:
        break;
    }
    if (ui_search_action_filter(target.action)) {
        if (application_composer_text_input_composing())
            return false;
        const auto token = name_list_at(
            "from:\0in:\0has:\0mentions:\0on:\0before:\0after:\0pinned:\0linkFrom:\0fileType:\0fileName:\0authorType:\0",
            static_cast<u8>(target.action) - static_cast<u8>(UiSearchAction::filter_from));
        const auto text = message_composer_text(s.edit);
        const u32 first = minimum(s.edit.anchor, s.edit.caret), last = maximum(s.edit.anchor, s.edit.caret);
        char inserted[16];
        u32 used = 0;
        if (first && text.data[first - 1] != ' ')
            inserted[used++] = ' ';
        copy_bytes(inserted + used, token.data, token.bytes);
        used += token.bytes;
        const u32 caret = first + used;
        if (last < text.bytes && text.data[last] != ' ')
            inserted[used++] = ' ';
        if (!edit_replace(s, first, last, {inserted, used}))
            return false;
        message_composer_set_selection(&s.edit, caret, caret);
        suggestions_open(s);
        focus(s, true);
        return application_search_edit_changed(true);
    }
    return true;
}

} // namespace search_detail
} // namespace maicord::app_detail
