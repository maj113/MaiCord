#include "media_visibility_internal.hpp"
#include "../../support/ui_scene_fixtures.hpp"
#include "../../support/model_store_fixtures.hpp"
#include "maicord/media/core/presentation_policy.hpp"
#include "maicord/ui/text/regional_flag.hpp"
#include "maicord/ui/chat/search/avatar_media.hpp"
#include "maicord/render/media/inline_media.hpp"
#include "../../../src/app/account/profile.hpp"
#include "../../../src/app/chat/channel_authority.hpp"
#include "maicord/ui/chat/messages/forward_scene.hpp"
#include "../../support/test_output.hpp"

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
bool application_local_preview_key_current(const MediaAssetKey& key) noexcept {
    return application_forum_preview_key_current(key);
}
// The profile editor's preview card (a fixture while its test runs) and dialogs.
ProfileScene* fixture_editor_preview;
bool fixture_editor_frame_current;
ProfileScene* application_profile_editor_preview() noexcept {
    return fixture_editor_preview;
}
bool application_profile_editor_frame_current(const MediaAssetKey&) noexcept {
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
bool application_image_viewer_key_current(const MediaAssetKey& key) noexcept {
    return g_app.image_viewer.scene.active && media_asset_key_equal(key, g_app.image_viewer.key);
}
void application_image_viewer_request_failed(const MediaAssetKey&) noexcept {}
// The fixture does not select native video or submit a frame to the GPU.
bool application_video_ordinary(const MediaAssetKey&) noexcept {
    return false;
}
MediaAssetKey fixture_catalog_owner_key{};
bool fixture_catalog_owner_retained;
bool fixture_catalog_visible;
bool application_media_picker_preview_current(const MediaAssetKey& key, u64 scope, u32 revision) noexcept {
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

u32 failures;

} // namespace

namespace media_visibility_detail {

u32 attachment_requests;
u32 identity_requests, catalog_requests;
MediaAssetKey last_identity_key{};
u64 last_attachment_message;
u32 last_attachment_revision;
MediaAssetKey last_attachment_key{};

} // namespace media_visibility_detail

namespace {

MediaAssetKey watched_profile_key{}, last_profile_key{};
char last_profile_url[512];
u32 last_profile_url_bytes;
CdnImageType last_profile_type{};
u16 last_profile_proxy_width;

} // namespace

namespace media_visibility_detail {

bool observe_picker_requests;
bool picker_busy_flag_first;
u32 picker_emoji_requests;
MediaAssetKey picker_requested_keys[8];

void check(bool accepted, const char* label) noexcept {
    test_check(&failures, accepted, label);
}

} // namespace media_visibility_detail

namespace {

AccountMediaRequestResult busy_result() noexcept {
    AccountMediaRequestResult result{};
    result.state = AccountMediaRequestState::busy;
    return result;
}

} // namespace

namespace media_visibility_detail {

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

} // namespace media_visibility_detail

namespace {

u32 last_identity_size;

} // namespace

namespace media_visibility_detail {

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

} // namespace media_visibility_detail

namespace {

AccountMediaRequestResult WINAPI request_catalog(void*, const AccountMediaCatalogPreviewRequest*, u64,
                                                 u64) noexcept {
    ++catalog_requests;
    return busy_result();
}

} // namespace

namespace media_visibility_detail {

AccountMediaRequestResult WINAPI request_attachment(void*, const AccountMediaAttachmentRequest* request,
                                                    u64, u64) noexcept {
    ++attachment_requests;
    last_attachment_message = request->source_message_id;
    last_attachment_revision = request->source_message_revision;
    last_attachment_key = request->key;
    // Profile images by URL: the watched key's latest request.
    if (media_asset_key_equal(request->key, watched_profile_key) &&
        request->signed_url.bytes <= sizeof(last_profile_url)) {
        last_profile_key = request->key;
        last_profile_type = request->image_type;
        last_profile_proxy_width = request->proxy_width;
        last_profile_url_bytes = static_cast<u32>(request->signed_url.bytes);
        copy_bytes(last_profile_url, request->signed_url.data, last_profile_url_bytes);
    }
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

void run_animation_owner_test(
    VisibilityFixture& fixture, AccountUiRuntime& ui, ApplicationMediaPump& pump, MediaAssetKey key,
    AccountMediaFrameKind frame_kind) noexcept {
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
    u32& uploads = pump.uploads = 0;
    bool stop = false;
    const u32 before_requests = attachment_requests;
    pump.visibility_only = true;
    const bool observed = edited && pump.pass(clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, 0, &repaint).request_visible_attachment(
                                        fixture.records[1], fixture.rich_records[1].attachments[0], 64, 64, &stop);
    pump.visibility_only = false;
    const bool retired = observed && !animation_timeline_find(&g_app.animations.timeline, key).ok() &&
                         g_app.animations.pending_advance_count == 0 &&
                         attachment_requests == before_requests;
    const bool reacquired = retired && pump.pass(clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, 0, &repaint).request_visible_attachment(
                                           fixture.records[1], fixture.rich_records[1].attachments[0], 64,
                                           64, &stop);
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
    pump.uploads = 0;
    bool repaint = false, stop = false;
    const u32 before_hidden = identity_requests;
    for (u32 pass = 0; pass < 8; ++pass)
        valid = pump.pass(1'000, 1'100, 0, &repaint).request_visible_user_avatar_value(9, hash, &stop) &&
                valid;
    check(valid && !pump.avatar_key_is_current(avatar) && identity_requests == before_hidden,
          "avatar admission: grouped authors without a visible avatar never queue discarded decodes");
    char command_text[32]{};
    MessageComposer command_model{};
    MessageComposerProjectionContext command_projection{};
    command_projection.command = {avatar, 5};
    const bool composer_ready = message_composer_initialize(&command_model, {command_text, sizeof(command_text)}) &&
        message_composer_restore(&command_model, 7, utf8_literal("/test "), 6, 6) &&
        message_composer_scene_initialize_owned(&g_app.message_composer_scene) &&
        message_composer_scene_update(&g_app.message_composer_scene, &command_model,
            {0, 300, 600, 360}, true, 0, {}, &command_projection).ok();
    check(composer_ready && !g_app.popup_scene.active && pump.avatar_key_is_current(avatar),
          "command avatar: selected composer admits its bot after the discovery picker closes");
    command_projection.command = {};
    check(composer_ready && message_composer_scene_update(&g_app.message_composer_scene, &command_model,
              {0, 300, 600, 360}, true, 0, {}, &command_projection).ok() && !pump.avatar_key_is_current(avatar),
          "command avatar: clearing command presentation retires its media ownership");
    message_composer_scene_release(&g_app.message_composer_scene);
    for (u32 index = 0; index < g_app.message_scene.count; ++index)
        fixture.rows[index].grouped = grouped[index];
    check(pump.avatar_key_is_current(avatar) &&
              pump.pass(1'000, 1'100, 0, &repaint).request_visible_user_avatar_value(9, hash, &stop) &&
              identity_requests == before_hidden + 1,
          "avatar admission: matching visible author still admits its normal cold request");
    check(media_asset_key_equal(last_identity_key, avatar) &&
              last_identity_size == cdn_image_size(kCdnIdentityDisplayDips, g_app.renderer.dpi) &&
              last_identity_size <= kCdnIdentityDefaultSize && (last_identity_size & (last_identity_size - 1)) == 0,
          "avatar admission: the CDN size matches the 40-DIP avatar extent at the renderer DPI");
    // The atlas refused this avatar's upload (the decoded frame is gone): asking again decodes the same
    // frame again, so the key waits instead of costing a decode per pump pass, and only then retries.
    const u32 before_refusal = identity_requests;
    g_app.media_deferrals = {};
    media_capacity_defer(&g_app.media_deferrals, avatar, 32, 32, 1'000);
    for (u32 pass = 0; pass < 64; ++pass)
        valid = pump.pass(1'000 + pass, 1'100, 0, &repaint).request_visible_user_avatar_value(9, hash, &stop) &&
                valid;
    const bool held_back = identity_requests == before_refusal && g_app.media_deferrals.waited == 64;
    valid = pump.pass(2'100, 1'100, 0, &repaint).request_visible_user_avatar_value(9, hash, &stop) && valid;
    check(valid && held_back && identity_requests == before_refusal + 1 &&
              g_app.media_deferrals.released == 1 && !media_capacity_waiting_count(g_app.media_deferrals),
          "avatar admission: an atlas-refused key is held back for its backoff, then asks once");

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
    for (u32 pass = 0; pass < 8; ++pass) {
        valid = pump.pass(1'000, 1'100, 0, &repaint).request_visible_user_avatar_value(9, hash, &stop,
                                                       true) &&
                valid;
        valid =
            pump.pass(1'000, 1'100, 0, &repaint).request_visible_guild_icon(guild, &stop, true) && valid;
    }
    check(valid && identity_requests == before_static,
          "identity residency: decoded static avatars and icons override animated hash hints during forced "
          "reacquisition");
    valid =
        valid && media_atlas_set_decoded_animation(&fixture.atlas, avatar_resident.handle, true) &&
        media_atlas_set_decoded_animation(&fixture.atlas, icon_resident.handle, true) &&
        pump.pass(1'000, 1'100, 0, &repaint).request_visible_user_avatar_value(9, hash, &stop, true) &&
        pump.pass(1'000, 1'100, 0, &repaint).request_visible_guild_icon(guild, &stop, true);
    check(valid && identity_requests == before_static + 2,
          "identity residency: decoded multiframe residents still admit missing-timeline reacquisition");
}

// A visible timeline author without an avatar hash shows the CDN's default avatar: its key is current while that
// author is on screen, and the request is the profile card's by-URL fetch, never skipped for want of a hash.
void run_default_avatar_timeline_test(AccountUiRuntime& ui, ApplicationMediaPump& pump) noexcept {
    const auto key = default_avatar_key(default_avatar_index(9));
    bool valid = user_store_upsert(&ui.user_store, 9, utf8_literal("Author")).ok() &&
                 user_store_set_avatar(&ui.user_store, 9, Utf8View{});
    const auto* user = user_store_find(ui.user_store, 9);
    valid = valid && user && !user->avatar_hash_bytes && media_asset_key_valid(key);
    bool repaint = false, stop = false;
    pump.uploads = 0;
    last_profile_key = {};
    watched_profile_key = key;
    const bool current = pump.avatar_key_is_current(key) && pump.media_key_is_current(key);
    valid = valid && pump.pass(1'000, 1'100, 0, &repaint).request_visible_user_avatar_value(9, Utf8View{}, &stop);
    check(valid && current && media_asset_key_equal(last_profile_key, key) &&
              utf8_equal({last_profile_url, last_profile_url_bytes},
                         utf8_literal("https://cdn.discordapp.com/embed/avatars/0.png")),
          "default avatar: a timeline author without an avatar requests the CDN default avatar");
    last_profile_key = {};
    const u32 before_identity = identity_requests;
    valid = pump.pass(1'000, 1'100, 0, &repaint).request_visible_user_avatar_value(9, Utf8View{}, &stop);
    check(valid && media_asset_key_equal(last_profile_key, key) && identity_requests == before_identity,
          "default avatar: it is fetched by URL under the shared key, never as a hashed user avatar");
    watched_profile_key = {};
    user_store_set_avatar(&ui.user_store, 9, utf8_literal("a_0123456789abcdef0123456789abcdef"));
    // A user whose id picks another default shares nothing with this key.
    check(!media_asset_key_equal(default_avatar_key(default_avatar_index(9 + (1ull << 22))), key) &&
              is_default_avatar_key(key) && !is_default_avatar_key(media_asset_user_avatar(9, 1, false)),
          "default avatar: six shared keys, one per CDN picture");
}

} // namespace media_visibility_detail

namespace {

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

} // namespace

namespace media_visibility_detail {

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
    u32& uploads = pump.uploads = 0;
    bool repaint = false;
    const auto animated = media_asset_custom_emoji(981, true);
    const auto flag = media_asset_regional_flag(regional_flag_sequence(utf8_literal("🇸🇮")));
    uploads = kMessageMediaUploadBudget;
    const u32 before_other_admission = attachment_requests + identity_requests + catalog_requests;
    check(!pump.visibility_only && pump.pass(1'000, 1'100, 1'000, &repaint).scan_visible_media(1'000) &&
              !picker_emoji_requests && uploads == kMessageMediaUploadBudget &&
              attachment_requests + identity_requests + catalog_requests == before_other_admission,
          "picker emoji visibility: the shared scan stops admissions at the exhausted foreground budget");
    check(!pump.visibility_only && pump.pass(1'000, 1'100, 1'000, &repaint).request_picker_emoji() &&
              !picker_emoji_requests && uploads == kMessageMediaUploadBudget,
          "picker emoji visibility: a real exhausted upload budget admits no missing asset requests");
    uploads = 0;
    const float clipped_scroll = scene.projection->cells[0].rectangle.bottom - scene.body.top - 1;
    UiMediaPickerMediaSlot clipped{};
    check(ui_media_picker_scene_scroll(&scene, clipped_scroll) &&
              scene.projection->cells[0].source_index == 0 &&
              ui_media_picker_scene_media(scene, 0, &clipped) && !ui_rect_has_area(clipped.clip) &&
              pump.pass(1'000, 1'100, 1'000, &repaint).request_picker_emoji() &&
              !picker_emoji_requests && ui_media_picker_scene_scroll(&scene, -scene.scroll_dips),
          "picker emoji visibility: an exposed cell with entirely clipped inset art queues no image");
    const bool requested = pump.pass(1'000, 1'100, 1'000, &repaint).request_picker_emoji();
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
    g_app.renderer.inline_media = {inline_flags, 4};
    picker_emoji_requests = 0;
    uploads = 0;
    check(pump.pass(1'000, 1'000, 0, &repaint).request_inline_flags() && picker_emoji_requests == 1 &&
              media_asset_key_equal(picker_requested_keys[0], flag) &&
              !renderer_inline_flag_current(g_app.renderer, inline_flags[2]) &&
              !renderer_inline_flag_current(g_app.renderer, inline_flags[3]),
          "inline flags: exact live clipped placements dedupe one static asset; unrelated pointers "
          "are rejected before dereference");
    ++inline_flags[0].owner.handle.generation;
    inline_flags[1].layer = RendererInlineMediaLayer::base;
    check(pump.pass(1'000, 1'000, 0, &repaint).request_inline_flags() && picker_emoji_requests == 1,
          "inline flags: stale widget generation and wrong surface layer admit no requests");
    inline_flags[0].owner.handle = cell.widget;
    inline_flags[1].layer = RendererInlineMediaLayer::gallery;
    uploads = kMessageMediaUploadBudget;
    check(pump.pass(1'000, 1'000, 0, &repaint).request_inline_flags() && picker_emoji_requests == 1,
          "inline flags: exhausted foreground budget cannot admit another label asset");
    uploads = 0;
    pump.visibility_only = true;
    check(pump.pass(1'000, 1'000, 0, &repaint).request_inline_flags() && picker_emoji_requests == 1,
          "inline flags: visibility-only pass never sends static image requests");
    pump.visibility_only = false;
    ++source.query_generation;
    check(pump.pass(1'000, 1'000, 0, &repaint).request_inline_flags() && picker_emoji_requests == 1,
          "inline flags: replaced borrowed source rejects old label placements before admission");
    --source.query_generation;
    g_app.renderer.inline_media = previous_inline;
    picker_emoji_requests = 0;
    picker_busy_flag_first = true;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, scene.viewport, scene.panel) &&
              pump.pass(1'000, 1'100, 1'000, &repaint).request_picker_emoji() &&
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
    for (u32 i = 0; i < 8; ++i)
        retained &= pump.pass(1'000, 1'100, 1'000, &repaint).request_picker_emoji();
    check(retained && picker_emoji_requests == resident_requests,
          "picker emoji visibility: decoded still residents override animation hints without repeated "
          "fetches");
    check(media_atlas_set_decoded_animation(&atlas, custom_resident.handle, true) &&
              pump.pass(1'000, 1'100, 1'000, &repaint).request_picker_emoji() &&
              picker_emoji_requests == resident_requests + 1,
          "picker emoji visibility: decoder-confirmed animation admits missing-timeline reacquisition");
    const auto owned = animation_timeline_publish(&g_app.animations.timeline,
                                                  {animated, 1'000'000, 67, 3, 0, false}, 1'000);
    const u32 owned_requests = picker_emoji_requests;
    bool observed = owned.ok();
    for (u32 i = 0; i < 8; ++i)
        observed &= pump.pass(1'000, 1'100, 1'000, &repaint).request_picker_emoji();
    check(observed && picker_emoji_requests == owned_requests &&
              (g_app.animations.timeline.entries[owned.slot].flags & animation_timeline_entry_visible),
          "picker emoji visibility: owned animation stays visible through repeated pumps without transport "
          "churn");
    const u32 before_observation = picker_emoji_requests;
    pump.visibility_only = true;
    uploads = kMessageMediaUploadBudget;
    check(pump.pass(1'000, 1'100, 1'000, nullptr).request_picker_emoji() &&
              picker_emoji_requests == before_observation && uploads == kMessageMediaUploadBudget,
          "picker emoji visibility: exhausted-budget visibility pass performs no fetches");
    pump.visibility_only = false;
    ++source.revision;
    check(pump.pass(1'000, 1'100, 1'000, &repaint).request_picker_emoji() &&
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
    pump.uploads = 0;
    bool repaint = false;
    const u32 before_static = catalog_requests;
    for (u32 pass = 0; pass < 8; ++pass)
        valid = pump.pass(1'000, 1'100, 1'000, &repaint).request_catalog_previews() && valid;
    check(valid && (fixture_catalog_owner_key.flags & media_asset_flag_animated) &&
              catalog_requests == before_static,
          "catalog residency: decoder-proven still WebP never restarts from a provisional animation hint");
    valid = valid && media_atlas_set_decoded_animation(&fixture.atlas, resident.handle, true) &&
            pump.pass(1'000, 1'100, 1'000, &repaint).request_catalog_previews();
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
    for (u32 pass = 0; pass < 8; ++pass)
        valid = pump.pass(1'000, 1'100, 1'000, &repaint).request_catalog_previews() && valid;
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
              pump.pass(1'000, 1'100, 1'000, &repaint).request_catalog_previews() &&
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
    AvatarMedia media{};
    valid = valid && search_avatar_media_resolve(scene, 0, &media);
    check(valid && !user_store_find(g_app.account_ui->user_store, 770) &&
              pump.avatar_key_is_current(media.key),
          "search avatar: exact result bank owns a visible author absent from the conversation user cache");
    const u32 before = identity_requests;
    pump.uploads = 0;
    bool repaint = false, stop = false;
    if (valid)
        valid = pump.pass(1000, 1100, 0, &repaint).request_visible_user_avatar_value(media.key.id, media.hash, &stop, true);
    check(valid && identity_requests == before + 1 && media_asset_key_equal(last_identity_key, media.key),
          "search avatar: existing native identity lane receives the result's exact hash variant");
    scope.current = false;
    if (valid)
        valid = pump.pass(1000, 1100, 0, &repaint).request_visible_user_avatar_value(media.key.id, media.hash, &stop, true);
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
        UiTextOutput text_output{&text, nullptr};
        PaintStream paint{};
        const bool ok = ui_text_command_stream_initialize(&text, {commands, 256}) &&
                        ui_settings_scene_text_output(scene, kDefaultTheme, &text_output) && paint_stream_initialize(&paint) &&
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
    pump.uploads = 0;
    bool repaint = false;
    check(pump.pass(1000, 1100, 0, &repaint).request_settings_avatars() && identity_requests == before + 1 &&
              media_asset_key_equal(last_identity_key, key),
          "settings avatar: the account card requests the shared user-avatar key with its hash");
    avatars[0] = {};
    ++content.revision;
    valid = ui_settings_scene_update(&scene, &registry, {0, 0, 1280, 800});
    cursor = 0;
    check(valid && !ui_settings_scene_next_avatar(scene, &cursor, &extra) && !pump.media_key_is_current(key) &&
              pump.pass(1000, 1100, 0, &repaint).request_settings_avatars() && identity_requests == before + 1,
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
    check(pump.pass(1000, 1100, 0, &repaint).request_settings_avatars() && identity_requests == before + 2 &&
              media_asset_key_equal(last_identity_key, guild_key) && last_identity_size != 0,
          "settings avatar: the server identity row requests the shared guild icon key with its hash");
    // Audit log actors without an avatar hash show the default avatar,
    // fetched by its CDN path under the key every avatar list shares.
    const auto default_path = utf8_literal("embed/avatars/3.png");
    const auto default_key = default_avatar_key(3);
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
              pump.pass(1000, 1100, 0, &repaint).request_settings_avatars() &&
              attachment_requests == before_default + 1 && media_asset_key_equal(last_attachment_key, default_key),
          "settings avatar: a listed user without an avatar hash requests the default avatar by path");
    // Server Settings > Emoji and Stickers: the previews stay current under
    // opaque Settings, or their decoded frames are dropped and rows stay grey.
    const auto emoji_key = media_asset_custom_emoji(993, false);
    const auto sticker_key = media_asset_sticker(994, 1);
    avatars[0] = {};
    avatars[0].key = emoji_key;
    avatars[1] = {};
    avatars[1].key = sticker_key;
    presentation[1].flags = ui_settings_presentation_avatar;
    ++content.revision;
    valid = ui_settings_scene_update(&scene, &registry, {0, 0, 1280, 800});
    observe_picker_requests = true;
    picker_emoji_requests = 0;
    check(valid && pump.media_key_is_current(emoji_key) && pump.media_key_is_current(sticker_key) &&
              !pump.media_key_is_current(media_asset_custom_emoji(995, false)) &&
              pump.pass(1000, 1100, 0, &repaint).request_settings_avatars() && picker_emoji_requests == 2 &&
              media_asset_key_equal(picker_requested_keys[0], emoji_key) &&
              media_asset_key_equal(picker_requested_keys[1], sticker_key),
          "settings avatar: Server Settings emoji and sticker previews stay current and request their images");
    observe_picker_requests = false;
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

// The profile popout's Spotify card asks for its album art (a synthetic 640 px
// key) as JPEG from i.scdn.co; only the open card's images may claim
// decorative atlas space.
void run_profile_activity_art_test(ApplicationMediaPump& pump) noexcept {
    static u32 scratch[2048];
    static UserStore users{};
    auto* profile = static_cast<ApplicationProfile*>(
        VirtualAlloc(nullptr, sizeof(ApplicationProfile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!profile) {
        check(false, "profile activity art: profile allocation");
        return;
    }
    const auto presence = utf8_literal(R"({"activities":[{"type":2,"name":"Spotify","details":"Synthetic Song",)"
                                       R"("state":"Synthetic Artist","party":{"id":"spotify:4343"},)"
                                       R"("assets":{"large_image":"spotify:ab67616d0000b2730123456789abcdef01234567",)"
                                       R"("large_text":"Synthetic Album"}}]})");
    UserActivitySet set{};
    bool has = false;
    const auto scan = json_scan_structurals_avx1(presence, {scratch, 2048});
    bool valid = scan.ok() &&
                 user_activities_decode({presence, {scratch, scan.count}}, {presence, 0, JsonValueKind::object}, 4343,
                                        &set, &has) &&
                 has && user_activity_upsert(&users.activities, set);
    profile->data.user_id = 4343;
    profile->data.username.bytes = 4;
    copy_bytes(profile->data.username.data, "art4", 4);
    valid = valid && profile_scene_open(&profile->scene, &profile->data, nullptr, &users, 1, {900, 100, 940, 140},
                                        {0, 0, 1280, 800});
    MediaAssetKey art{};
    for (u32 i = 0; valid && i < profile->scene.part_count; ++i) {
        ProfileMedia media{};
        if (profile->scene.parts[i].media == ProfileMediaKind::activity_large &&
            profile_media_resolve(profile->scene, i, &media))
            art = media.key;
    }
    check(valid && media_asset_key_valid(art), "profile activity art: the popout card resolves its album art");
    auto* const previous = g_app.profile;
    g_app.profile = profile;
    pump.uploads = 0;
    bool repaint = false;
    last_profile_key = {};
    watched_profile_key = art;
    valid = valid && pump.pass(clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, 0, &repaint).scan_visible_media(0);
    check(valid && media_asset_key_equal(last_profile_key, art) &&
              utf8_equal({last_profile_url, last_profile_url_bytes},
                         utf8_literal("https://i.scdn.co/image/ab67616d0000b2730123456789abcdef01234567")) &&
              last_profile_type == CdnImageType::jpeg && last_profile_proxy_width == 0,
          "profile activity art: the visible card requests its album art as JPEG from i.scdn.co");
    const auto unrelated = media_asset_custom_emoji(77, false);
    check(pump.media_key_is_current(art) && pump.yield_decorative_media(art) == 0 &&
              pump.yield_decorative_media(unrelated) == 0,
          "profile activity art: without member rows nothing yields, and other media never claims ornaments");
    watched_profile_key = {};
    g_app.profile = previous;
    profile_scene_release(&profile->scene);
    user_activity_release(&users.activities);
    VirtualFree(profile, 0, MEM_RELEASE);
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
    pump.uploads = 0;
    bool repaint = false, stop = false;
    if (valid)
        valid = pump.pass(1000, 1100, 0, &repaint).request_visible_user_avatar_value(user.key.id, user.hash,
                                                       &stop, false);
    check(valid && identity_requests == before + 1 && media_asset_key_equal(last_identity_key, user.key),
          "quick switcher avatar: picker rows use the shared user-avatar request");
    g_app.renderer.quick_switcher_scene = previous;
    check(!pump.avatar_key_is_current(user.key), "quick switcher avatar: a closed picker admits nothing");
    ui_quick_switcher_scene_release(&scene);
}

} // namespace media_visibility_detail
} // namespace maicord

extern "C" void maicord_media_visibility_tests_entry() noexcept {
    maicord::run_query_diagnostics_test();
    maicord::run_visibility_test();
    ExitProcess(maicord::failures ? 1 : 0);
}

