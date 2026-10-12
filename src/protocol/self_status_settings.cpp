#include "maicord/protocol/self_status_settings.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "proto_wire.hpp"

namespace maicord {
namespace {
using E = SelfStatusSettingsError;
using namespace protocol_detail;
using Wire = ProtoSource;
bool wrapper(const Wire& w, u32 p, u32 end, bool text, SelfStatusSettingsProjection& result) noexcept {
    while (p < end) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, end, f, t))
            return false;
        if (f != 1) {
            if (!proto_skip(w, p, end, t, f))
                return false;
            continue;
        }
        if (!text) {
            if (t != 0 || !proto_varint(w, p, end, result.created_at_ms))
                return false;
            continue;
        }
        u32 stop = 0;
        if (t != 2 || !proto_length(w, p, end, stop))
            return false;
        const auto equals = [&](const char* s, u32 bytes) noexcept {
            if (stop - p != bytes)
                return false;
            for (u32 i = 0; i < bytes; ++i)
                if (w.at(p + i) != static_cast<u8>(s[i]))
                    return false;
            return true;
        };
        result.known = true;
        if (equals("online", 6))
            result.status = GatewayUserStatus::online;
        else if (equals("idle", 4))
            result.status = GatewayUserStatus::idle;
        else if (equals("dnd", 3))
            result.status = GatewayUserStatus::dnd;
        else if (equals("invisible", 9))
            result.status = GatewayUserStatus::invisible;
        else
            result.known = false;
        p = stop;
    }
    return true;
}
bool group(const Wire& w, u32 p, u32 end, SelfStatusSettingsProjection& result) noexcept {
    while (p < end) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, end, f, t))
            return false;
        if (f == 1 || f == 5) {
            u32 stop = 0;
            if (t != 2 || !proto_length(w, p, end, stop) || !wrapper(w, p, stop, f == 1, result))
                return false;
            p = stop;
        } else if (f == 4) {
            if (t != 1 || end - p < 8)
                return false;
            result.expires_at_ms = proto_fixed64(w, p);
            p += 8;
        } else if (!proto_skip(w, p, end, t, f))
            return false;
    }
    return true;
}
E decode(const Wire& w, Slice<u8> output, SelfStatusSettingsProjection* result) noexcept {
    SelfStatusSettingsProjection candidate{};
    u32 p = 0;
    while (p < w.size) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, w.size, f, t))
            return E::protobuf;
        if (f != 11) {
            if (!proto_skip(w, p, w.size, t, f))
                return E::protobuf;
            continue;
        }
        u32 stop = 0;
        if (t != 2 || !proto_length(w, p, w.size, stop))
            return E::protobuf;
        candidate.present = true;
        if (stop - p > kSelfStatusSettingsMaximumGroupBytes - candidate.group_bytes)
            return E::capacity;
        candidate.group_bytes += stop - p;
        if (!group(w, p, stop, candidate))
            return E::protobuf;
        p = stop;
    }
    if (candidate.group_bytes > output.count)
        return E::capacity;
    p = 0;
    u32 written = 0;
    while (p < w.size) {
        u32 f = 0, t = 0;
        proto_tag(w, p, w.size, f, t);
        if (f != 11) {
            proto_skip(w, p, w.size, t, f);
            continue;
        }
        u32 stop = 0;
        proto_length(w, p, w.size, stop);
        while (p < stop)
            output.data[written++] = w.at(p++);
    }
    *result = candidate;
    return E::none;
}
Utf8View status_name(GatewayUserStatus status) noexcept {
    switch (status) {
    case GatewayUserStatus::online:
        return utf8_literal("online");
    case GatewayUserStatus::idle:
        return utf8_literal("idle");
    case GatewayUserStatus::dnd:
        return utf8_literal("dnd");
    case GatewayUserStatus::invisible:
        return utf8_literal("invisible");
    default:
        return {};
    }
}
bool custom_message(const Wire& w, u32 p, u32 end, SelfCustomStatus& out) noexcept {
    out = {};
    out.present = true;
    while (p < end) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, end, f, t))
            return false;
        if ((f == 1 || f == 3) && t == 2) {
            u32 stop = 0;
            if (!proto_length(w, p, end, stop) ||
                stop - p > (f == 1 ? kSelfCustomStatusTextBytes : kSelfCustomStatusEmojiBytes))
                return false;
            char* target = f == 1 ? out.text : out.emoji_name;
            const u32 bytes = stop - p;
            for (u32 i = 0; i < bytes; ++i)
                target[i] = static_cast<char>(w.at(p++));
            if (f == 1)
                out.text_bytes = static_cast<u16>(bytes);
            else
                out.emoji_name_bytes = static_cast<u8>(bytes);
        } else if ((f == 2 || f == 4 || f == 5) && t == 1) {
            if (end - p < 8)
                return false;
            (f == 2 ? out.emoji_id : f == 4 ? out.expires_at_ms : out.created_at_ms) = proto_fixed64(w, p);
            p += 8;
        } else if (!proto_skip(w, p, end, t, f))
            return false;
    }
    return utf8_validate({out.text, out.text_bytes}).valid &&
           utf8_validate({out.emoji_name, out.emoji_name_bytes}).valid;
}
bool range_valid(Slice<u8> s) noexcept {
    return byte_range_valid({s.data, s.count});
}
// The observed group, scratch and output of a patch writer are valid and disjoint.
bool patch_buffers_valid(ConstSlice<u8> observed, Slice<u8> scratch, Slice<u8> output) noexcept {
    return observed.count <= kSelfStatusSettingsMaximumGroupBytes && byte_range_valid(observed) &&
           range_valid(scratch) && range_valid(output) &&
           !byte_ranges_overlap(observed, {scratch.data, scratch.count}) &&
           !byte_ranges_overlap(observed, {output.data, output.count}) &&
           !byte_ranges_overlap({scratch.data, scratch.count}, {output.data, output.count});
}
// Copies the observed group's fields, except those `dropped` matches, byte for
// byte to scratch after five bytes reserved for finish_patch; returns the end
// of the copy.
template <class Dropped>
u32 copy_kept_fields(const Wire& w, ConstSlice<u8> observed, u8* scratch, Dropped dropped) noexcept {
    u32 p = 0, at = 5;
    while (p < w.size) {
        const u32 begin = p;
        u32 f = 0, t = 0;
        proto_tag(w, p, w.size, f, t);
        proto_skip(w, p, w.size, t, f);
        if (!dropped(f)) {
            copy_bytes(scratch + at, observed.data + begin, p - begin);
            at += p - begin;
        }
    }
    return at;
}
// Wraps the group in scratch[5, at) as settings field 11, in front of it in
// the reserved bytes, and writes the PATCH body for that settings message.
GatewayCommandWriteResult finish_patch(Slice<u8> scratch, u32 at, Slice<u8> output) noexcept {
    u8 prefix[5]{};
    ProtoWriter head{{prefix, 5}, 0, true};
    head.tag(11, 2);
    head.varint(at - 5);
    copy_bytes(scratch.data + 5 - head.at, prefix, head.at);
    const ConstSlice<u8> proto{scratch.data + 5 - head.at, at - 5 + head.at};
    const u32 required = proto_settings_patch_bytes(proto.count, 0);
    if (output.count < required)
        return {0, GatewayCommandError::output_too_small};
    if (!proto_write_settings_patch(proto, 0, output))
        return {0, GatewayCommandError::invalid_input};
    return {required, GatewayCommandError::none};
}
} // namespace
E self_status_settings_decode_proto(ConstSlice<u8> input, Slice<u8> output,
                                    SelfStatusSettingsProjection* result) noexcept {
    if (!result || input.count > kSelfStatusSettingsMaximumProtoBytes || !byte_range_valid(input) ||
        !range_valid(output) || byte_ranges_overlap(input, {output.data, output.count}))
        return E::input;
    return decode({input, {}, input.count}, output, result);
}
E self_status_settings_decode_base64(Utf8View input, Slice<u8> output,
                                     SelfStatusSettingsProjection* result) noexcept {
    if (!result || input.bytes > base64_encoded_size(kSelfStatusSettingsMaximumProtoBytes) ||
        !range_valid(output) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(input.data), input.bytes},
                            {output.data, output.count}))
        return E::input;
    const auto valid = base64_decode(input, {});
    if (!valid.ok() && valid.error != Base64Error::output_too_small)
        return E::base64;
    return decode({{}, input, valid.bytes}, output, result);
}
GatewayCommandWriteResult self_status_settings_write_patch(ConstSlice<u8> observed,
                                                           GatewayUserStatus status, u64 unix_now_ms,
                                                           Slice<u8> scratch, Slice<u8> output) noexcept {
    const auto name = status_name(status);
    if (!name.bytes || !unix_now_ms || !patch_buffers_valid(observed, scratch, output))
        return {0, GatewayCommandError::invalid_input};
    if (scratch.count < observed.count + 64)
        return {0, GatewayCommandError::output_too_small};
    Wire w{observed, {}, observed.count};
    SelfStatusSettingsProjection valid{};
    if (!group(w, 0, w.size, valid))
        return {0, GatewayCommandError::invalid_input};
    // Preserve opaque siblings byte-for-byte, including custom status, game
    // visibility and unknown tags.
    u32 at = copy_kept_fields(w, observed, scratch.data,
                              [](u32 f) noexcept { return f == 1 || f == 4 || f == 5; });
    const auto replace_wrapper = [&](u32 wrapper_field, bool text) noexcept {
        const u32 begin = at;
        at += 4; // one-byte tag, up to three-byte group length
        u32 read = 0;
        while (read < w.size) {
            u32 f = 0, t = 0;
            proto_tag(w, read, w.size, f, t);
            if (f != wrapper_field) {
                proto_skip(w, read, w.size, t, f);
                continue;
            }
            u32 stop = 0;
            proto_length(w, read, w.size, stop);
            while (read < stop) {
                const u32 old_begin = read;
                u32 inner = 0, inner_type = 0;
                proto_tag(w, read, stop, inner, inner_type);
                proto_skip(w, read, stop, inner_type, inner);
                if (inner != 1) {
                    copy_bytes(scratch.data + at, observed.data + old_begin, read - old_begin);
                    at += read - old_begin;
                }
            }
        }
        ProtoWriter value{scratch, at, true};
        if (text) {
            value.tag(1, 2);
            value.varint(name.bytes);
            value.bytes(name.data, name.bytes);
        } else {
            value.tag(1, 0);
            value.varint(unix_now_ms);
        }
        at = value.at;
        u8 prefix[4]{};
        ProtoWriter head{{prefix, 4}, 0, true};
        head.tag(wrapper_field, 2);
        head.varint(at - begin - 4);
        const u32 bytes = head.at;
        // Copy downward without memcpy overlap; retained opaque wrapper fields
        // survive a known value change as well as their unknown siblings.
        const u32 contents = at - begin - 4;
        for (u32 i = 0; i < contents; ++i)
            scratch.data[begin + bytes + i] = scratch.data[begin + 4 + i];
        copy_bytes(scratch.data + begin, prefix, bytes);
        at = begin + bytes + contents;
    };
    replace_wrapper(1, true);
    replace_wrapper(5, false);
    return finish_patch(scratch, at, output);
}
bool self_status_settings_read_custom(ConstSlice<u8> group, SelfCustomStatus* output) noexcept {
    if (!output || !byte_range_valid(group))
        return false;
    *output = {};
    const Wire w{group, {}, group.count};
    u32 p = 0;
    SelfCustomStatus last{};
    bool found = false;
    while (p < w.size) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, w.size, f, t))
            return false;
        if (f != 2 || t != 2) {
            if (!proto_skip(w, p, w.size, t, f))
                return false;
            continue;
        }
        u32 stop = 0;
        if (!proto_length(w, p, w.size, stop) || !custom_message(w, p, stop, last))
            return false;
        found = true;
        p = stop;
    }
    if (found)
        *output = last;
    return true;
}
bool self_custom_status_active(const SelfCustomStatus& status, u64 unix_now_ms) noexcept {
    return status.present && (status.text_bytes || status.emoji_id || status.emoji_name_bytes) &&
           (!status.expires_at_ms || status.expires_at_ms > unix_now_ms);
}
bool self_custom_status_equal(const SelfCustomStatus& a, const SelfCustomStatus& b) noexcept {
    return a.present == b.present && a.emoji_id == b.emoji_id && a.expires_at_ms == b.expires_at_ms &&
           a.created_at_ms == b.created_at_ms && a.text_bytes == b.text_bytes &&
           a.emoji_name_bytes == b.emoji_name_bytes && bytes_equal(a.text, b.text, a.text_bytes) &&
           bytes_equal(a.emoji_name, b.emoji_name, a.emoji_name_bytes);
}
GatewayCommandWriteResult self_status_settings_write_custom_patch(ConstSlice<u8> observed,
                                                                  const SelfCustomStatus* custom,
                                                                  Slice<u8> scratch, Slice<u8> output) noexcept {
    if (!patch_buffers_valid(observed, scratch, output) ||
        (custom && (custom->text_bytes > kSelfCustomStatusTextBytes ||
                    custom->emoji_name_bytes > kSelfCustomStatusEmojiBytes ||
                    !utf8_validate({custom->text, custom->text_bytes}).valid ||
                    !utf8_validate({custom->emoji_name, custom->emoji_name_bytes}).valid)))
        return {0, GatewayCommandError::invalid_input};
    if (scratch.count < observed.count + 704)
        return {0, GatewayCommandError::output_too_small};
    const Wire w{observed, {}, observed.count};
    SelfStatusSettingsProjection valid{};
    if (!group(w, 0, w.size, valid))
        return {0, GatewayCommandError::invalid_input};
    u32 at = copy_kept_fields(w, observed, scratch.data, [](u32 f) noexcept { return f == 2; });
    const bool keep = custom && (custom->text_bytes || custom->emoji_id || custom->emoji_name_bytes);
    if (keep) {
        u8 inner[kSelfCustomStatusTextBytes + kSelfCustomStatusEmojiBytes + 48];
        ProtoWriter message{{inner, sizeof(inner)}, 0, true};
        if (custom->text_bytes) {
            message.tag(1, 2);
            message.varint(custom->text_bytes);
            message.bytes(custom->text, custom->text_bytes);
        }
        if (custom->emoji_id) {
            message.tag(2, 1);
            message.fixed64(custom->emoji_id);
        }
        if (custom->emoji_name_bytes) {
            message.tag(3, 2);
            message.varint(custom->emoji_name_bytes);
            message.bytes(custom->emoji_name, custom->emoji_name_bytes);
        }
        if (custom->expires_at_ms) {
            message.tag(4, 1);
            message.fixed64(custom->expires_at_ms);
        }
        if (custom->created_at_ms) {
            message.tag(5, 1);
            message.fixed64(custom->created_at_ms);
        }
        ProtoWriter status{scratch, at, true};
        status.tag(2, 2);
        status.varint(message.at);
        status.bytes(inner, message.at);
        at = status.at;
    }
    return finish_patch(scratch, at, output);
}
bool self_status_settings_read_show_game(ConstSlice<u8> group, bool* share) noexcept {
    if (!share || !byte_range_valid(group))
        return false;
    *share = true; // An absent BoolValue keeps the server default: shared.
    const Wire w{group, {}, group.count};
    u32 p = 0;
    while (p < w.size) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, w.size, f, t))
            return false;
        if (f != 3 || t != 2) {
            if (!proto_skip(w, p, w.size, t, f))
                return false;
            continue;
        }
        u32 stop = 0;
        if (!proto_length(w, p, w.size, stop))
            return false;
        bool value = false; // A present, empty wrapper is false.
        while (p < stop) {
            u32 inner = 0, inner_type = 0;
            if (!proto_tag(w, p, stop, inner, inner_type))
                return false;
            if (inner == 1 && inner_type == 0) {
                u64 n = 0;
                if (!proto_varint(w, p, stop, n))
                    return false;
                value = n != 0;
            } else if (!proto_skip(w, p, stop, inner_type, inner))
                return false;
        }
        *share = value;
    }
    return true;
}
GatewayCommandWriteResult self_status_settings_write_show_game_patch(ConstSlice<u8> observed, bool share,
                                                                     Slice<u8> scratch, Slice<u8> output) noexcept {
    if (!patch_buffers_valid(observed, scratch, output))
        return {0, GatewayCommandError::invalid_input};
    if (scratch.count < observed.count + 16)
        return {0, GatewayCommandError::output_too_small};
    const Wire w{observed, {}, observed.count};
    SelfStatusSettingsProjection valid{};
    if (!group(w, 0, w.size, valid))
        return {0, GatewayCommandError::invalid_input};
    // Every other StatusSettings field is kept byte for byte.
    u32 at = copy_kept_fields(w, observed, scratch.data, [](u32 f) noexcept { return f == 3; });
    scratch.data[at++] = 26;
    if (share) {
        scratch.data[at++] = 2;
        scratch.data[at++] = 8;
        scratch.data[at++] = 1;
    } else
        scratch.data[at++] = 0;
    return finish_patch(scratch, at, output);
}
u64 self_custom_status_expiry(SelfCustomStatusClear clear, u64 now, u64 local_ms_of_day) noexcept {
    switch (clear) {
    case SelfCustomStatusClear::minutes_30:
        return now + 1'800'000;
    case SelfCustomStatusClear::hour_1:
        return now + 3'600'000;
    case SelfCustomStatusClear::hours_4:
        return now + 14'400'000;
    case SelfCustomStatusClear::today:
        return local_ms_of_day < 86'400'000 ? now + (86'400'000 - local_ms_of_day) : 0;
    default:
        return 0;
    }
}
bool self_custom_status_build(Utf8View emoji, Utf8View text, SelfCustomStatusClear clear, u64 now,
                              u64 local_ms_of_day, SelfCustomStatus* output) noexcept {
    const auto trim = [](Utf8View v) noexcept {
        while (v.bytes && (v.data[0] == ' ' || v.data[0] == '	')) {
            ++v.data;
            --v.bytes;
        }
        while (v.bytes && (v.data[v.bytes - 1] == ' ' || v.data[v.bytes - 1] == '	'))
            --v.bytes;
        return v;
    };
    if (!output || clear >= SelfCustomStatusClear::count)
        return false;
    emoji = trim(emoji);
    text = trim(text);
    const auto valid = utf8_validate(text);
    if (!valid.valid || text.bytes > kSelfCustomStatusTextBytes ||
        valid.codepoints > kSelfCustomStatusTextCodepoints || !utf8_validate(emoji).valid)
        return false;
    SelfCustomStatus result{};
    if (emoji.bytes > 3 && emoji.data[0] == '<' && emoji.data[emoji.bytes - 1] == '>') {
        u32 at = emoji.data[1] == 'a' ? 2 : 1;
        if (emoji.data[at++] != ':')
            return false;
        const u32 name = at;
        while (at < emoji.bytes && emoji.data[at] != ':')
            ++at;
        if (at == name || at + 2 >= emoji.bytes || at - name > kSelfCustomStatusEmojiBytes)
            return false;
        copy_bytes(result.emoji_name, emoji.data + name, at - name);
        result.emoji_name_bytes = static_cast<u8>(at - name);
        for (++at; at + 1 < emoji.bytes; ++at) {
            const char c = emoji.data[at];
            if (c < '0' || c > '9' || result.emoji_id > (~u64{0} - 9) / 10)
                return false;
            result.emoji_id = result.emoji_id * 10 + static_cast<u64>(c - '0');
        }
        if (!result.emoji_id)
            return false;
    } else if (emoji.bytes) {
        if (emoji.bytes > kSelfCustomStatusEmojiBytes)
            return false;
        copy_bytes(result.emoji_name, emoji.data, emoji.bytes);
        result.emoji_name_bytes = static_cast<u8>(emoji.bytes);
    }
    copy_bytes(result.text, text.data, text.bytes);
    result.text_bytes = static_cast<u16>(text.bytes);
    result.present = text.bytes || result.emoji_name_bytes || result.emoji_id;
    if (result.present) {
        result.created_at_ms = now;
        result.expires_at_ms = self_custom_status_expiry(clear, now, local_ms_of_day);
    }
    *output = result;
    return true;
}
} // namespace maicord
