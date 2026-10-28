#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
// One list serves the prototypes and the table, in execution order: sharding indexes it.
#define MAICORD_NATIVE_EXTENSION_FIXTURES(X) \
    X(test_update_manifest) \
    X(test_update_artifact) \
    X(test_update_acceptance) \
    X(test_update_prepare) \
    X(test_update_package) \
    X(test_desktop_integration) \
    X(test_regional_flag_media) \
    X(test_flag_surfaces) \
    X(test_composer_source_spans) \
    X(test_reaction_sections) \
    X(test_message_reaction_controller) \
    X(test_gateway_message_poll) \
    X(test_message_poll_store) \
    X(test_message_poll_actions) \
    X(test_poll_controller) \
    X(test_poll_controls) \
    X(test_message_poll_presentation) \
    X(test_application_login_geometry) \
    X(test_tsf_end_edit) \
    X(test_member_list_sparse_model) \
    X(test_member_list_sparse_wire) \
    X(test_member_list_visible_users) \
    X(test_member_user_perf) \
    X(test_member_paging_pump) \
    X(test_thread_members) \
    X(test_thread_authority_model) \
    X(test_thread_header) \
    X(test_application_thread_header_activation) \
    X(test_thread_archive) \
    X(test_thread_storage) \
    X(test_read_state_model) \
    X(test_read_state_wire) \
    X(test_message_ack) \
    X(test_bulk_message_ack) \
    X(test_read_scope) \
    X(test_bulk_read_ack_controller) \
    X(test_read_ack_controller) \
    X(test_channel_context_policy) \
    X(test_guild_context_policy) \
    X(test_user_context_menu) \
    X(test_message_context_extras) \
    X(test_user_note_store) \
    X(test_active_now) \
    X(test_friends_active_now) \
    X(test_unread_context) \
    X(test_message_unread_context_menu) \
    X(test_read_state_pump) \
    X(test_read_ack_visibility) \
    X(test_mention_badge_text) \
    X(test_gateway_ready_projection) \
    X(test_text_emitter) \
    X(test_widget_builder) \
    X(thread_member_viewport_fixture) \
    X(test_thread_member_pump) \
    X(test_thread_subscriptions) \
    X(test_role_lookup_performance) \
    X(test_role_order) \
    X(test_guild_role_replacement) \
    X(test_permission_prefix) \
    X(test_guild_channel_validation) \
    X(name_admission_fixture) \
    X(member_viewport_fixture) \
    X(member_avatar_fixture) \
    X(member_media_fixture) \
    X(test_ui_page_registry) \
    X(test_plugin_host) \
    X(test_message_context_controller_extensions) \
    X(test_settings_scene) \
    X(test_profile_editor_layout) \
    X(test_settings_choice) \
    X(test_password_recovery) \
    X(test_password_reset_ui) \
    X(test_phone_recovery) \
    X(test_password_phone_ui) \
    X(test_friends_scene) \
    X(test_friends_media) \
    X(test_text_form_confirmation) \
    X(test_friend_search) \
    X(test_relationship_sync) \
    X(test_client_preferences) \
    X(test_visible_author_hydration) \
    X(test_scroll_host_preferences) \
    X(test_menu_rail_smooth_scroll) \
    X(test_background_theme) \
    X(test_background_effect_policy) \
    X(test_background_color_math) \
    X(test_theme_editor) \
    X(test_theme_styles) \
    X(test_theme_style_projection) \
    X(test_client_theme_storage) \
    X(test_client_theme_document) \
    X(test_theme_tokens) \
    X(test_surface_elevation) \
    X(test_login_continuations) \
    X(test_menu_row) \
    X(test_icon_pack) \
    X(test_client_preferences_writer) \
    X(test_plugin_module_loader) \
    X(test_plugin_catalog) \
    X(test_plugin_bundle_install) \
    X(test_video_controls) \
    X(test_audio_controls) \
    X(test_ui_cursor) \
    X(test_audio_presentation) \
    X(test_ogg_opus) \
    X(test_opus_decoder) \
    X(test_wave_pcm) \
    X(test_mp3_stream) \
    X(test_video_scene) \
    X(test_spinner) \
    X(test_ui_transition) \
    X(test_tooltip) \
    X(test_account_menu) \
    X(test_message_search) \
    X(test_message_rich_store_sparse) \
    X(test_message_pins) \
    X(test_forwarded_message_snapshots) \
    X(test_forwarded_message_pages) \
    X(test_search_scene) \
    X(test_pinned_messages_scene) \
    X(test_inbox_scene) \
    X(test_thread_browser_scene) \
    X(test_forum_post_scene) \
    X(test_quick_switcher_scene) \
    X(test_message_forward_scene) \
    X(test_download_scene) \
    X(test_http_response_stream) \
    X(test_attachment_download) \
    X(test_https_file_download) \
    X(test_staged_file) \
    X(test_media_picker) \
    X(test_channel_overview) \
    X(test_channel_content_policy) \
    X(test_channel_overview_controller) \
    X(test_pinned_message_controller) \
    X(test_quick_switcher_controller) \
    X(test_message_forward_controller) \
    X(test_message_forward_source_flags) \
    X(test_bounded_edit) \
    X(test_gif_favorites) \
    X(test_emoji_preferences) \
    X(test_thread_starter) \
    X(test_media_catalog) \
    X(test_message_create_stickers) \
    X(test_poll_draft) \
    X(test_message_composer_stickers) \
    X(test_search_query) \
    X(test_search_filter_context) \
    X(test_search_recent) \
    X(test_link_trust) \
    X(test_application_video_request) \
    X(test_account_video_session) \
    X(test_hevc) \
    X(test_video_frame_bounds) \
    X(test_account_video_commands) \
    X(test_account_media_video_dispatch) \
    X(test_local_draft_preview) \
    X(test_aac_decoder) \
    X(test_media_audio) \
    X(test_browser_sign_in) \
    X(test_browser_sign_in_receiver) \
    X(test_browser_login_ui) \
    X(test_gateway_self_status) \
    X(test_application_plugin_settings_projection) \
    X(test_plugin_preferences)
#define MAICORD_DECLARE_FIXTURE(name) void name(void (*check)(bool, const char*) noexcept) noexcept;
MAICORD_NATIVE_EXTENSION_FIXTURES(MAICORD_DECLARE_FIXTURE)
#undef MAICORD_DECLARE_FIXTURE
void test_channel_name_preflight(void (*check)(bool, const char*) noexcept) noexcept;
void test_chat_state_backing(void (*check)(bool, const char*) noexcept) noexcept;
u32 test_thread_member_viewport() noexcept;
u32 test_member_viewport_ranges_and_sparse_scene() noexcept;
u32 test_member_visible_avatar_requests() noexcept;
u32 test_member_media_cache_scroll() noexcept;
void thread_member_viewport_fixture(void (*verify)(bool, const char*) noexcept) noexcept {
    verify(test_thread_member_viewport() == 0, "thread member viewport and scene regressions");
}
// Name admission and the demand backing it relies on share a fixture slot.
void name_admission_fixture(void (*verify)(bool, const char*) noexcept) noexcept {
    test_channel_name_preflight(verify);
    test_chat_state_backing(verify);
}
void member_viewport_fixture(void (*verify)(bool, const char*) noexcept) noexcept {
    verify(test_member_viewport_ranges_and_sparse_scene() == 0, "member viewport planner and sparse scene regressions");
}
void member_avatar_fixture(void (*verify)(bool, const char*) noexcept) noexcept {
    verify(test_member_visible_avatar_requests() == 0, "member list: only on-screen rows request shared avatars; guild avatars keep their own identity");
}
void member_media_fixture(void (*verify)(bool, const char*) noexcept) noexcept {
    verify(test_member_media_cache_scroll() == 0, "member list media resolve once per user: unchanged frames 0, scroll only entering rows");
}
namespace {
u32 failures = 0;
void check(bool success, const char* name) noexcept {
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
#define MAICORD_FIXTURE_ENTRY(name) name,
    MAICORD_NATIVE_EXTENSION_FIXTURES(MAICORD_FIXTURE_ENTRY)
#undef MAICORD_FIXTURE_ENTRY
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
