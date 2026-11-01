#pragma once
#include "builtin_commands.hpp"
#include "maicord/app/plugins/ui.hpp"

namespace maicord::app_detail {
// The enabled ABI-6 plugins' slash commands as a command index: one application per plugin, named
// after it, so the picker lists them in sections, option chips come from their typed arguments and
// autocomplete from their choices. Null while no plugin declares a command. The index is rebuilt by
// plugin_commands_refresh, never behind the composer's back: pointers into it stay valid until then.
const CommandIndex* plugin_commands_index() noexcept;
// The index object itself, whether or not it holds commands: what a selection compares against.
const CommandIndex* plugin_commands_storage() noexcept;
// True when plugins were enabled or disabled since the index was built: the composer drops what
// points into it, then calls plugin_commands_refresh.
bool plugin_commands_changed() noexcept;
void plugin_commands_refresh() noexcept;
// Runs a command of the plugin index with the options command_arguments wrote. insert: *text goes
// into the draft unsent. Text storage holds *text.
BuiltinCommandRun plugin_command_execute(const ApplicationCommand&, Utf8View options_json, u64 channel_id,
                                         u64 guild_id, Slice<char> text_storage, Utf8View* text) noexcept;
void plugin_commands_release() noexcept;
// A command of the built-in section or of a plugin: never sent to an application.
inline bool local_command_application(u64 application_id) noexcept {
    return application_id == kBuiltinCommandApplication || plugin_command_application(application_id);
}
} // namespace maicord::app_detail
