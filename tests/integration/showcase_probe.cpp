#include "showcase_probe_internal.hpp"
// Website screenshots through the real renderer: a server channel with
// avatars, server tags, an image, a link embed, reactions, a reply, the full
// header and the member list, under the caption, in dark, light and gradient
// themes; the profile popout and full profile with an activity; Settings >
// Appearance; the slash command picker; a bot's answer with buttons; a
// theme-switch loop; and an animated hero where typing, messages, reactions and
// a slash command arrive through the timeline and rich store as the Gateway
// path applies them. Every name, message and image is invented and generated
// here. Opt-in (label showcase); frames go to
// MAICORD_UI_PROBE_CAPTURE_DIR as showcase-*.bmp, at 1440x900 DIP and 1x/2x.
#include "probe_capture.hpp"
#include "probe_present.hpp"
#include "settings/catalog.hpp"
#include "../../src/app/shell/app_icon.hpp"
#include "../../src/render/d3d11/renderer_caption.hpp"
#include "../../src/render/d3d11/renderer_draw.hpp"
#include "maicord/app/settings/client_theme.hpp"
#include "maicord/app/settings/client_theme_document.hpp"
#include "maicord/app/packages/mcfx.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/media/image/png.hpp"
#include "maicord/media/core/presentation_policy.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "maicord/model/guild/display_order.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "maicord/render/d3d11/layer_presentation.hpp"
#include "maicord/ui/guild/rail_scene.hpp"
#include "maicord/ui/guild/member_list_scene.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/chat/messages/text_coordinator.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/widgets/presence.hpp"
#include "maicord/ui/account/profile_media.hpp"
#include "maicord/ui/core/scene.hpp"
#include "maicord/ui/media/picker.hpp"
#include "maicord/ui/widgets/spinner.hpp"
#include "maicord/ui/chat/search/scene.hpp"
#include "maicord/ui/chat/messages/typing_presentation.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/shell/navigation_scene.hpp"
#include "maicord/ui/shell/caption_bar.hpp"

namespace maicord {
namespace {

// --- Fixture -------------------------------------------------------------

struct Person {
    u64 id;
    const char* name;
    const char* username;
    u64 role;
    GuildMemberStatus status;
    u32 colors[2];
    const char* activity;
    const char* emoji;
};
// Display names, roles, presence and avatar colours; all invented.
constexpr Person kPeople[]{
    {kSelf,
     "Mai",
     "mai",
     kRoleMaintainers,
     GuildMemberStatus::online,
     {0x5865f2, 0x9b6bff},
     "Shipping 0.6",
     nullptr},
    {11,
     "Ada Park",
     "ada.park",
     kRoleMaintainers,
     GuildMemberStatus::online,
     {0xf0a04b, 0xe0457b},
     "Reviewing pull requests",
     "\xF0\x9F\x9B\xA0\xEF\xB8\x8F"},
    {12,
     "Milo Reyes",
     "milo.makes",
     kRoleDesigners,
     GuildMemberStatus::online,
     {0x2bc0e4, 0x5b6cf0},
     "Playing Lumen Drift",
     nullptr},
    {13,
     "Juniper",
     "juniper.ink",
     kRoleDesigners,
     GuildMemberStatus::idle,
     {0x8bd17c, 0x2f9e8f},
     "Listening to Lab Radio",
     nullptr},
    {14,
     "Theo Lang",
     "theo.lang",
     kRoleContributors,
     GuildMemberStatus::online,
     {0xff9a8b, 0xff6a88},
     nullptr,
     nullptr},
    {15,
     "Priya N.",
     "priya.n",
     kRoleContributors,
     GuildMemberStatus::dnd,
     {0xa18cd1, 0xfbc2eb},
     "Deep in shaders",
     "\xF0\x9F\x8E\xA8"},
    {16,
     "Sam Okafor",
     "sam.okafor",
     0,
     GuildMemberStatus::online,
     {0xf6d365, 0xfda085},
     "Writing the theme docs",
     "\xE2\x9C\x8D\xEF\xB8\x8F"},
    {17, "Ezra Bloom", "ezra.bloom", 0, GuildMemberStatus::online, {0x43e97b, 0x38a3d1}, nullptr, nullptr},
    {18,
     "Lena Fischer",
     "lena.f",
     0,
     GuildMemberStatus::idle,
     {0x667eea, 0x764ba2},
     "Back in ten",
     "\xE2\x98\x95"},
    {19, "Kai", "kai.dev", 0, GuildMemberStatus::dnd, {0x30cfd0, 0x330867}, nullptr, nullptr},
    {20, "Rosa Lima", "rosa.lima", 0, GuildMemberStatus::offline, {0xfa709a, 0xfee140}, nullptr, nullptr},
    {21, "Noor", "noor", 0, GuildMemberStatus::offline, {0x96fbc4, 0x4fa3a5}, nullptr, nullptr},
    {kBot, "Lab Bot", "labbot", 0, GuildMemberStatus::online, {0xc471f5, 0x5b6cf0}, "Watching the builds", nullptr},
};
constexpr u32 kPeopleCount = sizeof(kPeople) / sizeof(kPeople[0]);

// Server tags: the badge is the tagged server's icon at 32 px.
struct Tag {
    u64 user, guild;
    const char *tag, *badge;
};
constexpr Tag kTags[]{{11, kGuild, "LAB", "5lab0badge"}, {12, kGuild, "LAB", "5lab0badge"},
                      {14, kGuild, "LAB", "5lab0badge"}, {13, 200, "PXL", "5pixel0badge"},
                      {16, 201, "OWL", "5owl0badge"},    {17, 203, "SYN", "5synth0badge"}};

struct Line {
    u64 author;
    u32 minutes, serial;
    const char* text;
};
constexpr Line kLines[]{
    {11, 52, 0,
     "Morning, everyone! \xE2\x98\x80\xEF\xB8\x8F The 0.5 release notes are pinned in <#302>."},
    {12, 20, 0,
     "Tried the **Aurora** gradient on my laptop and it idles at basically 0% CPU. "
     "Here's a palette study for the website:"},
    {14, 14, 0, "That sunset is gorgeous. Mind if we use it for the release banner?"},
    {16, 6, 0,
     "<@12> writing up the theme docs next, draft is here: https://lab.example/notes/theme-gallery"},
};
constexpr u32 kLineCount = sizeof(kLines) / sizeof(kLines[0]);

Utf8View text(const char* s) noexcept {
    return {s, c_string_length(s)};
}

struct Fixture {
    ChatStateStore state;
    GuildRecord guilds[kGuilds];
    u16 guild_order[kGuilds];
    ChannelRecord channels[kChannels];
    u16 channel_order[kChannels];
    RoleRecord roles[16];
    PermissionOverwriteRecord overwrites[16];
    u8 names[8192];
    UserStore users;
    UserRecord user_records[kUsers];
    u32 user_order[kUsers];
    u8 user_names[4096];
    char avatar_hashes[kUsers][33];
    char icon_hashes[kGuilds][33];
    GuildMemberStore members;
    GuildMemberRecord member_records[kMembers];
    u32 member_order[kMembers];
    MemberListView member_view;
    MemberListRow member_rows[kMembers * 2];
    u32 member_scratch[kMembers];
    NavigationView view;
    GuildViewRow guild_rows[kGuilds];
    ChannelViewRow channel_rows[kChannels];
    u16 channel_scratch[kChannels];
    u8 channel_visibility[kChannels];
    ChannelSortKey channel_keys[kChannels];
    MessageStore store;
    MessageRecord records[kMessages];
    u32 order[kMessages], heights[kMessages];
    u64 fenwick[kMessages];
    u8 text[8192], rich_text[32768];
    u16 rich_order[kMessages];
    MessageRichStore rich;
    StableStorage rich_storage;
    UiVariableList list;
    MessageTimeline timeline;
    UiScene shell;
    GuildRailScene rail;
    GuildRailRowWidget rail_rows[kGuilds];
    UiWidget rail_widgets[kGuilds + 2];
    UiLayoutNode rail_nodes[kGuilds + 2];
    ShellNavigationScene navigation;
    ShellNavigationRowWidget navigation_rows[kChannels];
    UiWidget navigation_widgets[kChannels * 4 + 2];
    UiLayoutNode navigation_nodes[kChannels * 4 + 2];
    PopupScene popup;
    UiWidget popup_widgets[70];
    UiLayoutNode popup_nodes[70];
    MessageScene messages;
    UiWidget message_widgets[kRows * kMessageSceneWidgetsPerRow + 1];
    UiLayoutNode message_nodes[kRows * kMessageSceneWidgetsPerRow + 1];
    MessageComposer composer;
    char composer_text[256];
    MessageComposerScene composer_scene;
    MemberListScene member_scene;
    MemberListRowWidget member_scene_rows[48];
    UiWidget member_widgets[193];
    UiLayoutNode member_nodes[193];
    MessageTextPresentation presentation;
    MediaAtlas atlas, preview_atlas;
    MediaAtlasEntry atlas_entries[64], preview_entries[64];
    u32 atlas_buckets[128], preview_buckets[128];
    GlyphAtlasEntry atlas_allocations[64], preview_allocations[64];
    GlyphAtlasRow atlas_rows[32], preview_rows[32];
    ProfileScene profile;
    UiSearchScene search;
    UiSearchContent search_content;
    TypingStore typing;
    TypingRecord typing_records[8];
    char typing_label[kMessageTypingLabelMaximumBytes];
    UiPage pages[kSettingsPanelCount];
    UiSettingsPageContent contents[kSettingsPanelCount];
    UiSettingsControl controls[kSettingsPanelCount][32];
    UiSettingsControlPresentation looks[kSettingsPanelCount][32];
    UiPageRegistry registry;
    UiSettingsScene settings;
    Theme theme;
    // The selected slash command's option bar and chips, and /gif's inline results.
    MessageComposerProjectionContext command;
    UiMediaPickerScene gifs;
    UiMediaPickerSource gif_source;
    UiMediaPickerContent gif_content;
};
constinit Fixture fixture{};

// Generated pixels (RGBA rows), filled once.
constexpr u32 kPhotoW = 1280, kCardW = 1024, kAvatar = 128, kIcon = 96;
constexpr u32 kBannerW = 600, kBannerH = 240, kArt = 256;
u8 g_photo[kPhotoW * kPhotoW * 4];
u8 g_card[kCardW * kCardW * 4];
u8 g_avatars[kPeopleCount][kAvatar * kAvatar * 4];
u8 g_icons[kGuilds][kIcon * kIcon * 4];
u8 g_badges[kGuilds][32 * 32 * 4];
u8 g_banner[kBannerW * kBannerH * 4];
u8 g_game[kArt * kArt * 4], g_record[kArt * kArt * 4], g_scanlines[kPhotoW * kPhotoW * 4 + kPhotoW];
u32 g_caption_icon[kAppIconLargestExtent * kAppIconLargestExtent];
u32 g_failures;
// /gif's six results at the strip's tile height of 150 DIP, painted at 2x.
constexpr u32 kGifTiles = 6, kGifHeight = 300;
constexpr u64 kGifMessage = snowflake(0, 40);
constexpr u32 kGifSizes[kGifTiles][2]{{450, 300}, {400, 300}, {450, 300},
                                      {450, 300}, {300, 300}, {400, 300}};
u8 g_gif[450 * kGifHeight * 4];
GuildFolderSettings g_folders;
u64 g_expanded_folder;

bool check(bool ok, const char* what) noexcept {
    if (!ok) {
        ++g_failures;
        log_write(LogLevel::error, "showcase", what);
    }
    return ok;
}

void make_hash(u64 seed, char* out) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    u64 a = hash64_mix(kHash64Seed, seed * 0x9e3779b97f4a7c15ull), b = hash64_mix(a, seed + 17);
    for (u32 i = 0; i < 32; ++i) {
        const u64 v = i < 16 ? a : b;
        out[i] = digits[(v >> ((i & 15) * 4)) & 15];
    }
    out[0] = '5'; // never the animated "a_" prefix
    out[32] = 0;
}

// Small images (at most 512 px a side, the straight upload's limit).
bool upload_rgba(D3D11Renderer& r, MediaAssetKey key, const u8* pixels, u32 w, u32 h) noexcept {
    MediaAtlas* atlas = r.preview_media_atlas;
    if (media_atlas_peek(r.media_atlas, key).ok() || (atlas && media_atlas_peek(atlas, key).ok()))
        return true;
    return renderer_upload_media_straight_rgba8(&r, key, static_cast<u16>(w), static_cast<u16>(h), w * 4,
                                                {pixels, w * h * 4})
        .ok();
}
// Larger images and profile media go through the PNG path, as decoded PNGs do.
bool upload_png(D3D11Renderer& r, MediaAssetKey key, const u8* pixels, u32 w, u32 h) noexcept {
    if (media_atlas_peek(r.media_atlas, key).ok() ||
        (r.preview_media_atlas && media_atlas_peek(r.preview_media_atlas, key).ok()))
        return true;
    PngImage image{};
    if (!png_image_configure(&image, w, h, 8, 6))
        return false;
    for (u32 y = 0; y < h; ++y) {
        g_scanlines[y * (w * 4 + 1)] = 0;
        copy_bytes(g_scanlines + y * (w * 4 + 1) + 1, pixels + y * w * 4, w * 4);
    }
    return renderer_upload_png_scanlines(&r, key, image, {g_scanlines, h * (w * 4 + 1)}).ok();
}

// A user's server tag (the global appearance record carries it).
void apply_tag(UserAppearance* a, const Tag& t) noexcept {
    const Utf8View tag = text(t.tag), badge = text(t.badge);
    a->user_id = t.user;
    a->fields |= appearance_guild_tag;
    a->primary_enabled = true;
    a->primary_guild_id = t.guild;
    copy_bytes(a->primary_tag.data, tag.data, tag.bytes);
    a->primary_tag.bytes = static_cast<u16>(tag.bytes);
    copy_bytes(a->primary_badge.data, badge.data, badge.bytes);
    a->primary_badge.bytes = static_cast<u16>(badge.bytes);
}

struct GuildSeed {
    u64 id;
    const char* name;
    u32 colors[2];
    u32 shape;
};
constexpr GuildSeed kGuildSeeds[]{
    {kGuild, "MaiCord Lab", {0, 0}, 0},           {200, "Pixel Garden", {0x56ab2f, 0x2f8f6f}, 3},
    {201, "Night Owls", {0x23074d, 0x7b4397}, 0}, {202, "Study Hall", {0xf7971e, 0xd35400}, 4},
    {203, "Synth Club", {0xec008c, 0x6a3093}, 2}, {204, "Trail Crew", {0x134e5e, 0x3ca55c}, 1}};

constexpr u32 kGuildSeedCount = sizeof(kGuildSeeds) / sizeof(kGuildSeeds[0]);

bool build_state(Fixture& f) noexcept {
    constexpr u64 everyone = kPermissionViewChannel | kPermissionSendMessages |
                             kPermissionReadMessageHistory | kPermissionAddReactions |
                             kPermissionEmbedLinks | kPermissionAttachFiles | kPermissionConnect |
                             kPermissionSpeak;
    const auto& guilds = kGuildSeeds;
    bool ok = chat_state_store_initialize(&f.state, {f.guilds, kGuilds}, {f.guild_order, kGuilds},
                                          {f.channels, kChannels}, {f.channel_order, kChannels},
                                          {f.names, sizeof(f.names)}) &&
              chat_state_store_bind_permissions(&f.state, {f.roles, 16}, {f.overwrites, 16});
    for (u32 i = 0; ok && i < kGuildSeedCount; ++i) {
        make_hash(guilds[i].id + 1000, f.icon_hashes[i]);
        GuildInput input{guilds[i].id, text(guilds[i].name), GuildMutationKind::create, 999};
        input.icon_hash = {f.icon_hashes[i], 32};
        input.icon_known = true;
        ok = chat_state_store_upsert_guild(&f.state, input).ok() &&
             chat_state_store_upsert_role(
                 &f.state, RoleInput{guilds[i].id, guilds[i].id, everyone, utf8_literal("@everyone")}) &&
             chat_state_store_set_self_roles(&f.state, guilds[i].id, 999, kSelf, {});
        if (i == 0) {
            paint_logo(g_icons[i], kIcon);
            paint_logo(g_badges[i], 32);
        } else {
            paint_guild_icon(g_icons[i], kIcon, guilds[i].colors[0], guilds[i].colors[1], guilds[i].shape);
            paint_guild_icon(g_badges[i], 32, guilds[i].colors[0], guilds[i].colors[1], guilds[i].shape);
        }
    }
    struct ChannelSeed {
        u64 id, guild, parent;
        u32 position;
        u8 type;
        const char* name;
    };
    constexpr ChannelSeed channels[]{{300, kGuild, 0, 0, 4, "Information"},
                                     {301, kGuild, 300, 0, 0, "welcome"},
                                     {302, kGuild, 300, 1, 5, "announcements"},
                                     {310, kGuild, 0, 1, 4, "Studio"},
                                     {311, kGuild, 310, 0, 0, "general"},
                                     {312, kGuild, 310, 1, 0, "design-crit"},
                                     {313, kGuild, 310, 2, 0, "show-and-tell"},
                                     {314, kGuild, 310, 3, 0, "plugins"},
                                     {315, kGuild, 310, 4, 0, "off-topic"},
                                     {320, kGuild, 0, 2, 4, "Voice"},
                                     {321, kGuild, 320, 0, 2, "Lounge"},
                                     {322, kGuild, 320, 1, 2, "Pairing Room"},
                                     {400, 200, 0, 0, 0, "general"},
                                     {401, 201, 0, 0, 0, "general"},
                                     {402, 202, 0, 0, 0, "general"},
                                     {403, 203, 0, 0, 0, "general"},
                                     {404, 204, 0, 0, 0, "general"}};
    for (const auto& c : channels)
        ok = ok && chat_state_store_upsert_channel(
                       &f.state, ChannelInput{c.id, c.guild, c.parent, text(c.name), c.position, 0, c.type})
                       .ok();
    ok =
        ok &&
        chat_state_store_upsert_role(&f.state, RoleInput{kRoleMaintainers, kGuild, 0,
                                                         utf8_literal("Maintainers"), 3, 0xf0b232, true}) &&
        chat_state_store_upsert_role(
            &f.state, RoleInput{kRoleDesigners, kGuild, 0, utf8_literal("Designers"), 2, 0xeb6fa1, true}) &&
        chat_state_store_upsert_role(&f.state, RoleInput{kRoleContributors, kGuild, 0,
                                                         utf8_literal("Contributors"), 1, 0x3fbfa8, true});
    const u64 self_roles[]{kRoleMaintainers};
    ok = ok && chat_state_store_set_self_roles(&f.state, kGuild, 999, kSelf, {self_roles, 1});
    for (const auto& g : guilds)
        chat_state_store_recompute_guild_permissions(&f.state, g.id);
    // Unread channels and servers, with a couple of mentions.
    ok = ok && chat_state_store_note_message(&f.state, 302, snowflake(70), 11, true, false) &&
         chat_state_store_note_message(&f.state, 312, snowflake(15), 12, false, false) &&
         chat_state_store_note_message(&f.state, 314, snowflake(12), 15, false, false) &&
         chat_state_store_note_message(&f.state, 401, snowflake(20), 18, true, false) &&
         chat_state_store_note_message(&f.state, 401, snowflake(19), 18, true, false) &&
         chat_state_store_note_message(&f.state, 403, snowflake(25), 19, false, false) &&
         chat_state_store_select_channel(&f.state, kChannel).ok();
    return check(ok, "state_failed");
}

// The server list as the account's folder settings arrange it: Lab, a "Side
// projects" folder holding Pixel Garden, Night Owls and Study Hall, then the
// two single servers. The folder shows its members (and their pills) open.
void build_folders(Fixture& f) noexcept {
    constexpr u64 ids[]{kGuild, 200, 201, 202, 203, 204};
    constexpr struct {
        u16 first, count;
        u64 id;
    } layout[]{{0, 1, 0}, {1, 3, 0x5a1f}, {4, 1, 0}, {5, 1, 0}};
    auto& s = g_folders;
    for (u32 i = 0; i < 6; ++i)
        s.guild_ids[i] = ids[i];
    for (u32 i = 0; i < 4; ++i) {
        auto& folder = s.folders[i];
        folder.first_guild = layout[i].first;
        folder.guild_count = layout[i].count;
        folder.id = layout[i].id;
        folder.has_id = layout[i].id != 0;
        if (folder.has_id) {
            const Utf8View name = utf8_literal("Side projects");
            copy_bytes(folder.name, name.data, name.bytes);
            folder.name_bytes = static_cast<u8>(name.bytes);
            folder.color = 0x7c5cff;
            folder.has_color = true;
        }
    }
    s.guild_id_count = 6;
    s.folder_count = 4;
    s.present = true;
    g_expanded_folder = guild_folder_row_key(s, 1);
    f.view.guild_folders = &s;
    f.view.expanded_folders = &g_expanded_folder;
    f.view.expanded_folder_count = 1;
    ++f.view.folder_revision;
}

bool build_people(Fixture& f, D3D11Renderer& r) noexcept {
    bool ok =
        user_store_initialize(&f.users, {f.user_records, kUsers}, {f.user_order, kUsers},
                              {f.user_names, sizeof(f.user_names)}) &&
        guild_member_store_initialize(&f.members, {f.member_records, kMembers}, {f.member_order, kMembers});
    for (u32 i = 0; ok && i < kPeopleCount; ++i) {
        const Person& p = kPeople[i];
        make_hash(p.id, f.avatar_hashes[i]);
        paint_avatar(g_avatars[i], kAvatar, p.colors[0], p.colors[1], i);
        const Utf8View hash{f.avatar_hashes[i], 32};
        const u64 roles[]{p.role};
        ok = user_store_upsert(&f.users, p.id, text(p.name), text(p.username)).ok() &&
             user_store_set_avatar(&f.users, p.id, hash) &&
             upload_rgba(r, media_asset_user_avatar(p.id, user_avatar_variant(hash), false), g_avatars[i],
                         kAvatar, kAvatar) &&
             guild_member_store_upsert(&f.members, f.state, kGuild, p.id, {}, {roles, p.role ? 1u : 0u},
                                       true, p.status, i, kList)
                 .ok();
        if (ok && p.id == kBot) {
            UserAppearance bot{};
            bot.user_id = p.id;
            bot.fields = appearance_bot;
            bot.bot = true;
            ok = user_appearance_upsert(&f.users.appearance, bot);
        }
        if (ok && p.activity) {
            UserAppearance appearance{};
            appearance.user_id = p.id;
            appearance.fields = appearance_activity;
            const Utf8View activity = text(p.activity), emoji = text(p.emoji);
            copy_bytes(appearance.activity.data, activity.data, activity.bytes);
            appearance.activity.bytes = static_cast<u16>(activity.bytes);
            copy_bytes(appearance.status_emoji.data, emoji.data, emoji.bytes);
            appearance.status_emoji.bytes = static_cast<u16>(emoji.bytes);
            ok = user_appearance_upsert(&f.users.appearance, appearance);
        }
    }
    for (u32 i = 0; ok && i < kGuildSeedCount; ++i) {
        const GuildRecord* guild = chat_state_store_find_guild(f.state, kGuildSeeds[i].id);
        if (guild && guild->icon_variant)
            ok = upload_rgba(r, media_asset_guild_icon(guild->id, guild->icon_variant, false), g_icons[i],
                             kIcon, kIcon);
    }
    for (const Tag& t : kTags) {
        u32 seed = 0;
        while (seed + 1 < kGuildSeedCount && kGuildSeeds[seed].id != t.guild)
            ++seed;
        UserAppearance tag{};
        apply_tag(&tag, t);
        ok = ok && user_appearance_upsert(&f.users.appearance, tag);
        ProfileMedia badge{};
        if (ok && user_guild_badge_media(tag, {}, {}, &badge))
            ok = upload_png(r, badge.key, g_badges[seed], 32, 32);
    }
    ok = ok &&
         member_list_view_initialize(&f.member_view, {f.member_rows, kMembers * 2},
                                     {f.member_scratch, kMembers}) &&
         member_list_view_rebuild(&f.member_view, f.members, f.users, f.state, kGuild, kList).ok();
    return check(ok, "people_failed");
}

bool build_messages(Fixture& f) noexcept {
    bool ok = message_store_initialize(&f.store, {f.records, kMessages}, {f.order, kMessages},
                                       {f.text, sizeof(f.text)});
    for (const auto& line : kLines) {
        const u64 id = snowflake(line.minutes, line.serial);
        ok = ok &&
             message_store_upsert(&f.store, MessageInput{id, kChannel, line.author,
                                                         snowflake_timestamp_ms(id), 0, text(line.text), 0})
                 .ok();
    }
    ok = ok &&
         message_rich_store_initialize_owned_sparse(&f.rich, &f.rich_storage, kMessages,
                                                    {f.rich_order, kMessages},
                                                    {f.rich_text, sizeof(f.rich_text)}) &&
         ui_variable_list_initialize(&f.list, {f.heights, kMessages}, {f.fenwick, kMessages}, kLineCount,
                                     ui_extent_units(48)) &&
         message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(48),
                                     ui_extent_units(800), true) &&
         message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
         message_scene_initialize_owned(&f.messages, &f.timeline, kRows, 8,
                                        {f.message_widgets, kRows * kMessageSceneWidgetsPerRow + 1},
                                        {f.message_nodes, kRows * kMessageSceneWidgetsPerRow + 1});
    if (!check(ok, "message_store_failed"))
        return false;
    const auto apply = [&](u64 id, GatewayMessageEvent& event) noexcept {
        const auto found = message_store_find(f.store, id);
        const MessageRecord* record = found.ok() ? message_store_resolve(f.store, found.handle) : nullptr;
        event.id = id;
        return record && message_rich_store_apply(&f.rich, id, record->revision, event).ok();
    };
    // The palette study, with reactions.
    GatewayMessageAttachment photo{};
    photo.id = 0x5c0001;
    photo.size = 418'204;
    photo.filename = utf8_literal("palette-study.png");
    photo.content_type = utf8_literal("image/png");
    photo.url = utf8_literal("https://cdn.discordapp.com/attachments/311/6029312/palette-study.png");
    // Shown at its own size: small enough to keep the reply in view.
    photo.width = 480;
    photo.height = 250;
    photo.has_width = photo.has_height = true;
    GatewayMessageReaction photo_reactions[3]{};
    photo_reactions[0] = {0, utf8_literal("\xF0\x9F\x98\x8D"), 4, 4, 0, true};
    photo_reactions[1] = {0, utf8_literal("\xF0\x9F\x94\xA5"), 6, 6, 0, false};
    photo_reactions[2] = {0, utf8_literal("\xF0\x9F\x91\x80"), 2, 2, 0, false};
    GatewayMessageEvent photo_event{};
    photo_event.fields = gateway_message_field_attachments | gateway_message_field_reactions;
    photo_event.attachments = {&photo, 1};
    photo_event.reactions = {photo_reactions, 3};
    // Theo's reply to it.
    GatewayMessageEvent reply{};
    reply.fields = gateway_message_field_message_reference | gateway_message_field_referenced_message;
    reply.reference.message_id = snowflake(20);
    reply.reference.channel_id = kChannel;
    reply.reference.guild_id = kGuild;
    reply.reference.author_id = 12;
    reply.reference.author_name = utf8_literal("Milo Reyes");
    reply.reference.content =
        utf8_literal("Tried the **Aurora** gradient on my laptop and it idles at basically 0% CPU.");
    reply.reference.state = GatewayMessageReferenceState::available;
    GatewayMessageReaction reply_reactions[1]{};
    reply_reactions[0] = {0, utf8_literal("\xF0\x9F\x91\x8D"), 3, 3, 0, false};
    reply.fields |= gateway_message_field_reactions;
    reply.reactions = {reply_reactions, 1};
    // The docs link with its preview card.
    GatewayMessageEmbed card{};
    card.type = GatewayMessageEmbedType::rich;
    card.provider_name = utf8_literal("MaiCord Lab Notes");
    card.title = utf8_literal("Building a theme gallery");
    card.url = utf8_literal("https://lab.example/notes/theme-gallery");
    card.description = utf8_literal("How the gradient presets, background effects and accent colours fit "
                                    "together, with palettes you can copy.");
    card.color = 0x7c5cff;
    card.has_color = card.has_title = true;
    card.thumbnail.url =
        utf8_literal("https://cdn.discordapp.com/attachments/311/6029313/theme-gallery.png");
    card.thumbnail.width = 400;
    card.thumbnail.height = 400;
    card.thumbnail.has_width = card.thumbnail.has_height = true;
    GatewayMessageReaction card_reactions[2]{};
    card_reactions[0] = {0, utf8_literal("\xF0\x9F\x8E\x89"), 5, 5, 0, true};
    card_reactions[1] = {0, utf8_literal("\xE2\x9D\xA4\xEF\xB8\x8F"), 3, 3, 0, false};
    GatewayMessageEvent card_event{};
    card_event.fields = gateway_message_field_embeds | gateway_message_field_reactions;
    card_event.embeds = {&card, 1};
    card_event.reactions = {card_reactions, 2};
    return check(apply(snowflake(20), photo_event) && apply(snowflake(14), reply) &&
                     apply(snowflake(6), card_event),
                 "rich_apply_failed");
}

// The displayed rectangle in pixels, at most max_w wide.
void display_pixels(float w_dip, float h_dip, u32 dpi, u32 max_w, u32* w, u32* h) noexcept {
    *w = static_cast<u32>(w_dip * dpi / 96.f + .5f);
    *w = *w < 16 ? 16 : *w > max_w ? max_w : *w;
    *h = static_cast<u32>(static_cast<float>(*w) * h_dip / (w_dip > 1 ? w_dip : 1) + .5f);
    *h = *h < 16 ? 16 : *h > max_w ? max_w : *h;
}

// Attachment and embed pixels for the keys the laid-out rows ask for, painted
// at the displayed size as the media proxy would deliver them.
bool upload_rows(Fixture& f, D3D11Renderer& r) noexcept {
    static u32 photo_w, photo_h, card_w, card_h;
    static bool photo_party;
    bool ok = true;
    for (u32 i = 0; i < f.messages.count; ++i) {
        const MessageSceneRowWidget& row = f.messages.rows[i];
        const MessageRichRecord* rich = message_rich_store_find(f.rich, row.message_id);
        if (!rich)
            continue;
        for (u32 a = 0; a < rich->attachment_count; ++a) {
            const auto& attachment = rich->attachments[a];
            const auto format = message_rich_attachment_format(f.rich, attachment);
            float w{}, h{};
            if (!message_rich_attachment_preview_extent(f.rich, attachment, row.measured_width, &w, &h))
                continue;
            const auto tier =
                media_resolution_tier_for_display(w, h, r.dpi, media_preview_extent_cap(format));
            u32 pw{}, ph{};
            display_pixels(w, h, r.dpi, kPhotoW, &pw, &ph);
            const bool party = row.message_id == kGifMessage;
            if (pw != photo_w || ph != photo_h || party != photo_party) {
                if (party)
                    paint_party(g_photo, pw, ph, 1);
                else
                    paint_landscape(g_photo, pw, ph);
                photo_w = pw, photo_h = ph, photo_party = party;
            }
            ok = upload_png(r, media_asset_attachment(attachment.id, format, tier), g_photo, photo_w,
                            photo_h) &&
                 ok;
        }
        for (u32 e = 0; e < rich->embed_count; ++e) {
            // The large image, or else the corner thumbnail of a link card.
            MessageRichEmbedMediaSelection selected{};
            float w = row.rich_item_layout.embed_media_widths[e], h = row.rich_item_layout.embed_media_heights[e];
            if (!message_rich_embed_media_select(f.rich, rich->embeds[e], &selected) &&
                !(message_rich_embed_thumbnail_extent(f.rich, rich->embeds[e], row.measured_width, &w, &h) &&
                  message_rich_embed_resource_select(f.rich, rich->embeds[e], MediaEmbedResource::thumbnail,
                                                     &selected)))
                continue;
            const auto tier =
                media_resolution_tier_for_display(w, h, r.dpi, media_preview_extent_cap(selected.format));
            const auto key = media_asset_embed(rich->message_id, rich->message_revision, static_cast<u8>(e),
                                               selected.resource, selected.format, selected.animated, tier,
                                               rich->forwarded_snapshot_child);
            u32 cw{}, ch{};
            display_pixels(w, h, r.dpi, kCardW, &cw, &ch);
            if (cw != card_w || ch != card_h) {
                paint_gallery(g_card, cw, ch);
                card_w = cw, card_h = ch;
            }
            ok = upload_png(r, key, g_card, card_w, card_h) && ok;
        }
    }
    return ok;
}

const UiLayoutNode* node(const UiScene& scene, UiSemantic semantic) noexcept {
    return ui_scene_node(scene, semantic);
}

// /gif's inline results, as the client floats them over the option bar: a
// heading and a row of reaction GIFs (each painted here), the first selected
// until a key steps on. Phase 1 loads (the spinner), 2 shows the row.
u32 g_gif_phase, g_gif_selected;
u64 g_clock_ms;
bool gif_resolve(void*, u32 i, UiMediaPickerItem* out) noexcept {
    if (!out || i >= kGifTiles)
        return false;
    *out = {};
    const auto key = media_asset_catalog_preview(0x7100 + i, 1, MediaAttachmentFormat::webp);
    out->id = 0x7100 + i;
    out->asset_id = key.id;
    out->variant = key.variant;
    out->asset_flags = key.flags;
    out->label = utf8_literal("Party GIF");
    out->enabled = true;
    out->kind = UiMediaPickerItemKind::gif;
    out->width = kGifSizes[i][0] / 3;
    out->height = kGifSizes[i][1] / 3;
    return true;
}
bool upload_gif_tiles(D3D11Renderer& r) noexcept {
    bool ok = true;
    for (u32 i = 0; i < kGifTiles; ++i) {
        paint_party(g_gif, kGifSizes[i][0], kGifSizes[i][1], i);
        ok = upload_rgba(r, media_asset_catalog_preview(0x7100 + i, 1, MediaAttachmentFormat::webp), g_gif,
                         kGifSizes[i][0], kGifSizes[i][1]) &&
             ok;
    }
    return check(ok, "gif_upload_failed");
}
bool place_gifs(Fixture& f, D3D11Renderer& r) noexcept {
    static u32 shown;
    if (!g_gif_phase) {
        r.media_picker_scene = nullptr;
        shown = 0;
        return true;
    }
    const auto* surface = ui_widget_manager_layout_node(f.composer_scene.widgets, f.composer_scene.surface);
    if (!check(surface != nullptr, "gif_anchor_missing"))
        return false;
    auto& content = f.gif_content;
    content.phase = g_gif_phase == 1 ? UiMediaPickerPhase::loading : UiMediaPickerPhase::ready;
    content.revision += shown != g_gif_phase;
    shown = g_gif_phase;
    const UiRect viewport{0, 0, r.width * 96.f / r.dpi, r.height * 96.f / r.dpi};
    if (!check(ui_media_picker_scene_update(&f.gifs, &f.gif_source, &content, viewport, surface->rectangle),
               "gif_strip_failed"))
        return false;
    if (g_gif_phase == 2)
        ui_media_picker_scene_select(&f.gifs, g_gif_selected);
    f.gifs.spinner_phase = ui_spinner_phase(g_clock_ms, false);
    r.media_picker_scene = &f.gifs;
    return true;
}

// "Show time on reply previews", on for the extras capture.
bool g_reply_timestamps;

// Lays the shell out at the renderer's size and converges message text.
bool project(Fixture& f, D3D11Renderer& r) noexcept {
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    // The typing line as the composer gets it from the typing store.
    MessageTypingName typing_names[3]{};
    const auto typed = message_typing_presentation_build(f.typing, f.users, kChannel, kSelf,
                                                         {f.typing_label, sizeof(f.typing_label)}, typing_names);
    const Utf8View typing{f.typing_label, typed.ok() ? typed.bytes : 0};
    // Pins and threads as a readable channel with history shows them.
    UiShellLayoutMetrics metrics = kGuildChannelShellLayoutMetrics;
    metrics.flags = static_cast<u16>(metrics.flags | ui_shell_layout_pins | ui_shell_layout_threads);
    const bool bar = f.command.command_bar_name.bytes != 0;
    metrics.composer_height =
        message_composer_scene_preferred_height(f.composer, ui_scene_composer_width(metrics, width), false,
                                                typing.bytes != 0) +
        (bar ? message_composer_scene_chrome_height(false, typing.bytes != 0, 0, nullptr, true) -
                   message_composer_scene_chrome_height(false, typing.bytes != 0, 0, nullptr, false)
             : 0.f);
    metrics.revision += 10'000u + ui_extent_units(metrics.composer_height) + metrics.flags * 7u;
    message_scene_set_display(&f.messages, {0, false, MessageSceneClock::meridiem, g_reply_timestamps, true});
    if (!navigation_view_rebuild(&f.view, f.state).ok() ||
        !ui_scene_layout_with_metrics(&f.shell, UiShellView::guild_channel, metrics, width, height))
        return check(false, "shell_layout_failed");
    const UiLayoutNode* rail = node(f.shell, UiSemantic::guild_navigation);
    const UiLayoutNode* channels = node(f.shell, UiSemantic::channel_navigation);
    const UiLayoutNode* body = node(f.shell, UiSemantic::message_view);
    const UiLayoutNode* compose = node(f.shell, UiSemantic::composer);
    const UiLayoutNode* pane = node(f.shell, UiSemantic::member_sidebar);
    MessageComposerProjectionContext& projection = f.command;
    projection.users = &f.users;
    projection.state = &f.state;
    projection.guild_id = kGuild;
    if (!check(rail && channels && body && compose && pane, "shell_nodes_missing") ||
        !check(guild_rail_scene_update(&f.rail, &f.view, rail->rectangle, 0).ok(), "rail_update_failed") ||
        !check(shell_navigation_scene_update(&f.navigation, &f.view, channels->rectangle, 0).ok(),
               "channels_update_failed") ||
        !check(message_scene_update(&f.messages, body->rectangle).ok(), "messages_update_failed") ||
        !check(message_composer_scene_update(&f.composer_scene, &f.composer, compose->rectangle, true, 0,
                                             {}, &projection, typing)
                   .ok(),
               "composer_update_failed") ||
        !check(member_list_scene_update(&f.member_scene, &f.member_view, &f.members, &f.users, &f.state,
                                        pane->rectangle, 0)
                   .ok(),
               "members_update_failed"))
        return false;
    // The header's search field, bound the way the conversation view binds it.
    UiRect search_viewport = ui_scene_content_rectangle(f.shell, UiSemantic::content);
    search_viewport.right = width;
    const UiShellHeaderGeometry header = ui_scene_header_geometry(f.shell);
    const UiLayoutNode* title = node(f.shell, UiSemantic::title_bar);
    if (!check(title && ui_search_scene_update(&f.search, nullptr, &f.search_content, search_viewport,
                                               title->rectangle, &header),
               "search_update_failed"))
        return false;
    // Built-in extras: the typing names take their members' role colours; reply previews show their time in
    // the extras capture only.
    f.composer_scene.typing_name_count = 0;
    for (u32 i = 0; typed.ok() && typed.users <= 3 && i < typed.users; ++i) {
        const GuildMemberRecord* member = guild_member_store_find(f.members, kGuild, typing_names[i].user_id);
        f.composer_scene.typing_names[f.composer_scene.typing_name_count++] = {
            typing_names[i].offset, typing_names[i].bytes, member ? member->role_color & 0xFFFFFFu : 0u};
    }
    if (!place_gifs(f, r))
        return false;
    r.search_scene = &f.search;
    f.composer_scene.typing_dots = typing.bytes != 0;
    message_composer_scene_set_caret_visible(&f.composer_scene, false);
    if (!renderer_bind_ui_scene(&r, &f.shell) || !renderer_bind_guild_rail_scene(&r, &f.rail) ||
        !renderer_bind_shell_navigation_scene(&r, &f.navigation) ||
        !renderer_bind_message_scene(&r, &f.messages) ||
        !renderer_bind_message_composer_scene(&r, &f.composer_scene) ||
        !renderer_bind_member_list_scene(&r, &f.member_scene))
        return check(false, "scene_bind_failed");
    renderer_bind_navigation_view(&r, &f.state, &f.users, &f.view);
    r.retained_presentation.valid = false;
    content_text_retention_invalidate(&r.content_text_retention);
    const auto result = message_text_coordinator_converge(
        {&f.messages, &f.presentation, r.message_measure_cache, &f.state, &f.users, &f.view, &f.theme,
         +[](void* owner, const Theme& theme) noexcept {
             return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner), theme);
         },
         &r, kNowSeconds, static_cast<u16>(r.dpi), nullptr});
    if (!result.ok()) {
        log_u64(LogLevel::error, "showcase", "converge_error", static_cast<u32>(result.error));
        log_u64(LogLevel::error, "showcase", "converge_scene_error", static_cast<u32>(result.scene_error));
        log_u64(LogLevel::error, "showcase", "converge_presentation_error",
                static_cast<u32>(result.presentation_error));
        return check(false, "converge_failed");
    }
    // A missing image is counted but still leaves a usable frame.
    check(upload_rows(f, r), "media_upload_failed");
    return true;
}

ClientPreferences preferences(ClientTheme theme, BackgroundEffect effect) noexcept {
    ClientPreferences p = client_preferences_defaults();
    p.theme = theme;
    p.gradient = background_theme_defaults();
    p.gradient.effect = effect;
    // Turned up in the theme editor's own ranges (0-100) so the effect reads at a glance.
    if (effect != BackgroundEffect::none) {
        p.gradient.speed = 80;
        p.gradient.strength = 100;
        p.gradient.density = 80;
    }
    return p;
}

// Frame, then the caption over it, read back with the caption rows.
bool shot(Fixture& f, D3D11Renderer& r, const wchar_t* name, float phase = 6.f) noexcept {
    const UiCaptionBarState state{
        UiCaptionPart::none, UiCaptionPart::none, true, false, UiCaptionUpdate::hidden, false, 0};
    if (!renderer_set_caption(&r, state, utf8_literal("MaiCord Lab")) ||
        !renderer_set_background_phase(&r, phase) || !renderer_rebuild_content_text(&r, f.theme) ||
        !probe_present(&r, f.theme))
        return check(false, "present_failed");
    const float black[4]{0, 0, 0, 1};
    r.graphics.context->ClearRenderTargetView(r.graphics.target, black);
    if (!renderer_set_background_phase(&r, phase) ||
        !d3d11_renderer_draw_stream(&r, f.theme, true, true, nullptr, 10'000))
        return check(false, "draw_failed");
    r.present_full = true;
    d3d11_renderer_caption_pending(&r, f.theme);
    const bool caption = d3d11_renderer_caption_draw(&r, f.theme);
    r.present_full = false;
    ProbeTargetReadback image{};
    const bool ok = caption && !probe_target_readback(r, &image) && probe_write_capture(image, name);
    if (ok)
        log_write(LogLevel::info, "showcase", "captured");
    return check(ok, "capture_failed");
}

bool set_theme(Fixture& f, D3D11Renderer& r, ClientTheme theme,
               BackgroundEffect effect = BackgroundEffect::none) noexcept {
    f.theme = client_theme_projection(preferences(theme, effect));
    return project(f, r);
}

// Content size for a 1440x900 DIP window at `dpi`, the caption included.
bool size_window(Fixture& f, D3D11Renderer& r, u32 dpi) noexcept {
    const float scale = dpi / 96.f;
    renderer_set_dpi(&r, dpi);
    const u32 caption = static_cast<u32>(ui_caption_bar_height(f.theme) * scale + .5f);
    const u32 icon = static_cast<u32>(f.theme.scale.caption_icon * scale + .5f);
    renderer_set_caption_height(&r, caption);
    const UiCaptionBarState state{
        UiCaptionPart::none, UiCaptionPart::none, true, false, UiCaptionUpdate::hidden, false, 0};
    return check(renderer_resize(&r, static_cast<u32>(kWidth * scale),
                                 static_cast<u32>(kHeight * scale) - caption) &&
                     renderer_set_caption(&r, state, utf8_literal("MaiCord Lab")) &&
                     icon <= kAppIconLargestExtent &&
                     app_icon_decode(GetModuleHandleW(nullptr), icon, g_caption_icon) &&
                     renderer_set_caption_icon(&r, g_caption_icon, icon),
                 "window_size_failed");
}

// Milo's profile: bio, banner, roles and a game activity with a second card.
UserProfile* g_profile;
bool open_profile(Fixture& f, D3D11Renderer& r, bool modal) noexcept {
    ProfileScene& scene = f.profile;
    const float width = r.width * 96.f / r.dpi, height = r.height * 96.f / r.dpi;
    const UiRect viewport{0, 0, width, height};
    static bool decoded, decode_failed;
    if (decode_failed)
        return false;
    if (!decoded) {
        decode_failed = true;
        g_profile = static_cast<UserProfile*>(
            VirtualAlloc(nullptr, sizeof(UserProfile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        static u32 scratch[2048];
        char json[1024]{};
        const auto append = [&](u32* used, const char* s) noexcept {
            for (u32 i = 0; s[i] && *used + 1 < sizeof(json); ++i)
                json[(*used)++] = s[i];
        };
        u32 used = 0;
        append(&used,
               R"({"user":{"id":"12","username":"milo.makes","global_name":"Milo Reyes","avatar":")");
        append(&used, f.avatar_hashes[2]);
        append(
            &used,
            R"("},"user_profile":{"bio":"Pixel pusher and palette hoarder. I make the gradients you scroll )"
            R"(past, then argue about them in #design-crit.","pronouns":"he/him","banner":"5b4e7e1a2c3d4f5061)"
            R"(728394a5b6c7d8"},"guild_member":{"roles":["902"],"joined_at":"2025-03-14T12:00:00.000Z"}})");
        static UserActivitySet activities{};
        bool has = false;
        const auto presence = utf8_literal(R"({"activities":[
            {"type":0,"name":"Lumen Drift","application_id":"700000000000000001","details":"Chapter 3: Glass Canyon",
             "state":"Exploring","created_at":1700000000100,"timestamps":{"start":1700000000000},
             "party":{"id":"p","size":[2,4]},
             "assets":{"large_image":"700000000000000002","large_text":"Glass Canyon",
                       "small_image":"700000000000000003","small_text":"Cartographer"}},
            {"type":2,"name":"Lab Radio","details":"Night Shift","state":"Synthwave Collective",
             "created_at":1700000000200,"timestamps":{"start":1700000000000,"end":1700000240000},
             "assets":{"large_image":"mp:external/lab/https/lab.example/radio.png","large_text":"Late Hours"}}]})");
        // The profile decode and the presence scan share the scratch, in turn.
        bool ok =
            check(g_profile && user_profile_decode({json, used}, {scratch, 2048}, 12, kGuild, g_profile),
                  "profile_json_failed");
        const auto scan = json_scan_structurals_avx1(presence, {scratch, 2048});
        ok = ok && check(scan.ok() &&
                             user_activities_decode({presence, {scratch, scan.count}},
                                                    {presence, 0, JsonValueKind::object}, 12, &activities,
                                                    &has) &&
                             has && activities.count == 2,
                         "activity_json_failed");
        if (ok) {
            const u64 now = clock_unix_milliseconds();
            activities.items[0].start_ms = now - 2'754'000;
            activities.items[1].start_ms = now - 95'000;
            activities.items[1].end_ms = now + 145'000;
            auto& appearance = g_profile->user_appearance;
            const Utf8View activity = text(kPeople[2].activity);
            appearance.fields |= appearance_activity;
            copy_bytes(appearance.activity.data, activity.data, activity.bytes);
            appearance.activity.bytes = static_cast<u16>(activity.bytes);
            apply_tag(&appearance, kTags[1]);
            ok = user_activity_upsert(&f.users.activities, activities) &&
                 user_appearance_upsert(&f.users.appearance, appearance);
        }
        paint_banner(g_banner, kBannerW, kBannerH);
        paint_game_art(g_game, kArt);
        paint_record(g_record, kArt);
        if (!check(ok, "profile_decode_failed"))
            return false;
        decoded = true;
        decode_failed = false;
    }
    // Anchor the popout on Milo's member row, as a click there opens it.
    UiRect anchor{width - 240, 120, width - 8, 164};
    for (u32 i = 0; i < f.member_scene.count; ++i)
        if (f.member_scene.rows[i].user_id == 12)
            if (const auto* row =
                    member_list_scene_layout_node(f.member_scene, f.member_scene.rows[i].widget))
                anchor = row->rectangle;
    profile_scene_release(&scene);
    scene = {};
    scene.phase = ProfilePhase::ready;
    scene.measure = r.message_measure_cache;
    if (!profile_scene_open(&scene, g_profile, &f.state, &f.users, kSelf, anchor, viewport))
        return check(false, "profile_open_failed");
    scene.presence = UiPresence::online;
    if (modal) {
        scene.presentation = ProfilePresentation::modal;
        scene.tab = 0;
        if (!profile_scene_layout(&scene, viewport, true))
            return check(false, "profile_layout_failed");
    }
    ++scene.revision;
    for (u32 i = 0; i < scene.part_count; ++i) {
        ProfileMedia media{};
        if (!profile_media_resolve(scene, i, &media))
            continue;
        const auto kind = scene.parts[i].media;
        bool ok = true;
        if (kind == ProfileMediaKind::avatar)
            ok = upload_png(r, media.key, g_avatars[2], kAvatar, kAvatar);
        else if (kind == ProfileMediaKind::banner)
            ok = upload_png(r, media.key, g_banner, kBannerW, kBannerH);
        else if (kind == ProfileMediaKind::activity_large)
            ok = upload_png(r, media.key, scene.parts[i].source ? g_record : g_game, kArt, kArt);
        else if (kind == ProfileMediaKind::activity_small)
            ok = upload_png(r, media.key, g_avatars[5], kAvatar, kAvatar);
        else
            ok = upload_png(r, media.key, g_icons[1], kIcon, kIcon);
        check(ok, "profile_media_failed");
    }
    renderer_set_layer_opacity(&r, RendererLayerSurface::profile, 1);
    r.profile_scene = &scene;
    return true;
}

// Settings catalog rows at representative values (see user_settings_ui_probe).
constexpr ClientPreferences kPreferences = client_preferences_defaults();
SettingBinding binding_of(u64 id) noexcept {
    UiSettingsSchemaLocation location{};
    const auto* n = settings_control_location(id, &location) ? settings_catalog_node(location) : nullptr;
    return n ? static_cast<SettingBinding>(n->binding()) : SettingBinding::none;
}
bool palette(void*, u64 id, u32 choice, BackgroundTheme* output) noexcept {
    return settings_theme_swatch(kPreferences, binding_of(id), choice, output);
}
constexpr Utf8View kDevices[]{utf8_literal("Default"), utf8_literal("Speakers (Synthetic Audio)")};
constexpr Utf8View kGames[]{utf8_literal("Lumen Drift")};
constexpr Utf8View kThemes[]{utf8_literal("Light"), utf8_literal("Dark"), utf8_literal("AMOLED")};
u32 project_panel(u32 panel, UiSettingsControl* out, UiSettingsControlPresentation* looks) noexcept {
    const auto& nodes = settings_catalog_panel(static_cast<SettingsPanel>(panel)).nodes;
    u32 count = 0;
    for (u32 i = 0; i < nodes.count && count < 32; ++i) {
        const auto& n = nodes[i];
        if ((n.flags & ui_settings_schema_search_hidden) ||
            (n.kind == UiSettingsControlKind::category && (n.flags & ui_settings_schema_dynamic)))
            continue;
        if (n.kind == UiSettingsControlKind::category && n.binding())
            continue;
        auto& c = out[count];
        c = {settings_control_id(panel, i),
             n.label(),
             n.description(),
             n.kind,
             true,
             (i % 3) != 1,
             n.choices(),
             n.default_choice() < n.choices().count ? n.default_choice() : 0,
             n.action_label(),
             nullptr};
        if (n.kind == UiSettingsControlKind::choice && !c.choices.count)
            c.choices = n.binding() == static_cast<u32>(SettingBinding::add_game)
                            ? ConstSlice<Utf8View>{kGames, 1}
                            : ConstSlice<Utf8View>{kDevices, 2};
        if (n.kind == UiSettingsControlKind::palette && !c.choices.count)
            c.choices = {kThemes, 3};
        if (n.kind == UiSettingsControlKind::slider && c.choices.count > 120)
            c.selected_choice = c.choices.count * 2 / 3;
        looks[count] = {
            static_cast<UiSettingsVariant>(n.variant),
            static_cast<u8>(n.flags & ui_settings_schema_marks ? ui_settings_presentation_marks : 0),
            n.default_choice() < kUiSettingsNoDefault ? static_cast<u16>(n.default_choice())
                                                      : kUiSettingsNoDefault};
        SettingValue value{};
        if (n.kind == UiSettingsControlKind::palette &&
            settings_store_read(kPreferences, static_cast<SettingBinding>(n.binding()), &value))
            c.selected_choice = value.choice;
        ++count;
    }
    return count;
}
bool open_settings(Fixture& f, D3D11Renderer& r) noexcept {
    static bool registered;
    if (!registered) {
        bool ok = ui_page_registry_initialize(&f.registry, {f.pages, kSettingsPanelCount});
        for (u32 panel = 0; ok && panel < kSettingsPanelCount; ++panel) {
            const auto& source = settings_catalog_panel(static_cast<SettingsPanel>(panel));
            const u32 count = project_panel(panel, f.controls[panel], f.looks[panel]);
            f.contents[panel] = {source.title, {}, {f.controls[panel], count}, 1};
            f.contents[panel].presentation = f.looks[panel];
            ok = ui_page_registry_register(
                     &f.registry, {{kSettingsNativeOwner, panel + 1u}, source.title, &f.contents[panel]})
                     .ok();
        }
        ok = ok && ui_settings_scene_set_presentation(&f.settings, UiSettingsPresentation::modal) &&
             ui_settings_scene_bind_navigation(&f.settings, settings_catalog_navigation(true), 1) &&
             ui_settings_scene_bind_palette(&f.settings, {&f, palette, 1});
        if (!check(ok, "settings_registry_failed"))
            return false;
        registered = true;
    }
    const UiRect viewport{0, 0, r.width * 96.f / r.dpi, r.height * 96.f / r.dpi};
    const UiPageKey page{kSettingsNativeOwner, static_cast<u64>(SettingsPanel::appearance) + 1};
    renderer_set_layer_opacity(&r, RendererLayerSurface::settings_page, 1);
    renderer_bind_settings_scene(&r, &f.settings);
    return check(ui_page_registry_select(&f.registry, page).ok() &&
                     ui_settings_scene_update(&f.settings, &f.registry, viewport) &&
                     f.settings.content.controls.count,
                 "settings_open_failed");
}

wchar_t g_frame_name[64];
const wchar_t* frame_name(const wchar_t* stem, u32 index) noexcept {
    u32 n = 0;
    for (const wchar_t* s = L"showcase-anim-"; *s; ++s)
        g_frame_name[n++] = *s;
    for (const wchar_t* s = stem; *s; ++s)
        g_frame_name[n++] = *s;
    g_frame_name[n++] = L'-';
    g_frame_name[n++] = static_cast<wchar_t>(L'0' + index / 100 % 10);
    g_frame_name[n++] = static_cast<wchar_t>(L'0' + index / 10 % 10);
    g_frame_name[n++] = static_cast<wchar_t>(L'0' + index % 10);
    for (const wchar_t* s = L".bmp"; *s; ++s)
        g_frame_name[n++] = *s;
    g_frame_name[n] = 0;
    return g_frame_name;
}

// A message arriving as the Gateway pump applies MESSAGE_CREATE: the
// author's typing ends and the timeline inserts it, following the bottom.
bool arrive(Fixture& f, u64 id, u64 author, Utf8View content, u64 reply_to = 0, u64 reply_author = 0,
            Utf8View reply_name = {}, Utf8View reply_content = {}) noexcept {
    GatewayMessageEvent m{};
    m.id = id;
    m.channel_id = kChannel;
    m.author_id = author;
    m.timestamp_ms = snowflake_timestamp_ms(id);
    m.content = content;
    m.fields = gateway_message_field_id | gateway_message_field_channel_id |
               gateway_message_field_author_id | gateway_message_field_content |
               gateway_message_field_timestamp;
    if (reply_to) {
        m.fields |= gateway_message_field_message_reference | gateway_message_field_referenced_message;
        m.reference = {reply_to,   kChannel, kGuild,        reply_author,
                       reply_name, {},       reply_content, GatewayMessageReferenceState::available};
    }
    (void)typing_store_note_message(&f.typing, kChannel, author, false);
    return check(message_timeline_apply_created(&f.timeline, m).ok(), "message_create_failed");
}

// MESSAGE_REACTION_ADD into the rich store, as the pump applies it.
bool react(Fixture& f, u64 id, Utf8View emoji, bool self) noexcept {
    const auto found = message_store_find(f.store, id);
    const MessageRecord* record = found.ok() ? message_store_resolve(f.store, found.handle) : nullptr;
    return check(record &&
                     message_rich_store_mutate_reaction(&f.rich, id, record->revision,
                                                        {0, emoji, MessageReactionType::normal,
                                                         MessageRichReactionMutationKind::add, self, false})
                         .ok(),
                 "reaction_add_failed");
}

// The bot's deferred answer to /release: the attribution header with "is
// thinking...", then the release card and link buttons replace it.
bool answer_release(Fixture& f, u64 id, bool ready) noexcept {
    GatewayMessageEmbed card{};
    card.type = GatewayMessageEmbedType::rich;
    card.title = utf8_literal("MaiCord 0.5.2");
    card.url = utf8_literal("https://lab.example/releases/0.5.2");
    card.description = utf8_literal("Slash commands, bot buttons and sticker popouts. Voice encrypts in hardware "
                                    "and Opus decodes up to 25% faster.");
    card.color = 0x7c5cff;
    card.has_color = card.has_title = true;
    GatewayMessageComponent components[3]{};
    components[0].type = gateway_component_action_row;
    components[0].parent = 0xFFFFu;
    components[1].type = components[2].type = gateway_component_button;
    components[1].style = components[2].style = 5;
    components[1].label = utf8_literal("Release notes");
    components[1].url = utf8_literal("https://lab.example/releases/0.5.2");
    components[2].label = utf8_literal("Download");
    components[2].url = utf8_literal("https://lab.example/download");
    components[2].order = 1;
    GatewayMessageEvent m{};
    m.id = id;
    m.channel_id = kChannel;
    m.author_id = kBot;
    m.author_bot = true;
    m.timestamp_ms = snowflake_timestamp_ms(id);
    m.fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_author_id |
               gateway_message_field_content | gateway_message_field_timestamp | gateway_message_field_type |
               gateway_message_field_flags | gateway_message_field_interaction;
    m.type = 20;
    m.interaction = {id + 1, kSelf, utf8_literal("Mai"), {f.avatar_hashes[0], 32}, utf8_literal("release"), 2};
    if (!ready) {
        m.flags = 1u << 7; // The API's LOADING flag.
        return check(message_timeline_apply_created(&f.timeline, m).ok(), "bot_message_failed");
    }
    m.fields |= gateway_message_field_embeds | gateway_message_field_components;
    m.embeds = {&card, 1};
    m.components = {components, 3};
    return check(message_timeline_apply_updated(&f.timeline, m).ok(), "bot_answer_failed");
}

// The composer's picker as a slash opens it: the bot's commands and the
// built-in ones beside their rail, or under a title, what a query matches.
// list: the default discovery or /release query, what "/g" matches (/gif), or
// /gif's required option once the command is chosen.
enum class CommandList : u8 { release, gif, gif_option };
bool open_commands(Fixture& f, D3D11Renderer& r, Utf8View title = {},
                   CommandList list = CommandList::release) noexcept {
    const UiLayoutNode* compose =
        ui_widget_manager_layout_node(f.composer_scene.widgets, f.composer_scene.surface);
    if (!check(compose != nullptr, "picker_anchor_missing"))
        return false;
    const auto picture = [&](PopupItem& item) noexcept {
        item.flags |= popup_item_avatar;
        item.media_id = kBot;
        item.media_variant = user_avatar_variant({f.avatar_hashes[kPeopleCount - 1], 32});
    };
    const auto command = [](PopupItem& item, u32 action, Utf8View name, Utf8View detail) noexcept {
        item = {name, action, popup_item_enabled | popup_item_command_row, 0};
        item.detail = detail;
    };
    const auto release = utf8_literal("/release"),
               release_detail = utf8_literal("Post the release notes\x1FLab Bot\x1Fversion");
    PopupItem items[9]{};
    u32 count = 0;
    const bool query = title.bytes != 0;
    if (list == CommandList::gif) {
        items[count++] = {title, 1, popup_item_command_section, 0};
        command(items[count++], 2, utf8_literal("/gif"),
                utf8_literal("Search Animated GIFs on the Web\x1F"
                             "Built-in\x1Fquery"));
    } else if (list == CommandList::gif_option) {
        items[count++] = {utf8_literal("REQUIRED OPTIONS"), 1, popup_item_command_section, 0};
        items[count] = {utf8_literal("query"), 2, popup_item_enabled, 0};
        items[count++].detail = utf8_literal("Search for a GIF");
    } else if (query) {
        items[count++] = {title, 1, popup_item_command_section, 0};
        command(items[count], 2, release, release_detail);
        picture(items[count++]);
    } else {
        items[count] = {utf8_literal("Lab Bot"), 1,
                        popup_item_enabled | popup_item_command_category | popup_item_checked, 0};
        picture(items[count++]);
        items[count] = {utf8_literal("Built-in"), 2,
                        popup_item_enabled | popup_item_command_category | popup_item_separator_before, 0};
        items[count++].presentation = ui_menu_row_presentation(UiIcon::command);
        items[count] = {utf8_literal("Lab Bot"), 3, popup_item_command_section, 0};
        picture(items[count++]);
        command(items[count++], 4, release, release_detail);
        command(items[count++], 5, utf8_literal("/nightly"),
                utf8_literal("Show the latest nightly build\x1FLab Bot"));
        command(items[count++], 6, utf8_literal("/poll"),
                utf8_literal("Ask the channel a question\x1FLab Bot\x1Fquestion answers"));
        items[count] = {utf8_literal("Built-in"), 7, popup_item_command_section, 0};
        items[count++].presentation = ui_menu_row_presentation(UiIcon::command);
        command(items[count++], 8, utf8_literal("/shrug"),
                utf8_literal("Appends \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF to your message.\x1F"
                             "Built-in\x1Fmessage"));
    }
    const UiRect viewport{0, 0, r.width * 96.f / r.dpi, r.height * 96.f / r.dpi};
    if (!check(popup_scene_open(&f.popup, PopupKind::autocomplete, kChannel, {items, count}, compose->rectangle,
                                viewport, ui_rect_width(compose->rectangle), true)
                   .ok(),
               "picker_open_failed"))
        return false;
    f.popup.selected_index = query || list != CommandList::release ? 1 : 3;
    renderer_set_layer_opacity(&r, RendererLayerSurface::popup, 1);
    return check(renderer_bind_popup_scene(&r, &f.popup), "picker_bind_failed");
}
// The popup alone; the draft stays.
void close_popup(Fixture& f, D3D11Renderer& r) noexcept {
    renderer_bind_popup_scene(&r, nullptr);
    popup_scene_close(&f.popup);
}
void close_commands(Fixture& f, D3D11Renderer& r) noexcept {
    close_popup(f, r);
    message_composer_clear(&f.composer);
    f.command = {};
    g_gif_phase = g_gif_selected = 0;
}

// The selected /gif as the composer projects it: the option bar, then the
// query chip once its key is typed, holding `query_bytes` of value.
void gif_command(Fixture& f, bool chip, u32 query_bytes) noexcept {
    auto& c = f.command;
    c.command.name_bytes = 4;
    c.option_chip_count = chip ? 1 : 0;
    if (chip) {
        const u16 end = static_cast<u16>(11 + query_bytes);
        c.option_chips[0] = {4, 5, 11, end, end, message_composer_chip_active};
    }
    c.command_bar_name = chip ? utf8_literal("query") : utf8_literal("/gif");
    c.command_bar_detail =
        chip ? utf8_literal("Search for a GIF") : utf8_literal("Search Animated GIFs on the Web");
}

// Mai sends the second result: the GIF arrives as an image on her message.
bool send_gif(Fixture& f) noexcept {
    GatewayMessageAttachment gif{};
    gif.id = 0x5c0002;
    gif.size = 96'412;
    gif.filename = utf8_literal("party-blob.gif");
    gif.content_type = utf8_literal("image/gif");
    gif.url = utf8_literal("https://cdn.discordapp.com/attachments/311/6029314/party-blob.gif");
    gif.width = 400;
    gif.height = 300;
    gif.has_width = gif.has_height = true;
    GatewayMessageEvent m{};
    m.id = kGifMessage;
    m.channel_id = kChannel;
    m.author_id = kSelf;
    m.timestamp_ms = snowflake_timestamp_ms(kGifMessage);
    m.fields = gateway_message_field_id | gateway_message_field_channel_id |
               gateway_message_field_author_id | gateway_message_field_content |
               gateway_message_field_timestamp | gateway_message_field_attachments;
    m.attachments = {&gif, 1};
    return check(message_timeline_apply_created(&f.timeline, m).ok(), "gif_message_failed");
}

// The animated hero: 10.7 s at 12.5 fps under Aurora with its effect running.
// Juniper finishes typing and posts, reactions land and Ada replies. Mai then
// types /gif: the picker narrows to it, its option bar and query chip appear,
// the results open above them and the second one is sent. Last she types a
// slash, picks /release and the bot thinks and answers. The clock, dots and
// effect phase are fixed per frame, so every run draws the same frames.
void hero_loop(Fixture& f, D3D11Renderer& r) noexcept {
    constexpr u32 kFrames = 134;
    constexpr float kStep = .08f;
    const u64 juniper = snowflake(0, 10), ada = snowflake(0, 20), release = snowflake(0, 30);
    constexpr u32 kFrameMs = 80, kThinking = 113, kAnswer = 121;
    constexpr Utf8View fire = utf8_literal("\xF0\x9F\x94\xA5"), eyes = utf8_literal("\xF0\x9F\x98\x8D");
    constexpr Utf8View rocket = utf8_literal("\xF0\x9F\x9A\x80"), party = utf8_literal("\xF0\x9F\x8E\x89");
    constexpr Utf8View juniper_text =
        utf8_literal("<@14> the comets at 80% speed look amazing with Ocean \xF0\x9F\x8C\x8A");
    if (!set_theme(f, r, ClientTheme::aurora, BackgroundEffect::stars_and_comets))
        return;
    // What the popup lists now, the draft's first frame and its query length.
    enum class Popup : u8 { none, discover, release, gif, gif_option } popup = Popup::none;
    Utf8View title{};
    u32 composing_from = 0, popup_from = 0, query_bytes = 0;
    bool composing = false;
    const auto insert = [&](const char* s) noexcept {
        message_composer_insert_utf8(&f.composer, text(s));
    };
    const auto open = [&](Popup next, const char* heading, u32 frame) noexcept {
        popup = next;
        title = text(heading);
        popup_from = frame;
    };
    const auto start_composing = [&](u32 frame) noexcept {
        f.composer.focused = true;
        composing = true;
        composing_from = frame;
    };
    for (u32 frame = 0; frame < kFrames; ++frame) {
        bool changed = true;
        g_clock_ms = u64{frame} * kFrameMs;
        f.composer_scene.typing_phase = f.messages.interaction_phase =
            ui_typing_dots_phase(g_clock_ms, true);
        switch (frame) {
        case 8:
            arrive(f, juniper, 13, juniper_text);
            break;
        case 13:
            react(f, juniper, fire, false);
            break;
        case 16:
            react(f, juniper, eyes, false);
            break;
        case 20:
            check(typing_store_start(&f.typing, kChannel, kGuild, 11, kNowSeconds * 1000 + 3'500).ok(),
                  "typing_failed");
            break;
        case 30:
            arrive(f, ada, 11, utf8_literal("Love it. Let's ship it Friday \xF0\x9F\x9A\x80"), juniper, 13,
                   utf8_literal("Juniper"), juniper_text);
            break;
        case 35:
            react(f, ada, rocket, true);
            break;
        case 38:
            react(f, ada, rocket, false);
            break;
        // Mai types /gif: the picker opens on the slash and narrows as she types.
        case 42:
            start_composing(frame);
            insert("/");
            open(Popup::discover, "", frame);
            break;
        case 47:
            insert("g");
            open(Popup::gif, "Commands matching /g", frame);
            break;
        case 49:
            insert("i");
            open(Popup::gif, "Commands matching /gi", frame);
            break;
        case 51:
            insert("f");
            open(Popup::gif, "Commands matching /gif", frame);
            break;
        // The command is chosen: its option bar shows, with the required option listed.
        case 54:
            insert(" ");
            gif_command(f, false, 0);
            open(Popup::gif_option, "", frame);
            break;
        // The option is picked: its chip opens and the list closes.
        case 58:
            insert("query:");
            gif_command(f, true, 0);
            popup = Popup::none;
            close_popup(f, r);
            break;
        case 60:
        case 62:
        case 64:
        case 66:
        case 68:
        {
            const char letter[2]{"party"[query_bytes], 0};
            insert(letter);
            f.composer.focused = true;
            gif_command(f, true, ++query_bytes);
            break;
        }
        // After the 300 ms debounce the results load, then show.
        case 72:
            g_gif_phase = 1;
            break;
        case 75:
            g_gif_phase = 2;
            break;
        case 81:
            g_gif_selected = 1;
            break;
        case 87:
            composing = false;
            f.composer.focused = false;
            close_commands(f, r);
            send_gif(f);
            query_bytes = 0;
            break;
        case 92:
        case 95:
            react(f, kGifMessage, party, false);
            break;
        // Then a slash, /rel and the bot's answer.
        case 99:
            start_composing(frame);
            insert("/");
            open(Popup::discover, "", frame);
            break;
        case 104:
            insert("r");
            open(Popup::release, "Commands matching /r", frame);
            break;
        case 106:
            insert("e");
            open(Popup::release, "Commands matching /re", frame);
            break;
        case 108:
            insert("l");
            open(Popup::release, "Commands matching /rel", frame);
            break;
        case kThinking:
            composing = false;
            popup = Popup::none;
            f.composer.focused = false;
            close_commands(f, r);
            answer_release(f, release, false);
            break;
        case kAnswer:
            answer_release(f, release, true);
            break;
        case 127:
            react(f, release, party, false);
            break;
        case 130:
            react(f, release, rocket, false);
            break;
        default:
            // "is thinking..." is message text, so its dots lay out again.
            changed = frame > kThinking && frame < kAnswer;
        }
        if ((changed || composing) && !project(f, r))
            return;
        if (composing) {
            message_composer_scene_set_caret_visible(&f.composer_scene,
                                                     (frame - composing_from) / 6 % 2 == 0);
            r.retained_presentation.valid = false;
        }
        if (popup != Popup::none) {
            open_commands(f, r, title,
                          popup == Popup::gif          ? CommandList::gif
                          : popup == Popup::gif_option ? CommandList::gif_option
                                                       : CommandList::release);
            if (frame - popup_from < 2 && popup == Popup::discover)
                renderer_set_layer_opacity(&r, RendererLayerSurface::popup,
                                           frame == popup_from ? .5f : 1.f);
        }
        shot(f, r, frame_name(L"hero", frame), 6.f + frame * kStep);
    }
}

// MAICORD_SHOWCASE_THEME names a theme that changes with the time of day (the catalog's Silk Waves theme) and
// MAICORD_SHOWCASE_EFFECT, optionally, the .mcfx it asks for: then the probe takes only the main window at 03:00,
// 08:00, 12:00, 18:00 and 21:00 in April and October (showcase-day-MM-HH.bmp), with the theme's look and the
// effect's mc_ambient as MaiCord sets them at that moment.
u8* read_whole(const wchar_t* variable, u32 maximum, u32* bytes) noexcept {
    wchar_t path[600];
    const DWORD length = GetEnvironmentVariableW(variable, path, 600);
    if (!length || length >= 600)
        return nullptr;
    auto* data = static_cast<u8*>(VirtualAlloc(nullptr, maximum, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD read = 0;
    const bool ok = data && file != INVALID_HANDLE_VALUE && ReadFile(file, data, maximum, &read, nullptr) && read;
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (!ok && data)
        VirtualFree(data, 0, MEM_RELEASE);
    *bytes = read;
    return ok ? data : nullptr;
}
u32 g_theme_scratch[kClientThemeDocumentMaximumBytes / 2];
ClientThemeDocument g_day_theme;
McfxWorkspace g_effect_workspace;
bool theme_day(Fixture& f, D3D11Renderer& r, bool* taken) noexcept {
    u32 bytes = 0;
    u8* text = read_whole(L"MAICORD_SHOWCASE_THEME", kClientThemeDocumentMaximumBytes, &bytes);
    *taken = text != nullptr;
    if (!text)
        return true;
    const bool parsed = client_theme_document_parse({reinterpret_cast<const char*>(text), bytes},
                                                    client_theme_document_defaults(),
                                                    {g_theme_scratch, kClientThemeDocumentMaximumBytes / 2},
                                                    &g_day_theme)
                            .ok() &&
                        client_theme_document_scheduled(g_day_theme);
    VirtualFree(text, 0, MEM_RELEASE);
    if (!check(parsed, "day_theme_parse_failed"))
        return false;
    u32 package_bytes = 0;
    u8* package = read_whole(L"MAICORD_SHOWCASE_EFFECT", 2u << 20, &package_bytes);
    const McfxPackage& effect = g_effect_workspace.package;
    BackgroundEffectSpec spec{};
    if (package) {
        if (!check(mcfx_parse({package, package_bytes}, &g_effect_workspace).ok(), "day_effect_parse_failed"))
            return false;
        McfxKnobValue values[kMcfxKnobMaximum];
        mcfx_knob_defaults(effect, values);
        mcfx_knob_pack(effect, values, spec.knobs);
        spec.bytecode = effect.shaders[0];
        spec.identity = hash64_bytes(kHash64Seed, package, package_bytes) | 1;
        spec.uses = effect.uses;
        spec.blend = effect.blend;
        if (effect.graph.pass_count) {
            spec.graph = &effect.graph;
            spec.shaders = effect.shaders;
        }
    }
    constexpr u32 kMonths[]{4, 10}, kHours[]{3, 8, 12, 18, 21};
    for (const u32 month : kMonths)
        for (const u32 hour : kHours) {
            f.theme = client_theme_document_at(g_day_theme, hour * 60, month, 3);
            ui_theme_resolve_emphasis(&f.theme);
            float phase = 6.f;
            if (package) {
                // A fresh selection each time, so the cloth starts over, then two seconds of it at 30 fps.
                renderer_clear_effect(&r);
                if (!check(renderer_select_effect(&r, spec, {1.f, 1.f, spec.blend, 4, 2, EffectBudget::high, false})
                                   .state != EffectSelect::rejected &&
                               r.effect.selected,
                           "day_effect_select_failed"))
                    return false;
                r.effect.step = r.effect.budget.step = r.effect.budget.ceiling = 4;
                r.effect.budget.warmup = 0x7fffffff;
                EffectAmbient ambient{};
                float inputs[4];
                // The 3rd: Silk Waves turns toward the next month's colours from the 5th.
                const EffectAmbientSources sources{{2026, static_cast<u16>(month), 3, static_cast<u16>(hour), 0},
                                                   f.theme.appearance != ThemeAppearance::light, false, true};
                renderer_set_effect_light(&r, effect_ambient_sample(&ambient, effect.uses, sources, 1000, false, false,
                                                                    inputs));
                renderer_set_effect_ambient(&r, inputs);
            }
            if (!project(f, r))
                return false;
            for (u32 step = 0; package && step < 60; ++step, phase += 1.f / 30)
                if (!check(renderer_set_background_phase(&r, phase) &&
                               d3d11_renderer_draw_stream(&r, f.theme, true, true, nullptr, 10'000),
                           "day_effect_frame_failed"))
                    return false;
            wchar_t name[40] = L"showcase-day-MM-HH.bmp";
            name[13] = static_cast<wchar_t>(L'0' + month / 10);
            name[14] = static_cast<wchar_t>(L'0' + month % 10);
            name[16] = static_cast<wchar_t>(L'0' + hour / 10);
            name[17] = static_cast<wchar_t>(L'0' + hour % 10);
            shot(f, r, name, phase);
        }
    if (package)
        VirtualFree(package, 0, MEM_RELEASE);
    return g_failures == 0;
}
bool setup(Fixture& f, D3D11Renderer& r, MessageTextPresentation& presentation) noexcept {
    f.theme = kDefaultTheme;
    // Atlases that hold every image at both scales; nothing is uploaded yet.
    renderer_unbind_media_atlas(&r);
    bool ok = media_atlas_initialize(&f.atlas, 1024, 1024, 1, {f.atlas_entries, 64}, {f.atlas_buckets, 128},
                                     {f.atlas_allocations, 64}, {f.atlas_rows, 32}) &&
              media_atlas_initialize(&f.preview_atlas, 4096, 4096, 1, {f.preview_entries, 64},
                                     {f.preview_buckets, 128}, {f.preview_allocations, 64},
                                     {f.preview_rows, 32}) &&
              renderer_bind_media_atlas(&r, &f.atlas) &&
              renderer_bind_preview_media_atlas(&r, &f.preview_atlas);
    if (!check(ok, "atlas_failed") || !size_window(f, r, 96) || !build_state(f) || !build_people(f, r) ||
        !build_messages(f))
        return false;
    ok = navigation_view_initialize(&f.view, {f.guild_rows, kGuilds}, {f.channel_rows, kChannels},
                                    {f.channel_scratch, kChannels}, {f.channel_visibility, kChannels},
                                    {f.channel_keys, kChannels}) &&
         ui_scene_initialize_bootstrap(&f.shell) &&
         guild_rail_scene_initialize(&f.rail, {f.rail_rows, kGuilds}, {f.rail_widgets, kGuilds + 2},
                                     {f.rail_nodes, kGuilds + 2}) &&
         shell_navigation_scene_initialize(&f.navigation, {f.navigation_rows, kChannels},
                                           {f.navigation_widgets, kChannels * 4 + 2},
                                           {f.navigation_nodes, kChannels * 4 + 2}) &&
         message_composer_initialize(&f.composer, {f.composer_text, sizeof(f.composer_text)}) &&
         message_composer_scene_initialize_owned(&f.composer_scene) &&
         member_list_scene_initialize(&f.member_scene, {f.member_scene_rows, 48}, {f.member_widgets, 193},
                                      {f.member_nodes, 193}) &&
         popup_scene_initialize_owned(&f.popup, 32, {f.popup_widgets, 70}, {f.popup_nodes, 70});
    if (!check(ok, "scene_initialize_failed"))
        return false;
    message_composer_reset(&f.composer, kChannel, false);
    f.search_content.edit = {{}, 0, 0, true, false, false};
    f.search_content.heading = f.search_content.placeholder = utf8_literal("Search MaiCord Lab");
    f.search_content.revision = 1;
    build_folders(f);
    f.gif_source = {nullptr, 0x72, 1, kGifTiles, 1, {}, gif_resolve};
    auto& gifs = f.gif_content;
    gifs.account_scope = f.gif_source.account_scope;
    gifs.query_generation = f.gif_source.query_generation;
    gifs.revision = 1;
    gifs.kind = UiMediaPickerKind::gif;
    gifs.available_tabs = ui_media_picker_tab_bit(UiMediaPickerKind::gif);
    gifs.variant = UiMediaPickerVariant::inline_results;
    gifs.heading = utf8_literal("Gif content matching party");
    gifs.heading_plain = 21;
    if (!upload_gif_tiles(r))
        return false;
    // Juniper is typing when the stills are taken; the hero loop starts there.
    if (!check(typing_store_initialize(&f.typing, {f.typing_records, 8}) &&
                   typing_store_start(&f.typing, kChannel, kGuild, 13, kNowSeconds * 1000).ok(),
               "typing_failed"))
        return false;
    f.composer.focused = false;
    f.member_scene.measure = r.message_measure_cache;
    // Borrow the fixture's command storage; the bound presentation starts empty.
    f.presentation = presentation;
    f.presentation.active = false;
    f.presentation.projection_signature = 0;
    f.presentation.command_count = f.presentation.measurement_count = 0;
    // Rebinding clears the renderer's measure cache; keep the fixture's.
    UiTextMeasureCache* measure = r.message_measure_cache;
    renderer_unbind_message_text_presentation(&r);
    const bool bound = message_text_presentation_bind_guild_members(&f.presentation, &f.members) &&
                       renderer_bind_message_text_presentation(&r, &f.presentation);
    r.message_measure_cache = measure;
    return check(bound, "presentation_bind_failed");
}

} // namespace

bool showcase_ui_probe(D3D11Renderer& r, MessageTextPresentation& presentation) noexcept {
    Fixture& f = fixture;
    if (!r.message_text || !r.message_measure_cache || !setup(f, r, presentation))
        return false;
    bool day_only = false;
    if (!theme_day(f, r, &day_only) || day_only)
        return g_failures == 0;
    constexpr u32 kDpis[]{96, 192};
    for (u32 dpi : kDpis) {
        const bool two = dpi == 192;
        if (!size_window(f, r, dpi))
            return false;
        // The hero in each theme; Aurora with its stars and comets.
        if (set_theme(f, r, ClientTheme::dark))
            shot(f, r, two ? L"showcase-hero-dark@2x.bmp" : L"showcase-hero-dark@1x.bmp");
        if (set_theme(f, r, ClientTheme::light))
            shot(f, r, two ? L"showcase-hero-light@2x.bmp" : L"showcase-hero-light@1x.bmp");
        if (set_theme(f, r, ClientTheme::aurora, BackgroundEffect::stars_and_comets))
            shot(f, r, two ? L"showcase-hero-aurora@2x.bmp" : L"showcase-hero-aurora@1x.bmp");
        if (set_theme(f, r, ClientTheme::dark)) {
            g_reply_timestamps = true;
            if (project(f, r))
                shot(f, r, two ? L"showcase-extras@2x.bmp" : L"showcase-extras@1x.bmp");
            g_reply_timestamps = false;
            project(f, r);
            if (open_profile(f, r, false))
                shot(f, r, two ? L"showcase-profile-popout@2x.bmp" : L"showcase-profile-popout@1x.bmp");
            if (open_profile(f, r, true))
                shot(f, r, two ? L"showcase-profile-full@2x.bmp" : L"showcase-profile-full@1x.bmp");
            r.profile_scene = nullptr;
            if (open_settings(f, r))
                shot(f, r,
                     two ? L"showcase-settings-appearance@2x.bmp" : L"showcase-settings-appearance@1x.bmp");
            renderer_bind_settings_scene(&r, nullptr);
            message_composer_insert_utf8(&f.composer, utf8_literal("/"));
            if (project(f, r) && open_commands(f, r))
                shot(f, r, two ? L"showcase-commands@2x.bmp" : L"showcase-commands@1x.bmp");
            close_commands(f, r);
        }
        if (two)
            continue;
        // One frame per theme for the theme-switch loop.
        const ClientTheme themes[]{ClientTheme::dark, ClientTheme::light, ClientTheme::aurora,
                                   ClientTheme::sunset, ClientTheme::ocean};
        for (u32 i = 0; i < 5; ++i)
            if (set_theme(f, r, themes[i],
                          i >= 2 ? BackgroundEffect::stars_and_comets : BackgroundEffect::none))
                shot(f, r, frame_name(L"themes", i));
    }
    // Last, since it changes the conversation: the animated hero at 1x, then
    // where it ends, with the bot's answer on screen.
    if (size_window(f, r, 96))
        hero_loop(f, r);
    for (u32 dpi : kDpis)
        if (size_window(f, r, dpi) && set_theme(f, r, ClientTheme::sunset, BackgroundEffect::stars_and_comets))
            shot(f, r, dpi == 192 ? L"showcase-bot@2x.bmp" : L"showcase-bot@1x.bmp");
    r.media_picker_scene = nullptr;
    ui_media_picker_scene_release(&f.gifs);
    log_u64(g_failures ? LogLevel::error : LogLevel::info, "showcase", "failures", g_failures);
    return g_failures == 0;
}

} // namespace maicord
