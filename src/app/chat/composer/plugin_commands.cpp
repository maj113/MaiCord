#include "plugin_commands.hpp"
#include "../../account/account_ui_runtime.hpp"
#include "../../shell/state.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::app_detail {
namespace {

// 64 KiB of index JSON holds about a hundred commands with their options.
constexpr u32 kIndexBytes = 65536;
struct State {
    CommandIndex index;
    u32 revision;
    bool built;
    char json[kIndexBytes];
};
State g_plugins;

} // namespace

bool plugin_commands_changed() noexcept {
    return g_plugins.built && g_plugins.revision != plugin_ui_revision();
}

void plugin_commands_refresh() noexcept {
    auto& s = g_plugins;
    if (!g_app.account_ui) return;
    command_index_release(&s.index);
    s.built = true;
    s.revision = plugin_ui_revision();
    const u32 bytes = plugin_command_index_json({s.json, sizeof(s.json)});
    if (bytes &&
        !command_index_decode({s.json, bytes},
                              {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount},
                              &s.index))
        log_write(LogLevel::warning, "commands", "plugin_index_decode_failed");
}

const CommandIndex* plugin_commands_index() noexcept {
    if (!g_plugins.built) plugin_commands_refresh();
    return g_plugins.index.command_count ? &g_plugins.index : nullptr;
}

const CommandIndex* plugin_commands_storage() noexcept {
    return &g_plugins.index;
}

BuiltinCommandRun plugin_command_execute(const ApplicationCommand& command, Utf8View options_json, u64 channel_id,
                                         u64 guild_id, Slice<char> text_storage, Utf8View* text) noexcept {
    if (!g_app.account_ui || !text) return BuiltinCommandRun::failed;
    const PluginCommandResult result = plugin_command_run(
        command.application_id, command.id, channel_id, guild_id, options_json,
        {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount});
    const auto keep = [&]() noexcept {
        if (result.text.bytes > text_storage.count) return false;
        copy_bytes(text_storage.data, result.text.data, result.text.bytes);
        *text = {text_storage.data, result.text.bytes};
        return true;
    };
    const PluginUiHost* host = plugin_ui_host();
    switch (result.outcome) {
    case PluginCommandOutcome::none: return BuiltinCommandRun::handled;
    case PluginCommandOutcome::send: return keep() ? BuiltinCommandRun::text : BuiltinCommandRun::failed;
    case PluginCommandOutcome::insert: return keep() ? BuiltinCommandRun::insert : BuiltinCommandRun::failed;
    case PluginCommandOutcome::local:
        return host && host->local_notice && host->local_notice(channel_id, result.text, nullptr)
                   ? BuiltinCommandRun::handled
                   : BuiltinCommandRun::failed;
    case PluginCommandOutcome::toast:
        if (!host || !host->toast) return BuiltinCommandRun::failed;
        host->toast(result.owner_id, result.text, result.tone);
        return BuiltinCommandRun::handled;
    default: return BuiltinCommandRun::failed;
    }
}

void plugin_commands_release() noexcept {
    command_index_release(&g_plugins.index);
    g_plugins.built = false;
}

} // namespace maicord::app_detail
