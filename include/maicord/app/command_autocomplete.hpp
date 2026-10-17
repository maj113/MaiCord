#pragma once
#include "maicord/protocol/application_commands.hpp"
namespace maicord {
struct CommandAutocompleteQuery {
    u64 channel_id, command_id, fingerprint;
};
enum class CommandAutocompletePhase : u8 {
    idle,
    debounce,
    waiting,
    ready,
    failed
};
struct CommandAutocomplete {
    CommandAutocompleteQuery query;
    CommandSuggestions suggestions;
    u64 nonce, due_at, deadline;
    CommandAutocompletePhase phase;
};
// Owns only the current query/result. A changed draft invalidates old nonces
// immediately, independently of the REST completion and Gateway arrival order.
bool command_autocomplete_observe(CommandAutocomplete*, CommandAutocompleteQuery, u64 now) noexcept;
bool command_autocomplete_begin(CommandAutocomplete*, u64 nonce, u64 now) noexcept;
bool command_autocomplete_transport(CommandAutocomplete*, u64 nonce, bool success) noexcept;
bool command_autocomplete_accept(CommandAutocomplete*, Utf8View body, Slice<u32> scratch) noexcept;
bool command_autocomplete_tick(CommandAutocomplete*, u64 now) noexcept;
void command_autocomplete_cancel(CommandAutocomplete*) noexcept;
} // namespace maicord
