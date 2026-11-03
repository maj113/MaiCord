// MaiCord plugin authoring layer for ABI 6: header-only C++23 over maicord_plugin.h.
// Standalone: no MaiCord core headers, no STL, CRT, exceptions or RTTI.
// maicord/sdk/README.md is the full reference.
//
// A plugin is one .cpp file of declarations:
//
//   #include <maicord/sdk/plugin.hpp>
//   using namespace mc;
//
//   MC_SETTINGS((Toggle, loud, "Shout", false));        // optional: typed mc::Settings
//   struct State { u32 sent; };
//   MC_STATE(State, .version = 1);                      // optional: host-owned state
//   static void pre_send(Ctx& ctx, Draft& draft) { ... }
//   MC_HOOKS(pre_send);                                 // hook kinds come from parameter types
//   MC_COMMAND(shout, (Str text), "Shout it") { ... return Reply::send(text); }
//   MC_MENU(search, menu::image, "Search the web") { ... target ... }
//   MC_BUTTON(peek, surface::chat_bar, "Peek", "lucide/eye") { ... button ... }
//   MC_PLUGIN(.owner = 0x..., .name = "...", .version = "1.0.0", .summary = "...");
//
// MC_PLUGIN ends the file and defines the exported descriptor. Use the macros
// at global scope; MC_SETTINGS, MC_STATE, MC_HOOKS and MC_PLUGIN once each.
// Designated options follow declaration order.
//
// Hook signatures (MC_HOOKS; mc::filter<fn, bits> sets hook.filter):
//   void (Ctx&, Draft&)                                   PRE_SEND
//   void (Ctx&, EditDraft&)                               PRE_EDIT
//   bool (Ctx&, const Message&, const Click&)             MESSAGE_CLICK; true = handled
//   bool (Ctx&, const DeletedMessage&)                    MESSAGE_DELETED; true = keep
//   bool (Ctx&, const Message& before, const Message&)    MESSAGE_EDITED; true = keep history
//   void (Ctx&, const ev::<Kind>&)                        EVENTS: one handler per kind, merged
//   void (Ctx&, const Message&, Decoration&)              DECORATE_MESSAGE
//   void (Ctx&, const User&, Decoration&)                 DECORATE_USER
//   void (Ctx&, const Channel&, Decoration&)              DECORATE_CHANNEL
//   void (Ctx&, const Guild&, Decoration&)                DECORATE_GUILD
//   void (Ctx&, const Header&, Decoration&)               DECORATE_HEADER
//   void (Ctx&, const NodeAction&)                        NODE_ACTION
//   bool (Ctx&, const TypingNotice&)                      PRE_TYPING; true = suppress
//   void (Ctx&, const SettingChanged&)                    SETTING_CHANGED
//   void (Ctx&, const KeybindPressed&)                    KEYBIND
//   void (Ctx&, const Timer&)                             TIMER
// The COMPLETION hook is always present: ctx.http, ctx.modal and ctx.banner
// take the handler that receives their result as a template argument.
//
// Settings rows (MC_SETTINGS): (Type, field, "Label", default, options...)
//   Toggle    bool    default true/false          .description .flags .visible_if
//   Int       i32     default n                   .min .max .description .flags .visible_if
//   Slider    i32     default n                   .min .max .step .description .flags .visible_if
//   Choice    u32     default index               .items = {"A", "B"} .description .flags .visible_if
//   Text      Str     default "text" or {}        .description .hint .max_bytes .flags .visible_if
//   Secret    Str     default {}                  .description .hint .max_bytes .flags .visible_if
//   TextList  StrList default {"a", "b"} or {}    .description .hint .max_bytes .flags .visible_if
//   Colour    u32     default 0xRRGGBB            .description .flags .visible_if
//   Keybind   u32     default MC_KEY(vk, mods)/0  .description .flags .visible_if
//   Channel   u64     default {}                  .description .flags .visible_if
//   UserList  IdList  default {}                  .description .flags .visible_if
//   Header    -       default {}                  .description .flags .visible_if
// .visible_if = Setting::toggle, or Setting::choice with .visible_value = index.
// The field name is the persisted key: 1..32 bytes of [a-z0-9_].
//
// Define MC_RUNTIME (clang-cl) in the plugin's one translation unit to get
// memcpy, memmove, memset, memcmp, __chkstk and _fltused without a CRT.
#pragma once

#include "maicord_plugin.h"

namespace mc {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

// ---- Strings ------------------------------------------------------------------

// A UTF-8 view; layout-identical to mc_str, so it passes to the C ABI as is.
struct Str : mc_str {
    constexpr Str() : mc_str{nullptr, 0, 0} {
    }
    constexpr Str(const char* bytes_data, u32 byte_count) : mc_str{bytes_data, byte_count, 0} {
    }
    constexpr Str(const mc_str& view) : mc_str{view.data, view.bytes, 0} {
    }
    // A string literal (the terminating NUL is not part of the view).
    template <size_t N>
    constexpr Str(const char (&literal)[N]) : mc_str{literal, static_cast<u32>(N - 1), 0} {
    }

    constexpr u32 size() const {
        return bytes;
    }
    constexpr bool empty() const {
        return bytes == 0;
    }
    constexpr char operator[](u32 index) const {
        return data[index];
    }
    constexpr const char* begin() const {
        return data;
    }
    constexpr const char* end() const {
        return data + bytes;
    }
    // Bytes [from, from + count), clamped to the view.
    constexpr Str sub(u32 from, u32 count = 0xFFFFFFFFu) const {
        if (from > bytes)
            from = bytes;
        if (count > bytes - from)
            count = bytes - from;
        return Str(data + from, count);
    }
    // The index of the first c at or after from; size() when there is none.
    constexpr u32 find(char c, u32 from = 0) const {
        for (u32 i = from; i < bytes; ++i)
            if (data[i] == c)
                return i;
        return bytes;
    }
    constexpr bool starts_with(Str prefix) const {
        return prefix.bytes <= bytes && sub(0, prefix.bytes) == prefix;
    }
    // Equality ignoring ASCII case.
    constexpr bool equals_ascii_nocase(Str other) const {
        if (other.bytes != bytes)
            return false;
        for (u32 i = 0; i < bytes; ++i)
            if (lower(data[i]) != lower(other.data[i]))
                return false;
        return true;
    }
    // Without leading and trailing ASCII spaces and tabs.
    constexpr Str trimmed() const {
        u32 from = 0;
        u32 to = bytes;
        while (from < to && (data[from] == ' ' || data[from] == '\t'))
            ++from;
        while (to > from && (data[to - 1] == ' ' || data[to - 1] == '\t'))
            --to;
        return Str(data + from, to - from);
    }
    static constexpr char lower(char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
    }

    friend constexpr bool operator==(const Str& a, const Str& b) {
        if (a.bytes != b.bytes)
            return false;
        for (u32 i = 0; i < a.bytes; ++i)
            if (a.data[i] != b.data[i])
                return false;
        return true;
    }
};

// A list of strings (TextList settings); layout-identical to mc_list.
struct StrList : mc_list {
    struct iterator {
        const mc_str* at;
        constexpr Str operator*() const {
            return Str(*at);
        }
        constexpr iterator& operator++() {
            ++at;
            return *this;
        }
        constexpr bool operator!=(iterator other) const {
            return at != other.at;
        }
    };
    constexpr u32 size() const {
        return count;
    }
    constexpr bool empty() const {
        return count == 0;
    }
    constexpr Str operator[](u32 index) const {
        return Str(items[index]);
    }
    constexpr iterator begin() const {
        return {items};
    }
    constexpr iterator end() const {
        return {items + count};
    }
};

// Fixed-capacity text. Appends that don't fit change nothing and set overflowed().
template <u32 N> class Text {
    static_assert(N > 0 && N <= (1u << 24), "Text<N>: capacity out of range");

  public:
    Text() = default;

    static constexpr u32 capacity() {
        return N;
    }
    u32 size() const {
        return size_;
    }
    bool empty() const {
        return size_ == 0;
    }
    bool overflowed() const {
        return overflow_;
    }
    char* data() {
        return data_;
    }
    const char* data() const {
        return data_;
    }
    Str view() const {
        return Str(data_, size_);
    }
    operator Str() const {
        return view();
    }

    void clear() {
        size_ = 0;
        overflow_ = false;
    }
    bool append(Str text) {
        if (text.bytes > N - size_) {
            overflow_ = true;
            return false;
        }
        for (u32 i = 0; i < text.bytes; ++i)
            data_[size_ + i] = text.data[i];
        size_ += text.bytes;
        return true;
    }
    bool push(char c) {
        return append(Str(&c, 1));
    }
    // After writing into data() directly: the first bytes are the text.
    void set_size(u32 bytes) {
        size_ = bytes < N ? bytes : N;
    }

  private:
    u32 size_ = 0;
    bool overflow_ = false;
    char data_[N];
};

// ---- Hook arguments -----------------------------------------------------------

// The text you're about to send (MC_HOOK_PRE_SEND).
struct Draft : mc_draft {
    Str text() const {
        return Str(mc_draft::text);
    }
    u64 channel() const {
        return channel_id;
    }
    u64 guild() const {
        return guild_id;
    }
    // PRE_SEND: the replied-to message and its author, or 0.
    u64 reply() const {
        return reply_to;
    }
    u64 reply_author() const {
        return reply_author_id;
    }
    // Replaces the text; false (nothing changes) when it's empty or longer than the host's buffer.
    bool replace(Str replacement) {
        if (replacement.bytes == 0 || replacement.bytes > buffer_capacity)
            return false;
        for (u32 i = 0; i < replacement.bytes; ++i)
            buffer[i] = replacement.data[i];
        replaced_bytes = replacement.bytes;
        verdict = MC_DRAFT_REPLACE;
        return true;
    }
    // Doesn't send; the composer keeps the text.
    void cancel() {
        verdict = MC_DRAFT_CANCEL;
    }
    // PRE_SEND: asks the user first; the text is sent unchanged when they accept.
    bool confirm(Str question) {
        if (question.bytes == 0 || question.bytes > MC_CONFIRM_TEXT_MAX || question.bytes > buffer_capacity)
            return false;
        for (u32 i = 0; i < question.bytes; ++i)
            buffer[i] = question.data[i];
        replaced_bytes = question.bytes;
        verdict = MC_DRAFT_CONFIRM;
        return true;
    }
    bool flag(u32 bit) const {
        return (flags & bit) != 0;
    }
    void set_flag(u32 bit, bool on) {
        flags = on ? flags | bit : flags & ~bit;
    }
};

// An edit of one of your messages (MC_HOOK_PRE_EDIT); flags are ignored.
struct EditDraft : Draft {
    u64 message() const {
        return message_id;
    }
};

static_assert(sizeof(Str) == sizeof(mc_str) && sizeof(StrList) == sizeof(mc_list));
static_assert(sizeof(Draft) == sizeof(mc_draft) && sizeof(EditDraft) == sizeof(mc_draft));

// ---- Context ------------------------------------------------------------------

enum class Tone : u32 {
    neutral = MC_TONE_NEUTRAL,
    accent = MC_TONE_ACCENT,
    positive = MC_TONE_POSITIVE,
    warning = MC_TONE_WARNING,
    critical = MC_TONE_CRITICAL,
    role = MC_TONE_ROLE // the author's role colour; name prefix and suffix only
};
enum class Level : u32 {
    debug = MC_LOG_DEBUG,
    info = MC_LOG_INFO,
    warning = MC_LOG_WARNING,
    error = MC_LOG_ERROR
};

// ---- Views (host-written; valid for the call) ---------------------------------

struct Message : mc_message {
    u64 id() const {
        return message_id;
    }
    u64 channel() const {
        return channel_id;
    }
    u64 guild() const {
        return guild_id;
    }
    u64 author() const {
        return author_id;
    }
    Str content() const {
        return Str(mc_message::content);
    }
    Str author_name() const {
        return Str(mc_message::author_name);
    }
    bool has(u32 flag) const {
        return (flags & flag) != 0;
    }
    bool own() const {
        return has(MC_MESSAGE_OWN);
    }
    bool bot() const {
        return has(MC_MESSAGE_BOT);
    }
};
// MESSAGE_DELETED's argument: the last copy the host has.
struct DeletedMessage : Message {};

struct User : mc_user {
    u64 id() const {
        return user_id;
    }
    u64 guild() const {
        return guild_id;
    }
    Str name() const {
        return Str(mc_user::name);
    }
    Str username() const {
        return Str(mc_user::username);
    }
    bool bot() const {
        return (flags & MC_USER_BOT) != 0;
    }
    bool self() const {
        return (flags & MC_USER_SELF) != 0;
    }
};
struct Channel : mc_channel {
    u64 id() const {
        return channel_id;
    }
    u64 guild() const {
        return guild_id;
    }
    Str name() const {
        return Str(mc_channel::name);
    }
    bool has(u32 flag) const {
        return (flags & flag) != 0;
    }
};
struct Guild : mc_guild {
    u64 id() const {
        return guild_id;
    }
    Str name() const {
        return Str(mc_guild::name);
    }
};
struct Header : mc_header {
    u64 channel() const {
        return channel_id;
    }
    u64 guild() const {
        return guild_id;
    }
    Str name() const {
        return Str(mc_header::name);
    }
    bool member_list() const {
        return where == MC_HEADER_MEMBER_LIST;
    }
};
struct Click : mc_click {
    bool double_click() const {
        return count >= 2;
    }
    bool middle() const {
        return button == MC_CLICK_MIDDLE;
    }
    bool with(u32 modifier) const {
        return (modifiers & modifier) != 0;
    }
};
struct NodeAction : mc_node_action {};
struct TypingNotice {
    u64 channel;
};
struct SettingChanged {
    u32 index; // compare with mc::Setting values
};
struct KeybindPressed {
    u32 index; // the KEYBIND setting
};
struct Timer {
    u64 id;
};

// A button press or a visibility question (MC_BUTTON).
struct ButtonEvent : mc_button_event {
    u64 channel() const {
        return channel_id;
    }
    u64 guild() const {
        return guild_id;
    }
    // MESSAGE_POPOVER buttons: the message.
    const Message& message() const {
        return *static_cast<const Message*>(mc_button_event::message);
    }
};
// What a context menu opened on (MC_MENU).
struct MenuTarget : mc_menu_target {
    bool is(u32 menu_bit) const {
        return kind == menu_bit;
    }
    Str url() const {
        return Str(mc_menu_target::url);
    }
    Str text() const {
        return Str(mc_menu_target::text);
    }
    bool own() const {
        return (flags & MC_MENU_OWN) != 0;
    }
};

// EVENTS handlers take one of these; each kind is its own type.
namespace ev {
template <u32 Kind> struct Of : mc_event {
    static constexpr u32 event_kind = Kind;
    Str text() const {
        return Str(mc_event::text);
    }
    Str name() const {
        return Str(mc_event::name);
    }
    bool has(u32 flag) const {
        return (flags & flag) != 0;
    }
};
using MessageCreate = Of<MC_EVENT_MESSAGE_CREATE>;
using MessageUpdate = Of<MC_EVENT_MESSAGE_UPDATE>;
using MessageDelete = Of<MC_EVENT_MESSAGE_DELETE>;
using Typing = Of<MC_EVENT_TYPING>;
using ReactionAdd = Of<MC_EVENT_REACTION_ADD>;
using ReactionRemove = Of<MC_EVENT_REACTION_REMOVE>;
using Presence = Of<MC_EVENT_PRESENCE>;
using VoiceState = Of<MC_EVENT_VOICE_STATE>;
using ChannelCreate = Of<MC_EVENT_CHANNEL_CREATE>;
using ChannelUpdate = Of<MC_EVENT_CHANNEL_UPDATE>;
using ChannelDelete = Of<MC_EVENT_CHANNEL_DELETE>;
using GuildCreate = Of<MC_EVENT_GUILD_CREATE>;
using GuildUpdate = Of<MC_EVENT_GUILD_UPDATE>;
using GuildDelete = Of<MC_EVENT_GUILD_DELETE>;
using MemberAdd = Of<MC_EVENT_MEMBER_ADD>;
using MemberUpdate = Of<MC_EVENT_MEMBER_UPDATE>;
using MemberRemove = Of<MC_EVENT_MEMBER_REMOVE>;
using RelationshipAdd = Of<MC_EVENT_RELATIONSHIP_ADD>;
using RelationshipRemove = Of<MC_EVENT_RELATIONSHIP_REMOVE>;
using Overflow = Of<MC_EVENT_OVERFLOW>; // value: events lost
} // namespace ev

// ---- Decorations ----------------------------------------------------------------
// The host reads a decoration after the hook returns, when the hook's locals are gone, so Decoration copies the
// text it is given (Text<N> locals are fine) into a buffer every entry point starts over.

namespace detail {
// A valid decoration's text totals about 12.3 KiB.
inline char g_kept[16384];
inline u32 g_kept_used;
inline mc_str kept(mc_str text) {
    if (!text.bytes || text.bytes > sizeof(g_kept) - g_kept_used)
        return text;
    char* at = g_kept + g_kept_used;
    for (u32 i = 0; i < text.bytes; ++i)
        at[i] = text.data[i];
    g_kept_used += text.bytes;
    return {at, text.bytes, 0};
}
inline mc_node kept(mc_node node) {
    node.text = kept(node.text);
    node.icon = kept(node.icon);
    node.tooltip = kept(node.tooltip);
    node.value = kept(node.value);
    return node;
}
} // namespace detail

struct Node : mc_node {};
inline Node label(Str text, Tone tone = Tone::neutral) {
    Node n{};
    n.kind = MC_NODE_LABEL;
    n.tone = static_cast<u32>(tone);
    n.text = text;
    return n;
}
inline Node badge(Str text, Tone tone = Tone::neutral, Str icon = {}, Str tooltip = {}) {
    Node n = label(text, tone);
    n.kind = MC_NODE_BADGE;
    n.icon = icon;
    n.tooltip = tooltip;
    return n;
}
inline Node icon(Str icon_name, Str tooltip, Tone tone = Tone::neutral) {
    Node n = badge({}, tone, icon_name, tooltip);
    n.kind = MC_NODE_ICON;
    return n;
}
// tag (non-zero) reaches the NODE_ACTION hook.
inline Node button(Str text, u64 tag, Tone tone = Tone::neutral) {
    Node n = label(text, tone);
    n.kind = MC_NODE_BUTTON;
    n.tag = tag;
    return n;
}
inline Node link(Str text, Str url) {
    Node n = label(text);
    n.kind = MC_NODE_LINK;
    n.value = url;
    return n;
}

struct Card : mc_card {
    Card& title(Str text) {
        mc_card::title = text;
        return *this;
    }
    Card& text(Str markdown) {
        mc_card::text = markdown;
        return *this;
    }
    Card& caption(Str text) {
        mc_card::caption = text;
        return *this;
    }
    Card& link(Str url) {
        mc_card::link = url;
        return *this;
    }
    Card& thumbnail(Str url) {
        mc_card::thumbnail = url;
        return *this;
    }
    Card& tone(Tone accent) {
        mc_card::tone = static_cast<u32>(accent);
        return *this;
    }
    // Up to MC_CARD_BUTTONS; tag (non-zero) reaches NODE_ACTION.
    Card& button(Str label, u64 tag, Tone tone = Tone::neutral) {
        if (button_count < MC_CARD_BUTTONS)
            buttons[button_count++] = {label, tag, static_cast<u32>(tone), 0};
        return *this;
    }
};
inline Card card() {
    return {};
}

// A decorate hook's output, zeroed by the host.
struct Decoration : mc_decoration {
    void hide() {
        visibility = MC_VIS_HIDDEN;
    }
    void collapse(Str label = {}) {
        visibility = MC_VIS_COLLAPSED;
        collapsed_label = detail::kept(label);
    }
    void blur_media() {
        media = MC_MEDIA_BLUR;
    }
    void hide_media() {
        media = MC_MEDIA_HIDE;
    }
    void prefix(Str text, Tone tone = Tone::neutral) {
        name_prefix = detail::kept(text);
        name_tone = static_cast<u32>(tone);
    }
    void suffix(Str text, Tone tone = Tone::neutral) {
        name_suffix = detail::kept(text);
        name_tone = static_cast<u32>(tone);
    }
    // Shows markdown instead of the message text (display only).
    void replace_content(Str markdown) {
        content = detail::kept(markdown);
        flags |= MC_DECO_REPLACE_CONTENT;
    }
    bool badge(const mc_node& node) {
        if (badge_count >= MC_DECORATION_BADGES)
            return false;
        badges[badge_count++] = detail::kept(node);
        return true;
    }
    bool accessory(const mc_node& node) {
        if (accessory_count >= MC_DECORATION_ACCESSORIES)
            return false;
        accessories[accessory_count++] = detail::kept(node);
        return true;
    }
    void card(const mc_card& block) {
        mc_card& c = mc_decoration::card;
        c = block;
        c.title = detail::kept(block.title);
        c.text = detail::kept(block.text);
        c.caption = detail::kept(block.caption);
        c.link = detail::kept(block.link);
        c.thumbnail = detail::kept(block.thumbnail);
        for (u32 i = 0; i < c.button_count && i < MC_CARD_BUTTONS; ++i)
            c.buttons[i].label = detail::kept(block.buttons[i].label);
        has_card = 1;
    }
};

static_assert(sizeof(Message) == sizeof(mc_message) && sizeof(Decoration) == sizeof(mc_decoration) &&
              sizeof(Card) == sizeof(mc_card) && sizeof(MenuTarget) == sizeof(mc_menu_target));

// ---- Commands -------------------------------------------------------------------

// MC_COMMAND argument types: Str (text), i64, bool, UserId, ChannelId; Opt<T> is optional.
struct UserId {
    u64 id;
};
struct ChannelId {
    u64 id;
};
template <class T> struct Opt {
    T value;
    bool present;
    explicit operator bool() const {
        return present;
    }
    const T& operator*() const {
        return value;
    }
};

namespace detail {
inline mc_command_call* g_command; // the command call in progress (UI thread)
}

// What a command does after it ran. The text is copied into the host's buffer
// when the Reply is made, so it may point at locals.
struct Reply {
    u32 kind;
    static Reply none() {
        return {MC_REPLY_NONE};
    }
    static Reply send(Str text) {
        return make(MC_REPLY_SEND, text, Tone::neutral);
    }
    static Reply insert(Str text) {
        return make(MC_REPLY_INSERT, text, Tone::neutral);
    }
    static Reply local(Str text, Tone tone = Tone::neutral) {
        return make(MC_REPLY_LOCAL, text, tone);
    }
    static Reply toast(Str text, Tone tone = Tone::neutral) {
        return make(MC_REPLY_TOAST, text, tone);
    }

  private:
    static Reply make(u32 kind, Str text, Tone tone) {
        mc_command_call* call = detail::g_command;
        u32 bytes = text.bytes < call->reply_capacity ? text.bytes : call->reply_capacity;
        while (bytes < text.bytes && bytes && (static_cast<u8>(text.data[bytes]) & 0xC0) == 0x80)
            --bytes; // cut on a character boundary
        for (u32 i = 0; i < bytes; ++i)
            call->reply[i] = text.data[i];
        call->reply_bytes = bytes;
        call->reply_tone = static_cast<u32>(tone);
        return {kind};
    }
};

// ---- Async results ----------------------------------------------------------------

struct HttpResult {
    static constexpr u32 kind = MC_COMPLETION_HTTP;
    const mc_completion* raw;
    // A 2xx response arrived.
    bool ok() const {
        return raw->error == MC_HTTP_OK && raw->status >= 200 && raw->status < 300;
    }
    u32 status() const {
        return raw->status;
    }
    u32 error() const {
        return raw->error;
    }
    Str body() const {
        return Str(static_cast<const char*>(raw->body), raw->body_bytes);
    }
};
struct ModalResult {
    static constexpr u32 kind = MC_COMPLETION_MODAL;
    const mc_completion* raw;
    bool accepted() const {
        return raw->result == 1;
    }
    Str text() const {
        return Str(raw->text);
    }
    bool toggled() const {
        return raw->toggle != 0;
    }
    u32 selected() const {
        return raw->select_index;
    }
};
struct BannerResult {
    static constexpr u32 kind = MC_COMPLETION_BANNER;
    const mc_completion* raw;
    bool action() const {
        return raw->result == MC_BANNER_ACTION;
    }
};

struct Modal : mc_modal {};
struct Banner : mc_banner {};
struct Activity : mc_activity {};
struct ButtonState : mc_button_state {};
// An HTTP header row: {"Name", value}.
struct HttpHeader {
    Str name, value;
};
static_assert(sizeof(HttpHeader) == sizeof(mc_http_header));

struct Settings; // MC_SETTINGS defines it
class Ctx;

namespace detail {
template <class A, class B> inline constexpr bool same = false;
template <class A> inline constexpr bool same<A, A> = true;
template <class T> inline constexpr bool is_array = false;
template <class T, size_t N> inline constexpr bool is_array<T[N]> = true;
template <int> struct StateSlot {
    using value_type = void;
};
// MC_PLUGIN specializes slot 0 with what the plugin declares; the Ctx calls
// that need a permission check it when they're instantiated (end of file).
template <int> struct PluginSlot {
    static constexpr u64 permissions = 0;
    static constexpr u32 hosts = 0;
    static constexpr int counter_end = 0; // __COUNTER__ at MC_PLUGIN
};
template <int D> consteval bool declared(u64 bits) {
    return (PluginSlot<D>::permissions & bits) == bits;
}
template <int D> consteval bool may_write() {
    return declared<D>(MC_PERM_ACT_MESSAGES) || declared<D>(MC_PERM_AUTOMATED_ACTIONS);
}
template <auto F, int D> consteval u32 button_index(); // defined with MC_BUTTON's registry

// Calls in flight whose result comes back through MC_HOOK_COMPLETION: the
// host's tag is a key into this table, which holds the handler and the
// caller's own tag. A reloaded DLL starts empty and drops older results.
using Resume = void (*)(Ctx&, const mc_completion&, u64 tag);
struct Pending {
    u64 key;
    u64 tag;
    Resume resume;
    u32 kind; // MC_COMPLETION_*
};
inline constexpr u32 kPendingMax = 16;
inline Pending g_pending[kPendingMax];
inline u64 g_pending_key;
// Calls start(key) with a fresh key and keeps the entry only when it returns MC_OK.
template <class Result, auto F, class Start> mc_status start_pending(u64 tag, Start start) {
    static_assert(same<decltype(F), void (*)(Ctx&, const Result&, u64)>,
                  "the result handler must be void (Ctx&, const HttpResult&/ModalResult&/BannerResult&, u64 tag)");
    for (Pending& slot : g_pending)
        if (!slot.resume) {
            slot = {++g_pending_key, tag,
                    [](Ctx& ctx, const mc_completion& done, u64 user_tag) { F(ctx, Result{&done}, user_tag); },
                    Result::kind};
            const mc_status status = start(slot.key);
            if (status != MC_OK)
                slot = {};
            return status;
        }
    return MC_E_BUSY;
}
// Forgets every pending call of kind; returns the last key, or 0.
inline u64 forget_pending(u32 kind) {
    u64 key = 0;
    for (Pending& slot : g_pending)
        if (slot.resume && slot.kind == kind) {
            key = slot.key;
            slot = {};
        }
    return key;
}
} // namespace detail

// The plugin's key-value store (persisted by the host).
struct Store {
    mc_ctx* raw;

    // Copies a value of exactly sizeof(T) bytes into out.
    template <class T>
        requires(!detail::is_array<T>)
    bool get(Str key, T& out) const {
        static_assert(__is_trivially_copyable(T), "store values must be trivially copyable");
        u32 bytes = 0;
        return raw->api->store_get(raw, key, &out, sizeof(T), &bytes) == MC_OK && bytes == sizeof(T);
    }
    template <u32 N> bool get(Str key, Text<N>& out) const {
        u32 bytes = 0;
        const bool found = raw->api->store_get(raw, key, out.data(), N, &bytes) == MC_OK;
        out.set_size(found ? bytes : 0);
        return found;
    }
    mc_status get(Str key, void* out, u32 capacity, u32* bytes) const {
        return raw->api->store_get(raw, key, out, capacity, bytes);
    }
    template <class T>
        requires(!detail::is_array<T>)
    mc_status put(Str key, const T& value) const {
        static_assert(__is_trivially_copyable(T), "store values must be trivially copyable");
        return raw->api->store_set(raw, key, &value, sizeof(T));
    }
    mc_status put(Str key, Str text) const {
        return raw->api->store_set(raw, key, text.data, text.bytes);
    }
    mc_status remove(Str key) const {
        return raw->api->store_remove(raw, key);
    }
};

// HTTPS to the hosts in MC_PLUGIN's .network. F receives the result:
// void F(Ctx&, const HttpResult&, u64 tag).
struct Http {
    mc_ctx* raw;

    template <auto F, int D = 0> mc_status get(Str url, u64 tag = 0) const {
        return request<F, D>(MC_HTTP_GET, url, {}, tag, nullptr, 0);
    }
    template <auto F, int D = 0, u32 N> mc_status get(Str url, u64 tag, const HttpHeader (&headers)[N]) const {
        return request<F, D>(MC_HTTP_GET, url, {}, tag, headers, N);
    }
    template <auto F, int D = 0> mc_status post(Str url, Str body, u64 tag = 0) const {
        return request<F, D>(MC_HTTP_POST, url, body, tag, nullptr, 0);
    }
    template <auto F, int D = 0, u32 N>
    mc_status post(Str url, Str body, u64 tag, const HttpHeader (&headers)[N]) const {
        return request<F, D>(MC_HTTP_POST, url, body, tag, headers, N);
    }

  private:
    template <auto F, int D>
    mc_status request(u32 method, Str url, Str body, u64 tag, const HttpHeader* headers, u32 count) const {
        static_assert(detail::PluginSlot<D>::hosts != 0, "ctx.http needs .network = {\"host\", ...} in MC_PLUGIN");
        mc_http_request request{};
        request.method = method;
        request.url = url;
        request.headers = reinterpret_cast<const mc_http_header*>(headers);
        request.header_count = count;
        request.body = body.data;
        request.body_bytes = body.bytes;
        mc_ctx* ctx = raw;
        return detail::start_pending<HttpResult, F>(tag, [&](u64 key) { return ctx->api->http(ctx, &request, key); });
    }
};

// The composer of the open channel (MC_PERM_COMPOSER).
struct Composer {
    mc_ctx* raw;

    template <u32 N, int D = 0> bool text(Text<N>& out) const {
        static_assert(detail::declared<D>(MC_PERM_COMPOSER), "ctx.composer needs .permissions = MC_PERM_COMPOSER");
        u32 bytes = 0;
        const bool found = raw->api->composer_text(raw, out.data(), N, &bytes) == MC_OK;
        out.set_size(found ? bytes : 0);
        return found;
    }
    template <int D = 0> mc_status insert(Str text) const {
        static_assert(detail::declared<D>(MC_PERM_COMPOSER), "ctx.composer needs .permissions = MC_PERM_COMPOSER");
        return raw->api->composer_insert(raw, text);
    }
    template <int D = 0> mc_status replace(Str text) const {
        static_assert(detail::declared<D>(MC_PERM_COMPOSER), "ctx.composer needs .permissions = MC_PERM_COMPOSER");
        return raw->api->composer_replace(raw, text);
    }
    template <int D = 0> mc_status focus() const {
        static_assert(detail::declared<D>(MC_PERM_COMPOSER), "ctx.composer needs .permissions = MC_PERM_COMPOSER");
        return raw->api->composer_focus(raw);
    }
    template <int D = 0> mc_status reply(u64 channel, u64 message, bool ping = true) const {
        static_assert(detail::declared<D>(MC_PERM_COMPOSER), "ctx.composer needs .permissions = MC_PERM_COMPOSER");
        return raw->api->composer_reply(raw, channel, message, ping ? 1u : 0u);
    }
    template <int D = 0> mc_status clear_reply(u64 channel) const {
        static_assert(detail::declared<D>(MC_PERM_COMPOSER), "ctx.composer needs .permissions = MC_PERM_COMPOSER");
        return raw->api->composer_reply(raw, channel, 0, 0);
    }
    template <int D = 0> mc_status edit(u64 channel, u64 message) const {
        static_assert(detail::declared<D>(MC_PERM_COMPOSER), "ctx.composer needs .permissions = MC_PERM_COMPOSER");
        return raw->api->composer_edit(raw, channel, message);
    }
};

// A copy out of MaiCord's cache; valid until the handler returns.
template <class T> struct Found {
    T value;
    bool ok;
    explicit operator bool() const {
        return ok;
    }
    const T* operator->() const {
        return &value;
    }
    const T& operator*() const {
        return value;
    }
};

// Cache reads; they never fetch.
struct Reader {
    mc_ctx* raw;

    // guild: the guild whose nickname and roles apply, or 0.
    Found<User> user(u64 id, u64 guild = 0) const {
        Found<User> out{};
        out.ok = raw->api->read_user(raw, id, guild, &out.value) == MC_OK;
        return out;
    }
    Found<Channel> channel(u64 id) const {
        Found<Channel> out{};
        out.ok = raw->api->read_channel(raw, id, &out.value) == MC_OK;
        return out;
    }
    Found<Guild> guild(u64 id) const {
        Found<Guild> out{};
        out.ok = raw->api->read_guild(raw, id, &out.value) == MC_OK;
        return out;
    }
    // Discord permission bits of user in channel (0: guild-wide); 0 when unknown.
    u64 permissions(u64 guild, u64 channel, u64 user) const {
        u64 bits = 0;
        return raw->api->read_permissions(raw, guild, channel, user, &bits) == MC_OK ? bits : 0;
    }
    // The voice channel user is in (guild 0: a private call first, then any server); 0 when none.
    u64 voice_channel(u64 user, u64 guild = 0) const {
        u64 channel = 0;
        return raw->api->read_voice_channel(raw, user, guild, &channel) == MC_OK ? channel : 0;
    }
    // The group DMs you share with user, most recently active first; returns how many were written,
    // total (optional) how many there are.
    template <u32 N, int D = 0> u32 shared_group_dms(u64 user, u64 (&out)[N], u32* total = nullptr) const {
        static_assert(detail::declared<D>(MC_PERM_READ_MESSAGES),
                      "ctx.read.shared_group_dms needs .permissions = MC_PERM_READ_MESSAGES");
        u32 count = 0;
        if (raw->api->read_shared_group_dms(raw, user, out, N, &count) != MC_OK)
            count = 0;
        if (total)
            *total = count;
        return count < N ? count : N;
    }
    // The open channel with the composer's reply and edit targets.
    Found<mc_open_channel> open_channel() const {
        Found<mc_open_channel> out{};
        out.ok = raw->api->read_open_channel(raw, &out.value) == MC_OK;
        return out;
    }
    // Loaded messages of channel, newest first, starting below before (0: the newest); how many were written.
    template <u32 N, int D = 0> u32 messages(u64 channel, u64 before, mc_message_ref (&out)[N]) const {
        static_assert(N <= MC_READ_MESSAGES_MAX, "ctx.read.messages: at most MC_READ_MESSAGES_MAX at a time");
        static_assert(detail::declared<D>(MC_PERM_READ_MESSAGES),
                      "ctx.read.messages needs .permissions = MC_PERM_READ_MESSAGES");
        u32 count = 0;
        return raw->api->read_messages(raw, channel, before, out, N, &count) == MC_OK ? count : 0;
    }
};

// Messages the host kept after MESSAGE_DELETED or MESSAGE_EDITED said keep.
struct Kept {
    mc_ctx* raw;

    Found<mc_kept_info> info(u64 channel, u64 message) const {
        Found<mc_kept_info> out{};
        out.ok = raw->api->keep_info(raw, channel, message, &out.value) == MC_OK;
        return out;
    }
    // version 0: the latest text; 1..edit_count: older ones, 1 the most recent.
    template <u32 N> bool text(u64 channel, u64 message, u32 version, Text<N>& out) const {
        u32 bytes = 0;
        const bool found = raw->api->keep_text(raw, channel, message, version, out.data(), N, &bytes) == MC_OK;
        out.set_size(found ? bytes : 0);
        return found;
    }
    template <int D = 0> mc_status restore(const mc_kept* items, u32 count) const {
        static_assert(detail::declared<D>(MC_PERM_PERSISTENT_LOG),
                      "ctx.kept.restore needs .permissions = MC_PERM_PERSISTENT_LOG");
        return raw->api->keep_restore(raw, items, count);
    }
};

// Passed to every handler; valid until the handler returns.
class Ctx {
  public:
    // Every entry point makes one: the decoration text buffer starts over.
    explicit Ctx(mc_ctx* raw)
        : store{raw}, http{raw}, composer{raw}, read{raw}, kept{raw}, raw_(raw) {
        detail::g_kept_used = 0;
    }

    Store store;
    Http http;
    Composer composer;
    Reader read;
    Kept kept;

    mc_ctx* raw() const {
        return raw_;
    }
    // The values from MC_SETTINGS; plain loads.
    const Settings& settings() const {
        return *static_cast<const Settings*>(raw_->settings);
    }
    // The MC_STATE object, zeroed at first start.
    template <class T> T& state() const {
        static_assert(detail::same<T, typename detail::StateSlot<sizeof(T) * 0>::value_type>,
                      "ctx.state<T>(): T must be the type named by MC_STATE");
        return *static_cast<T*>(raw_->state);
    }
    bool gesture() const {
        return (raw_->flags & MC_CTX_GESTURE) != 0;
    }
    bool reloaded() const {
        return (raw_->flags & MC_CTX_RELOADED) != 0;
    }
    bool first_start() const {
        return (raw_->flags & MC_CTX_FIRST_START) != 0;
    }

    mc_status toast(Str text, Tone tone = Tone::neutral) const {
        return raw_->api->toast(raw_, text, static_cast<u32>(tone), 0);
    }
    void log(Str text) const {
        raw_->api->log(raw_, MC_LOG_INFO, text);
    }
    void log(Level level, Str text) const {
        raw_->api->log(raw_, static_cast<u32>(level), text);
    }

    // Changes one of the plugin's settings (mc::Setting::<field>); visible from the next call.
    template <class Id> mc_status set_setting(Id id, bool value) const {
        const u8 byte = value ? 1 : 0;
        return raw_->api->set_setting(raw_, static_cast<u32>(id), &byte, 1);
    }
    template <class Id> mc_status set_setting(Id id, i32 value) const {
        return raw_->api->set_setting(raw_, static_cast<u32>(id), &value, sizeof(value));
    }
    template <class Id> mc_status set_setting(Id id, u32 value) const {
        return raw_->api->set_setting(raw_, static_cast<u32>(id), &value, sizeof(value));
    }
    template <class Id> mc_status set_setting(Id id, u64 value) const {
        return raw_->api->set_setting(raw_, static_cast<u32>(id), &value, sizeof(value));
    }
    template <class Id> mc_status set_setting(Id id, Str value) const {
        return raw_->api->set_setting(raw_, static_cast<u32>(id), value.data, value.bytes);
    }

    // -- Decorations and buttons --
    // The decorate hook is asked again for these at the next pass; id 0 = all.
    mc_status invalidate_message(u64 id) const {
        return raw_->api->invalidate(raw_, MC_INVALIDATE_MESSAGE, id);
    }
    mc_status invalidate_user(u64 id) const {
        return raw_->api->invalidate(raw_, MC_INVALIDATE_USER, id);
    }
    mc_status invalidate_channel(u64 id) const {
        return raw_->api->invalidate(raw_, MC_INVALIDATE_CHANNEL, id);
    }
    mc_status invalidate_guild(u64 id) const {
        return raw_->api->invalidate(raw_, MC_INVALIDATE_GUILD, id);
    }
    mc_status invalidate_headers() const {
        return raw_->api->invalidate(raw_, MC_INVALIDATE_HEADER, 0);
    }
    mc_status invalidate_buttons() const {
        return raw_->api->invalidate(raw_, MC_INVALIDATE_BUTTONS, 0);
    }
    // Decorations set without a pull hook; null clears.
    mc_status push_message(u64 channel, u64 message, const Decoration* d) const {
        return raw_->api->decorate_push(raw_, MC_HOOK_DECORATE_MESSAGE, message, channel, d);
    }
    // Covers every surface the user appears on.
    mc_status push_user(u64 user, const Decoration* d) const {
        return raw_->api->decorate_push(raw_, MC_HOOK_DECORATE_USER, user, 0, d);
    }
    mc_status push_channel(u64 channel, const Decoration* d) const {
        return raw_->api->decorate_push(raw_, MC_HOOK_DECORATE_CHANNEL, channel, 0, d);
    }
    mc_status push_guild(u64 guild, const Decoration* d) const {
        return raw_->api->decorate_push(raw_, MC_HOOK_DECORATE_GUILD, guild, 0, d);
    }
    // where: MC_HEADER_CHANNEL, MC_HEADER_MEMBER_LIST, or 0 for both.
    mc_status push_header(u32 where, const Decoration* d) const {
        return raw_->api->decorate_push(raw_, MC_HOOK_DECORATE_HEADER, where, 0, d);
    }
    // The state of the button MC_BUTTON(F, ...) declared.
    template <auto F, int D = 0> mc_status button(const ButtonState& state) const {
        return raw_->api->button_state(raw_, detail::button_index<F, D>(), &state);
    }

    // -- Feedback and navigation --
    // Shows the plugin's banner, replacing the previous one. The banner's tag
    // reaches F: void (Ctx&, const BannerResult&, u64 tag).
    template <auto F> mc_status banner(const Banner& b) const {
        detail::forget_pending(MC_COMPLETION_BANNER);
        mc_ctx* ctx = raw_;
        return detail::start_pending<BannerResult, F>(b.tag, [&](u64 key) {
            Banner keyed = b;
            keyed.tag = key;
            return ctx->api->banner(ctx, &keyed);
        });
    }
    // A banner whose button needs no handler.
    mc_status banner(const Banner& b) const {
        detail::forget_pending(MC_COMPLETION_BANNER);
        Banner untagged = b;
        untagged.tag = 0;
        return raw_->api->banner(raw_, &untagged);
    }
    mc_status clear_banner() const {
        return raw_->api->banner_clear(raw_, detail::forget_pending(MC_COMPLETION_BANNER));
    }
    // In a gesture. F: void (Ctx&, const ModalResult&, u64 tag).
    template <auto F> mc_status modal(const Modal& m, u64 tag = 0) const {
        mc_ctx* ctx = raw_;
        return detail::start_pending<ModalResult, F>(tag, [&](u64 key) { return ctx->api->modal(ctx, &m, key); });
    }
    // In a gesture: a modal whose answer needs no handler, such as an information dialog (MC_MODAL_INFO).
    mc_status modal(const Modal& m) const {
        return raw_->api->modal(raw_, &m, 0);
    }
    mc_status open_url(Str url) const {
        return raw_->api->open_url(raw_, url);
    }
    template <int D = 0> mc_status copy(Str text) const {
        static_assert(detail::declared<D>(MC_PERM_CLIPBOARD), "ctx.copy needs .permissions = MC_PERM_CLIPBOARD");
        return raw_->api->copy_text(raw_, text);
    }
    mc_status open_channel(u64 channel) const {
        return raw_->api->open(raw_, MC_OPEN_CHANNEL, channel, 0);
    }
    mc_status open_profile(u64 user, u64 guild = 0) const {
        return raw_->api->open(raw_, MC_OPEN_PROFILE, user, guild);
    }

    // -- Local messages --
    // A message only you see in the open channel; its id, or 0.
    u64 local_message(Str text, u64 channel = 0) const {
        u64 id = 0;
        return raw_->api->local_message(raw_, channel, text, &id) == MC_OK ? id : 0;
    }
    mc_status remove_local_message(u64 channel, u64 id) const {
        return raw_->api->local_message_remove(raw_, channel, id);
    }

    // -- Actions: a gesture, or MC_PERM_AUTOMATED_ACTIONS --
    template <int D = 0> mc_status send(u64 channel, Str text, u32 flags = 0, u64 reply_to = 0) const {
        static_assert(detail::may_write<D>(), "ctx.send needs .permissions = MC_PERM_ACT_MESSAGES");
        const mc_send request{channel, reply_to, text, flags, 0};
        return raw_->api->send_message(raw_, &request);
    }
    template <int D = 0> mc_status edit_message(u64 channel, u64 message, Str text) const {
        static_assert(detail::may_write<D>(), "ctx.edit_message needs .permissions = MC_PERM_ACT_MESSAGES");
        return raw_->api->edit_message(raw_, channel, message, text);
    }
    // emoji: Unicode, or name:id.
    template <int D = 0> mc_status react(u64 channel, u64 message, Str emoji, bool remove = false) const {
        static_assert(detail::may_write<D>(), "ctx.react needs .permissions = MC_PERM_ACT_MESSAGES");
        return raw_->api->react(raw_, channel, message, emoji, remove ? MC_REACT_REMOVE : 0u);
    }
    template <int D = 0> mc_status delete_message(u64 channel, u64 message) const {
        static_assert(detail::may_write<D>(), "ctx.delete_message needs .permissions = MC_PERM_ACT_MESSAGES");
        return raw_->api->delete_own(raw_, channel, message);
    }
    // scope: MC_READ_CHANNEL, MC_READ_GUILD or MC_READ_ALL (id 0).
    template <int D = 0> mc_status mark_read(u32 scope, u64 id) const {
        static_assert(detail::may_write<D>(), "ctx.mark_read needs .permissions = MC_PERM_ACT_MESSAGES");
        return raw_->api->mark_read(raw_, scope, id);
    }
    // null clears.
    template <int D = 0> mc_status set_activity(const Activity* activity) const {
        static_assert(detail::declared<D>(MC_PERM_ACT_PRESENCE),
                      "ctx.set_activity needs .permissions = MC_PERM_ACT_PRESENCE");
        return raw_->api->set_activity(raw_, activity);
    }

    // -- Timers: MC_HOOK_TIMER gets Timer{id} --
    mc_status timer(u64 id, u32 interval_ms, bool repeat = false) const {
        return raw_->api->timer(raw_, id, interval_ms, repeat ? 1u : 0u);
    }
    mc_status cancel_timer(u64 id) const {
        return raw_->api->cancel_timer(raw_, id);
    }

    // -- Time --
    // Unix milliseconds (UTC); 0 on failure.
    u64 now_ms() const {
        u64 ms = 0;
        return raw_->api->clock(raw_, &ms) == MC_OK ? ms : 0;
    }
    // The user's offset from UTC in seconds (east positive, daylight saving included) at unix_seconds.
    i32 local_offset(i64 unix_seconds) const {
        i32 seconds = 0;
        return raw_->api->local_offset(raw_, unix_seconds, &seconds) == MC_OK ? seconds : 0;
    }

  private:
    mc_ctx* raw_;
};

// ---- URLs ---------------------------------------------------------------------

namespace url {

// Common tracking parameters; a trailing '*' matches by prefix.
inline constexpr mc_str kTrackerItems[] = {
    MC_S("utm_*"),   MC_S("fbclid"),     MC_S("gclid"),   MC_S("gclsrc"),      MC_S("dclid"),
    MC_S("gbraid"),  MC_S("wbraid"),     MC_S("msclkid"), MC_S("yclid"),       MC_S("twclid"),
    MC_S("ttclid"),  MC_S("li_fat_id"),  MC_S("igshid"),  MC_S("mc_cid"),      MC_S("mc_eid"),
    MC_S("_hsenc"),  MC_S("_hsmi"),      MC_S("mkt_tok"), MC_S("oly_anon_id"), MC_S("oly_enc_id"),
    MC_S("vero_id"), MC_S("rb_clickid"), MC_S("s_cid"),   MC_S("srsltid"),     MC_S("_gl")};
inline constexpr StrList kTrackers{{kTrackerItems, sizeof(kTrackerItems) / sizeof(kTrackerItems[0]), 0}};

// key equals pattern, or starts with it when the pattern ends in '*'; ASCII
// case-insensitive, pattern trimmed. Empty patterns and a lone '*' match nothing.
constexpr bool key_matches(Str key, Str pattern) {
    pattern = pattern.trimmed();
    if (pattern.bytes != 0 && pattern.data[pattern.bytes - 1] == '*') {
        pattern = pattern.sub(0, pattern.bytes - 1);
        return pattern.bytes != 0 && key.bytes >= pattern.bytes &&
               key.sub(0, pattern.bytes).equals_ascii_nocase(pattern);
    }
    return pattern.bytes != 0 && key.equals_ascii_nocase(pattern);
}

namespace detail {
struct Writer {
    char* out;
    u32 capacity;
    u32 size;
    bool overflow;
    void put(Str text) {
        if (text.bytes > capacity - size) {
            overflow = true;
            return;
        }
        for (u32 i = 0; i < text.bytes; ++i)
            out[size + i] = text.data[i];
        size += text.bytes;
    }
};
inline bool any_matches(Str key, const StrList& a, const StrList& b) {
    for (Str pattern : a)
        if (key_matches(key, pattern))
            return true;
    for (Str pattern : b)
        if (key_matches(key, pattern))
            return true;
    return false;
}
// 7 or 8 when an http:// or https:// scheme starts at text[at], else 0.
inline u32 scheme_length(Str text, u32 at) {
    if (text.sub(at, 7).equals_ascii_nocase("http://"))
        return 7;
    if (text.sub(at, 8).equals_ascii_nocase("https://"))
        return 8;
    return 0;
}
// Where the link starting at text[at] ends: at whitespace or <>"`, without
// trailing punctuation and markdown markers, or ')'s that close nothing.
inline u32 link_end(Str text, u32 at) {
    u32 end = at;
    i32 open = 0; // '(' minus ')' in text[at, end)
    while (end < text.bytes) {
        const char c = text.data[end];
        if (static_cast<u8>(c) <= 0x20 || c == '<' || c == '>' || c == '"' || c == '`')
            break;
        open += c == '(' ? 1 : c == ')' ? -1 : 0;
        ++end;
    }
    for (;;) {
        const char c = end > at ? text.data[end - 1] : '\0';
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '\'' || c == '|' ||
            c == '*' || c == '_' || c == '~') {
            --end;
        } else if (c == ')' && open < 0) {
            ++open;
            --end;
        } else {
            return end;
        }
    }
}
} // namespace detail

// Writes text with every query parameter matching a pattern in a or b removed
// from its http(s) links. Returns the bytes written, or 0 when nothing was
// removed or the result doesn't fit in capacity.
inline u32 strip_params(Str text, const StrList& a, const StrList& b, char* out, u32 capacity) {
    detail::Writer writer{out, capacity, 0, false};
    u32 copied = 0; // text[copied, at) is still to be written
    u32 at = 0;
    while (at < text.bytes) {
        const u32 scheme = detail::scheme_length(text, at);
        if (scheme == 0) {
            ++at;
            continue;
        }
        const u32 end = detail::link_end(text, at + scheme);
        const Str link = text.sub(at, end - at);
        const u32 fragment = link.find('#');
        const u32 query = link.sub(0, fragment).find('?');
        bool strip = false;
        for (u32 from = query + 1; from < fragment && !strip;) {
            const u32 to = link.sub(0, fragment).find('&', from);
            const Str param = link.sub(from, to - from);
            strip = detail::any_matches(param.sub(0, param.find('=')), a, b);
            from = to + 1;
        }
        if (strip) {
            writer.put(text.sub(copied, at + query - copied));
            char separator = '?';
            for (u32 from = query + 1; from < fragment;) {
                const u32 to = link.sub(0, fragment).find('&', from);
                const Str param = link.sub(from, to - from);
                if (!param.empty() && !detail::any_matches(param.sub(0, param.find('=')), a, b)) {
                    writer.put(Str(&separator, 1));
                    writer.put(param);
                    separator = '&';
                }
                from = to + 1;
            }
            writer.put(link.sub(fragment));
            copied = end;
        }
        at = end;
    }
    if (copied == 0)
        return 0;
    writer.put(text.sub(copied));
    return writer.overflow ? 0 : writer.size;
}
// strip_params into out; true when parameters were removed and the result fit.
template <u32 N> bool strip_params(Str text, const StrList& a, const StrList& b, Text<N>& out) {
    const u32 bytes = strip_params(text, a, b, out.data(), N);
    out.set_size(bytes);
    return bytes != 0;
}
template <u32 N> bool strip_params(Str text, const StrList& patterns, Text<N>& out) {
    return strip_params(text, patterns, StrList{}, out);
}

// Appends text percent-encoded for a URL query value or a form body: every byte
// but A-Z a-z 0-9 - . _ ~ becomes %XX. False (out unchanged) when it doesn't fit.
template <u32 N> bool encode(Str text, Text<N>& out) {
    const u32 before = out.size();
    constexpr char kHex[] = "0123456789ABCDEF";
    for (char c : text) {
        const u8 b = static_cast<u8>(c);
        const bool plain = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') ||
                           b == '-' || b == '.' || b == '_' || b == '~';
        const char escaped[3] = {'%', kHex[b >> 4], kHex[b & 15]};
        if (!(plain ? out.push(c) : out.append(Str(escaped, 3)))) {
            out.set_size(before);
            return false;
        }
    }
    return true;
}

} // namespace url

// ---- JSON (reading HTTP responses) ----------------------------------------------

namespace json {

constexpr bool space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
inline void skip_space(Str body, u32& at) {
    while (at < body.bytes && space(body.data[at]))
        ++at;
}

namespace detail {
inline i32 hex_digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
inline bool hex4(Str body, u32 at, u32& value) {
    if (at + 4 > body.bytes)
        return false;
    value = 0;
    for (u32 i = 0; i < 4; ++i) {
        const i32 digit = hex_digit(body.data[at + i]);
        if (digit < 0)
            return false;
        value = value << 4 | static_cast<u32>(digit);
    }
    return true;
}
// Reads the string whose opening quote is body[at] into out (null: skip it),
// unescaping \uXXXX (surrogate pairs too) to UTF-8; at ends past the closing quote.
inline bool read_string(Str body, u32& at, char* out, u32 capacity, u32* bytes) {
    if (at >= body.bytes || body.data[at] != '"')
        return false;
    u32 size = 0;
    const auto put = [&](u32 code) {
        char encoded[4];
        u32 length = 0;
        if (code < 0x80) {
            encoded[length++] = static_cast<char>(code);
        } else if (code < 0x800) {
            encoded[length++] = static_cast<char>(0xC0 | code >> 6);
            encoded[length++] = static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            encoded[length++] = static_cast<char>(0xE0 | code >> 12);
            encoded[length++] = static_cast<char>(0x80 | (code >> 6 & 0x3F));
            encoded[length++] = static_cast<char>(0x80 | (code & 0x3F));
        } else {
            encoded[length++] = static_cast<char>(0xF0 | code >> 18);
            encoded[length++] = static_cast<char>(0x80 | (code >> 12 & 0x3F));
            encoded[length++] = static_cast<char>(0x80 | (code >> 6 & 0x3F));
            encoded[length++] = static_cast<char>(0x80 | (code & 0x3F));
        }
        if (!out)
            return true;
        if (length > capacity - size)
            return false;
        for (u32 i = 0; i < length; ++i)
            out[size++] = encoded[i];
        return true;
    };
    for (++at; at < body.bytes;) {
        const char c = body.data[at++];
        if (c == '"') {
            if (bytes)
                *bytes = size;
            return true;
        }
        if (c != '\\') {
            if (out && size == capacity)
                return false;
            if (out)
                out[size++] = c;
            continue;
        }
        if (at >= body.bytes)
            return false;
        const char e = body.data[at++];
        u32 code = 0;
        switch (e) {
        case 'n': code = '\n'; break;
        case 't': code = '\t'; break;
        case 'r': code = '\r'; break;
        case 'b': code = '\b'; break;
        case 'f': code = '\f'; break;
        case '"': case '\\': case '/': code = static_cast<u8>(e); break;
        case 'u': {
            if (!hex4(body, at, code))
                return false;
            at += 4;
            u32 low = 0;
            if (code >= 0xD800 && code < 0xDC00 && at + 6 <= body.bytes && body.data[at] == '\\' &&
                body.data[at + 1] == 'u' && hex4(body, at + 2, low) && low >= 0xDC00 && low < 0xE000) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                at += 6;
            } else if (code >= 0xD800 && code < 0xE000) {
                code = 0xFFFD; // a lone surrogate
            }
            break;
        }
        default: return false;
        }
        if (!put(code))
            return false;
    }
    return false;
}
} // namespace detail

// Reads the string at body[at] (an opening quote) into out; at ends past it.
template <u32 N> bool read_string(Str body, u32& at, Text<N>& out) {
    u32 bytes = 0;
    const bool read = detail::read_string(body, at, out.data(), N, &bytes);
    out.set_size(read ? bytes : 0);
    return read;
}

// Skips the value at body[at] (after spaces): a string, number, literal, array or object.
inline bool skip(Str body, u32& at) {
    skip_space(body, at);
    u32 depth = 0;
    while (at < body.bytes) {
        const char c = body.data[at];
        if (c == '"') {
            if (!detail::read_string(body, at, nullptr, 0, nullptr))
                return false;
        } else if (c == '[' || c == '{') {
            ++depth;
            ++at;
            continue;
        } else if (c == ']' || c == '}') {
            if (!depth)
                return true; // the end of the enclosing container
            --depth;
            ++at;
        } else if (c == ',' || space(c)) {
            if (!depth)
                return true;
            ++at;
            continue;
        } else {
            ++at; // number or literal
            continue;
        }
        if (!depth)
            return true;
    }
    return depth == 0;
}

// The first string value of "key" in body, at any depth.
template <u32 N> bool find(Str body, Str key, Text<N>& out) {
    u32 at = 0;
    while (at < body.bytes) {
        if (body.data[at] != '"') {
            ++at;
            continue;
        }
        const u32 start = at + 1;
        if (!detail::read_string(body, at, nullptr, 0, nullptr))
            return false;
        if (Str(body.data + start, at - 1 - start) != key)
            continue;
        skip_space(body, at);
        if (at >= body.bytes || body.data[at] != ':')
            continue;
        ++at;
        skip_space(body, at);
        if (at < body.bytes && body.data[at] == '"')
            return read_string(body, at, out);
    }
    return false;
}

} // namespace json

// ---- Settings types (MC_SETTINGS rows) ----------------------------------------

// A list of ids (UserList settings); layout-identical to mc_ids.
struct IdList : mc_ids {
    u32 size() const {
        return count;
    }
    const u64* begin() const {
        return items;
    }
    const u64* end() const {
        return items + count;
    }
    bool contains(u64 id) const {
        for (u64 item : *this)
            if (item == id)
                return true;
        return false;
    }
};
static_assert(sizeof(IdList) == sizeof(mc_ids));

// .visible_if = Setting::field (an enumerator MC_SETTINGS defines).
struct SettingRef {
    u16 index1 = 0; // 1-based
    constexpr SettingRef() = default;
    template <class E>
        requires(__is_enum(E))
    constexpr SettingRef(E setting) : index1(static_cast<u16>(static_cast<u32>(setting) + 1)) {
    }
};
struct NoDefault {}; // the `{}` default of rows that have none

namespace detail {
// Calling one of these during constant evaluation is a compile error that names the rule.
void error_setting_key_must_be_1_to_32_bytes_of_a_z_0_9_or_underscore();
void error_setting_label_must_be_1_to_64_bytes();
void error_setting_text_too_long();
void error_setting_unknown_flags();
void error_setting_default_out_of_range();
void error_setting_too_many_items();
void error_plugin_owner_must_be_nonzero();
void error_plugin_name_must_be_1_to_64_bytes();
void error_plugin_version_must_be_1_to_32_bytes();
void error_plugin_text_too_long();
void error_plugin_api_level_unsupported();
void error_plugin_unknown_permissions();
void error_plugin_too_many_network_hosts();

// The fields every row has: description, flags, visible_if, visible_value.
template <const auto& O> consteval mc_setting_def setting_def(u32 type, Str key, Str label, u32 offset) {
    bool key_ok = key.bytes >= 1 && key.bytes <= MC_SETTING_KEY_MAX;
    for (char c : key)
        key_ok = key_ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
    if (!key_ok)
        error_setting_key_must_be_1_to_32_bytes_of_a_z_0_9_or_underscore();
    if (label.bytes == 0 || label.bytes > MC_SETTING_LABEL_MAX)
        error_setting_label_must_be_1_to_64_bytes();
    if (O.description.bytes > MC_SETTING_TEXT_MAX)
        error_setting_text_too_long();
    if (O.flags & ~(MC_SET_HIDDEN | MC_SET_DANGER | MC_SET_MULTILINE))
        error_setting_unknown_flags();
    mc_setting_def def{};
    def.key = key;
    def.label = label;
    def.description = O.description;
    def.type = type;
    def.offset = type == MC_SET_HEADER ? 0 : offset;
    def.flags = O.flags;
    def.visible_if = O.visible_if.index1;
    def.visible_value = O.visible_value;
    return def;
}
consteval u32 count_items(const Str* list) {
    u32 count = 0;
    while (count < MC_SETTING_ITEM_MAX && list[count].data != nullptr)
        ++count;
    return count;
}
template <u32 N> struct StrArray {
    mc_str items[N];
};
// The default items or choice labels of a settings row, as an mc_str array.
template <const auto& Options> struct RowItems {
    static constexpr u32 count = count_items(Options.list());
    static constexpr StrArray<count ? count : 1> array = [] {
        StrArray<count ? count : 1> out{};
        for (u32 i = 0; i < count; ++i)
            out.items[i] = Options.list()[i];
        return out;
    }();
    static constexpr const mc_str* items = count ? array.items : nullptr;
};
template <const auto& Options> consteval void put_text_fields(mc_setting_def& def) {
    if (Options.hint.bytes > MC_SETTING_LABEL_MAX || Options.max_bytes > MC_SETTING_TEXT_MAX)
        error_setting_text_too_long();
    def.hint = Options.hint;
    def.max_bytes = Options.max_bytes;
}
} // namespace detail

namespace setting {
// Options follow this order: value, the row's own fields, description, hint,
// max_bytes, flags, visible_if, visible_value.
#define MC_DETAIL_ROW_TAIL                                                                                 \
    u32 flags = 0;                                                                                         \
    SettingRef visible_if = {};                                                                            \
    u32 visible_value = 0
#define MC_DETAIL_ROW_COMMON                                                                               \
    Str description = {};                                                                                  \
    MC_DETAIL_ROW_TAIL
#define MC_DETAIL_ROW_TEXT                                                                                 \
    Str description = {};                                                                                  \
    Str hint = {};                                                                                         \
    u16 max_bytes = 0;                                                                                     \
    MC_DETAIL_ROW_TAIL

struct Toggle {
    using value_type = bool;
    struct Options {
        bool value = false;
        MC_DETAIL_ROW_COMMON;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        mc_setting_def def = detail::setting_def<O>(MC_SET_TOGGLE, key, label, offset);
        def.default_int = O.value ? 1 : 0;
        return def;
    }
};
struct Int {
    using value_type = i32;
    struct Options {
        i32 value = 0;
        i32 min = -2147483647 - 1;
        i32 max = 2147483647;
        MC_DETAIL_ROW_COMMON;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        if (O.min > O.max || O.value < O.min || O.value > O.max)
            detail::error_setting_default_out_of_range();
        mc_setting_def def = detail::setting_def<O>(MC_SET_INT, key, label, offset);
        def.minimum = O.min;
        def.maximum = O.max;
        def.default_int = O.value;
        return def;
    }
};
struct Slider {
    using value_type = i32;
    struct Options {
        i32 value = 0;
        i32 min = 0;
        i32 max = 100;
        i32 step = 1;
        MC_DETAIL_ROW_COMMON;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        if (O.min >= O.max || O.step <= 0 || O.value < O.min || O.value > O.max)
            detail::error_setting_default_out_of_range();
        mc_setting_def def = detail::setting_def<O>(MC_SET_SLIDER, key, label, offset);
        def.minimum = O.min;
        def.maximum = O.max;
        def.step = O.step;
        def.default_int = O.value;
        return def;
    }
};
struct Choice {
    using value_type = u32;
    struct Options {
        u32 value = 0;
        Str items[MC_SETTING_ITEM_MAX] = {};
        MC_DETAIL_ROW_COMMON;
        constexpr const Str* list() const {
            return items;
        }
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        using Items = detail::RowItems<O>;
        if (Items::count == 0 || O.value >= Items::count)
            detail::error_setting_default_out_of_range();
        for (u32 i = 0; i < Items::count; ++i)
            if (O.items[i].bytes == 0 || O.items[i].bytes > MC_SETTING_LABEL_MAX)
                detail::error_setting_label_must_be_1_to_64_bytes();
        mc_setting_def def = detail::setting_def<O>(MC_SET_CHOICE, key, label, offset);
        def.items = Items::items;
        def.item_count = Items::count;
        def.default_int = static_cast<i32>(O.value);
        return def;
    }
};
struct Text {
    using value_type = Str;
    struct Options {
        Str value = {};
        MC_DETAIL_ROW_TEXT;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        if (O.value.bytes > (O.max_bytes ? O.max_bytes : MC_SETTING_TEXT_MAX))
            detail::error_setting_text_too_long();
        mc_setting_def def = detail::setting_def<O>(MC_SET_TEXT, key, label, offset);
        detail::put_text_fields<O>(def);
        def.default_text = O.value;
        return def;
    }
};
// Stored sealed by the host and shown masked; it reads like Text.
struct Secret {
    using value_type = Str;
    struct Options {
        NoDefault value = {};
        MC_DETAIL_ROW_TEXT;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        if (O.flags & MC_SET_MULTILINE)
            detail::error_setting_unknown_flags();
        mc_setting_def def = detail::setting_def<O>(MC_SET_SECRET, key, label, offset);
        detail::put_text_fields<O>(def);
        return def;
    }
};
struct TextList {
    using value_type = StrList;
    struct Options {
        Str value[MC_SETTING_ITEM_MAX] = {};
        MC_DETAIL_ROW_TEXT;
        constexpr const Str* list() const {
            return value;
        }
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        mc_setting_def def = detail::setting_def<O>(MC_SET_TEXT_LIST, key, label, offset);
        detail::put_text_fields<O>(def);
        u32 total = 0;
        for (u32 i = 0; i < detail::RowItems<O>::count; ++i) {
            if (O.value[i].bytes > (O.max_bytes ? O.max_bytes : MC_SETTING_TEXT_MAX))
                detail::error_setting_text_too_long();
            total += O.value[i].bytes;
        }
        if (total > MC_SETTING_LIST_BYTES_MAX)
            detail::error_setting_text_too_long();
        def.items = detail::RowItems<O>::items;
        def.item_count = detail::RowItems<O>::count;
        return def;
    }
};
// 0xRRGGBB.
struct Colour {
    using value_type = u32;
    struct Options {
        u32 value = 0;
        MC_DETAIL_ROW_COMMON;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        if (O.value > 0xFFFFFFu)
            detail::error_setting_default_out_of_range();
        mc_setting_def def = detail::setting_def<O>(MC_SET_COLOUR, key, label, offset);
        def.default_int = static_cast<i32>(O.value);
        return def;
    }
};
// MC_KEY(vk, MC_KEY_CTRL | ...), 0 = unbound; pressing it calls the KEYBIND hook.
struct Keybind {
    using value_type = u32;
    struct Options {
        u32 value = 0;
        MC_DETAIL_ROW_COMMON;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        const u32 vk = O.value & 0xFFFFu;
        const bool function_key = vk >= 0x70 && vk <= 0x87 && !(O.value & ~0xFFFFu); // F1..F24 alone
        if ((O.value & ~(0xFFFFu | MC_KEY_CTRL | MC_KEY_SHIFT | MC_KEY_ALT)) ||
            (O.value && !function_key && !(O.value & (MC_KEY_CTRL | MC_KEY_ALT))))
            detail::error_setting_default_out_of_range();
        mc_setting_def def = detail::setting_def<O>(MC_SET_KEYBIND, key, label, offset);
        def.default_int = static_cast<i32>(O.value);
        return def;
    }
};
// A channel id, 0 = none.
struct Channel {
    using value_type = u64;
    struct Options {
        NoDefault value = {};
        MC_DETAIL_ROW_COMMON;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        return detail::setting_def<O>(MC_SET_CHANNEL, key, label, offset);
    }
};
struct UserList {
    using value_type = IdList;
    struct Options {
        NoDefault value = {};
        MC_DETAIL_ROW_COMMON;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        return detail::setting_def<O>(MC_SET_USER_LIST, key, label, offset);
    }
};
// A section label; its field holds nothing.
struct Header {
    using value_type = u8;
    struct Options {
        NoDefault value = {};
        MC_DETAIL_ROW_COMMON;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        return detail::setting_def<O>(MC_SET_HEADER, key, label, offset);
    }
};
#undef MC_DETAIL_ROW_COMMON
#undef MC_DETAIL_ROW_TEXT
#undef MC_DETAIL_ROW_TAIL
} // namespace setting

// ---- Descriptor assembly ------------------------------------------------------

struct StateOptions {
    u32 version = 0; // bump to drop state kept across a reload when its meaning changes
};

// MC_PLUGIN's options.
struct Plugin {
    u64 owner = 0;
    Str name = {};
    Str version = {};
    Str summary = {};
    Str author = {};
    Str homepage = {};
    u64 permissions = 0;                    // MC_PERM_* the APIs need; hooks, buttons and hosts add theirs
    Str network[MC_NETWORK_HOSTS_MAX] = {}; // HTTPS hosts: "api.example.com" or "*.example.com"
    u32 api_level = MC_API_LEVEL;
    void (*start)(Ctx&) = nullptr;
    void (*stop)(Ctx&) = nullptr;
};

// MC_HOOKS(mc::filter<fn, bits>): hook.filter for MESSAGE_CLICK (MC_CLICK_FILTER_*),
// DECORATE_MESSAGE (MC_FILTER_*) and DECORATE_USER ((1u << MC_USER_*) surfaces).
template <auto F, u64 Bits> struct Filtered {};
template <auto F, u64 Bits> inline constexpr Filtered<F, Bits> filter{};

// MC_SUBMENU's name, used as MC_MENU's .parent.
struct MenuRef {
    int id = 0; // 0 = top level
};

using MenuPredicate = bool (*)(Ctx&, const MenuTarget&);
struct MenuOptions {
    MenuRef parent = {};
    MenuPredicate visible = nullptr; // shown when it returns true
    MenuPredicate checked = nullptr; // makes the item a checkbox
    bool danger = false;
};
using ButtonPredicate = bool (*)(Ctx&, const ButtonEvent&);
struct ButtonOptions {
    ButtonPredicate visible = nullptr;
    bool toggle = false; // shows ctx.button<fn>() state's MC_BUTTON_ACTIVE
    bool danger = false;
};
struct CommandOptions {
    Str args[MC_COMMAND_ARGS_MAX] = {}; // argument descriptions, in order; default: the name
    bool guild_only = false;
};

namespace surface {
inline constexpr u32 chat_bar = MC_SURFACE_CHAT_BAR;
inline constexpr u32 message_popover = MC_SURFACE_MESSAGE_POPOVER;
inline constexpr u32 account_panel = MC_SURFACE_ACCOUNT_PANEL;
inline constexpr u32 header = MC_SURFACE_HEADER;
inline constexpr u32 server_list_top = MC_SURFACE_SERVER_LIST_TOP;
inline constexpr u32 server_list_bottom = MC_SURFACE_SERVER_LIST_BOTTOM;
} // namespace surface
namespace menu {
inline constexpr u32 user = MC_MENU_USER;
inline constexpr u32 message = MC_MENU_MESSAGE;
inline constexpr u32 channel = MC_MENU_CHANNEL;
inline constexpr u32 guild = MC_MENU_GUILD;
inline constexpr u32 link = MC_MENU_LINK;
inline constexpr u32 image = MC_MENU_IMAGE;
inline constexpr u32 emoji = MC_MENU_EMOJI;
} // namespace menu

namespace detail {
template <class T, u32 N> struct Array {
    T items[N];
};
template <class T, T... I> struct Sequence {};
template <int N> using Indices = __make_integer_seq<Sequence, int, N>;

template <int> struct SettingsSlot {
    static constexpr const mc_setting_def* table = nullptr;
    static constexpr u32 count = 0;
    static constexpr u32 bytes = 0;
};

// -- Entry points: C ABI shims that wrap the context and views --
template <class D, auto F> mc_status MC_CALL draft_hook(mc_ctx* raw, mc_draft* draft) {
    Ctx ctx(raw);
    F(ctx, *static_cast<D*>(draft));
    return MC_OK;
}
template <auto F> uint32_t MC_CALL click_hook(mc_ctx* raw, const mc_message* m, const mc_click* click) {
    Ctx ctx(raw);
    return F(ctx, *static_cast<const Message*>(m), *static_cast<const Click*>(click)) ? MC_CLICK_HANDLED
                                                                                        : MC_CLICK_PASS;
}
template <auto F> uint32_t MC_CALL deleted_hook(mc_ctx* raw, const mc_message* m) {
    Ctx ctx(raw);
    return F(ctx, *static_cast<const DeletedMessage*>(m)) ? MC_KEEP : MC_DROP;
}
template <auto F> uint32_t MC_CALL edited_hook(mc_ctx* raw, const mc_message* before, const mc_message* after) {
    Ctx ctx(raw);
    return F(ctx, *static_cast<const Message*>(before), *static_cast<const Message*>(after)) ? MC_KEEP : MC_DROP;
}
template <class View, class Raw, auto F> mc_status MC_CALL decorate_hook(mc_ctx* raw, const Raw* view, mc_decoration* out) {
    Ctx ctx(raw);
    F(ctx, *static_cast<const View*>(view), *static_cast<Decoration*>(out));
    return MC_OK;
}
template <auto F> mc_status MC_CALL node_action_hook(mc_ctx* raw, const mc_node_action* action) {
    Ctx ctx(raw);
    F(ctx, *static_cast<const NodeAction*>(action));
    return MC_OK;
}
template <auto F> uint32_t MC_CALL typing_hook(mc_ctx* raw, uint64_t channel) {
    Ctx ctx(raw);
    return F(ctx, TypingNotice{channel}) ? MC_SUPPRESS : MC_ALLOW;
}
template <class Arg, auto F> void MC_CALL setting_hook(mc_ctx* raw, uint32_t index) {
    Ctx ctx(raw);
    F(ctx, Arg{index});
}
template <auto F> void MC_CALL timer_hook(mc_ctx* raw, uint64_t id) {
    Ctx ctx(raw);
    F(ctx, Timer{id});
}
// Routes a result to the handler its call registered (see start_pending).
inline void MC_CALL completion_hook(mc_ctx* raw, const mc_completion* done) {
    for (Pending& slot : g_pending)
        if (slot.resume && slot.key == done->tag && slot.kind == done->kind) {
            const Pending taken = slot;
            slot = {};
            Ctx ctx(raw);
            taken.resume(ctx, *done, taken.tag);
            return;
        }
}
template <auto F> mc_status MC_CALL start_entry(mc_ctx* raw) {
    Ctx ctx(raw);
    F(ctx);
    return MC_OK;
}
template <auto F> void MC_CALL stop_entry(mc_ctx* raw) {
    Ctx ctx(raw);
    F(ctx);
}

// -- MC_HOOKS: the kind of each handler comes from its type --
template <class> struct EventArg {
    static constexpr bool is = false;
};
template <u32 K> struct EventArg<void (*)(Ctx&, const ev::Of<K>&)> {
    static constexpr bool is = true;
    static constexpr u32 kind = K;
    static constexpr u64 bit = K == MC_EVENT_OVERFLOW ? 0 : 1ull << K; // OVERFLOW always arrives
};
template <class T> struct Unfilter {
    template <auto H> static constexpr auto fn = H;
    static constexpr u64 bits = 0;
};
template <auto F, u64 Bits> struct Unfilter<Filtered<F, Bits>> {
    template <auto> static constexpr auto fn = F;
    static constexpr u64 bits = Bits;
};
// An MC_HOOKS entry: a function, or mc::filter<function, bits>.
template <auto H> inline constexpr auto hook_fn = Unfilter<__remove_cvref(decltype(H))>::template fn<H>;
template <auto H> inline constexpr u64 hook_filter = Unfilter<__remove_cvref(decltype(H))>::bits;
template <auto H> inline constexpr bool is_event = EventArg<__remove_cv(decltype(hook_fn<H>))>::is;
template <auto H> consteval u64 event_bit() {
    if constexpr (is_event<H>)
        return EventArg<__remove_cv(decltype(hook_fn<H>))>::bit;
    else
        return 0;
}

template <class F> inline constexpr bool unknown_hook = false;
// The descriptor entry for a non-event hook; its parameter types pick the kind.
template <auto F, u64 Filter> consteval mc_hook hook() {
    using Fn = decltype(F);
    mc_hook h{};
    if constexpr (same<Fn, void (*)(Ctx&, Draft&)>) {
        h = {MC_HOOK_PRE_SEND, 0, 0, {.draft = &draft_hook<Draft, F>}};
    } else if constexpr (same<Fn, void (*)(Ctx&, EditDraft&)>) {
        h = {MC_HOOK_PRE_EDIT, 0, 0, {.draft = &draft_hook<EditDraft, F>}};
    } else if constexpr (same<Fn, bool (*)(Ctx&, const Message&, const Click&)>) {
        h = {MC_HOOK_MESSAGE_CLICK, 0, 0, {.click = &click_hook<F>}};
    } else if constexpr (same<Fn, bool (*)(Ctx&, const DeletedMessage&)>) {
        h = {MC_HOOK_MESSAGE_DELETED, 0, 0, {.deleted = &deleted_hook<F>}};
    } else if constexpr (same<Fn, bool (*)(Ctx&, const Message&, const Message&)>) {
        h = {MC_HOOK_MESSAGE_EDITED, 0, 0, {.edited = &edited_hook<F>}};
    } else if constexpr (same<Fn, void (*)(Ctx&, const Message&, Decoration&)>) {
        h = {MC_HOOK_DECORATE_MESSAGE, MC_NEED_CONTENT | MC_NEED_AUTHOR, 0,
             {.decorate_message = &decorate_hook<Message, mc_message, F>}};
    } else if constexpr (same<Fn, void (*)(Ctx&, const User&, Decoration&)>) {
        h = {MC_HOOK_DECORATE_USER, 0, 0, {.decorate_user = &decorate_hook<User, mc_user, F>}};
    } else if constexpr (same<Fn, void (*)(Ctx&, const Channel&, Decoration&)>) {
        h = {MC_HOOK_DECORATE_CHANNEL, 0, 0, {.decorate_channel = &decorate_hook<Channel, mc_channel, F>}};
    } else if constexpr (same<Fn, void (*)(Ctx&, const Guild&, Decoration&)>) {
        h = {MC_HOOK_DECORATE_GUILD, 0, 0, {.decorate_guild = &decorate_hook<Guild, mc_guild, F>}};
    } else if constexpr (same<Fn, void (*)(Ctx&, const Header&, Decoration&)>) {
        h = {MC_HOOK_DECORATE_HEADER, 0, 0, {.decorate_header = &decorate_hook<Header, mc_header, F>}};
    } else if constexpr (same<Fn, void (*)(Ctx&, const NodeAction&)>) {
        h = {MC_HOOK_NODE_ACTION, 0, 0, {.node_action = &node_action_hook<F>}};
    } else if constexpr (same<Fn, bool (*)(Ctx&, const TypingNotice&)>) {
        h = {MC_HOOK_PRE_TYPING, 0, 0, {.typing = &typing_hook<F>}};
    } else if constexpr (same<Fn, void (*)(Ctx&, const SettingChanged&)>) {
        h = {MC_HOOK_SETTING_CHANGED, 0, 0, {.setting = &setting_hook<SettingChanged, F>}};
    } else if constexpr (same<Fn, void (*)(Ctx&, const KeybindPressed&)>) {
        h = {MC_HOOK_KEYBIND, 0, 0, {.setting = &setting_hook<KeybindPressed, F>}};
    } else if constexpr (same<Fn, void (*)(Ctx&, const Timer&)>) {
        h = {MC_HOOK_TIMER, 0, 0, {.timer = &timer_hook<F>}};
    } else {
        static_assert(unknown_hook<Fn>, "MC_HOOKS: no hook kind takes these parameters (see plugin.hpp)");
    }
    constexpr bool takes_filter = same<Fn, bool (*)(Ctx&, const Message&, const Click&)> ||
                                  same<Fn, void (*)(Ctx&, const Message&, Decoration&)> ||
                                  same<Fn, void (*)(Ctx&, const User&, Decoration&)>;
    static_assert(Filter == 0 || takes_filter, "mc::filter: this hook kind takes no filter");
    h.filter = Filter;
    return h;
}

template <auto F> void dispatch_event(Ctx& ctx, const mc_event& e) {
    using Arg = EventArg<decltype(F)>;
    if constexpr (Arg::is)
        if (e.kind == Arg::kind)
            F(ctx, static_cast<const ev::Of<Arg::kind>&>(e));
}
template <auto... H> void MC_CALL events_hook(mc_ctx* raw, const mc_event* events, uint32_t count) {
    Ctx ctx(raw);
    for (u32 i = 0; i < count; ++i)
        (dispatch_event<hook_fn<H>>(ctx, events[i]), ...);
}

consteval bool unique_kinds(const mc_hook* hooks, u32 count) {
    for (u32 i = 0; i < count; ++i)
        for (u32 j = i + 1; j < count; ++j)
            if (hooks[i].kind == hooks[j].kind)
                return false;
    return true;
}

template <auto H> consteval void append_hook(mc_hook* out, u32& n) {
    if constexpr (!is_event<H>)
        out[n++] = hook<hook_fn<H>, hook_filter<H>>();
}
consteval u32 popcount(u64 bits) {
    u32 count = 0;
    for (; bits; bits &= bits - 1)
        ++count;
    return count;
}

// MC_HOOKS(h...): every hook, events merged into one EVENTS hook, plus the COMPLETION hook.
template <auto... H> struct HookSet {
    static constexpr u32 event_handlers = (0u + ... + (is_event<H> ? 1u : 0u));
    static constexpr u32 overflow_handlers = (0u + ... + (is_event<H> && event_bit<H>() == 0 ? 1u : 0u));
    static constexpr u64 event_mask = (0ull | ... | event_bit<H>());
    static_assert(event_handlers == 0 || event_mask != 0, "MC_HOOKS: handle at least one event besides ev::Overflow");
    static_assert(popcount(event_mask) + overflow_handlers == event_handlers && overflow_handlers <= 1,
                  "MC_HOOKS: two handlers for the same event");
    static constexpr u32 count = sizeof...(H) - event_handlers + (event_handlers ? 1 : 0) + 1;
    static constexpr Array<mc_hook, count> array = [] {
        Array<mc_hook, count> out{};
        u32 n = 0;
        (append_hook<H>(out.items, n), ...);
        if (event_handlers)
            out.items[n++] = {MC_HOOK_EVENTS, 0, event_mask, {.events = &events_hook<H...>}};
        out.items[n++] = {MC_HOOK_COMPLETION, 0, 0, {.completion = &completion_hook}};
        return out;
    }();
    static constexpr const mc_hook* table = array.items;
    static_assert(unique_kinds(array.items, count), "MC_HOOKS: two hooks of the same kind");
};
template <int> struct HooksSlot : HookSet<> {};

// -- MC_COMMAND, MC_MENU, MC_SUBMENU, MC_BUTTON register here by __COUNTER__ --
enum : u32 { kNothing, kCommand, kMenu, kButton };
template <int> struct Registry {
    static constexpr u32 kind = kNothing;
};

// Command arguments: the C++ type picks the MC_ARG_* type.
template <class T> struct ArgOf;
template <> struct ArgOf<Str> {
    static constexpr u32 type = MC_ARG_TEXT;
    static Str get(const mc_arg& a) {
        return Str(a.text);
    }
};
template <> struct ArgOf<i64> {
    static constexpr u32 type = MC_ARG_INT;
    static i64 get(const mc_arg& a) {
        return a.integer;
    }
};
template <> struct ArgOf<bool> {
    static constexpr u32 type = MC_ARG_BOOL;
    static bool get(const mc_arg& a) {
        return a.integer != 0;
    }
};
template <> struct ArgOf<UserId> {
    static constexpr u32 type = MC_ARG_USER;
    static UserId get(const mc_arg& a) {
        return {a.id};
    }
};
template <> struct ArgOf<ChannelId> {
    static constexpr u32 type = MC_ARG_CHANNEL;
    static ChannelId get(const mc_arg& a) {
        return {a.id};
    }
};
template <class T> struct ArgOf<Opt<T>> {
    static constexpr u32 type = ArgOf<T>::type;
    static constexpr bool optional = true;
    static Opt<T> get(const mc_arg& a) {
        return {ArgOf<T>::get(a), a.present != 0};
    }
};
template <class T> consteval bool arg_optional() {
    if constexpr (requires { ArgOf<T>::optional; })
        return true;
    else
        return false;
}

template <class> struct CommandFn {
    static constexpr bool valid = false;
};
template <class... A> struct CommandFn<Reply (*)(Ctx&, A...)> {
    static constexpr bool valid = true;
    static constexpr u32 count = sizeof...(A);
    static constexpr u32 types[count ? count : 1] = {ArgOf<A>::type...};
    static constexpr bool optional[count ? count : 1] = {arg_optional<A>()...};
    template <auto F, int... I> static Reply call(Ctx& ctx, const mc_arg* args, Sequence<int, I...>) {
        return F(ctx, ArgOf<A>::get(args[I])...);
    }
};
template <auto F> mc_status MC_CALL command_entry(mc_ctx* raw, mc_command_call* call) {
    using Fn = CommandFn<decltype(F)>;
    if (call->arg_count != Fn::count)
        return MC_E_INVALID;
    Ctx ctx(raw);
    g_command = call;
    call->reply_bytes = 0;
    const Reply reply = Fn::template call<F>(ctx, call->args, Indices<static_cast<int>(Fn::count)>{});
    call->reply_kind = reply.kind;
    g_command = nullptr;
    return MC_OK;
}

void error_command_name_must_be_1_to_32_bytes_of_a_z_0_9_dash_or_underscore();
void error_command_description_must_be_1_to_100_bytes();
void error_command_required_arguments_come_first();
void error_command_too_many_arguments();
consteval bool command_name_ok(Str name) {
    bool ok = name.bytes >= 1 && name.bytes <= MC_COMMAND_NAME_MAX;
    for (char c : name)
        ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
    return ok;
}
// The parameter names in "(Str text, Opt<i64> count)": the last word before each top-level comma.
template <const auto& Params, u32 Count> struct ArgNames {
    static constexpr Array<mc_str, Count ? Count : 1> names = [] {
        Array<mc_str, Count ? Count : 1> out{};
        constexpr u32 length = sizeof(Params) - 1;
        u32 index = 0, depth = 0, word_start = 0, word_end = 0;
        for (u32 i = 1; i < length && index < Count; ++i) {
            const char c = Params[i];
            const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            if (word) {
                if (i == 0 || word_end != i)
                    word_start = i;
                word_end = i + 1;
            } else if (c == '<') {
                ++depth;
            } else if (c == '>') {
                --depth;
            } else if ((c == ',' && depth == 0) || c == ')') {
                out.items[index++] = mc_str{Params + word_start, word_end - word_start, 0};
            }
        }
        return out;
    }();
};
template <const auto& Params, auto F, const CommandOptions& O> struct CommandArgs {
    using Fn = CommandFn<decltype(F)>;
    static_assert(Fn::valid, "MC_COMMAND: the handler takes (Ctx&, Str/i64/bool/UserId/ChannelId/Opt<...>...)");
    static constexpr Array<mc_arg_def, Fn::count ? Fn::count : 1> defs = [] {
        if (Fn::count > MC_COMMAND_ARGS_MAX)
            error_command_too_many_arguments();
        Array<mc_arg_def, Fn::count ? Fn::count : 1> out{};
        bool optional_seen = false;
        for (u32 i = 0; i < Fn::count; ++i) {
            const Str name(ArgNames<Params, Fn::count>::names.items[i]);
            if (!command_name_ok(name))
                error_command_name_must_be_1_to_32_bytes_of_a_z_0_9_dash_or_underscore();
            const Str doc = O.args[i].bytes ? O.args[i] : name;
            if (doc.bytes > MC_COMMAND_DESCRIPTION_MAX)
                error_command_description_must_be_1_to_100_bytes();
            if (optional_seen && !Fn::optional[i])
                error_command_required_arguments_come_first();
            optional_seen = optional_seen || Fn::optional[i];
            out.items[i].name = name;
            out.items[i].description = doc;
            out.items[i].type = Fn::types[i];
            out.items[i].flags = Fn::optional[i] ? 0 : MC_ARG_REQUIRED;
        }
        return out;
    }();
};
template <const auto& Params, auto F, const CommandOptions& O>
consteval mc_command_def command_def(Str name, Str description) {
    if (!command_name_ok(name))
        error_command_name_must_be_1_to_32_bytes_of_a_z_0_9_dash_or_underscore();
    if (description.bytes == 0 || description.bytes > MC_COMMAND_DESCRIPTION_MAX)
        error_command_description_must_be_1_to_100_bytes();
    using Args = CommandArgs<Params, F, O>;
    return {name, description, Args::Fn::count ? Args::defs.items : nullptr, Args::Fn::count,
            O.guild_only ? MC_COMMAND_GUILD_ONLY : 0u, &command_entry<F>};
}

template <auto F> mc_status MC_CALL menu_entry(mc_ctx* raw, const mc_menu_target* target) {
    Ctx ctx(raw);
    F(ctx, *static_cast<const MenuTarget*>(target));
    return MC_OK;
}
template <MenuPredicate Visible, MenuPredicate Checked>
uint32_t MC_CALL menu_state(mc_ctx* raw, const mc_menu_target* target) {
    Ctx ctx(raw);
    const MenuTarget& t = *static_cast<const MenuTarget*>(target);
    if constexpr (Visible != nullptr)
        if (!Visible(ctx, t))
            return MC_MENU_HIDE;
    if constexpr (Checked != nullptr)
        return Checked(ctx, t) ? MC_MENU_SHOW | MC_MENU_CHECKED : MC_MENU_SHOW;
    return MC_MENU_SHOW;
}
void error_menu_label_must_be_1_to_64_bytes();
void error_menu_targets_unknown();
template <const MenuOptions& O> consteval mc_menu_def menu_def(Str label, u32 targets, mc_menu_fn select) {
    if (label.bytes == 0 || label.bytes > MC_MENU_LABEL_MAX)
        error_menu_label_must_be_1_to_64_bytes();
    if (!targets || (targets & ~MC_MENU_TARGETS))
        error_menu_targets_unknown();
    mc_menu_def def{};
    def.label = label;
    def.targets = targets;
    def.flags = (O.checked ? MC_MENU_CHECKBOX : 0u) | (O.danger ? MC_MENU_DANGER : 0u) |
                (select ? 0u : MC_MENU_SUBMENU);
    def.parent = static_cast<u32>(O.parent.id); // a counter value until MC_PLUGIN numbers the menus
    if constexpr (O.visible != nullptr || O.checked != nullptr)
        def.visible = &menu_state<O.visible, O.checked>;
    def.select = select;
    return def;
}

template <auto F> mc_status MC_CALL button_entry(mc_ctx* raw, const mc_button_event* event) {
    Ctx ctx(raw);
    F(ctx, *static_cast<const ButtonEvent*>(event));
    return MC_OK;
}
template <ButtonPredicate Visible> uint32_t MC_CALL button_visible(mc_ctx* raw, const mc_button_event* event) {
    Ctx ctx(raw);
    return Visible(ctx, *static_cast<const ButtonEvent*>(event)) ? 1u : 0u;
}
void error_button_label_must_be_1_to_64_bytes();
void error_button_surface_unknown();
template <const ButtonOptions& O, auto F> consteval mc_button_def button_def(u32 where, Str label, Str icon) {
    if (label.bytes == 0 || label.bytes > MC_BUTTON_LABEL_MAX || icon.bytes > MC_BUTTON_LABEL_MAX)
        error_button_label_must_be_1_to_64_bytes();
    if (where < MC_SURFACE_CHAT_BAR || where > MC_SURFACE_SERVER_LIST_BOTTOM)
        error_button_surface_unknown();
    mc_button_def def{};
    def.surface = where;
    def.flags = (O.toggle ? MC_BUTTON_TOGGLE : 0u) | (O.danger ? MC_BUTTON_DANGER : 0u);
    def.label = label;
    def.icon = icon;
    if constexpr (O.visible != nullptr)
        def.visible = &button_visible<O.visible>;
    def.click = &button_entry<F>;
    return def;
}

// Everything registered before MC_PLUGIN, in declaration order. Each Registry
// entry holds `kind` and its descriptor row as `value`.
template <int End> struct Registered {
    template <int... I> static consteval u32 count_of(u32 kind, Sequence<int, I...>) {
        return (0u + ... + (Registry<I>::kind == kind ? 1u : 0u));
    }
    static constexpr u32 commands = count_of(kCommand, Indices<End>{});
    static constexpr u32 menus = count_of(kMenu, Indices<End>{});
    static constexpr u32 buttons = count_of(kButton, Indices<End>{});
    static_assert(commands <= MC_COMMAND_MAX && menus <= MC_MENU_MAX && buttons <= MC_BUTTON_MAX,
                  "MC_PLUGIN: too many commands, menu items or buttons");

    template <int I, u32 Kind, class T> static consteval void put(T* out, u32& n) {
        if constexpr (Registry<I>::kind == Kind)
            out[n++] = Registry<I>::value;
    }
    template <class T, u32 Kind, u32 N, int... I> static consteval Array<T, N ? N : 1> gather(Sequence<int, I...>) {
        Array<T, N ? N : 1> out{};
        u32 n = 0;
        (put<I, Kind>(out.items, n), ...);
        return out;
    }
    // The 1-based position among the menus of the menu registered at counter value index.
    template <int... I> static consteval u32 menu_position(int index, Sequence<int, I...>) {
        return 1u + (0u + ... + (I < index && Registry<I>::kind == kMenu ? 1u : 0u));
    }
    static constexpr auto command_table = gather<mc_command_def, kCommand, commands>(Indices<End>{});
    static constexpr auto menu_table = [] {
        auto out = gather<mc_menu_def, kMenu, menus>(Indices<End>{});
        for (u32 i = 0; i < menus; ++i)
            if (out.items[i].parent) // MenuRef.id: the submenu's counter value + 1
                out.items[i].parent = menu_position(static_cast<int>(out.items[i].parent) - 1, Indices<End>{});
        return out;
    }();
    static constexpr auto button_table = gather<mc_button_def, kButton, buttons>(Indices<End>{});
    template <auto F, int... I> static consteval u32 button_index(Sequence<int, I...>) {
        u32 index = 0, found = ~0u;
        (
            [&] {
                if constexpr (Registry<I>::kind == kButton) {
                    if (Registry<I>::fn == F)
                        found = index;
                    ++index;
                }
            }(),
            ...);
        return found;
    }
};

template <const Plugin& P> struct Hosts {
    static constexpr u32 count = count_items(P.network);
    static constexpr Array<mc_str, count ? count : 1> array = [] {
        Array<mc_str, count ? count : 1> out{};
        for (u32 i = 0; i < count; ++i)
            out.items[i] = P.network[i];
        return out;
    }();
};

consteval u64 implied_permissions(const mc_hook* hooks, u32 hook_count, const mc_button_def* buttons,
                                  u32 button_count, u32 hosts, u64 declared) {
    u64 implied = hosts ? MC_PERM_NETWORK : 0;
    if (declared & MC_PERM_AUTOMATED_ACTIONS)
        implied |= MC_PERM_ACT_MESSAGES;
    for (u32 i = 0; i < hook_count; ++i) {
        switch (hooks[i].kind) {
        case MC_HOOK_PRE_SEND:
        case MC_HOOK_PRE_EDIT:
        case MC_HOOK_PRE_TYPING: implied |= MC_PERM_MESSAGE_SEND; break;
        case MC_HOOK_MESSAGE_CLICK:
        case MC_HOOK_DECORATE_MESSAGE: implied |= MC_PERM_READ_MESSAGES; break;
        case MC_HOOK_MESSAGE_DELETED:
        case MC_HOOK_MESSAGE_EDITED: implied |= MC_PERM_READ_MESSAGES | MC_PERM_MESSAGE_HISTORY; break;
        case MC_HOOK_EVENTS:
            if (hooks[i].filter & MC_EVENT_MESSAGE_KINDS)
                implied |= MC_PERM_READ_MESSAGES;
            if (hooks[i].filter & ~MC_EVENT_MESSAGE_KINDS)
                implied |= MC_PERM_READ_ACTIVITY;
            break;
        default: break;
        }
    }
    for (u32 i = 0; i < button_count; ++i)
        if (buttons[i].surface == MC_SURFACE_MESSAGE_POPOVER)
            implied |= MC_PERM_READ_MESSAGES;
    return implied;
}
consteval u64 fnv1a(u64 hash, u64 value) {
    for (u32 i = 0; i < 8; ++i)
        hash = (hash ^ ((value >> (i * 8)) & 0xFF)) * 0x100000001B3ull;
    return hash;
}

// Slot is always 0. It makes the slots dependent, so this definition doesn't
// instantiate them before MC_SETTINGS, MC_STATE and MC_HOOKS specialize them.
template <const Plugin& P, int End, int Slot = 0> consteval mc_descriptor describe() {
    using Settings = SettingsSlot<Slot>;
    using Hooks = HooksSlot<Slot>;
    using State = typename StateSlot<Slot>::value_type;
    using Items = Registered<End>;
    using Network = Hosts<P>;
    if (P.owner == 0)
        error_plugin_owner_must_be_nonzero();
    if (P.name.bytes == 0 || P.name.bytes > MC_NAME_MAX)
        error_plugin_name_must_be_1_to_64_bytes();
    if (P.version.bytes == 0 || P.version.bytes > MC_VERSION_MAX)
        error_plugin_version_must_be_1_to_32_bytes();
    if (P.summary.bytes > MC_SUMMARY_MAX || P.author.bytes > MC_NAME_MAX ||
        P.homepage.bytes > MC_SUMMARY_MAX)
        error_plugin_text_too_long();
    if (P.api_level == 0 || P.api_level > MC_API_LEVEL)
        error_plugin_api_level_unsupported();
    if (P.permissions & ~MC_PERM_KNOWN)
        error_plugin_unknown_permissions();
    for (u32 i = 0; i < Network::count; ++i)
        if (P.network[i].bytes > MC_HOST_MAX)
            error_plugin_text_too_long();
    mc_descriptor d{};
    d.size = sizeof(mc_descriptor);
    d.abi = MC_ABI;
    d.api_level = P.api_level;
    d.owner_id = P.owner;
    d.name = P.name;
    d.version = P.version;
    d.summary = P.summary;
    d.author = P.author;
    d.homepage = P.homepage;
    if constexpr (!same<State, void>) {
        static_assert(__is_trivially_copyable(State),
                      "MC_STATE: the state type must be trivially copyable");
        static_assert(sizeof(State) <= MC_STATE_MAX && alignof(State) <= MC_STATE_ALIGN_MAX,
                      "MC_STATE: too large");
        d.state_bytes = sizeof(State);
        d.state_align = alignof(State);
        d.state_layout = fnv1a(fnv1a(fnv1a(0xCBF29CE484222325ull, sizeof(State)), alignof(State)),
                               StateSlot<Slot>::options.version);
    }
    d.settings = Settings::table;
    d.setting_count = Settings::count;
    d.settings_bytes = Settings::bytes;
    d.hooks = Hooks::table;
    d.hook_count = Hooks::count;
    if constexpr (Network::count != 0) {
        d.network = Network::array.items;
        d.network_count = Network::count;
    }
    if constexpr (Items::commands != 0) {
        d.commands = Items::command_table.items;
        d.command_count = Items::commands;
    }
    if constexpr (Items::menus != 0) {
        d.menus = Items::menu_table.items;
        d.menu_count = Items::menus;
    }
    if constexpr (Items::buttons != 0) {
        d.buttons = Items::button_table.items;
        d.button_count = Items::buttons;
    }
    d.permissions = P.permissions | implied_permissions(Hooks::table, Hooks::count, d.buttons, d.button_count,
                                                        Network::count, P.permissions);
    if constexpr (P.start != nullptr)
        d.start = &start_entry<P.start>;
    if constexpr (P.stop != nullptr)
        d.stop = &stop_entry<P.stop>;
    return d;
}

template <auto F, int D> consteval u32 button_index() {
    constexpr int end = PluginSlot<D>::counter_end;
    constexpr u32 index = Registered<end>::template button_index<F>(Indices<end>{});
    static_assert(index != ~0u, "ctx.button<F>(): F must be a function MC_BUTTON declared");
    return index;
}
} // namespace detail

} // namespace mc

// ---- Macros -------------------------------------------------------------------

#define MC_DETAIL_EXPAND(x) x
#define MC_DETAIL_CAT_(a, b) a##b
#define MC_DETAIL_CAT(a, b) MC_DETAIL_CAT_(a, b)
#define MC_DETAIL_COUNT(...)                                                                               \
    MC_DETAIL_EXPAND(MC_DETAIL_COUNT_(__VA_ARGS__, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, \
                                      18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0))
#define MC_DETAIL_COUNT_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18,  \
                         _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, n, ...)     \
    n
// m(x) for each argument, up to 32.
#define MC_DETAIL_EACH(m, ...)                                                                             \
    MC_DETAIL_EXPAND(MC_DETAIL_CAT(MC_DETAIL_EACH_, MC_DETAIL_COUNT(__VA_ARGS__))(m, __VA_ARGS__))
#define MC_DETAIL_EACH_1(m, x) m(x)
#define MC_DETAIL_EACH_2(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_1(m, __VA_ARGS__))
#define MC_DETAIL_EACH_3(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_2(m, __VA_ARGS__))
#define MC_DETAIL_EACH_4(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_3(m, __VA_ARGS__))
#define MC_DETAIL_EACH_5(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_4(m, __VA_ARGS__))
#define MC_DETAIL_EACH_6(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_5(m, __VA_ARGS__))
#define MC_DETAIL_EACH_7(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_6(m, __VA_ARGS__))
#define MC_DETAIL_EACH_8(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_7(m, __VA_ARGS__))
#define MC_DETAIL_EACH_9(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_8(m, __VA_ARGS__))
#define MC_DETAIL_EACH_10(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_9(m, __VA_ARGS__))
#define MC_DETAIL_EACH_11(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_10(m, __VA_ARGS__))
#define MC_DETAIL_EACH_12(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_11(m, __VA_ARGS__))
#define MC_DETAIL_EACH_13(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_12(m, __VA_ARGS__))
#define MC_DETAIL_EACH_14(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_13(m, __VA_ARGS__))
#define MC_DETAIL_EACH_15(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_14(m, __VA_ARGS__))
#define MC_DETAIL_EACH_16(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_15(m, __VA_ARGS__))
#define MC_DETAIL_EACH_17(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_16(m, __VA_ARGS__))
#define MC_DETAIL_EACH_18(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_17(m, __VA_ARGS__))
#define MC_DETAIL_EACH_19(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_18(m, __VA_ARGS__))
#define MC_DETAIL_EACH_20(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_19(m, __VA_ARGS__))
#define MC_DETAIL_EACH_21(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_20(m, __VA_ARGS__))
#define MC_DETAIL_EACH_22(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_21(m, __VA_ARGS__))
#define MC_DETAIL_EACH_23(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_22(m, __VA_ARGS__))
#define MC_DETAIL_EACH_24(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_23(m, __VA_ARGS__))
#define MC_DETAIL_EACH_25(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_24(m, __VA_ARGS__))
#define MC_DETAIL_EACH_26(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_25(m, __VA_ARGS__))
#define MC_DETAIL_EACH_27(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_26(m, __VA_ARGS__))
#define MC_DETAIL_EACH_28(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_27(m, __VA_ARGS__))
#define MC_DETAIL_EACH_29(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_28(m, __VA_ARGS__))
#define MC_DETAIL_EACH_30(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_29(m, __VA_ARGS__))
#define MC_DETAIL_EACH_31(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_30(m, __VA_ARGS__))
#define MC_DETAIL_EACH_32(m, x, ...) m(x) MC_DETAIL_EXPAND(MC_DETAIL_EACH_31(m, __VA_ARGS__))

#define MC_DETAIL_SETTING_FIELD(row) MC_DETAIL_EXPAND(MC_DETAIL_SETTING_FIELD_ row)
#define MC_DETAIL_SETTING_FIELD_(type, field, ...) ::mc::setting::type::value_type field;
#define MC_DETAIL_SETTING_ID(row) MC_DETAIL_EXPAND(MC_DETAIL_SETTING_ID_ row)
#define MC_DETAIL_SETTING_ID_(type, field, ...) field,
#define MC_DETAIL_SETTING_OPTIONS(row) MC_DETAIL_EXPAND(MC_DETAIL_SETTING_OPTIONS_ row)
#define MC_DETAIL_SETTING_OPTIONS_(type, field, label, ...)                                                \
    inline constexpr ::mc::setting::type::Options field{__VA_OPT__(.value = __VA_ARGS__)};
#define MC_DETAIL_SETTING_DEF(row) MC_DETAIL_EXPAND(MC_DETAIL_SETTING_DEF_ row)
#define MC_DETAIL_SETTING_DEF_(type, field, label, ...)                                                    \
    ::mc::setting::type::def<::mc::detail::setting_rows::field>(#field, label,                             \
                                                                offsetof(::mc::Settings, field)),

// MC_SETTINGS((Type, field, "Label", default, options...), ...): defines
// mc::Settings (read with ctx.settings()), mc::Setting ids (ctx.set_setting)
// and the descriptor's schema.
#define MC_SETTINGS(...)                                                                                   \
    namespace mc {                                                                                         \
    struct Settings {                                                                                      \
        MC_DETAIL_EACH(MC_DETAIL_SETTING_FIELD, __VA_ARGS__)                                               \
    };                                                                                                     \
    enum class Setting : u32 {                                                                             \
        MC_DETAIL_EACH(MC_DETAIL_SETTING_ID, __VA_ARGS__)                                                  \
    };                                                                                                     \
    namespace detail::setting_rows {                                                                       \
    MC_DETAIL_EACH(MC_DETAIL_SETTING_OPTIONS, __VA_ARGS__)                                                 \
    }                                                                                                      \
    template <> struct detail::SettingsSlot<0> {                                                           \
        static constexpr mc_setting_def table[] = {MC_DETAIL_EACH(MC_DETAIL_SETTING_DEF, __VA_ARGS__)};    \
        static constexpr u32 count = sizeof(table) / sizeof(table[0]);                                     \
        static constexpr u32 bytes = sizeof(Settings);                                                     \
        static_assert(count <= MC_SETTING_MAX && bytes <= MC_SETTINGS_BYTES_MAX,                           \
                      "MC_SETTINGS: too large");                                                           \
    };                                                                                                     \
    }                                                                                                      \
    static_assert(true)

// MC_STATE(Type, .version = n): host-owned state, zeroed at first start and
// kept across a reload while its size, alignment and version stay the same.
#define MC_STATE(type, ...)                                                                                \
    template <> struct mc::detail::StateSlot<0> {                                                          \
        using value_type = type;                                                                           \
        static constexpr ::mc::StateOptions options{__VA_ARGS__};                                          \
    };                                                                                                     \
    static_assert(true)

// MC_HOOKS(fn, mc::filter<fn, bits>, ...): the plugin's hooks; each function's
// parameters pick its kind (see the top of this file).
#define MC_HOOKS(...)                                                                                      \
    using mc_detail_hook_set = ::mc::detail::HookSet<__VA_ARGS__>;                                         \
    template <> struct mc::detail::HooksSlot<0> : ::mc_detail_hook_set {};                                 \
    static_assert(true)

#define MC_DETAIL_PARAMS(...) [[maybe_unused]] ::mc::Ctx& ctx __VA_OPT__(, __VA_ARGS__)
// MC_COMMAND(name, (Str text, Opt<i64> count), "Description", .args = {"..."}) { ... return Reply::...; }
// A local slash command; the body sees ctx and the arguments, whose names
// are the option names. Argument types: Str, i64, bool, UserId, ChannelId, Opt<...>.
#define MC_COMMAND(name, params, description, ...)                                                         \
    MC_DETAIL_COMMAND(__COUNTER__, name, params, description, __VA_ARGS__)
#define MC_DETAIL_COMMAND(n, name, params, description, ...)                                               \
    static ::mc::Reply mc_command_##name(MC_DETAIL_PARAMS params);                                         \
    template <> struct mc::detail::Registry<n> {                                                           \
        static constexpr ::mc::u32 kind = ::mc::detail::kCommand;                                          \
        static constexpr char params_text[] = #params;                                                     \
        static constexpr ::mc::CommandOptions options{__VA_ARGS__};                                        \
        static constexpr mc_command_def value =                                                            \
            ::mc::detail::command_def<params_text, &::mc_command_##name, options>(#name, description);     \
    };                                                                                                     \
    static ::mc::Reply mc_command_##name(MC_DETAIL_PARAMS params)

// MC_MENU(name, menu::link | menu::image, "Label", .parent = submenu, .visible = fn, .checked = fn) { ... }
// A context menu item; the body sees ctx and target (const MenuTarget&) in a gesture.
#define MC_MENU(name, targets, label, ...) MC_DETAIL_MENU(__COUNTER__, name, targets, label, __VA_ARGS__)
#define MC_DETAIL_MENU(n, name, targets, label, ...)                                                       \
    static void name([[maybe_unused]] ::mc::Ctx& ctx, [[maybe_unused]] const ::mc::MenuTarget& target);    \
    template <> struct mc::detail::Registry<n> {                                                           \
        static constexpr ::mc::u32 kind = ::mc::detail::kMenu;                                             \
        static constexpr ::mc::MenuOptions options{__VA_ARGS__};                                           \
        static constexpr mc_menu_def value =                                                               \
            ::mc::detail::menu_def<options>(label, targets, &::mc::detail::menu_entry<&::name>);           \
    };                                                                                                     \
    static void name([[maybe_unused]] ::mc::Ctx& ctx, [[maybe_unused]] const ::mc::MenuTarget& target)

// MC_SUBMENU(name, targets, "Label", .visible = fn): a submenu; its items name it as .parent.
#define MC_SUBMENU(name, targets, label, ...) MC_DETAIL_SUBMENU(__COUNTER__, name, targets, label, __VA_ARGS__)
#define MC_DETAIL_SUBMENU(n, name, targets, label, ...)                                                    \
    template <> struct mc::detail::Registry<n> {                                                           \
        static constexpr ::mc::u32 kind = ::mc::detail::kMenu;                                             \
        static constexpr ::mc::MenuOptions options{__VA_ARGS__};                                           \
        static constexpr mc_menu_def value = ::mc::detail::menu_def<options>(label, targets, nullptr);     \
    };                                                                                                     \
    inline constexpr ::mc::MenuRef name{(n) + 1}

// MC_BUTTON(name, surface::chat_bar, "Label", "lucide/icon", .visible = fn, .toggle = true) { ... }
// The body sees ctx and button (const ButtonEvent&; button.message() on a popover).
#define MC_BUTTON(name, where, label, icon, ...) MC_DETAIL_BUTTON(__COUNTER__, name, where, label, icon, __VA_ARGS__)
#define MC_DETAIL_BUTTON(n, name, where, label, icon, ...)                                                 \
    static void name([[maybe_unused]] ::mc::Ctx& ctx, [[maybe_unused]] const ::mc::ButtonEvent& button);   \
    template <> struct mc::detail::Registry<n> {                                                           \
        static constexpr ::mc::u32 kind = ::mc::detail::kButton;                                           \
        static constexpr ::mc::ButtonOptions options{__VA_ARGS__};                                         \
        static constexpr auto fn = &::name;                                                                \
        static constexpr mc_button_def value = ::mc::detail::button_def<options, &::name>(where, label, icon); \
    };                                                                                                     \
    static void name([[maybe_unused]] ::mc::Ctx& ctx, [[maybe_unused]] const ::mc::ButtonEvent& button)

// MC_PLUGIN(.owner = ..., .name = "...", ...): the exported descriptor (see mc::Plugin). Last in the file.
#define MC_PLUGIN(...) MC_DETAIL_PLUGIN(__COUNTER__, __VA_ARGS__)
#define MC_DETAIL_PLUGIN(end, ...)                                                                         \
    inline constexpr ::mc::Plugin mc_detail_plugin_options{__VA_ARGS__};                                   \
    template <> struct mc::detail::PluginSlot<0> {                                                         \
        static constexpr ::mc::u64 permissions = ::mc_detail_plugin_options.permissions;                   \
        static constexpr ::mc::u32 hosts = ::mc::detail::count_items(::mc_detail_plugin_options.network);  \
        static constexpr int counter_end = end;                                                            \
    };                                                                                                     \
    extern "C" MC_EXPORT constinit const mc_descriptor MC_DESCRIPTOR_SYMBOL =                              \
        ::mc::detail::describe<::mc_detail_plugin_options, end>()

// ---- MC_RUNTIME ---------------------------------------------------------------

#if defined(MC_RUNTIME)
#if !defined(__clang__)
#error "MC_RUNTIME needs clang-cl; with MSVC, link memcpy, memmove, memset, memcmp and __chkstk yourself."
#endif
extern "C" const int _fltused = 0;
extern "C" void* memcpy(void* destination, const void* source, size_t bytes) {
    void* result = destination;
    __asm__ volatile("rep movsb" : "+D"(destination), "+S"(source), "+c"(bytes) : : "memory");
    return result;
}
extern "C" void* memmove(void* destination, const void* source, size_t bytes) {
    const auto to = reinterpret_cast<size_t>(destination);
    const auto from = reinterpret_cast<size_t>(source);
    if (to - from >= bytes)
        return memcpy(destination, source, bytes); // no overlap, or copying down
    void* result = destination;
    char* last_to = static_cast<char*>(destination) + bytes - 1;
    const char* last_from = static_cast<const char*>(source) + bytes - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(last_to), "+S"(last_from), "+c"(bytes) : : "memory");
    return result;
}
extern "C" void* memset(void* destination, int value, size_t bytes) {
    void* result = destination;
    __asm__ volatile("rep stosb" : "+D"(destination), "+c"(bytes) : "a"(value) : "memory");
    return result;
}
extern "C" __attribute__((no_builtin)) int memcmp(const void* a, const void* b, size_t bytes) {
    const auto* left = static_cast<const unsigned char*>(a);
    const auto* right = static_cast<const unsigned char*>(b);
    for (size_t i = 0; i < bytes; ++i)
        if (left[i] != right[i])
            return left[i] < right[i] ? -1 : 1;
    return 0;
}
// Probes each page of a stack allocation of rax bytes (the compiler emits the call for frames over 4 KiB).
extern "C" __attribute__((naked)) void __chkstk() {
    __asm__ volatile("pushq %rcx\n\t"
                     "pushq %rax\n\t"
                     "leaq 24(%rsp), %rcx\n\t"
                     "cmpq $0x1000, %rax\n\t"
                     "jb 1f\n"
                     "2:\n\t"
                     "subq $0x1000, %rcx\n\t"
                     "testq %rcx, (%rcx)\n\t"
                     "subq $0x1000, %rax\n\t"
                     "cmpq $0x1000, %rax\n\t"
                     "ja 2b\n"
                     "1:\n\t"
                     "subq %rax, %rcx\n\t"
                     "testq %rcx, (%rcx)\n\t"
                     "popq %rax\n\t"
                     "popq %rcx\n\t"
                     "retq");
}
#endif
