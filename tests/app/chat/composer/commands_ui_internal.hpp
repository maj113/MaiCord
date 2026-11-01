#pragma once

#include "../../../../src/app/shell/state.hpp"
#include "maicord/ui/media/picker.hpp"

namespace maicord::app_detail {

extern UiRect command_viewport;

void set_popup_owner(PopupOwnerKind owner) noexcept;

extern u32 context_changes;

extern UiMediaPickerKind searched_kind;

extern char searched[64];

extern u32 searched_bytes, searches;

// The fake inline results (see commands_ui.cpp): what Enter finds on them.
enum class FakeResults : u8 { none, loading, ready };

constexpr auto fake_empty_message = utf8_literal("No results found. Try a different search.");

extern FakeResults fake_results;

// fake_selected: the highlighted result; fake_sent_index: the one Enter sent.
extern u32 fake_selected, fake_sent_index, fake_sends, fake_waits;

// The waited-for results arrive: count of them; none is the command's refusal.
bool fake_results_arrive(u32 count) noexcept;

extern u64 left_group;

extern bool left_silent;

extern u64 opened_dm_user;

extern bool thread_sendable;

} // namespace maicord::app_detail

namespace maicord {
namespace commands_ui_detail {

using namespace app_detail;

extern u32 failures, calls, operation;

extern char payload[65536];

extern u32 payload_bytes;

extern u64 submitted_channel, submitted_guild, submitted_application, submitted_nonce, submitted_request;

void check(bool ok, const char* label) noexcept;

u32 action_named(Utf8View label, u32 ordinal = 0) noexcept;

bool payload_contains(Utf8View expected) noexcept;

constexpr auto fixture_index = utf8_literal(R"({"applications":[{"id":"10","name":"Test","bot":{"id":"11"}},{"id":"20","name":"Other","icon":"0f1e2d","bot":{"id":"21"}}],"application_commands":[{"id":"100","application_id":"10","version":"7","name":"wordle","description":"Play wordle","type":1},{"id":"101","application_id":"10","version":"7","name":"echo","description":"Echo","type":1,"options":[{"name":"text","type":3,"required":true}]},{"id":"102","application_id":"10","version":"7","name":"pick","description":"Pick","type":1,"options":[{"name":"mode","type":4,"required":true,"choices":[{"name":"One","value":1},{"name":"Two","value":2}]}]},{"id":"103","application_id":"10","version":"7","name":"admin","description":"Admin","type":1,"options":[{"name":"group","type":2,"options":[{"name":"run","description":"Run","type":1,"options":[{"name":"count","type":4,"required":true}]}]}]},{"id":"104","application_id":"10","version":"7","name":"bump","description":"First bump","type":1},{"id":"105","application_id":"20","version":"7","name":"bump","description":"Second bump","type":1},{"id":"106","application_id":"10","version":"7","name":"search","description":"Search","type":1,"options":[{"name":"query","type":3,"required":true,"autocomplete":true},{"name":"count","type":4}]},{"id":"107","application_id":"10","version":"7","name":"upload","description":"Upload","type":1,"options":[{"name":"file","type":11,"required":true},{"name":"extra","type":11}]}]})");

Utf8View picture_url(MediaAssetKey key, char (&url)[160]) noexcept;

void edit(Utf8View text) noexcept;

void respond(u16 status, Utf8View body = {}) noexcept;

void command_index_invalidation_cases() noexcept;

void composer_slowmode_cases() noexcept;

void builtin_cases() noexcept;

void guild_picture_cases() noexcept;

} // namespace commands_ui_detail

using namespace commands_ui_detail;
} // namespace maicord
