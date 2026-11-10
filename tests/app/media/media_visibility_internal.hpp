#pragma once

#include "../../../src/app/media/pump.hpp"
#include "../../../src/app/chat/threads/forum_form.hpp"
#include "maicord/app/media/account_media.hpp"

namespace maicord::app_detail {

extern ForumPostScene* fixture_forum_scene;

extern ApplicationForumPostTicket fixture_forum_ticket;

extern bool fixture_forum_current, fixture_local_current;

extern bool fixture_local_exact;

extern u32 fixture_local_admissions, fixture_local_rejections, fixture_local_uploads;

extern MediaAssetKey fixture_local_key;

extern ApplicationSearchSourceView fixture_search_source;

extern bool fixture_search_current;

extern u32 fixture_search_dimension_publications;

extern MediaAssetKey fixture_catalog_owner_key;

extern bool fixture_catalog_owner_retained;

} // namespace maicord::app_detail

namespace maicord {
namespace media_visibility_detail {

using namespace app_detail;

extern u32 attachment_requests;

extern u32 identity_requests, catalog_requests;

extern MediaAssetKey last_identity_key;

extern u64 last_attachment_message;

extern u32 last_attachment_revision;

extern MediaAssetKey last_attachment_key;

extern bool observe_picker_requests;

extern bool picker_busy_flag_first;

extern u32 picker_emoji_requests;

extern MediaAssetKey picker_requested_keys[8];

void check(bool accepted, const char* label) noexcept;

// The pump reads the surface mask as its pass began: a check after the fixture changed the scenes starts a new pass.
inline ApplicationMediaPump& repass(ApplicationMediaPump& pump) noexcept {
    return pump.pass(pump.now_ms, pump.now_seconds, pump.now_us, pump.repaint);
}

AccountMediaRequestResult WINAPI request_static(void*, MediaAssetKey key, u64, u64) noexcept;

extern MediaAssetKey previous_identity_key;

extern bool identity_requests_queue;

AccountMediaRequestResult WINAPI request_avatar(void*, const AccountMediaGuildIconRequest* request, u64,
                                                u64) noexcept;

AccountMediaRequestResult WINAPI request_attachment(void*, const AccountMediaAttachmentRequest* request,
                                                    u64, u64) noexcept;

AccountMediaRequestResult WINAPI advance_animation(void*, MediaAssetKey, u16, u32, u64) noexcept;

extern AccountMediaFrame fixture_mailbox[5];

extern u32 fixture_mailbox_count, fixture_mailbox_taken;

bool WINAPI take_frame(void*, AccountMediaFrame* frame) noexcept;

struct VisibilityFixture {
    GuildRecord guilds[1]{};
    ChannelRecord channels[2]{};
    u16 guild_order[1]{}, channel_order[2]{};
    RoleRecord roles[1]{};
    PermissionOverwriteRecord overwrites[1]{};
    u8 channel_names[128]{};
    MessageRecord records[3]{};
    u32 order[3]{};
    u8 text[64]{};
    MessageRichRecord rich_records[3]{};
    u16 rich_order[3]{};
    u8 rich_text[2048]{};
    u32 heights[3]{};
    u64 fenwick[3]{};
    MessageSceneRowWidget rows[3]{};
    UiWidget widgets[3 * kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[3 * kMessageSceneWidgetsPerRow + 1]{};
    UserRecord users[1]{};
    u32 user_order[1]{};
    u8 user_names[32]{};
    MediaAtlasEntry atlas_entries[8]{};
    u32 atlas_buckets[16]{};
    GlyphAtlasEntry allocations[8]{};
    GlyphAtlasRow atlas_rows[4]{};
    MediaAtlas atlas{};
    MessageMediaVisibleRow visible_rows[3]{};
    MessageMediaAssetMark marks[16]{};
    AnimationTimelineEntry animations[4]{};
    AnimationVisualState visuals[4]{};
    u32 animation_buckets[8]{};
};

void run_animation_owner_test(
    VisibilityFixture& fixture, AccountUiRuntime& ui, ApplicationMediaPump& pump, MediaAssetKey key,
    AccountMediaFrameKind frame_kind = AccountMediaFrameKind::gif_indices) noexcept;

bool seed_scene(VisibilityFixture& fixture, AccountUiRuntime& ui) noexcept;

MediaAssetKey attachment_key(const VisibilityFixture& fixture, const AccountUiRuntime& ui,
                             u32 index) noexcept;

void run_identity_residency_test(VisibilityFixture& fixture, AccountUiRuntime& ui,
                                 ApplicationMediaPump& pump) noexcept;

void run_default_avatar_timeline_test(AccountUiRuntime& ui, ApplicationMediaPump& pump) noexcept;

void run_picker_emoji_visibility_test(ApplicationMediaPump& pump) noexcept;

void run_catalog_residency_test(VisibilityFixture& fixture, ApplicationMediaPump& pump) noexcept;

void run_search_avatar_test(ApplicationMediaPump& pump) noexcept;

void run_settings_avatar_test(ApplicationMediaPump& pump) noexcept;

void run_profile_editor_preview_test(ApplicationMediaPump& pump) noexcept;

void run_profile_activity_art_test(ApplicationMediaPump& pump) noexcept;

void run_quick_switcher_avatar_test(ApplicationMediaPump& pump) noexcept;

void run_visibility_test() noexcept;

void run_query_diagnostics_test() noexcept;

} // namespace media_visibility_detail

using namespace media_visibility_detail;
} // namespace maicord
