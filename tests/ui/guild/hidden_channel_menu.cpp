#include "maicord/ui/guild/hidden_channel_menu.hpp"
#include "../../support/model_store_fixtures.hpp"
#include "maicord/model/guild/permission_bits.hpp"

namespace maicord {
namespace {
bool starts(Utf8View text, Utf8View prefix) noexcept {
    return text.bytes >= prefix.bytes && utf8_equal(Utf8View{text.data, prefix.bytes}, prefix);
}
} // namespace

// "Show hidden channels": the explanation rows come from the cached roles and overwrites alone.
void test_hidden_channel_menu(void (*check)(bool, const char*) noexcept) noexcept {
    GuildRecord guild{};
    ChannelRecord channels[3]{};
    RoleRecord roles[16]{};
    PermissionOverwriteRecord overwrites[16]{};
    u16 guild_order[1]{}, channel_order[3]{};
    u8 names[1024]{};
    ChatStateStore state{};
    UserRecord user_records[2]{};
    u32 user_order[2]{};
    u8 user_names[64]{};
    UserStore users{};
    constexpr u64 view = kPermissionViewChannel;
    bool built = chat_state_store_initialize(&state, {&guild, 1}, {guild_order, 1}, {channels, 3}, {channel_order, 3},
                                             {names, sizeof(names)}) &&
                 chat_state_store_bind_permissions(&state, {roles, 16}, {overwrites, 16}) &&
                 chat_state_store_upsert_guild(&state, {1, {}, GuildMutationKind::create, 99}).ok() &&
                 chat_state_store_upsert_role(&state, {1, 1, view, utf8_literal("@everyone")}) &&
                 chat_state_store_upsert_role(&state, {8, 1, view, utf8_literal("Mods"), 3, 0x112233u}) &&
                 chat_state_store_upsert_role(&state, {9, 1, kPermissionAdministrator, utf8_literal("Admin"), 5}) &&
                 chat_state_store_upsert_role(&state, {10, 1, view, utf8_literal("Bot"), 9, 0, false, true}) &&
                 chat_state_store_upsert_role(&state, {12, 1, 0, utf8_literal("Plain"), 2});
    for (u64 id = 30; id < 36; ++id)
        built = built && chat_state_store_upsert_role(&state, {id, 1, 0, utf8_literal("Extra"), 1});
    ChannelInput secret{20, 1, 0, utf8_literal("secret"), 0, 0, 0};
    secret.last_message_id = 1'700'000'000'000ull << 22;
    secret.last_message_known = true;
    built = built && chat_state_store_set_self_roles(&state, 1, 99, 42, {}) &&
            chat_state_store_upsert_channel(&state, secret).ok() &&
            chat_state_store_upsert_channel(&state, {21, 1, 0, utf8_literal("open"), 1, 0, 0}).ok() &&
            chat_state_store_append_channel_overwrite(&state, 20, {1, 0, view, 0}) &&
            chat_state_store_append_channel_overwrite(&state, 20, {8, view, 0, 0}) &&
            chat_state_store_append_channel_overwrite(&state, 20, {55, view, 0, 1});
    for (u64 id = 30; id < 36; ++id)
        built = built && chat_state_store_append_channel_overwrite(&state, 20, {id, view, 0, 0});
    built = built && user_store_initialize(&users, {user_records, 2}, {user_order, 2}, {user_names, 64}) &&
            user_store_upsert(&users, 55, utf8_literal("Dana")).ok();
    chat_state_store_recompute_guild_permissions(&state, 1);
    check(built, "hidden channel menu: fixture");

    const auto* hidden = chat_state_store_find_channel(state, 20);
    const auto* open = chat_state_store_find_channel(state, 21);
    char text[kHiddenChannelMenuTextBytes];
    TextBank bank{text, sizeof(text), 0};
    PopupItem rows[kHiddenChannelMenuRows]{};
    check(hidden && open && chat_state_store_channel_hidden(state, *hidden) &&
              !chat_state_store_channel_hidden(state, *open) &&
              hidden_channel_menu_build(state, users, *open, 1'800'000'000, &bank, {rows, kHiddenChannelMenuRows}) == 0,
          "hidden channel menu: only a hidden channel explains itself");
    const u32 count = hidden ? hidden_channel_menu_build(state, users, *hidden, 1'800'000'000, &bank,
                                                         {rows, kHiddenChannelMenuRows})
                             : 0;
    check(count == 11 && utf8_equal(rows[0].label, utf8_literal("secret")) &&
              utf8_equal(rows[0].detail, utf8_literal("You can't view this channel")) &&
              ui_menu_row_icon(rows[0].presentation) == UiIcon::lock && starts(rows[1].label, utf8_literal("Created ")) &&
              starts(rows[2].label, utf8_literal("Last message ")),
          "hidden channel menu: name, lock, creation and last message come first");
    check(count == 11 && utf8_equal(rows[3].label, utf8_literal("Who can view it")) &&
              (rows[3].flags & popup_item_separator_before) && utf8_equal(rows[4].label, utf8_literal("Admin")) &&
              utf8_equal(rows[5].label, utf8_literal("Mods")) && rows[5].media_flags == kPopupItemMediaIconTint &&
              rows[5].media_variant == 0x112233u && utf8_equal(rows[6].label, utf8_literal("Extra")) &&
              utf8_equal(rows[8].label, utf8_literal("Extra")),
          "hidden channel menu: roles that resolve View Channel, highest first, Admin before Mods");
    check(count == 11 && utf8_equal(rows[9].label, utf8_literal("Dana")) &&
              utf8_equal(rows[10].label, utf8_literal("and 3 more")),
          "hidden channel menu: a named member follows, the rest collapse into a count");
    bool no_noise = true;
    for (u32 i = 0; i < count; ++i)
        no_noise = no_noise && !utf8_equal(rows[i].label, utf8_literal("Bot")) &&
                   !utf8_equal(rows[i].label, utf8_literal("Plain")) &&
                   !utf8_equal(rows[i].label, utf8_literal("@everyone"));
    check(no_noise, "hidden channel menu: managed roles, roles without access and @everyone never list");
    bool distinct = count != 0;
    for (u32 i = 0; i < count; ++i) {
        distinct = distinct && rows[i].action != 0 && !(rows[i].flags & popup_item_enabled);
        for (u32 j = 0; j < i; ++j)
            distinct = distinct && rows[j].action != rows[i].action;
    }
    check(distinct, "hidden channel menu: every row has its own action and none is enabled");
    PopupItem small[2]{};
    check(hidden_channel_menu_build(state, users, *hidden, 0, &bank, {small, 2}) == 0,
          "hidden channel menu: too little output builds nothing");
}
} // namespace maicord
