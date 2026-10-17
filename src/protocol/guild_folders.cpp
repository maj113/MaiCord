#include "maicord/protocol/guild_folders.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/user_settings_proto.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "proto_wire.hpp"

namespace maicord {
namespace {
using namespace protocol_detail;
using E = GuildFoldersError;
// Raw or lazily decoded base64 bytes; the outer settings message is never
// materialized.
using Wire = ProtoSource;
// Wrapper messages keep their value in field 1; a present empty wrapper is zero.
bool wrapper_varint(const Wire& w, u32 p, u32 end, u64& value) noexcept {
    value = 0;
    while (p < end) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, end, f, t))
            return false;
        if (f == 1) {
            if (t != 0 || !proto_varint(w, p, end, value))
                return false;
        } else if (!proto_skip(w, p, end, t, f))
            return false;
    }
    return true;
}
bool wrapper_string(const Wire& w, u32 p, u32 end, u32& start, u32& stop) noexcept {
    start = stop = p;
    while (p < end) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, end, f, t))
            return false;
        if (f == 1) {
            if (t != 2 || !proto_length(w, p, end, stop))
                return false;
            start = p;
            p = stop;
        } else if (!proto_skip(w, p, end, t, f))
            return false;
    }
    return true;
}
struct Counts {
    u32 folders, guilds, positions;
};
// Repeated fixed64 accepts packed (LEN) and unpacked (I64) encodings.
bool ids(const Wire& w, u32& p, u32 end, u32 type, u64* output, u32 capacity, u32& count) noexcept {
    if (type == 1) {
        if (end - p < 8 || count >= capacity)
            return false;
        if (output)
            output[count] = proto_fixed64(w, p);
        ++count;
        p += 8;
        return true;
    }
    u32 stop = 0;
    if (type != 2 || !proto_length(w, p, end, stop) || (stop - p) % 8 || (stop - p) / 8 > capacity - count)
        return false;
    for (; p < stop; p += 8, ++count)
        if (output)
            output[count] = proto_fixed64(w, p);
    return true;
}
void copy_name(const Wire& w, u32 start, u32 stop, GuildFolderRecord& record) noexcept {
    u32 bytes = minimum(stop - start, kGuildFolderNameBytes);
    // Truncate at a code point boundary; invalid UTF-8 names fall back to empty.
    while (bytes && bytes < stop - start && (w.at(start + bytes) & 0xC0) == 0x80)
        --bytes;
    char text[kGuildFolderNameBytes];
    for (u32 i = 0; i < bytes; ++i)
        text[i] = static_cast<char>(w.at(start + i));
    if (!utf8_validate({text, bytes}).valid)
        bytes = 0;
    copy_bytes(record.name, text, bytes);
    record.name_bytes = static_cast<u8>(bytes);
}
bool folder(const Wire& w, u32 p, u32 end, GuildFolderSettings* out, Counts& counts) noexcept {
    GuildFolderRecord record{};
    record.first_guild = static_cast<u16>(counts.guilds);
    while (p < end) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, end, f, t))
            return false;
        if (f == 1) {
            if (!ids(w, p, end, t, out ? out->guild_ids : nullptr, kGuildFoldersMaximumGuilds, counts.guilds))
                return false;
            continue;
        }
        if (f < 2 || f > 4) {
            if (!proto_skip(w, p, end, t, f))
                return false;
            continue;
        }
        u32 stop = 0;
        if (t != 2 || !proto_length(w, p, end, stop))
            return false;
        if (f == 3) {
            u32 a = 0, b = 0;
            if (!wrapper_string(w, p, stop, a, b))
                return false;
            if (out)
                copy_name(w, a, b, record);
        } else {
            u64 value = 0;
            if (!wrapper_varint(w, p, stop, value))
                return false;
            if (f == 2) {
                record.id = value;
                record.has_id = true;
            } else {
                record.color = static_cast<u32>(value & 0xFFFFFF);
                record.has_color = value <= 0xFFFFFF && value != kGuildFolderDefaultColor;
            }
        }
        p = stop;
    }
    record.guild_count = static_cast<u16>(counts.guilds - record.first_guild);
    if (counts.folders >= kGuildFoldersMaximumFolders)
        return false;
    if (out)
        out->folders[counts.folders] = record;
    ++counts.folders;
    return true;
}
bool group(const Wire& w, u32 p, u32 end, GuildFolderSettings* out, Counts& counts) noexcept {
    while (p < end) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, end, f, t))
            return false;
        if (f == 1) {
            u32 stop = 0;
            if (t != 2 || !proto_length(w, p, end, stop) || !folder(w, p, stop, out, counts))
                return false;
            p = stop;
        } else if (f == 2) {
            if (!ids(w, p, end, t, out ? out->positions : nullptr, kGuildFoldersMaximumPositions, counts.positions))
                return false;
        } else if (!proto_skip(w, p, end, t, f))
            return false;
    }
    return true;
}
E extract(const Wire& w, Slice<u8> output, u32* bytes, bool* present) noexcept {
    u32 total = 0;
    bool found = false;
    for (u32 p = 0; p < w.size;) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, w.size, f, t))
            return E::protobuf;
        if (f != kGuildFoldersSettingsField) {
            if (!proto_skip(w, p, w.size, t, f))
                return E::protobuf;
            continue;
        }
        u32 stop = 0;
        if (t != 2 || !proto_length(w, p, w.size, stop))
            return E::protobuf;
        found = true;
        if (stop - p > kGuildFoldersMaximumGroupBytes - total)
            return E::capacity;
        total += stop - p;
        p = stop;
    }
    if (total > output.count || (total && !output.data))
        return E::capacity;
    u32 written = 0;
    for (u32 p = 0; p < w.size;) {
        u32 f = 0, t = 0, stop = 0;
        proto_tag(w, p, w.size, f, t);
        if (f != kGuildFoldersSettingsField) {
            proto_skip(w, p, w.size, t, f);
            continue;
        }
        proto_length(w, p, w.size, stop);
        while (p < stop)
            output.data[written++] = w.at(p++);
    }
    *bytes = total;
    *present = found;
    return E::none;
}
bool output_valid(Slice<u8> output) noexcept {
    return byte_range_valid({output.data, output.count});
}

// Encoded sizes of wrapper messages {value = 1}.
u32 varint_wrapper_size(u64 value) noexcept {
    return value ? 1 + proto_varint_size(value) : 0;
}
u32 folder_size(const GuildFolderRecord& f) noexcept {
    u32 size = 0;
    if (f.guild_count)
        size += 1 + proto_varint_size(8u * f.guild_count) + 8u * f.guild_count;
    if (f.has_id) {
        const u32 inner = varint_wrapper_size(f.id);
        size += 1 + proto_varint_size(inner) + inner;
    }
    if (f.name_bytes) {
        const u32 inner = 1 + proto_varint_size(f.name_bytes) + f.name_bytes;
        size += 1 + proto_varint_size(inner) + inner;
    }
    if (f.has_color) {
        const u32 inner = varint_wrapper_size(f.color);
        size += 1 + proto_varint_size(inner) + inner;
    }
    return size;
}
void varint_wrapper(ProtoWriter& out, u32 field, u64 value) noexcept {
    out.tag(field, 2);
    out.varint(varint_wrapper_size(value));
    if (value) {
        out.tag(1, 0);
        out.varint(value);
    }
}
bool settings_valid(const GuildFolderSettings& s) noexcept {
    if (s.folder_count > kGuildFoldersMaximumFolders || s.guild_id_count > kGuildFoldersMaximumGuilds ||
        s.position_count > kGuildFoldersMaximumPositions)
        return false;
    for (u32 i = 0; i < s.folder_count; ++i) {
        const auto& f = s.folders[i];
        if (f.first_guild > s.guild_id_count || f.guild_count > s.guild_id_count - f.first_guild ||
            f.name_bytes > kGuildFolderNameBytes || !utf8_validate({f.name, f.name_bytes}).valid ||
            (f.has_color && f.color > 0xFFFFFF))
            return false;
    }
    return true;
}
} // namespace

u32 guild_folders_encode(const GuildFolderSettings& settings, Slice<u8> output) noexcept {
    if (!output.data || !output_valid(output) || !settings_valid(settings))
        return 0;
    ProtoWriter out{output, 0, true};
    for (u32 i = 0; i < settings.folder_count && out.ok; ++i) {
        const auto& f = settings.folders[i];
        out.tag(1, 2);
        out.varint(folder_size(f));
        if (f.guild_count) {
            out.tag(1, 2);
            out.varint(8u * f.guild_count);
            for (u32 g = 0; g < f.guild_count; ++g)
                out.fixed64(settings.guild_ids[f.first_guild + g]);
        }
        if (f.has_id)
            varint_wrapper(out, 2, f.id);
        if (f.name_bytes) {
            out.tag(3, 2);
            out.varint(1 + proto_varint_size(f.name_bytes) + f.name_bytes);
            out.tag(1, 2);
            out.varint(f.name_bytes);
            out.bytes(f.name, f.name_bytes);
        }
        if (f.has_color)
            varint_wrapper(out, 4, f.color);
    }
    if (settings.position_count) {
        out.tag(2, 2);
        out.varint(8u * settings.position_count);
        for (u32 p = 0; p < settings.position_count; ++p)
            out.fixed64(settings.positions[p]);
    }
    return out.ok ? out.at : 0;
}

u32 guild_folders_encode_settings_patch(const GuildFolderSettings& settings, Slice<u8> output) noexcept {
    if (!output.data || !output_valid(output) || output.count < 4)
        return 0;
    // Field 14 tag (0x72), a varint length of up to 3 bytes, then the group.
    const Slice<u8> body{output.data + 4, output.count - 4};
    const u32 bytes = guild_folders_encode(settings, body);
    if (!bytes && settings.folder_count + settings.position_count)
        return 0;
    const u32 header = 1 + proto_varint_size(bytes);
    if (header > 4 || bytes > output.count - header)
        return 0;
    // Close the gap between the reserved header and the group.
    output[0] = static_cast<u8>((kGuildFoldersSettingsField << 3) | 2);
    ProtoWriter length{{output.data + 1, 3}, 0, true};
    length.varint(bytes);
    for (u32 i = 0; i < bytes; ++i)
        output[header + i] = output[4 + i];
    return header + bytes;
}

GuildFoldersError guild_folders_extract_base64(Utf8View settings, Slice<u8> group, u32* bytes,
                                               bool* present) noexcept {
    if (!bytes || !present || settings.bytes > base64_encoded_size(kUserSettingsProtoMaximumProtoBytes) ||
        !output_valid(group) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(settings.data), settings.bytes}, {group.data, group.count}))
        return E::input;
    const auto valid = base64_decode(settings, {});
    if (!valid.ok() && valid.error != Base64Error::output_too_small)
        return E::base64;
    return extract({{}, settings, valid.bytes}, group, bytes, present);
}

GuildFoldersError guild_folders_decode(ConstSlice<u8> input, GuildFolderSettings* output) noexcept {
    if (!output || input.count > kGuildFoldersMaximumGroupBytes || !byte_range_valid(input))
        return E::input;
    const Wire w{input, {}, input.count};
    Counts counts{};
    // Validate everything first so a rejected group cannot half-replace output.
    if (!group(w, 0, w.size, nullptr, counts))
        return counts.folders >= kGuildFoldersMaximumFolders || counts.guilds >= kGuildFoldersMaximumGuilds
                   ? E::capacity
                   : E::protobuf;
    counts = {};
    group(w, 0, w.size, output, counts);
    output->folder_count = counts.folders;
    output->guild_id_count = counts.guilds;
    output->position_count = counts.positions;
    output->present = true;
    return E::none;
}

} // namespace maicord
