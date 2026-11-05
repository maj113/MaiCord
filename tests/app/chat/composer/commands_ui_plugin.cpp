#include "commands_ui_internal.hpp"
#include "../../../../src/app/chat/composer/commands.hpp"
#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/plugins/ui.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "../../../../src/app/account/account_ui_runtime.hpp"
#if !defined(_WIN32)
#include "../../plugins/plugin_fixture.hpp"
#endif

// Plugin slash commands in the composer: the picker lists the plugin as an app section, option chips and
// their validation come from its typed arguments, Enter runs the command and its reply lands where it
// says (the draft, a local message, a toast), a plugin that goes away takes its commands with it.
// The plugin is the UI fixture (tests/fixtures/plugin_ui_fixture.c) behind a recording host.
namespace maicord {
namespace commands_ui_detail {
namespace {

// The start of the fixture's state block (tests/fixtures/plugin_ui_fixture.c) up to its command counter.
struct FixtureState {
    u32 menu_calls, menu_kind, menu_item, menu_flags, menu_gesture;
    u64 user_id, channel_id, guild_id, message_id, item_id;
    u32 attachment_index, width, height, url_bytes, text_bytes, type_bytes;
    char url[96], text[48], type[32];
    u32 checked, visible_calls;
    u32 command_calls;
};

u32 length(const wchar_t* text) noexcept {
    u32 count = 0;
    while (text[count]) ++count;
    return count;
}
#if defined(_WIN32)
void join(wchar_t (&output)[MAX_PATH], const wchar_t* directory, const wchar_t* name) noexcept {
    u32 at = 0;
    for (; directory[at]; ++at) output[at] = directory[at];
    output[at++] = L'\\';
    for (u32 i = 0; name[i]; ++i) output[at++] = name[i];
    output[at] = 0;
}
void remove_tree(const wchar_t* directory) noexcept {
    wchar_t pattern[MAX_PATH];
    join(pattern, directory, L"*");
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(pattern, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (found.cFileName[0] == L'.' && (!found.cFileName[1] || (found.cFileName[1] == L'.' && !found.cFileName[2])))
                continue;
            wchar_t child[MAX_PATH];
            join(child, directory, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileW(child);
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    RemoveDirectoryW(directory);
}
#else
using plugin_fixture::join;
using plugin_fixture::remove_tree;
#endif

// What the application side of the plugin UI was asked to show.
struct Shown {
    u32 notices, toasts;
    u64 notice_channel;
    u32 tone;
    char notice[128], toast[128];
};
Shown shown;
template <u32 N> void keep(char (&storage)[N], Utf8View text) noexcept {
    const u32 bytes = text.bytes < N - 1 ? text.bytes : N - 1;
    copy_bytes(storage, text.data, bytes);
    storage[bytes] = 0;
}
bool host_notice(u64 channel, Utf8View text, u64*) noexcept {
    ++shown.notices;
    shown.notice_channel = channel;
    keep(shown.notice, text);
    return true;
}
void host_toast(u64, Utf8View text, u32 tone) noexcept {
    ++shown.toasts;
    shown.tone = tone;
    keep(shown.toast, text);
}
const PluginUiHost kHost{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, host_notice, host_toast};

bool equals(const char* text, const char* expected) noexcept {
    u32 at = 0;
    while (text[at] && expected[at] && text[at] == expected[at]) ++at;
    return !text[at] && !expected[at];
}

} // namespace

void plugin_cases() noexcept {
    auto& ui = *g_app.account_ui;
    auto& composer = g_app.message_composer;
    const auto saved_state = ui.chat_state;
    const u64 saved_active_channel = ui.active_channel_id;
    const u64 saved_composer_channel = composer.channel_id;
    GuildRecord guilds[1]{};
    ChannelRecord channels[1]{};
    u16 guild_order[1]{}, channel_order[1]{};
    u8 names[128]{};
    ChannelInput channel{};
    channel.id = 9;
    channel.guild_id = 55;
    channel.name = utf8_literal("general");
    const bool initialized =
        chat_state_store_initialize(&ui.chat_state, {guilds, 1}, {guild_order, 1}, {channels, 1},
                                    {channel_order, 1}, {names, sizeof(names)}) &&
        chat_state_store_upsert_guild(&ui.chat_state, {55, utf8_literal("Guild"), GuildMutationKind::create, 1})
            .ok() &&
        chat_state_store_upsert_channel(&ui.chat_state, channel).ok() &&
        message_composer_controller_select_channel(&ui.message_composer_controller, 9).ok();
    check(initialized, "plugin command fixture initializes");

    // The fixture DLL in a plugins directory of its own.
    wchar_t temp[MAX_PATH], base[MAX_PATH], plugins[MAX_PATH], data[MAX_PATH], module[MAX_PATH], source[MAX_PATH],
        destination[MAX_PATH];
    static PluginRuntime runtime;
    bool opened = false;
    PluginBinding* binding = nullptr;
    if (initialized) {
#if defined(_WIN32)
        const DWORD count = GetTempPathW(MAX_PATH, temp);
        const DWORD built = GetModuleFileNameW(nullptr, module, MAX_PATH);
        bool ready = count && count < MAX_PATH && built && built < MAX_PATH &&
                     GetTempFileNameW(temp, L"mcc", 0, base) && DeleteFileW(base) && CreateDirectoryW(base, nullptr);
        if (ready) {
            join(plugins, base, L"plugins");
            join(data, base, L"data");
            u32 at = built;
            while (at && module[at - 1] != L'\\') --at;
            module[at ? at - 1 : 0] = 0;
            join(source, module, L"MaiCordPluginUiFixture.dll");
            join(destination, plugins, L"ui.dll");
            ready = CreateDirectoryW(plugins, nullptr) && CreateDirectoryW(data, nullptr) &&
                    CopyFileW(source, destination, FALSE);
        }
#else
        (void)temp, (void)module, (void)source, (void)destination;
        plugin_fixture::Root root{};
        bool ready = plugin_fixture::make_root(&root, L"mcc") &&
                     plugin_fixture::install(root, L"MaiCordPluginUiFixture.dll", L"ui.dll");
        copy_bytes(base, root.base, (length(root.base) + 1) * sizeof(wchar_t));
        copy_bytes(plugins, root.plugins, (length(root.plugins) + 1) * sizeof(wchar_t));
        copy_bytes(data, root.data, (length(root.data) + 1) * sizeof(wchar_t));
#endif
        opened = ready && plugin_runtime_open(&runtime, {plugins, length(plugins)}, {data, length(data)}, nullptr).ok();
        check(opened, "the plugin fixture is installed");
    }
    if (opened) {
        plugin_ui_host_install(&kHost);
        const wchar_t leaf[] = L"ui.dll";
        const auto enabled = plugin_runtime_enable(&runtime, {leaf, length(leaf)}, MC_PERM_KNOWN);
        binding = enabled.binding;
        check(enabled.ok(), "the plugin fixture is enabled");
    }
    if (binding) {
        ui.active_channel_id = 9;
        ui.chat_state.self_user_id = 1;
        channels[0].permissions_resolved = true;
        channels[0].effective_permissions = kPermissionViewChannel | kPermissionSendMessages |
                                            kPermissionReadMessageHistory;
        // The composer notices the new plugin on its next tick and builds the index.
        application_commands_tick(clock_monotonic_milliseconds());
        const auto listed = [](Utf8View query, Utf8View name) noexcept {
            edit(query);
            return action_named(name) != 0;
        };
        const auto pick = [&](Utf8View name, Utf8View typed) noexcept {
            edit(name);
            const u32 action = action_named(name);
            if (!action || !application_commands_activate(action)) return false;
            edit(typed);
            return true;
        };
        edit(utf8_literal("/"));
        check(action_named(utf8_literal("UI fixture")) && listed(utf8_literal("/calc"), utf8_literal("/calc")) &&
                  listed(utf8_literal("/note"), utf8_literal("/note")) &&
                  listed(utf8_literal("/guild-only"), utf8_literal("/guild-only")),
              "the picker lists the plugin's commands in a section named after it");

        // An insert reply puts the text in the draft, unsent.
        check(pick(utf8_literal("/calc"), utf8_literal("/calc a:5 b:7")) && application_commands_key(key::enter) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("5+7=12")),
              "a command's insert reply replaces the draft");

        // A send reply goes out as the message itself.
        check(pick(utf8_literal("/say"), utf8_literal("/say text:hello there")) && application_commands_key(key::enter) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("hello there")),
              "a command's send reply becomes the message to send");
        bool handled = true;
        check(application_commands_send(&handled) && !handled, "and it sends as typed, not as a command");

        // A local reply, with the plugin's tone, shows only here and clears the draft. The first Enter
        // takes the highlighted choice of the mode; the second sends.
        shown = {};
        bool noted = pick(utf8_literal("/note"), utf8_literal("/note text:hey mode:loud")) &&
                     application_commands_key(key::enter);
        if (!shown.notices) noted = noted && application_commands_key(key::enter);
        check(noted && shown.notices == 1 && shown.notice_channel == 9 && equals(shown.notice, "note:loud:hey") &&
                  message_composer_text(composer).bytes == 0,
              "a local reply is shown only to the user and clears the draft");

        // Typed options are validated by the composer before the plugin runs.
        const auto* state = static_cast<const FixtureState*>(binding->ctx.state);
        const u32 before = state->command_calls;
        const bool typed = pick(utf8_literal("/calc"), utf8_literal("/calc a:500"));
        application_commands_key(key::enter);
        check(typed && state->command_calls == before && message_composer_text(composer).bytes != 0,
              "a value above the declared maximum is refused before the plugin runs");

        // A toast reply. The first Enter takes the boolean's highlighted choice; the second sends.
        shown = {};
        bool flagged = pick(utf8_literal("/flag"), utf8_literal("/flag on:true")) && application_commands_key(key::enter);
        if (!shown.toasts) flagged = flagged && application_commands_key(key::enter);
        check(flagged && shown.toasts == 1 && equals(shown.toast, "flag:1:0:0") && shown.tone == MC_TONE_POSITIVE,
              "a toast reply reaches the application with its tone");

        // A failing command keeps the draft and says so.
        check(pick(utf8_literal("/fail"), utf8_literal("/fail")) && application_commands_key(key::enter) &&
                  message_composer_text(composer).bytes != 0,
              "a command that fails keeps the draft");

        // A plugin that is disabled takes its commands out of the picker.
        edit({});
        check(plugin_runtime_disable(&runtime, binding), "the plugin is disabled");
        binding = nullptr;
        application_commands_tick(clock_monotonic_milliseconds());
        edit(utf8_literal("/"));
        check(!action_named(utf8_literal("UI fixture")) && !listed(utf8_literal("/calc"), utf8_literal("/calc")),
              "a disabled plugin's commands leave the picker");
    }
    if (opened) {
        if (binding) plugin_runtime_disable(&runtime, binding);
        plugin_runtime_close(&runtime);
        plugin_ui_host_install(nullptr);
        remove_tree(base);
    }
    message_composer_clear(&composer);
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
    ui.chat_state = saved_state;
    ui.active_channel_id = saved_active_channel;
    message_composer_controller_select_channel(&ui.message_composer_controller, saved_composer_channel);
    edit(utf8_literal("/"));
    message_composer_clear(&composer);
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
}

} // namespace commands_ui_detail
} // namespace maicord
