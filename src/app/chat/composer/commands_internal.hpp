#pragma once
// Shared by commands.cpp and its parts (commands_*.cpp); private to them.
#include "maicord/app/chat/composer/command_autocomplete.hpp"
#include "maicord/core/memory.hpp"
#include "../../shell/state.hpp"
#include "composer.hpp"
#include "maicord/protocol/messages/application_commands.hpp"
#include "builtin_commands.hpp"
#include "plugin_commands.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/user/activity.hpp"
#include "maicord/model/user/profile.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail {
namespace commands_detail {}
using namespace commands_detail;
namespace commands_detail {

constexpr u32 choose_base = 100000, rail_base = 200000, option_base = 300000, value_base = 400000;
constexpr u32 clear_selection = choose_base - 3;
// The picker's sections: Frequently Used, each app (by name) and Built-in last.
enum class SectionKind : u8 { frequent, app, builtin };
struct Section {
    const CommandApplication* app; // null for Frequently Used
    u32 first, count;              // rows in entries
    u32 placeholders;              // loading rows after them
    SectionKind kind;
};
// The lane's built-in request (the API operation is the action's own).
constexpr u8 builtin_operation = 9;
constexpr u32 no_row = ~0u;
// The query results' size cap and the scroll-spy's lead.
constexpr u32 query_limit = 20;
constexpr float spy_lead = 20;
// While an index loads, discovery shows seven placeholder rows and query results three.
constexpr u32 discovery_placeholder_rows = 7, query_placeholder_rows = 3;
// The local command message ("<user> used <command>") while
// the interaction is sent, created, and when it fails.
constexpr auto echo_sending = utf8_literal("Sending command...");
constexpr auto echo_no_response = utf8_literal("The application did not respond");
constexpr auto echo_failed = utf8_literal("This interaction failed");
// A failure reported this long after sending reads as no response.
constexpr u64 echo_response_window_ms = 3000;
constexpr u16 message_type_chat_command = 20, message_type_context_command = 23;
constexpr u32 echo_slots = 4;
struct Entry {
    const ApplicationCommand* command;
    const CommandIndex* index;
    const CommandApplication* app;
};
enum class EchoPhase : u8 { free, sending, created, settled };
// One local command message on the active timeline; its id is the nonce. The
// sent command data is kept so a failed one can be sent again.
struct Echo {
    u64 nonce, channel, guild, application, author, interaction_id;
    EchoPhase phase;
    bool failed;
    u16 type;
    u8 name_bytes;
    char name[128];
    u32 data_bytes;
    char data[65536];
};
struct Commands {
    CommandIndex index[2];
    Arena projection;
    Arena retired_index_storage;
    // Picker rows in body order (discovery: section by section; a query:
    // ranked), the rail's sections and the body's sections (the filtered one
    // alone).
    Entry* entries;
    u32 count;
    Section *rail, *body;
    u32 rail_count, body_count, query_placeholders;
    // The query text the results answer ("/" prefixed in their title).
    char query[128];
    u32 query_bytes;
    Entry selected;
    CommandAutocomplete autocomplete;
    u64 issued_nonce, nonce_serial;
    bool suggestions_dirty, echo_dirty;
    // The option chip the caret is in (the active option): its
    // definition, slot and raw value range in the draft; no_option outside.
    JsonValue focused_option;
    u32 focused_slot, value_begin, value_end, choice_count;
    bool choices_shown;
    // The rows of the focused option's choice list: the names put in the draft,
    // and for autocomplete rows the suggestion slot that holds the value.
    char choice_names[25][512];
    u32 choice_bytes[25];
    bool choice_suggested;
    // Option states: each option's last check (empty error when
    // it passed or never ran) and its value when last seen; the active option
    // at the previous sync is checked once the caret leaves it.
    struct OptionState {
        Utf8View error;
        u64 value_hash;
        char message[160];
    } option_states[25];
    u32 previous_slot;
    // Autocomplete choices taken, one per option.
    CommandAcceptedChoice accepted[25];
    char accepted_text[25][3][512];
    u32 accepted_count;
    // The command whose required options were set up as chips; the rows of
    // the selected command's list under its option bar.
    const ApplicationCommand* prepared;
    u32 focused_revision;
    // The selected command's list content (title gaps included, without its bottom inset).
    float list_height;
    // The composer's option bar text (scenes copy it).
    char bar_name[128], bar_detail[1024];
    // The app shake of a refused send: its start, next frame, frame count,
    // horizontal direction and random state.
    u64 shake_started, shake_next;
    u32 shake_frame, shake_seed;
    float shake_direction;
    u64 guild, channel, filter_app, requested_channel, requested_guild, retry_at;
    u32 revision, generation, rendered_generation;
    u32 channel_index_epoch, requested_index_epoch;
    u64 dismissed_document;
    u32 dismissed_revision, catalog_revision;
    // A command chosen outside the picker (profile Commands, command mention);
    // selected once its index is loaded while the draft starts with its name.
    u64 insert_application, insert_command;
    char insert_name[128];
    u32 insert_name_bytes;
    bool insert_wanted;
    u8 pending, operation, requested_slot;
    bool loaded[2], sending, queued, catalog_dirty, indexing_retry;
    float offset, rail_offset;
    // The keyboard selection (no_row: none, as after the pointer hovers a row).
    // An empty query is the discovery list (no initial selection,
    // arrows stop at the ends); a typed query and the option lists wrap.
    // rail_focus is the rail section the keyboard moved into (no_row: none).
    u32 keyboard_row, option_count, rail_focus;
    bool discovery;
    u8 attempts[2];
    Utf8View error;
    char labels[32][512], details[32][1024], payload[65536];
    u32 payload_bytes;
    u64 nonce, send_application, send_channel, send_guild;
    // A context menu wants the index loaded.
    bool context_wanted;
    // A built-in text command rewrote the draft at this revision; it sends as
    // an ordinary message.
    bool builtin_text;
    u32 builtin_text_revision;
    Echo echoes[echo_slots];
    u32 echo_next;
    // Pictures fetched by URL (app icons, bots' default avatars) that picker
    // items name by the URL's hash; a ring past the visible rail and rows.
    struct IconUrl {
        u64 hash;
        u16 bytes;
        char url[160];
    } icon_urls[64];
    u32 icon_url_next;
};

extern Commands* commands;
Commands* ensure_commands() noexcept;

// Command frecency (settings-proto/2): uses
// are tracked locally, saved a few seconds later and replayed over a fresh
// copy until a save confirms them. Allocated with the first picker or use.
constexpr u32 frecency_pending_uses = kCommandFrecencyPendingUses, frequently_shown = 5, frequently_ranked = 100;
constexpr u64 frecency_save_delay_ms = 5000, frecency_retry_ms = 30000;
struct Frecency {
    CommandFrecencyEntry entries[kCommandFrecencyMaximumEntries];
    CommandFrecencyUse pending[frecency_pending_uses];
    u32 count, data_version, pending_count, sent_pending;
    u64 revision, save_due, retry_after;
    // The profile whose unsaved uses were read from disk.
    u64 store_profile;
    bool loaded, fetching, saving, store_read;
    u8 scratch[64 * 1024];
    u8 json[96 * 1024];
};

extern Frecency* frecency;
Frecency* ensure_frecency() noexcept;
bool frecency_current() noexcept;
void frecency_track(const ApplicationCommand& command, u64 guild) noexcept;
bool frecency_install(Utf8View body, u32 confirmed) noexcept;
bool frecency_ready() noexcept;
bool frecency_fetch_due(u64 now) noexcept;
bool frecency_save_due(u64 now) noexcept;
const CommandFrecencyEntry* frecency_entry(const ApplicationCommand& command, u64 guild) noexcept;
i32 frequently_score(const ApplicationCommand& command, u64 guild) noexcept;
void advance_channel_index_epoch(Commands& c) noexcept;
bool retired_text(const Utf8View& text, const Arena& storage) noexcept;
Utf8View field_text(const CommandIndex& index, JsonValue value, const char* key,
                    const Slice<char>& scratch) noexcept;
bool command_space(char value) noexcept;
Utf8View copy_label(char* out, u32 capacity, const Utf8View& a, const Utf8View& b = {},
                    const Utf8View& c = {}, const Utf8View& d = {}) noexcept;
void close_popup() noexcept;
const CommandApplication* find_app(const CommandIndex& source, u64 id) noexcept;
Utf8View section_name(const CommandApplication& app) noexcept;
bool starts_with_ignore_case(const Utf8View& text, const Utf8View& prefix) noexcept;
bool fuzzy_ignore_case(Utf8View needle, Utf8View text) noexcept;
i32 compare_names(Utf8View a, Utf8View b) noexcept;

// The query text: before the word ahead of a
// "name:" argument, at most three words; a cut or a trailing space makes it
// "terminated" (only that command and its subcommands match).
struct PickerQuery {
    Utf8View text;
    bool terminated;
};

PickerQuery picker_query(Utf8View input) noexcept;

// The query score (lower ranks first): name prefix 0,
// first-word and rest prefixes 1, name substring 2, option prefix 3, option
// substring 4, section prefix 5 or substring 6, description substring 7,
// fuzzy name 9, option 10, section 11, description 12; ~0u: no match.
struct OptionMatch {
    const CommandIndex* index;
    Utf8View name, query;
    u32 score;
};

bool project(const Utf8View& input) noexcept;
MediaAssetKey app_picture(const CommandApplication* app) noexcept;

constexpr u32 no_option = ~0u;
// The selected command's options in definition order.
struct OptionList {
    JsonValue options[25];
    u32 count;
};

OptionList command_options() noexcept;
bool option_flag(JsonValue option, const char* key) noexcept;
u64 option_type(const CommandIndex& index, JsonValue option) noexcept;
u32 typed_options(const Utf8View& input, Slice<CommandArgumentSpan> spans) noexcept;
u32 chip_at(const Utf8View&, const CommandArgumentSpan* spans, u32 count, u32 caret) noexcept;
bool option_end_at(const Utf8View& input, u32 at) noexcept;
Utf8View caret_word(Utf8View input, u32 caret) noexcept;
ConstSlice<CommandAcceptedChoice> accepted_choices() noexcept;
void check_options(const CommandArgumentSpan* spans, u32 count, u32 active_slot, bool validate_all,
                   bool allow_empty) noexcept;
void reset_option_states(Commands& c) noexcept;
void update_focus(Utf8View input) noexcept;
bool autocomplete_enabled() noexcept;
u64 next_nonce() noexcept;
Utf8View focused_query() noexcept;
float list_item_height(const PopupItem& item) noexcept;
bool render_command(const UiLayoutNode& surface) noexcept;
float rail_top(const Commands& c, u32 r) noexcept;
float rail_extent(const Commands& c) noexcept;
float body_extent(const Commands& c) noexcept;
float row_top(const Commands& c, u32 k) noexcept;
u32 active_rail(const Commands& c) noexcept;
bool render_picker(const UiLayoutNode& surface) noexcept;
bool render() noexcept;

// The deferred-response "thinking" flag the timeline draws as "<app> is thinking...".
constexpr u32 message_flag_loading = 1u << 7;

Echo* find_echo(u64 nonce, u64 interaction_id) noexcept;
void echo_write(Echo& echo, Utf8View content, u32 flags, MessageDeliveryState delivery) noexcept;
u64 echo_author(u64 application_id, const CommandApplication* app) noexcept;
void queue_command(u64 application, u64 author, const Utf8View& name, u16 type) noexcept;
void echo_fail(Echo& echo, const Utf8View& reason) noexcept;
void echo_remove(Echo& echo) noexcept;
void mark_indexes_stale(Commands& c) noexcept;
bool find_entry(u64 application_id, u64 command_id, Utf8View name, Entry* output) noexcept;
bool has_attachment_option(const CommandIndex& index, const ApplicationCommand& command) noexcept;
bool commands_permitted(const Commands& c) noexcept;
void resolve_wanted(const Utf8View& input) noexcept;
bool select_typed(Utf8View input) noexcept;
MessageComposerResult normalize_markers(Utf8View input, bool command) noexcept;
MessageComposerResult chip_single_option(const Utf8View& input, u32 typed) noexcept;
MessageComposerResult set_up_command() noexcept;
MessageComposerResult select_chip(const CommandArgumentSpan& span) noexcept;
void shake_app() noexcept;
void shake_step(u64 now) noexcept;
bool send_check(bool* blocked) noexcept;
bool command_in_draft() noexcept;

} // namespace commands_detail
} // namespace maicord::app_detail
