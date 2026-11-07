// Every profile card shape lays out and projects text commands the renderer
// accepts: the popout and the full profile, dark and light, with and without
// a server tag, pronouns, theme colours, a bio, roles, a join date, a note,
// a decoration and the guild context, and the card a failed profile fetch
// shows from cached user and member data.
#include "maicord/core/memory.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/model/user/profile.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/ui/account/profile_scene.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::profile_text_projection_test {
u32 failures = 0;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}
template <u32 N> void set_text(ProfileString<N>* out, Utf8View value) noexcept {
    copy_bytes(out->data, value.data, value.bytes);
    out->bytes = static_cast<u16>(value.bytes);
}

// A server (7) with forty roles the viewer cannot edit.
struct Guild {
    GuildRecord guilds[1];
    u16 order[1];
    RoleRecord roles[40];
    u8 names[1024];
    ChatStateStore state;
};
void make_guild(Guild* g) noexcept {
    g->state.guilds = g->guilds;
    g->state.guild_order = g->order;
    g->state.guild_count = 1;
    g->state.roles = g->roles;
    g->state.role_count = 40;
    g->state.names = g->names;
    g->state.name_bank_bytes = sizeof(g->names);
    g->state.self_user_id = 1;
    g->guilds[0].id = 7;
    g->guilds[0].owner_id = 99;
    for (u32 i = 0; i < 40; ++i) {
        auto& role = g->roles[i];
        role.id = 100 + i;
        role.guild_id = 7;
        role.position = i + 1;
        role.color = 0x3366CC;
        role.name_offset = i * 16;
        role.name_bytes = 12;
        copy_bytes(g->names + i * 16, "Role name xx", 12);
    }
}

struct Projection {
    bool laid_out, projected, nodes_placed;
    u32 commands;
    bool name_shown, notice_shown;
};
// Lays the card out and projects its text through the renderer's checks. Every
// part that carries text must have a laid-out rectangle.
Projection project(ProfileScene* s, const UiRect& viewport, const Theme& theme, Utf8View name) noexcept {
    Projection result{};
    result.laid_out = profile_scene_layout(s, viewport, true);
    result.nodes_placed = s->part_count > 0;
    for (u32 i = 0; i < s->part_count; ++i) {
        const auto* node = ui_widget_manager_layout_node(s->widgets, s->parts[i].widget);
        if (s->parts[i].text.bytes)
            result.nodes_placed &= node && ui_rect_has_area(node->rectangle);
        result.name_shown |= utf8_equal(s->parts[i].text, name);
        result.notice_shown |= utf8_equal(s->parts[i].text, utf8_literal("Unable to load parts of profile"));
    }
    UiTextEmitter emitter{};
    UiTextOutput output{nullptr, &emitter};
    result.projected = ui_text_emitter_initialize(&emitter) && profile_scene_text_output(*s, theme, &output, nullptr);
    result.commands = emitter.count;
    return result;
}

void test_shapes(const Guild& guild) noexcept {
    const UiRect viewports[]{{0, 0, 1930, 1282}, {0, 0, 1280, 720}, {0, 0, 900, 600}};
    const Theme* themes[]{&kDefaultTheme, &kLightTheme};
    bool all = true, member_modal = true, member_tag_modal = true;
    for (u32 shape = 0; shape < 256; ++shape) {
        UserStore users{};
        UserProfile profile{};
        profile.user_id = 42;
        profile.guild_id = 7;
        set_text(&profile.username, utf8_literal("member"));
        if (shape & 1)
            set_text(&profile.display_name, utf8_literal("Member Name"));
        if (shape & 2) {
            profile.user_appearance.primary_enabled = true;
            profile.user_appearance.primary_guild_id = 9;
            set_text(&profile.user_appearance.primary_tag, utf8_literal("YURI"));
        }
        if (shape & 4)
            set_text(&profile.pronouns, utf8_literal("they/them"));
        if (shape & 8) {
            profile.premium_type = 2;
            profile.has_theme = true;
            profile.theme_colors[0] = 0xE0C8E8;
            profile.theme_colors[1] = 0xC8A8D8;
        }
        if (shape & 16)
            set_text(&profile.bio, utf8_literal("Hello there"));
        if (shape & 64)
            for (u32 i = 0; i < 40; ++i)
                profile.roles[profile.role_count++] = guild.roles[i].id;
        if (shape & 128) {
            set_text(&profile.joined_at, utf8_literal("2024-01-02T03:04:05.000000+00:00"));
            set_text(&profile.user_appearance.decoration, utf8_literal("a_decoration"));
        }
        const auto name = shape & 1 ? profile.display_name.view() : profile.username.view();
        for (const auto& viewport : viewports)
            for (const Theme* theme : themes)
                for (u32 modal = 0; modal < 2; ++modal) {
                    ProfileScene s{};
                    s.phase = ProfilePhase::ready;
                    s.note_visible = (shape & 128) != 0;
                    const bool opened = profile_scene_open(&s, &profile, shape & 32 ? &guild.state : nullptr, &users, 1,
                                                           {300, 200, 340, 240}, viewport);
                    s.presentation = modal ? ProfilePresentation::modal : ProfilePresentation::popout;
                    const auto r = project(&s, viewport, *theme, name);
                    const bool ok = opened && r.laid_out && r.projected && r.nodes_placed && r.name_shown && r.commands;
                    all &= ok;
                    // A server member none of whose roles the store knows: the full
                    // profile's Roles heading was added and dropped again, and the
                    // next part reused its widget ID.
                    if (modal && (shape & (32 | 64)) == 32) {
                        member_modal &= ok;
                        if (shape & 2)
                            member_tag_modal &= ok;
                    }
                    profile_scene_release(&s);
                }
    }
    check(member_modal, "profile projection: the full profile of a member without known roles lays out");
    check(member_tag_modal, "profile projection: that member's server tag chip projects an ordered rectangle");
    check(all, "profile projection: every shape lays out, places every text part and projects valid commands");
}

// A profile fetch that failed (404 Unknown User, 403): the card shows what
// the client cached, the member's nickname and server tag, and the notice;
// the fetch brought no roles.
void test_failed_fetch(const Guild& guild) noexcept {
    const Theme* themes[]{&kDefaultTheme, &kLightTheme};
    for (u32 modal = 0; modal < 2; ++modal)
        for (const Theme* theme : themes) {
            UserStore users{};
            UserProfile profile{};
            profile.user_id = 42;
            profile.guild_id = 7;
            set_text(&profile.username, utf8_literal("cached.member"));
            set_text(&profile.display_name, utf8_literal("Cached Member"));
            set_text(&profile.nickname, utf8_literal("Server Nick"));
            profile.user_appearance.primary_enabled = true;
            profile.user_appearance.primary_guild_id = 9;
            set_text(&profile.user_appearance.primary_tag, utf8_literal("B16"));
            ProfileScene s{};
            s.phase = ProfilePhase::failed;
            s.http_status = 404;
            const UiRect viewport{0, 0, 1930, 1282};
            const bool opened = profile_scene_open(&s, &profile, &guild.state, &users, 1, {300, 200, 340, 240}, viewport);
            s.presentation = modal ? ProfilePresentation::modal : ProfilePresentation::popout;
            const auto r = project(&s, viewport, *theme, profile.nickname.view());
            check(opened && r.laid_out && r.projected && r.nodes_placed && r.name_shown && r.notice_shown &&
                      r.commands >= 3,
                  modal ? "profile projection: a failed fetch's full profile shows the cached nickname, tag and notice"
                        : "profile projection: a failed fetch's popout shows the cached nickname, tag and notice");
            profile_scene_release(&s);
        }
}

void run() noexcept {
    static Guild guild{};
    make_guild(&guild);
    test_shapes(guild);
    test_failed_fetch(guild);
}
} // namespace maicord::profile_text_projection_test

extern "C" __declspec(noreturn) void WINAPI maicord_profile_text_projection_entry() noexcept {
    maicord::profile_text_projection_test::run();
    ExitProcess(maicord::profile_text_projection_test::failures ? 1 : 0);
}
