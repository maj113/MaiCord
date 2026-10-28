#include "application_commands.hpp"
#include "application_profile.hpp"
#include "application_composer.hpp"

#include "application_account_presentation.hpp"
#include "application_composer_text_input.hpp"
#include "application_state.hpp"
#include "application_popup_plugin_owner.hpp"

#include "maicord/app/mention_member_query.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/typing_store.hpp"
#include "maicord/ui/composer_autocomplete.hpp"
#include "maicord/ui/message_action_bar.hpp"
#include "maicord/ui/message_context_controller.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "account_ui_runtime.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {

// The gateway member search behind '@' in guild channels (debounced, cached),
// shared with the search bar's people suggestions; only one has focus.
MentionMemberQueryOwner g_mention_member_query{};
bool g_mention_member_query_composer = false;
u64 g_mention_member_interest_key = 0;
u64 g_mention_member_interest_channel = 0;

void cancel_member_query() noexcept {
    if (g_mention_member_interest_key && g_app.account.instance &&
        g_app.account.api.guild_members_query_cancel)
        g_app.account.api.guild_members_query_cancel(g_app.account.instance);
    g_mention_member_interest_key = g_mention_member_interest_channel = 0;
    mention_member_query_cancel(&g_mention_member_query);
}

void note_member_query(u64 guild_id, u64 channel_id, Utf8View query) noexcept {
    char folded[kGatewayGuildMemberQueryMaximumBytes]{};
    u32 bytes = 0;
    if (!guild_id || !channel_id || !mention_member_query_fold(query, folded, &bytes)) {
        cancel_member_query();
        return;
    }
    const u64 key = mention_member_query_key(guild_id, {folded, bytes});
    if (g_mention_member_interest_key != key || g_mention_member_interest_channel != channel_id) {
        cancel_member_query();
        g_mention_member_interest_key = key;
        g_mention_member_interest_channel = channel_id;
    }
    mention_member_query_note(&g_mention_member_query, guild_id, channel_id, query,
                              clock_monotonic_milliseconds());
}

// The composer drops only a search it asked for itself.
void cancel_composer_member_query() noexcept {
    if (g_mention_member_query_composer)
        cancel_member_query();
    g_mention_member_query_composer = false;
}

// Asks for members matching the current '@' query, or drops a pending ask
// once the composer no longer has one.
void note_mention_member_query() noexcept {
    const ComposerAutocomplete& autocomplete = g_app.composer_autocomplete;
    const MessageComposer& composer = g_app.message_composer;
    if (!autocomplete.member_query_guild_id || autocomplete.token_end <= autocomplete.token_begin ||
        autocomplete.token_end > composer.bytes || !composer.text) {
        cancel_composer_member_query();
        return;
    }
    const Utf8View query{composer.text + autocomplete.token_begin + 1,
                         autocomplete.token_end - autocomplete.token_begin - 1};
    // The scheduler wakes for the debounce deadline (application_mention_member_query_next_wake).
    g_mention_member_query_composer = true;
    note_member_query(autocomplete.member_query_guild_id, composer.channel_id, query);
}

bool composer_owns_popup() noexcept {
    return g_app.popup_owner_kind == PopupOwnerKind::autocomplete ||
           g_app.popup_owner_kind == PopupOwnerKind::commands;
}
void close_composer_popup() noexcept {
    if (composer_owns_popup()) {
        popup_scene_close(&g_app.popup_scene);
        set_popup_owner(PopupOwnerKind::none);
    }
}

bool synchronize_autocomplete() noexcept {
    if (application_composer_text_input_composing() ||
        (g_app.profile && g_app.profile->scene.active) || !g_app.message_composer.focused) {
        g_app.composer_autocomplete.member_query_guild_id = 0;
        cancel_composer_member_query();
        close_composer_popup();
        return true;
    }
    if (!g_app.account_ui || g_app.login.visible || g_app.reaction_viewer.active ||
        (g_app.popup_scene.active && !composer_owns_popup())) {
        cancel_composer_member_query();
        return true;
    }
    bool commands_handled = false;
    if (!application_commands_sync(&commands_handled)) return false;
    if (commands_handled) {
        cancel_composer_member_query();
        return true;
    }
    AccountUiRuntime& ui = *g_app.account_ui;
    if (!composer_autocomplete_build(&g_app.composer_autocomplete, g_app.message_composer, ui.member_list,
                                     ui.user_store, ui.navigation_view, ui.chat_state, &ui.guild_member_store,
                                     &g_app.message_view.store)) {
        return false;
    }
    note_mention_member_query();
    const ComposerAutocomplete& autocomplete = g_app.composer_autocomplete;
    if (autocomplete.count == 0) {
        close_composer_popup();
        return true;
    }
    PopupItem items[kComposerAutocompleteCapacity]{};
    for (u32 index = 0; index < autocomplete.count; ++index) {
        items[index] = PopupItem{autocomplete.suggestions[index].label,
                                 popup_action_autocomplete_first + index, popup_item_enabled, 0};
        const auto& suggestion = autocomplete.suggestions[index];
        if (suggestion.user_id) {
            items[index].flags |= popup_item_avatar;
            items[index].detail = suggestion.username;
            items[index].media_id = suggestion.user_id;
            items[index].media_variant = suggestion.avatar_variant;
            items[index].media_flags = suggestion.animated_avatar ? 1 : 0;
            items[index].status = static_cast<u8>(suggestion.status);
        }
        if (suggestion.item_kind == ComposerAutocompleteKind::role ||
            suggestion.item_kind == ComposerAutocompleteKind::global_mention) {
            // The @ glyph, in the role colour when the role has one.
            items[index].presentation = ui_menu_row_presentation(UiIcon::mention);
            items[index].detail = suggestion.description;
            if (suggestion.role_color) {
                items[index].media_flags = kPopupItemMediaIconTint;
                items[index].media_variant = suggestion.role_color;
            }
        }
        if (suggestion.emoji_id) {
            items[index].flags |= popup_item_emoji_preview;
            items[index].media_id = suggestion.emoji_id;
            items[index].media_flags = suggestion.animated_emoji ? 1 : 0;
        } else if (autocomplete.kind == ComposerAutocompleteKind::emoji && suggestion.insertion_bytes) {
            // Insertion has one trailing space. Label remains the searchable
            // alias while its preview carries the exact Unicode identity.
            const auto flag = regional_flag_sequence({suggestion.insertion, suggestion.insertion_bytes - 1u});
            if (flag) {
                items[index].flags |= popup_item_emoji_preview;
                items[index].regional_flag = flag;
            }
        }
    }
    const auto* surface = ui_widget_manager_layout_node(g_app.message_composer_scene.widgets,
                                                         g_app.message_composer_scene.surface);
    if (!surface) return false;
    const UiRect anchor = surface->rectangle;
    const UiRect viewport = application_viewport_dips();
    const float preferred_width = ui_rect_width(anchor);
    const u64 owner_id = g_app.message_composer.channel_id != 0 ? g_app.message_composer.channel_id : 1;
    if (g_app.popup_scene.active && g_app.popup_owner_kind == PopupOwnerKind::autocomplete &&
        g_app.popup_scene.kind == PopupKind::autocomplete && g_app.popup_scene.owner_id == owner_id &&
        g_app.popup_scene.item_count == autocomplete.count &&
        g_app.popup_scene.items[0].flags == items[0].flags &&
        ui_rect_equal(g_app.popup_scene.anchor, anchor) &&
        ui_rect_equal(g_app.popup_scene.viewport, viewport) &&
        g_app.popup_scene.preferred_width == preferred_width) {
        // The suggestions live in stable application storage. Update their
        // views without resetting the retained popup's selection or scroll.
        for (u32 index = 0; index < autocomplete.count; ++index) {
            g_app.popup_scene.items[index] = items[index];
        }
        ++g_app.popup_scene.revision;
        return true;
    }
    const PopupSceneResult opened = popup_scene_open(
        &g_app.popup_scene, PopupKind::autocomplete, owner_id,
        ConstSlice<PopupItem>{items, autocomplete.count}, anchor, viewport, preferred_width, true);
    if (!opened.ok()) {
        return false;
    }
    set_popup_owner(PopupOwnerKind::autocomplete);
    return true;
}

} // namespace

bool refresh_active_autocomplete() noexcept {
    const bool shown = composer_owns_popup() && g_app.popup_scene.active;
    // An '@' query in a guild also re-runs while closed: members the gateway
    // returns for it may now match (the official client re-queries on member changes).
    return (!shown && !g_app.composer_autocomplete.member_query_guild_id) || synchronize_autocomplete();
}

void application_mention_member_query_tick(u64 now_ms) noexcept {
    GatewayGuildMemberRequest request{};
    if (!mention_member_query_due(g_mention_member_query, now_ms, &request))
        return;
    const bool admitted = g_app.account.instance && g_app.account.api.guild_members_request &&
                          g_app.account.api.guild_members_request(g_app.account.instance, &request);
    mention_member_query_complete(&g_mention_member_query, request, now_ms, admitted);
}

u64 application_mention_member_query_next_wake() noexcept {
    return mention_member_query_next_wake(g_mention_member_query);
}

void application_member_query_note(u64 guild_id, u64 channel_id, Utf8View query) noexcept {
    g_mention_member_query_composer = false;
    note_member_query(guild_id, channel_id, query);
}

void application_member_query_cancel() noexcept {
    if (!g_mention_member_query_composer)
        cancel_member_query();
}

void set_popup_owner(PopupOwnerKind owner) noexcept {
    application_popup_plugin_owner_transition(owner);
    if (owner != PopupOwnerKind::none && owner != PopupOwnerKind::autocomplete && owner != PopupOwnerKind::commands)
        application_composer_text_input_suspend();
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    if (owner != g_app.popup_owner_kind) message_composer_break_history_run(&g_app.message_composer);
    if (owner != g_app.popup_owner_kind) ui_scroll_host_cancel(&g_app.scroll_host);
    if (g_app.popup_owner_kind == PopupOwnerKind::message && owner != PopupOwnerKind::message) {
        message_context_controller_close(&g_app.message_context);
    }
    if (owner != PopupOwnerKind::none) {
        message_action_bar_close(&g_app.message_action_bar);
    }
    if(g_app.popup_owner_kind==PopupOwnerKind::commands && owner!=PopupOwnerKind::commands)application_commands_dismiss();
    g_app.popup_owner_kind = owner;
}

bool refresh_message_composer_ui() noexcept {
    return synchronize_shell_layout() && synchronize_autocomplete() && refresh_account_frame(true);
}

static u32 caret_blink_ms() noexcept {
    return g_app.login.caret_blink_ms != 0 ? g_app.login.caret_blink_ms : kDefaultCaretBlinkMilliseconds;
}

void reset_message_composer_caret_timing(bool edit_active) noexcept {
    if (!edit_active) message_composer_break_history_run(&g_app.message_composer);
    const bool visible = edit_active && g_app.account_ui && g_app.window && GetFocus() == g_app.window;
    const u32 blink_ms = caret_blink_ms();
    g_app.message_composer_caret_visible = visible;
    message_composer_scene_set_caret_visible(&g_app.message_composer_scene, visible);
    g_app.message_composer_caret_blink_at_ms =
        visible && blink_ms != INFINITE ? clock_monotonic_milliseconds() + blink_ms : 0;
}

void application_tick_message_composer_caret(u64 now_ms) noexcept {
    application_profile_tick(now_ms);
    application_mention_member_query_tick(now_ms);
    if (g_app.message_composer_caret_blink_at_ms == 0 ||
        now_ms < g_app.message_composer_caret_blink_at_ms) {
        return;
    }
    const u32 blink_ms = caret_blink_ms();
    if (!g_app.renderer_ready || !g_app.account_ui || !g_app.message_composer_scene.active ||
        !g_app.message_composer.focused || GetFocus() != g_app.window || blink_ms == INFINITE) {
        reset_message_composer_caret_timing(false);
        return;
    }
    g_app.message_composer_caret_visible = !g_app.message_composer_caret_visible;
    message_composer_scene_set_caret_visible(&g_app.message_composer_scene,
                                             g_app.message_composer_caret_visible);
    g_app.message_composer_caret_blink_at_ms = now_ms + blink_ms;
    application_request_frame();
}

bool stop_local_typing(u64 channel_id) noexcept {
    if (!g_app.account_ui || channel_id == 0) {
        return true;
    }
    const TypingStoreResult stopped =
        typing_store_stop_local(&g_app.account_ui->typing_store, channel_id);
    if (!stopped.ok()) {
        log_u64(LogLevel::warning, "typing", "local_stop_error", static_cast<u64>(stopped.error));
        return true;
    }
    if (stopped.changed) {
        refresh_account_text(true);
        if (g_app.window) {
            application_request_frame();
        }
    }
    return true;
}

bool note_local_typing_input() noexcept {
    if (!g_app.account_ui || g_app.message_edit.editor_active ||
        g_app.account.status.required_action != AccountRequirement::none) {
        return true;
    }
    AccountUiRuntime& ui = *g_app.account_ui;
    const u64 channel_id = ui.active_channel_id;
    if (channel_id == 0 || ui.chat_state.self_user_id == 0 ||
        g_app.message_composer.channel_id != channel_id) {
        return true;
    }
    if (g_app.message_composer.bytes == 0) {
        return stop_local_typing(channel_id);
    }
    const ChannelRecord* channel = chat_state_store_find_channel(ui.chat_state, channel_id);
    const TypingStoreResult noted = typing_store_note_local_input(
        &ui.typing_store, channel_id, channel ? channel->guild_id : 0, ui.chat_state.self_user_id,
        clock_monotonic_milliseconds());
    if (!noted.ok()) {
        log_u64(noted.error == TypingStoreError::capacity ? LogLevel::warning : LogLevel::error,
                "typing", "local_input_error", static_cast<u64>(noted.error));
    }
    return true;
}

bool composer_apply_result(MessageComposerResult result, bool content_input) noexcept {
    if (result == MessageComposerResult::changed) {
        if (!g_app.account_ui) {
            return false;
        }
        if (!g_app.message_edit.editor_active) {
            const MessageComposerControllerResult committed =
                message_composer_controller_commit_draft(&g_app.account_ui->message_composer_controller);
            if (!committed.ok()) {
                log_u64(LogLevel::error, "message", "composer_draft_commit_error",
                        static_cast<u64>(committed.error));
                return false;
            }
        }
        reset_message_composer_caret_timing(g_app.message_composer.focused);
        return (!content_input || note_local_typing_input()) && refresh_message_composer_ui();
    }
    return result == MessageComposerResult::unchanged || result == MessageComposerResult::capacity;
}

} // namespace maicord::app_detail
