#include "application_invite_cards.hpp"
#include "application_message_components.hpp"
#include "application_shortcuts.hpp"
#include "application_call.hpp"
#include "application_stream.hpp"
#include "application_guild_card.hpp"
#include "application_stage.hpp"
#include "application_soundboard.hpp"
#include "application_inbox.hpp"
#include "application_downloads.hpp"
#include "application_channel_snapshots.hpp"
#include "application_commands.hpp"
#include "application_ui_runtime.hpp"
#include "application_settings.hpp"
#include "application_theme_editor.hpp"
#include "application_custom_theme.hpp"
#include "application_friends.hpp"

#include "application_state.hpp"
#include "application_profile.hpp"
#include "application_text_form.hpp"
#include "maicord/model/reaction_preferences.hpp"
#include "application_forum_form.hpp"
#include "application_forum_post.hpp"
#include "application_composer_text_input.hpp"
#include "application_tooltip.hpp"
#include "application_account_menu.hpp"
#include "application_search.hpp"
#include "application_pinned_messages.hpp"
#include "application_media_picker.hpp"
#include "application_quick_switcher.hpp"
#include "application_message_forward.hpp"
#include "application_read_state.hpp"
#include "application_message_interaction.hpp"
#include "application_scoped_read.hpp"
#include "application_scoped_read_menu.hpp"
#include "application_message_history.hpp"
#include "application_thread_actions.hpp"
#include "application_thread_browser.hpp"
#include "application_thread_creation.hpp"
#include "application_poll.hpp"
#include "application_member_moderation.hpp"
#include "account_permission_storage.hpp"

#include "maicord/app/message_composer_controller.hpp"
#include "maicord/app/message_edit_runtime.hpp"
#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/message_media_controller.hpp"
#include "maicord/app/message_view_runtime.hpp"
#include "maicord/core/time.hpp"
#include "maicord/app/reaction_viewer_storage.hpp"
#include "maicord/model/typing_store.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/composer_autocomplete.hpp"
#include "maicord/ui/widget_storage.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
// The demand tail commits in steps, so growth costs a system call only every
// few hundred channels or 64 KiB of names.
constexpr u32 kChannelCommitStep = 256;
constexpr u32 kNameCommitStep = 64 * 1024;
constexpr u32 kEmojiCommitStep = 1'024;

u32 commit_step(u32 required, u32 step, u32 limit) noexcept {
    return minimum(limit, (required + step - 1) / step * step);
}

bool commit_bytes(void* base, usize from, usize to) noexcept {
    return to <= from || VirtualAlloc(static_cast<u8*>(base) + from, to - from, MEM_COMMIT, PAGE_READWRITE);
}

bool commit_chat_state_backing(void* context, ChatStateBacking backing, u32 bank, u32 required) noexcept {
    auto& ui = *static_cast<AccountUiRuntime*>(context);
    auto& committed = ui.demand_commit;
    if (backing == ChatStateBacking::channels) {
        if (required <= committed.channel_slots)
            return true;
        if (required > kChatStateChannelCapacity)
            return false;
        const u32 from = committed.channel_slots;
        const u32 slots = commit_step(required, kChannelCommitStep, kChatStateChannelCapacity);
        if (!commit_bytes(ui.channel_record_storage, usize{from} * sizeof(ChannelRecord),
                          usize{slots} * sizeof(ChannelRecord)) ||
            !commit_bytes(ui.channel_order_storage, usize{from} * sizeof(u16),
                          usize{slots} * sizeof(u16)) ||
            !commit_bytes(ui.navigation_visibility, from, slots))
            return false;
        committed.channel_slots = slots;
        return true;
    }
    if (backing == ChatStateBacking::emojis || backing == ChatStateBacking::emoji_roles) {
        const bool records = backing == ChatStateBacking::emojis;
        u32& done = records ? committed.emoji_records : committed.emoji_roles;
        const u32 limit = records ? kChatStateEmojiCapacity : kChatStateEmojiRoleCapacity;
        const usize size = records ? sizeof(CustomEmojiRecord) : sizeof(u64);
        if (required <= done)
            return true;
        if (required > limit)
            return false;
        const u32 count = commit_step(required, kEmojiCommitStep, limit);
        void* const base = records ? static_cast<void*>(ui.emoji_record_storage) : ui.emoji_role_storage;
        if (!commit_bytes(base, done * size, count * size))
            return false;
        done = count;
        return true;
    }
    constexpr u32 bank_bytes = kChatStateNameBytes / 2;
    if (bank > 1 || required > bank_bytes)
        return false;
    if (required <= committed.name_bytes[bank])
        return true;
    const u32 bytes = commit_step(required, kNameCommitStep, bank_bytes);
    u8* const bank_base = ui.chat_state_name_storage + usize{bank} * bank_bytes;
    if (!commit_bytes(bank_base, committed.name_bytes[bank], bytes))
        return false;
    committed.name_bytes[bank] = bytes;
    return true;
}

void release_shell_storage(Application& application) noexcept {
    popup_scene_release(&application.popup_scene);
    ui_widget_storage_release(&application.popup_storage);
    ui_widget_storage_release(&application.member_list_storage);
    ui_widget_storage_release(&application.shell_navigation_storage);
    ui_widget_storage_release(&application.guild_rail_storage);
    ui_virtual_row_storage_release(&application.member_list_rows);
    ui_virtual_row_storage_release(&application.shell_navigation_rows);
    ui_virtual_row_storage_release(&application.guild_rail_rows);
}

void release_overlay_owners() noexcept {
    application_account_menu_release();
    application_search_release();
    application_pinned_messages_release();
    application_inbox_release();
    application_thread_browser_release();
    application_media_picker_release();
    application_downloads_release();
    application_shortcuts_overlay_release();
    application_call_release();
    application_stream_release();
    application_guild_card_release();
    application_stage_release();
    application_soundboard_release();
    application_message_components_release();
    application_invite_cards_release();
    application_message_forward_release();
    application_quick_switcher_release();
}
} // namespace

bool application_initialize_shell_scenes(Application& application) noexcept {
    if (!ui_virtual_row_storage_initialize(&application.guild_rail_rows, kGuildRailRowCapacity,
                                           kInitialVirtualRowCapacity, sizeof(GuildRailRowWidget),
                                           alignof(GuildRailRowWidget)) ||
        !ui_widget_storage_initialize(&application.guild_rail_storage, kGuildRailWidgetCapacity,
                                      kGuildRailInitialWidgetCapacity) ||
        !guild_rail_scene_initialize(
            &application.guild_rail_scene,
            ui_virtual_row_storage_rows<GuildRailRowWidget>(application.guild_rail_rows),
            ui_widget_storage_widgets(application.guild_rail_storage),
            ui_widget_storage_layout_nodes(application.guild_rail_storage)) ||
        !ui_widget_manager_bind_growable_storage(&application.guild_rail_scene.widgets,
                                                 ui_widget_storage_grow, &application.guild_rail_storage) ||
        !ui_virtual_row_storage_initialize(&application.shell_navigation_rows, kShellNavigationRowCapacity,
                                           kInitialVirtualRowCapacity, sizeof(ShellNavigationRowWidget),
                                           alignof(ShellNavigationRowWidget)) ||
        !ui_widget_storage_initialize(&application.shell_navigation_storage, kShellNavigationWidgetCapacity,
                                      kShellNavigationInitialWidgetCapacity) ||
        !shell_navigation_scene_initialize(
            &application.shell_navigation_scene,
            ui_virtual_row_storage_rows<ShellNavigationRowWidget>(application.shell_navigation_rows),
            ui_widget_storage_widgets(application.shell_navigation_storage),
            ui_widget_storage_layout_nodes(application.shell_navigation_storage)) ||
        !ui_widget_manager_bind_growable_storage(&application.shell_navigation_scene.widgets,
                                                 ui_widget_storage_grow,
                                                 &application.shell_navigation_storage) ||
        !ui_virtual_row_storage_initialize(&application.member_list_rows, kMemberListRowCapacity,
                                           kInitialVirtualRowCapacity, sizeof(MemberListRowWidget),
                                           alignof(MemberListRowWidget)) ||
        !ui_widget_storage_initialize(&application.member_list_storage, kMemberListWidgetCapacity,
                                      kMemberListInitialWidgetCapacity) ||
        !member_list_scene_initialize(
            &application.member_list_scene,
            ui_virtual_row_storage_rows<MemberListRowWidget>(application.member_list_rows),
            ui_widget_storage_widgets(application.member_list_storage),
            ui_widget_storage_layout_nodes(application.member_list_storage)) ||
        !ui_widget_manager_bind_growable_storage(&application.member_list_scene.widgets,
                                                 ui_widget_storage_grow,
                                                 &application.member_list_storage) ||
        !ui_widget_storage_initialize(&application.popup_storage, kPopupWidgetCapacity,
                                      kPopupInitialWidgetCapacity) ||
        !popup_scene_initialize_owned(&application.popup_scene, kPopupItemCapacity,
                                      ui_widget_storage_widgets(application.popup_storage),
                                      ui_widget_storage_layout_nodes(application.popup_storage)) ||
        !ui_widget_manager_bind_growable_storage(&application.popup_scene.widgets, ui_widget_storage_grow,
                                                 &application.popup_storage)) {
        release_shell_storage(application);
        return false;
    }
    application.guild_rail_scene.row_storage = &application.guild_rail_rows;
    application.shell_navigation_scene.row_storage = &application.shell_navigation_rows;
    application.member_list_scene.row_storage = &application.member_list_rows;
    return true;
}

// The login screen never lays out messages: the scene reserves its address
// range at startup and commits its signed-in working set in
// application_initialize_account_ui.
bool application_initialize_message_scene(Application& application) noexcept {
    if (!ui_widget_storage_initialize(&application.message_scene_storage, kMessageSceneWidgetCapacity, 1)) {
        return false;
    }
    if (!message_scene_initialize_owned(
            &application.message_scene, &application.message_view.timeline, kMessageSceneRowCapacity, 32,
            ui_widget_storage_widgets(application.message_scene_storage),
            ui_widget_storage_layout_nodes(application.message_scene_storage), true) ||
        !ui_widget_manager_bind_growable_storage(&application.message_scene.widgets, ui_widget_storage_grow,
                                                 &application.message_scene_storage)) {
        message_scene_release(&application.message_scene);
        ui_widget_storage_release(&application.message_scene_storage);
        return false;
    }
    // The chat timeline sits against the composer like the official client's scroller.
    application.message_scene.bottom_anchored = true;
    return true;
}

void application_release_widget_storage(Application& application) noexcept {
    if (&application == &g_app) {
        application_theme_editor_release();
        application_tooltip_release();
        release_overlay_owners();
    }
    message_scene_release(&application.message_scene);
    ui_widget_storage_release(&application.message_scene_storage);
    message_composer_scene_release(&application.message_composer_scene);
    application_commands_release();
    composer_autocomplete_release(&application.composer_autocomplete);
    release_shell_storage(application);
    ui_scene_release(&application.shell_scene);
    application.member_list_scene.row_storage = nullptr;
    application.shell_navigation_scene.row_storage = nullptr;
    application.guild_rail_scene.row_storage = nullptr;
}

void application_release_account_ui(Application& application) noexcept {
    // Account stop owns worker quiescence before this UI teardown. Startup
    // failures have not published a read-state request to the account owner.
    if (&application == &g_app) {
        application_custom_theme_cancel_interaction();
        application_poll_release();
        application_member_moderation_release();
        application_tooltip_clear();
        release_overlay_owners();
        // Detach borrowed popup feedback while its retained owner is alive.
        application_scoped_read_menu_clear();
        if (application.scoped_read && application.popup_scene.active)
            dismiss_popup();
        application_scoped_read_release();
    }
    application_read_state_release(application);
    if (&application == &g_app) {
        application_friends_release();
        application_theme_editor_release();
        application_settings_release();
    }
    application.message_rejection = {};
    application.message_rejection_channel_id = 0;
    application.message_mutation_notice = {};
    if (&application == &g_app) {
        application_profile_release();
        application_image_viewer_close();
        application_text_form_close(false, false);
        application_thread_creation_release();
        application_forum_form_release();
        application_forum_post_release();
        application_composer_text_input_release();
    }
    application.image_viewer.scene.active = false;
    application.image_viewer.held_key = 0;
    application.renderer.image_viewer = nullptr;
    application.composer_menu_held_key = 0;
    application.pane_layout_pending = false;
    application.keyboard_focus_visible = false;
    if (!application.account_ui) {
        return;
    }
    message_composer_reset(&application.message_composer, 0, false);
    application.message_composer.focused = false;
    application.message_composer_caret_blink_at_ms = 0;
    application.message_composer_caret_visible = false;
    const bool composer_had_capture =
        application.message_composer_pointer_selection && GetCapture() == application.window;
    application.message_composer_pointer_selection = false;
    ui_widget_manager_capture(&application.message_composer_scene.widgets, ui_widget_invalid_handle());
    application.message_text_pointer_selection = false;
    application.message_text_selection = RendererMessageTextPositionHit{};
    application.message_text_selection_anchor = 0;
    application.message_text_selection_caret = 0;
    renderer_clear_message_text_selection(&application.renderer);
    if (composer_had_capture) {
        ReleaseCapture();
    }
    message_composer_scene_set_caret_visible(&application.message_composer_scene, false);
    message_event_pump_reset(&application.message_pump);
    if (&application == &g_app)
        application_channel_snapshots_reset();
    message_view_runtime_unbind_rich_store(&application.message_view);
    renderer_unbind_media_atlas(&application.renderer);
    application.renderer.message_measure_cache = nullptr;
    renderer_unbind_message_text_presentation(&application.renderer);
    renderer_unbind_text_storage(&application.renderer);
    renderer_unbind_navigation_view(&application.renderer);
    guild_rail_scene_update(&application.guild_rail_scene, nullptr, application.guild_rail_scene.viewport,
                            0.0f);
    shell_navigation_scene_update(&application.shell_navigation_scene, nullptr,
                                  application.shell_navigation_scene.viewport, 0.0f);
    application.message_catchup = MessageHistoryCatchup{};
    application.message_older = MessageHistoryOlder{};
    application.message_history_request_owner = MessageHistoryRequestOwner::none;
    application.details_request_owner = MessageHistoryRequestOwner::none;
    application.primary_request_id = 0;
    application.details_request_id = 0;
    application.attachment_refresh = AttachmentRefresh{};
    application.reply_refresh = ReplyReferenceRefresh{};
    application.media_urls = MediaUrlResolver{};
    application.message_delete = MessageDeleteRequest{};
    message_edit_runtime_release(&application.message_edit);
    if (&application == &g_app) {
        application_thread_actions_release();
        reset_message_reaction();
        application_message_history_reset_reaction_turn();
    } else {
        // The demand-backed reaction owner belongs only to the global host.
        application.message_reaction = MessageReactionRequest{};
    }
    application_commands_release();
    composer_autocomplete_release(&application.composer_autocomplete);
    message_media_controller_release(&application.message_media);
    animation_visual_runtime_reset(&application.animations);
    message_rich_store_release(&application.account_ui->message_rich_store);
    message_scene_bind_poll_source(&application.message_scene, nullptr);
    message_poll_store_release(&application.account_ui->message_poll_store);
    reaction_viewer_storage_release(&application.reaction_viewer_storage, &application.reaction_viewer,
                                    &application.reaction_viewer_controller);
    private_channel_store_release(&application.account_ui->private_channel_store);
    user_appearance_release(&application.account_ui->user_store.appearance);
    user_activity_release(&application.account_ui->user_store.activities);
    stable_storage_release(&application.account_ui->thread_member_banks[0]);
    stable_storage_release(&application.account_ui->thread_member_banks[1]);
    stable_storage_release(&application.account_ui->read_state_bank);
    application_history_decode_storage_release(*application.account_ui);
    reaction_preferences_release(&application.account_ui->reaction_preferences);
    account_permission_storage_release(*application.account_ui);
    if (application.member_list_scene.media_cache == &application.account_ui->member_media_cache)
        application.member_list_scene.media_cache = nullptr;
    VirtualFree(application.account_ui, 0, MEM_RELEASE);
    application.account_ui = nullptr;
}

bool application_initialize_account_ui(Application& application) noexcept {
    if (application.account_ui) {
        if (application.renderer.message_text == &application.account_ui->message_text)
            application.renderer.message_measure_cache = &application.account_ui->text_measure_cache;
        return true;
    }
    // Reserve the demand tail; commit only the fixed owner up front.
    auto* runtime = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE, PAGE_READWRITE));
    if (!runtime) {
        return false;
    }
    if (!VirtualAlloc(runtime, kAccountUiFixedBytes, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(runtime, 0, MEM_RELEASE);
        return false;
    }
    if (!application_history_decode_storage_initialize(*runtime) ||
        !chat_state_store_initialize_sparse(
            &runtime->chat_state,
            Slice<GuildRecord>{runtime->guild_record_storage, kChatStateGuildCapacity},
            Slice<u16>{runtime->guild_order_storage, kChatStateGuildCapacity},
            Slice<ChannelRecord>{runtime->channel_record_storage, kChatStateChannelCapacity},
            Slice<u16>{runtime->channel_order_storage, kChatStateChannelCapacity},
            Slice<u8>{runtime->chat_state_name_storage, kChatStateNameBytes}) ||
        !chat_state_store_bind_backing(&runtime->chat_state, commit_chat_state_backing, runtime) ||
        !stable_storage_initialize(&runtime->read_state_bank, kChatStateChannelCapacity, 0,
                                   sizeof(ReadStateEntry), alignof(ReadStateEntry)) ||
        !read_state_initialize(
            &runtime->read_state_store,
            {stable_storage_data<ReadStateEntry>(runtime->read_state_bank),
             kChatStateChannelCapacity}) ||
        !read_state_set_reserve(
            &runtime->read_state_store,
            +[](void* context, u32 count) noexcept {
                return stable_storage_reserve(static_cast<StableStorage*>(context), count);
            },
            &runtime->read_state_bank) ||
        !read_state_bind(&runtime->chat_state, &runtime->read_state_store) ||
        !account_permission_storage_initialize(*runtime) ||
        !chat_state_store_bind_emojis_sparse(
            &runtime->chat_state,
            Slice<CustomEmojiRecord>{runtime->emoji_record_storage, kChatStateEmojiCapacity},
            Slice<u64>{runtime->emoji_role_storage, kChatStateEmojiRoleCapacity}) ||
        !user_store_initialize_sparse(
            &runtime->user_store,
            Slice<UserRecord>{runtime->user_record_storage, kUserCapacity},
            Slice<u32>{runtime->user_order_storage, kUserCapacity},
            Slice<u8>{runtime->user_name_storage, kUserNameBytes}) ||
        !private_channel_store_initialize_owned(&runtime->private_channel_store,
                                                kPrivateChannelCapacity,
                                                kPrivateChannelCapacity * 16) ||
        !guild_member_store_initialize(
            &runtime->guild_member_store,
            Slice<GuildMemberRecord>{runtime->guild_member_record_storage, kGuildMemberCapacity},
            Slice<u32>{runtime->guild_member_order_storage, kGuildMemberCapacity}) ||
        !stable_storage_initialize(&runtime->thread_member_banks[0], kThreadMemberMaximumIds, 0,
                                   sizeof(u64), alignof(u64)) ||
        !stable_storage_initialize(&runtime->thread_member_banks[1], kThreadMemberMaximumIds, 0,
                                   sizeof(u64), alignof(u64)) ||
        !thread_member_store_initialize(
            &runtime->thread_member_store,
            {stable_storage_data<u64>(runtime->thread_member_banks[0]), kThreadMemberMaximumIds},
            {stable_storage_data<u64>(runtime->thread_member_banks[1]), kThreadMemberMaximumIds}) ||
        !thread_member_store_set_reserve(
            &runtime->thread_member_store,
            +[](void* context, u32 count) noexcept {
                auto* ui = static_cast<AccountUiRuntime*>(context);
                return stable_storage_reserve(&ui->thread_member_banks[0], count) &&
                       stable_storage_reserve(&ui->thread_member_banks[1], count);
            },
            runtime) ||
        !typing_store_initialize(
            &runtime->typing_store,
            Slice<TypingRecord>{runtime->typing_record_storage, kTypingRecordCapacity}) ||
        !voice_state_store_initialize_sparse(
            &runtime->voice_states,
            Slice<VoiceState>{runtime->voice_state_records, kVoiceStateCapacity}) ||
        !member_list_view_initialize(
            &runtime->member_list,
            Slice<MemberListRow>{runtime->member_list_rows, kMemberListViewCapacity},
            Slice<u32>{runtime->projection_scratch.member_list, kGuildMemberCapacity}) ||
        // The whole owner is committed. Readers use only the active text
        // prefix, so initialization and reset need only two bank sentinels.
        !message_rich_store_initialize_owned_sparse(
            &runtime->message_rich_store, &runtime->message_rich_record_storage, kMessageViewItemCapacity,
            Slice<u16>{runtime->message_rich_order, kMessageViewItemCapacity},
            Slice<u8>{runtime->message_rich_text, kMessageRichTextCapacity}) ||
        !message_poll_store_initialize_owned(&runtime->message_poll_store) ||
        !media_atlas_initialize(
            &runtime->media_atlas, kMediaAtlasExtent, kMediaAtlasExtent, 1,
            Slice<MediaAtlasEntry>{runtime->media_atlas_entries, kMediaAtlasEntryCapacity},
            Slice<u32>{runtime->media_atlas_buckets, kMediaAtlasEntryCapacity * 2},
            Slice<GlyphAtlasEntry>{runtime->media_atlas_allocations, kMediaAtlasEntryCapacity},
            Slice<GlyphAtlasRow>{runtime->media_atlas_rows, kMediaAtlasRowCapacity}, true) ||
        !media_atlas_configure_growth(&runtime->media_atlas, kMediaAtlasInitialHeight, kMediaAtlasGrowthStep) ||
        !media_atlas_initialize(
            &runtime->preview_media_atlas, kPreviewMediaAtlasExtent, kPreviewMediaAtlasExtent, 1,
            Slice<MediaAtlasEntry>{runtime->preview_media_atlas_entries, kPreviewMediaAtlasEntryCapacity},
            Slice<u32>{runtime->preview_media_atlas_buckets, kPreviewMediaAtlasEntryCapacity * 2},
            Slice<GlyphAtlasEntry>{runtime->preview_media_atlas_allocations,
                                   kPreviewMediaAtlasEntryCapacity},
            Slice<GlyphAtlasRow>{runtime->preview_media_atlas_rows, kPreviewMediaAtlasRowCapacity}, true) ||
        !media_atlas_configure_growth(&runtime->preview_media_atlas, kPreviewMediaAtlasInitialHeight,
                                      kPreviewMediaAtlasGrowthStep) ||
        !message_view_runtime_bind_rich_store(&application.message_view, &runtime->message_rich_store)
             .ok() ||
        !navigation_view_initialize_sparse(
            &runtime->navigation_view,
            Slice<GuildViewRow>{runtime->navigation_guild_rows, kChatStateGuildCapacity * 2},
            Slice<ChannelViewRow>{runtime->navigation_channel_rows, kNavigationChannelRows},
            Slice<u16>{runtime->projection_scratch.navigation_channels, kNavigationChannelRows},
            Slice<u8>{runtime->navigation_visibility, kChatStateChannelCapacity},
            Slice<ChannelSortKey>{runtime->navigation_sort_keys, kNavigationChannelRows}) ||
        !message_event_pump_initialize(
            &application.message_pump, &application.message_view.timeline,
            Slice<u8>{runtime->event_payload_scratch, kGatewayClientDispatchPayloadCapacity},
            Slice<u64>{runtime->message_bulk_ids, kGatewayBulkDeleteMaximumIds},
            MessageContentAvailability::unknown) ||
        !message_event_pump_set_state_store(
            &application.message_pump, &runtime->chat_state,
            Slice<u64>{runtime->guild_id_scratch, kChatStateGuildCapacity}) ||
        !guild_detail_store_bind(&runtime->guild_details,
                                 Slice<GuildDetailRecord>{runtime->guild_detail_records, kChatStateGuildCapacity},
                                 Slice<char>{runtime->guild_detail_text, kGuildDetailTextBytes}) ||
        !message_event_pump_set_user_store(&application.message_pump, &runtime->user_store) ||
        !message_event_pump_set_member_store(&application.message_pump, &runtime->guild_member_store) ||
        !message_event_pump_set_thread_member_store(&application.message_pump,
                                                    &runtime->thread_member_store) ||
        !message_event_pump_set_typing_store(&application.message_pump, &runtime->typing_store) ||
        !message_event_pump_set_voice_state_store(&application.message_pump, &runtime->voice_states) ||
        // Fresh committed (zero) pages: the text banks commit on first use.
        !message_draft_store_initialize_sparse(
            &runtime->message_drafts,
            Slice<MessageDraftRecord>{runtime->message_draft_records, kMessageDraftCapacity},
            Slice<u8>{runtime->message_draft_text, kMessageDraftTextCapacity}) ||
        !reaction_preferences_initialize(&runtime->reaction_preferences) ||
        !message_composer_controller_initialize_sparse(
            &runtime->message_composer_controller, &application.message_composer, &runtime->message_drafts,
            &application.message_view.timeline,
            Slice<u8>{runtime->message_request_text, kMessageRequestTextCapacity}) ||
        !message_edit_runtime_initialize(&application.message_edit) ||
        !message_media_controller_initialize_owned(&application.message_media, kMessageSceneRowCapacity,
                                                   kMessageMediaAssetMarkCapacity) ||
        !message_event_pump_set_send_tracker(
            &application.message_pump,
            message_composer_controller_send_tracker(&runtime->message_composer_controller))) {
        message_event_pump_set_send_tracker(&application.message_pump, nullptr);
        message_event_pump_set_typing_store(&application.message_pump, nullptr);
        message_event_pump_set_voice_state_store(&application.message_pump, nullptr);
        message_view_runtime_unbind_rich_store(&application.message_view);
        message_event_pump_reset(&application.message_pump);
        message_edit_runtime_release(&application.message_edit);
        message_media_controller_release(&application.message_media);
        message_rich_store_release(&runtime->message_rich_store);
        message_poll_store_release(&runtime->message_poll_store);
        private_channel_store_release(&runtime->private_channel_store);
        stable_storage_release(&runtime->thread_member_banks[0]);
        stable_storage_release(&runtime->thread_member_banks[1]);
        stable_storage_release(&runtime->read_state_bank);
        application_history_decode_storage_release(*runtime);
        reaction_preferences_release(&runtime->reaction_preferences);
        account_permission_storage_release(*runtime);
        VirtualFree(runtime, 0, MEM_RELEASE);
        return false;
    }
    application.account_ui = runtime;
    message_event_pump_set_guild_detail_store(&application.message_pump, &runtime->guild_details);
    runtime->message_poll_binding = {
        &runtime->message_poll_store,
        &runtime->text_measure_cache,
        clock_unix_milliseconds(),
        renderer_text_measure_revision(application.renderer),
        false,
        true,
        +[](void*, u64 id, Slice<UiPollAnswer> answers, UiPollState* state) noexcept {
            return application_poll_state(id, answers, state);
        },
        nullptr};
    message_scene_bind_poll_source(&application.message_scene, &runtime->message_poll_binding);
    runtime->chat_state.private_channels = &runtime->private_channel_store;
    renderer_bind_navigation_view(&application.renderer, &runtime->chat_state, &runtime->user_store,
                               &runtime->navigation_view);
    if (!renderer_bind_media_atlas(&application.renderer, &runtime->media_atlas) ||
        !renderer_bind_preview_media_atlas(&application.renderer, &runtime->preview_media_atlas)) {
        application_release_account_ui(application);
        return false;
    }
    runtime->renderer_text.quads = runtime->renderer_text_quads;
    runtime->renderer_text.batches = runtime->renderer_text_batches;
    runtime->renderer_text.utf16 = runtime->response_shape_scratch.shape.utf16;
    runtime->renderer_text.glyphs = runtime->response_shape_scratch.shape.glyphs;
    runtime->renderer_text.runs = runtime->response_shape_scratch.shape.runs;
    runtime->renderer_text.clusters = runtime->response_shape_scratch.shape.clusters;
    runtime->renderer_text.decorations =
        reinterpret_cast<TextDecoration*>(runtime->event_payload_scratch + kRendererDecorationOffset);
    runtime->renderer_text.hit_spans =
        reinterpret_cast<TextHitSpan*>(runtime->event_payload_scratch + kRendererHitSpanOffset);
    runtime->renderer_text.hit_region_scratch =
        reinterpret_cast<TextHitRegion*>(runtime->event_payload_scratch + kRendererHitRegionOffset);
    runtime->renderer_text.content_semantics = runtime->renderer_content_semantics;
    runtime->renderer_text.content_hit_regions = runtime->renderer_content_hit_regions;
    runtime->renderer_text.quad_capacity = kRendererContentGlyphCapacity;
    runtime->renderer_text.batch_capacity = kRendererContentBatchCapacity;
    runtime->renderer_text.utf16_capacity = kRendererContentTextUnitCapacity;
    runtime->renderer_text.glyph_capacity = kRendererContentTextUnitCapacity;
    runtime->renderer_text.run_capacity = kRendererContentRunCapacity;
    runtime->renderer_text.cluster_capacity = kRendererContentTextUnitCapacity;
    runtime->renderer_text.hit_span_capacity = kTextHitSpanMaximum;
    runtime->renderer_text.hit_region_scratch_capacity = kRendererContentHitRegionCapacity;
    runtime->renderer_text.decoration_capacity = kRendererTextDecorationCapacity;
    runtime->renderer_text.content_semantic_capacity = kRendererContentSemanticCapacity;
    runtime->renderer_text.content_hit_region_capacity = kRendererContentHitRegionCapacity;
    if (!message_text_presentation_initialize(
            &runtime->message_text,
            Slice<MessageTextCommand>{runtime->message_text_commands, kMaximumMessageTextCommands},
            Slice<char>{runtime->message_text_utf8, kMaximumMessageTextUtf8Bytes},
            Slice<TextStyleSpan>{runtime->message_text_styles, kMaximumMessageTextStyleSpans},
            Slice<MessageMarkdownSemantic>{runtime->message_text_semantics, kMaximumMessageTextSemantics},
            Slice<MessageTextMeasurement>{runtime->message_text_measurements,
                                          kMaximumMessageTextMeasurements},
            Slice<MessageTextSpoilerReveal>{runtime->message_text_spoiler_reveals,
                                            kMaximumMessageTextSpoilerReveals},
            Slice<u32>{reinterpret_cast<u32*>(runtime->event_payload_scratch + kMessageTextByteStyleOffset),
                       kMessageTextByteStyleScratchCapacity}) ||
        !message_text_presentation_bind_guild_members(&runtime->message_text, &runtime->guild_member_store) ||
        !message_markdown_cache_initialize(
            &runtime->message_markdown_cache,
            Slice<MessageMarkdownCacheEntry>{runtime->message_markdown_cache_entries, kMessageMarkdownCacheSlots},
            Slice<u8>{runtime->message_markdown_cache_arena, kMessageMarkdownCacheArenaBytes}) ||
        !message_text_presentation_bind_markdown_cache(&runtime->message_text, &runtime->message_markdown_cache) ||
        !renderer_bind_text_storage(&application.renderer, &runtime->renderer_text) ||
        !ui_text_measure_cache_initialize(
            &runtime->text_measure_cache,
            Slice<UiTextMeasureRecord>{runtime->message_text_measure_cache_records,
                                       kMaximumMessageTextMeasureCacheRecords},
            renderer_text_measure_backend, &application.renderer) ||
        !renderer_bind_message_text_presentation(&application.renderer, &runtime->message_text) ||
        !ui_widget_storage_reserve(&application.message_scene_storage, kMessageSceneInitialWidgetCapacity) ||
        !message_scene_reserve_measurements(&application.message_scene)) {
        application_release_account_ui(application);
        return false;
    }
    application.renderer.message_measure_cache = &runtime->text_measure_cache;
    return true;
}

} // namespace maicord::app_detail
