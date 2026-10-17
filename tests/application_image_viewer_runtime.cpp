// Shipping image host with independent accepted Search/live banks. Renderer,
// Search authority and Save boundaries are deterministic; no network is used.
#include "../src/app/application_image_viewer.cpp"
#include "maicord/core/memory.hpp"
#include "../src/app/application_media_context.hpp"
#include "test_output.hpp"

namespace maicord::image_viewer_runtime_test {
using namespace app_detail;
u32 failures, saves, suspends, rebuilds;
u32 publication = 1000;
bool panel = true, allowed = true, close_on_save;
u8 account_marker;
MessageStore bank;
MessageRichStore rich;
MessageRecord records[4], live_records[4];
u32 order[4], live_order[4];
u8 text[1024], live_text[1024], rich_text[8192];
MessageRichRecord rich_records[8];
u16 rich_order[4];
GuildRecord guilds[1];
ChannelRecord channels[2];
RoleRecord roles[1];
PermissionOverwriteRecord overwrites[2];
u16 guild_order[1], channel_order[2];
u8 names[1024];
ApplicationSearchSourceTicket saved_ticket{};
u64 saved_attachment;
bool saved_snapshot;
u32 opened_urls, opened_bytes;
char opened_url[256];
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
u32 confirmations, media_refreshes;
MessageExplicitFilter media_filter = MessageExplicitFilter::show;
bool media_reveal_allowed = true;
MessageMediaOwner queried_owner{};
MessageTimeline search_timeline{};
UiVariableList search_list{};
u32 search_heights[4]{};
u64 search_fenwick[4]{};
UiSearchSource search_source{};
UiSearchContent search_content{};
UiSearchScene search_scene{};
MessageMediaPolicyResult query_media(const void*, const MessageMediaOwner& owner) noexcept {
    queried_owner = owner;
    return {media_filter, media_reveal_allowed};
}
bool publish_search() noexcept {
    if (!ui_variable_list_initialize(&search_list, {search_heights, 4}, {search_fenwick, 4}, bank.count,
                                     ui_extent_units(57)) ||
        !message_timeline_initialize(&search_timeline, &bank, &search_list, ui_extent_units(57),
                                     ui_extent_units(900), false) ||
        !message_timeline_bind_rich_store(&search_timeline, &rich))
        return false;
    search_source.count = bank.count;
    search_source.revision = publication;
    search_source.rich_timeline = &search_timeline;
    search_source.current = +[](void*) noexcept { return panel; };
    search_source.resolve = +[](void*, u32 index, UiSearchRow* row) noexcept {
        if (index || !row)
            return false;
        *row = {101, 800, utf8_literal("Fixture author"), utf8_literal("#fixture"), {}, {}, true, true};
        return true;
    };
    search_content.edit.active = search_content.panel = true;
    search_content.phase = UiSearchPhase::ready;
    search_content.revision = publication;
    g_app.renderer.search_scene = &search_scene;
    return ui_search_scene_set_media_policy(&search_scene, {nullptr, query_media, publication}) &&
           ui_search_scene_update(&search_scene, &search_source, &search_content, {0, 0, 1200, 900},
                                  {0, 0, 1200, 48});
}
bool seed() noexcept {
    if (!message_store_initialize(&bank, {records, 4}, {order, 4}, {text, sizeof(text)}) ||
        !message_rich_store_initialize(&rich, {rich_records, 8}, {rich_order, 4}, {rich_text, sizeof(rich_text)})) return false;
    auto inserted = message_store_upsert(&bank, {800, 101, 4, 1000, 0, utf8_literal("Search source")});
    if (!inserted.ok()) return false;
    records[inserted.handle.slot].revision = publication;
    GatewayMessageAttachment attachment{};
    attachment.id = 900; attachment.size = 17; attachment.filename = utf8_literal("original.png");
    attachment.url = utf8_literal("https://cdn.discordapp.com/attachments/101/900/original.png");
    attachment.width = 640; attachment.height = 480; attachment.has_width = attachment.has_height = true;
    GatewayMessageAttachment child = attachment;
    child.id = 901; child.filename = utf8_literal("forwarded.png");
    child.url = utf8_literal("https://cdn.discordapp.com/attachments/777/901/forwarded.png");
    GatewayMessageEmbed embed{};
    embed.image.url = utf8_literal("https://cdn.discordapp.com/attachments/777/902/preview.png");
    embed.image.width = 320; embed.image.height = 240;
    embed.image.has_width = embed.image.has_height = true;
    embed.thumbnail.url = utf8_literal("https://cdn.discordapp.com/attachments/777/903/thumbnail.png");
    embed.thumbnail.width = 80; embed.thumbnail.height = 64;
    embed.thumbnail.has_width = embed.thumbnail.has_height = true;
    GatewayMessageEvent event{}; event.id = 800;
    event.fields = gateway_message_field_attachments | gateway_message_field_message_reference | gateway_message_field_message_snapshots;
    event.attachments = {&attachment, 1}; event.reference.type = 1;
    event.reference.channel_id = 777; event.reference.message_id = 888;
    event.snapshot.present = true; event.snapshot.fields = gateway_message_field_attachments | gateway_message_field_embeds;
    event.snapshot.attachments = {&child, 1}; event.snapshot.embeds = {&embed, 1};
    return message_rich_store_apply(&rich, 800, publication, event).ok() && publish_search();
}
// The same source message as a Tenor-style GIFV link: a looping MP4 shown as an image.
bool seed_gifv() noexcept {
    if (!message_store_initialize(&bank, {records, 4}, {order, 4}, {text, sizeof(text)}) ||
        !message_rich_store_initialize(&rich, {rich_records, 8}, {rich_order, 4}, {rich_text, sizeof(rich_text)}))
        return false;
    auto inserted = message_store_upsert(&bank, {800, 101, 4, 1000, 0, utf8_literal("https://tenor.com/view/wave")});
    if (!inserted.ok()) return false;
    records[inserted.handle.slot].revision = publication;
    GatewayMessageEmbed gif{};
    gif.type = GatewayMessageEmbedType::gifv;
    gif.url = utf8_literal("https://tenor.com/view/wave");
    gif.thumbnail.url = utf8_literal("https://media.tenor.com/wave.png");
    gif.thumbnail.content_type = utf8_literal("image/png");
    gif.video.url = utf8_literal("https://media.tenor.com/wave.mp4");
    gif.video.proxy_url = utf8_literal("https://images-ext-1.discordapp.net/external/wave.mp4");
    gif.video.content_type = utf8_literal("video/mp4");
    gif.thumbnail.width = gif.video.width = 498;
    gif.thumbnail.height = gif.video.height = 280;
    gif.thumbnail.has_width = gif.thumbnail.has_height = gif.video.has_width = gif.video.has_height = true;
    GatewayMessageEvent event{}; event.id = 800;
    event.fields = gateway_message_field_embeds; event.embeds = {&gif, 1};
    return message_rich_store_apply(&rich, 800, publication, event).ok() && publish_search();
}
bool seed_authority() noexcept {
    auto& state = g_app.account_ui->chat_state;
    constexpr u64 rights = kPermissionViewChannel | (1ull << 16);
    if (!(chat_state_store_initialize(&state,{guilds,1},{guild_order,1},{channels,2},{channel_order,2},{names,sizeof(names)}) &&
        chat_state_store_bind_permissions(&state,{roles,1},{overwrites,2}) &&
        chat_state_store_upsert_guild(&state,{7,utf8_literal("Fixture server"),GuildMutationKind::create,4}).ok() &&
        chat_state_store_upsert_role(&state,{7,7,rights,utf8_literal("everyone")}) &&
        chat_state_store_set_self_roles(&state,7,4,3,{}) &&
        chat_state_store_upsert_channel(&state,{101,7,0,utf8_literal("Search channel"),0,0,0}).ok() &&
        chat_state_store_upsert_channel(&state,{999,7,0,utf8_literal("Live channel"),0,0,0}).ok())) return false;
    chat_state_store_recompute_guild_permissions(&state,7);
    return application_channel_readable(g_app,101) && application_channel_readable(g_app,999);
}
void check_media_context() noexcept {
    ChatStateStore state{};
    GuildRecord context_guilds[1]{};
    ChannelRecord context_channels[3]{};
    u16 context_guild_order[1]{}, context_channel_order[3]{};
    u8 context_names[128]{};
    RelationshipRecord friends[1]{};
    RelationshipStore relationships{};
    MessageStore live{}, accepted{};
    MessageRecord live_rows[1]{}, accepted_rows[1]{};
    u32 live_indices[1]{}, accepted_indices[1]{};
    u8 live_bytes[8]{}, accepted_bytes[8]{};
    const bool ready =
        chat_state_store_initialize(&state, {context_guilds, 1}, {context_guild_order, 1},
                                    {context_channels, 3}, {context_channel_order, 3},
                                    {context_names, 128}) &&
        chat_state_store_upsert_channel(&state, {10, 0, 0, utf8_literal("DM"), 0, 0, 1}).ok() &&
        chat_state_store_upsert_channel(&state, {11, 0, 0, utf8_literal("Group"), 0, 0, 3}).ok() &&
        chat_state_store_upsert_channel(&state, {12, 7, 0, utf8_literal("Guild"), 0, 0, 0}).ok() &&
        relationship_store_initialize(&relationships, {friends, 1}) &&
        message_store_initialize(&live, {live_rows, 1}, {live_indices, 1}, {live_bytes, 8}) &&
        message_store_initialize(&accepted, {accepted_rows, 1}, {accepted_indices, 1}, {accepted_bytes, 8});
    RelationshipRecord friend_record{};
    friend_record.user_id = 4;
    friend_record.type = RelationshipType::friend_;
    check(ready && relationship_store_upsert(&relationships, friend_record).ok(),
          "media context fixtures initialize accepted ownership stores");
    const auto resolve = [&](MessageMediaOwner owner) noexcept {
        return application_media_context_resolve(owner, 3, &state, &relationships, &live, &accepted);
    };
    check(resolve({10, 4}).context == MessageMediaContext::friend_dm &&
              resolve({11, 4}).context == MessageMediaContext::friend_dm &&
              resolve({11, 5}).context == MessageMediaContext::other_dm,
          "DM and group DM policy context follows the actual author's current friendship");
    check(resolve({12, 4}).context == MessageMediaContext::guild &&
              resolve({99, 4}).context == MessageMediaContext::other_dm && resolve({10, 3}).self_authored,
          "guild context wins over friendship, unknown channels stay conservative, and self identity is "
          "explicit");
    check(!resolve({10, 3, 77, 80, true}).self_authored && resolve({10, 3, 77, 80, true}).author_id == 0,
          "uncached forwards never inherit their self-authored forwarder's identity");
    check(message_store_upsert(&live, {80, 78, 3, 0, 0, {}}).ok() &&
              message_store_upsert(&accepted, {80, 77, 4, 0, 0, {}}).ok() &&
              resolve({10, 3, 77, 80, true}).context == MessageMediaContext::friend_dm &&
              resolve({10, 3, 77, 80, true}).author_id == 4,
          "forwarded source channel mismatch in live cache falls through to the validated accepted Search "
          "bank");
    live_rows[0].channel_id = 77;
    check(resolve({11, 4, 77, 80, true}).self_authored && resolve({11, 4, 77, 80, true}).author_id == 3,
          "validated live source takes precedence and can establish the forwarded author's self identity");
    live_rows[0].author_id = 5;
    check(resolve({11, 4, 77, 80, true}).context == MessageMediaContext::other_dm &&
              !resolve({11, 4, 77, 80, true}).self_authored &&
              resolve({12, 4, 77, 80, true}).context == MessageMediaContext::guild,
          "nonfriend forwarded author remains other DM while the destination guild still wins");
}
void run() noexcept {
    check_media_context();
    g_app.account_ui = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_app.window = CreateWindowExW(0, L"STATIC", L"Search image fixture", WS_POPUP, 0, 0, 1200, 900, nullptr, nullptr, nullptr, nullptr);
    check(g_app.account_ui && g_app.window, "Search viewer fixture owns a hidden native account window");
    if (!g_app.account_ui || !g_app.window) return;
    g_app.account.instance = &account_marker; g_app.account.status.profile_id = 4; g_app.plugin_account_scope_serial = 7;
    g_app.account.status.running = true;
    g_app.account_ui->chat_state.self_user_id = 3; g_app.account_ui->active_channel_id = 999;
    check(seed_authority(), "Search viewer source has real known channel metadata and resolved role authority independently of live timeline");
    g_app.renderer.dpi = 96; g_app.renderer.width = 1200; g_app.renderer.height = 900;
    check(message_store_initialize(&g_app.message_view.store, {live_records, 4}, {live_order, 4}, {live_text, sizeof(live_text)}) &&
        message_store_upsert(&g_app.message_view.store, {800, 999, 3, 1000, 0, utf8_literal("Different live source")}).ok() && seed(),
        "Search viewer fixture retains same message id in an independent live and accepted page bank");
    const UiRect preview{10, 10, 330, 250};
    ApplicationSearchSourceTicket ticket{800, publication, 0};
    g_app.renderer.search_scene = nullptr;
    check(!application_image_viewer_open_search(ticket, MessageSceneRichItemKind::attachment, 0, false, preview) &&
              !g_app.image_viewer.scene.active,
          "Search activation cannot borrow the selected conversation's policy or reveal memory");
    g_app.renderer.search_scene = &search_scene;
    auto* native_media = ui_search_scene_message_scene(&search_scene);
    auto* outer = const_cast<MessageRichRecord*>(message_rich_store_find(rich, 800));
    auto* child = const_cast<MessageRichRecord*>(message_rich_store_snapshot(rich, 800));
    check(native_media && outer && child, "Search image activation uses its actual native media scene");
    if (!native_media || !outer || !child)
        return;
    outer->attachments[0].flags |= kGatewayMediaExplicitFlag;
    child->attachments[0].flags |= kGatewayMediaExplicitFlag;
    media_filter = MessageExplicitFilter::block;
    const auto activate_outer = [&]() noexcept {
        return application_image_viewer_open_search(ticket, MessageSceneRichItemKind::attachment, 0, false,
                                                    preview);
    };
    check(activate_outer() && !g_app.image_viewer.scene.active && !native_media->media_reveals.count &&
              !confirmations,
          "blocked image activation neither opens nor reveals nor requests age confirmation");
    media_filter = MessageExplicitFilter::blur;
    media_reveal_allowed = false;
    check(activate_outer() && !g_app.image_viewer.scene.active && confirmations == 1 &&
              !native_media->media_reveals.count && queried_owner.forwarded &&
              queried_owner.author_id == 0 && queried_owner.channel_id == 101 &&
              queried_owner.source_channel_id == 777 && queried_owner.source_message_id == 888,
          "age gated image uses outer forwarding ownership and requests honest native confirmation before "
          "reveal");
    media_reveal_allowed = true;
    const u64 outer_key = message_media_reveal_key(800, MessageMediaSource::attachment, 0);
    const u64 child_key = message_media_reveal_key(800, MessageMediaSource::attachment, 0, true);
    check(activate_outer() && !g_app.image_viewer.scene.active &&
              message_media_revealed(native_media->media_reveals, outer_key) &&
              !message_media_revealed(native_media->media_reveals, child_key) && media_refreshes == 0 &&
              activate_outer() && g_app.image_viewer.scene.active,
          "Search image reveals stay scoped to native Search without refreshing unrelated timeline text");
    media_filter = MessageExplicitFilter::block;
    check(application_image_viewer_synchronize() && !g_app.image_viewer.scene.active,
          "a newly blocked policy closes an already open image viewer");
    media_filter = MessageExplicitFilter::blur;
    check(application_image_viewer_open_search(ticket, MessageSceneRichItemKind::attachment, 0, true,
                                               preview) &&
              !g_app.image_viewer.scene.active &&
              message_media_revealed(native_media->media_reveals, child_key),
          "forwarded child activation reveals its own scoped key without opening");
    outer->attachments[0].flags = 0;
    native_media->media_reveals = {};
    native_media->plugin_decorations = true;
    media_reveal_allowed = false;
    g_ui_plugin_render_query = +[](u64, u32, UiPluginRenderDecoration* output) noexcept {
        *output = {};
        output->media = ui_plugin_media_hide;
        return true;
    };
    check(activate_outer() && !g_app.image_viewer.scene.active && native_media->media_reveals.count == 0 &&
              confirmations == 1,
          "plugin hidden image consumes activation without reveal or age confirmation");
    g_ui_plugin_render_query = +[](u64, u32, UiPluginRenderDecoration* output) noexcept {
        *output = {};
        output->media = ui_plugin_media_blur;
        return true;
    };
    check(activate_outer() && !g_app.image_viewer.scene.active &&
              message_media_revealed(native_media->media_reveals, outer_key) && confirmations == 1,
          "plugin-only blur reveals normally without requesting explicit-content age confirmation");
    g_ui_plugin_render_query = nullptr;
    native_media->plugin_decorations = false;
    media_reveal_allowed = true;
    outer->attachments[0].flags = child->attachments[0].flags = 0;
    native_media->media_reveals = {};
    media_filter = MessageExplicitFilter::show;
    auto retired = ticket; --retired.publication_revision;
    check(!application_image_viewer_open_search(retired, MessageSceneRichItemKind::attachment, 0, false, preview) &&
        !application_image_viewer_open_search(ticket, MessageSceneRichItemKind::attachment, 1, false, preview),
        "Search viewer requires exact publication and rich attachment index");
    check(application_image_viewer_open_search(ticket, MessageSceneRichItemKind::attachment, 0, false, preview) &&
        g_app.image_viewer.scene.active && g_app.image_viewer.search_result && g_app.image_viewer.channel_id == 101 &&
        g_app.image_viewer.message_revision == publication && g_app.image_viewer.attachment_id == 900 &&
        media_asset_search(g_app.image_viewer.key) && media_asset_search_revision(g_app.image_viewer.key) == publication &&
        !media_asset_search_snapshot(g_app.image_viewer.key) && !media_asset_search_snapshot(g_app.image_viewer.fallback) &&
        !media_asset_key_equal(g_app.image_viewer.key, media_asset_attachment(900, MediaAttachmentFormat::png, MediaResolutionTier::px2048)) &&
        g_app.image_viewer.scene.can_save && panel && suspends,
        "Search viewer opens authorized accepted attachment with scoped decode key without selecting or publishing it into live timeline");
    ApplicationSearchSourceView source{};
    check(application_image_viewer_source(&source) && source.store == &bank && source.message->channel_id == 101 && source.rich_store == &rich,
        "Search viewer high resolution requests borrow the accepted page source");
    activate(1); activate(2);
    check(g_app.image_viewer.scene.zoom == 1.f, "Search viewer keeps native zoom and reset behavior");
    application_image_viewer_close();
    auto* pending = const_cast<MessageRichRecord*>(message_rich_store_find(rich, 800));
    if (pending && pending->attachment_count) {
        pending->attachments[0].width = pending->attachments[0].height = 0;
        pending->attachments[0].state = static_cast<u8>(pending->attachments[0].state & ~3u);
    }
    check(application_image_viewer_open_search(ticket, MessageSceneRichItemKind::attachment, 0, false, preview) &&
        g_app.image_viewer.scene.pixels.width == ui_rect_width(preview) && g_app.image_viewer.scene.pixels.height == ui_rect_height(preview),
        "Search viewer opens an authoritative pending preview using its painted aspect before dimensions decode");
    bool dimensions_changed = false;
    check(message_rich_store_set_attachment_dimensions(&rich, 800, publication, 900, 800, 400, &dimensions_changed) &&
        dimensions_changed && application_image_viewer_synchronize() && g_app.image_viewer.scene.pixels.width == 800 &&
        g_app.image_viewer.scene.pixels.height == 400 && media_asset_search_revision(g_app.image_viewer.key) == publication,
        "Search viewer adopts decoded dimensions only from the same accepted source without changing media identity");
    activate(4);
    check(saves == 1 && saved_ticket.publication_revision == publication && saved_attachment == 900 && !saved_snapshot,
        "Search viewer Save dispatches exact source ticket and attachment rather than displayed texture");
    application_image_viewer_close();
    check(application_image_viewer_open_search(ticket, MessageSceneRichItemKind::attachment, 0, true, preview) &&
        g_app.image_viewer.attachment_id == 901 && g_app.image_viewer.attachment_snapshot && g_app.image_viewer.scene.can_save,
        "Search viewer independently opens actual forwarded child attachment");
    check(media_asset_search_snapshot(g_app.image_viewer.key) && media_asset_search_snapshot(g_app.image_viewer.fallback),
        "Search viewer forwarded attachment decode and fallback keys retain exact child content scope");
    {
        auto& viewer = g_app.image_viewer;
        check(viewer.item_count == 2 && viewer.item_index == 0 && viewer.scene.item_count == 2 &&
            viewer.items[1].kind == MessageSceneRichItemKind::embed && !viewer.items[1].video,
            "Search viewer gallery collects the forwarded attachment and embed media of one message");
        MediaAssetKey keys[3]{};
        check(application_image_viewer_request_keys(keys, 3) == 2 && media_asset_key_equal(keys[0], viewer.key) &&
            media_asset_key_equal(keys[1], viewer.items[1].key) && media_asset_embed_snapshot(keys[1]) &&
            application_image_viewer_key_current(keys[1]) &&
            !application_image_viewer_key_current(media_asset_attachment(902, MediaAttachmentFormat::png, MediaResolutionTier::px2048)),
            "Search viewer prefetches only its bounded neighbour with exact content scope");
        activate(ui_image_viewer_zoom_in);
        application_image_viewer_message(WM_KEYDOWN, VK_RIGHT, 0);
        check(viewer.item_index == 0 && viewer.scene.zoom > 1,
            "Search viewer arrows pan a zoomed image instead of browsing");
        activate(ui_image_viewer_fit);
        application_image_viewer_message(WM_KEYDOWN, VK_RIGHT, 0);
        check(viewer.item_index == 1 && application_image_viewer_synchronize() && !viewer.attachment_id &&
            !viewer.scene.can_save && viewer.scene.zoom == 1 && viewer.scene.pixels.width == 320 &&
            viewer.scene.pixels.height == 240 && media_asset_key_equal(viewer.key, viewer.items[1].key) &&
            ui_rect_has_area(ui_image_viewer_button(viewer.scene, ui_image_viewer_previous)),
            "Search viewer Right browses to embed media with its own size, reset zoom and no attachment Save");
        application_image_viewer_message(WM_KEYDOWN, VK_RIGHT, 0);
        check(viewer.item_index == 0 && viewer.attachment_id == 901 && application_image_viewer_synchronize() &&
            viewer.scene.can_save, "Search viewer Right wraps to the first item and restores its Save authority");
        application_image_viewer_message(WM_KEYDOWN, VK_LEFT, 0);
        check(viewer.item_index == 1, "Search viewer Left wraps backwards");
        activate(ui_image_viewer_previous);
        check(viewer.item_index == 0 && viewer.attachment_id == 901, "Search viewer chevrons browse the gallery");
        const u32 before_open = opened_urls;
        activate(ui_image_viewer_open_original);
        check(opened_urls == before_open + 1 && utf8_equal({opened_url, opened_bytes},
            utf8_literal("https://cdn.discordapp.com/attachments/777/901/forwarded.png")),
            "Search viewer Open original routes the exact original URL through the safe link opener");
    }
    close_on_save = true;
    activate(4); g_app.image_viewer.held_key = 'S';
    check(saves == 2 && saved_attachment == 901 && saved_snapshot && !g_app.image_viewer.scene.active &&
        application_image_viewer_message(WM_KEYUP, 'S', 0) && !g_app.image_viewer.held_key,
        "Search viewer Save overlay closure retains exact forwarded source and consumes its closing key tail");
    close_on_save = false;
    check(application_image_viewer_open_search(ticket, MessageSceneRichItemKind::embed, 0, true, preview) &&
        !g_app.image_viewer.attachment_id && !g_app.image_viewer.scene.can_save &&
        media_asset_search(g_app.image_viewer.key) && media_asset_embed_snapshot(g_app.image_viewer.key),
        "Search viewer embed previews retain snapshot scope while original-file Save stays unavailable");
    const u32 before_embed = saves; activate(4);
    check(saves == before_embed, "Search viewer never infers attachment identity from embed texture");
    application_image_viewer_close();
    check(application_image_viewer_open_search(ticket, MessageSceneRichItemKind::embed, 0, true, preview, MediaEmbedResource::thumbnail) &&
        media_asset_embed_resource(g_app.image_viewer.key) == MediaEmbedResource::thumbnail &&
        g_app.image_viewer.scene.pixels.width == 80 && g_app.image_viewer.scene.pixels.height == 64 && !g_app.image_viewer.scene.can_save,
        "Search viewer painted thumbnail target preserves exact resource instead of selecting the main image");
    application_image_viewer_close();
    check(application_image_viewer_open_search(ticket, MessageSceneRichItemKind::attachment, 0, false, preview),
        "Search viewer source-retirement fixture opens");
    const UiRect save = ui_image_viewer_button(g_app.image_viewer.scene, 4);
    const LPARAM point = MAKELPARAM(static_cast<i16>((save.left + save.right) * .5f), static_cast<i16>((save.top + save.bottom) * .5f));
    application_image_viewer_message(WM_LBUTTONDOWN, MK_LBUTTON, point);
    ++publication;
    check(application_image_viewer_synchronize() && !g_app.image_viewer.scene.active && GetCapture() != g_app.window &&
        !g_app.renderer.image_viewer && saves == before_embed,
        "Search viewer page replacement retires source and releases pending native Save capture");
    check(application_image_viewer_message(WM_LBUTTONUP, 0, point) && !g_app.image_viewer.pointer_tail && saves == before_embed,
        "Search viewer retired Save press consumes its pointer release before restored Search can receive it");
    ticket.publication_revision = publication; check(seed(), "Search viewer publication replacement rebuilds owned fixture");
    check(application_image_viewer_open_search(ticket, MessageSceneRichItemKind::attachment, 0, false, preview),
        "Search viewer keyboard-retirement fixture opens");
    ui_widget_manager_focus(&g_app.image_viewer.scene.widgets, g_app.image_viewer.scene.buttons[4]);
    application_image_viewer_message(WM_KEYDOWN, VK_SPACE, 0); panel = false;
    check(application_image_viewer_synchronize() && !g_app.image_viewer.scene.active && g_app.image_viewer.held_key == VK_SPACE &&
        application_image_viewer_message(WM_CHAR, VK_SPACE, 0) && application_image_viewer_message(WM_KEYUP, VK_SPACE, 0) &&
        !g_app.image_viewer.held_key && saves == before_embed,
        "Search viewer cancelled Space press consumes closing text and key tails without activating Save");
    for (u32 fence = 0; fence < 5; ++fence) {
        panel = allowed = true;
        check(application_image_viewer_open_search(ticket, MessageSceneRichItemKind::attachment, 0, false, preview),
            "Search viewer scope fence fixture opens");
        switch (fence) {
        case 0: ++g_app.plugin_account_scope_serial; break;
        case 1: ++g_app.account.status.profile_id; break;
        case 2: ++g_app.account_ui->chat_state.self_user_id; break;
        case 3:
            check(chat_state_store_append_channel_overwrite(&g_app.account_ui->chat_state,101,
                {7,0,kPermissionViewChannel,0}), "Search viewer permission retirement publishes real channel denial");
            chat_state_store_recompute_guild_permissions(&g_app.account_ui->chat_state,7);
            break;
        case 4: panel = false; break;
        }
        activate(4);
        check(application_image_viewer_synchronize() && !g_app.image_viewer.scene.active && saves == before_embed,
            "Search viewer account profile self permission or explicit source close blocks pending Save and retires viewer");
        g_app.plugin_account_scope_serial = 7; g_app.account.status.profile_id = 4; g_app.account_ui->chat_state.self_user_id = 3;
        if (fence == 3) {
            chat_state_store_clear_channel_overwrites(&g_app.account_ui->chat_state,101);
            chat_state_store_recompute_guild_permissions(&g_app.account_ui->chat_state,7);
        }
    }
    application_image_viewer_close();
    panel = allowed = true;
    ++publication; ticket.publication_revision = publication;
    check(seed_gifv() && application_image_viewer_open_search(ticket, MessageSceneRichItemKind::embed, 0, false, preview) &&
        g_app.image_viewer.item_count == 1 && !g_app.image_viewer.items[0].video &&
        media_asset_embed_resource(g_app.image_viewer.key) == MediaEmbedResource::video &&
        media_asset_embed_format(g_app.image_viewer.key) == MediaAttachmentFormat::mp4 &&
        (g_app.image_viewer.key.flags & media_asset_flag_animated) && !g_app.image_viewer.scene.can_save,
        "Search viewer opens a GIFV embed full screen as the looping animation it plays inline");
    application_image_viewer_close();
    ui_search_scene_release(&search_scene);
    g_app.renderer.search_scene = nullptr;
    DestroyWindow(g_app.window);
    g_app.window = nullptr;
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE); g_app.account_ui = nullptr; g_app.account.instance = nullptr;
}
} // namespace maicord::image_viewer_runtime_test

namespace maicord {
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept { ++image_viewer_runtime_test::rebuilds; return true; }
bool renderer_read_media_rgba(D3D11Renderer*, MediaAssetKey, Slice<u8>, u32*, u32*) noexcept { return false; }
namespace app_detail {
UiSearchScene* application_search_scene() noexcept { return &image_viewer_runtime_test::search_scene; }
bool refresh_account_frame(bool) noexcept {
    ++image_viewer_runtime_test::media_refreshes;
    return true;
}
void application_user_settings_request_content_confirmation() noexcept {
    ++image_viewer_runtime_test::confirmations;
}
bool application_open_content_url(Utf8View url) noexcept {
    using namespace image_viewer_runtime_test;
    ++opened_urls; opened_bytes = minimum<u32>(url.bytes, sizeof(opened_url));
    copy_bytes(opened_url, url.data, opened_bytes);
    return true;
}
bool application_video_open_viewer_item(u64, u32, bool, u8, bool, const ApplicationSearchSourceTicket*, UiRect,
                                        MediaAssetKey*) noexcept { return false; }
void application_video_viewer_detach() noexcept {}
bool application_video_viewer_pointer(UINT, UiPoint) noexcept { return false; }
bool application_video_viewer_hidden_cursor() noexcept { return false; }
void application_video_cancel_pointer() noexcept {}
bool application_video_key(UINT, bool, bool) noexcept { return false; }
void application_composer_text_input_suspend() noexcept { ++image_viewer_runtime_test::suspends; }
float client_dip_x(LPARAM value) noexcept { return static_cast<i16>(LOWORD(value)); }
float client_dip_y(LPARAM value) noexcept { return static_cast<i16>(HIWORD(value)); }
bool application_downloads_active() noexcept { return false; }
bool application_downloads_available(u64, u64, bool) noexcept { return false; }
bool application_downloads_begin(u64, u64, bool) noexcept { return false; }
bool application_search_source_resolve(const ApplicationSearchSourceTicket& ticket, bool snapshot, ApplicationSearchSourceView* out) noexcept {
    using namespace image_viewer_runtime_test;
    if (!out) return false; *out = {};
    if (!panel || !allowed || ticket.message_id != 800 || ticket.index != 0 || ticket.publication_revision != publication ||
        g_app.account.instance != &account_marker || g_app.plugin_account_scope_serial != 7 || g_app.account.status.profile_id != 4 ||
        !g_app.account_ui || g_app.account_ui->chat_state.self_user_id != 3) return false;
    const auto found = message_store_find(bank, ticket.message_id);
    const auto* message = found.ok() ? message_store_resolve(bank, found.handle) : nullptr;
    const auto* content = snapshot ? message_rich_store_snapshot(rich, ticket.message_id) : message_rich_store_find(rich, ticket.message_id);
    if (!message || !content || message->revision != publication || content->message_revision != publication) return false;
    out->ticket = ticket; out->store = &bank; out->message = message; out->handle = found.handle;
    out->rich_store = &rich; out->rich = content; out->snapshot = snapshot;
    out->guild_id = 7; out->source_channel_id = snapshot ? message_rich_store_find(rich,800)->reference.channel_id : 0;
    return true;
}
bool application_downloads_search_available(const ApplicationSearchSourceTicket& ticket, u64 attachment, bool snapshot) noexcept {
    ApplicationSearchSourceView source{};
    if (!application_search_source_resolve(ticket, snapshot, &source)) return false;
    for (u32 i = 0; i < source.rich->attachment_count; ++i) if (source.rich->attachments[i].id == attachment) return true;
    return false;
}
bool application_downloads_begin_search(const ApplicationSearchSourceTicket& ticket, u64 attachment, bool snapshot) noexcept {
    using namespace image_viewer_runtime_test;
    if (!application_downloads_search_available(ticket, attachment, snapshot)) return true;
    ++saves; saved_ticket = ticket; saved_attachment = attachment; saved_snapshot = snapshot;
    if (close_on_save) application_image_viewer_close();
    return true;
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_application_image_viewer_runtime_entry() noexcept {
    maicord::image_viewer_runtime_test::run();
    ExitProcess(maicord::image_viewer_runtime_test::failures ? 1 : 0);
}
