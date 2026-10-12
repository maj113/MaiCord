#include "../src/app/application_media_pump.hpp"
#include "../src/app/application_forum_form.hpp"
#include "../src/app/application_media_picker.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/model/typing_store.hpp"
#include "maicord/media/presentation_policy.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/ui/media_picker.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/search_avatar_media.hpp"
#include "maicord/render/inline_media.hpp"
#include "maicord/ui/thread_browser_scene.hpp"
#include "../src/app/application_search.hpp"
#include "../src/app/application_image_viewer.hpp"
#include "../src/app/application_profile_editor.hpp"
#include "maicord/app/local_draft_preview.hpp"
#include "maicord/model/user_profile.hpp"
#include "maicord/ui/profile_scene.hpp"
#include "../src/app/application_channel_authority.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/quick_switcher_scene.hpp"
#include "maicord/ui/message_forward_scene.hpp"
#include "maicord/render/renderer_presentation.hpp"

namespace maicord::app_detail {
ForumPostScene* fixture_forum_scene;
ApplicationForumPostTicket fixture_forum_ticket{10, 70, 3};
bool fixture_forum_current, fixture_local_current;
bool fixture_local_exact;
u32 fixture_local_admissions, fixture_local_rejections, fixture_local_uploads;
MediaAssetKey fixture_local_key{};
ForumPostScene* application_forum_form_scene() noexcept { return fixture_forum_scene; }
ApplicationForumPostView application_forum_post_view() noexcept {
    ApplicationForumPostView value{}; value.ticket = fixture_forum_ticket; value.available = fixture_forum_current; return value;
}
bool application_forum_post_current(const ApplicationForumPostTicket& ticket) noexcept {
    return fixture_forum_current && ticket.serial == fixture_forum_ticket.serial &&
        ticket.parent_id == fixture_forum_ticket.parent_id && ticket.publication_revision == fixture_forum_ticket.publication_revision;
}
bool application_forum_preview_request(u64, u64) noexcept {
    ++fixture_local_admissions;
    return true;
}
bool application_forum_preview_frame_current(const AccountMediaFrame& frame) noexcept {
    return fixture_local_current && media_asset_key_equal(frame.key, fixture_local_key) &&
           (!fixture_local_exact || (frame.source_catalog_scope == 700 && frame.source_catalog_generation == 9 &&
                                     frame.source_message_id == frame.key.id && frame.source_message_revision == frame.key.variant));
}
void application_forum_preview_frame_uploaded(const AccountMediaFrame&) noexcept {
    ++fixture_local_uploads;
}
bool application_forum_preview_key_current(MediaAssetKey key) noexcept {
    return fixture_local_current && media_asset_key_equal(key, fixture_local_key);
}
void application_forum_preview_rejected(MediaAssetKey) noexcept {
    ++fixture_local_rejections;
}
// The composer strip owner shares the forum fixture's local-preview routing.
bool application_composer_preview_request(u64, u64) noexcept { return true; }
bool application_local_preview_key_current(MediaAssetKey key) noexcept {
    return application_forum_preview_key_current(key);
}
// The profile editor's preview card (a fixture while its test runs) and dialogs.
ProfileScene* fixture_editor_preview;
bool fixture_editor_frame_current;
ProfileScene* application_profile_editor_preview() noexcept {
    return fixture_editor_preview;
}
bool application_profile_editor_frame_current(MediaAssetKey) noexcept {
    return fixture_editor_frame_current;
}
const UiProfileDialogScene* application_profile_editor_dialog() noexcept {
    return nullptr;
}
bool application_local_preview_frame_current(const AccountMediaFrame& frame) noexcept {
    return application_forum_preview_frame_current(frame);
}
void application_local_preview_frame_uploaded(const AccountMediaFrame& frame) noexcept {
    application_forum_preview_frame_uploaded(frame);
}
void application_local_preview_rejected(MediaAssetKey key) noexcept { application_forum_preview_rejected(key); }
void application_local_preview_deferred(MediaAssetKey, u64) noexcept {}
ApplicationSearchSourceView fixture_search_source{};
bool fixture_search_current;
u32 fixture_search_dimension_publications;
bool application_search_source_ticket(u32 index, ApplicationSearchSourceTicket* ticket) noexcept {
    if (!fixture_search_current || !ticket || index != fixture_search_source.ticket.index)
        return false;
    *ticket = fixture_search_source.ticket;
    return true;
}
bool application_search_source_resolve(const ApplicationSearchSourceTicket& ticket, bool snapshot,
                                       ApplicationSearchSourceView* source) noexcept {
    if (!fixture_search_current || !source ||
        ticket.message_id != fixture_search_source.ticket.message_id ||
        ticket.publication_revision != fixture_search_source.ticket.publication_revision ||
        ticket.index != fixture_search_source.ticket.index)
        return false;
    *source = fixture_search_source;
    source->snapshot = snapshot;
    source->rich = snapshot ? message_rich_store_snapshot(*source->rich_store, ticket.message_id)
                            : message_rich_store_find(*source->rich_store, ticket.message_id);
    return !snapshot || source->rich;
}
bool application_search_source_resolve(u64 id, u32 publication, bool snapshot,
                                       ApplicationSearchSourceView* source) noexcept {
    auto ticket = fixture_search_source.ticket;
    ticket.message_id = id;
    ticket.publication_revision = publication;
    return application_search_source_resolve(ticket, snapshot, source);
}
void application_search_media_dimensions_changed() noexcept {
    ++fixture_search_dimension_publications;
}
bool application_image_viewer_source(ApplicationSearchSourceView*) noexcept {
    return false;
}
u32 application_image_viewer_request_keys(MediaAssetKey* keys, u32 capacity) noexcept {
    if (!keys || !capacity || !g_app.image_viewer.scene.active || !media_asset_key_valid(g_app.image_viewer.key))
        return 0;
    keys[0] = g_app.image_viewer.key;
    return 1;
}
bool application_image_viewer_key_current(MediaAssetKey key) noexcept {
    return g_app.image_viewer.scene.active && media_asset_key_equal(key, g_app.image_viewer.key);
}
// The fixture does not select native video or submit a frame to the GPU.
bool application_video_ordinary(MediaAssetKey) noexcept {
    return false;
}
MediaAssetKey fixture_catalog_owner_key{};
bool fixture_catalog_owner_retained;
bool fixture_catalog_visible;
bool application_media_picker_preview_current(MediaAssetKey key, u64 scope, u32 revision) noexcept {
    return fixture_catalog_visible && scope == 600 && revision == 20 &&
           media_asset_key_equal(key, fixture_catalog_owner_key);
}
bool application_media_picker_preview_request(u32 cell,
                                              AccountMediaCatalogPreviewRequest* request) noexcept {
    if (!fixture_catalog_visible || cell != 0 || !request)
        return false;
    *request = {};
    request->key = fixture_catalog_owner_key;
    request->catalog_scope = 600;
    request->catalog_generation = 20;
    return true;
}
bool application_media_picker_preview_visible(MediaAssetKey key) noexcept {
    return fixture_catalog_visible && media_asset_key_equal(key, fixture_catalog_owner_key);
}
bool application_media_picker_preview_owner_current(MediaAssetKey key, u64 scope, u32 revision) noexcept {
    return fixture_catalog_owner_retained && scope == 600 && revision == 20 &&
           media_asset_key_equal(key, fixture_catalog_owner_key);
}
bool application_media_picker_preview_rejected(MediaAssetKey key, u64 scope, u32 revision) noexcept {
    if (!application_media_picker_preview_owner_current(key, scope, revision))
        return false;
    fixture_catalog_owner_retained = fixture_catalog_visible = false;
    return true;
}
} // namespace maicord::app_detail

namespace maicord {
namespace {
using namespace app_detail;

u32 failures;
u32 attachment_requests;
u32 identity_requests, catalog_requests;
MediaAssetKey last_identity_key{};
u64 last_attachment_message;
u32 last_attachment_revision;
MediaAssetKey last_attachment_key{};
bool observe_picker_requests;
bool picker_busy_flag_first;
u32 picker_emoji_requests;
MediaAssetKey picker_requested_keys[8];

void check(bool accepted, const char* label) noexcept {
    if (accepted)
        return;
    ++failures;
    u32 bytes = 0;
    while (label[bytes])
        ++bytes;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), label, bytes, &written, nullptr);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), "\r\n", 2, &written, nullptr);
}

AccountMediaRequestResult busy_result() noexcept {
    AccountMediaRequestResult result{};
    result.state = AccountMediaRequestState::busy;
    return result;
}

AccountMediaRequestResult WINAPI request_static(void*, MediaAssetKey key, u64, u64) noexcept {
    if (observe_picker_requests) {
        if (picker_emoji_requests < 8)
            picker_requested_keys[picker_emoji_requests++] = key;
        AccountMediaRequestResult result{};
        result.state = picker_busy_flag_first && key.kind == MediaAssetKind::regional_flag
                           ? AccountMediaRequestState::busy
                           : AccountMediaRequestState::queued;
        return result;
    }
    return busy_result();
}

u32 last_identity_size;
MediaAssetKey previous_identity_key{};
bool identity_requests_queue;
AccountMediaRequestResult WINAPI request_avatar(void*, const AccountMediaGuildIconRequest* request, u64,
                                                u64) noexcept {
    ++identity_requests;
    previous_identity_key = last_identity_key;
    last_identity_key = request->key;
    last_identity_size = request->reserved[0];
    if (identity_requests_queue) {
        AccountMediaRequestResult result{};
        result.state = AccountMediaRequestState::queued;
        return result;
    }
    return busy_result();
}
AccountMediaRequestResult WINAPI request_catalog(void*, const AccountMediaCatalogPreviewRequest*, u64,
                                                 u64) noexcept {
    ++catalog_requests;
    return busy_result();
}

AccountMediaRequestResult WINAPI request_attachment(void*, const AccountMediaAttachmentRequest* request,
                                                    u64, u64) noexcept {
    ++attachment_requests;
    last_attachment_message = request->source_message_id;
    last_attachment_revision = request->source_message_revision;
    last_attachment_key = request->key;
    return busy_result();
}

AccountMediaRequestResult WINAPI advance_animation(void*, MediaAssetKey, u16, u32, u64) noexcept {
    return busy_result();
}

AccountMediaFrame fixture_mailbox[5]{};
u32 fixture_mailbox_count, fixture_mailbox_taken;
bool WINAPI take_frame(void*, AccountMediaFrame* frame) noexcept {
    if (fixture_mailbox_taken >= fixture_mailbox_count) return false;
    *frame = fixture_mailbox[fixture_mailbox_taken++]; return true;
}

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
    AccountMediaFrameKind frame_kind = AccountMediaFrameKind::gif_indices) noexcept {
    animation_timeline_reset(&g_app.animations.timeline);
    g_app.animations.visuals = fixture.visuals;
    const auto published =
        animation_timeline_publish(&g_app.animations.timeline, {key, 1000, 1, 3, 0, true}, 100);
    AnimationFrameAdvance advance{};
    const auto collected = animation_timeline_collect_due(&g_app.animations.timeline, 1100, {&advance, 1});
    if (!published.ok() || !collected.ok() || collected.advances != 1) {
        check(false, "media owner: real pending timeline token initializes");
        return;
    }
    auto& visual = fixture.visuals[published.slot];
    visual.key = key;
    visual.generation = 1;
    visual.cache_generation = 11;
    visual.kind = frame_kind;
    visual.occupied = true;
    visual.source_scope = fixture.records[1].id;
    visual.source_revision = fixture.records[1].revision;
    AccountMediaFrame frame{};
    frame.key = key;
    frame.cache_generation = 11;
    frame.frame_index = advance.frame_index;
    frame.kind = frame_kind;
    check(!pump.message_image_frame_is_current(frame) &&
              pump.animation_frame_is_current(advance, frame, visual),
          "media owner: zero-authority cold frame requires exact retained owner and pending token");
    auto changed = frame;
    changed.cache_generation = 12;
    bool rejected = !pump.animation_frame_is_current(advance, changed, visual);
    changed = frame;
    ++changed.frame_index;
    rejected = rejected && !pump.animation_frame_is_current(advance, changed, visual);
    changed = frame;
    ++changed.key.id;
    rejected = rejected && !pump.animation_frame_is_current(advance, changed, visual);
    changed = frame;
    changed.content_identity = 1;
    rejected = rejected && !pump.animation_frame_is_current(advance, changed, visual);
    changed = frame;
    changed.source_message_id = visual.source_scope;
    rejected = rejected && !pump.animation_frame_is_current(advance, changed, visual);
    auto wrong_token = advance;
    ++wrong_token.scheduled_at_us;
    rejected = rejected && !pump.animation_frame_is_current(wrong_token, frame, visual);
    wrong_token = advance;
    ++wrong_token.play_index;
    rejected = rejected && !pump.animation_frame_is_current(wrong_token, frame, visual);
    check(rejected,
          "media owner: cache, frame, key, content, partial authority and pending token stay fenced");
    ui.active_channel_id = 8;
    rejected = !pump.animation_frame_is_current(advance, frame, visual);
    ui.active_channel_id = 7;
    g_app.login.visible = true;
    rejected = rejected && !pump.animation_frame_is_current(advance, frame, visual);
    g_app.login.visible = false;
    const u8 attachment_count = fixture.rich_records[1].attachment_count;
    fixture.rich_records[1].attachment_count = 0;
    rejected = rejected && !pump.animation_frame_is_current(advance, frame, visual);
    fixture.rich_records[1].attachment_count = attachment_count;
    check(rejected, "media owner: channel, opaque root and removed attachment reject cold completion");
    const bool edited = message_store_upsert(&g_app.message_view.store,
                                             {fixture.records[1].id, 7, 9, 0, 0, utf8_literal("edited"), 0})
                            .ok();
    // This is a text edit: the immutable attachment remains in the same rich
    // row, but its owner revision advances together with the message.
    fixture.rich_records[1].message_revision = fixture.records[1].revision;
    g_app.animations.pending_advances[0] = advance;
    g_app.animations.pending_advance_count = 1;
    animation_timeline_suspend(&g_app.animations.timeline, 1100);
    bool repaint = false;
    u32 uploads = 0;
    bool stop = false;
    const u32 before_requests = attachment_requests;
    pump.visibility_only = true;
    const bool observed = edited && pump.request_visible_attachment(
                                        fixture.records[1], fixture.rich_records[1].attachments[0], 64, 64,
                                        clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000,
                                        &uploads, &repaint, &stop);
    pump.visibility_only = false;
    const bool retired = observed && !animation_timeline_find(&g_app.animations.timeline, key).ok() &&
                         g_app.animations.pending_advance_count == 0 &&
                         attachment_requests == before_requests;
    const bool reacquired = retired && pump.request_visible_attachment(
                                           fixture.records[1], fixture.rich_records[1].attachments[0], 64,
                                           64, clock_monotonic_milliseconds(),
                                           clock_unix_milliseconds() / 1000, &uploads, &repaint, &stop);
    check(reacquired && attachment_requests == before_requests + 1 &&
              last_attachment_message == fixture.records[1].id &&
              last_attachment_revision == fixture.records[1].revision && !repaint && !uploads,
          frame_kind == AccountMediaFrameKind::gif_indices
              ? "media owner: edited revision retires old advance and resident GIF re-admits the real new "
                "owner"
              : "media owner: edited revision retires old advance and resident decoded PNG/WebP re-admits "
                "the real new owner");
    const auto new_owner =
        animation_timeline_publish(&g_app.animations.timeline, {key, 1000, 1, 3, 0, true}, 1100);
    visual = AnimationVisualState{};
    visual.key = key;
    visual.generation = 1;
    visual.cache_generation = 11;
    visual.kind = frame_kind;
    visual.occupied = true;
    visual.source_scope = fixture.records[1].id;
    visual.source_revision = fixture.records[1].revision;
    bool consumed = true;
    check(
        new_owner.ok() && pump.consume_pending_animation_frame(frame, &repaint, &consumed) && !consumed &&
            !repaint && animation_timeline_find(&g_app.animations.timeline, key).ok() && visual.occupied &&
            visual.source_revision == fixture.records[1].revision,
        "media owner: retired pending token cannot consume late frame or destroy same-cachegen new owner");
}

bool seed_scene(VisibilityFixture& fixture, AccountUiRuntime& ui) noexcept {
    ui.active_channel_id = 7;
    if (!chat_state_store_initialize(&ui.chat_state, {fixture.guilds, 1}, {fixture.guild_order, 1},
                                        {fixture.channels, 2}, {fixture.channel_order, 2},
                                        {fixture.channel_names, 128}) ||
        !chat_state_store_upsert_channel(&ui.chat_state,
                                            {7, 0, 0, utf8_literal("private conversation"), 0, 0, 1})
             .ok())
        return false;
    auto& view = g_app.message_view;
    if (!message_store_initialize(&view.store, {fixture.records, 3}, {fixture.order, 3},
                                  {fixture.text, 64}) ||
        !message_rich_store_initialize(&ui.message_rich_store, {fixture.rich_records, 3},
                                       {fixture.rich_order, 3}, {fixture.rich_text, 2048}) ||
        !user_store_initialize(&ui.user_store, {fixture.users, 1}, {fixture.user_order, 1},
                                       {fixture.user_names, 32}))
        return false;
    for (u32 index = 0; index < 3; ++index) {
        const u64 message_id = 100 + index;
        if (!message_store_upsert(&view.store, {message_id, 7, 9, 0, 0, utf8_literal(""), 0}).ok())
            return false;
        GatewayMessageAttachment attachment{};
        attachment.id = 200 + index;
        attachment.filename = index == 0 ? utf8_literal("blocked.png") : utf8_literal("loop.gif");
        attachment.content_type = index == 0 ? utf8_literal("image/png") : utf8_literal("image/gif");
        // Synthetic authority is valid and far in the future. No request is
        // sent: controlled account callbacks report saturated admission.
        attachment.url =
            index == 0   ? utf8_literal("https://cdn.discordapp.com/attachments/7/200/blocked.png"
                                        "?ex=ffffffff&is=1&hm="
                                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
            : index == 1 ? utf8_literal("https://cdn.discordapp.com/attachments/7/201/loop.gif"
                                        "?ex=ffffffff&is=1&hm="
                                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
                         : utf8_literal("https://cdn.discordapp.com/attachments/7/202/loop.gif"
                                        "?ex=ffffffff&is=1&hm="
                                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        attachment.width = attachment.height = 64;
        attachment.has_width = attachment.has_height = true;
        GatewayMessageEvent event{};
        event.id = message_id;
        event.fields = gateway_message_field_attachments;
        event.attachments = {&attachment, 1};
        if (!message_rich_store_apply(&ui.message_rich_store, message_id, fixture.records[index].revision,
                                      event)
                 .ok())
            return false;
    }
    if (!ui_variable_list_initialize(&view.list, {fixture.heights, 3}, {fixture.fenwick, 3}, 3,
                                     ui_extent_units(57)) ||
        !message_timeline_initialize(&view.timeline, &view.store, &view.list, ui_extent_units(57),
                                     ui_extent_units(1000), false) ||
        !message_timeline_bind_rich_store(&view.timeline, &ui.message_rich_store) ||
        !message_scene_initialize(&g_app.message_scene, &view.timeline, {fixture.rows, 3},
                                  {fixture.widgets, 3 * kMessageSceneWidgetsPerRow + 1},
                                  {fixture.nodes, 3 * kMessageSceneWidgetsPerRow + 1}) ||
        !message_scene_update(&g_app.message_scene, {0, 0, 640, 1000}).ok())
        return false;
    for (u32 index = 0; index < 3; ++index) {
        bool changed = false;
        if (!message_scene_set_text_metrics(&g_app.message_scene, index, 0, 0, false, nullptr, nullptr, {},
                                            &changed))
            return false;
    }
    if (!message_scene_update(&g_app.message_scene, {0, 0, 640, 1000}).ok())
        return false;
    const auto* third = message_scene_rich_item_layout_node(g_app.message_scene, fixture.rows[2],
                                                            MessageSceneRichItemKind::attachment, 0);
    if (!third || !ui_rect_has_area(third->rectangle))
        return false;
    // The third message's author may remain visible, while its attachment is
    // outside the viewport. Row visibility alone must not keep it playing.
    return message_scene_update(&g_app.message_scene, {0, 0, 640, third->rectangle.top}).ok();
}

MediaAssetKey attachment_key(const VisibilityFixture& fixture, const AccountUiRuntime& ui,
                             u32 index) noexcept {
    const auto* rich = message_rich_store_find(ui.message_rich_store, 100 + index);
    const auto* node = message_scene_rich_item_layout_node(g_app.message_scene, fixture.rows[index],
                                                           MessageSceneRichItemKind::attachment, 0);
    float width = 0, height = 0;
    if (!rich || !node ||
        !message_rich_attachment_preview_extent(ui.message_rich_store, rich->attachments[0],
                                                ui_rect_width(node->content_rectangle), &width, &height))
        return {};
    const auto proxy = media_presentation_request(64, 64, width, height, 96,
                                                  media_preview_extent_cap(MediaAttachmentFormat::gif));
    return media_asset_attachment(200 + index, MediaAttachmentFormat::gif, proxy.resolution);
}

void run_identity_residency_test(VisibilityFixture& fixture, AccountUiRuntime& ui,
                                 ApplicationMediaPump& pump) noexcept {
    constexpr auto hash = utf8_literal("a_0123456789abcdef0123456789abcdef");
    const auto avatar = media_asset_user_avatar(9, user_avatar_variant(hash), true);
    bool valid = user_store_upsert(&ui.user_store, 9, utf8_literal("Author")).ok() &&
                 user_store_set_avatar(&ui.user_store, 9, hash);
    bool grouped[3]{};
    for (u32 index = 0; index < g_app.message_scene.count; ++index) {
        grouped[index] = fixture.rows[index].grouped;
        fixture.rows[index].grouped = true;
    }
    u32 uploads = 0;
    bool repaint = false, stop = false;
    const u32 before_hidden = identity_requests;
    for (u32 pass = 0; pass < 64; ++pass)
        valid = pump.request_visible_user_avatar_value(9, hash, 1'000, 1'100, &uploads, &repaint, &stop) &&
                valid;
    check(valid && !pump.avatar_key_is_current(avatar) && identity_requests == before_hidden,
          "avatar admission: grouped authors without a visible avatar never queue discarded decodes");
    for (u32 index = 0; index < g_app.message_scene.count; ++index)
        fixture.rows[index].grouped = grouped[index];
    check(pump.avatar_key_is_current(avatar) &&
              pump.request_visible_user_avatar_value(9, hash, 1'000, 1'100, &uploads, &repaint, &stop) &&
              identity_requests == before_hidden + 1,
          "avatar admission: matching visible author still admits its normal cold request");
    check(media_asset_key_equal(last_identity_key, avatar) &&
              last_identity_size == cdn_image_size(kCdnIdentityDisplayDips, g_app.renderer.dpi) &&
              last_identity_size <= kCdnIdentityDefaultSize && (last_identity_size & (last_identity_size - 1)) == 0,
          "avatar admission: the CDN size matches the 40-DIP avatar extent at the renderer DPI");

    GuildRecord guild{};
    guild.id = 600;
    guild.occupied = true;
    guild.icon_variant = cdn_guild_icon_variant(hash);
    guild.icon_hash_bytes = static_cast<u8>(hash.bytes);
    CopyMemory(guild.icon_hash, hash.data, hash.bytes);
    const auto icon = media_asset_guild_icon(guild.id, guild.icon_variant, true);
    const auto avatar_resident = media_atlas_allocate(&fixture.atlas, avatar, 32, 32);
    const auto icon_resident = media_atlas_allocate(&fixture.atlas, icon, 32, 32);
    valid = valid && avatar_resident.ok() && icon_resident.ok() &&
            media_atlas_set_decoded_animation(&fixture.atlas, avatar_resident.handle, false) &&
            media_atlas_set_decoded_animation(&fixture.atlas, icon_resident.handle, false);
    const u32 before_static = identity_requests;
    for (u32 pass = 0; pass < 64; ++pass) {
        valid = pump.request_visible_user_avatar_value(9, hash, 1'000, 1'100, &uploads, &repaint, &stop,
                                                       true) &&
                valid;
        valid =
            pump.request_visible_guild_icon(guild, 1'000, 1'100, &uploads, &repaint, &stop, true) && valid;
    }
    check(valid && identity_requests == before_static,
          "identity residency: decoded static avatars and icons override animated hash hints during forced "
          "reacquisition");
    valid =
        valid && media_atlas_set_decoded_animation(&fixture.atlas, avatar_resident.handle, true) &&
        media_atlas_set_decoded_animation(&fixture.atlas, icon_resident.handle, true) &&
        pump.request_visible_user_avatar_value(9, hash, 1'000, 1'100, &uploads, &repaint, &stop, true) &&
        pump.request_visible_guild_icon(guild, 1'000, 1'100, &uploads, &repaint, &stop, true);
    check(valid && identity_requests == before_static + 2,
          "identity residency: decoded multiframe residents still admit missing-timeline reacquisition");
}

bool picker_emoji_source(void*, u32 index, UiMediaPickerItem* output) noexcept {
    if (!output || index >= 200)
        return false;
    *output = {};
    output->id = index + 1;
    output->label = utf8_literal("Original native emoji");
    output->kind = UiMediaPickerItemKind::emoji;
    output->enabled = true;
    output->width = output->height = 64;
    if (picker_busy_flag_first && index < 2)
        index = 1 - index;
    if (index == 0) {
        output->asset_id = 981;
        output->asset_flags = custom_emoji_animated;
    } else if (index == 1) {
        output->glyph = utf8_literal("🇸🇮");
        output->regional_flag = regional_flag_sequence(output->glyph);
    } else if (index == 199) {
        output->asset_id = 982;
    } else {
        output->glyph = utf8_literal("👍");
    }
    return true;
}
void run_picker_emoji_visibility_test(ApplicationMediaPump& pump) noexcept {
    UiMediaPickerScene scene{};
    UiMediaPickerContent content{};
    UiMediaPickerSource source{};
    content.edit.active = true;
    content.kind = UiMediaPickerKind::emoji;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.account_scope = source.account_scope = 600;
    content.query_generation = source.query_generation = 40;
    content.revision = source.revision = 1;
    source.count = 200;
    source.resolve = picker_emoji_source;
    const bool ready =
        ui_media_picker_scene_update(&scene, &source, &content, {0, 0, 1280, 720}, {312, 672, 1040, 720});
    check(ready && scene.cell_count > 3 && scene.cell_count <= kUiMediaPickerVisibleCells,
          "picker emoji visibility: real shared scene materializes bounded visible rows from 200 items");
    if (!ready) {
        ui_media_picker_scene_release(&scene);
        return;
    }
    const auto* previous = g_app.renderer.media_picker_scene;
    auto* previous_atlas = g_app.renderer.media_atlas;
    MediaAtlasEntry entries[4]{};
    u32 buckets[8]{};
    GlyphAtlasEntry allocations[4]{};
    GlyphAtlasRow atlas_rows[4]{};
    MediaAtlas atlas{};
    const bool atlas_ready = media_atlas_initialize(&atlas, 128, 128, 0, {entries, 4}, {buckets, 8},
                                                    {allocations, 4}, {atlas_rows, 4});
    check(atlas_ready, "picker emoji visibility: independent ordinary atlas initializes");
    if (!atlas_ready) {
        ui_media_picker_scene_release(&scene);
        return;
    }
    g_app.renderer.media_picker_scene = &scene;
    g_app.renderer.media_atlas = &atlas;
    observe_picker_requests = true;
    picker_emoji_requests = 0;
    message_media_controller_begin_asset_scan(&g_app.message_media);
    u32 uploads = 0;
    bool repaint = false;
    const auto animated = media_asset_custom_emoji(981, true);
    const auto flag = media_asset_regional_flag(regional_flag_sequence(utf8_literal("🇸🇮")));
    uploads = kMessageMediaUploadBudget;
    const u32 before_other_admission = attachment_requests + identity_requests + catalog_requests;
    check(!pump.visibility_only && pump.scan_visible_media(1'000, 1'100, 1'000, uploads, &repaint) &&
              !picker_emoji_requests && uploads == kMessageMediaUploadBudget &&
              attachment_requests + identity_requests + catalog_requests == before_other_admission,
          "picker emoji visibility: the shared scan stops admissions at the exhausted foreground budget");
    check(!pump.visibility_only && pump.request_picker_emoji(1'000, 1'100, 1'000, &uploads, &repaint) &&
              !picker_emoji_requests && uploads == kMessageMediaUploadBudget,
          "picker emoji visibility: a real exhausted upload budget admits no missing asset requests");
    uploads = 0;
    const float clipped_scroll = scene.projection->cells[0].rectangle.bottom - scene.body.top - 1;
    UiMediaPickerMediaSlot clipped{};
    check(ui_media_picker_scene_scroll(&scene, clipped_scroll) &&
              scene.projection->cells[0].source_index == 0 &&
              ui_media_picker_scene_media(scene, 0, &clipped) && !ui_rect_has_area(clipped.clip) &&
              pump.request_picker_emoji(1'000, 1'100, 1'000, &uploads, &repaint) &&
              !picker_emoji_requests && ui_media_picker_scene_scroll(&scene, -scene.scroll_dips),
          "picker emoji visibility: an exposed cell with entirely clipped inset art queues no image");
    const bool requested = pump.request_picker_emoji(1'000, 1'100, 1'000, &uploads, &repaint);
    check(requested && picker_emoji_requests == 2 &&
              media_asset_key_equal(picker_requested_keys[0], animated) &&
              media_asset_key_equal(picker_requested_keys[1], flag),
          "picker emoji visibility: only clipped custom emoji and flag use the ordinary media lane; "
          "Unicode and hidden rows do not fetch");
    const auto previous_inline = g_app.renderer.inline_media;
    const auto& cell = scene.projection->cells[0];
    RendererInlineFlag inline_flags[4]{};
    inline_flags[0] = {{&scene.widgets, cell.widget},
                       cell.rectangle,
                       scene.body,
                       static_cast<u16>(flag.id),
                       RendererInlineMediaLayer::gallery};
    inline_flags[1] = inline_flags[0];
    inline_flags[2] = inline_flags[0];
    inline_flags[2].clip = {};
    inline_flags[3] = inline_flags[0];
    inline_flags[3].owner.manager = reinterpret_cast<UiWidgetManager*>(static_cast<usize>(1));
    g_app.renderer.inline_media = {inline_flags, 4, 0};
    picker_emoji_requests = 0;
    uploads = 0;
    check(pump.request_inline_flags(1'000, 1'000, &uploads, &repaint) && picker_emoji_requests == 1 &&
              media_asset_key_equal(picker_requested_keys[0], flag) &&
              !renderer_inline_flag_current(g_app.renderer, inline_flags[2]) &&
              !renderer_inline_flag_current(g_app.renderer, inline_flags[3]),
          "inline flags: exact live clipped placements dedupe one static asset; unrelated pointers "
          "are rejected before dereference");
    ++inline_flags[0].owner.handle.generation;
    inline_flags[1].layer = RendererInlineMediaLayer::base;
    check(pump.request_inline_flags(1'000, 1'000, &uploads, &repaint) && picker_emoji_requests == 1,
          "inline flags: stale widget generation and wrong surface layer admit no requests");
    inline_flags[0].owner.handle = cell.widget;
    inline_flags[1].layer = RendererInlineMediaLayer::gallery;
    uploads = kMessageMediaUploadBudget;
    check(pump.request_inline_flags(1'000, 1'000, &uploads, &repaint) && picker_emoji_requests == 1,
          "inline flags: exhausted foreground budget cannot admit another label asset");
    uploads = 0;
    pump.visibility_only = true;
    check(pump.request_inline_flags(1'000, 1'000, &uploads, &repaint) && picker_emoji_requests == 1,
          "inline flags: visibility-only pass never sends static image requests");
    pump.visibility_only = false;
    ++source.query_generation;
    check(pump.request_inline_flags(1'000, 1'000, &uploads, &repaint) && picker_emoji_requests == 1,
          "inline flags: replaced borrowed source rejects old label placements before admission");
    --source.query_generation;
    g_app.renderer.inline_media = previous_inline;
    picker_emoji_requests = 0;
    picker_busy_flag_first = true;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, scene.viewport, scene.panel) &&
              pump.request_picker_emoji(1'000, 1'100, 1'000, &uploads, &repaint) &&
              picker_emoji_requests == 2 && media_asset_key_equal(picker_requested_keys[0], flag) &&
              media_asset_key_equal(picker_requested_keys[1], animated),
          "picker emoji visibility: a busy still-image lane cannot hide a later animation admission");
    picker_busy_flag_first = false;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, {0, 0, 1280, 720}, {312, 672, 1040, 720}),
          "picker emoji visibility: source replacement restores original visible order");
    const auto custom_resident = media_atlas_allocate(&atlas, animated, 32, 32);
    const auto flag_resident = media_atlas_allocate(&atlas, flag, 32, 32);
    bool retained = custom_resident.ok() && flag_resident.ok() &&
                    media_atlas_set_decoded_animation(&atlas, custom_resident.handle, false);
    const u32 resident_requests = picker_emoji_requests;
    for (u32 i = 0; i < 64; ++i)
        retained &= pump.request_picker_emoji(1'000, 1'100, 1'000, &uploads, &repaint);
    check(retained && picker_emoji_requests == resident_requests,
          "picker emoji visibility: decoded still residents override animation hints without repeated "
          "fetches");
    check(media_atlas_set_decoded_animation(&atlas, custom_resident.handle, true) &&
              pump.request_picker_emoji(1'000, 1'100, 1'000, &uploads, &repaint) &&
              picker_emoji_requests == resident_requests + 1,
          "picker emoji visibility: decoder-confirmed animation admits missing-timeline reacquisition");
    const auto owned = animation_timeline_publish(&g_app.animations.timeline,
                                                  {animated, 1'000'000, 67, 3, 0, false}, 1'000);
    const u32 owned_requests = picker_emoji_requests;
    bool observed = owned.ok();
    for (u32 i = 0; i < 64; ++i)
        observed &= pump.request_picker_emoji(1'000, 1'100, 1'000, &uploads, &repaint);
    check(observed && picker_emoji_requests == owned_requests &&
              (g_app.animations.timeline.entries[owned.slot].flags & animation_timeline_entry_visible),
          "picker emoji visibility: owned animation stays visible through repeated pumps without transport "
          "churn");
    const u32 before_observation = picker_emoji_requests;
    pump.visibility_only = true;
    uploads = kMessageMediaUploadBudget;
    check(pump.request_picker_emoji(1'000, 1'100, 1'000, &uploads, nullptr) &&
              picker_emoji_requests == before_observation && uploads == kMessageMediaUploadBudget,
          "picker emoji visibility: exhausted-budget visibility pass performs no fetches");
    pump.visibility_only = false;
    ++source.revision;
    check(pump.request_picker_emoji(1'000, 1'100, 1'000, &uploads, &repaint) &&
              picker_emoji_requests == before_observation,
          "picker emoji visibility: stale shared source cannot submit visible assets from the previous "
          "projection");
    observe_picker_requests = false;
    g_app.renderer.media_picker_scene = previous;
    g_app.renderer.media_atlas = previous_atlas;
    animation_timeline_reset(&g_app.animations.timeline);
    ui_media_picker_scene_release(&scene);
}
void run_catalog_residency_test(VisibilityFixture& fixture, ApplicationMediaPump& pump) noexcept {
    UiMediaPickerScene scene{};
    UiMediaPickerProjection projection{};
    UiMediaPickerContent content{};
    UiMediaPickerSource source{};
    content.account_scope = source.account_scope = scene.account_scope = 600;
    content.query_generation = source.query_generation = scene.query_generation = 20;
    content.revision = source.revision = scene.content_revision = scene.source_revision = 1;
    source.count = scene.cell_count = 1;
    scene.active = true;
    scene.source = &source;
    scene.content = &content;
    scene.projection = &projection;
    g_app.renderer.media_picker_scene = &scene;
    g_app.account.api.catalog_preview_request = request_catalog;
    fixture_catalog_owner_key = media_asset_catalog_preview(902, 20);
    fixture_catalog_owner_retained = fixture_catalog_visible = true;
    animation_timeline_reset(&g_app.animations.timeline);
    const auto resident = media_atlas_allocate(&fixture.atlas, fixture_catalog_owner_key, 32, 32);
    bool valid = resident.ok() && media_atlas_set_decoded_animation(&fixture.atlas, resident.handle, false);
    u32 uploads = 0;
    bool repaint = false;
    const u32 before_static = catalog_requests;
    for (u32 pass = 0; pass < 64; ++pass)
        valid = pump.request_catalog_previews(1'000, 1'100, 1'000, &uploads, &repaint) && valid;
    check(valid && (fixture_catalog_owner_key.flags & media_asset_flag_animated) &&
              catalog_requests == before_static,
          "catalog residency: decoder-proven still WebP never restarts from a provisional animation hint");
    valid = valid && media_atlas_set_decoded_animation(&fixture.atlas, resident.handle, true) &&
            pump.request_catalog_previews(1'000, 1'100, 1'000, &uploads, &repaint);
    check(valid && catalog_requests == before_static + 1,
          "catalog residency: decoded multiframe resident without a timeline admits cold reacquisition");
    const auto published = animation_timeline_publish(
        &g_app.animations.timeline, {fixture_catalog_owner_key, 1'000'000, 42, 3, 0, false}, 1'000);
    if (published.ok()) {
        auto& visual = fixture.visuals[published.slot];
        visual = {};
        visual.key = fixture_catalog_owner_key;
        visual.generation = 42;
        visual.cache_generation = 42;
        visual.source_scope = 600;
        visual.source_revision = 20;
        visual.kind = AccountMediaFrameKind::animated_webp_rgba8;
        visual.occupied = true;
    }
    const u32 before_owned = catalog_requests;
    for (u32 pass = 0; pass < 64; ++pass)
        valid = pump.request_catalog_previews(1'000, 1'100, 1'000, &uploads, &repaint) && valid;
    check(valid && published.ok() && catalog_requests == before_owned &&
              (fixture.animations[published.slot].flags & animation_timeline_entry_visible),
          "catalog residency: current owned animation remains visible without another decode request");
    auto wrong_key = fixture_catalog_owner_key;
    ++wrong_key.id;
    check(!application_media_picker_preview_rejected(wrong_key, 600, 20) &&
              !application_media_picker_preview_rejected(fixture_catalog_owner_key, 601, 20) &&
              !application_media_picker_preview_rejected(fixture_catalog_owner_key, 600, 21) &&
              fixture_catalog_owner_retained && fixture_catalog_visible,
          "catalog rejection: wrong key, account scope and generation cannot retire the current preview");
    AccountMediaFrame malformed{};
    malformed.key = fixture_catalog_owner_key;
    malformed.source_catalog_scope = 600;
    malformed.source_catalog_generation = 20;
    check(pump.upload_account_media_frame(malformed) ==
                  ApplicationMediaPump::MediaFrameUploadResult::deferred &&
              !fixture_catalog_owner_retained && !fixture_catalog_visible &&
              !application_media_picker_preview_rejected(fixture_catalog_owner_key, 600, 20) &&
              pump.request_catalog_previews(1'000, 1'100, 1'000, &uploads, &repaint) &&
              catalog_requests == before_owned,
          "catalog rejection: a malformed current upload withdraws preview admission once and cannot "
          "request another decode");
    g_app.renderer.media_picker_scene = nullptr;
    fixture_catalog_owner_retained = fixture_catalog_visible = false;
    animation_timeline_reset(&g_app.animations.timeline);
}

void run_search_avatar_test(ApplicationMediaPump& pump) noexcept {
    UiSearchScene scene{};
    struct Scope {
        bool current = true;
    } scope{};
    UiSearchSource source{&scope, 2, 1,
                          +[](void*, u32 i, UiSearchRow* row) noexcept {
                              if (i >= 2 || !row)
                                  return false;
                              *row = {70,
                                      700 + i,
                                      utf8_literal("Original search author"),
                                      utf8_literal("#native-search"),
                                      utf8_literal("Original result"),
                                      {},
                                      true,
                                      i == 0,
                                      24,
                                      770 + i,
                                      utf8_literal("a_0123456789abcdef0123456789abcdef")};
                              return true;
                          },
                          +[](void* opaque) noexcept { return static_cast<Scope*>(opaque)->current; }};
    UiSearchContent content{};
    content.edit.active = content.panel = true;
    content.phase = UiSearchPhase::ready;
    content.revision = 1;
    const auto* previous_search = g_app.renderer.search_scene;
    bool valid = ui_search_scene_update(&scene, &source, &content, {312, 0, 1200, 800}, {312, 0, 1200, 48});
    g_app.renderer.search_scene = &scene;
    SearchAvatarMedia media{};
    valid = valid && search_avatar_media_resolve(scene, 0, &media);
    check(valid && !user_store_find(g_app.account_ui->user_store, 770) &&
              pump.avatar_key_is_current(media.key),
          "search avatar: exact result bank owns a visible author absent from the conversation user cache");
    const u32 before = identity_requests;
    u32 uploads = 0;
    bool repaint = false, stop = false;
    if (valid)
        valid = pump.request_visible_user_avatar_value(media.key.id, media.hash, 1000, 1100, &uploads,
                                                       &repaint, &stop, true);
    check(valid && identity_requests == before + 1 && media_asset_key_equal(last_identity_key, media.key),
          "search avatar: existing native identity lane receives the result's exact hash variant");
    scope.current = false;
    if (valid)
        valid = pump.request_visible_user_avatar_value(media.key.id, media.hash, 1000, 1100, &uploads,
                                                       &repaint, &stop, true);
    check(valid && !pump.avatar_key_is_current(media.key) && identity_requests == before + 1,
          "search avatar: retired account scope rejects stale admission without draining unrelated work");
    scope.current = true;
    ++source.revision;
    check(!pump.avatar_key_is_current(media.key),
          "search avatar: replaced result page cannot authorize the prior borrowed projection");
    ++content.revision;
    content.filters = true;
    valid = ui_search_scene_update(&scene, &source, &content, {312, 0, 1200, 800}, {312, 0, 1200, 48});
    check(valid && !search_avatar_media_resolve(scene, 0, &media),
          "search avatar: opaque filter chooser prevents covered profile media admission");
    g_app.renderer.search_scene = previous_search;
    ui_search_scene_release(&scene);
}

// Opaque Settings rejects chat media, but its avatar rows (My account card)
// still request and admit the shared user-avatar key.
void run_settings_avatar_test(ApplicationMediaPump& pump) noexcept {
    const Utf8View hash = utf8_literal("0123456789abcdef0123456789abcdef");
    const auto key = media_asset_user_avatar(990, user_avatar_variant(hash), false);
    const auto other = media_asset_user_avatar(991, user_avatar_variant(hash), false);
    UiSettingsControl controls[2]{{1, utf8_literal("Ada Lovelace"), utf8_literal("ada"),
                                   UiSettingsControlKind::action, true, false, {}, 0, utf8_literal("Edit profile"),
                                   nullptr},
                                  {2, utf8_literal("Username"), utf8_literal("ada"), UiSettingsControlKind::action,
                                   true, false, {}, 0, utf8_literal("Edit"), nullptr}};
    UiSettingsControlPresentation presentation[2]{
        {UiSettingsVariant::standard, ui_settings_presentation_avatar, kUiSettingsNoDefault},
        {UiSettingsVariant::standard, 0, kUiSettingsNoDefault}};
    UiSettingsAvatarMedia avatars[2]{};
    avatars[0].key = key;
    avatars[0].hash_bytes = static_cast<u8>(hash.bytes);
    copy_bytes(avatars[0].hash, hash.data, hash.bytes);
    // The plain row carries a key too; only avatar-flagged rows may use it.
    avatars[1] = avatars[0];
    avatars[1].key = other;
    UiSettingsPageContent content{utf8_literal("My account"), {}, {controls, 2}, 1};
    content.presentation = presentation;
    content.avatar_media = avatars;
    UiPage page{};
    UiPageRegistry registry{};
    static UiSettingsScene scene{};
    bool valid = ui_page_registry_initialize(&registry, {&page, 1}) &&
                 ui_page_registry_register(&registry, {{1, 4}, utf8_literal("My account"), &content}).ok() &&
                 ui_settings_scene_update(&scene, &registry, {0, 0, 1280, 800});
    auto* previous = g_app.renderer.settings_scene;
    g_app.renderer.settings_scene = &scene;
    UiSettingsAvatarGeometry geometry{}, extra{};
    u32 cursor = 0;
    valid = valid && ui_settings_scene_next_avatar(scene, &cursor, &geometry) &&
            media_asset_key_equal(geometry.media->key, key) && ui_rect_has_area(geometry.clip) &&
            ui_rect_width(geometry.avatar) == ui_rect_height(geometry.avatar);
    check(valid && !ui_settings_scene_next_avatar(scene, &cursor, &extra),
          "settings avatar: only the avatar-flagged row with media exposes a square avatar slot");
    // Server Settings > Members: a row with an avatar image paints only a
    // plain loading placeholder exactly under its slot (the renderer hides it
    // once the image is drawn), never initials a transparent avatar would
    // show through; a row without an image keeps its initials circle.
    const UiRect slot = geometry.avatar;
    const auto placeholder_paint = [&](bool* placeholder, bool* initials) noexcept {
        static UiTextCommand commands[256];
        UiTextCommandStream text{};
        PaintStream paint{};
        const bool ok = ui_text_command_stream_initialize(&text, {commands, 256}) &&
                        ui_settings_scene_text(scene, kDefaultTheme, &text) && paint_stream_initialize(&paint) &&
                        paint_stream_reset(&paint, 1280, 800) && ui_settings_scene_paint(scene, kDefaultTheme, &paint);
        const Color grey = kDefaultTheme.avatar_placeholder;
        *placeholder = *initials = false;
        for (u32 i = 0; ok && i < paint.count; ++i) {
            const auto& q = paint.quads[i];
            *placeholder = *placeholder || (ui_rect_equal({q.left, q.top, q.right, q.bottom}, slot) &&
                                            q.border_width == 0 && q.fill.red == grey.red &&
                                            q.fill.green == grey.green && q.fill.blue == grey.blue &&
                                            q.fill.alpha == grey.alpha);
        }
        // The initials are the only text centred in the avatar circle (their
        // characters live in the paint call's own storage).
        for (u32 i = 0; ok && i < text.count; ++i)
            *initials = *initials || ui_rect_equal(commands[i].rectangle, slot);
        paint_stream_release(&paint);
        return ok;
    };
    bool placeholder = false, initials = false;
    check(placeholder_paint(&placeholder, &initials) && placeholder && !initials,
          "settings avatar: a row with an avatar image paints a hideable loading placeholder, no initials");
    check(pump.media_key_is_current(key) && !pump.media_key_is_current(other),
          "settings avatar: opaque Settings admits exactly its account-card avatar");
    const u32 before = identity_requests;
    u32 uploads = 0;
    bool repaint = false;
    check(pump.request_settings_avatars(1000, 1100, &uploads, &repaint) && identity_requests == before + 1 &&
              media_asset_key_equal(last_identity_key, key),
          "settings avatar: the account card requests the shared user-avatar key with its hash");
    avatars[0] = {};
    ++content.revision;
    valid = ui_settings_scene_update(&scene, &registry, {0, 0, 1280, 800});
    cursor = 0;
    check(valid && !ui_settings_scene_next_avatar(scene, &cursor, &extra) && !pump.media_key_is_current(key) &&
              pump.request_settings_avatars(1000, 1100, &uploads, &repaint) && identity_requests == before + 1,
          "settings avatar: a row without media keeps its initials and requests nothing");
    check(placeholder_paint(&placeholder, &initials) && !placeholder && initials,
          "settings avatar: a row without an avatar image keeps its initials instead of the loading placeholder");
    // Server Settings' identity row draws the guild icon (the rail's key and
    // CDN asset) over the server initials, animated hashes included.
    const Utf8View icon = utf8_literal("a_0123456789abcdef0123456789abcdef");
    const auto guild_key = media_asset_guild_icon(992, cdn_guild_icon_variant(icon), true);
    avatars[0] = {};
    avatars[0].key = guild_key;
    avatars[0].hash_bytes = static_cast<u8>(icon.bytes);
    copy_bytes(avatars[0].hash, icon.data, icon.bytes);
    ++content.revision;
    valid = ui_settings_scene_update(&scene, &registry, {0, 0, 1280, 800});
    cursor = 0;
    check(valid && ui_settings_scene_next_avatar(scene, &cursor, &geometry) &&
              media_asset_key_equal(geometry.media->key, guild_key) && pump.media_key_is_current(guild_key),
          "settings avatar: a server identity row admits its guild icon instead of initials");
    check(pump.request_settings_avatars(1000, 1100, &uploads, &repaint) && identity_requests == before + 2 &&
              media_asset_key_equal(last_identity_key, guild_key) && last_identity_size != 0,
          "settings avatar: the server identity row requests the shared guild icon key with its hash");
    // Audit log actors without an avatar hash show the default avatar,
    // fetched by its CDN path under the profile card's key.
    const auto default_path = utf8_literal("embed/avatars/3.png");
    const auto default_url = utf8_literal("https://cdn.discordapp.com/embed/avatars/3.png");
    auto default_key = media_asset_embed(hash64_bytes(kHash64Seed, default_url.data, default_url.bytes), 1, 0,
                                         MediaEmbedResource::image, MediaAttachmentFormat::png, false,
                                         MediaResolutionTier::px512);
    default_key.kind = MediaAssetKind::profile_image;
    avatars[0] = {};
    avatars[0].key = default_key;
    avatars[0].hash_bytes = static_cast<u8>(default_path.bytes);
    copy_bytes(avatars[0].hash, default_path.data, default_path.bytes);
    ++content.revision;
    valid = ui_settings_scene_update(&scene, &registry, {0, 0, 1280, 800});
    cursor = 0;
    const u32 before_default = attachment_requests;
    check(valid && ui_settings_scene_next_avatar(scene, &cursor, &geometry) &&
              media_asset_key_equal(geometry.media->key, default_key) && pump.media_key_is_current(default_key) &&
              pump.request_settings_avatars(1000, 1100, &uploads, &repaint) &&
              attachment_requests == before_default + 1 && media_asset_key_equal(last_attachment_key, default_key),
          "settings avatar: a listed user without an avatar hash requests the default avatar by path");
    g_app.renderer.settings_scene = previous;
    check(!pump.media_key_is_current(key), "settings avatar: closed Settings no longer admits its avatar");
    ui_settings_scene_release(&scene);
}

// Edit Profile's preview card lives in the editor's own opaque Settings modal:
// its current avatar, banner and decoration (and picked images) must stay
// current there, or every decoded frame is dropped and the avatar stays black.
void run_profile_editor_preview_test(ApplicationMediaPump& pump) noexcept {
    static u32 scratch[2048];
    static UserProfile profile{};
    static ProfileScene preview{};
    const auto json = utf8_literal(
        R"({"user":{"id":"4242","username":"native.test","global_name":"Native Profile",)"
        R"("avatar":"0123456789abcdef0123456789abcdef","avatar_decoration_data":{"asset":"a_fedcba9876543210fedcba9876543210","sku_id":"7"}},)"
        R"("user_profile":{"bio":"Preview","banner":"a_testbanner"}})");
    preview.presentation = ProfilePresentation::preview;
    preview.phase = ProfilePhase::ready;
    const bool decoded = user_profile_decode(json, {scratch, 2048}, 4242, 0, &profile);
    check(decoded && profile.avatar.bytes && profile.banner.bytes, "profile editor preview: fixture profile decodes");
    bool valid = decoded &&
                 profile_scene_open(&preview, &profile, nullptr, nullptr, 4242, {0, 0, 300, 400}, {0, 0, 1280, 800});
    check(valid && preview.part_count, "profile editor preview: fixture card opens");
    UiSettingsControl control{1, utf8_literal("Display Name"), utf8_literal("Native Profile"),
                              UiSettingsControlKind::action, true, false, {}, 0, utf8_literal("Edit"), nullptr};
    UiSettingsPageContent content{utf8_literal("Profiles"), {}, {&control, 1}, 1};
    UiPage page{};
    UiPageRegistry registry{};
    static UiSettingsScene scene{};
    const bool paged = ui_page_registry_initialize(&registry, {&page, 1}) &&
                       ui_page_registry_register(&registry, {{1, 2}, utf8_literal("Profiles"), &content}).ok() &&
                       ui_settings_scene_update(&scene, &registry, {0, 0, 1280, 800});
    check(paged, "profile editor preview: the editor's settings scene opens");
    valid = valid && paged;
    auto* previous = g_app.renderer.settings_scene;
    g_app.renderer.settings_scene = &scene;
    MediaAssetKey keys[16]{};
    u32 count = 0;
    bool avatar = false, banner = false;
    for (u32 i = 0; valid && i < preview.part_count && count < 16; ++i) {
        ProfileMedia media{};
        if (!profile_media_resolve(preview, i, &media))
            continue;
        keys[count++] = media.key;
        avatar = avatar || preview.parts[i].media == ProfileMediaKind::avatar;
        banner = banner || preview.parts[i].media == ProfileMediaKind::banner;
    }
    check(valid && avatar, "profile editor preview: the card resolves its avatar media");
    check(valid && banner, "profile editor preview: the card resolves its banner media");
    bool hidden = valid && count;
    for (u32 i = 0; i < count; ++i)
        hidden = hidden && !pump.media_key_is_current(keys[i]);
    check(hidden,
          "profile editor preview: Settings alone does not admit the card's profile media");
    fixture_editor_preview = &preview;
    bool current = count != 0;
    for (u32 i = 0; i < count; ++i)
        current = current && pump.media_key_is_current(keys[i]);
    const auto picked = media_asset_local_draft_preview(0x5052000000000001ull, 1u, MediaAttachmentFormat::png);
    const bool picked_hidden = !pump.media_key_is_current(picked);
    fixture_editor_frame_current = true;
    const bool picked_current = pump.media_key_is_current(picked);
    check(current && picked_hidden && picked_current,
          "profile editor preview: the open editor admits its avatar, banner, decoration and picked images");
    fixture_editor_preview = nullptr;
    fixture_editor_frame_current = false;
    g_app.renderer.settings_scene = previous;
    profile_scene_release(&preview);
    ui_settings_scene_release(&scene);
}

// The quick switcher and people (DM) picker show user avatars over their glyphs;
// a group's custom icon hash is not a user avatar and stays a glyph.
void run_quick_switcher_avatar_test(ApplicationMediaPump& pump) noexcept {
    static UiQuickSwitcherScene scene{};
    UiQuickSwitcherSource source{nullptr, 3, 1,
                                 +[](void*, u32 i, UiQuickSwitcherRow* row) noexcept {
                                     if (i >= 3 || !row)
                                         return false;
                                     constexpr UiQuickSwitcherKind kinds[]{UiQuickSwitcherKind::user,
                                                                           UiQuickSwitcherKind::private_channel,
                                                                           UiQuickSwitcherKind::group_private};
                                     *row = {};
                                     row->key = 500 + i;
                                     row->channel_id = i ? 600 + i : 0;
                                     row->user_id = 880 + i;
                                     row->kind = kinds[i];
                                     row->label = utf8_literal("Picker person");
                                     row->avatar_hash = utf8_literal("0123456789abcdef0123456789abcdef");
                                     return true;
                                 },
                                 nullptr};
    UiQuickSwitcherContent content{};
    content.revision = 1;
    content.phase = UiQuickSwitcherPhase::ready;
    content.picker = true;
    const auto* previous = g_app.renderer.quick_switcher_scene;
    bool valid = ui_quick_switcher_scene_update(&scene, &source, &content, {0, 0, 1280, 800});
    g_app.renderer.quick_switcher_scene = &scene;
    UiQuickSwitcherMedia user{}, direct{}, group{};
    valid = valid && ui_quick_switcher_scene_media(scene, 0, nullptr, &user) &&
            ui_quick_switcher_scene_media(scene, 1, nullptr, &direct) &&
            !ui_quick_switcher_scene_media(scene, 2, nullptr, &group) &&
            user.key.kind == MediaAssetKind::user_avatar && user.key.id == 880 && direct.key.id == 881 &&
            ui_rect_has_area(user.clip);
    check(valid && pump.avatar_key_is_current(user.key) && pump.avatar_key_is_current(direct.key) &&
              !pump.avatar_key_is_current(media_asset_user_avatar(882, user.key.variant, false)),
          "quick switcher avatar: people and DM rows admit their avatars; group icons are not user avatars");
    const u32 before = identity_requests;
    u32 uploads = 0;
    bool repaint = false, stop = false;
    if (valid)
        valid = pump.request_visible_user_avatar_value(user.key.id, user.hash, 1000, 1100, &uploads, &repaint,
                                                       &stop, false);
    check(valid && identity_requests == before + 1 && media_asset_key_equal(last_identity_key, user.key),
          "quick switcher avatar: picker rows use the shared user-avatar request");
    g_app.renderer.quick_switcher_scene = previous;
    check(!pump.avatar_key_is_current(user.key), "quick switcher avatar: a closed picker admits nothing");
    ui_quick_switcher_scene_release(&scene);
}

// Forward destinations: a server channel's tile resolves the rail's guild icon
// key, a server without an icon keeps its glyph, DM rows admit avatars, and the
// icon's arrival in the atlas changes the presentation stamp (a repaint).
void run_forward_identity_test(AccountUiRuntime& ui, ApplicationMediaPump& pump) noexcept {
    static GuildRecord guilds[2]{};
    static u16 guild_order[2]{};
    static ChannelRecord channels[1]{};
    static u16 channel_order[1]{};
    static u8 names[256]{};
    static UiMessageForwardScene scene{};
    static Theme theme{};
    constexpr auto icon = utf8_literal("a_0123456789abcdef0123456789abcdef");
    const ChatStateStore saved = ui.chat_state;
    bool valid =
        chat_state_store_initialize(&ui.chat_state, {guilds, 2}, {guild_order, 2}, {channels, 1}, {channel_order, 1},
                                    {names, sizeof(names)}) &&
        chat_state_store_upsert_guild(&ui.chat_state,
                                      {710, utf8_literal("Iconic"), GuildMutationKind::create, 1, icon, true})
            .ok() &&
        chat_state_store_upsert_guild(&ui.chat_state,
                                      {720, utf8_literal("Plain server"), GuildMutationKind::create, 1, {}, true})
            .ok();
    UiMessageForwardSource source{nullptr, 3, 1,
                                  +[](void*, u32 i, UiMessageForwardRow* row) noexcept {
                                      if (i >= 3 || !row)
                                          return false;
                                      *row = {};
                                      row->key = row->channel_id = 7101 + i * 100;
                                      row->guild_id = i == 0 ? 710 : i == 1 ? 720 : 0;
                                      row->user_id = i == 2 ? 883 : 0;
                                      row->kind = i == 2 ? UiQuickSwitcherKind::private_channel
                                                         : UiQuickSwitcherKind::text_channel;
                                      row->label = utf8_literal("destination");
                                      row->avatar_hash =
                                          i == 2 ? utf8_literal("0123456789abcdef0123456789abcdef") : Utf8View{};
                                      return true;
                                  },
                                  nullptr};
    UiMessageForwardContent content{};
    content.revision = 1;
    valid = valid && ui_message_forward_scene_update(&scene, &source, &content, {0, 0, 1280, 800});
    const auto* previous = g_app.renderer.message_forward_scene;
    g_app.renderer.message_forward_scene = &scene;
    // Server tiles draw the still icon (the non-animated icon URL), the
    // rail's at-rest key even for an a_ hash.
    const auto expected = media_asset_guild_icon(710, cdn_guild_icon_variant(icon), false);
    UiMessageForwardMedia server{}, plain{}, direct{};
    check(valid && ui_message_forward_scene_media(scene, 0, &ui.chat_state, &server) &&
              media_asset_key_equal(server.key, expected) && server.hash.bytes == icon.bytes && !server.circle &&
              server.radius > 0 && ui_rect_has_area(server.clip) && pump.media_key_is_current(server.key),
          "forward identity: a server channel's tile resolves the rail's guild icon key");
    check(!ui_message_forward_scene_media(scene, 1, &ui.chat_state, &plain) &&
              !ui_message_forward_scene_media(scene, 0, nullptr, &plain),
          "forward identity: a server without an icon keeps its tile placeholder");
    check(ui_message_forward_scene_media(scene, 2, nullptr, &direct) &&
              direct.key.kind == MediaAssetKind::user_avatar && direct.key.id == 883 && direct.circle &&
              pump.avatar_key_is_current(direct.key),
          "forward identity: a DM destination admits its user avatar");
    const u32 before = identity_requests;
    u32 uploads = 0;
    bool repaint = false, stop = false;
    identity_requests_queue = true;
    valid = pump.request_forward_identities(1000, 1100, &uploads, &repaint, &stop);
    check(valid && !stop && identity_requests == before + 2 && media_asset_key_equal(previous_identity_key, expected) &&
              media_asset_key_equal(last_identity_key, direct.key),
          "forward identity: tiles use the rail's guild icon request and the shared avatar request");
    // Arrival: the resident icon is drawn by key, changes the presentation
    // stamp (so the frame repaints) and is not requested again.
    const auto cold = renderer_capture_presentation_stamp(g_app.renderer, theme);
    const auto arrived = media_atlas_allocate(g_app.renderer.media_atlas, expected, 32, 32);
    const auto warm = renderer_capture_presentation_stamp(g_app.renderer, theme);
    const u32 resident_before = identity_requests;
    stop = false;
    valid = arrived.ok() && pump.request_forward_identities(1000, 1100, &uploads, &repaint, &stop);
    check(valid && cold.valid && warm.valid && !renderer_presentation_stamp_equal(cold, warm) &&
              media_atlas_peek(g_app.renderer.media_atlas, server.key).ok() &&
              identity_requests == resident_before + 1 && media_asset_key_equal(last_identity_key, direct.key),
          "forward identity: an arriving server icon repaints its tile and is not requested again");
    {
        // Only Home's conversation rows show typing subtitles: typing in a
        // server channel (typing expiry ticks every few hundred ms during
        // chatter) must not repaint a server view.
        const NavigationView* saved_navigation = g_app.renderer.navigation_view;
        const ChatStateStore* saved_state = g_app.renderer.chat_state;
        NavigationView navigation = saved_navigation ? *saved_navigation : NavigationView{};
        TypingStore typing{};
        const UiPrivateRowSource rows{&ui.chat_state, nullptr, nullptr, &typing};
        navigation.private_rows = &rows;
        g_app.renderer.navigation_view = &navigation;
        g_app.renderer.chat_state = &ui.chat_state;
        const bool home = ui.chat_state.home_selected;
        ui.chat_state.home_selected = false;
        const auto server_before = renderer_capture_presentation_stamp(g_app.renderer, theme);
        ++typing.revision;
        const auto server_after = renderer_capture_presentation_stamp(g_app.renderer, theme);
        ui.chat_state.home_selected = true;
        const auto home_before = renderer_capture_presentation_stamp(g_app.renderer, theme);
        ++typing.revision;
        const auto home_after = renderer_capture_presentation_stamp(g_app.renderer, theme);
        ui.chat_state.home_selected = home;
        g_app.renderer.navigation_view = saved_navigation;
        g_app.renderer.chat_state = saved_state;
        check(server_before.valid && server_after.valid && renderer_presentation_stamp_equal(server_before, server_after) &&
                  home_before.valid && home_after.valid && !renderer_presentation_stamp_equal(home_before, home_after),
              "presentation stamp: typing repaints conversation rows in Home only");
    }
    identity_requests_queue = false;
    g_app.renderer.message_forward_scene = previous;
    check(!pump.avatar_key_is_current(direct.key), "forward identity: a closed picker admits nothing");
    ui_message_forward_scene_release(&scene);
    // The quick switcher's server rows share the same resolver and key.
    static UiQuickSwitcherScene switcher{};
    UiQuickSwitcherSource servers{nullptr, 2, 1,
                                  +[](void*, u32 i, UiQuickSwitcherRow* row) noexcept {
                                      if (i >= 2 || !row)
                                          return false;
                                      *row = {};
                                      row->key = row->guild_id = i ? 720 : 710;
                                      row->kind = UiQuickSwitcherKind::guild;
                                      row->label = utf8_literal("server");
                                      return true;
                                  },
                                  nullptr};
    UiQuickSwitcherContent switcher_content{};
    switcher_content.revision = 1;
    switcher_content.phase = UiQuickSwitcherPhase::ready;
    UiQuickSwitcherMedia iconic{}, initials{};
    check(ui_quick_switcher_scene_update(&switcher, &servers, &switcher_content, {0, 0, 1280, 800}) &&
              ui_quick_switcher_scene_media(switcher, 0, &ui.chat_state, &iconic) &&
              media_asset_key_equal(iconic.key, expected) &&
              !ui_quick_switcher_scene_media(switcher, 1, &ui.chat_state, &initials),
          "quick switcher identity: server rows resolve the rail's icon key; icon-less servers keep the glyph");
    ui_quick_switcher_scene_release(&switcher);
    ui.chat_state = saved;
}

void run_search_media_test(VisibilityFixture& fixture, AccountUiRuntime& ui,
                           ApplicationMediaPump& pump) noexcept {
    MessageRecord record[1]{};
    u32 order[1]{};
    u8 text[64]{};
    MessageRichRecord rich_records[2]{};
    u16 rich_order[1]{};
    u8 rich_text[1024]{};
    MessageStore store{};
    MessageRichStore rich{};
    UiVariableList list{};
    MessageTimeline timeline{};
    u32 heights[1]{};
    u64 fenwick[1]{};
    bool valid = message_store_initialize(&store, {record, 1}, {order, 1}, {text, sizeof(text)}) &&
                 message_store_upsert(&store, {880, 88, 89, 0, 0, utf8_literal("Accepted bank"), 0}).ok() &&
                 message_rich_store_initialize(&rich, {rich_records, 2}, {rich_order, 1},
                                               {rich_text, sizeof(rich_text)});
    record[0].revision = 73;
    GatewayMessageAttachment outer{};
    outer.id = 555;
    outer.filename = utf8_literal("fixture.png");
    outer.content_type = utf8_literal("image/png");
    outer.url = utf8_literal(
        "https://cdn.discordapp.com/attachments/88/555/"
        "fixture.png?ex=ffffffff&is=1&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    outer.width = outer.height = 64;
    outer.has_width = outer.has_height = true;
    auto child = outer;
    child.width = child.height = 0;
    child.has_width = child.has_height = false;
    GatewayMessageEvent event{};
    event.id = 880;
    event.fields = gateway_message_field_attachments | gateway_message_field_message_snapshots |
                   gateway_message_field_message_reference;
    event.attachments = {&outer, 1};
    event.reference.type = 1;
    event.reference.channel_id = 99;
    event.reference.message_id = 990;
    event.snapshot.present = true;
    event.snapshot.fields = gateway_message_field_attachments;
    event.snapshot.attachments = {&child, 1};
    valid = valid && message_rich_store_apply(&rich, 880, 73, event).ok() &&
            ui_variable_list_initialize(&list, {heights, 1}, {fenwick, 1}, 1, ui_extent_units(57)) &&
            message_timeline_initialize(&timeline, &store, &list, ui_extent_units(57), ui_extent_units(600),
                                        false) &&
            message_timeline_bind_rich_store(&timeline, &rich);
    fixture_search_source = {};
    fixture_search_source.ticket = {880, 73, 0};
    fixture_search_source.store = &store;
    fixture_search_source.message = &record[0];
    fixture_search_source.handle = message_store_ordered_handle(store, 0);
    fixture_search_source.rich_store = &rich;
    fixture_search_current = true;
    UiSearchSource source{};
    source.count = source.revision = 1;
    source.rich_timeline = &timeline;
    source.current = +[](void*) noexcept { return fixture_search_current; };
    source.resolve = +[](void*, u32 i, UiSearchRow* row) noexcept {
        if (i || !row)
            return false;
        *row = {88, 880, utf8_literal("Original author"), utf8_literal("#accepted"), {}, {}, true, true};
        return true;
    };
    UiSearchContent content{};
    content.edit.active = content.panel = true;
    content.phase = UiSearchPhase::ready;
    content.revision = 1;
    UiSearchScene scene{};
    valid = valid && ui_search_scene_update(&scene, &source, &content, {0, 0, 1200, 800}, {0, 0, 1200, 48});
    const auto* prior = g_app.renderer.search_scene;
    g_app.renderer.search_scene = &scene;
    const auto parent_key =
        media_asset_search_attachment(555, MediaAttachmentFormat::png, MediaResolutionTier::px256, 73);
    const auto child_key = media_asset_search_attachment(555, MediaAttachmentFormat::png,
                                                         MediaResolutionTier::px256, 73, true);
    check(valid && !message_store_find(g_app.message_view.store, 880).ok() &&
              pump.message_image_source_is_current(parent_key, 880, 73) &&
              pump.message_image_source_is_current(child_key, 880, 73),
          "search media: independent accepted bank authorizes outer and forwarded sources absent from "
          "timeline");
    if (valid) {
        AccountMediaFrame frame{};
        frame.key = child_key;
        frame.source_message_id = 880;
        frame.source_message_revision = 73;
        frame.kind = AccountMediaFrameKind::straight_rgba8;
        frame.width = 80;
        frame.height = 40;
        bool changed = false;
        const u32 publications = fixture_search_dimension_publications;
        const bool dimensions = pump.update_media_display_dimensions(frame, &changed);
        const auto* parent = message_rich_store_find(rich, 880);
        const auto* snapshot = message_rich_store_snapshot(rich, 880);
        check(dimensions && changed && snapshot && snapshot->attachments[0].width == 80 &&
                  parent->attachments[0].width == 64 &&
                  fixture_search_dimension_publications == publications + 1,
              "search media: child same-ID completion changes only exact child geometry and invalidates "
              "Search layout");
        u32 uploads = 0;
        bool repaint = false, stop = false;
        const u32 requests = attachment_requests;
        check(pump.request_visible_attachment(record[0], snapshot->attachments[0], 80, 40, 1000, 1100,
                                              &uploads, &repaint, &stop, &rich, true) &&
                  attachment_requests == requests,
              "search media: accepted bank cannot request media without containing channel authority");
        auto& state = ui.chat_state;
        constexpr u64 permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
        const bool authority =
            chat_state_store_bind_permissions(&state, {fixture.roles, 1}, {fixture.overwrites, 1}) &&
            chat_state_store_upsert_guild(
                &state, {7, utf8_literal("Search guild"), GuildMutationKind::create, 90})
                .ok() &&
            chat_state_store_upsert_channel(&state, {88, 7, 0, utf8_literal("accepted"), 0, 0, 0})
                .ok() &&
            chat_state_store_upsert_role(&state, {7, 7, permissions, utf8_literal("Everyone")}) &&
            chat_state_store_set_self_roles(&state, 7, 90, 89, {});
        chat_state_store_recompute_guild_permissions(&state, 7);
        check(authority && application_channel_readable(g_app, 88) &&
                  !chat_state_store_find_channel(state, 99),
              "search media: real containing permissions authorize the accepted bank independently of "
              "uncached forwarding origin");
        check(pump.request_visible_attachment(record[0], snapshot->attachments[0], 80, 40, 1000, 1100,
                                              &uploads, &repaint, &stop, &rich, true) &&
                  attachment_requests == requests + 1 && last_attachment_message == 880 &&
                  last_attachment_revision == 73 && media_asset_search_snapshot(last_attachment_key),
              "search media: actual shared media lane receives exact accepted publication and child key");
        MessageTextSemantic semantics[5]{};
        TextHitRegion regions[6]{};
        RendererTextStorage renderer_text{};
        for (u32 i = 0; i < 5; ++i) {
            semantics[i] = {880,
                            330 + i,
                            73,
                            0,
                            2,
                            MessageMarkdownSemanticKind::custom_emoji,
                            kRendererSearchSemanticFlag};
            regions[i] = {scene.body.left + 10, scene.body.top + 10, scene.body.left + 30,
                          scene.body.top + 30,  static_cast<u16>(i), 0};
        }
        semantics[1].reserved |= 2u;
        semantics[2].reserved = 0;
        semantics[3].message_revision = 72;
        regions[4].top = scene.body.bottom + 1;
        regions[4].bottom = regions[4].top + 20;
        regions[5] = regions[0];
        renderer_text.content_semantics = semantics;
        renderer_text.content_semantic_count = 5;
        renderer_text.content_hit_regions = regions;
        renderer_text.content_hit_region_count = 6;
        auto* prior_text = g_app.renderer.content_text;
        g_app.renderer.content_text = &renderer_text;
        observe_picker_requests = true;
        picker_busy_flag_first = false;
        picker_emoji_requests = 0;
        message_media_controller_begin_asset_scan(&g_app.message_media);
        check(pump.request_search_decorations(1000, 1100, 1000000, &uploads, &repaint, &stop) &&
                  picker_emoji_requests == 1 && picker_requested_keys[0].id == 330,
              "search media: parsed clipped decoration ignores concealed ordinary stale and offscreen "
              "semantics and deduplicates regions");
        fixture_search_current = false;
        check(!pump.message_image_source_is_current(parent_key, 880, 73) &&
                  !pump.message_image_source_is_current(child_key, 880, 73) &&
                  !pump.message_image_source_is_current(parent_key, 880, 72),
              "search media: retired accepted source rejects both scopes and old publication completion");
        observe_picker_requests = false;
        g_app.renderer.content_text = prior_text;
    }
    fixture_search_current = false;
    fixture_search_source = {};
    g_app.renderer.search_scene = prior;
    ui_search_scene_release(&scene);
}

u8 poster_actor, poster_other_actor, poster_gpu_token;
u32 poster_uploaded_calls, poster_presented_calls;
bool poster_shape_ok, poster_change_account;
AccountMediaRequestState poster_upload_state, poster_present_state;
AccountMediaLocalPreviewRequest poster_receipt;
AccountMediaRequestResult WINAPI poster_control(void* instance, AccountLocalPreviewOperation operation,
                                                const AccountMediaLocalPreviewRequest* request,
                                                u64 now, u64 unix_now) noexcept {
    poster_shape_ok = poster_shape_ok && instance == &poster_actor && !now && !unix_now;
    AccountMediaRequestResult result{};
    if (operation == AccountLocalPreviewOperation::uploaded) {
        ++poster_uploaded_calls;
        poster_shape_ok = poster_shape_ok && request && !request->content.context && !request->content.ops &&
            !request->content.bytes && !request->snapshot.bytes && !request->snapshot.identity.index &&
            !request->snapshot.identity.last_write && !request->snapshot.identity.volume &&
            !request->snapshot.identity.reserved && !request->path.data && !request->path.count &&
            !request->video_device && !request->reserved;
        if (request) poster_receipt = *request;
        result.state = poster_upload_state;
    } else if (operation == AccountLocalPreviewOperation::presented) {
        ++poster_presented_calls;
        poster_shape_ok = poster_shape_ok && !request;
        result.state = poster_present_state;
    } else poster_shape_ok = false;
    if (poster_change_account) g_app.account.instance = &poster_other_actor;
    return result;
}
void run_poster_presentation_test(ApplicationMediaPump& pump) noexcept {
    const auto saved_instance = g_app.account.instance;
    const auto saved_api = g_app.account.api.media_local_preview;
    const auto saved_profile = g_app.account.status.profile_id;
    const auto saved_running = g_app.account.status.running;
    const auto saved_scope = g_app.plugin_account_scope_serial;
    const auto saved_pending = g_app.local_poster_presentation;
    const auto saved_local_key = fixture_local_key;
    const auto saved_current = fixture_local_current;
    const auto saved_exact = fixture_local_exact;
    const auto saved_rejections = fixture_local_rejections;
    const bool frame_pending = g_app.frame_pending;
    const u32 uploads = fixture_local_uploads;
    g_app.account.instance = &poster_actor;
    g_app.account.api.media_local_preview = poster_control;
    g_app.account.status.profile_id = 44;
    g_app.account.status.running = true;
    g_app.plugin_account_scope_serial = 200;
    g_app.local_poster_presentation = {};
    fixture_local_current = fixture_local_exact = true;
    fixture_local_key = media_asset_local_draft_preview(9000, 9u | kLocalDraftPreviewThumbnailVariant,
                                                       MediaAttachmentFormat::mp4);
    AccountMediaFrame frame{};
    frame.key = fixture_local_key;
    frame.kind = AccountMediaFrameKind::video_nv12_gpu;
    frame.gpu_video = reinterpret_cast<const GpuVideoFrame*>(&poster_gpu_token);
    frame.source_kind = AccountMediaSourceKind::local_draft_preview;
    frame.source_catalog_scope = 700;
    frame.source_catalog_generation = 9;
    frame.source_message_id = frame.key.id;
    frame.source_message_revision = frame.key.variant;
    frame.lease_serial = 812;
    poster_shape_ok = true;
    poster_upload_state = AccountMediaRequestState::pending;
    poster_present_state = AccountMediaRequestState::ready;
    // Invalid dimensions reject before any fake GPU pointer can be used.
    check(pump.upload_account_media_frame(frame) == ApplicationMediaPump::MediaFrameUploadResult::deferred &&
              !poster_uploaded_calls && !g_app.local_poster_presentation.pending,
          "poster cleanup: rejected GPU upload sends no receipt and arms no Present work");
    for (u32 change = 0; change < 8; ++change) {
        auto invalid = frame;
        if (change == 0) invalid.lease_serial = 0;
        if (change == 1) invalid.kind = AccountMediaFrameKind::straight_rgba8;
        if (change == 2) invalid.gpu_video = nullptr;
        if (change == 3) invalid.source_kind = AccountMediaSourceKind::none;
        if (change == 4) ++invalid.key.id;
        if (change == 5) ++invalid.source_catalog_scope;
        if (change == 6) ++invalid.source_catalog_generation;
        if (change == 7) ++invalid.source_message_revision;
        application_media_poster_uploaded(g_app, invalid);
    }
    fixture_local_current = false;
    application_media_poster_uploaded(g_app, frame);
    fixture_local_current = true;
    check(!poster_uploaded_calls && !g_app.local_poster_presentation.pending,
          "poster cleanup: only a current local GPU frame with a lease can send an upload receipt");
    application_media_poster_uploaded(g_app, frame);
    check(poster_uploaded_calls == 1 && poster_shape_ok && g_app.local_poster_presentation.pending &&
              g_app.local_poster_presentation.instance == &poster_actor &&
              g_app.local_poster_presentation.profile == 44 && g_app.local_poster_presentation.account_scope == 200 &&
              media_asset_key_equal(poster_receipt.key, frame.key) && poster_receipt.owner_scope == 700 &&
              poster_receipt.owner_generation == 9 && poster_receipt.lease_serial == 812,
          "poster cleanup: successful upload sends only the exact lease and binds pending work to its account");
    application_media_posters_presented(g_app, false, 10, 11);
    application_media_posters_presented(g_app, true, 10, 10);
    check(!poster_presented_calls && g_app.local_poster_presentation.pending,
          "poster cleanup: failed and skipped Present keep the current pending lease without a callback");
    // A duplicate ready reply must not erase a prior pending lane.
    poster_upload_state = AccountMediaRequestState::ready;
    application_media_poster_uploaded(g_app, frame);
    check(g_app.local_poster_presentation.pending, "poster cleanup: ready duplicate preserves another pending lane");
    poster_present_state = AccountMediaRequestState::pending;
    application_media_posters_presented(g_app, true, 10, 11);
    check(poster_presented_calls == 1 && g_app.local_poster_presentation.pending,
          "poster cleanup: backend can retain pending work for a later real Present");
    poster_present_state = AccountMediaRequestState::ready;
    application_media_posters_presented(g_app, true, 11, 12);
    const auto presented = poster_presented_calls;
    for (u32 i = 0; i < 64; ++i) application_media_posters_presented(g_app, true, i, i + 1);
    check(presented == 2 && poster_presented_calls == presented && !g_app.local_poster_presentation.pending,
          "poster cleanup: one accepted Present clears the flag and later frames make no account calls");
    const AccountMediaRequestState terminal[]{AccountMediaRequestState::ready, AccountMediaRequestState::invalid,
                                             AccountMediaRequestState::unsupported, AccountMediaRequestState::failed};
    for (const auto state : terminal) {
        poster_upload_state = state;
        application_media_poster_uploaded(g_app, frame);
        check(!g_app.local_poster_presentation.pending, "poster cleanup: fallback upload does not arm a frame");
        poster_upload_state = AccountMediaRequestState::pending;
        application_media_poster_uploaded(g_app, frame);
        poster_present_state = state;
        application_media_posters_presented(g_app, true, 12, 13);
        check(!g_app.local_poster_presentation.pending, "poster cleanup: terminal Present reply leaves no frame retry");
    }
    for (u32 change = 0; change < 5; ++change) {
        application_media_poster_uploaded(g_app, frame);
        const auto count = poster_presented_calls;
        if (change == 0) g_app.account.instance = &poster_other_actor;
        if (change == 1) ++g_app.account.status.profile_id;
        if (change == 2) ++g_app.plugin_account_scope_serial;
        if (change == 3) g_app.account.status.running = false;
        if (change == 4) g_app.account.api.media_local_preview = nullptr;
        application_media_posters_presented(g_app, true, 13, 14);
        check(!g_app.local_poster_presentation.pending && count == poster_presented_calls,
              "poster cleanup: stale or stopped account clears its flag without an account call");
        g_app.account.instance = &poster_actor; g_app.account.status.profile_id = 44;
        g_app.plugin_account_scope_serial = 200; g_app.account.status.running = true;
        g_app.account.api.media_local_preview = poster_control;
    }
    poster_change_account = true;
    application_media_poster_uploaded(g_app, frame);
    check(!g_app.local_poster_presentation.pending,
          "poster cleanup: an account replacement during acknowledgement cannot arm stale work");
    poster_change_account = false;
    check(poster_shape_ok && g_app.frame_pending == frame_pending && fixture_local_current && fixture_local_uploads == uploads,
          "poster cleanup: receipts preserve tile ownership and readiness and do not request an extra frame");
    g_app.account.instance = saved_instance; g_app.account.api.media_local_preview = saved_api;
    g_app.account.status.profile_id = saved_profile; g_app.account.status.running = saved_running;
    g_app.plugin_account_scope_serial = saved_scope; g_app.local_poster_presentation = saved_pending;
    fixture_local_key = saved_local_key; fixture_local_current = saved_current; fixture_local_exact = saved_exact;
    fixture_local_rejections = saved_rejections;
}

void run_visibility_test() noexcept {
    auto* ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!ui) {
        check(false, "media visibility: bounded fixture UI allocation");
        return;
    }
    VisibilityFixture fixture{};
    g_app.account_ui = ui;
    g_app.account.instance = &fixture;
    g_app.account.api.media_request = request_static;
    g_app.account.api.media_guild_icon_request = request_avatar;
    g_app.account.api.media_attachment_request = request_attachment;
    g_app.account.api.media_advance = advance_animation;
    g_app.account.api.media_take = take_frame;
    g_app.renderer.dpi = 96;
    g_app.renderer.media_atlas = g_app.renderer.preview_media_atlas = &fixture.atlas;
    g_app.clock = clock_initialize();
    g_app.process_start = clock_now();
    bool valid = seed_scene(fixture, *ui) &&
                 media_atlas_initialize(&fixture.atlas, 256, 256, 0, {fixture.atlas_entries, 8},
                                        {fixture.atlas_buckets, 16}, {fixture.allocations, 8},
                                        {fixture.atlas_rows, 4}) &&
                 message_media_controller_initialize(&g_app.message_media, {fixture.visible_rows, 3},
                                                     {fixture.marks, 16}) &&
                 animation_timeline_initialize(&g_app.animations.timeline, {fixture.animations, 4},
                                               {fixture.animation_buckets, 8});
    const auto visible_key = attachment_key(fixture, *ui, 1);
    const auto hidden_key = attachment_key(fixture, *ui, 2);
    const u64 published_at = clock_microseconds(g_app.clock, g_app.process_start, clock_now());
    valid = valid && media_atlas_allocate(&fixture.atlas, visible_key, 64, 64).ok() &&
            media_atlas_allocate(&fixture.atlas, hidden_key, 64, 64).ok() &&
            animation_timeline_publish(&g_app.animations.timeline, {visible_key, 1000000, 1, 3, 0, true},
                                       published_at)
                .ok() &&
            animation_timeline_publish(&g_app.animations.timeline, {hidden_key, 1000000, 2, 3, 0, true},
                                       published_at)
                .ok();
    g_app.animations.visuals = fixture.visuals;
    for (u32 index = 1; valid && index < 3; ++index) {
        const auto key = index == 1 ? visible_key : hidden_key;
        const auto found = animation_timeline_find(&g_app.animations.timeline, key);
        valid = found.ok();
        if (!valid)
            break;
        auto& visual = fixture.visuals[found.slot];
        visual.key = key;
        visual.generation = index;
        visual.cache_generation = static_cast<u16>(index);
        visual.kind = AccountMediaFrameKind::gif_indices;
        visual.occupied = true;
        visual.source_scope = fixture.records[index].id;
        visual.source_revision = fixture.records[index].revision;
    }
    check(valid, "media visibility: real clipped attachment scene and resident timelines initialize");
    if (valid) {
        ApplicationMediaPump pump{g_app};
        bool repaint = false;
        valid = pump.pump_visible_static_media(&repaint);
        const auto visible = animation_timeline_find(&g_app.animations.timeline, visible_key);
        const auto hidden = animation_timeline_find(&g_app.animations.timeline, hidden_key);
        check(
            valid && attachment_requests == 1 && !repaint &&
                (fixture.animations[visible.slot].flags & animation_timeline_entry_visible) &&
                !(fixture.animations[hidden.slot].flags & animation_timeline_entry_visible),
            "media visibility: busy earlier request cannot pause later visible GIF or retain hidden child");
        // Repeated saturated pumps must preserve the visible frame deadline,
        // rather than pausing and resuming it in alternating visibility epochs.
        const u64 deadline = fixture.animations[visible.slot].due_at_us;
        const u64 pauses = g_app.animations.timeline.counters.pauses;
        valid = pump.pump_visible_static_media(&repaint);
        check(valid && attachment_requests == 2 &&
                  (fixture.animations[visible.slot].flags & animation_timeline_entry_visible) &&
                  fixture.animations[visible.slot].due_at_us == deadline &&
                  g_app.animations.timeline.counters.pauses == pauses,
              "media visibility: repeated saturation keeps animation cadence and hidden pause stable");
        u32 uploads = kMessageMediaUploadBudget;
        pump.visibility_only = true;
        animation_timeline_begin_visibility(&g_app.animations.timeline);
        valid = pump.scan_visible_media(clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, 0,
                                        uploads, nullptr);
        pump.visibility_only = false;
        animation_timeline_end_visibility(&g_app.animations.timeline, 0);
        check(valid && attachment_requests == 2 && uploads == kMessageMediaUploadBudget &&
                  (fixture.animations[visible.slot].flags & animation_timeline_entry_visible),
              "media visibility: observation does not fetch or stop at an exhausted upload budget");
        g_app.preferences.reduced_motion = true;
        animation_timeline_suspend(&g_app.animations.timeline, published_at);
        valid = pump.pump_visible_static_media(&repaint);
        check(valid && g_app.animations.timeline.suspended &&
                  (fixture.animations[visible.slot].flags & animation_timeline_entry_visible) &&
                  !(fixture.animations[hidden.slot].flags & animation_timeline_entry_visible) &&
                  animation_timeline_next_wake_at_us(&g_app.animations.timeline) == 0,
              "media visibility: reduced motion preserves scene ownership without animation wakes");
        g_app.login.visible = true;
        valid = pump.pump_visible_static_media(&repaint);
        check(valid && !(fixture.animations[visible.slot].flags & animation_timeline_entry_visible) &&
                  !(fixture.animations[hidden.slot].flags & animation_timeline_entry_visible),
              "media visibility: opaque login commits an empty visible animation set");
        g_app.login.visible = false;
        // PNG/WebP metadata need not carry an animation hint. The actual
        // decoder owns that classification after publishing a timeline; a
        // resident first frame must still participate in every visible epoch.
        constexpr MediaAttachmentFormat decoded_formats[] = {MediaAttachmentFormat::png,
                                                             MediaAttachmentFormat::webp};
        for (const auto format : decoded_formats) {
            GatewayMessageAttachment attachment{};
            attachment.id = 201;
            attachment.filename = format == MediaAttachmentFormat::png ? utf8_literal("decoded.png")
                                                                       : utf8_literal("decoded.webp");
            attachment.content_type = format == MediaAttachmentFormat::png ? utf8_literal("image/png")
                                                                           : utf8_literal("image/webp");
            attachment.url =
                utf8_literal("https://cdn.discordapp.com/attachments/7/201/decoded.png"
                             "?ex=ffffffff&is=1&hm="
                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
            attachment.width = attachment.height = 64;
            attachment.has_width = attachment.has_height = true;
            GatewayMessageEvent event{};
            event.id = fixture.records[1].id;
            event.fields = gateway_message_field_attachments;
            event.attachments = {&attachment, 1};
            const auto decoded_key =
                media_asset_attachment(attachment.id, format, media_asset_resolution_tier(visible_key));
            const auto decoded_timeline = animation_timeline_publish(
                &g_app.animations.timeline, {decoded_key, 1000000, 17, 3, 0, true}, published_at);
            valid = message_rich_store_apply(&ui->message_rich_store, event.id, fixture.records[1].revision,
                                             event)
                        .ok() &&
                    media_atlas_allocate(&fixture.atlas, decoded_key, 64, 64).ok() && decoded_timeline.ok();
            const auto decoded_resident = media_atlas_peek(&fixture.atlas, decoded_key);
            valid = valid && decoded_resident.ok() &&
                    media_atlas_set_decoded_animation(&fixture.atlas, decoded_resident.handle, true);
            if (valid) {
                auto& visual = fixture.visuals[decoded_timeline.slot];
                visual = {};
                visual.key = decoded_key;
                visual.generation = 17;
                visual.cache_generation = 17;
                visual.source_scope = event.id;
                visual.source_revision = fixture.records[1].revision;
                visual.kind = format == MediaAttachmentFormat::png
                                  ? AccountMediaFrameKind::apng_scanlines
                                  : AccountMediaFrameKind::animated_webp_rgba8;
                visual.occupied = true;
                valid = pump.pump_visible_static_media(&repaint);
            }
            check(valid && !(decoded_key.flags & media_asset_flag_animated) &&
                      (fixture.animations[decoded_timeline.slot].flags & animation_timeline_entry_visible),
                  "media visibility: decoded PNG/WebP animation stays visible without metadata animation "
                  "flag");
            if (valid) {
                run_animation_owner_test(fixture, *ui, pump, decoded_key,
                                         format == MediaAttachmentFormat::png
                                             ? AccountMediaFrameKind::apng_scanlines
                                             : AccountMediaFrameKind::animated_webp_rgba8);
                check(message_scene_update(&g_app.message_scene, g_app.message_scene.viewport).ok(),
                      "media visibility: real edited owner refreshes the resident scene");
                const auto prior = animation_timeline_find(&g_app.animations.timeline, decoded_key);
                if (prior.ok())
                    pump.release_animation_visual(decoded_key, fixture.animations[prior.slot].generation);
                const u32 before_static = attachment_requests;
                u32 static_uploads = 0;
                bool static_stop = false;
                check(media_atlas_set_decoded_animation(&fixture.atlas, decoded_resident.handle, false) &&
                          !media_atlas_decoded_animation(fixture.atlas, decoded_resident.handle) &&
                          pump.request_visible_attachment(
                              fixture.records[1], fixture.rich_records[1].attachments[0], 64, 64,
                              clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000,
                              &static_uploads, &repaint, &static_stop) &&
                          attachment_requests == before_static,
                      "media visibility: accepted static replacement clears decoded classification without "
                      "reacquisition loop");
                auto stale_handle = decoded_resident.handle;
                ++stale_handle.generation;
                check(!media_atlas_set_decoded_animation(&fixture.atlas, stale_handle, true) &&
                          !media_atlas_decoded_animation(fixture.atlas, stale_handle) &&
                          !media_atlas_decoded_animation(fixture.atlas, decoded_resident.handle),
                      "media visibility: stale residency cannot restore decoded animation classification");
            }
            if (decoded_timeline.ok())
                pump.release_animation_visual(decoded_key, 17);
        }
        // Restore the original owner format for the subsequent stale-token
        // tests, which deliberately exercise the GIF authority.
        GatewayMessageAttachment restored_attachment{};
        restored_attachment.id = 201;
        restored_attachment.filename = utf8_literal("owner.gif");
        restored_attachment.content_type = utf8_literal("image/gif");
        restored_attachment.url =
            utf8_literal("https://cdn.discordapp.com/attachments/7/201/loop.gif"
                         "?ex=ffffffff&is=1&hm="
                         "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        restored_attachment.width = restored_attachment.height = 64;
        restored_attachment.has_width = restored_attachment.has_height = true;
        GatewayMessageEvent restored_event{};
        restored_event.id = fixture.records[1].id;
        restored_event.fields = gateway_message_field_attachments;
        restored_event.attachments = {&restored_attachment, 1};
        check(message_rich_store_apply(&ui->message_rich_store, restored_event.id,
                                       fixture.records[1].revision, restored_event)
                  .ok(),
              "media visibility: original owner fixture restores after decoded animation coverage");
        run_animation_owner_test(fixture, *ui, pump, visible_key);
        animation_timeline_reset(&g_app.animations.timeline);
        const auto retained_catalog = media_asset_catalog_preview(900, 20);
        const auto stale_catalog = media_asset_catalog_preview(901, 19);
        const auto retained_slot = animation_timeline_publish(
            &g_app.animations.timeline, {retained_catalog, 1000, 40, 3, 0, false}, published_at);
        const auto stale_slot = animation_timeline_publish(
            &g_app.animations.timeline, {stale_catalog, 1000, 41, 3, 0, false}, published_at);
        bool catalog_seeded = retained_slot.ok() && stale_slot.ok();
        if (catalog_seeded) {
            auto& retained_visual = fixture.visuals[retained_slot.slot];
            retained_visual = {};
            retained_visual.key = retained_catalog;
            retained_visual.generation = 40;
            retained_visual.source_scope = 600;
            retained_visual.source_revision = 20;
            retained_visual.occupied = true;
            auto& stale_visual = fixture.visuals[stale_slot.slot];
            stale_visual = retained_visual;
            stale_visual.key = stale_catalog;
            stale_visual.generation = 41;
            stale_visual.source_revision = 19;
            g_app.animations.pending_advances[0] = {stale_catalog, published_at, 41, 1, 0};
            g_app.animations.pending_advance_count = 1;
            fixture_catalog_owner_key = retained_catalog;
            fixture_catalog_owner_retained = true;
            catalog_seeded = pump.pump_visible_static_media(&repaint);
        }
        check(catalog_seeded &&
                  animation_timeline_find(&g_app.animations.timeline, retained_catalog).ok() &&
                  !animation_timeline_find(&g_app.animations.timeline, stale_catalog).ok() &&
                  !g_app.animations.pending_advance_count &&
                  !(fixture.animations[retained_slot.slot].flags & animation_timeline_entry_visible),
              "catalog ownership: replaced bank retires stale token while current offscreen bank remains "
              "paused");
        fixture_catalog_owner_retained = false;
        check(pump.pump_visible_static_media(&repaint) &&
                  !animation_timeline_find(&g_app.animations.timeline, retained_catalog).ok() &&
                  !g_app.animations.pending_advance_count,
              "catalog ownership: closed picker retires paused visuals without animation capacity growth");
        run_identity_residency_test(fixture, *ui, pump);
        run_catalog_residency_test(fixture, pump);
        run_picker_emoji_visibility_test(pump);
        run_search_avatar_test(pump);
        run_settings_avatar_test(pump);
        run_profile_editor_preview_test(pump);
        run_quick_switcher_avatar_test(pump);
        run_forward_identity_test(*ui, pump);
        run_search_media_test(fixture, *ui, pump);
        run_poster_presentation_test(pump);
    }
    if (valid) {
        ForumPostScene forum_scene{}; ForumPostSurface forum_surface{};
        forum_scene.active = true; forum_scene.content.owner_serial = fixture_forum_ticket.serial;
        forum_surface.scene = &forum_scene;
        fixture_forum_scene = &forum_scene; fixture_forum_current = fixture_local_current = true;
        g_app.renderer.forum_form = &forum_surface;
        fixture_local_key = media_asset_local_draft_preview(1000, 1, MediaAttachmentFormat::png);
        animation_timeline_reset(&g_app.animations.timeline);
        const auto published = animation_timeline_publish(&g_app.animations.timeline, {visible_key, 1000, 1, 3, 0, true}, 100);
        AnimationFrameAdvance wrapped{};
        bool advanced = true;
        for (u64 due = 1100; due <= 2100; due += 1000) {
            const auto next = animation_timeline_collect_due(&g_app.animations.timeline, due, {&wrapped, 1});
            advanced = advanced && next.ok() && next.advances == 1 &&
                animation_timeline_commit_advance(&g_app.animations.timeline, wrapped, 1000).ok();
        }
        const auto collected = animation_timeline_collect_due(&g_app.animations.timeline, 3100, {&wrapped, 1});
        check(published.ok() && advanced && collected.ok() && collected.advances == 1 && !wrapped.frame_index,
            "forum preview pump: real wrapped frame-zero pending token initializes");
        if (published.ok()) {
            auto& visual = fixture.visuals[published.slot]; visual = {};
            visual.key = visible_key; visual.generation = 1; visual.cache_generation = 11; visual.occupied = true;
            g_app.animations.pending_advances[0] = wrapped; g_app.animations.pending_advance_count = 1;
        }
        fixture_mailbox_count = 5; fixture_mailbox_taken = 0;
        fixture_mailbox[0] = {}; fixture_mailbox[0].key = visible_key; // Current ordinary cold source.
        fixture_mailbox[1] = {}; fixture_mailbox[1].key = hidden_key;
        fixture_mailbox[2] = {}; fixture_mailbox[2].key = visible_key;
        fixture_mailbox[2].cache_generation = 11; fixture_mailbox[2].frame_index = wrapped.frame_index;
        fixture_mailbox[3] = {}; fixture_mailbox[3].source_kind = AccountMediaSourceKind::local_draft_preview;
        fixture_mailbox[3].key = fixture_local_key; ++fixture_mailbox[3].key.variant;
        fixture_mailbox[4] = {}; fixture_mailbox[4].source_kind = AccountMediaSourceKind::local_draft_preview;
        fixture_mailbox[4].key = fixture_local_key; // Invalid pixels exercise exact local rejection before GPU.
        const auto admissions = fixture_local_admissions, attachments = attachment_requests, identities = identity_requests;
        const auto message_revision = fixture.records[1].revision, rich_revision = fixture.rich_records[1].message_revision;
        const auto narrow = application_pump_forum_preview_media(&g_app);
        check(narrow.ok && !narrow.repaint && !narrow.text_changed && !narrow.history_requested &&
            fixture_local_admissions == admissions + 1 && fixture_mailbox_taken == 5 &&
            fixture_local_rejections == 1 && !fixture_local_uploads && !g_app.animations.pending_advance_count &&
            published.ok() && !(fixture.animations[published.slot].flags & animation_timeline_entry_pending) &&
            fixture.visuals[published.slot].occupied && attachment_requests == attachments && identity_requests == identities &&
            fixture.records[1].revision == message_revision && fixture.rich_records[1].message_revision == rich_revision,
            "forum preview pump: local admission/current-source rejection drains shared cold output and defers exact wrapped animation without underlying uploads/read/layout work");
        fixture_forum_current = false; fixture_mailbox_taken = 0;
        check(application_pump_forum_preview_media(&g_app).ok && !fixture_mailbox_taken &&
            fixture_local_admissions == admissions + 1,
            "forum preview pump: stale form publication cannot admit or drain shared mailbox");
        fixture_forum_scene = nullptr; g_app.renderer.forum_form = nullptr;
        fixture_mailbox_count = fixture_mailbox_taken = 0; fixture_local_current = false;
    }
    if (valid) {
        // The thread browser covers only the chat column. The footer avatar
        // stays on screen but holds its frame (the official account panel shows
        // a still avatar); chat media pauses with its remainder instead of
        // stalling visible and replaying on close.
        UiThreadBrowserView browser_view{};
        browser_view.source_publication = 1;
        UiThreadBrowserScene browser{};
        UiThreadBrowserBacking browser_backing{};
        browser.active = true;
        browser.backing = &browser_backing;
        browser.view = &browser_view;
        browser.source_publication = 1;
        g_app.renderer.thread_browser_scene = &browser;
        animation_timeline_reset(&g_app.animations.timeline);
        g_app.animations.pending_advance_count = 0;
        ui->chat_state.self_user_id = 9;
        constexpr auto hash = utf8_literal("a_0123456789abcdef0123456789abcdef");
        const auto avatar = media_asset_user_avatar(9, user_avatar_variant(hash), true);
        // Run the process clock ten seconds in so the footer can be overdue.
        const i64 saved_process_start = g_app.process_start;
        g_app.process_start -= g_app.clock.frequency * 10;
        const u64 start = clock_microseconds(g_app.clock, g_app.process_start, clock_now());
        const auto chat = animation_timeline_publish(&g_app.animations.timeline,
                                                     {visible_key, 5'000'000, 1, 3, 0, true}, start);
        // Published 300 ms ago with a 1 us first frame: overdue far beyond jitter.
        const auto footer = animation_timeline_publish(&g_app.animations.timeline, {avatar, 1, 2, 3, 0, true},
                                                       start - 300'000);
        if (chat.ok()) {
            auto& visual = fixture.visuals[chat.slot];
            visual = {};
            visual.key = visible_key;
            visual.generation = 1;
            visual.cache_generation = 1;
            visual.kind = AccountMediaFrameKind::gif_indices;
            visual.occupied = true;
            visual.source_scope = fixture.records[1].id;
            visual.source_revision = fixture.records[1].revision;
        }
        const u64 advances = g_app.animations.timeline.counters.due_advances;
        const u64 late = g_app.animations.timeline.counters.late_advances;
        const u32 attachments = attachment_requests, identities = identity_requests;
        const auto covered = application_pump_covered_media(&g_app);
        const auto& chat_entry = fixture.animations[chat.slot];
        const auto& footer_entry = fixture.animations[footer.slot];
        check(chat.ok() && footer.ok() && covered.ok && !covered.text_changed && !covered.history_requested &&
                  !(chat_entry.flags & animation_timeline_entry_visible) && chat_entry.due_at_us == 0 &&
                  chat_entry.paused_remaining_us > 4'000'000 && chat_entry.paused_remaining_us <= 5'000'000 &&
                  !(footer_entry.flags & animation_timeline_entry_visible) &&
                  g_app.animations.timeline.counters.due_advances == advances &&
                  g_app.animations.timeline.counters.late_advances == late &&
                  attachment_requests == attachments && identity_requests == identities,
              "covered browser pump: account panel avatar holds its frame, chat pauses with its "
              "remainder, no admission");
        // Closing the browser resumes chat from its remainder, not from a
        // stale visible deadline that would replay missed frames.
        g_app.renderer.thread_browser_scene = nullptr;
        // (The ordinary pump's chat scan observes the entry exactly like this.)
        const u64 remaining = chat_entry.paused_remaining_us;
        const u64 reopened = clock_microseconds(g_app.clock, g_app.process_start, clock_now()) + 2'000'000;
        animation_timeline_begin_visibility(&g_app.animations.timeline);
        const bool rescanned = animation_timeline_observe_visible(&g_app.animations.timeline, visible_key, reopened);
        animation_timeline_end_visibility(&g_app.animations.timeline, reopened);
        AnimationFrameAdvance resumed[2]{};
        check(rescanned && (chat_entry.flags & animation_timeline_entry_visible) &&
                  chat_entry.due_at_us == reopened + remaining && chat_entry.frame_index == 0 &&
                  animation_timeline_collect_due(&g_app.animations.timeline, reopened, {resumed, 2}).advances == 0,
              "covered browser pump: closing resumes chat at its paused frame without catch-up");
        ui->chat_state.self_user_id = 0;
        g_app.process_start = saved_process_start;
        animation_timeline_reset(&g_app.animations.timeline);
    }
    message_scene_release(&g_app.message_scene);
    g_app.account_ui = nullptr;
    g_app.account.instance = nullptr;
    g_app.renderer.media_atlas = g_app.renderer.preview_media_atlas = nullptr;
    g_app.animations.visuals = nullptr;
    VirtualFree(ui, 0, MEM_RELEASE);
}

void run_query_diagnostics_test() noexcept {
    const u8 webp[] = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'E', 'B', 'P'};
    const u8 mp4[] = {0, 0, 0, 12, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm'};
    check(account_media_video_body_failure({webp, sizeof(webp)}) ==
                  AccountMediaVideoFailure::unexpected_webp &&
              account_media_video_body_failure({webp, 11}) == AccountMediaVideoFailure::none &&
              account_media_video_body_failure({mp4, sizeof(mp4)}) == AccountMediaVideoFailure::none,
          "media diagnostics: proxy image signatures are bounded and do not admit a fake MP4 decode");
    u8 target[kCdnMaximumTargetBytes]{};
    const auto duplicate = parse_signed_attachment_url(
        utf8_literal("https://cdn.discordapp.com/attachments/7/200/a.gif?ex=2&ex=3&is=1&hm="
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        200, {target, sizeof(target)});
    const auto animated = parse_signed_attachment_url(
        utf8_literal("https://cdn.discordapp.com/attachments/7/200/a.gif?ex=2&is=1&hm="
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&animated=invalid"),
        200, {target, sizeof(target)});
    const auto missing = parse_signed_attachment_url(
        utf8_literal("https://cdn.discordapp.com/attachments/7/200/a.gif?format=webp"), 200,
        {target, sizeof(target)});
    check(duplicate.error == AttachmentUrlError::invalid_query &&
              duplicate.query_error == AttachmentQueryError::duplicate_authority &&
              animated.error == AttachmentUrlError::invalid_query &&
              animated.query_error == AttachmentQueryError::animated_parameter &&
              missing.error == AttachmentUrlError::invalid_query &&
              missing.query_error == AttachmentQueryError::missing_authority,
          "media diagnostics: authority and proxy-query failures stay rejected with categorical reasons");
    const auto valid_animated = parse_signed_attachment_url(
        utf8_literal("https://cdn.discordapp.com/attachments/7/200/a.gif?ex=2&is=1&hm="
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&animated=true"),
        200, {target, sizeof(target)});
    u8 proxy[kCdnMaximumTargetBytes]{};
    const auto canonical = valid_animated.ok()
                               ? write_media_proxy_target(
                                     {target, valid_animated.target_bytes}, valid_animated.host, 64, 64,
                                     CdnMediaFormat::source, {proxy, sizeof(proxy)}, true)
                               : CdnTargetResult{};
    check(valid_animated.ok() && valid_animated.expires_at_seconds == 2 &&
              valid_animated.issued_at_seconds == 1 && canonical.ok(),
          "media diagnostics: public-client animated boolean retains validated signed authority and proxy "
          "routing");
    const auto routed = parse_signed_attachment_url(
        utf8_literal("https://cdn.discordapp.com/attachments/7/200/a.png?backend=b2&ex=2&is=1&hm="
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        200, {target, sizeof(target)});
    const auto routed_proxy =
        routed.ok() ? write_media_proxy_target({target, routed.target_bytes}, routed.host, 64, 64,
                                               CdnMediaFormat::png, {proxy, sizeof(proxy)})
                    : CdnTargetResult{};
    const auto expected_proxy = utf8_literal(
        "/attachments/7/200/a.png?backend=b2&ex=2&is=1&hm="
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&width=64&height=64&format=png");
    check(routed.ok() && routed.expires_at_seconds == 2 && routed.issued_at_seconds == 1 &&
              routed_proxy.ok() && routed_proxy.host == CdnHost::media &&
              routed_proxy.bytes == expected_proxy.bytes &&
              bytes_equal(proxy, expected_proxy.data, expected_proxy.bytes),
          "media routing: confirmed B2 backend survives canonical proxy with unchanged signed authority");
    const auto repeated_backend = parse_signed_attachment_url(
        utf8_literal(
            "https://cdn.discordapp.com/attachments/7/200/a.png?backend=b2&backend=b2&ex=2&is=1&hm="
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        200, {target, sizeof(target)});
    const auto other_backend = parse_signed_attachment_url(
        utf8_literal("https://cdn.discordapp.com/attachments/7/200/a.png?backend=other&ex=2&is=1&hm="
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        200, {target, sizeof(target)});
    check(repeated_backend.error == AttachmentUrlError::invalid_query &&
              repeated_backend.query_error == AttachmentQueryError::backend_parameter &&
              other_backend.error == AttachmentUrlError::invalid_query &&
              other_backend.query_error == AttachmentQueryError::backend_parameter,
          "media routing: repeated and unrecognized backend remain rejected");
}
} // namespace
} // namespace maicord

extern "C" void maicord_media_visibility_tests_entry() noexcept {
    maicord::run_query_diagnostics_test();
    maicord::run_visibility_test();
    ExitProcess(maicord::failures ? 1 : 0);
}
