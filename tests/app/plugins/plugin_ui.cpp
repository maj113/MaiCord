#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/plugins/dispatch.hpp"
#include "maicord/app/plugins/ui.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/protocol/messages/application_commands.hpp"
#include "../../support/test_output.hpp"
#include "plugin_fixture.hpp"
#include "fake_ui_host.hpp"

#include "maicord/core/win32_kernel.hpp"

// Menus, slash commands and feedback of ABI-6 plugins against the UI fixture
// (tests/fixtures/plugin_ui_fixture.c): which items a target shows and what they report, the command
// index the composer decodes and what running a command does with typed options, banners and modals
// through a recording host, and the navigation actions with their gesture and argument checks.
namespace maicord::plugin_ui_tests {
using namespace plugin_fixture;

u32 failures;
void check(bool value, const char* label) noexcept {
    if (value) return;
    ++failures;
    test_write_text("FAIL plugin_ui: ");
    test_write_line(label);
}

constexpr u64 kOwner = 0x4D43525455494658ull;

// The fixture's state block (tests/fixtures/plugin_ui_fixture.c).
struct FixtureState {
    u32 menu_calls, menu_kind, menu_item, menu_flags, menu_gesture;
    u64 user_id, channel_id, guild_id, message_id, item_id;
    u32 attachment_index, width, height, url_bytes, text_bytes, type_bytes;
    char url[96], text[48], type[32];
    u32 checked, visible_calls;
    u32 command_calls, command_gesture, arg_count;
    u64 arg_user, arg_channel, command_channel, command_guild;
    i64 arg_int;
    u32 arg_present[8];
    u32 arg_bool;
    i32 banner_status, banner_clear_status, banner_wrong_clear, modal_status, modal_busy_status, url_status, copy_status,
        channel_status, profile_status, bad_status;
    i32 probe_banner, probe_modal, probe_url, probe_copy, probe_open;
    u32 completions, completion_kind, completion_result, completion_toggle, completion_select, completion_gesture,
        completion_text_bytes;
    u64 completion_tag;
    char completion_text[64];
    i32 toast_status, toast_flags_status;
    i32 info_status, info_bad_cancel, info_bad_edit, info_bad_danger, read_only_bad_hint, read_only_bad_confirm;
};

Utf8View view(const char* text) noexcept {
    return {text, length(text)};
}
bool same_label(const PluginMenuRow& row, const char* expected) noexcept {
    return same(Utf8View{row.label, row.label_bytes}, expected);
}

// ---- The recording host ---------------------------------------------------------------------------

// What the application's side of the plugin UI was asked to show (fake_ui_host.cpp).
plugin_fake_ui::Recorded& host_state = plugin_fake_ui::recorded;

// ---- Helpers --------------------------------------------------------------------------------------

FixtureState* state_of(PluginBinding* binding) noexcept {
    return static_cast<FixtureState*>(binding->ctx.state);
}

PluginMenuTarget target(PluginMenuKind kind) noexcept {
    PluginMenuTarget t{};
    t.kind = kind;
    t.user_id = 11;
    t.channel_id = 22;
    t.guild_id = 33;
    t.message_id = 44;
    return t;
}

// The label list of a collected menu, joined with '|'.
bool labels_are(const PluginMenuRow* rows, u32 count, const char* expected) noexcept {
    char joined[200];
    u32 used = 0;
    for (u32 i = 0; i < count; ++i) {
        if (i) joined[used++] = '|';
        copy_bytes(joined + used, rows[i].label, rows[i].label_bytes);
        used += rows[i].label_bytes;
    }
    return same({joined, used}, expected);
}

u32 collect(const PluginMenuTarget& t, PluginMenuRow (&rows)[8]) noexcept {
    return plugin_menu_collect(t, {rows, 8});
}

// ---- Menus ----------------------------------------------------------------------------------------

void test_menus(PluginBinding* binding) noexcept {
    FixtureState& state = *state_of(binding);
    PluginMenuRow rows[8];

    PluginMenuTarget user = target(PluginMenuKind::user);
    u32 count = collect(user, rows);
    check(count == 2 && labels_are(rows, count, "Inspect|Others only") && rows[0].owner_id == kOwner &&
              rows[0].item == 0 && rows[1].item == 5 && !rows[0].flags,
          "a user menu shows the plugin's user items in descriptor order");
    user.flags = kPluginMenuTargetOwn;
    count = collect(user, rows);
    check(count == 1 && same_label(rows[0], "Inspect") && state.visible_calls > 0,
          "an item that hides itself for your own account is left out");

    PluginMenuTarget message = target(PluginMenuKind::message);
    count = collect(message, rows);
    check(count == 2 && labels_are(rows, count, "Inspect|More") && (rows[1].flags & kPluginMenuRowSubmenu) &&
              !(rows[0].flags & kPluginMenuRowSubmenu),
          "a message menu lists its items and a submenu row");
    PluginMenuRow children[8];
    count = plugin_menu_children(message, kOwner, 1, {children, 8});
    check(count == 1 && same_label(children[0], "Pin to top") && children[0].item == 3 &&
              !(children[0].flags & kPluginMenuRowChecked),
          "the submenu holds the items for this target; the checkbox starts unchecked");
    check(plugin_menu_activate(message, kOwner, 3) == PluginMenuRun::ran && state.checked == 1 && state.menu_item == 3 &&
              state.menu_kind == MC_MENU_MESSAGE && state.menu_gesture == 1 && state.message_id == 44 &&
              state.user_id == 11 && state.channel_id == 22 && state.guild_id == 33,
          "a checkbox item runs as a gesture with the message's identity");
    count = plugin_menu_children(message, kOwner, 1, {children, 8});
    check(count == 1 && (children[0].flags & kPluginMenuRowChecked), "the checkbox reports the state the plugin gives");

    PluginMenuTarget link = target(PluginMenuKind::link);
    link.url = view("https://example.com/a?b=c");
    link.text = view("a link");
    link.flags = kPluginMenuTargetPrivate;
    count = collect(link, rows);
    check(count == 1 && same_label(rows[0], "More") && (rows[0].flags & kPluginMenuRowSubmenu),
          "a link menu shows the submenu that has a link child");
    count = plugin_menu_children(link, kOwner, 1, {children, 8});
    check(count == 1 && same_label(children[0], "Copy link"), "its child is the link item");
    check(plugin_menu_activate(link, kOwner, 2) == PluginMenuRun::ran && state.menu_kind == MC_MENU_LINK &&
              state.menu_flags == MC_MENU_PRIVATE && state.url_bytes == length("https://example.com/a?b=c") &&
              same({state.url, state.url_bytes}, "https://example.com/a?b=c") &&
              same({state.text, state.text_bytes}, "a link"),
          "a link item receives the URL, its text and the private flag");

    count = collect(target(PluginMenuKind::channel), rows);
    check(count == 1 && same_label(rows[0], "Wipe") && (rows[0].flags & kPluginMenuRowDanger),
          "a channel menu shows the danger item");
    count = collect(target(PluginMenuKind::guild), rows);
    check(count == 2 && labels_are(rows, count, "Wipe|Broken"), "a server menu shows both its items");

    PluginMenuTarget image = target(PluginMenuKind::image);
    image.item_id = 987;
    image.attachment_index = 2;
    image.url = view("https://cdn.example/x.png");
    image.text = view("x.png");
    image.content_type = view("image/png");
    image.width = 640;
    image.height = 480;
    count = collect(image, rows);
    check(count == 2 && labels_are(rows, count, "Greyed|Media") && (rows[0].flags & kPluginMenuRowDisabled) &&
              !(rows[1].flags & kPluginMenuRowDisabled),
          "an image menu lists a disabled item and an enabled one");
    check(plugin_menu_activate(image, kOwner, 7) == PluginMenuRun::ran && state.menu_kind == MC_MENU_IMAGE &&
              state.item_id == 987 && state.attachment_index == 2 && state.width == 640 && state.height == 480 &&
              same({state.type, state.type_bytes}, "image/png") && same({state.text, state.text_bytes}, "x.png"),
          "an image item receives the attachment's id, index, size, type and name");

    PluginMenuTarget emoji = target(PluginMenuKind::emoji);
    emoji.item_id = 555;
    emoji.text = view("wave");
    emoji.flags = kPluginMenuTargetAnimated;
    count = collect(emoji, rows);
    check(count == 1 && same_label(rows[0], "Media"), "an emoji menu shows the media item");
    check(plugin_menu_activate(emoji, kOwner, 7) == PluginMenuRun::ran && state.menu_kind == MC_MENU_EMOJI &&
              state.item_id == 555 && state.menu_flags == MC_MENU_ANIMATED && same({state.text, state.text_bytes}, "wave"),
          "an emoji item receives its id, name and the animated flag");

    PluginMenuRow one[1];
    check(plugin_menu_collect(user, {one, 1}) == 1 && plugin_menu_collect(user, {rows, 0}) == 0,
          "collecting stops at the output's capacity");

    const u32 calls = state.menu_calls;
    check(plugin_menu_activate(user, kOwner, 4) == PluginMenuRun::gone &&
              plugin_menu_activate(message, kOwner, 1) == PluginMenuRun::gone &&
              plugin_menu_activate(message, kOwner, 99) == PluginMenuRun::gone &&
              plugin_menu_activate(message, kOwner + 1, 0) == PluginMenuRun::gone && state.menu_calls == calls,
          "an item for another target, a submenu, an unknown item or owner never runs");
    check(plugin_menu_activate(target(PluginMenuKind::guild), kOwner, 8) == PluginMenuRun::failed &&
              state.menu_calls == calls + 1,
          "a handler that fails reports failed");
    check(plugin_menu_children(message, kOwner, 0, {children, 8}) == 0 &&
              plugin_menu_children(message, kOwner + 1, 1, {children, 8}) == 0,
          "only a submenu has children");
    check(plugin_menu_present(PluginMenuKind::user) && plugin_menu_present(PluginMenuKind::emoji),
          "the registry knows the plugin has items");
}

// ---- Command index and dispatch ------------------------------------------------------------------

char g_json[65536];
u32 g_scratch[16384];
CommandIndex g_index;

const ApplicationCommand* find_command(const char* name) noexcept {
    for (u32 i = 0; i < g_index.command_count; ++i)
        if (same(g_index.commands[i].name, name)) return g_index.commands + i;
    return nullptr;
}

// Composer text -> typed options -> the plugin.
PluginCommandResult run_text(const char* name, const char* arguments, Utf8View* error = nullptr,
                             bool* parsed = nullptr) noexcept {
    const ApplicationCommand* command = find_command(name);
    PluginCommandResult none{PluginCommandOutcome::gone, 0, 0, {}};
    if (!command) return none;
    char options[4096];
    ProtocolWriter writer{{reinterpret_cast<u8*>(options), sizeof(options)}, 0};
    Utf8View message{};
    const bool ok = command_arguments(g_index, *command, view(arguments), &writer, &message);
    if (error) *error = message;
    if (parsed) *parsed = ok;
    if (!ok) return none;
    return plugin_command_run(command->application_id, command->id, 77, 88, {options, writer.used}, {g_scratch, 16384});
}

PluginCommandResult run_json(const char* name, const char* options) noexcept {
    const ApplicationCommand* command = find_command(name);
    if (!command) return {PluginCommandOutcome::gone, 0, 0, {}};
    return plugin_command_run(command->application_id, command->id, 77, 88, view(options), {g_scratch, 16384});
}

void test_commands(PluginBinding* binding) noexcept {
    FixtureState& state = *state_of(binding);
    const u32 bytes = plugin_command_index_json({g_json, sizeof(g_json)});
    check(bytes && plugin_command_count() == 16, "the plugin's commands make an index");
    check(plugin_command_index_json({g_json, 100}) == 0, "an output that is too small yields no index");
    check(command_index_decode({g_json, bytes}, {g_scratch, 16384}, &g_index) && g_index.application_count == 1 &&
              g_index.command_count == 16,
          "the composer decodes the index: one application, sixteen commands");
    if (g_index.command_count != 16) return;
    check(g_index.applications[0].id == (kOwner | kPluginCommandApplicationFlag) &&
              plugin_command_application(g_index.applications[0].id) && same(g_index.applications[0].name, "UI fixture"),
          "the application is the plugin, marked as local");
    bool unique = true, flagged = true;
    for (u32 i = 0; i < g_index.command_count; ++i) {
        flagged = flagged && plugin_command_application(g_index.commands[i].application_id);
        for (u32 j = 0; j < i; ++j) unique = unique && g_index.commands[i].id != g_index.commands[j].id;
    }
    check(unique && flagged, "command ids are unique and belong to the plugin's application");
    const ApplicationCommand* guild_only = find_command("guild-only");
    const ApplicationCommand* say = find_command("say");
    check(guild_only && !guild_only->dm_permission && say && say->dm_permission && same(say->description, "Sends text"),
          "a guild-only command is not offered in DMs");

    // Typed parsing through the composer's own validation.
    Utf8View error{};
    bool parsed = false;
    PluginCommandResult result = run_text("calc", " a:5 b:7", &error, &parsed);
    check(parsed && result.outcome == PluginCommandOutcome::insert && same(result.text, "5+7=12") &&
              result.owner_id == kOwner,
          "integer options reach the plugin as numbers and a reply can insert text");
    check(state.command_calls == 1 && state.command_gesture == 1 && state.arg_count == 2 && state.arg_present[0] == 1 &&
              state.arg_present[1] == 1 && state.command_channel == 77 && state.command_guild == 88,
          "a command runs as a gesture with its channel and server");
    result = run_text("calc", " a:-7", &error, &parsed);
    check(parsed && result.outcome == PluginCommandOutcome::insert && same(result.text, "-7+0=-7") &&
              state.arg_present[1] == 0,
          "an omitted optional option arrives absent");
    run_text("calc", " a:500", &error, &parsed);
    check(!parsed && error.bytes, "the composer rejects a value above the declared maximum");
    run_text("calc", " b:3", &error, &parsed);
    check(!parsed && error.bytes, "the composer rejects a missing required option");
    result = run_text("note", " text:hello world mode:loud", &error, &parsed);
    check(parsed && result.outcome == PluginCommandOutcome::local && same(result.text, "note:loud:hello world") &&
              result.tone == MC_TONE_WARNING,
          "a text option takes spaces and a choice; a local reply carries its tone");
    result = run_text("note", " text:plain", &error, &parsed);
    check(parsed && same(result.text, "note:-:plain"), "an omitted choice option is absent");
    run_text("note", " text:x mode:bogus", &error, &parsed);
    check(!parsed && error.bytes, "a value outside the declared choices is rejected");
    result = run_text("flag", " on:true who:<@123456789012345678> where:<#42>", &error, &parsed);
    check(parsed && result.outcome == PluginCommandOutcome::toast && same(result.text, "flag:1:123456789012345678:42") &&
              result.tone == MC_TONE_POSITIVE && state.arg_user == 123456789012345678ull && state.arg_channel == 42,
          "boolean, user and channel options reach the plugin and a reply can be a toast");

    // Replies and failures.
    result = run_json("ping", "[]");
    check(result.outcome == PluginCommandOutcome::none && result.text.bytes == 0, "a command can reply with nothing");
    check(run_json("ping", "").outcome == PluginCommandOutcome::none, "options may be absent altogether");
    result = run_json("say", "[{\"name\":\"text\",\"type\":3,\"value\":\"hi \\\"there\\\" \\u00e9\"}]");
    check(result.outcome == PluginCommandOutcome::send && same(result.text, "hi \"there\" \xC3\xA9"),
          "escapes in a text option are decoded and a reply can be sent");
    check(run_json("fail", "[]").outcome == PluginCommandOutcome::failed, "a failing handler reports failed");
    check(run_json("bad-reply", "[]").outcome == PluginCommandOutcome::failed,
          "an unknown reply kind is a failure");
    check(run_json("say", "[{\"name\":\"nope\",\"type\":3,\"value\":\"x\"}]").outcome == PluginCommandOutcome::failed,
          "an option the command does not declare fails");
    check(run_json("say", "[]").outcome == PluginCommandOutcome::failed, "a missing required option fails");
    check(run_json("say", "[{\"name\":\"text\",\"type\":3,\"value\":5}]").outcome == PluginCommandOutcome::failed,
          "a value of the wrong type fails");
    check(run_json("say", "{broken").outcome == PluginCommandOutcome::failed, "malformed options fail");
    check(run_json("calc", "[{\"name\":\"a\",\"type\":4,\"value\":\"x\"}]").outcome == PluginCommandOutcome::failed,
          "a number given as text fails");
    const u32 calls = state.command_calls;
    check(plugin_command_run(0x1234 | kPluginCommandApplicationFlag, 1, 1, 1, {}, {g_scratch, 16384}).outcome ==
                  PluginCommandOutcome::gone &&
              plugin_command_run(say->application_id, say->id + 1000, 1, 1, {}, {g_scratch, 16384}).outcome ==
                  PluginCommandOutcome::gone &&
              plugin_command_run(say->application_id, 0, 1, 1, {}, {g_scratch, 16384}).outcome ==
                  PluginCommandOutcome::gone &&
              state.command_calls == calls,
          "an unknown plugin or command never runs");
}

// ---- Banners ---------------------------------------------------------------------------------------

void test_banners(PluginBinding* binding) noexcept {
    FixtureState& state = *state_of(binding);
    PluginBannerView shown{};
    check(!plugin_banner_current(&shown), "there is no banner at first");
    const u32 changes = host_state.banner_changes;
    run_json("banner", "[]");
    check(state.banner_status == MC_OK && plugin_banner_current(&shown) && shown.owner_id == kOwner && shown.tag == 77 &&
              shown.warning && same(shown.title, "Plugin banner") && same(shown.text, "Something needs a look") &&
              same(shown.action, "Look") && host_state.banner_changes == changes + 1,
          "a banner shows its title, text and action and tells the application");
    const u32 serial = shown.serial;
    run_json("banner", "[]");
    check(plugin_banner_current(&shown) && shown.serial != serial, "a new banner replaces the old one");

    state.completions = 0;
    check(plugin_banner_resolve(true) && !plugin_banner_current(&shown) && state.completions == 1 &&
              state.completion_kind == MC_COMPLETION_BANNER && state.completion_tag == 77 &&
              state.completion_result == MC_BANNER_ACTION && state.completion_gesture == 1,
          "pressing the action reports it to the plugin as a gesture and removes the banner");
    run_json("banner", "[]");
    check(plugin_banner_resolve(false) && state.completions == 2 && state.completion_result == MC_BANNER_DISMISSED,
          "dismissing reports MC_BANNER_DISMISSED");
    check(!plugin_banner_resolve(true) && state.completions == 2, "with no banner there is nothing to resolve");

    run_json("banner", "[]");
    run_json("banner-clear", "[]");
    check(state.banner_wrong_clear == MC_E_NOT_FOUND && state.banner_clear_status == MC_OK &&
              !plugin_banner_current(&shown) && state.completions == 2,
          "clearing needs the current banner's tag and completes nothing");
}

// ---- Modals ----------------------------------------------------------------------------------------

void test_modals(PluginBinding* binding) noexcept {
    FixtureState& state = *state_of(binding);
    check(!plugin_modal_current() && !plugin_modal_resolve(true, {}, false, 0), "there is no modal at first");
    host_state.accept_modal = true;
    state.completions = 0;
    run_json("confirm", "[]");
    const PluginModalRequest* modal = plugin_modal_current();
    check(state.modal_status == MC_OK && state.modal_busy_status == MC_E_BUSY && modal && host_state.modal_opens == 1 &&
              host_state.modal.kind == PluginModalKind::toggle && host_state.modal.danger && !host_state.modal.multiline &&
              same(view(host_state.title), "Delete everything?") && same(view(host_state.context), "This cannot be undone.") &&
              same(view(host_state.accept), "Delete") && host_state.cancel[0] == 0 &&
              same(view(host_state.toggle), "Also the backups") && host_state.modal.toggle_initial &&
              host_state.modal.owner_id == kOwner,
          "a confirmation with a toggle reaches the application as asked; a second modal is refused as busy");
    check(plugin_modal_resolve(true, view("ignored"), true, 3) && !plugin_modal_current() && state.completions == 1 &&
              state.completion_kind == MC_COMPLETION_MODAL && state.completion_tag == 501 &&
              state.completion_result == 1 && state.completion_toggle == 1 && state.completion_gesture == 1 &&
              state.completion_text_bytes == 0 && state.completion_select == 0,
          "accepting reports the toggle as a gesture; fields the modal lacks stay empty");
    run_json("confirm", "[]");
    const u32 urls = host_state.urls, copies = host_state.copies, channels = host_state.channels,
              opens = host_state.modal_opens;
    check(plugin_modal_resolve(false, view("ignored"), true, 0) && state.completions == 2 && state.completion_result == 0 &&
              state.completion_toggle == 0 && state.completion_gesture == 0,
          "cancelling reports nothing but the refusal, and no gesture");
    // The cancelled modal's completion ran without a gesture: the plugin tried every action there.
    PluginBannerView quiet{};
    check(state.probe_banner == MC_OK && plugin_banner_current(&quiet) && same(quiet.title, "Quiet banner") &&
              !quiet.warning && quiet.action.bytes == 0,
          "a banner can be raised without a gesture");
    check(state.probe_modal == MC_E_GESTURE && state.probe_url == MC_E_GESTURE && state.probe_copy == MC_E_GESTURE &&
              state.probe_open == MC_E_GESTURE && host_state.urls == urls && host_state.copies == copies &&
              host_state.channels == channels && host_state.modal_opens == opens && !plugin_modal_current(),
          "a modal, links, the clipboard and navigation need a gesture");
    plugin_banner_resolve(false);

    run_json("form", "[]");
    check(state.modal_status == MC_OK && host_state.modal.kind == PluginModalKind::input_select &&
              same(view(host_state.input_label), "Name") && same(view(host_state.input_initial), "start") &&
              same(view(host_state.hint), "a name") && host_state.modal.input_max_chars == 20 &&
              same(view(host_state.select_label), "Kind") && host_state.modal.select_count == 3 &&
              host_state.modal.select_initial == 1 && same(host_state.modal.select_items[2], "three"),
          "a form with a dropdown reaches the application with its items");
    check(plugin_modal_resolve(true, view("hello"), true, 2) && state.completion_tag == 601 &&
              same({state.completion_text, state.completion_text_bytes}, "hello") && state.completion_select == 2 &&
              state.completion_toggle == 0,
          "accepting a form reports its text and choice");
    run_json("form", "[]");
    plugin_modal_resolve(true, view("again"), false, 9);
    check(state.completion_select == 0, "a dropdown index outside the items falls back to the first");

    run_json("info", "[]");
    check(state.bad_status == MC_E_INVALID, "a toggle together with an input is refused");
    check(state.info_bad_cancel == MC_E_INVALID && state.info_bad_danger == MC_E_INVALID &&
              state.info_bad_edit == MC_E_INVALID && state.read_only_bad_hint == MC_E_INVALID &&
              state.read_only_bad_confirm == MC_E_INVALID,
          "an information dialog refuses a Cancel label, a destructive button and an editable input; read-only "
          "text refuses a hint and needs a form");
    check(state.info_status == MC_OK && plugin_modal_current() && host_state.modal.kind == PluginModalKind::text &&
              host_state.modal.single && !host_state.modal.danger && same(view(host_state.title), "Permissions") &&
              same(view(host_state.context), "Someone in Somewhere") && same(view(host_state.input_label), "Granted") &&
              same(view(host_state.input_initial), "Text: Send Messages\nVoice: Connect") && host_state.accept[0] == 0 &&
              host_state.cancel[0] == 0 && host_state.hint[0] == 0,
          "an information dialog with read-only text reaches the application as one-button text, not an input");
    check(plugin_modal_resolve(true, view("typed"), true, 2) && state.completion_tag == 801 &&
              state.completion_result == 1 && state.completion_text_bytes == 0 && state.completion_toggle == 0 &&
              state.completion_select == 0,
          "closing an information dialog reports no text, toggle or choice");
    run_json("info", "[]");
    check(plugin_modal_resolve(false, {}, false, 0) && state.completion_result == 0 && !plugin_modal_current(),
          "dismissing an information dialog reports it was not accepted");

    host_state.accept_modal = false;
    run_json("confirm", "[]");
    check(state.modal_status == MC_E_BUSY && !plugin_modal_current(),
          "an application that cannot show the modal leaves none behind");
    host_state.accept_modal = true;
}

// ---- Actions ---------------------------------------------------------------------------------------

void test_actions(PluginBinding* binding) noexcept {
    FixtureState& state = *state_of(binding);
    host_state.status = PluginUiStatus::ok;
    run_json("link", "[{\"name\":\"text\",\"type\":3,\"value\":\"https://example.com/a?b=c\"}]");
    check(state.url_status == MC_OK && host_state.urls == 1 && same(view(host_state.url), "https://example.com/a?b=c"),
          "a link goes to the application's trust confirmation");
    host_state.status = PluginUiStatus::busy;
    run_json("link", "[{\"name\":\"text\",\"type\":3,\"value\":\"HTTP://example.com\"}]");
    check(state.url_status == MC_E_BUSY && host_state.urls == 2, "schemes are case-insensitive and statuses map back");
    host_state.status = PluginUiStatus::denied;
    run_json("link", "[{\"name\":\"text\",\"type\":3,\"value\":\"https://example.com\"}]");
    check(state.url_status == MC_E_DENIED, "a refused link reports denied");
    host_state.status = PluginUiStatus::ok;
    const u32 urls = host_state.urls;
    run_json("link", "[{\"name\":\"text\",\"type\":3,\"value\":\"javascript:alert(1)\"}]");
    check(state.url_status == MC_E_INVALID, "only http and https links are opened");
    run_json("link", "[{\"name\":\"text\",\"type\":3,\"value\":\"https://exa mple.com\"}]");
    check(state.url_status == MC_E_INVALID && host_state.urls == urls, "a link with a space is invalid");

    run_json("copy", "[{\"name\":\"text\",\"type\":3,\"value\":\"some text\"}]");
    check(state.copy_status == MC_OK && host_state.copies == 1 && same(view(host_state.copy), "some text"),
          "copying reaches the clipboard through the application");
    check(state.toast_status == MC_OK && host_state.toasts == 1 && same(view(host_state.toast), "some text") &&
              host_state.toast_tone == MC_TONE_ACCENT && state.toast_flags_status == MC_E_INVALID,
          "a toast reaches the application's host; flags must stay zero");

    run_json("goto", "[{\"name\":\"where\",\"type\":7,\"value\":\"4242\"},{\"name\":\"who\",\"type\":6,\"value\":\"9\"}]");
    check(state.channel_status == MC_OK && host_state.channel == 4242 && state.profile_status == MC_OK &&
              host_state.user == 9 && host_state.guild == 55 && state.bad_status == MC_E_INVALID,
          "channels and profiles open through the application; an unknown target is invalid");
}

// ---- Lifecycle -------------------------------------------------------------------------------------

void test_disable(PluginRuntime* runtime, PluginBinding* binding, u32 revision) noexcept {
    FixtureState& state = *state_of(binding);
    (void)state;
    host_state.accept_modal = true;
    run_json("banner", "[]");
    run_json("confirm", "[]");
    const u32 closes = host_state.modal_closes, changes = host_state.banner_changes;
    PluginBannerView shown{};
    check(plugin_banner_current(&shown) && plugin_modal_current(), "a banner and a modal are up");
    check(plugin_runtime_disable(runtime, binding) && !plugin_banner_current(&shown) && !plugin_modal_current() &&
              host_state.modal_closes == closes + 1 && host_state.banner_changes == changes + 1,
          "disabling the plugin takes its banner and modal down and tells the application");
    PluginMenuRow rows[8];
    check(collect(target(PluginMenuKind::user), rows) == 0 && !plugin_menu_present(PluginMenuKind::user) &&
              plugin_menu_activate(target(PluginMenuKind::user), kOwner, 0) == PluginMenuRun::gone &&
              plugin_command_count() == 0 && plugin_command_index_json({g_json, sizeof(g_json)}) == 0 &&
              run_json("ping", "[]").outcome == PluginCommandOutcome::gone && plugin_ui_revision() != revision,
          "a disabled plugin has no items or commands and the revision moved");
    check(!plugin_modal_resolve(true, {}, false, 0), "its modal cannot be answered");
}

// ---- The samples, through the real runtime ----------------------------------------------------------

bool reload_index() noexcept {
    command_index_release(&g_index);
    const u32 bytes = plugin_command_index_json({g_json, sizeof(g_json)});
    return bytes && command_index_decode({g_json, bytes}, {g_scratch, 16384}, &g_index);
}

PluginBinding* enable_sample(PluginRuntime* runtime, const Root& root, const wchar_t* built, const wchar_t* name) noexcept {
    if (!install(root, built, name)) {
        check(false, "a sample plugin is built");
        return nullptr;
    }
    const PluginEnableResult enabled = plugin_runtime_enable(runtime, {name, length(name)}, MC_PERM_KNOWN);
    check(enabled.ok(), "a sample plugin is enabled");
    return enabled.binding;
}

// Reverse Image Search: a submenu on images with an address, one child per engine, and a setting that hides one.
void test_reverse_image_search(PluginRuntime* runtime, const Root& root) noexcept {
    PluginBinding* binding = enable_sample(runtime, root, L"reverse-image-search.dll", L"search.dll");
    if (!binding) return;
    PluginMenuTarget image = target(PluginMenuKind::image);
    image.url = view("https://cdn.example/a b.png");
    PluginMenuRow rows[8], children[8];
    u32 count = collect(image, rows);
    check(count == 1 && same_label(rows[0], "Search image") && (rows[0].flags & kPluginMenuRowSubmenu),
          "an image with an address shows the search submenu");
    if (count != 1) return;
    count = plugin_menu_children(image, rows[0].owner_id, rows[0].item, {children, 8});
    check(count == 4 && labels_are(children, count, "Google Lens|Bing|Yandex|TinEye"),
          "the submenu has an item per engine");
    host_state.status = PluginUiStatus::ok;
    const u32 urls = host_state.urls;
    check(plugin_menu_activate(image, rows[0].owner_id, children[0].item) == PluginMenuRun::ran &&
              host_state.urls == urls + 1 &&
              same(view(host_state.url), "https://lens.google.com/uploadbyurl?url=https%3A%2F%2Fcdn.example%2Fa%20b.png"),
          "an engine opens its search with the image's address percent-encoded, through the link confirmation");
    const u8 off = 0;
    check(plugin_binding_set_setting(binding, 0, &off, 1) == MC_OK &&
              plugin_menu_children(image, rows[0].owner_id, rows[0].item, {children, 8}) == 3,
          "turning an engine off in the plugin's settings removes its item");
    image.url = {};
    check(collect(image, rows) == 0, "an image without an address shows no submenu");
    check(collect(target(PluginMenuKind::user), rows) == 0 && collect(target(PluginMenuKind::message), rows) == 0,
          "other targets show nothing");
    check(plugin_runtime_disable(runtime, binding), "the sample is disabled");
}

// Silent Toggle: a /silent command with an optional boolean that answers with a toast.
void test_silent_toggle(PluginRuntime* runtime, const Root& root) noexcept {
    PluginBinding* binding = enable_sample(runtime, root, L"silent-toggle.dll", L"silent.dll");
    if (!binding) return;
    const ApplicationCommand* silent = reload_index() ? find_command("silent") : nullptr;
    check(silent && same(silent->description, "Send messages without notifications") && silent->dm_permission,
          "the command is in the index with its description");
    if (!silent) return;
    const u8* on = static_cast<const u8*>(binding->ctx.settings);
    check(!on[0], "messages start out notifying");
    PluginCommandResult result = run_json("silent", "[]");
    check(result.outcome == PluginCommandOutcome::toast && same(result.text, "Messages are sent silently") && on[0] == 1,
          "/silent with no argument switches silent sending on and says so");
    result = run_json("silent", "[{\"name\":\"on\",\"type\":5,\"value\":false}]");
    check(result.outcome == PluginCommandOutcome::toast && same(result.text, "Messages notify again") && on[0] == 0,
          "/silent on:false switches it off");
    Utf8View error{};
    bool parsed = false;
    result = run_text("silent", " on:true", &error, &parsed);
    check(parsed && result.outcome == PluginCommandOutcome::toast && same(result.text, "Messages are sent silently") &&
              on[0] == 1,
          "the composer's own option parsing reaches the optional boolean");
    result = run_json("silent", "[]");
    check(result.outcome == PluginCommandOutcome::toast && same(result.text, "Messages notify again") && on[0] == 0,
          "without an argument it switches back");
    check(plugin_runtime_disable(runtime, binding), "the sample is disabled");
}

void run() noexcept {
    Root root;
    if (!make_root(&root, L"mcu")) {
        check(false, "temporary directory");
        return;
    }
    static PluginRuntime runtime;
    host_state.accept_modal = true;
    const u32 before = plugin_ui_revision();
    PluginBannerView no_banner{};
    check(!plugin_menu_present(PluginMenuKind::message) && plugin_command_count() == 0 &&
              plugin_command_index_json({g_json, sizeof(g_json)}) == 0 && !plugin_banner_current(&no_banner),
          "with no plugin there are no items, commands or banners");
    PluginMenuRow rows[8];
    check(collect(target(PluginMenuKind::message), rows) == 0, "with no plugin a menu collects nothing");
    const PluginLoadResult opened =
        plugin_runtime_open(&runtime, {root.plugins, length(root.plugins)}, {root.data, length(root.data)}, nullptr);
    check(opened.ok() && install(root, L"MaiCordPluginUiFixture.dll", L"ui.dll"), "the fixture is installed");
    if (opened.ok()) {
        const wchar_t leaf[] = L"ui.dll";
        const PluginEnableResult enabled = plugin_runtime_enable(&runtime, {leaf, length(leaf)}, MC_PERM_KNOWN);
        check(enabled.ok() && enabled.binding && plugin_ui_revision() != before,
              "the plugin is enabled and the registry changes");
        if (enabled.ok()) {
            const u32 revision = plugin_ui_revision();
            test_menus(enabled.binding);
            test_commands(enabled.binding);
            test_banners(enabled.binding);
            test_modals(enabled.binding);
            test_actions(enabled.binding);
            test_disable(&runtime, enabled.binding, revision);
            test_reverse_image_search(&runtime, root);
            test_silent_toggle(&runtime, root);
        }
        check(runtime.binding_count == 0, "the test left no plugin enabled");
        plugin_runtime_close(&runtime);
    }
    command_index_release(&g_index);
    remove_tree(root.base);
}

} // namespace maicord::plugin_ui_tests

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_ui_tests_entry() noexcept {
    using namespace maicord;
    plugin_ui_tests::run();
    ExitProcess(plugin_ui_tests::failures ? 1 : 0);
}
