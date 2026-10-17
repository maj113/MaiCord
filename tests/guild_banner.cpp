// Server banners: Gateway decode (GUILD_CREATE/UPDATE and READY properties)
// into the guild record, CDN URLs and media identities, the channel-list
// header geometry and animation rules, the list's banner inset, the header's
// banner presentation (paint and text) and the server tag popout's banner.
#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/guild_settings_model.hpp"
#include "maicord/ui/guild_banner.hpp"
#include "maicord/ui/guild_card_scene.hpp"
#include "maicord/ui/shell_presentation.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "test_output.hpp"

namespace maicord::guild_banner_test {
u32 failures = 0;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}
bool approx(float a, float b) noexcept {
    return a - b < .001f && b - a < .001f;
}
bool same(UiRect a, UiRect b) noexcept {
    return approx(a.left, b.left) && approx(a.top, b.top) && approx(a.right, b.right) && approx(a.bottom, b.bottom);
}
bool same(Color a, Color b) noexcept {
    return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}
constexpr Utf8View kStill = utf8_literal("0123456789abcdef0123456789abcdef");
constexpr Utf8View kAnimated = utf8_literal("a_0123456789abcdef0123456789abcdef");

void test_urls() noexcept {
    check(ui_media_proxy_size(16) == 16 && ui_media_proxy_size(360) == 480 && ui_media_proxy_size(450) == 480 &&
              ui_media_proxy_size(540) == 600 && ui_media_proxy_size(720) == 1024 &&
              ui_media_proxy_size(5000) == 4096,
          "urls: sizes round up to the media proxy's size list");
    UiGuildImageMedia still{}, moving{}, gif{}, hidpi{}, frozen{};
    const bool built = ui_guild_banner_media(10, kStill, 96, false, false, &still) &&
                       ui_guild_banner_media(10, kAnimated, 96, true, false, &moving) &&
                       ui_guild_banner_media(10, kAnimated, 96, true, true, &gif) &&
                       ui_guild_banner_media(10, kStill, 192, false, false, &hidpi) &&
                       ui_guild_banner_media(10, kAnimated, 144, false, false, &frozen);
    check(built &&
              utf8_equal(still.url_view(), utf8_literal("https://cdn.discordapp.com/banners/10/"
                                                        "0123456789abcdef0123456789abcdef.webp?size=480")) &&
              utf8_equal(moving.url_view(),
                         utf8_literal("https://cdn.discordapp.com/banners/10/"
                                      "a_0123456789abcdef0123456789abcdef.webp?size=480&animated=true")) &&
              utf8_equal(gif.url_view(), utf8_literal("https://cdn.discordapp.com/banners/10/"
                                                      "a_0123456789abcdef0123456789abcdef.gif?size=480")) &&
              utf8_equal(hidpi.url_view(), utf8_literal("https://cdn.discordapp.com/banners/10/"
                                                        "0123456789abcdef0123456789abcdef.webp?size=512")) &&
              utf8_equal(frozen.url_view(), utf8_literal("https://cdn.discordapp.com/banners/10/"
                                                         "a_0123456789abcdef0123456789abcdef.webp?size=512")),
          "urls: banners pick webp sized for 360*dpr up to the decoders' 512 px, animated=true (or gif) only "
          "while animating");
    check(still.key.kind == MediaAssetKind::profile_image && media_asset_key_valid(still.key) &&
              media_asset_key_valid(moving.key) && media_asset_key_valid(gif.key) &&
              !(still.key.flags & media_asset_flag_animated) && (moving.key.flags & media_asset_flag_animated) &&
              (gif.key.flags & media_asset_flag_animated) && !(frozen.key.flags & media_asset_flag_animated) &&
              media_asset_embed_format(gif.key) == MediaAttachmentFormat::gif &&
              !media_asset_key_equal(still.key, hidpi.key) && !media_asset_key_equal(moving.key, gif.key),
          "urls: each representation is its own profile_image identity");
    UiGuildImageMedia splash{}, discovery{}, rejected{};
    check(ui_guild_splash_media(10, kStill, false, 300, 96, &splash) &&
              utf8_equal(splash.url_view(), utf8_literal("https://cdn.discordapp.com/splashes/10/"
                                                         "0123456789abcdef0123456789abcdef.jpg?size=300")) &&
              ui_guild_splash_media(10, kStill, false, 640, 96, &splash) &&
              utf8_equal(splash.url_view(), utf8_literal("https://cdn.discordapp.com/splashes/10/"
                                                         "0123456789abcdef0123456789abcdef.jpg?size=512")) &&
              ui_guild_splash_media(10, kStill, true, 512, 144, &discovery) &&
              utf8_equal(discovery.url_view(), utf8_literal("https://cdn.discordapp.com/discovery-splashes/10/"
                                                            "0123456789abcdef0123456789abcdef.jpg?size=512")) &&
              media_asset_embed_format(discovery.key) == MediaAttachmentFormat::jpeg &&
              !ui_guild_banner_media(10, utf8_literal("zz"), 96, false, false, &rejected) &&
              !ui_guild_banner_media(0, kStill, 96, false, false, &rejected) &&
              !ui_guild_splash_media(10, utf8_literal("a_zz"), true, 512, 96, &rejected),
          "urls: splashes are JPEG at the surface's size up to 512 px; malformed hashes build nothing");
}

void test_geometry() noexcept {
    const UiRect sidebar{72, 0, 340, 800}, header{72, 0, 340, 48};
    const auto rest = ui_guild_banner_geometry(sidebar, header, 0, false);
    check(rest.visible && approx(rest.value, 1) && same(rest.image, {72, 0, 340, 130}) &&
              same(rest.clip, {72, 0, 340, 130}) && same(rest.hover, {72, 48, 340, 136}) && approx(rest.glass, 0) &&
              approx(rest.scrim, 1) && approx(rest.opacity, 1),
          "geometry: at rest the 130 DIP banner covers the header and the 88 DIP strip below it");
    const auto ten = ui_guild_banner_geometry(sidebar, header, 10, false);
    const auto half = ui_guild_banner_geometry(sidebar, header, 44, false);
    check(approx(ten.value, 1 - 10.f / 88) && approx(ten.glass, 60.f / 88) && same(ten.image, rest.image) &&
              approx(half.value, .5f) && same(half.clip, {72, 0, 340, 85}) && same(half.hover, {72, 48, 340, 92}) &&
              approx(half.glass, 1) && approx(half.opacity, .5f) && half.visible,
          "geometry: scrolling fades the banner, slides its container up 90 DIP and frosts the header 6x faster");
    const auto hovered = ui_guild_banner_geometry(sidebar, header, 0, true);
    const auto gone = ui_guild_banner_geometry(sidebar, header, 88, true);
    const auto distant = ui_guild_banner_geometry(sidebar, header, 400, false);
    check(approx(hovered.glass, 1) && !gone.visible && approx(gone.value, 0) && approx(gone.glass, 0) &&
              !ui_rect_has_area(gone.hover) && !distant.visible && approx(distant.scrim, 0),
          "geometry: hovering the header frosts it; at 88 DIP the banner and its layers are gone");
}

void test_animation() noexcept {
    UiGuildBannerAnimation a{};
    const bool first = ui_guild_banner_animate(&a, 10, true, true, false, true, 1000);
    const u64 wake = ui_guild_banner_next_wake(a, 1000);
    const bool held = ui_guild_banner_animate(&a, 10, true, true, false, true, 5999);
    const bool ended = ui_guild_banner_animate(&a, 10, true, true, false, true, 6000);
    check(first && wake == 6000 && held && !ended && !ui_guild_banner_next_wake(a, 6000),
          "animation: coming into view plays the banner for 5 s when GIFs autoplay");
    const bool hover = ui_guild_banner_animate(&a, 10, true, true, true, true, 7000);
    const bool still_hover = ui_guild_banner_animate(&a, 10, true, true, true, true, 20000);
    const bool left = ui_guild_banner_animate(&a, 10, true, true, false, true, 20001);
    check(hover && still_hover && !left, "animation: the hover strip plays it until the pointer leaves");
    ui_guild_banner_animate(&a, 10, true, false, false, true, 21000);
    const bool back = ui_guild_banner_animate(&a, 10, true, true, false, true, 22000);
    const bool entered = ui_guild_banner_animate(&a, 10, true, true, true, true, 23000);
    const bool exited = ui_guild_banner_animate(&a, 10, true, true, false, true, 23001);
    check(back && entered && !exited, "animation: scrolling back into view replays; hovering ends the timed play");
    UiGuildBannerAnimation quiet{};
    const bool no_autoplay = ui_guild_banner_animate(&quiet, 10, true, true, false, false, 1000);
    const bool hovered = ui_guild_banner_animate(&quiet, 10, true, true, true, false, 1001);
    UiGuildBannerAnimation still{};
    const bool ineligible = ui_guild_banner_animate(&still, 10, false, true, true, true, 1000);
    UiGuildBannerAnimation hidden{};
    const bool offscreen = ui_guild_banner_animate(&hidden, 10, true, false, true, true, 1000);
    const bool switched = ui_guild_banner_animate(&a, 11, true, true, false, true, 30000);
    check(!no_autoplay && hovered && !ineligible && !offscreen && switched && a.guild_id == 11,
          "animation: without autoplay only hover plays; still or hidden banners never play; a new server replays");
}

GatewayClientEvent event;
ConstSlice<u8> payload;
u32 scratch[16384];
char storage[64 * 1024];
GatewayClientDispatchRouteResult route(Utf8View name, Utf8View data) noexcept {
    event = {};
    payload = {};
    GatewayEnvelope envelope{};
    envelope.opcode = 0;
    envelope.has_event_name = true;
    envelope.event_name = {name, 0, false};
    envelope.data = {data, 0, JsonValueKind::object};
    return gateway_client_route_dispatch(envelope, {scratch, 16384}, 64, {storage, sizeof(storage)}, {}, 7, &event,
                                         &payload);
}

void test_gateway() noexcept {
    static GuildRecord guilds[2];
    static ChannelRecord channels[4];
    static u16 guild_order[2], channel_order[4];
    static u8 names[4096];
    static RoleRecord roles[8];
    static PermissionOverwriteRecord overwrites[4];
    static ChatStateStore store;
    store = {};
    u64 ids[8]{};
    const bool bound = chat_state_store_initialize(&store, {guilds, 2}, {guild_order, 2}, {channels, 4},
                                                   {channel_order, 4}, {names, sizeof(names)}) &&
                       chat_state_store_bind_permissions(&store, {roles, 8}, {overwrites, 4});
    store.self_user_id = 7;
    const auto created = route(
        utf8_literal("GUILD_CREATE"),
        utf8_literal(R"({"id":"10","name":"Banner","owner_id":"1","icon":"0123456789abcdef0123456789abcdef",)"
                     R"("banner":"a_0123456789abcdef0123456789abcdef","features":["ANIMATED_BANNER","BANNER"],)"
                     R"("roles":[{"id":"10","name":"@everyone","permissions":"1024"},)"
                     R"({"id":"11","name":"Mods","permissions":"8"}],)"
                     R"("members":[{"user":{"id":"7","username":"me"},"roles":["11"]}],)"
                     R"("channels":[{"id":"20","type":0,"name":"general","position":0}]})"));
    Utf8View banner{}, icon{};
    const bool decoded = created.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_create &&
                         event.state.guild_banner == 35 &&
                         gateway_client_event_guild_banner(event, payload, &banner) &&
                         utf8_equal(banner, kAnimated) && gateway_client_event_guild_icon(event, payload, &icon) &&
                         utf8_equal(icon, kStill) && (event.state.upload_features & guild_record_animated_banner);
    const auto applied = chat_state_apply_gateway_event(&store, event, payload, {ids, 8});
    const auto* guild = chat_state_store_find_guild(store, 10);
    check(bound && decoded && applied.ok() && guild &&
              utf8_equal(chat_state_store_guild_banner_hash(*guild), kAnimated) &&
              utf8_equal(chat_state_store_guild_icon_hash(*guild), kStill) &&
              (guild->upload_features & guild_record_animated_banner) &&
              chat_state_store_find_role(store, 10, 11) && chat_state_store_find_channel(store, 20),
          "gateway: GUILD_CREATE stores the banner beside the icon; roles and channels after it still parse");
    const auto renamed = route(
        utf8_literal("GUILD_UPDATE"),
        utf8_literal(R"({"id":"10","name":"Renamed","owner_id":"1","icon":null,"features":[],)"
                     R"("roles":[{"id":"10","name":"@everyone","permissions":"1024"}]})"));
    const bool kept_key = renamed.ok() && !event.state.guild_banner &&
                          !gateway_client_event_guild_banner(event, payload, &banner);
    const auto kept = chat_state_apply_gateway_event(&store, event, payload, {ids, 8});
    guild = chat_state_store_find_guild(store, 10);
    check(kept_key && kept.ok() && guild && utf8_equal(chat_state_store_guild_banner_hash(*guild), kAnimated) &&
              !(guild->upload_features & guild_record_animated_banner),
          "gateway: an update without a banner key keeps the stored banner");
    const auto cleared = route(utf8_literal("GUILD_UPDATE"),
                               utf8_literal(R"({"id":"10","name":"Renamed","owner_id":"1","banner":null,)"
                                            R"("roles":[{"id":"10","name":"@everyone","permissions":"1024"}]})"));
    const bool null_decoded = cleared.ok() && event.state.guild_banner == 1 &&
                              gateway_client_event_guild_banner(event, payload, &banner) && !banner.bytes;
    const auto applied_null = chat_state_apply_gateway_event(&store, event, payload, {ids, 8});
    guild = chat_state_store_find_guild(store, 10);
    check(null_decoded && applied_null.ok() && guild && !chat_state_store_guild_banner_hash(*guild).bytes,
          "gateway: a null banner clears it");
    const auto nested = route(
        utf8_literal("GUILD_CREATE"),
        utf8_literal(R"({"id":"12","properties":{"name":"Nested","owner_id":"1",)"
                     R"("banner":"0123456789abcdef0123456789abcdef"},)"
                     R"("roles":[{"id":"12","name":"@everyone","permissions":"0"}],)"
                     R"("members":[{"user":{"id":"7","username":"me"},"roles":[]}],"channels":[]})"));
    const auto applied_nested = chat_state_apply_gateway_event(&store, event, payload, {ids, 8});
    const auto* nested_guild = chat_state_store_find_guild(store, 12);
    check(nested.ok() && applied_nested.ok() && nested_guild &&
              utf8_equal(chat_state_store_guild_banner_hash(*nested_guild), kStill),
          "gateway: a READY-style properties object carries the banner");
    const auto odd = route(utf8_literal("GUILD_UPDATE"),
                           utf8_literal(R"({"id":"12","name":"Nested","owner_id":"1","banner":"not a hash",)"
                                        R"("roles":[{"id":"12","name":"@everyone","permissions":"0"}]})"));
    const auto applied_odd = chat_state_apply_gateway_event(&store, event, payload, {ids, 8});
    nested_guild = chat_state_store_find_guild(store, 12);
    const auto shaped = route(utf8_literal("GUILD_UPDATE"),
                              utf8_literal(R"({"id":"12","name":"Nested","owner_id":"1","banner":{"x":1},)"
                                           R"("roles":[{"id":"12","name":"@everyone","permissions":"0"}]})"));
    check(odd.ok() && applied_odd.ok() && nested_guild && !chat_state_store_guild_banner_hash(*nested_guild).bytes &&
              shaped.ok() && !event.state.guild_banner,
          "gateway: a malformed banner is dropped without rejecting the guild");
    const auto channel = route(utf8_literal("CHANNEL_UPDATE"),
                               utf8_literal(R"({"id":"20","guild_id":"10","type":0,"name":"general","banner":"x"})"));
    check(channel.ok() && !event.state.guild_banner, "gateway: channel records never carry a banner");
}

struct Fixture {
    UiScene shell;
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channels[6];
    u16 guild_order[1], channel_order[6];
    u8 names[1024];
    NavigationView view;
    GuildViewRow guild_rows[1];
    ChannelViewRow channel_rows[6];
    ShellNavigationScene navigation;
    ShellNavigationRowWidget navigation_rows[6];
    UiWidget navigation_widgets[26];
    UiLayoutNode navigation_nodes[26];
    UiTextCommand commands[64];
    char text[256];
    UiTextCommandStream stream;
    PaintStream paint;
};
Fixture fixture{};
bool initialize() noexcept {
    auto& f = fixture;
    GuildInput guild{7, utf8_literal("Guild"), GuildMutationKind::create, 1};
    guild.banner_hash = kStill;
    guild.banner_known = true;
    if (!ui_scene_initialize_bootstrap(&f.shell) ||
        !ui_scene_layout_with_metrics(&f.shell, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics, 1280, 800) ||
        !chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1}, {f.channels, 6},
                                     {f.channel_order, 6}, {f.names, sizeof(f.names)}) ||
        !chat_state_store_upsert_guild(&f.state, guild).ok() ||
        !shell_navigation_scene_initialize(&f.navigation, {f.navigation_rows, 6}, {f.navigation_widgets, 26},
                                           {f.navigation_nodes, 26}) ||
        !ui_text_command_stream_initialize(&f.stream, {f.commands, 64}, {f.text, sizeof(f.text)}) ||
        !paint_stream_initialize(&f.paint))
        return false;
    f.state.selected_guild_id = 7;
    f.view = {};
    f.view.guild_rows = f.guild_rows;
    f.view.guild_count = f.view.guild_capacity = 1;
    f.guild_rows[0] = {};
    f.guild_rows[0].id = 7;
    f.view.channel_rows = f.channel_rows;
    f.view.channel_count = f.view.channel_capacity = 6;
    f.view.selected_guild_id = 7;
    f.view.guild_revision = f.view.channel_revision = 1;
    for (u32 index = 0; index < 6; ++index) {
        ChannelInput input{};
        input.id = 80 + index;
        input.guild_id = 7;
        input.name = utf8_literal("channel");
        input.name_known = true;
        if (!chat_state_store_upsert_channel(&f.state, input).ok())
            return false;
        f.channel_rows[index] = {};
        f.channel_rows[index].id = input.id;
        f.channel_rows[index].position = index;
        f.channel_rows[index].selectable = true;
    }
    return true;
}
const UiNode* node(UiSemantic semantic) noexcept {
    return ui_scene_resolve(fixture.shell, ui_scene_find(fixture.shell, semantic));
}
const UiTextCommand* header_text(Color* shadow) noexcept {
    auto& f = fixture;
    const UiTextCommand* found = nullptr;
    *shadow = {};
    for (u32 i = 0; i < f.stream.count; ++i)
        if (utf8_equal(f.commands[i].text, utf8_literal("Guild"))) {
            if (found)
                *shadow = found->color;
            found = &f.commands[i];
        }
    return found;
}
bool has_quad(UiRect rectangle, Color fill) noexcept {
    for (u32 i = 0; i < fixture.paint.count; ++i) {
        const auto& q = fixture.paint.quads[i];
        if (same({q.left, q.top, q.right, q.bottom}, rectangle) && same(q.fill, fill))
            return true;
    }
    return false;
}

void test_shell() noexcept {
    auto& f = fixture;
    const bool ready = initialize();
    const auto* channels = node(UiSemantic::channel_navigation);
    const auto* sidebar = node(UiSemantic::channel_sidebar);
    const auto* header = node(UiSemantic::channel_header);
    const bool plain = ready && channels && shell_navigation_scene_update(&f.navigation, &f.view, channels->rectangle, 0).ok();
    const auto* first = plain ? shell_navigation_scene_layout_node(f.navigation, f.navigation.rows[0]) : nullptr;
    const float plain_top = first ? first->rectangle.top - channels->rectangle.top : -1;
    const float plain_content = f.navigation.content_height;
    const bool set = shell_navigation_scene_set_banner(&f.navigation, kUiGuildBannerListInset, false, 1) &&
                     !shell_navigation_scene_set_banner(&f.navigation, kUiGuildBannerListInset, false, 1);
    const bool banner = set && shell_navigation_scene_update(&f.navigation, &f.view, channels->rectangle, 0).ok();
    first = banner ? shell_navigation_scene_layout_node(f.navigation, f.navigation.rows[0]) : nullptr;
    check(plain && approx(plain_top, 8) && banner && first &&
              approx(first->rectangle.top - channels->rectangle.top, 8 + kUiGuildBannerListInset) &&
              approx(f.navigation.content_height, plain_content + kUiGuildBannerListInset),
          "shell: a banner insets the channel list by 84 DIP (content grows with it)");
    // Header text: white over the banner with a shadow copy beneath.
    Color shadow{};
    const bool built = shell_text_presentation_build(f.shell, f.state, f.view, kDefaultTheme, f.navigation, nullptr,
                                                     nullptr, nullptr, &f.stream);
    const auto* name = header_text(&shadow);
    check(built && name && same(name->color, kDefaultTheme.tokens.text.on_accent) &&
              same(shadow, kDefaultTheme.tokens.overlay.shadow) &&
              shell_presentation_guild_banner_visible(f.shell, &f.navigation),
          "shell: the server name turns white with a drop shadow over the banner");
    // Paint: the container fill in the chrome, the scrim above the picture.
    ShellPresentationRegions regions{};
    const bool chrome = sidebar && header &&
                        shell_presentation_build_chrome(f.shell, &f.view, nullptr, &f.navigation, nullptr, nullptr,
                                                        kDefaultTheme, 1280, 800, &f.paint, &regions, nullptr);
    check(chrome && has_quad({sidebar->rectangle.left, header->rectangle.top, sidebar->rectangle.right,
                              header->rectangle.top + kUiGuildBannerHeight},
                             kDefaultTheme.tokens.state.pressed),
          "shell: the banner container shows the active interactive fill until its picture is resident");
    const u32 before = f.paint.count;
    const bool overlay = shell_presentation_build_guild_banner_overlay(f.shell, &f.view, &f.navigation,
                                                                       kDefaultTheme, &f.paint);
    bool scrim = false;
    for (u32 i = before; i < f.paint.count; ++i) {
        const auto& q = f.paint.quads[i];
        scrim = scrim || (q.border_width == -2 && approx(q.top, header->rectangle.top) &&
                          approx(q.fill.alpha, kDefaultTheme.tokens.overlay.scrim_light.alpha) && q.border.alpha == 0);
    }
    check(overlay && scrim && f.paint.count >= before + (node(UiSemantic::guild_menu) ? 2u : 1u),
          "shell: the header scrim and the dropdown chevron paint above the banner picture");
    // Scrolled past 88 DIP the header is ordinary again (a short list viewport lets it scroll).
    const UiRect short_list{channels->rectangle.left, channels->rectangle.top, channels->rectangle.right,
                            channels->rectangle.top + 100};
    const bool scrolled = shell_navigation_scene_update(&f.navigation, &f.view, short_list, 120).ok() &&
                          shell_text_presentation_build(f.shell, f.state, f.view, kDefaultTheme, f.navigation,
                                                        nullptr, nullptr, nullptr, &f.stream);
    name = header_text(&shadow);
    const u32 mark = f.paint.count;
    const bool quiet = shell_presentation_build_guild_banner_overlay(f.shell, &f.view, &f.navigation,
                                                                     kDefaultTheme, &f.paint) &&
                       f.paint.count == mark;
    check(scrolled && f.navigation.scroll_dips >= kUiGuildBannerScrollRange && name &&
              same(name->color, kDefaultTheme.text_primary) && shadow.alpha == 0 && quiet,
          "shell: once scrolled away the header text and chevron return to normal");
    shell_navigation_scene_set_banner(&f.navigation, 0, false, 2);
    const bool removed = shell_navigation_scene_update(&f.navigation, &f.view, channels->rectangle, 0).ok();
    first = removed ? shell_navigation_scene_layout_node(f.navigation, f.navigation.rows[0]) : nullptr;
    check(removed && first && approx(first->rectangle.top - channels->rectangle.top, 8) &&
              approx(f.navigation.content_height, plain_content),
          "shell: without a banner the list returns to its 8 DIP inset");
    paint_stream_release(&f.paint);
}

// A channel row scrolled half under the server header keeps its own text
// geometry; only the clip stops at the list's top edge (with and without the
// banner inset), so the name slides away under the header instead of being
// squeezed into the visible strip and pushed down.
void test_list_clip() noexcept {
    auto& f = fixture; // initialized by test_shell
    const auto* channels = node(UiSemantic::channel_navigation);
    if (!channels || !f.navigation.rows.data) {
        check(false, "list clip: the shell fixture is ready");
        return;
    }
    const UiRect list{channels->rectangle.left, channels->rectangle.top, channels->rectangle.right,
                      channels->rectangle.top + 100};
    for (u32 pass = 0; pass < 2; ++pass) {
        const bool banner = pass != 0;
        shell_navigation_scene_set_banner(&f.navigation, banner ? kUiGuildBannerListInset : 0, false, 10 + pass);
        // The first row's top lands 12 DIP above the list's top edge.
        const float scroll = 8 + (banner ? kUiGuildBannerListInset : 0) + 12;
        const bool updated = shell_navigation_scene_update(&f.navigation, &f.view, list, scroll).ok() &&
                             shell_text_presentation_build(f.shell, f.state, f.view, kDefaultTheme, f.navigation,
                                                           nullptr, nullptr, nullptr, &f.stream);
        const auto* row = updated && f.navigation.count ? shell_navigation_scene_layout_node(f.navigation, f.navigation.rows[0])
                                                        : nullptr;
        const auto* label = row ? ui_widget_manager_layout_node(f.navigation.widgets, f.navigation.rows[0].label)
                                : nullptr;
        const UiTextCommand* name = nullptr;
        for (u32 i = 0; label && i < f.stream.count; ++i)
            if (utf8_equal(f.commands[i].text, utf8_literal("channel")) &&
                f.commands[i].owner.handle.slot == f.navigation.rows[0].label.slot &&
                f.commands[i].owner.handle.generation == f.navigation.rows[0].label.generation)
                name = &f.commands[i];
        check(row && label && name && approx(row->rectangle.top, list.top - 12) &&
                  same(name->rectangle, label->content_rectangle) && name->rectangle.top < list.top &&
                  approx(name->clip.top, list.top) && approx(name->clip.bottom, label->content_rectangle.bottom),
              banner ? "list clip: under the banner a half-hidden row keeps its text rectangle, clipped at the top"
                     : "list clip: a half-hidden row keeps its text rectangle and is clipped at the list's top");
    }
    shell_navigation_scene_set_banner(&f.navigation, 0, false, 12);
}

void test_card() noexcept {
    static UiGuildCardScene scene;
    scene = {};
    UiGuildCardContent content{};
    content.guild_id = 10;
    content.phase = UiGuildCardPhase::ready;
    content.name = utf8_literal("Server");
    UiGuildImageMedia splash{};
    ui_guild_splash_media(10, kStill, true, 512, 96, &splash);
    content.banner = splash.key;
    const bool shown = ui_guild_card_show(&scene, 10, content, {400, 300, 460, 320}, {0, 0, 1280, 800}, 1000, true,
                                          nullptr);
    UiGuildCardMedia media{};
    const bool banner = ui_guild_card_media(scene, kUiGuildCardBannerMedia, &media);
    const UiRect expected{scene.panel.left, scene.panel.top, scene.panel.right, scene.panel.top + kUiGuildCardBanner};
    check(shown && banner && media.cover && media_asset_key_equal(media.key, splash.key) &&
              same(media.rectangle, expected) && approx(media.mask_radius, 16) &&
              approx(media.mask.bottom, expected.bottom + 16),
          "card: the custom banner covers the 120 DIP banner, rounded with the card's top corners");
    content.banner = {};
    ui_guild_card_show(&scene, 10, content, {400, 300, 460, 320}, {0, 0, 1280, 800}, 1000, true, nullptr);
    const Color base{.2f, .3f, .8f, 1};
    const Color lit = ui_guild_card_banner_highlight(base);
    check(!ui_guild_card_media(scene, kUiGuildCardBannerMedia, &media) && lit.red > base.red &&
              lit.green > base.green && lit.blue >= base.blue && lit.alpha == 1,
          "card: without a picture the brand colour brightens toward the bottom");
}

void run() noexcept {
    test_urls();
    test_geometry();
    test_animation();
    test_gateway();
    test_shell();
    test_list_clip();
    test_card();
}
} // namespace maicord::guild_banner_test

extern "C" __declspec(noreturn) void WINAPI maicord_guild_banner_entry() noexcept {
    maicord::guild_banner_test::run();
    ExitProcess(maicord::guild_banner_test::failures ? 1 : 0);
}
