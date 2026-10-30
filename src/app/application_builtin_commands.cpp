#include "application_builtin_commands.hpp"
#include "application_account_access.hpp"
#include "application_account_presentation.hpp"
#include "application_command_request.hpp"
#include "application_media_picker.hpp"
#include "application_private_channel.hpp"
#include "application_private_conversation.hpp"
#include "application_state.hpp"
#include "application_thread_actions.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/app/message_send_response.hpp"
#include "maicord/app/private_conversation.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/member_moderation.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/protocol/guild_admin.hpp"
#include "maicord/ui/media_picker.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/private_conversation.hpp"

namespace maicord::app_detail {
namespace {
// The client's built-in commands (its ids, negated), in its section order
// (by name). /tts (no text-to-speech here), /schedule and /roll-dice (behind
// experiments) are left out.
enum Builtin : u64 {
    shrug = 1,
    tableflip = 2,
    unflip = 3,
    me = 5,
    spoiler = 6,
    nick = 7,
    thread = 10,
    kick = 11,
    ban = 12,
    timeout = 13,
    msg = 14,
    leave = 15,
    gif = 16,
    sticker = 17,
};
constexpr auto kIndex = utf8_literal(R"JSON({"applications":[{"id":"1","name":"Built-in"}],"application_commands":[
{"id":"12","application_id":"1","version":"1","type":1,"name":"ban","description":"Ban user","options":[{"name":"user","description":"The user to ban","type":6,"required":true},{"name":"delete_messages","description":"How much of their recent message history to delete","type":4,"required":true,"choices":[{"name":"Don't Delete Any","value":0},{"name":"Previous Hour","value":3600},{"name":"Previous 6 Hours","value":21600},{"name":"Previous 12 Hours","value":43200},{"name":"Previous 24 Hours","value":86400},{"name":"Previous 3 Days","value":259200},{"name":"Previous 7 Days","value":604800}]},{"name":"reason","description":"The reason for banning, if any","type":3}]},
{"id":"16","application_id":"1","version":"1","type":1,"name":"gif","description":"Search Animated GIFs on the Web","options":[{"name":"query","description":"Search for a GIF","type":3,"required":true}]},
{"id":"11","application_id":"1","version":"1","type":1,"name":"kick","description":"Kick user","options":[{"name":"user","description":"The user to kick","type":6,"required":true},{"name":"reason","description":"The reason for kicking, if any","type":3}]},
{"id":"15","application_id":"1","version":"1","type":1,"name":"leave","description":"Leave Group","options":[{"name":"silent","description":"Leave without notifying other members","type":5}]},
{"id":"5","application_id":"1","version":"1","type":1,"name":"me","description":"Displays text with emphasis.","options":[{"name":"message","description":"Your message","type":3,"required":true}]},
{"id":"14","application_id":"1","version":"1","type":1,"name":"msg","description":"Message user","options":[{"name":"user","description":"The user to message","type":6,"required":true},{"name":"message","description":"Message to send","type":3,"required":true}]},
{"id":"7","application_id":"1","version":"1","type":1,"name":"nick","description":"Change nickname on this server.","options":[{"name":"new_nick","description":"New nickname","type":3,"max_length":32}]},
{"id":"1","application_id":"1","version":"1","type":1,"name":"shrug","description":"Appends ¯\\_(ツ)_/¯ to your message.","options":[{"name":"message","description":"Your message","type":3}]},
{"id":"6","application_id":"1","version":"1","type":1,"name":"spoiler","description":"Marks your message as a spoiler.","options":[{"name":"message","description":"Your message","type":3,"required":true}]},
{"id":"17","application_id":"1","version":"1","type":1,"name":"sticker","description":"Search your stickers","options":[{"name":"query","description":"Search for a sticker","type":3,"required":true}]},
{"id":"2","application_id":"1","version":"1","type":1,"name":"tableflip","description":"Appends (╯°□°)╯︵ ┻━┻ to your message.","options":[{"name":"message","description":"Your message","type":3}]},
{"id":"10","application_id":"1","version":"1","type":1,"name":"thread","description":"Start new thread","options":[{"name":"name","description":"Type a name for your thread","type":3,"required":true,"max_length":100},{"name":"message","description":"Type the first message in your thread","type":3,"required":true}]},
{"id":"13","application_id":"1","version":"1","type":1,"name":"timeout","description":"Time out user","options":[{"name":"user","description":"The user to time out","type":6,"required":true},{"name":"duration","description":"How long they should be timed out for","type":4,"required":true,"choices":[{"name":"60 secs","value":60},{"name":"5 mins","value":300},{"name":"10 mins","value":600},{"name":"1 hour","value":3600},{"name":"1 day","value":86400},{"name":"1 week","value":604800}]},{"name":"reason","description":"The reason for timing them out, if any","type":3}]},
{"id":"3","application_id":"1","version":"1","type":1,"name":"unflip","description":"Appends ┬─┬ ノ( ゜-゜ノ) to your message.","options":[{"name":"message","description":"Your message","type":3}]}
]})JSON");

enum class Action : u8 { none, nick, thread, kick, ban, timeout };
struct State {
    CommandIndex index;
    bool decoded;
    Action action;
    bool sending;
    u64 guild, channel, target, request_id;
    u32 seconds;
    u32 text_bytes, tag_bytes, body_bytes;
    char text[512], tag[128], body[1024];
    // A message waiting for its channel: /msg's DM (recipient) or /thread's
    // new thread (thread), noticed in origin on failure.
    MessageComposer message;
    u64 recipient, thread, origin;
    u32 notice_sequence;
};
State g_builtins;

Utf8View view(const char* data, u32 bytes) noexcept {
    return {data, bytes};
}
bool space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}
Utf8View trim(Utf8View text) noexcept {
    while (text.bytes && space(text.data[0]))
        text = {text.data + 1, text.bytes - 1};
    while (text.bytes && space(text.data[text.bytes - 1]))
        --text.bytes;
    return text;
}
// Joins parts into storage; empty on overflow.
Utf8View join(Slice<char> storage, Utf8View a, Utf8View b = {}, Utf8View c = {}) noexcept {
    u32 used = 0;
    const Utf8View parts[] = {a, b, c};
    for (const auto part : parts) {
        if (part.bytes > storage.count - used)
            return {};
        copy_bytes(storage.data + used, part.data, part.bytes);
        used += part.bytes;
    }
    return {storage.data, used};
}

// One option of command_arguments' array: {"name","type","value"}.
struct OptionFind {
    const JsonIndexedDocument* document;
    Utf8View name;
    JsonValue value;
};
bool find_option(void* context, u32, JsonValue item) noexcept {
    auto& find = *static_cast<OptionFind*>(context);
    char name[64];
    Utf8View text{};
    if (command_json_text(command_member(*find.document, item, utf8_literal("name")), {name, sizeof(name)}, &text) &&
        utf8_equal(text, find.name))
        find.value = command_member(*find.document, item, utf8_literal("value"));
    return true;
}
struct Options {
    JsonIndexedDocument document;
    bool ok;
};
Options parse_options(Utf8View json) noexcept {
    Options options{};
    auto& ui = *g_app.account_ui;
    const auto scan =
        json_scan_structurals_avx1(json, {ui.message_history_page_structurals, kMessageHistoryPageStructuralCount});
    options.ok = scan.ok();
    options.document = {json, {ui.message_history_page_structurals, options.ok ? scan.count : 0u}};
    return options;
}
JsonValue option(const Options& options, Utf8View name) noexcept {
    OptionFind find{&options.document, name, {}};
    if (options.ok)
        json_visit_indexed_array(options.document, {options.document.input, 0, JsonValueKind::array}, 8, find_option,
                                 &find);
    return find.value;
}
Utf8View option_text(const Options& options, Utf8View name, Slice<char> storage) noexcept {
    Utf8View text{};
    return command_json_text(option(options, name), storage, &text) ? text : Utf8View{};
}
u64 option_number(const Options& options, Utf8View name) noexcept {
    u64 value = 0;
    json_value_to_u64_or_string(option(options, name), &value);
    return value;
}

void notice(u64 channel, Utf8View text) noexcept {
    // The client's bot message, only in the channel it was asked from.
    if (!g_app.account_ui || !channel || g_app.account_ui->active_channel_id != channel || !text.bytes)
        return;
    // Local notice ids leave the send rejection notices' low sequence range;
    // the id's low bit is always set, so the sequence steps by two.
    const u32 sequence = 0x200000u | ((++g_builtins.notice_sequence << 1) & 0x1FFFFEu);
    auto input = message_send_rejection_notice_input(channel, text, clock_unix_milliseconds(), sequence);
    // The client's bot messages come from its local system user, id 1.
    input.author_id = 1;
    if (!message_timeline_insert_local(&g_app.message_view.timeline, input).ok())
        log_write(LogLevel::warning, "commands", "builtin_notice_insert_failed");
    refresh_account_frame(true);
}
void clear_action() noexcept {
    auto& s = g_builtins;
    s.action = Action::none;
    s.sending = false;
    s.request_id = 0;
    secure_clear_bytes(s.text, sizeof(s.text));
    secure_clear_bytes(s.body, sizeof(s.body));
    s.text_bytes = s.tag_bytes = s.body_bytes = 0;
}
void clear_message() noexcept {
    auto& s = g_builtins;
    s.recipient = s.thread = s.origin = 0;
    if (s.message.text)
        message_composer_clear(&s.message);
}
bool hold_message(Utf8View text, u64 origin) noexcept {
    auto& s = g_builtins;
    if (!s.message.text && !message_composer_initialize_owned(&s.message))
        return false;
    s.origin = origin;
    return message_composer_restore(&s.message, 0, text, text.bytes, text.bytes);
}
Utf8View user_tag(u64 user_id) noexcept {
    const auto* user = g_app.account_ui ? user_store_find(g_app.account_ui->user_store, user_id) : nullptr;
    if (!user)
        return {};
    const auto username = user_store_username(g_app.account_ui->user_store, *user);
    return username.bytes ? username : user_store_name(g_app.account_ui->user_store, *user);
}
bool permitted(const ChannelRecord* channel, PermissionSet required) noexcept {
    return channel && channel->guild_id && channel->permissions_resolved &&
           permission_has(channel->effective_permissions, required);
}
// /kick, /ban and /timeout: the permission gate, then the action
// on the lane, or its refusal notice at once.
bool moderate(Action action, const Options& options, u64 channel_id) noexcept {
    const auto* channel = application_channel_record(channel_id);
    if (!channel || !channel->guild_id || g_builtins.action != Action::none)
        return false;
    const u64 user = option_number(options, utf8_literal("user"));
    const u8 needed = action == Action::kick ? moderation_action_kick
                      : action == Action::ban ? moderation_action_ban
                                              : moderation_action_timeout;
    const auto capabilities =
        moderation_capabilities(g_app.account_ui->chat_state, g_app.account_ui->guild_member_store, channel->guild_id,
                                user, clock_unix_milliseconds() / 1000);
    const Utf8View verb = action == Action::kick  ? utf8_literal("kick")
                          : action == Action::ban ? utf8_literal("ban")
                                                  : utf8_literal("time out");
    char line[160];
    if (!user || !(capabilities.allowed & needed)) {
        notice(channel_id, join({line, sizeof(line)}, utf8_literal("You do not have the appropriate permissions to "),
                                verb, utf8_literal(" that user.")));
        return true;
    }
    auto& s = g_builtins;
    const auto tag = user_tag(user);
    // The client needs the user (only a ban names one it does not know).
    if (!tag.bytes && action != Action::ban) {
        notice(channel_id, join({line, sizeof(line)}, utf8_literal("Oops! We encountered an error when trying to "),
                                verb, utf8_literal(" that user.")));
        return true;
    }
    char reason_storage[512];
    const auto reason = option_text(options, utf8_literal("reason"), {reason_storage, sizeof(reason_storage)});
    s.text_bytes = utf8_copy_truncated(s.text, sizeof(s.text), reason);
    s.tag_bytes = utf8_copy_truncated(s.tag, sizeof(s.tag), tag);
    s.seconds = static_cast<u32>(
        option_number(options, action == Action::ban ? utf8_literal("delete_messages") : utf8_literal("duration")));
    s.guild = channel->guild_id;
    s.channel = channel_id;
    s.target = user;
    s.action = action;
    return true;
}
} // namespace

const CommandIndex* builtin_commands_index() noexcept {
    auto& s = g_builtins;
    if (!s.decoded && g_app.account_ui)
        s.decoded = command_index_decode(kIndex,
                                         {g_app.account_ui->message_history_page_structurals,
                                          kMessageHistoryPageStructuralCount},
                                         &s.index);
    return s.decoded ? &s.index : nullptr;
}

bool builtin_command_available(const ApplicationCommand& command, u64 channel_id) noexcept {
    const auto* channel = application_channel_record(channel_id);
    switch (command.id) {
    case nick:
        return permitted(channel, kPermissionChangeNickname) || permitted(channel, kPermissionManageNicknames);
    case thread:
        // Public threads start in text and announcement channels.
        return channel && (channel->type == 0 || channel->type == 5) &&
               permitted(channel, kPermissionCreatePublicThreads | kPermissionReadMessageHistory);
    case kick:
        return permitted(channel, kPermissionKickMembers);
    case ban:
        return permitted(channel, kPermissionBanMembers);
    case timeout:
        return permitted(channel, kPermissionModerateMembers);
    case leave:
        return channel && channel->type == 3;
    default:
        return true;
    }
}

BuiltinCommandRun builtin_command_run(const ApplicationCommand& command, Utf8View options_json, u64 channel_id,
                                      Slice<char> text_storage, Utf8View* text) noexcept {
    if (!g_app.account_ui || !text)
        return BuiltinCommandRun::failed;
    const auto options = parse_options(options_json);
    if (!options.ok)
        return BuiltinCommandRun::failed;
    char value_storage[4096];
    const Slice<char> value_slice{value_storage, sizeof(value_storage)};
    const auto message = option_text(options, utf8_literal("message"), value_slice);
    switch (command.id) {
    case shrug:
    case tableflip:
    case unflip: {
        const Utf8View suffix = command.id == shrug       ? utf8_literal(" ¯\\_(ツ)_/¯")
                                : command.id == tableflip ? utf8_literal(" (╯°□°)╯︵ ┻━┻")
                                                          : utf8_literal(" ┬─┬ノ( º _ ºノ)");
        *text = trim(join(text_storage, message, suffix));
        return text->bytes ? BuiltinCommandRun::text : BuiltinCommandRun::failed;
    }
    case me:
        *text = join(text_storage, utf8_literal("_"), message, utf8_literal("_"));
        return text->bytes ? BuiltinCommandRun::text : BuiltinCommandRun::failed;
    case spoiler:
        *text = trim(join(text_storage, utf8_literal("||"), message, utf8_literal("||")));
        return text->bytes ? BuiltinCommandRun::text : BuiltinCommandRun::failed;
    case gif:
    case sticker:
        // The client's integration commands send only a chosen result; Enter
        // shakes the app and keeps the draft.
        return BuiltinCommandRun::rejected;
    case leave: {
        bool silent = false;
        json_value_to_bool(option(options, utf8_literal("silent")), &silent);
        return application_private_channel_leave(channel_id, silent) ? BuiltinCommandRun::handled
                                                                      : BuiltinCommandRun::failed;
    }
    case msg: {
        // Open the DM (it becomes the selected conversation), then send there.
        const u64 user = option_number(options, utf8_literal("user"));
        clear_message();
        if (!user || !hold_message(message, channel_id))
            return BuiltinCommandRun::failed;
        g_builtins.recipient = user;
        if (!queue_private_conversation(PrivateConversationOperation::open, user)) {
            clear_message();
            notice(channel_id, utf8_literal("Oops! We encountered an error when trying to message that user."));
            return BuiltinCommandRun::handled;
        }
        return builtin_commands_tick() ? BuiltinCommandRun::handled : BuiltinCommandRun::failed;
    }
    case nick:
    case thread: {
        auto& s = g_builtins;
        const auto* channel = application_channel_record(channel_id);
        if (!channel || !channel->guild_id || s.action != Action::none)
            return BuiltinCommandRun::failed;
        ProtocolWriter body{{reinterpret_cast<u8*>(s.body), sizeof(s.body)}, 0};
        bool ok;
        if (command.id == nick) {
            char nick_storage[256];
            ok = protocol_writer_append_literal(&body, "{\"nick\":") &&
                 protocol_writer_append_json_string(
                     &body, option_text(options, utf8_literal("new_nick"), {nick_storage, sizeof(nick_storage)})) &&
                 protocol_writer_append_byte(&body, '}');
        } else {
            char name_storage[512];
            // The request body: name, a public thread, the channel's default archive
            // duration (three days unless set) and the location.
            ok = protocol_writer_append_literal(&body, "{\"name\":") &&
                 protocol_writer_append_json_string(
                     &body, option_text(options, utf8_literal("name"), {name_storage, sizeof(name_storage)})) &&
                 protocol_writer_append_literal(&body, ",\"type\":11,\"auto_archive_duration\":") &&
                 protocol_writer_append_u64(&body, channel_default_archive_minutes(*channel)) &&
                 protocol_writer_append_literal(&body, ",\"location\":\"Slash Command\"}");
            clear_message();
            ok = ok && hold_message(message, channel_id);
        }
        if (!ok) {
            clear_action();
            clear_message();
            return BuiltinCommandRun::failed;
        }
        s.body_bytes = body.used;
        s.guild = channel->guild_id;
        s.channel = channel_id;
        s.action = command.id == nick ? Action::nick : Action::thread;
        return BuiltinCommandRun::handled;
    }
    case kick:
        return moderate(Action::kick, options, channel_id) ? BuiltinCommandRun::handled
                                                                     : BuiltinCommandRun::failed;
    case ban:
        return moderate(Action::ban, options, channel_id) ? BuiltinCommandRun::handled
                                                                    : BuiltinCommandRun::failed;
    case timeout:
        return moderate(Action::timeout, options, channel_id) ? BuiltinCommandRun::handled
                                                                        : BuiltinCommandRun::failed;
    default:
        return BuiltinCommandRun::failed;
    }
}

bool builtin_commands_pending() noexcept {
    return g_builtins.action != Action::none && !g_builtins.sending;
}

bool builtin_commands_submit(u64 request_id, bool* busy) noexcept {
    auto& s = g_builtins;
    *busy = false;
    if (!builtin_commands_pending() || !request_id)
        return false;
    RestClientSubmitResult result{RestClientSubmitError::invalid_input};
    if (s.action == Action::nick || s.action == Action::thread) {
        if (g_app.account.api.application_command)
            result = g_app.account.api.application_command(
                g_app.account.instance, s.channel, s.guild, 0, 0, view(s.body, s.body_bytes), request_id,
                s.action == Action::nick ? kBuiltinCommandNick : kBuiltinCommandThread);
    } else if (g_app.account.api.guild_admin) {
        GuildAdminRequest request{};
        MemberPatch patch{};
        request.guild_id = s.guild;
        request.target_id = s.target;
        // The client always sends the reason, empty when none was given.
        request.reason = {s.text_bytes ? s.text : "", s.text_bytes};
        if (s.action == Action::kick)
            request.operation = GuildAdminOperation::member_kick;
        else if (s.action == Action::ban) {
            request.operation = GuildAdminOperation::ban_add;
            request.delete_message_seconds = s.seconds;
        } else {
            request.operation = GuildAdminOperation::member_update;
            patch.fields = member_patch_timeout;
            patch.communication_disabled_until_ms = clock_unix_milliseconds() + u64{s.seconds} * 1000;
            request.member = &patch;
        }
        result = g_app.account.api.guild_admin(g_app.account.instance, &request, request_id);
    }
    if (result.error == RestClientSubmitError::busy) {
        *busy = true;
        return false;
    }
    if (!result.ok()) {
        log_u64(LogLevel::warning, "commands", "builtin_submit_error", static_cast<u64>(result.error));
        RestClientResponse failed{};
        failed.request_id = request_id;
        s.sending = true;
        s.request_id = request_id;
        builtin_commands_response(failed, {});
        return false;
    }
    s.sending = true;
    s.request_id = request_id;
    return true;
}

bool builtin_commands_response(const RestClientResponse& response, Utf8View body) noexcept {
    auto& s = g_builtins;
    if (!s.sending || response.request_id != s.request_id)
        return true;
    const bool ok = response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    const Action action = s.action;
    const u64 channel = s.channel;
    char line[512];
    const Slice<char> storage{line, sizeof(line)};
    Utf8View text{};
    const Utf8View tag = s.tag_bytes ? view(s.tag, s.tag_bytes) : Utf8View{};
    char id_digits[24];
    const Utf8View named = tag.bytes ? tag : view(id_digits, decimal_write(id_digits, s.target));
    switch (action) {
    case Action::nick: {
        char nick_storage[256];
        Utf8View nick{};
        if (ok && body.bytes) {
            auto& ui = *g_app.account_ui;
            const auto scan = json_scan_structurals_avx1(
                body, {ui.message_history_page_structurals, kMessageHistoryPageStructuralCount});
            if (scan.ok()) {
                const JsonIndexedDocument document{body, {ui.message_history_page_structurals, scan.count}};
                command_json_text(command_member(document, {body, 0, JsonValueKind::object}, utf8_literal("nick")),
                                  {nick_storage, sizeof(nick_storage)}, &nick);
            }
        }
        text = !ok ? (response.transport_ok() && response.status_code == 403
                          ? utf8_literal("You do not have the **Change Nickname** permission on this server.")
                          : utf8_literal("Your nickname could not be changed on this server."))
               : nick.bytes ? join(storage, utf8_literal("Your nickname on this server has been changed to **"), nick,
                                   utf8_literal("**."))
                            : utf8_literal("Your nickname on this server has been reset.");
        break;
    }
    case Action::thread: {
        u64 id = 0;
        if (ok && body.bytes) {
            auto& ui = *g_app.account_ui;
            const auto scan = json_scan_structurals_avx1(
                body, {ui.message_history_page_structurals, kMessageHistoryPageStructuralCount});
            if (scan.ok())
                json_value_to_u64_or_string(
                    command_member({body, {ui.message_history_page_structurals, scan.count}},
                                   {body, 0, JsonValueKind::object}, utf8_literal("id")),
                    &id);
        }
        // The first message goes to the thread once its channel arrives.
        if (id)
            s.thread = id;
        else {
            log_u64(LogLevel::warning, "commands", "builtin_thread_status", response.status_code);
            clear_message();
        }
        break;
    }
    case Action::kick:
        text = ok ? join(storage, named, utf8_literal(" was kicked from the server."))
                  : utf8_literal("Oops! We encountered an error when trying to kick that user.");
        break;
    case Action::ban:
        text = ok ? join(storage, named, utf8_literal(" was banned from the server."))
                  : utf8_literal("Oops! We encountered an error when trying to ban that user.");
        break;
    case Action::timeout: {
        // The client formats the seconds as a number: "86,400 seconds."
        char tail[48];
        u32 digits = decimal_write_grouped(tail, s.seconds);
        digits += utf8_copy_truncated(tail + digits, 48u - digits, utf8_literal(" seconds."));
        text = ok ? join(storage, named, utf8_literal(" timed out for "), view(tail, digits))
                  : utf8_literal("Oops! We encountered an error when trying to time out that user.");
        break;
    }
    default:
        break;
    }
    clear_action();
    notice(channel, text);
    return action != Action::thread || builtin_commands_tick();
}

bool builtin_commands_tick() noexcept {
    auto& s = g_builtins;
    if ((!s.recipient && !s.thread) || !g_app.account_ui)
        return true;
    auto& ui = *g_app.account_ui;
    u64 channel = 0;
    if (s.recipient) {
        const auto& request = ui.private_conversation;
        if (request.phase == PrivateConversationPhase::failed &&
            request.operation == PrivateConversationOperation::open && request.target_id == s.recipient) {
            const u64 origin = s.origin;
            clear_message();
            notice(origin, utf8_literal("Oops! We encountered an error when trying to message that user."));
            return true;
        }
        channel = private_conversation_cached(ui.chat_state, s.recipient);
        if (!channel)
            return true;
        // A navigation change while the DM opened must not send elsewhere.
        if (channel != ui.active_channel_id) {
            clear_message();
            return true;
        }
    } else {
        if (!application_thread_actions_channel_sendable(s.thread))
            return true;
        channel = s.thread;
    }
    s.message.channel_id = channel;
    const auto queued = message_composer_controller_queue_send(
        &ui.message_composer_controller, true, ui.chat_state.self_user_id, clock_unix_milliseconds(),
        clock_monotonic_milliseconds(), 0, {}, {}, &s.message);
    if (!queued.ok())
        log_u64(LogLevel::warning, "commands", "builtin_message_queue_error", static_cast<u64>(queued.error));
    clear_message();
    return refresh_account_frame(true);
}

void builtin_commands_release() noexcept {
    clear_action();
    clear_message();
    message_composer_release(&g_builtins.message);
    command_index_release(&g_builtins.index);
    reset_value(&g_builtins);
}

bool builtin_command_results(const ApplicationCommand* command, Utf8View query, UiRect anchor) noexcept {
    if (!command || command->application_id != kBuiltinCommandApplication ||
        (command->id != gif && command->id != sticker)) {
        application_media_picker_inline_close();
        return true;
    }
    return application_media_picker_inline(command->id == gif ? UiMediaPickerKind::gif : UiMediaPickerKind::sticker,
                                           trim(query), anchor);
}

bool builtin_command_results_key(u32 key, bool* handled) noexcept {
    return application_media_picker_inline_key(key, handled);
}
} // namespace maicord::app_detail
