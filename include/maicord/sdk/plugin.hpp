// MaiCord plugin authoring layer for ABI 6: header-only C++23 over plugin_abi.h.
// Standalone: no MaiCord core headers, no STL, CRT, exceptions or RTTI.
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
//   MC_PLUGIN(.owner = 0x..., .name = "...", .version = "1.0.0", .summary = "...");
//
// MC_PLUGIN ends the file and defines the exported descriptor. Use the macros
// at global scope, once each. Designated options follow declaration order.
//
// Hook signatures (MC_HOOKS):
//   void (Ctx&, Draft&)      MC_HOOK_PRE_SEND
//   void (Ctx&, EditDraft&)  MC_HOOK_PRE_EDIT
//
// Settings rows (MC_SETTINGS): (Type, field, "Label", default, options...)
//   Toggle    bool  default true/false        .description .flags .visible_if
//   Int       i32   default n                 .min .max .description .flags .visible_if
//   Slider    i32   default n                 .min .max .step .description .flags .visible_if
//   Choice    u32   default index             .items = {"A", "B"} .description .flags .visible_if
//   Text      Str   default "text" or {}      .description .hint .max_bytes .flags .visible_if
//   TextList  StrList default {"a", "b"} or {} .description .hint .max_bytes .flags .visible_if
// The field name is the persisted key: 1..32 bytes of [a-z0-9_].
//
// Define MC_RUNTIME (clang-cl) in the plugin's one translation unit to get
// memcpy, memmove, memset, memcmp, __chkstk and _fltused without a CRT.
#pragma once

#include "plugin_abi.h"

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
    critical = MC_TONE_CRITICAL
};
enum class Level : u32 {
    debug = MC_LOG_DEBUG,
    info = MC_LOG_INFO,
    warning = MC_LOG_WARNING,
    error = MC_LOG_ERROR
};

struct Settings; // MC_SETTINGS defines it

namespace detail {
template <class A, class B> inline constexpr bool same = false;
template <class A> inline constexpr bool same<A, A> = true;
template <class T> inline constexpr bool is_array = false;
template <class T, size_t N> inline constexpr bool is_array<T[N]> = true;
template <int> struct StateSlot {
    using value_type = void;
};
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

// Passed to every handler; valid until the handler returns.
class Ctx {
  public:
    explicit Ctx(mc_ctx* raw) : store{raw}, raw_(raw) {
    }

    Store store;

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
    template <class Id> mc_status set_setting(Id id, Str value) const {
        return raw_->api->set_setting(raw_, static_cast<u32>(id), value.data, value.bytes);
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

} // namespace url

// ---- Settings types (MC_SETTINGS rows) ----------------------------------------

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

consteval mc_setting_def setting_def(u32 type, Str key, Str label, u32 offset, Str description, u32 flags,
                                     u16 visible_if) {
    bool key_ok = key.bytes >= 1 && key.bytes <= MC_SETTING_KEY_MAX;
    for (char c : key)
        key_ok = key_ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
    if (!key_ok)
        error_setting_key_must_be_1_to_32_bytes_of_a_z_0_9_or_underscore();
    if (label.bytes == 0 || label.bytes > MC_SETTING_LABEL_MAX)
        error_setting_label_must_be_1_to_64_bytes();
    if (description.bytes > MC_SETTING_TEXT_MAX)
        error_setting_text_too_long();
    if (flags & ~(MC_SET_HIDDEN | MC_SET_DANGER | MC_SET_MULTILINE))
        error_setting_unknown_flags();
    mc_setting_def def{};
    def.key = key;
    def.label = label;
    def.description = description;
    def.type = type;
    def.offset = offset;
    def.flags = flags;
    def.visible_if = visible_if;
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
    if (Options.hint.bytes > MC_SETTING_TEXT_MAX)
        error_setting_text_too_long();
    if (Options.max_bytes > MC_SETTING_TEXT_MAX)
        error_setting_text_too_long();
    def.hint = Options.hint;
    def.max_bytes = Options.max_bytes;
}
} // namespace detail

namespace setting {
struct Toggle {
    using value_type = bool;
    struct Options {
        bool value = false;
        Str description = {};
        u32 flags = 0;
        u16 visible_if = 0;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        mc_setting_def def =
            detail::setting_def(MC_SET_TOGGLE, key, label, offset, O.description, O.flags, O.visible_if);
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
        Str description = {};
        u32 flags = 0;
        u16 visible_if = 0;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        if (O.min > O.max || O.value < O.min || O.value > O.max)
            detail::error_setting_default_out_of_range();
        mc_setting_def def =
            detail::setting_def(MC_SET_INT, key, label, offset, O.description, O.flags, O.visible_if);
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
        Str description = {};
        u32 flags = 0;
        u16 visible_if = 0;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        if (O.min >= O.max || O.step <= 0 || O.value < O.min || O.value > O.max)
            detail::error_setting_default_out_of_range();
        mc_setting_def def =
            detail::setting_def(MC_SET_SLIDER, key, label, offset, O.description, O.flags, O.visible_if);
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
        Str description = {};
        u32 flags = 0;
        u16 visible_if = 0;
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
        mc_setting_def def =
            detail::setting_def(MC_SET_CHOICE, key, label, offset, O.description, O.flags, O.visible_if);
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
        Str description = {};
        Str hint = {};
        u16 max_bytes = 0;
        u32 flags = 0;
        u16 visible_if = 0;
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        if (O.value.bytes > (O.max_bytes ? O.max_bytes : MC_SETTING_TEXT_MAX))
            detail::error_setting_text_too_long();
        mc_setting_def def =
            detail::setting_def(MC_SET_TEXT, key, label, offset, O.description, O.flags, O.visible_if);
        detail::put_text_fields<O>(def);
        def.default_text = O.value;
        return def;
    }
};
struct TextList {
    using value_type = StrList;
    struct Options {
        Str value[MC_SETTING_ITEM_MAX] = {};
        Str description = {};
        Str hint = {};
        u16 max_bytes = 0;
        u32 flags = 0;
        u16 visible_if = 0;
        constexpr const Str* list() const {
            return value;
        }
    };
    template <const Options& O> static consteval mc_setting_def def(Str key, Str label, u32 offset) {
        mc_setting_def def =
            detail::setting_def(MC_SET_TEXT_LIST, key, label, offset, O.description, O.flags, O.visible_if);
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
    u64 permissions = 0; // MC_PERM_* the APIs need; hooks add their own
    u32 api_level = MC_API_LEVEL;
    void (*start)(Ctx&) = nullptr;
    void (*stop)(Ctx&) = nullptr;
};

namespace detail {
template <int> struct SettingsSlot {
    static constexpr const mc_setting_def* table = nullptr;
    static constexpr u32 count = 0;
    static constexpr u32 bytes = 0;
};
template <int> struct HooksSlot {
    static constexpr const mc_hook* table = nullptr;
    static constexpr u32 count = 0;
    static constexpr u64 permissions = 0;
};

template <class D, auto F> mc_status MC_CALL draft_hook(mc_ctx* raw, mc_draft* draft) {
    Ctx ctx(raw);
    F(ctx, *static_cast<D*>(draft));
    return MC_OK;
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

template <class F> inline constexpr bool unknown_hook = false;
// The descriptor entry for a hook function; its parameter types pick the kind.
template <auto F> consteval mc_hook hook() {
    using Fn = decltype(F);
    if constexpr (same<Fn, void (*)(Ctx&, Draft&)>) {
        return {MC_HOOK_PRE_SEND, 0, 0, {.draft = &draft_hook<Draft, F>}};
    } else if constexpr (same<Fn, void (*)(Ctx&, EditDraft&)>) {
        return {MC_HOOK_PRE_EDIT, 0, 0, {.draft = &draft_hook<EditDraft, F>}};
    } else {
        static_assert(unknown_hook<Fn>, "MC_HOOKS: no hook kind takes these parameters (see plugin.hpp)");
        return {};
    }
}
consteval bool unique_kinds(const mc_hook* hooks, u32 count) {
    for (u32 i = 0; i < count; ++i)
        for (u32 j = i + 1; j < count; ++j)
            if (hooks[i].kind == hooks[j].kind)
                return false;
    return true;
}
consteval u64 hook_permissions(const mc_hook* hooks, u32 count) {
    u64 permissions = 0;
    for (u32 i = 0; i < count; ++i)
        if (hooks[i].kind == MC_HOOK_PRE_SEND || hooks[i].kind == MC_HOOK_PRE_EDIT)
            permissions |= MC_PERM_MESSAGE_SEND;
    return permissions;
}
consteval u64 fnv1a(u64 hash, u64 value) {
    for (u32 i = 0; i < 8; ++i)
        hash = (hash ^ ((value >> (i * 8)) & 0xFF)) * 0x100000001B3ull;
    return hash;
}

// Slot is always 0. It makes the slots dependent, so this definition doesn't
// instantiate them before MC_SETTINGS, MC_STATE and MC_HOOKS specialize them.
template <const Plugin& P, int Slot = 0> consteval mc_descriptor describe() {
    using Settings = SettingsSlot<Slot>;
    using Hooks = HooksSlot<Slot>;
    using State = typename StateSlot<Slot>::value_type;
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
    d.permissions = P.permissions | Hooks::permissions;
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
    if constexpr (P.start != nullptr)
        d.start = &start_entry<P.start>;
    if constexpr (P.stop != nullptr)
        d.stop = &stop_entry<P.stop>;
    return d;
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
    inline constexpr ::mc::setting::type::Options field{.value = __VA_ARGS__};
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

#define MC_DETAIL_HOOK(fn) ::mc::detail::hook<fn>(),
// MC_HOOKS(fn, ...): the plugin's hooks; each function's parameters pick its kind.
#define MC_HOOKS(...)                                                                                      \
    template <> struct mc::detail::HooksSlot<0> {                                                          \
        static constexpr mc_hook table[] = {MC_DETAIL_EACH(MC_DETAIL_HOOK, __VA_ARGS__)};                  \
        static constexpr ::mc::u32 count = sizeof(table) / sizeof(table[0]);                               \
        static_assert(::mc::detail::unique_kinds(table, count), "MC_HOOKS: two hooks of the same kind");   \
        static constexpr ::mc::u64 permissions = ::mc::detail::hook_permissions(table, count);             \
    };                                                                                                     \
    static_assert(true)

// MC_PLUGIN(.owner = ..., .name = "...", ...): the exported descriptor (see mc::Plugin).
#define MC_PLUGIN(...)                                                                                     \
    namespace mc::detail {                                                                                 \
    inline constexpr ::mc::Plugin plugin_options{__VA_ARGS__};                                             \
    }                                                                                                      \
    extern "C" MC_EXPORT constinit const mc_descriptor MC_DESCRIPTOR_SYMBOL =                              \
        ::mc::detail::describe<::mc::detail::plugin_options>()

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
