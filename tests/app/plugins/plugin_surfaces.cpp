#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/plugins/surfaces.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/plugins/render.hpp"
#include "maicord/ui/plugins/slots.hpp"
#include "plugins/surfaces_internal.hpp"
#include "../../fixtures/plugin_surfaces_fixture.h"
#include "../../support/test_output.hpp"
#include "plugin_fixture.hpp"

#include "maicord/core/win32_kernel.hpp"

// Decorations and buttons against real DLLs: the surfaces fixture (tests/fixtures/plugin_surfaces_fixture.c) and a
// second decorating plugin built from the same file. The UI queries are read exactly as the scenes read them.
namespace maicord {
// tests/ui/plugins/surface_scenes.cpp
void test_plugin_surface_scenes(void (*check)(bool, const char*) noexcept) noexcept;
} // namespace maicord

namespace maicord::plugin_surfaces_tests {
using namespace plugin_surfaces_detail;
using namespace plugin_fixture;

u32 failures;
void check(bool value, const char* label) noexcept {
    if (value) return;
    ++failures;
    test_write_text("FAIL plugin_surfaces: ");
    test_write_line(label);
}

constexpr u64 kSecondOwner = SURFACES_OWNER + 1;

PluginEnableResult enable(PluginRuntime* runtime, const wchar_t* name) noexcept {
    return plugin_runtime_enable(runtime, {name, length(name)}, MC_PERM_KNOWN);
}

PluginMessageInput message(u64 id, const char* content, u32 revision = 1, u64 guild = 7) noexcept {
    PluginMessageInput input{};
    input.message_id = id;
    input.channel_id = 100;
    input.guild_id = guild;
    input.author_id = 9;
    input.revision = revision;
    input.content = {content, length(content)};
    return input;
}
bool prepare(const PluginMessageInput& input, PluginDecoratePass* pass = nullptr) noexcept {
    PluginDecoratePass local{};
    return g_plugin_surfaces->message(pass ? pass : &local, input);
}
const UiPluginRenderDecoration* decoration(u64 id, u32 revision = 1) noexcept { return ui_plugin_render(id, revision); }
surfaces_state* state_of(PluginBinding* binding) noexcept { return static_cast<surfaces_state*>(binding->ctx.state); }

u32 slots(u32 surface, u64 channel, u64 message_id, u64 user, u64 guild, UiPluginSlotNode* nodes, u32 capacity) noexcept {
    return ui_plugin_slots(surface, channel, message_id, user, guild, {nodes, capacity});
}

// ---- Tests ---------------------------------------------------------------------------

// With nothing enabled the UI has no query and the application no table: every surface is one null test.
void test_no_plugins() noexcept {
    check(!g_plugin_surfaces && !g_ui_plugin_render_query && !g_ui_plugin_slot_query,
          "nothing is installed before a decorating plugin is enabled");
    check(!ui_plugin_render(5, 1) && !ui_plugin_slot_surface_active(kUiPluginSlotChannelRow),
          "the UI reads nothing without a query");
}

void test_enable(PluginRuntime* runtime, PluginBinding** out) noexcept {
    const PluginEnableResult enabled = enable(runtime, L"surfaces.dll");
    check(enabled.ok() && enabled.binding, "the surfaces plugin is enabled");
    if (!enabled.ok()) return;
    *out = enabled.binding;
    check(g_plugin_surfaces && g_ui_plugin_render_query && g_ui_plugin_slot_query, "the queries are installed");
    check(g_plugin_surfaces->wants == 0x1f && g_plugin_surfaces->message_needs == MC_NEED_CONTENT,
          "every kind is wanted and only content is needed");
    const u32 mask = g_ui_plugin_slot_surfaces;
    const u32 expected = (1u << kUiPluginSlotMessageAccessory) | (1u << kUiPluginSlotComposerToolbar) | (1u << kUiPluginSlotMemberBadge) |
                         (1u << kUiPluginSlotProfileSection) | (1u << kUiPluginSlotHeaderToolbar) |
                         (1u << kUiPluginSlotChannelRow) | (1u << kUiPluginSlotGuildRail) |
                         (1u << kUiPluginSlotAccountPanel) | (1u << kUiPluginSlotMessageHover) |
                         (1u << kUiPluginSlotServerTop) | (1u << kUiPluginSlotServerBottom) |
                         (1u << kUiPluginSlotMemberListHeader) | (1u << kUiPluginSlotMemberName);
    check(mask == expected, "the slot surface mask names every surface the plugin touches");
    check(state_of(enabled.binding)->start_status == MC_OK, "button_state works inside start");
}

void test_messages(PluginBinding* binding) noexcept {
    surfaces_state* state = state_of(binding);
    check(prepare(message(1, "hide")), "a verdict is cached after one call");
    check(state->message_calls == 1, "the hook ran once");
    const auto* hidden = decoration(1);
    check(hidden && hidden->visibility == ui_plugin_render_hidden, "hide hides the message");
    check(!decoration(1, 2), "a verdict answers only its revision");
    prepare(message(1, "hide"));
    check(state->message_calls == 1, "a current verdict is not asked again");
    prepare(message(1, "hide", 2));
    check(state->message_calls == 2 && decoration(1, 2) && decoration(1, 2)->visibility == ui_plugin_render_hidden,
          "a new revision asks again");

    prepare(message(2, "collapse"));
    const auto* collapsed = decoration(2);
    check(collapsed && collapsed->visibility == ui_plugin_render_collapsed && same(collapsed->collapsed_label, "Spoiler"),
          "collapse keeps the label");
    check(g_plugin_surfaces->reveal(2) && !g_plugin_surfaces->reveal(2), "a message is revealed once");
    check(decoration(2) && decoration(2)->visibility == ui_plugin_render_normal, "a revealed message shows normally");

    prepare(message(3, "blur"));
    check(decoration(3) && decoration(3)->media == ui_plugin_media_blur, "blur blurs media");
    prepare(message(4, "hidemedia"));
    check(decoration(4) && decoration(4)->media == ui_plugin_media_hide, "hidemedia hides media");

    prepare(message(5, "affix"));
    const auto* affix = decoration(5);
    check(affix && same(affix->name_prefix, "[P]") && same(affix->name_suffix, "(S)") && affix->name_tone == kUiPluginToneRole,
          "name prefix, suffix and role tone");
    check(affix && affix->badge_count == 1 && same(affix->badges[0].text, "B") && same(affix->badges[0].tooltip, "Badge tip") &&
              affix->badges[0].icon == static_cast<u16>(UiIcon::favorite) && affix->badges[0].tone == 1 &&
              affix->badges[0].owner_id == SURFACES_OWNER,
          "a badge with its icon, tone and owner");

    prepare(message(6, "rows"));
    const auto* rows = decoration(6);
    check(rows && rows->accessory_count == 3 && rows->accessories[0].kind == kUiPluginRenderLabel &&
              rows->accessories[1].kind == kUiPluginRenderButton && rows->accessories[1].action_id == 21 &&
              rows->accessories[2].kind == kUiPluginRenderLink && same(rows->accessories[2].value, "https://example.com/docs"),
          "accessory rows: a label, a button and a link");

    prepare(message(7, "card"));
    const auto* card = decoration(7);
    check(card && card->has_card && same(card->card.title, "Card title") &&
              same(card->card.text, "Card text line one\nline two") && same(card->card.caption, "a caption") &&
              same(card->card.link, "https://example.com/card") && card->card.button_count == 2 &&
              card->card.buttons[0].action_id == 11 && same(card->card.buttons[1].label, "Dismiss") &&
              card->card.buttons[1].tone == 4 && card->card.tone == 1 && card->card.owner_id == SURFACES_OWNER &&
              card->accessory_count == 1,
          "a card with two buttons below its accessories");

    prepare(message(8, "replace"));
    check(decoration(8) && decoration(8)->replace_content && same(decoration(8)->content, "**Replaced** text"),
          "replacement content");

    prepare(message(9, "many"));
    check(decoration(9) && decoration(9)->badge_count == 4 && decoration(9)->accessory_count == 8,
          "the node limits fit");

    const u32 before = state->message_calls;
    prepare(message(10, "bad"));
    check(state->message_calls == before + 1 && !decoration(10), "an invalid answer decorates nothing");
    prepare(message(10, "bad"));
    check(state->message_calls == before + 1, "and is not asked every pass");
    prepare(message(11, "fail"));
    check(!decoration(11), "a failing hook decorates nothing");
    prepare(message(12, "plain"));
    check(!decoration(12), "an undecorated message has no verdict");
    check(!decoration(0), "message 0 is never decorated");

    const u32 calls = state->message_calls;
    prepare(message(13, "hide", 1, 0));
    check(state->message_calls == calls && !decoration(13), "the guild-only filter skips direct messages");
}

void test_budget(PluginBinding* binding) noexcept {
    surfaces_state* state = state_of(binding);
    const u32 calls = state->message_calls;
    PluginDecoratePass pass{};
    pass.budget_us = 1;
    pass.spent_us = 1;
    const bool complete = prepare(message(20, "hide"), &pass);
    check(!complete && pass.exhausted && state->message_calls == calls && !decoration(20),
          "an exhausted pass defers the call");
    PluginDecoratePass fresh{};
    check(prepare(message(20, "hide"), &fresh) && fresh.calls == 1 && decoration(20) && state->message_calls == calls + 1 &&
              fresh.budget_us == kPluginDecoratePassBudgetUs,
          "the next pass asks");

    const u32 strikes = binding->strikes;
    prepare(message(21, "slow"));
    check(binding->strikes == strikes + 1 && !decoration(21), "a call over budget is a strike and its answer is dropped");
    const u32 slow_calls = state->message_calls;
    prepare(message(21, "slow"));
    check(state->message_calls == slow_calls, "an overrun is not asked again");
}

void test_ring(PluginBinding* binding) noexcept {
    // 300 full records overflow the 96 KiB ring and its 256 entries: old verdicts drop out, never garbage.
    PluginDecoratePass pass{};
    pass.budget_us = ~0ull;
    const u64 base = 1000;
    for (u32 i = 0; i < 300; ++i) prepare(message(base + i, "many"), &pass);
    u32 resident = 0;
    bool intact = true;
    for (u32 i = 0; i < 300; ++i) {
        const auto* d = decoration(base + i);
        if (!d) continue;
        ++resident;
        intact = intact && d->badge_count == 4 && d->accessory_count == 8 && same(d->badges[3].text, "M") &&
                 same(d->accessories[7].text, "Many");
    }
    check(intact && resident >= 100 && resident < 300, "an overflowing cache keeps whole recent verdicts");
    check(decoration(base + 299) != nullptr, "the newest verdict is resident");
    (void)binding;
}

void test_pull_invalidate_push(PluginRuntime* runtime, PluginBinding* binding) noexcept {
    (void)runtime;
    surfaces_state* state = state_of(binding);
    prepare(message(30, "hide"));
    u32 calls = state->message_calls;
    // The plugin's own invalidate: the entry keeps answering until the next pass asks again.
    check(plugin_surfaces_invalidate(binding, MC_INVALIDATE_MESSAGE, 30) == MC_OK, "invalidate accepts a message");
    check(decoration(30) != nullptr, "an invalidated verdict still answers");
    prepare(message(30, "hide"));
    check(state->message_calls == calls + 1, "an invalidated message is asked again");
    calls = state->message_calls;
    check(plugin_surfaces_invalidate(binding, MC_INVALIDATE_MESSAGE, 0) == MC_OK, "invalidate accepts every message");
    prepare(message(30, "hide"));
    check(state->message_calls == calls + 1, "invalidating every message asks again");
    check(plugin_surfaces_invalidate(binding, 99, 0) == MC_E_INVALID, "an unknown invalidate kind is invalid");

    // A push replaces the pull verdict and is never asked over.
    mc_decoration pushed{};
    pushed.accessory_count = 1;
    pushed.accessories[0] = {MC_NODE_LABEL, 0, {"Pushed", 6, 0}, {}, {}, {}, 0};
    calls = state->message_calls;
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 30, 100, &pushed) == MC_OK, "a push is stored");
    check(decoration(30) && decoration(30)->accessory_count == 1 && same(decoration(30)->accessories[0].text, "Pushed") &&
              decoration(30)->visibility == ui_plugin_render_normal,
          "the pushed verdict answers");
    prepare(message(30, "hide", 5));
    check(state->message_calls == calls && decoration(30, 5) && decoration(30, 5)->accessory_count == 1,
          "a pushed verdict answers every revision and the hook is not asked");
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 30, 100, nullptr) == MC_OK && !decoration(30, 5),
          "a null push clears it");
    prepare(message(30, "hide", 5));
    check(state->message_calls == calls + 1 && decoration(30, 5), "and the hook is asked again");

    // A push for a message the plugin has no hook verdict for (no pull at all).
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 31, 100, &pushed) == MC_OK && decoration(31, 9),
          "a push needs no pull");

    // Rejected pushes change nothing.
    mc_decoration bad{};
    bad.accessory_count = 1;
    bad.accessories[0] = {MC_NODE_BUTTON, 0, {"Tagless", 7, 0}, {}, {}, {}, 0};
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 32, 100, &bad) == MC_E_INVALID && !decoration(32, 1),
          "a button without a tag is invalid");
    mc_decoration oversized{};
    oversized.accessory_count = 1;
    oversized.accessories[0] = {MC_NODE_BADGE, 0, {"This badge text is longer than thirty-two bytes", 47, 0}, {}, {}, {}, 0};
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 32, 100, &oversized) == MC_E_INVALID,
          "a badge past 32 bytes is invalid");
    mc_decoration insecure{};
    insecure.accessory_count = 1;
    insecure.accessories[0] = {MC_NODE_LINK, 0, {"x", 1, 0}, {}, {}, {"http://example.com", 18, 0}, 0};
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 32, 100, &insecure) == MC_E_INVALID,
          "a link must be https");
    mc_decoration control{};
    control.accessory_count = 1;
    control.accessories[0] = {MC_NODE_LABEL, 0, {"a\x01z", 3, 0}, {}, {}, {}, 0};
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 32, 100, &control) == MC_E_INVALID,
          "control characters are invalid");
    mc_decoration tone{};
    tone.accessory_count = 1;
    tone.accessories[0] = {MC_NODE_LABEL, MC_TONE_ROLE, {"role", 4, 0}, {}, {}, {}, 0};
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 32, 100, &tone) == MC_E_INVALID,
          "the role tone is for name affixes only");
    mc_decoration card{};
    card.has_card = 1;
    card.card.title = {"t", 1, 0};
    card.card.button_count = 1;
    card.card.buttons[0].label = {"go", 2, 0};
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 32, 100, &card) == MC_E_INVALID,
          "a card button needs a tag");
    card.card.buttons[0].tag = kButtonAction | 1;
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 32, 100, &card) == MC_E_INVALID,
          "a tag may not use bit 63, which names descriptor buttons");
    card.card.buttons[0].tag = 5;
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 32, 100, &card) == MC_OK && decoration(32, 1) &&
              decoration(32, 1)->has_card,
          "a card with a tagged button is stored");
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 0, 0, &pushed) == MC_E_INVALID &&
              plugin_surfaces_push(binding, MC_HOOK_DECORATE_USER + 20, 1, 0, &pushed) == MC_E_INVALID,
          "an unknown target is invalid");
    plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 31, 100, nullptr);
    plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 32, 100, nullptr);
}

void test_entities(PluginBinding* binding) noexcept {
    surfaces_state* state = state_of(binding);
    UiPluginSlotNode nodes[8];
    PluginDecoratePass pass{};
    pass.budget_us = ~0ull;

    PluginUserInput odd{};
    odd.user_id = 31;
    odd.where = MC_USER_MEMBER_ROW;
    odd.guild_id = 7;
    check(g_plugin_surfaces->user(&pass, odd), "a user is prepared");
    check(slots(kUiPluginSlotMemberBadge, 0, 0, 31, 0, nodes, 8) == 1 && same(nodes[0].text, "Fan") &&
              nodes[0].kind == ui_plugin_node_badge && nodes[0].icon == static_cast<u16>(UiIcon::favorite) &&
              nodes[0].owner_id == SURFACES_OWNER,
          "an odd user's member row shows a badge");
    PluginUserInput even = odd;
    even.user_id = 30;
    g_plugin_surfaces->user(&pass, even);
    check(slots(kUiPluginSlotMemberBadge, 0, 0, 30, 0, nodes, 8) == 0, "an even user shows nothing");
    const u32 user_calls = state->user_calls;
    g_plugin_surfaces->user(&pass, odd);
    check(state->user_calls == user_calls, "a current user is not asked again");
    PluginUserInput present = odd;
    present.presence = MC_PRESENCE_ONLINE;
    present.platforms = MC_PLATFORM_MOBILE;
    g_plugin_surfaces->user(&pass, present);
    check(state->user_calls == user_calls + 1, "a presence change asks again");
    check(!(binding->permissions & MC_PERM_READ_ACTIVITY) && state->last_user_presence == MC_PRESENCE_UNKNOWN &&
              state->last_user_platforms == 0,
          "a plugin without the activity permission sees no presence or devices");
    PluginUserInput author = odd;
    author.where = MC_USER_MESSAGE_AUTHOR;
    g_plugin_surfaces->user(&pass, author);
    check(state->user_calls == user_calls + 1, "the surface filter skips message authors");

    PluginUserInput profile = odd;
    profile.where = MC_USER_PROFILE;
    g_plugin_surfaces->user(&pass, profile);
    check(slots(kUiPluginSlotProfileSection, 0, 0, 31, 0, nodes, 8) == 2 && same(nodes[0].text, "Profile row") &&
              nodes[1].kind == ui_plugin_node_button && nodes[1].action_id == 31,
          "a profile shows its rows");

    // Clicking the profile button reaches NODE_ACTION with its source.
    PluginSurfaceTarget target{kUiPluginSlotProfileSection, 0, 0, 31, 0};
    check(g_plugin_surfaces->activate(SURFACES_OWNER, target, 31, nullptr), "a profile button click is handled");
    check(state->action_calls == 1 && state->last_action_tag == 31 && state->last_action_user == 31 &&
              state->last_action_kind == MC_NODE_BUTTON && state->last_action_source == MC_HOOK_DECORATE_USER &&
              (state->last_action_flags & MC_CTX_GESTURE),
          "the action carries the tag, user and source in a gesture");
    check(!g_plugin_surfaces->activate(SURFACES_OWNER, target, 32, nullptr), "a tag the plugin does not show is refused");
    check(!g_plugin_surfaces->activate(SURFACES_OWNER + 5, target, 31, nullptr), "an unknown owner is refused");
    check(state->action_calls == 1, "refused clicks reach nobody");

    PluginChannelInput channel{};
    channel.channel_id = 55;
    channel.guild_id = 7;
    channel.flags = MC_CHANNEL_UNREAD;
    check(g_plugin_surfaces->channel(&pass, channel) &&
              slots(kUiPluginSlotChannelRow, 55, 0, 0, 0, nodes, 8) == 1 && same(nodes[0].text, "new") && nodes[0].tone == 2,
          "an unread channel row shows a badge");
    PluginChannelInput read = channel;
    read.channel_id = 56;
    read.flags = 0;
    g_plugin_surfaces->channel(&pass, read);
    check(slots(kUiPluginSlotChannelRow, 56, 0, 0, 0, nodes, 8) == 0, "a read channel shows nothing");

    PluginGuildInput guild{};
    guild.guild_id = 70;
    guild.flags = MC_GUILD_UNREAD;
    check(g_plugin_surfaces->guild(&pass, guild) && slots(kUiPluginSlotGuildRail, 0, 0, 0, 70, nodes, 8) == 1 &&
              nodes[0].kind == ui_plugin_node_icon && nodes[0].icon == static_cast<u16>(UiIcon::bell) &&
              same(nodes[0].tooltip, "Unread"),
          "an unread server shows an icon badge");

    PluginHeaderInput header{};
    header.channel_id = 55;
    header.where = MC_HEADER_MEMBER_LIST;
    header.member_count = 120;
    header.online_count = 17;
    check(g_plugin_surfaces->header(&pass, header) &&
              slots(kUiPluginSlotMemberListHeader, 0, 0, 0, 0, nodes, 8) == 1 && same(nodes[0].text, "17 online"),
          "the member list header shows its label");
    header.online_count = 18;
    g_plugin_surfaces->header(&pass, header);
    check(slots(kUiPluginSlotMemberListHeader, 0, 0, 0, 0, nodes, 8) == 1 && same(nodes[0].text, "18 online"),
          "a count change asks again");
    PluginHeaderInput title = header;
    title.where = MC_HEADER_CHANNEL;
    g_plugin_surfaces->header(&pass, title);
    const u32 header_nodes = slots(kUiPluginSlotHeaderToolbar, 0, 0, 0, 0, nodes, 8);
    check(header_nodes == 2 && nodes[0].kind == ui_plugin_node_button && (nodes[0].action_id & kButtonAction) &&
              same(nodes[1].text, "Fixture header"),
          "the channel header shows its button, then its decoration");

    // A push to a user, channel, guild and header.
    mc_decoration push{};
    push.badge_count = 1;
    push.badges[0] = {MC_NODE_BADGE, 0, {"P", 1, 0}, {}, {}, {}, 0};
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_USER, 40, 0, &push) == MC_OK &&
              slots(kUiPluginSlotMemberBadge, 0, 0, 40, 0, nodes, 8) == 1,
          "a pushed user decoration covers its member row");
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_CHANNEL, 77, 0, &push) == MC_OK &&
              slots(kUiPluginSlotChannelRow, 77, 0, 0, 0, nodes, 8) == 1,
          "a pushed channel decoration");
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_GUILD, 78, 0, &push) == MC_OK &&
              slots(kUiPluginSlotGuildRail, 0, 0, 0, 78, nodes, 8) == 1,
          "a pushed guild decoration");
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_HEADER, 0, 0, &push) == MC_OK &&
              slots(kUiPluginSlotMemberListHeader, 0, 0, 0, 0, nodes, 8) == 1 && same(nodes[0].text, "P"),
          "a header push with id 0 covers both headers");
    check(plugin_surfaces_push(binding, MC_HOOK_DECORATE_HEADER, 0, 0, nullptr) == MC_OK &&
              slots(kUiPluginSlotMemberListHeader, 0, 0, 0, 0, nodes, 8) == 0,
          "clearing a push leaves nothing until the hook answers again");
    g_plugin_surfaces->header(&pass, header);
    check(slots(kUiPluginSlotMemberListHeader, 0, 0, 0, 0, nodes, 8) == 1 && same(nodes[0].text, "18 online"),
          "and the pulled header comes back");
    plugin_surfaces_push(binding, MC_HOOK_DECORATE_USER, 40, 0, nullptr);
    plugin_surfaces_push(binding, MC_HOOK_DECORATE_CHANNEL, 77, 0, nullptr);
    plugin_surfaces_push(binding, MC_HOOK_DECORATE_GUILD, 78, 0, nullptr);
}

u32 shown(u32 surface, u64 message_id, UiPluginSlotNode* nodes, u32 capacity) noexcept {
    return ui_plugin_slot_toolbar(surface, message_id, {nodes, capacity});
}

void test_buttons(PluginBinding* binding) noexcept {
    surfaces_state* state = state_of(binding);
    UiPluginSlotNode nodes[8];

    // Message-independent buttons show from enable; the one whose predicate says no does not.
    check(shown(kUiPluginSlotComposerToolbar, 0, nodes, 8) == 1 && same(nodes[0].text, "Fixture toggle") &&
              nodes[0].action_id == (kButtonAction | SURFACES_BUTTON_CHAT_TOGGLE) && nodes[0].icon == static_cast<u16>(UiIcon::languages) &&
              !nodes[0].state && !nodes[0].badge && nodes[0].tone == 0,
          "the chat bar shows its toggle and not the hidden button");
    check(shown(kUiPluginSlotAccountPanel, 0, nodes, 8) == 1 && same(nodes[0].text, "Fixture account") && !nodes[0].state,
          "the account panel button, whose start-time state was cleared again");
    check(shown(kUiPluginSlotServerTop, 0, nodes, 8) == 1 && same(nodes[0].text, "Fixture top") &&
              shown(kUiPluginSlotServerBottom, 0, nodes, 8) == 1 && same(nodes[0].text, "Fixture bottom"),
          "both server list ends have their button");
    check(state->visible_calls == 1, "visible ran once, for the one message-independent predicate");

    // The hover bar asks per message and remembers.
    PluginMessageInput apple = message(200, "apple");
    check(shown(kUiPluginSlotMessageHover, 200, nodes, 8) == 1 && same(nodes[0].text, "Popover B"),
          "before the application asks, only the button with no predicate shows");
    PluginDecoratePass pass{};
    check(g_plugin_surfaces->popover(&pass, apple) && pass.calls == 1, "the predicates run once for a message");
    check(state->visible_calls == 3, "both message predicates ran");
    check(shown(kUiPluginSlotMessageHover, 200, nodes, 8) == 2 && same(nodes[0].text, "Popover A") &&
              same(nodes[1].text, "Popover B"),
          "an 'a' message gets A and B");
    PluginDecoratePass again{};
    g_plugin_surfaces->popover(&again, apple);
    check(state->visible_calls == 3 && again.calls == 0, "a current answer is not asked again");
    apple.revision = 2;
    g_plugin_surfaces->popover(&again, apple);
    check(state->visible_calls == 5, "a new revision asks again");
    PluginMessageInput cat = message(201, "cat");
    g_plugin_surfaces->popover(&again, cat);
    check(shown(kUiPluginSlotMessageHover, 201, nodes, 8) == 2 && same(nodes[0].text, "Popover B") &&
              same(nodes[1].text, "Popover C") && nodes[1].tone == 4,
          "a 'c' message gets B and the dangerous C");
    PluginButtonRef refs[4];
    check(g_plugin_surfaces->overflow(200, 1, {refs, 4}) == 1 && same(refs[0].label, "Popover B") && !refs[0].danger &&
              (refs[0].action_id & kButtonAction),
          "past a one-button bar the rest overflow");
    check(g_plugin_surfaces->overflow(201, 2, {refs, 4}) == 0, "nothing overflows a full bar");
    check(g_plugin_surfaces->overflow(201, 1, {refs, 4}) == 1 && same(refs[0].label, "Popover C") && refs[0].danger,
          "the danger flag follows the button");
    PluginDecoratePass tight{};
    tight.budget_us = 1;
    tight.spent_us = 1;
    PluginMessageInput fresh = message(202, "apple");
    check(!g_plugin_surfaces->popover(&tight, fresh) && tight.exhausted &&
              shown(kUiPluginSlotMessageHover, 202, nodes, 8) == 1,
          "a pass out of budget leaves the predicates for later");

    // A click is a gesture with the clicked message; the toggle round trip goes through button_state.
    PluginSurfaceTarget bar{kUiPluginSlotComposerToolbar, 100, 0, 0, 7};
    const u64 toggle = kButtonAction | SURFACES_BUTTON_CHAT_TOGGLE;
    check(g_plugin_surfaces->activate(SURFACES_OWNER, bar, toggle, nullptr), "the toggle click is handled");
    check(state->click_calls == 1 && state->last_button == SURFACES_BUTTON_CHAT_TOGGLE && state->last_click_channel == 100 &&
              state->last_click_guild == 7 && !state->last_click_message && (state->last_click_flags & MC_CTX_GESTURE) &&
              state->state_status == MC_OK && state->toggled == 1,
          "the click carries the open channel and the gesture, with no message");
    check(shown(kUiPluginSlotComposerToolbar, 0, nodes, 8) == 1 && (nodes[0].state & kUiPluginSlotActive) &&
              nodes[0].badge == 3 && same(nodes[0].tooltip, "Fixture on"),
          "the toggle shows active with its badge and tooltip");
    const u64 revision = g_plugin_surface_revision;
    g_plugin_surfaces->activate(SURFACES_OWNER, bar, toggle, nullptr);
    check(shown(kUiPluginSlotComposerToolbar, 0, nodes, 8) == 1 && !nodes[0].state && !nodes[0].badge &&
              !nodes[0].tooltip.bytes && g_plugin_surface_revision != revision,
          "a second click turns it off and the UI revision moves");

    PluginSurfaceTarget hover{kUiPluginSlotMessageHover, 100, 201, 9, 7};
    PluginMessageInput clicked = message(201, "cat");
    check(g_plugin_surfaces->activate(SURFACES_OWNER, hover, kButtonAction | SURFACES_BUTTON_POPOVER_C, &clicked),
          "a popover click is handled");
    check(state->last_button == SURFACES_BUTTON_POPOVER_C && state->last_click_message == 201 &&
              state->last_click_content_bytes == 3 && state->push_status == MC_OK,
          "the plugin received the clicked message");
    check(decoration(201) && decoration(201)->accessory_count == 1 &&
              same(decoration(201)->accessories[0].text, "Pushed by click"),
          "the click's push shows under the message");
    plugin_surfaces_push(binding, MC_HOOK_DECORATE_MESSAGE, 201, 100, nullptr);
    check(!g_plugin_surfaces->activate(SURFACES_OWNER, hover, kButtonAction | SURFACES_BUTTON_POPOVER_A, &clicked),
          "a button the message does not show cannot be clicked");
    check(!g_plugin_surfaces->activate(SURFACES_OWNER, hover, kButtonAction | SURFACES_BUTTON_POPOVER_C, nullptr),
          "a popover click needs its message");
    check(!g_plugin_surfaces->activate(SURFACES_OWNER, bar, kButtonAction | SURFACES_BUTTON_HEADER, nullptr),
          "a click from the wrong surface is refused");
    check(!g_plugin_surfaces->activate(SURFACES_OWNER, bar, kButtonAction | 40, nullptr), "an unknown button is refused");

    // State from the host side: disabled and hidden.
    mc_button_state disabled{MC_BUTTON_DISABLED, 0, {}, {}};
    const u32 clicks = state->click_calls;
    check(buttons_set_state(binding, SURFACES_BUTTON_ACCOUNT, disabled) == MC_OK, "a state is set");
    check(shown(kUiPluginSlotAccountPanel, 0, nodes, 8) == 1 && (nodes[0].state & kUiPluginSlotDisabled), "it shows dimmed");
    PluginSurfaceTarget account{kUiPluginSlotAccountPanel, 100, 0, 0, 7};
    check(!g_plugin_surfaces->activate(SURFACES_OWNER, account, kButtonAction | SURFACES_BUTTON_ACCOUNT, nullptr) &&
              state->click_calls == clicks,
          "a disabled button's click does nothing");
    mc_button_state hidden{MC_BUTTON_HIDDEN, 0, {}, {}};
    buttons_set_state(binding, SURFACES_BUTTON_ACCOUNT, hidden);
    check(shown(kUiPluginSlotAccountPanel, 0, nodes, 8) == 0, "a hidden button leaves the surface");
    mc_button_state override_icon{0, 120, {"tip", 3, 0}, {"lucide/bug", 10, 0}};
    buttons_set_state(binding, SURFACES_BUTTON_ACCOUNT, override_icon);
    check(shown(kUiPluginSlotAccountPanel, 0, nodes, 8) == 1 && nodes[0].icon == static_cast<u16>(UiIcon::bug) &&
              nodes[0].badge == 120 && same(nodes[0].tooltip, "tip"),
          "a state may replace the icon and tooltip");
    buttons_set_state(binding, SURFACES_BUTTON_ACCOUNT, {});
    check(shown(kUiPluginSlotAccountPanel, 0, nodes, 8) == 1 && nodes[0].icon == static_cast<u16>(UiIcon::bell) &&
              !nodes[0].badge,
          "an empty state restores the declared icon");
    const u64 settled = g_plugin_surface_revision;
    buttons_set_state(binding, SURFACES_BUTTON_ACCOUNT, {});
    check(g_plugin_surface_revision == settled, "an unchanged state does not move the revision");
    mc_button_state invalid{8, 0, {}, {}};
    check(buttons_set_state(binding, SURFACES_BUTTON_ACCOUNT, invalid) == MC_E_INVALID &&
              buttons_set_state(binding, 40, {}) == MC_E_INVALID,
          "an unknown flag or button is invalid");

    // Predicates are asked again on a refresh.
    const u32 visible = state->visible_calls;
    g_plugin_surfaces->refresh_buttons(100, 7);
    check(state->visible_calls == visible + 1, "a refresh asks the message-independent predicates again");
}

void test_merge(PluginRuntime* runtime, PluginBinding* first) noexcept {
    const PluginEnableResult second = enable(runtime, L"second.dll");
    check(second.ok() && second.binding, "a second decorating plugin is enabled");
    if (!second.ok()) return;
    prepare(message(300, "affix"));
    const auto* merged = decoration(300);
    check(merged && same(merged->name_prefix, "[P] [Q]") && same(merged->name_suffix, "(S) (T)") &&
              merged->name_tone == kUiPluginToneRole && merged->badge_count == 2 &&
              merged->badges[0].owner_id == SURFACES_OWNER && merged->badges[1].owner_id == kSecondOwner &&
              same(merged->badges[1].text, "C"),
          "affixes join in enable order and badges append");
    prepare(message(301, "collapse"));
    check(decoration(301) && decoration(301)->visibility == ui_plugin_render_collapsed &&
              same(decoration(301)->collapsed_label, "Spoiler"),
          "equal visibility keeps the first label");
    prepare(message(302, "hide"));
    check(decoration(302) && decoration(302)->visibility == ui_plugin_render_hidden, "one plugin's hide wins");
    check(state_of(second.binding)->message_calls == 3 && state_of(first)->message_calls > 0, "both plugins were asked");
    check(plugin_runtime_disable(runtime, second.binding), "the second plugin is disabled");
    check(g_plugin_surfaces != nullptr, "the first plugin keeps the table installed");
    check(decoration(300) && decoration(300)->badge_count == 1 && same(decoration(300)->name_prefix, "[P]"),
          "and the disabled plugin's verdicts are gone");
}

void test_disable(PluginRuntime* runtime, PluginBinding* binding) noexcept {
    check(plugin_runtime_disable(runtime, binding), "the plugin is disabled");
    check(!g_plugin_surfaces && !g_ui_plugin_render_query && !g_ui_plugin_slot_query && g_ui_plugin_slot_surfaces == ~0u,
          "nothing stays installed");
    check(!ui_plugin_render(1, 1), "no verdict survives");
    UiPluginSlotNode nodes[2];
    check(ui_plugin_slot_toolbar(kUiPluginSlotComposerToolbar, {nodes, 2}) == 0, "no button survives");
    // Enabling again starts clean.
    const PluginEnableResult again = enable(runtime, L"surfaces.dll");
    check(again.ok() && state_of(again.binding)->message_calls == 0 && !ui_plugin_render(1, 1),
          "a re-enabled plugin starts with an empty cache and fresh state");
    if (again.ok()) plugin_runtime_disable(runtime, again.binding);
    check(!g_plugin_surfaces, "and leaves nothing behind");
}

// A fake cache for mc_api.read_*.
bool fake_user(u64 user, u64 guild, PluginUserInput* out) noexcept {
    if (user != 5) return false;
    out->user_id = 5;
    out->guild_id = guild;
    out->flags = kPluginUserBot;
    out->relationship = kPluginRelationshipFriend;
    out->presence = kPluginPresenceIdle;
    out->platforms = kPluginPlatformMobile | kPluginPlatformWeb;
    out->name = guild ? Utf8View{"Nick", 4} : Utf8View{"Global", 6};
    out->username = {"user5", 5};
    return true;
}
bool fake_channel(u64 channel, PluginChannelInput* out) noexcept {
    if (channel != 6) return false;
    *out = {6, 7, 8, 0x400, 0, kPluginChannelNsfw | kPluginChannelCanSend, 2, {"general", 7}};
    return true;
}
bool fake_guild(u64 guild, PluginGuildInput* out) noexcept {
    if (guild != 7) return false;
    *out = {7, 9, 0x8, kPluginGuildMuted, 3, 120, 17, {"Home", 4}, 14, 2, 3, 21, 6, 40};
    return true;
}
bool fake_permissions(u64 guild, u64 channel, u64 user, u64* bits) noexcept {
    if (guild != 7) return false;
    *bits = channel ? 0x400 + user : 0x8;
    return true;
}
bool fake_voice(u64 user, u64 guild, u64* channel) noexcept {
    *channel = user == 5 && (!guild || guild == 7) ? 66 : 0;
    return *channel != 0;
}
// User 5 shares three group DMs, the most recent first.
u32 fake_group_dms(u64 user, Slice<u64> out) noexcept {
    if (user != 5) return 0;
    for (u32 i = 0; i < 3 && i < out.count; ++i) out[i] = 300 - i;
    return 3;
}
void test_reads() noexcept {
    const PluginReader reader{fake_user, fake_channel, fake_guild, fake_permissions, fake_voice, fake_group_dms};
    mc_user user{};
    mc_channel channel{};
    mc_guild guild{};
    u64 bits = 1;
    check(read_user(5, 0, &user) == MC_E_UNSUPPORTED && read_permissions(7, 0, 1, &bits) == MC_E_UNSUPPORTED && !bits,
          "reads need a reader");
    g_plugin_reader = &reader;
    check(read_user(5, 0, &user) == MC_OK && user.user_id == 5 && user.where == 0 && user.flags == MC_USER_BOT &&
              user.relationship == MC_RELATIONSHIP_FRIEND && user.presence == MC_PRESENCE_IDLE &&
              user.platforms == (MC_PLATFORM_MOBILE | MC_PLATFORM_WEB) && same(Utf8View{user.name.data, user.name.bytes}, "Global") &&
              same(Utf8View{user.username.data, user.username.bytes}, "user5"),
          "read_user copies the cached user");
    check(read_user(5, 7, &user) == MC_OK && user.guild_id == 7 && same(Utf8View{user.name.data, user.name.bytes}, "Nick"),
          "a guild picks the nickname");
    check(read_user(6, 0, &user) == MC_E_NOT_FOUND && read_user(0, 0, &user) == MC_E_INVALID &&
              read_user(5, 0, nullptr) == MC_E_INVALID,
          "unknown and malformed users");
    check(read_channel(6, &channel) == MC_OK && channel.guild_id == 7 && channel.parent_id == 8 &&
              channel.self_permissions == 0x400 && channel.flags == (MC_CHANNEL_NSFW | MC_CHANNEL_CAN_SEND) &&
              channel.mention_count == 2 && same(Utf8View{channel.name.data, channel.name.bytes}, "general"),
          "read_channel copies the cached channel");
    check(read_channel(9, &channel) == MC_E_NOT_FOUND, "an unknown channel is not found");
    check(read_guild(7, &guild) == MC_OK && guild.owner_id == 9 && guild.self_permissions == 8 &&
              guild.flags == MC_GUILD_MUTED && guild.member_count == 120 && guild.online_count == 17 &&
              same(Utf8View{guild.name.data, guild.name.bytes}, "Home") && guild.boost_count == 14 &&
              guild.boost_tier == 2 && guild.verification_level == 3 && guild.channel_count == 21 &&
              guild.role_count == 6 && guild.emoji_count == 40,
          "read_guild copies the cached guild and its overview counts");
    check(read_permissions(7, 0, 1, &bits) == MC_OK && bits == 8 && read_permissions(7, 6, 2, &bits) == MC_OK &&
              bits == 0x402 && read_permissions(8, 0, 1, &bits) == MC_E_NOT_FOUND &&
              read_permissions(7, 0, 0, &bits) == MC_E_INVALID,
          "read_permissions asks the cache for any user");
    u64 voice = 1;
    check(read_voice_channel(5, 0, &voice) == MC_OK && voice == 66 && read_voice_channel(5, 8, &voice) == MC_E_NOT_FOUND &&
              voice == 0 && read_voice_channel(0, 0, &voice) == MC_E_INVALID,
          "read_voice_channel finds the user's voice channel");
    u64 groups[2] = {};
    u32 total = 0;
    check(read_shared_group_dms(5, groups, 2, &total) == MC_OK && total == 3 && groups[0] == 300 && groups[1] == 299 &&
              read_shared_group_dms(5, nullptr, 0, &total) == MC_OK && total == 3 &&
              read_shared_group_dms(6, groups, 2, &total) == MC_E_NOT_FOUND && total == 0 &&
              read_shared_group_dms(5, nullptr, 2, &total) == MC_E_INVALID,
          "read_shared_group_dms writes what fits and counts them all");
    g_plugin_reader = nullptr;
}

// The shipped samples through the real runtime: names beside names, and a popover button with content.
void test_samples(PluginRuntime* runtime) noexcept {
    const PluginEnableResult names = plugin_runtime_enable(runtime, {L"names.dll", 9}, MC_PERM_KNOWN, 0);
    check(names.ok() && names.binding && g_plugin_surfaces && g_plugin_surfaces->wants == (1u << kKindUser),
          "show-me-your-name decorates users only");
    if (names.ok()) {
        PluginDecoratePass pass{};
        pass.budget_us = ~0ull;
        PluginUserInput author{};
        author.user_id = 41;
        author.message_id = 900;
        author.where = kPluginUserMessageAuthor;
        author.name = {"Alice", 5};
        author.username = {"alice9", 6};
        UiPluginSlotNode nodes[4];
        check(g_plugin_surfaces->user(&pass, author), "an author is prepared");
        const auto* decorated = ui_plugin_render(900, 1);
        check(decorated && same(decorated->name_suffix, "alice9") && !decorated->name_prefix.bytes,
              "the username follows the author's name");
        PluginUserInput same_name = author;
        same_name.user_id = 42;
        same_name.message_id = 901;
        same_name.name = {"Bob", 3};
        same_name.username = {"bob", 3};
        g_plugin_surfaces->user(&pass, same_name);
        check(!ui_plugin_render(901, 1), "a name that already matches is left alone");
        PluginUserInput member = author;
        member.where = kPluginUserMemberRow;
        member.message_id = 0;
        g_plugin_surfaces->user(&pass, member);
        check(slots(kUiPluginSlotMemberName, 0, 0, 41, 0, nodes, 4) == 2 && !nodes[0].text.bytes &&
                  same(nodes[1].text, "alice9"),
              "a member row gets the suffix as its second node");
        const u32 calls = names.binding->calls;
        PluginUserInput profile = author;
        profile.where = kPluginUserProfile;
        g_plugin_surfaces->user(&pass, profile);
        check(names.binding->calls == calls, "profiles are not asked");
        // The "before the name" setting moves it, tinted with the author's role colour.
        const u32 before = 1;
        check(plugin_binding_set_setting(names.binding, 0, &before, 4) == MC_OK, "the placement setting is accepted");
        g_plugin_surfaces->refresh();
        g_plugin_surfaces->user(&pass, author);
        decorated = ui_plugin_render(900, 1);
        check(decorated && same(decorated->name_prefix, "alice9") && !decorated->name_suffix.bytes &&
                  decorated->name_tone == kUiPluginToneRole,
              "placed before the name in the role colour");
        const u8 off = 0;
        check(plugin_binding_set_setting(names.binding, 1, &off, 1) == MC_OK, "the member row setting is accepted");
        g_plugin_surfaces->refresh();
        g_plugin_surfaces->user(&pass, member);
        check(slots(kUiPluginSlotMemberName, 0, 0, 41, 0, nodes, 4) == 0, "member rows can be left out");
        check(plugin_runtime_disable(runtime, names.binding), "show-me-your-name is disabled");
    }
    check(!ui_plugin_render(900, 1) && !g_plugin_surfaces, "nothing of it remains");

    const PluginEnableResult translate = plugin_runtime_enable(runtime, {L"translate.dll", 13}, MC_PERM_KNOWN, 7);
    check(translate.ok() && translate.binding, "translate is enabled");
    if (!translate.ok()) return;
    PluginDecoratePass pass{};
    pass.budget_us = ~0ull;
    UiPluginSlotNode nodes[4];
    const PluginMessageInput text = message(950, "Bonjour");
    const PluginMessageInput empty = message(951, "");
    g_plugin_surfaces->popover(&pass, text);
    g_plugin_surfaces->popover(&pass, empty);
    check(shown(kUiPluginSlotMessageHover, 950, nodes, 4) == 1 && same(nodes[0].text, "Translate") &&
              nodes[0].icon == static_cast<u16>(UiIcon::languages) && shown(kUiPluginSlotMessageHover, 951, nodes, 4) == 0,
          "the popover button appears on messages that have text");
    const PluginSurfaceTarget hover{kUiPluginSlotMessageHover, 100, 950, 9, 7};
    check(g_plugin_surfaces->activate(translate.binding->owner_id, hover,
                                      kButtonAction | 0, &text),
          "its click is a handled gesture");
    check(plugin_runtime_disable(runtime, translate.binding), "translate is disabled");
}

void run() noexcept {
    Root root;
    if (!make_root(&root, L"mcs")) {
        check(false, "temporary directory");
        return;
    }
    static PluginRuntime runtime;
    static PluginCallLedger ledger;
    PluginCallMarker leftover{};
    wchar_t ledger_path[MAX_PATH];
    join(ledger_path, root.base, L"ledger.bin");
    const bool ledger_ready = plugin_call_ledger_open(&ledger, ledger_path, &leftover) && !leftover.present;
    // Decorate calls get 20 ms here, so only the fixture's 30 ms "slow" message overruns, whatever the machine does.
    ledger.decorate_budget_us = 20'000;
    const PluginLoadResult opened =
        plugin_runtime_open(&runtime, {root.plugins, length(root.plugins)}, {root.data, length(root.data)}, &ledger);
    check(ledger_ready && opened.ok(), "the plugins directory is pinned");
    const bool installed = install(root, L"MaiCordPluginSurfacesFixture.dll", L"surfaces.dll") &&
                           install(root, L"MaiCordPluginSurfacesSecondFixture.dll", L"second.dll") &&
                           install(root, L"show-me-your-name.dll", L"names.dll") &&
                           install(root, L"translate.dll", L"translate.dll");
    check(installed, "the fixtures are installed");
    if (opened.ok() && installed) {
        test_no_plugins();
        PluginBinding* binding = nullptr;
        test_enable(&runtime, &binding);
        if (binding) {
            test_messages(binding);
            test_budget(binding);
            test_ring(binding);
            test_pull_invalidate_push(&runtime, binding);
            test_entities(binding);
            test_buttons(binding);
            test_merge(&runtime, binding);
            test_disable(&runtime, binding);
        }
        test_reads();
        test_samples(&runtime);
        test_plugin_surface_scenes(check);
        check(runtime.binding_count == 0, "every test left its plugin disabled");
        check(plugin_runtime_close(&runtime).ok(), "closing the runtime unpins the directory");
    }
    plugin_call_ledger_close(&ledger);
    remove_tree(root.base);
}

} // namespace maicord::plugin_surfaces_tests

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_surfaces_tests_entry() noexcept {
    maicord::plugin_surfaces_tests::run();
    ExitProcess(maicord::plugin_surfaces_tests::failures ? 1 : 0);
}
