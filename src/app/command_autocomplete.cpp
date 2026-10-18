#include "maicord/app/command_autocomplete.hpp"
#include "maicord/core/deadline.hpp"
namespace maicord {
void command_autocomplete_cancel(CommandAutocomplete* state) noexcept {
    command_suggestions_release(&state->suggestions);
    *state = {};
}
bool command_autocomplete_observe(CommandAutocomplete* state, CommandAutocompleteQuery query,
                                  u64 now) noexcept {
    if (state->query.channel_id == query.channel_id && state->query.command_id == query.command_id &&
        state->query.fingerprint == query.fingerprint)
        return false;
    command_autocomplete_cancel(state);
    if (query.channel_id && query.command_id) {
        state->query = query;
        state->phase = CommandAutocompletePhase::debounce;
        state->due_at = deadline_after(now, 150);
    }
    return true;
}
bool command_autocomplete_begin(CommandAutocomplete* state, u64 nonce, u64 now) noexcept {
    if (!nonce || state->phase != CommandAutocompletePhase::debounce || now < state->due_at)
        return false;
    state->nonce = nonce;
    state->due_at = 0;
    state->deadline = deadline_after(now, 5000);
    state->phase = CommandAutocompletePhase::waiting;
    return true;
}
bool command_autocomplete_transport(CommandAutocomplete* state, u64 nonce, bool success) noexcept {
    if (state->nonce != nonce || state->phase != CommandAutocompletePhase::waiting || success)
        return false;
    state->phase = CommandAutocompletePhase::failed;
    state->deadline = 0;
    return true;
}
bool command_autocomplete_accept(CommandAutocomplete* state, Utf8View body, Slice<u32> scratch) noexcept {
    if (state->phase != CommandAutocompletePhase::waiting ||
        !command_suggestions_decode(body, scratch, state->nonce, &state->suggestions))
        return false;
    state->phase = CommandAutocompletePhase::ready;
    state->deadline = 0;
    return true;
}
bool command_autocomplete_tick(CommandAutocomplete* state, u64 now) noexcept {
    if (state->phase != CommandAutocompletePhase::waiting || now < state->deadline)
        return false;
    state->phase = CommandAutocompletePhase::failed;
    state->deadline = 0;
    return true;
}
} // namespace maicord
