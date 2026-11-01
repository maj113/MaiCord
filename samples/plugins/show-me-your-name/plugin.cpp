// Show Me Your Name: shows usernames beside display names and nicknames.
#include <maicord/sdk/plugin.hpp>
using namespace mc;

MC_SETTINGS((Choice, place, "Username goes", 0, .items = {"After the name", "Before the name"}),
            (Toggle, in_members, "Also in the member list", true),
            (Toggle, skip_same, "Skip names that already match", true));

static void decorate(Ctx& ctx, const User& user, Decoration& d) {
    const Settings& s = ctx.settings();
    if (user.username().empty() || (user.where == MC_USER_MEMBER_ROW && !s.in_members))
        return;
    if (s.skip_same && user.name().equals_ascii_nocase(user.username()))
        return;
    if (s.place == 1)
        d.prefix(user.username(), Tone::role);
    else
        d.suffix(user.username());
}

// Message authors and member rows; profiles already show the username.
MC_HOOKS(mc::filter<decorate, (1u << MC_USER_MESSAGE_AUTHOR) | (1u << MC_USER_MEMBER_ROW)>);
MC_PLUGIN(.owner = 0x6D63'736D'796E'616D, .name = "Show Me Your Name", .version = "1.0.0",
          .summary = "Shows usernames beside display names and nicknames.");
