#include "maicord/core/base.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
void test_theme_styles(void (*check)(bool, const char*) noexcept) noexcept;
void test_desktop_integration(void (*check)(bool, const char*) noexcept) noexcept;
void test_theme_style_projection(void (*check)(bool, const char*) noexcept) noexcept;
void test_client_theme_storage(void (*check)(bool, const char*) noexcept) noexcept;
void test_client_theme_document(void (*check)(bool, const char*) noexcept) noexcept;
void test_theme_tokens(void (*check)(bool, const char*) noexcept) noexcept;
void test_surface_elevation(void (*check)(bool, const char*) noexcept) noexcept;
void test_gateway_message_poll(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_poll_store(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_poll_actions(void (*check)(bool, const char*) noexcept) noexcept;
void test_poll_controller(void (*check)(bool, const char*) noexcept) noexcept;
void test_poll_controls(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_poll_presentation(void (*check)(bool, const char*) noexcept) noexcept;
void test_spinner(void (*check)(bool, const char*) noexcept) noexcept;
void test_ui_transition(void (*check)(bool, const char*) noexcept) noexcept;
void test_tooltip(void (*check)(bool, const char*) noexcept) noexcept;
void test_account_menu(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_search(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_rich_store_sparse(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_pins(void (*check)(bool, const char*) noexcept) noexcept;
void test_forwarded_message_snapshots(void (*check)(bool, const char*) noexcept) noexcept;
void test_forwarded_message_pages(void (*check)(bool, const char*) noexcept) noexcept;
void test_media_picker(void (*check)(bool, const char*) noexcept) noexcept;
void test_channel_overview(void (*check)(bool, const char*) noexcept) noexcept;
void test_channel_content_policy(void (*check)(bool, const char*) noexcept) noexcept;
void test_channel_overview_controller(void (*check)(bool, const char*) noexcept) noexcept;
void test_pinned_message_controller(void (*check)(bool, const char*) noexcept) noexcept;
void test_quick_switcher_controller(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_forward_controller(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_forward_source_flags(void (*check)(bool, const char*) noexcept) noexcept;
void test_bounded_edit(void (*check)(bool, const char*) noexcept) noexcept;
void test_gif_favorites(void (*check)(bool, const char*) noexcept) noexcept;
void test_emoji_preferences(void (*check)(bool, const char*) noexcept) noexcept;
void test_thread_starter(void (*check)(bool, const char*) noexcept) noexcept;
void test_media_catalog(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_create_stickers(void (*check)(bool, const char*) noexcept) noexcept;
void test_poll_draft(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_composer_stickers(void (*check)(bool, const char*) noexcept) noexcept;
void test_search_scene(void (*check)(bool, const char*) noexcept) noexcept;
void test_pinned_messages_scene(void (*check)(bool, const char*) noexcept) noexcept;
void test_inbox_scene(void (*check)(bool, const char*) noexcept) noexcept;
void test_thread_browser_scene(void (*check)(bool, const char*) noexcept) noexcept;
void test_forum_post_scene(void (*check)(bool, const char*) noexcept) noexcept;
void test_quick_switcher_scene(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_forward_scene(void (*check)(bool, const char*) noexcept) noexcept;
void test_download_scene(void (*check)(bool, const char*) noexcept) noexcept;
void test_http_response_stream(void (*check)(bool, const char*) noexcept) noexcept;
void test_attachment_download(void (*check)(bool, const char*) noexcept) noexcept;
void test_https_file_download(void (*check)(bool, const char*) noexcept) noexcept;
void test_staged_file(void (*check)(bool, const char*) noexcept) noexcept;
void test_search_query(void (*check)(bool, const char*) noexcept) noexcept;
void test_search_filter_context(void (*check)(bool, const char*) noexcept) noexcept;
void test_search_recent(void (*check)(bool, const char*) noexcept) noexcept;
void test_link_trust(void (*check)(bool, const char*) noexcept) noexcept;
void test_application_video_request(void (*check)(bool, const char*) noexcept) noexcept;
void test_menu_row(void (*check)(bool, const char*) noexcept) noexcept;
void test_icon_pack(void (*check)(bool, const char*) noexcept) noexcept;
void test_update_manifest(void (*check)(bool, const char*) noexcept) noexcept;
void test_update_artifact(void (*check)(bool, const char*) noexcept) noexcept;
void test_update_acceptance(void (*check)(bool, const char*) noexcept) noexcept;
void test_update_prepare(void (*check)(bool, const char*) noexcept) noexcept;
void test_update_package(void (*check)(bool, const char*) noexcept) noexcept;
void test_regional_flag_media(void (*check)(bool, const char*) noexcept) noexcept;
void test_flag_surfaces(void (*check)(bool, const char*) noexcept) noexcept;
void test_composer_source_spans(void (*check)(bool, const char*) noexcept) noexcept;
void test_reaction_sections(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_reaction_controller(void (*check)(bool, const char*) noexcept) noexcept;
void test_application_login_geometry(void (*check)(bool, const char*) noexcept) noexcept;
void test_tsf_end_edit(void (*check)(bool, const char*) noexcept) noexcept;
void test_member_list_sparse_model(void (*check)(bool, const char*) noexcept) noexcept;
void test_member_list_sparse_wire(void (*check)(bool, const char*) noexcept) noexcept;
void test_member_list_visible_users(void (*check)(bool, const char*) noexcept) noexcept;
void test_member_user_perf(void (*check)(bool, const char*) noexcept) noexcept;
void test_member_paging_pump(void (*check)(bool, const char*) noexcept) noexcept;
void test_thread_members(void (*check)(bool, const char*) noexcept) noexcept;
void test_thread_authority_model(void (*check)(bool, const char*) noexcept) noexcept;
void test_thread_header(void (*check)(bool, const char*) noexcept) noexcept;
void test_application_thread_header_activation(void (*check)(bool, const char*) noexcept) noexcept;
void test_thread_archive(void (*check)(bool, const char*) noexcept) noexcept;
void test_thread_storage(void (*check)(bool, const char*) noexcept) noexcept;
void test_read_state_model(void (*check)(bool, const char*) noexcept) noexcept;
void test_read_state_wire(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_ack(void (*check)(bool, const char*) noexcept) noexcept;
void test_bulk_message_ack(void (*check)(bool, const char*) noexcept) noexcept;
void test_read_scope(void (*check)(bool, const char*) noexcept) noexcept;
void test_bulk_read_ack_controller(void (*check)(bool, const char*) noexcept) noexcept;
void test_read_ack_controller(void (*check)(bool, const char*) noexcept) noexcept;
void test_channel_context_policy(void (*check)(bool, const char*) noexcept) noexcept;
void test_guild_context_policy(void (*check)(bool, const char*) noexcept) noexcept;
void test_user_context_menu(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_context_extras(void (*check)(bool, const char*) noexcept) noexcept;
void test_user_note_store(void (*check)(bool, const char*) noexcept) noexcept;
void test_active_now(void (*check)(bool, const char*) noexcept) noexcept;
void test_friends_active_now(void (*check)(bool, const char*) noexcept) noexcept;
void test_unread_context(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_unread_context_menu(void (*check)(bool, const char*) noexcept) noexcept;
void test_read_state_pump(void (*check)(bool, const char*) noexcept) noexcept;
void test_read_ack_visibility(void (*check)(bool, const char*) noexcept) noexcept;
void test_mention_badge_text(void (*check)(bool, const char*) noexcept) noexcept;
void test_gateway_ready_projection(void (*check)(bool, const char*) noexcept) noexcept;
void test_text_emitter(void (*check)(bool, const char*) noexcept) noexcept;
void test_widget_builder(void (*check)(bool, const char*) noexcept) noexcept;
u32 test_thread_member_viewport() noexcept;
void test_thread_member_pump(void (*check)(bool, const char*) noexcept) noexcept;
void test_thread_subscriptions(void (*check)(bool, const char*) noexcept) noexcept;
void test_role_lookup_performance(void (*check)(bool, const char*) noexcept) noexcept;
void test_role_order(void (*check)(bool, const char*) noexcept) noexcept;
void test_guild_role_replacement(void (*check)(bool, const char*) noexcept) noexcept;
void test_permission_prefix(void (*check)(bool, const char*) noexcept) noexcept;
void test_guild_channel_validation(void (*check)(bool, const char*) noexcept) noexcept;
void test_channel_name_preflight(void (*check)(bool, const char*) noexcept) noexcept;
void test_chat_state_backing(void (*check)(bool, const char*) noexcept) noexcept;
u32 test_member_viewport_ranges_and_sparse_scene() noexcept;
u32 test_member_visible_avatar_requests() noexcept;
u32 test_member_media_cache_scroll() noexcept;
void test_ui_page_registry(void (*check)(bool, const char*) noexcept) noexcept;
void test_plugin_host(void (*check)(bool, const char*) noexcept) noexcept;
void test_message_context_controller_extensions(void (*check)(bool, const char*) noexcept) noexcept;
void test_settings_scene(void (*check)(bool, const char*) noexcept) noexcept;
void test_profile_editor_layout(void (*check)(bool, const char*) noexcept) noexcept;
void test_settings_choice(void (*check)(bool, const char*) noexcept) noexcept;
void test_password_recovery(void (*check)(bool, const char*) noexcept) noexcept;
void test_password_reset_ui(void (*check)(bool, const char*) noexcept) noexcept;
void test_phone_recovery(void (*check)(bool, const char*) noexcept) noexcept;
void test_password_phone_ui(void (*check)(bool, const char*) noexcept) noexcept;
void test_friends_scene(void (*check)(bool, const char*) noexcept) noexcept;
void test_friends_media(void (*check)(bool, const char*) noexcept) noexcept;
void test_text_form_confirmation(void (*check)(bool, const char*) noexcept) noexcept;
void test_friend_search(void (*check)(bool, const char*) noexcept) noexcept;
void test_relationship_sync(void (*check)(bool, const char*) noexcept) noexcept;
void test_client_preferences(void (*check)(bool, const char*) noexcept) noexcept;
void test_visible_author_hydration(void (*check)(bool, const char*) noexcept) noexcept;
void test_scroll_host_preferences(void (*check)(bool, const char*) noexcept) noexcept;
void test_menu_rail_smooth_scroll(void (*check)(bool, const char*) noexcept) noexcept;
void test_background_theme(void (*check)(bool, const char*) noexcept) noexcept;
void test_background_effect_policy(void (*check)(bool, const char*) noexcept) noexcept;
void test_background_color_math(void (*check)(bool, const char*) noexcept) noexcept;
void test_theme_editor(void (*check)(bool, const char*) noexcept) noexcept;
void test_login_continuations(void (*check)(bool, const char*) noexcept) noexcept;
void test_client_preferences_writer(void (*check)(bool, const char*) noexcept) noexcept;
void test_plugin_module_loader(void (*check)(bool, const char*) noexcept) noexcept;
void test_plugin_catalog(void (*check)(bool, const char*) noexcept) noexcept;
void test_plugin_bundle_install(void (*check)(bool, const char*) noexcept) noexcept;
void test_video_controls(void (*check)(bool, const char*) noexcept) noexcept;
void test_audio_controls(void (*check)(bool, const char*) noexcept) noexcept;
void test_ui_cursor(void (*check)(bool, const char*) noexcept) noexcept;
void test_audio_presentation(void (*check)(bool, const char*) noexcept) noexcept;
void test_ogg_opus(void (*check)(bool, const char*) noexcept) noexcept;
void test_opus_decoder(void (*check)(bool, const char*) noexcept) noexcept;
void test_wave_pcm(void (*check)(bool, const char*) noexcept) noexcept;
void test_mp3_stream(void (*check)(bool, const char*) noexcept) noexcept;
void test_video_scene(void (*check)(bool, const char*) noexcept) noexcept;
void test_account_video_session(void (*check)(bool, const char*) noexcept) noexcept;
void test_account_video_commands(void (*check)(bool, const char*) noexcept) noexcept;
void test_account_media_video_dispatch(void (*check)(bool, const char*) noexcept) noexcept;
void test_local_draft_preview(void (*check)(bool, const char*) noexcept) noexcept;
void test_aac_decoder(void (*check)(bool, const char*) noexcept) noexcept;
void test_hevc(void (*check)(bool, const char*) noexcept) noexcept;
void test_video_frame_bounds(void (*check)(bool, const char*) noexcept) noexcept;
void test_media_audio(void (*check)(bool, const char*) noexcept) noexcept;
void test_browser_sign_in(void (*check)(bool, const char*) noexcept) noexcept;
void test_browser_sign_in_receiver(void (*check)(bool, const char*) noexcept) noexcept;
void test_browser_login_ui(void (*check)(bool, const char*) noexcept) noexcept;
void test_gateway_self_status(void (*check)(bool, const char*) noexcept) noexcept;
void test_application_plugin_settings_projection(void (*check)(bool, const char*) noexcept) noexcept;
void test_plugin_preferences(void (*check)(bool, const char*) noexcept) noexcept;
namespace {
u32 checks = 0, failures = 0;
void check(bool success, const char* name) noexcept {
    ++checks;
    if (success)
        return;
    ++failures;
    test_write_text("FAIL ");
    test_write_line(name);
}
using ExtensionCheck = void (*)(bool, const char*) noexcept;
using ExtensionFixture = void (*)(ExtensionCheck) noexcept;
// Keep the original fixture order. The update fixtures share one shard so
// durable staging/extraction work stays serial; the remaining fixtures run
// round-robin with one process/profile per shard and no shared mutable globals.
constexpr ExtensionFixture extension_fixtures[]{
    test_update_manifest,
    test_update_artifact,
    test_update_acceptance,
    test_update_prepare,
    test_update_package,
    test_desktop_integration,
    test_regional_flag_media,
    test_flag_surfaces,
    test_composer_source_spans,
    test_reaction_sections,
    test_message_reaction_controller,
    test_gateway_message_poll,
    test_message_poll_store,
    test_message_poll_actions,
    test_poll_controller,
    test_poll_controls,
    test_message_poll_presentation,
    test_application_login_geometry,
    test_tsf_end_edit,
    test_member_list_sparse_model,
    test_member_list_sparse_wire,
    test_member_list_visible_users,
    test_member_user_perf,
    test_member_paging_pump,
    test_thread_members,
    test_thread_authority_model,
    test_thread_header,
    test_application_thread_header_activation,
    test_thread_archive,
    test_thread_storage,
    test_read_state_model,
    test_read_state_wire,
    test_message_ack,
    test_bulk_message_ack,
    test_read_scope,
    test_bulk_read_ack_controller,
    test_read_ack_controller,
    test_channel_context_policy,
    test_guild_context_policy,
    test_user_context_menu,
    test_message_context_extras,
    test_user_note_store,
    test_active_now,
    test_friends_active_now,
    test_unread_context,
    test_message_unread_context_menu,
    test_read_state_pump,
    test_read_ack_visibility,
    test_mention_badge_text,
    test_gateway_ready_projection,
    test_text_emitter,
    test_widget_builder,
    [](ExtensionCheck verify) noexcept {
        verify(test_thread_member_viewport() == 0, "thread member viewport and scene regressions");
    },
    test_thread_member_pump,
    test_thread_subscriptions,
    test_role_lookup_performance,
    test_role_order,
    test_guild_role_replacement,
    test_permission_prefix,
    test_guild_channel_validation,
    // Name admission and the demand backing it relies on share a fixture slot.
    [](ExtensionCheck verify) noexcept {
        test_channel_name_preflight(verify);
        test_chat_state_backing(verify);
    },
    [](ExtensionCheck verify) noexcept {
        verify(test_member_viewport_ranges_and_sparse_scene() == 0, "member viewport planner and sparse scene regressions");
    },
    [](ExtensionCheck verify) noexcept {
        verify(test_member_visible_avatar_requests() == 0, "member list: only on-screen rows request shared avatars; guild avatars keep their own identity");
    },
    [](ExtensionCheck verify) noexcept {
        verify(test_member_media_cache_scroll() == 0, "member list media resolve once per user: unchanged frames 0, scroll only entering rows");
    },
    test_ui_page_registry,
    test_plugin_host,
    test_message_context_controller_extensions,
    test_settings_scene,
    test_profile_editor_layout,
    test_settings_choice,
    test_password_recovery,
    test_password_reset_ui,
    test_phone_recovery,
    test_password_phone_ui,
    test_friends_scene,
    test_friends_media,
    test_text_form_confirmation,
    test_friend_search,
    test_relationship_sync,
    test_client_preferences,
    test_visible_author_hydration,
    test_scroll_host_preferences,
    test_menu_rail_smooth_scroll,
    test_background_theme,
    test_background_effect_policy,
    test_background_color_math,
    test_theme_editor,
    test_theme_styles,
    test_theme_style_projection,
    test_client_theme_storage,
    test_client_theme_document,
    test_theme_tokens,
    test_surface_elevation,
    test_login_continuations,
    test_menu_row,
    test_icon_pack,
    test_client_preferences_writer,
    test_plugin_module_loader,
    test_plugin_catalog,
    test_plugin_bundle_install,
    test_video_controls,
    test_audio_controls,
    test_ui_cursor,
    test_audio_presentation,
    test_ogg_opus,
    test_opus_decoder,
    test_wave_pcm,
    test_mp3_stream,
    test_video_scene,
    test_spinner,
    test_ui_transition,
    test_tooltip,
    test_account_menu,
    test_message_search,
    test_message_rich_store_sparse,
    test_message_pins,
    test_forwarded_message_snapshots,
    test_forwarded_message_pages,
    test_search_scene,
    test_pinned_messages_scene,
    test_inbox_scene,
    test_thread_browser_scene,
    test_forum_post_scene,
    test_quick_switcher_scene,
    test_message_forward_scene,
    test_download_scene,
    test_http_response_stream,
    test_attachment_download,
    test_https_file_download,
    test_staged_file,
    test_media_picker,
    test_channel_overview,
    test_channel_content_policy,
    test_channel_overview_controller,
    test_pinned_message_controller,
    test_quick_switcher_controller,
    test_message_forward_controller,
    test_message_forward_source_flags,
    test_bounded_edit,
    test_gif_favorites,
    test_emoji_preferences,
    test_thread_starter,
    test_media_catalog,
    test_message_create_stickers,
    test_poll_draft,
    test_message_composer_stickers,
    test_search_query,
    test_search_filter_context,
    test_search_recent,
    test_link_trust,
    test_application_video_request,
    test_account_video_session,
    test_hevc,
    test_video_frame_bounds,
    test_account_video_commands,
    test_account_media_video_dispatch,
    test_local_draft_preview,
    test_aac_decoder,
    test_media_audio,
    test_browser_sign_in,
    test_browser_sign_in_receiver,
    test_browser_login_ui,
    test_gateway_self_status,
    test_application_plugin_settings_projection,
    test_plugin_preferences,
};
constexpr u32 kExtensionFixtureCount = sizeof(extension_fixtures) / sizeof(extension_fixtures[0]);
constexpr u32 kExtensionShardCount = 4;
constexpr u32 kUpdateFixtureCount = 5;
static_assert(kExtensionFixtureCount == 165);
static_assert(extension_fixtures[0] == test_update_manifest &&
              extension_fixtures[kUpdateFixtureCount - 1] == test_update_package &&
              extension_fixtures[kUpdateFixtureCount] == test_desktop_integration);
static_assert((kExtensionFixtureCount - kUpdateFixtureCount) % kExtensionShardCount == 0);

constexpr u32 fixture_shard(u32 index) noexcept {
    return index < kUpdateFixtureCount ? 0 : (index - kUpdateFixtureCount) % kExtensionShardCount;
}
constexpr u32 shard_fixture_count(u32 shard) noexcept {
    u32 count = 0;
    for (u32 index = 0; index < kExtensionFixtureCount; ++index)
        count += fixture_shard(index) == shard;
    return count;
}
static_assert(shard_fixture_count(0) == 45 && shard_fixture_count(1) == 40 &&
              shard_fixture_count(2) == 40 && shard_fixture_count(3) == 40);
static_assert(shard_fixture_count(0) + shard_fixture_count(1) + shard_fixture_count(2) +
                  shard_fixture_count(3) == kExtensionFixtureCount);

bool read_shard(const wchar_t* p, u32* shard, u32* count) noexcept {
    bool quoted = false;
    while (*p) {
        if (*p == L'"') quoted = !quoted;
        else if (!quoted && (*p == L' ' || *p == L'\t')) break;
        ++p;
    }
    if (quoted) return false;
    const auto spaces = [&]() noexcept { while (*p == L' ' || *p == L'\t') ++p; };
    spaces();
    *shard = 0;
    *count = 1;
    if (!*p) return true; // Offline full-driver use remains available.
    constexpr wchar_t option[] = L"--fixture-shard";
    for (u32 i = 0; i < sizeof(option) / sizeof(option[0]) - 1; ++i)
        if (*p++ != option[i]) return false;
    if (*p != L' ' && *p != L'\t') return false;
    spaces();
    const auto number = [&](u32* value) noexcept {
        if (*p < L'0' || *p > L'9') return false;
        *value = 0;
        do {
            *value = *value * 10 + static_cast<u32>(*p++ - L'0');
            if (*value > kExtensionShardCount) return false;
        } while (*p >= L'0' && *p <= L'9');
        return true;
    };
    if (!number(shard) || (*p != L' ' && *p != L'\t')) return false;
    spaces();
    if (!number(count)) return false;
    spaces();
    return !*p && *count == kExtensionShardCount && *shard < *count;
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_native_extensions_entry() noexcept {
    using namespace maicord;
    u32 shard = 0, count = 1;
    check(read_shard(GetCommandLineW(), &shard, &count),
          "native extensions: valid fixture shard command");
    if (failures) ExitProcess(1);
    u32 visited = 0, executed = 0;
    for (const auto fixture : extension_fixtures) {
        const u32 index = visited++;
        if (count != 1 && fixture_shard(index) != shard) continue;
        fixture(check);
        ++executed;
    }
    const u32 expected = count == 1 ? kExtensionFixtureCount : shard_fixture_count(shard);
    check(visited == kExtensionFixtureCount && executed == expected,
          "native extensions: every fixture belongs to exactly one complete shard");
    test_write_text(failures ? "FAIL native extensions shard=" : "PASS native extensions shard=");
    test_write_u64(shard);
    test_write_text("/");
    test_write_u64(count);
    test_write_text(" fixtures=");
    test_write_u64(executed);
    test_write_text("/");
    test_write_u64(kExtensionFixtureCount);
    test_write_line("");
    ExitProcess(failures ? 1 : 0);
}
