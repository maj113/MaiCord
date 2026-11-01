#include "guild_rail_icon_probe.hpp"
#include "probe_present.hpp"
#include "probe_media_upload.hpp"
#include "maicord/core/log.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/background_theme.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {
namespace {
// What a row's rail tile should draw: nothing (icon not resident), the still
// rendition, or the animated one (hovered rows only).
enum class Shown : u8 { none, still, moving };

struct IconCase {
    u64 id;
    Utf8View hash;
    bool still_resident;
    bool moving_resident;
};
constexpr u32 kCaseCount = 4;
// Ids 100 and 200 are the fixture's own guilds; 300 and 400 are added.
constexpr IconCase kCases[kCaseCount]{
    {100, utf8_literal("0123456789abcdef0123456789abcdef"), true, false},
    {200, utf8_literal("a_fedcba9876543210fedcba9876543210"), true, true},
    {300, utf8_literal("a_00112233445566778899aabbccddeeff"), true, false},
    {400, utf8_literal("ffeeddccbbaa99887766554433221100"), false, false},
};
static_assert(!asset_hash_animated(kCases[0].hash) && asset_hash_animated(kCases[1].hash) &&
              asset_hash_animated(kCases[2].hash) && !asset_hash_animated(kCases[3].hash));

struct Phase {
    u64 hovered;
    Shown shown[kCaseCount];
};

bool failed(u32 stage, u32 phase = 0) noexcept {
    log_u64(LogLevel::error, "probe", "guild_rail_icon_failure_stage", stage);
    log_u64(LogLevel::error, "probe", "guild_rail_icon_failure_phase", phase);
    return false;
}
bool close_float(float a, float b) noexcept {
    return a - b < .3f && b - a < .3f;
}

MediaAssetKey icon_key(const ChatStateStore& state, u64 id, bool moving) noexcept {
    const GuildRecord* guild = chat_state_store_find_guild(state, id);
    return media_asset_guild_icon(id, guild ? guild->icon_variant : 0, moving);
}

bool upload_icon(D3D11Renderer& r, MediaAssetKey key, u8 shade) noexcept {
    u8 pixels[16 * 16 * 4]{};
    for (u32 i = 0; i < 16 * 16; ++i) {
        pixels[i * 4] = shade;
        pixels[i * 4 + 1] = static_cast<u8>(255 - shade);
        pixels[i * 4 + 2] = 128;
        pixels[i * 4 + 3] = 255;
    }
    return renderer_upload_media_rgba8(&r, key, 16, 16, 16 * 4, {pixels, sizeof(pixels)}).ok();
}

const GuildRailRowWidget* rail_row(const GuildRailScene& rail, u64 id) noexcept {
    for (u32 i = 0; i < rail.count; ++i)
        if (rail.rows[i].view.id == id)
            return &rail.rows[i];
    return nullptr;
}

// Queued media quads drawing the resident `key` inside `icon`.
u32 quads_drawing(const D3D11Renderer& r, MediaAssetKey key, UiRect icon) noexcept {
    const MediaAtlasResult resident = media_atlas_peek(r.media_atlas, key);
    u32 count = 0;
    for (u32 i = 0; resident.ok() && i < r.media_quad_count; ++i) {
        const MediaAtlasHandle handle = r.media_draw.handle_data[i];
        const MediaQuad& quad = r.media_draw.quad_data[i];
        if (!r.media_draw.domain_data[i] && handle.slot == resident.handle.slot &&
            handle.generation == resident.handle.generation && close_float(quad.left, icon.left) &&
            close_float(quad.top, icon.top) && close_float(quad.right, icon.right) &&
            close_float(quad.bottom, icon.bottom))
            ++count;
    }
    return count;
}

// Every case draws exactly its expected rendition in its own tile, and no
// other rendition of any icon.
bool rail_matches(GuildRailIconProbeContext& c, const Phase& phase, u32 index) noexcept {
    u32 expected_total = 0, drawn_total = 0;
    for (u32 i = 0; i < kCaseCount; ++i) {
        const GuildRailRowWidget* row = rail_row(c.rail, kCases[i].id);
        const UiLayoutNode* node = row ? guild_rail_scene_layout_node(c.rail, *row) : nullptr;
        if (!node)
            return failed(30 + i, index);
        const UiRect icon = guild_rail_icon_rectangle(node->rectangle);
        const u32 still = quads_drawing(c.renderer, icon_key(c.state, kCases[i].id, false), icon);
        const u32 moving = quads_drawing(c.renderer, icon_key(c.state, kCases[i].id, true), icon);
        const Shown shown = phase.shown[i];
        if (still != (shown == Shown::still ? 1u : 0u) || moving != (shown == Shown::moving ? 1u : 0u)) {
            log_u64(LogLevel::error, "probe", "guild_rail_icon_mismatch_guild", kCases[i].id);
            log_u64(LogLevel::error, "probe", "guild_rail_icon_still_quads", still);
            log_u64(LogLevel::error, "probe", "guild_rail_icon_moving_quads", moving);
            return failed(40 + i, index);
        }
        expected_total += shown != Shown::none ? 1u : 0u;
        drawn_total += still + moving;
    }
    return drawn_total == expected_total || failed(50, index);
}

// Hover the row (0: nobody), present twice and check the frames and the quads.
bool run_phase(GuildRailIconProbeContext& c, const Phase& phase, u32 index) noexcept {
    auto& r = c.renderer;
    const GuildRailRowWidget* row = phase.hovered ? rail_row(c.rail, phase.hovered) : nullptr;
    if (phase.hovered && !row)
        return failed(10, index);
    ui_widget_manager_hover(&c.rail.widgets, row ? row->widget : ui_widget_invalid_handle());
    const u32 builds = r.paint_stream_builds, painted = r.painted_frames;
    const u32 swaps = r.full_present_draws + r.partial_present_draws;
    // A rejected build_layout makes renderer_present return false.
    if (!probe_present(&r, kDefaultTheme))
        return failed(11, index);
    if (!r.retained_presentation.valid || r.paint_stream_builds != builds + 1 || r.painted_frames != painted + 1 ||
        r.full_present_draws + r.partial_present_draws <= swaps)
        return failed(12, index);
    if (!rail_matches(c, phase, index))
        return false;
    // The next frame reuses the retained build and keeps the same quads.
    if (!probe_present(&r, kDefaultTheme) || !r.retained_presentation.valid ||
        r.paint_stream_builds != builds + 1)
        return failed(13, index);
    return rail_matches(c, phase, index);
}

// Optional captures of the finished rail: server 100 selected, 200 unread,
// 300 with a mention badge and 400, now without artwork, as a hovered text
// tile; flat and gradient themes at 100% and 125%.
bool capture_rail(GuildRailIconProbeContext& c) noexcept {
    if (!c.capture)
        return true;
    auto& r = c.renderer;
    if (!chat_state_store_upsert_guild(&c.state, GuildInput{400, {}, GuildMutationKind::update, 0, {}, true}).ok())
        return failed(60);
    // The account may invite to and manage every channel, so the server
    // header, the categories and the selected channel show their controls.
    for (u32 i = 0; i < c.state.channel_count; ++i) {
        ChannelRecord& channel = c.state.channels[c.state.channel_order[i]];
        channel.permissions_resolved = true;
        channel.effective_permissions |= kPermissionViewChannel | kPermissionCreateInvite | kPermissionManageChannels;
        channel.actions |= channel_action_view | channel_action_manage_channel;
    }
    if (!navigation_view_rebuild(&c.navigation, c.state).ok() || c.navigation.guild_count != kCaseCount)
        return failed(60);
    c.navigation.selected_guild_id = 100;
    for (u32 i = 0; i < c.navigation.guild_count; ++i) {
        GuildViewRow& row = c.navigation.guild_rows[i];
        row.selected = row.id == 100;
        row.unread = row.id == 200;
        row.mention_count = row.id == 300 ? 3 : 0;
        ++row.generation;
    }
    ++c.navigation.guild_revision;
    Theme gradient = kDefaultTheme;
    gradient.background = background_theme_defaults();
    const u32 dpi = r.dpi, width = r.width, height = r.height;
    struct Shot {
        u32 dpi;
        const Theme* theme;
        const wchar_t* name;
    };
    const Shot shots[]{{96, &kDefaultTheme, L"guild-rail-dark96.bmp"},
                       {96, &gradient, L"guild-rail-gradient96.bmp"},
                       {120, &kDefaultTheme, L"guild-rail-dark120.bmp"},
                       {120, &gradient, L"guild-rail-gradient120.bmp"}};
    for (const Shot& shot : shots) {
        renderer_set_dpi(&r, shot.dpi);
        if (!renderer_resize(&r, width, height) || !c.rebuild())
            return failed(61);
        const GuildRailRowWidget* text_tile = rail_row(c.rail, 400);
        if (!text_tile)
            return failed(62);
        ui_widget_manager_hover(&c.rail.widgets, text_tile->widget);
        // The first idle channel is hovered too: its controls, dimmer than
        // the selected channel's, and its brightened name.
        if (auto* channels = const_cast<ShellNavigationScene*>(r.shell_navigation_scene)) {
            for (u32 i = 0; i < channels->count; ++i) {
                const auto& row = channels->rows[i];
                if (row.view.type == 0 && !row.view.selected) {
                    ui_widget_manager_hover(&channels->widgets, row.widget);
                    break;
                }
            }
        }
        if (!c.rebuild() || !probe_present(&r, *shot.theme) || !c.capture(shot.name, *shot.theme))
            return failed(63);
    }
    renderer_set_dpi(&r, dpi);
    return (renderer_resize(&r, width, height) && c.rebuild()) || failed(64);
}
} // namespace

bool guild_rail_icon_ui_probe(GuildRailIconProbeContext c) noexcept {
    auto& r = c.renderer;
    if (!c.rebuild || !r.media_atlas || r.guild_rail_scene != &c.rail)
        return failed(1);
    for (const IconCase& icon : kCases) {
        const bool exists = chat_state_store_find_guild(c.state, icon.id) != nullptr;
        if (!chat_state_store_upsert_guild(
                 &c.state, GuildInput{icon.id, exists ? Utf8View{} : utf8_literal("Rail Icon Guild"),
                                      exists ? GuildMutationKind::update : GuildMutationKind::create, 0,
                                      icon.hash, true})
                 .ok())
            return failed(2);
    }
    if (!navigation_view_rebuild(&c.navigation, c.state).ok() || !c.rebuild() ||
        c.navigation.guild_count != kCaseCount)
        return failed(3);
    // Resident artwork: the still for 100, 200 and 300; the animation only
    // for 200. Guild 400's icon is still loading.
    for (u32 i = 0; i < kCaseCount; ++i) {
        if (kCases[i].still_resident && !upload_icon(r, icon_key(c.state, kCases[i].id, false), static_cast<u8>(40 + i * 50)))
            return failed(4, i);
        if (kCases[i].moving_resident && !upload_icon(r, icon_key(c.state, kCases[i].id, true), 200))
            return failed(5, i);
    }
    constexpr Shown S = Shown::still, M = Shown::moving, N = Shown::none;
    const Phase phases[]{
        // At rest every resident row shows its still, animated ones included.
        {0, {S, S, S, N}},
        // Hovering an animated row swaps in its animation once resident.
        {200, {S, M, S, N}},
        // An animated row whose animation is still loading keeps the still.
        {300, {S, S, S, N}},
        // Hovering a row with nothing resident draws no icon and breaks nothing.
        {400, {S, S, S, N}},
    };
    u32 index = 0;
    for (const Phase& phase : phases)
        if (!run_phase(c, phase, index++))
            return false;
    // The loading icon arrives: the next frame draws it.
    if (!upload_icon(r, icon_key(c.state, 400, false), 240))
        return failed(6);
    if (!run_phase(c, {0, {S, S, S, S}}, index++))
        return false;
    // The hovered row's animation arrives and replaces its still.
    if (!upload_icon(r, icon_key(c.state, 300, true), 120))
        return failed(7);
    if (!run_phase(c, {300, {S, S, M, S}}, index++))
        return false;
    if (!run_phase(c, {0, {S, S, S, S}}, index++) || !capture_rail(c))
        return false;
    log_write(LogLevel::info, "probe", "guild_rail_icons_layout_quads_present_passed");
    return true;
}
} // namespace maicord
