#include "media_visibility_internal.hpp"
#include "../../support/model_store_fixtures.hpp"
#include "../../support/message_media_controller_fixture.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/render/media/inline_media.hpp"
#include "maicord/ui/chat/threads/browser_scene.hpp"
#include "../../../src/app/chat/channel_authority.hpp"
#include "maicord/ui/chat/messages/forward_scene.hpp"
#include "maicord/render/d3d11/renderer_presentation.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {

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
    pump.uploads = 0;
    bool repaint = false, stop = false;
    identity_requests_queue = true;
    valid = pump.pass(1000, 1100, 0, &repaint).request_forward_identities(&stop);
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
    valid = arrived.ok() && pump.pass(1000, 1100, 0, &repaint).request_forward_identities(&stop);
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

struct PreviewAdmissionPolicy {
    MessageMediaPolicyResult result;
    mutable MessageMediaOwner owner;
};
MessageMediaPolicyResult preview_admission_policy(const void* context,
                                                  const MessageMediaOwner& owner) noexcept {
    const auto& policy = *static_cast<const PreviewAdmissionPolicy*>(context);
    policy.owner = owner;
    return policy.result;
}

void run_search_media_test(VisibilityFixture& fixture, AccountUiRuntime& ui,
                           ApplicationMediaPump& pump) noexcept {
    MessageRecord record[1]{};
    u32 order[1]{};
    u8 text[64]{};
    MessageRichRecord rich_records[2]{};
    u16 rich_order[1]{};
    u8 rich_text[2048]{};
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
    outer.flags = kGatewayMediaExplicitFlag | kGatewayAttachmentSpoilerFlag;
    auto child = outer;
    child.width = child.height = 0;
    child.has_width = child.has_height = false;
    GatewayMessageEmbed embed{};
    embed.flags = kGatewayMediaExplicitFlag;
    embed.image.url = outer.url;
    embed.image.content_type = outer.content_type;
    embed.image.width = embed.image.height = 64;
    embed.image.has_width = embed.image.has_height = true;
    GatewayMessageEvent event{};
    event.id = 880;
    event.fields = gateway_message_field_attachments | gateway_message_field_embeds |
                   gateway_message_field_message_snapshots | gateway_message_field_message_reference;
    event.attachments = {&outer, 1};
    event.embeds = {&embed, 1};
    event.reference.type = 1;
    event.reference.channel_id = 99;
    event.reference.message_id = 990;
    event.snapshot.present = true;
    event.snapshot.fields = gateway_message_field_attachments | gateway_message_field_embeds;
    event.snapshot.attachments = {&child, 1};
    event.snapshot.embeds = {&embed, 1};
    const auto seeded = message_rich_store_apply(&rich, 880, 73, event);
    valid = valid && seeded.ok() && !seeded.storage_truncated &&
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
        pump.uploads = 0;
        bool repaint = false, stop = false;
        const u32 requests = attachment_requests;
        check(pump.pass(1000, 1100, 0, &repaint).request_visible_attachment(record[0], snapshot->attachments[0], 80, 40, &stop, &rich, true) &&
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
        check(pump.pass(1000, 1100, 0, &repaint).request_visible_attachment(record[0], snapshot->attachments[0], 80, 40, &stop, &rich, true) &&
                  attachment_requests == requests + 1 && last_attachment_message == 880 &&
                  last_attachment_revision == 73 && media_asset_search_snapshot(last_attachment_key),
              "search media: actual shared media lane receives exact accepted publication and child key");
        PreviewAdmissionPolicy policy{{MessageExplicitFilter::block, true}, {}};
        MessageMediaPolicy binding{&policy, preview_admission_policy, 1};
        ui_search_scene_set_media_policy(&scene, binding);
        content.tabs = true;
        content.tab = content.result_tab = 1;
        ++content.revision;
        const bool grid =
            ui_search_scene_update(&scene, &source, &content, {0, 0, 1200, 800}, {0, 0, 1200, 48}) &&
            ui_search_scene_visible_tile_count(scene) == 4 && pump.search_media_visible();
        const u32 before_policy = attachment_requests;
        check(grid && pump.pass(1000, 1100, 0, &repaint).request_search_previews(&stop) && !stop &&
                  attachment_requests == before_policy && policy.owner.channel_id == 88 &&
                  policy.owner.forwarded && !policy.owner.author_id &&
                  policy.owner.source_channel_id == 99 && policy.owner.source_message_id == 990,
              "search preview admission: blocked outer and forwarded attachment/embed tiles fetch nothing "
              "using destination/source policy");
        policy.result = {MessageExplicitFilter::blur, false};
        ++binding.revision;
        ui_search_scene_set_media_policy(&scene, binding);
        check(pump.pass(1000, 1100, 0, &repaint).request_visible_attachment(record[0], snapshot->attachments[0], 80, 40, &stop, &rich, true) &&
                  pump.pass(1000, 1100, 0, &repaint).request_visible_embed_media(record[0], *snapshot, 0, 64, 64, &stop, false, &rich) &&
                  attachment_requests == before_policy + 2 &&
                  media_asset_search_snapshot(last_attachment_key),
              "search preview admission: explicit blur still decodes attachment and embed previews before "
              "reveal eligibility");
        policy.result = {MessageExplicitFilter::show, false};
        ++binding.revision;
        ui_search_scene_set_media_policy(&scene, binding);
        check(pump.pass(1000, 1100, 0, &repaint).request_visible_attachment(record[0], snapshot->attachments[0], 80, 40, &stop, &rich, true) &&
                  attachment_requests == before_policy + 3,
              "search preview admission: spoiler previews remain fetchable");
        auto* native = ui_search_scene_message_scene(&scene);
        const auto saved_plugin_query = g_ui_plugin_render_query;
        const bool saved_plugin_decorations = native && native->plugin_decorations;
        if (native)
            native->plugin_decorations = true;
        g_ui_plugin_render_query = +[](u64 id, u32 revision, UiPluginRenderDecoration* output) noexcept {
            if (id != 880 || revision != 73)
                return false;
            *output = {};
            output->media = ui_plugin_media_hide;
            return true;
        };
        check(grid && native && pump.pass(1000, 1100, 0, &repaint).request_search_previews(&stop) &&
                  !stop && attachment_requests == before_policy + 3,
              "search preview admission: plugin-hidden previews fetch no attachment or embed even from "
              "visible tiles");
        g_ui_plugin_render_query = saved_plugin_query;
        if (native)
            native->plugin_decorations = saved_plugin_decorations;
        const u32 before_retirement = attachment_requests;
        g_app.renderer.search_scene = nullptr;
        const bool absent =
            pump.pass(1000, 1100, 0, &repaint).request_visible_attachment(record[0], snapshot->attachments[0], 80, 40, &stop, &rich, true);
        g_app.renderer.search_scene = &scene;
        fixture_search_current = false;
        const bool retired = pump.pass(1000, 1100, 0, &repaint).request_visible_embed_media(record[0], *snapshot, 0, 64, 64, &stop, false, &rich);
        fixture_search_current = true;
        check(
            absent && retired && attachment_requests == before_retirement,
            "search preview admission: absent or retired Search cannot borrow timeline policy or reveals");
        ui_search_scene_set_media_policy(&scene, {});
        content.tabs = false;
        ++content.revision;
        check(ui_search_scene_update(&scene, &source, &content, {0, 0, 1200, 800}, {0, 0, 1200, 48}),
              "search preview admission: fixture restores ordinary list policy");
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
        check(pump.pass(1000, 1100, 1000000, &repaint).request_search_decorations(&stop) &&
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
    for (u32 i = 0; i < 8; ++i) application_media_posters_presented(g_app, true, i, i + 1);
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

} // namespace

namespace media_visibility_detail {

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
        valid = pump.pump_visible_static_media(&repaint);
        check(valid && attachment_requests == 2 &&
                  (fixture.animations[visible.slot].flags & animation_timeline_entry_visible) &&
                  fixture.animations[visible.slot].due_at_us == deadline,
              "media visibility: repeated saturation keeps animation cadence and hidden pause stable");
        u32& uploads = pump.uploads = kMessageMediaUploadBudget;
        pump.visibility_only = true;
        animation_timeline_begin_visibility(&g_app.animations.timeline);
        valid = pump.pass(clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, 0, nullptr).scan_visible_media(0);
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
                pump.uploads = 0;
                bool static_stop = false;
                check(media_atlas_set_decoded_animation(&fixture.atlas, decoded_resident.handle, false) &&
                          !media_atlas_decoded_animation(fixture.atlas, decoded_resident.handle) &&
                          pump.pass(clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000, 0, &repaint).request_visible_attachment(
                              fixture.records[1], fixture.rich_records[1].attachments[0], 64, 64, &static_stop) &&
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
        run_default_avatar_timeline_test(*ui, pump);
        run_catalog_residency_test(fixture, pump);
        run_picker_emoji_visibility_test(pump);
        run_search_avatar_test(pump);
        run_settings_avatar_test(pump);
        run_profile_editor_preview_test(pump);
        run_profile_activity_art_test(pump);
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
        const u32 attachments = attachment_requests, identities = identity_requests;
        const auto covered = application_pump_covered_media(&g_app);
        const auto& chat_entry = fixture.animations[chat.slot];
        const auto& footer_entry = fixture.animations[footer.slot];
        check(chat.ok() && footer.ok() && covered.ok && !covered.text_changed && !covered.history_requested &&
                  !(chat_entry.flags & animation_timeline_entry_visible) && chat_entry.due_at_us == 0 &&
                  chat_entry.paused_remaining_us > 4'000'000 && chat_entry.paused_remaining_us <= 5'000'000 &&
                  !(footer_entry.flags & animation_timeline_entry_visible) &&
                  g_app.animations.timeline.counters.due_advances == advances &&
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
    check(valid_animated.ok() && valid_animated.expires_at_seconds == 2 && canonical.ok(),
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
    check(routed.ok() && routed.expires_at_seconds == 2 &&
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

} // namespace media_visibility_detail
} // namespace maicord
