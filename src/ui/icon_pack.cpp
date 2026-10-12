#include "maicord/ui/icon_pack.hpp"

#include "maicord/media/deflate.hpp"

#include <Windows.h>

namespace maicord {
namespace {
#include "ui_icon_pack.inc"

constexpr u32 kHeaderBytes = 40;
constexpr u32 kRecordBytes = 16;
constexpr u32 kStemBytes = 4;
constexpr u32 kMaximumNameBytes = 32;
constexpr i32 kCoordinateMinimum = -4 * static_cast<i32>(kIconPackUnits);
constexpr i32 kCoordinateMaximum = 28 * static_cast<i32>(kIconPackUnits);
enum : u8 { op_move_stroke, op_move_fill, op_line, op_cubic, op_close };

constexpr u16 read_u16(const u8* p) noexcept {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
constexpr u32 read_u32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
constexpr i32 read_i16(const u8* p) noexcept {
    return static_cast<i16>(read_u16(p));
}

static_assert(sizeof(kUiIconPackData) > kHeaderBytes && kUiIconPackData[0] == 'M' &&
              kUiIconPackData[1] == 'I' && kUiIconPackData[2] == 'P' && kUiIconPackData[3] == 'K');
// The built-in pack ships zlib-compressed and is inflated once, on first use,
// into private pages kept for the process lifetime. Allocation or inflation
// failure leaves an empty pack.
constexpr u32 kBuiltinIconCount = read_u16(kUiIconPackData + 6);
IconPack g_builtin;
volatile LONG builtin_state; // 0 packed, 1 inflating, 2 published
const IconPack& builtin() noexcept {
    for (;;) {
        const LONG state = builtin_state;
        if (state == 2)
            return g_builtin;
        if (state == 0 && InterlockedCompareExchange(&builtin_state, 1, 0) == 0) {
            constexpr u32 bytes = sizeof(kUiIconPackData);
            auto* data = static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            DeflateWorkspace workspace;
            const DeflateInputSpan input{kUiIconPackPacked, sizeof(kUiIconPackPacked)};
            if (data && zlib_inflate({&input, 1}, {data, bytes}, bytes, &workspace).ok()) {
                g_builtin = {data, bytes, kBuiltinIconCount};
            } else if (data) {
                VirtualFree(data, 0, MEM_RELEASE);
            }
            builtin_state = 2;
            return g_builtin;
        }
        YieldProcessor();
    }
}

struct Header {
    u32 count, records, index, commands, stems, names, size;
};
Header header(const IconPack& pack) noexcept {
    const u8* p = pack.data;
    return {pack.icon_count, read_u32(p + 16), read_u32(p + 20), read_u32(p + 24),
            read_u32(p + 28), read_u32(p + 32), pack.size};
}

struct Record {
    u32 command_offset;
    u16 command_bytes, outline_count, stem_first;
    u8 stem_count, flags;
    u16 name_offset;
    u8 name_length;
};
Record record(const IconPack& pack, u32 index) noexcept {
    const u8* p = pack.data + kHeaderBytes + index * kRecordBytes;
    return {read_u32(p), read_u16(p + 4), read_u16(p + 6), read_u16(p + 8), p[10], p[11], read_u16(p + 12),
            p[14]};
}

Utf8View name_at(const IconPack& pack, const Record& r) noexcept {
    const Header h = header(pack);
    return {reinterpret_cast<const char*>(pack.data + h.names + r.name_offset), r.name_length};
}

i32 compare(Utf8View a, Utf8View b) noexcept {
    const u32 shared = minimum(a.bytes, b.bytes);
    for (u32 i = 0; i < shared; ++i)
        if (a.data[i] != b.data[i])
            return static_cast<u8>(a.data[i]) < static_cast<u8>(b.data[i]) ? -1 : 1;
    return a.bytes == b.bytes ? 0 : a.bytes < b.bytes ? -1 : 1;
}

bool coordinate_valid(i32 value) noexcept {
    return value >= kCoordinateMinimum && value <= kCoordinateMaximum;
}

// One walk serves validation and the decoder's command count.
bool walk_commands(const u8* p, u32 bytes, u32* outline_count, bool* closed_contour) noexcept {
    u32 count = 0, segments = 0, at = 0;
    bool open = false, closed = false, any_close = false;
    while (at < bytes) {
        const u8 op = p[at++];
        u32 coordinates = 0;
        if (op == op_move_stroke || op == op_move_fill) {
            if (open && segments == 0)
                return false;
            count += open || closed ? 1 : 0;
            open = true;
            closed = false;
            segments = 0;
            coordinates = 2;
            ++count;
        } else if (op == op_line || op == op_cubic) {
            if (!open || closed)
                return false;
            coordinates = op == op_line ? 2 : 6;
            ++segments;
            ++count;
        } else if (op == op_close) {
            if (!open || closed || segments == 0)
                return false;
            closed = any_close = true;
        } else {
            return false;
        }
        if (coordinates * 2 > bytes - at)
            return false;
        for (u32 i = 0; i < coordinates; ++i, at += 2)
            if (!coordinate_valid(read_i16(p + at)))
                return false;
    }
    if (!open || segments == 0)
        return false;
    ++count;
    *outline_count = count;
    *closed_contour = any_close;
    return count <= kIconMaximumOutlineCommands;
}

bool name_valid(Utf8View name) noexcept {
    if (name.bytes == 0 || name.bytes > kMaximumNameBytes)
        return false;
    for (u32 i = 0; i < name.bytes; ++i) {
        const char c = name.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

struct Overlay {
    IconPack pack;
    u32 generation;
    bool active;
};
Overlay g_overlay{};
} // namespace

IconPackError icon_pack_open(ConstSlice<u8> bytes, IconPack* output) noexcept {
    if (!output || !bytes.data)
        return IconPackError::invalid_input;
    const u8* p = bytes.data;
    if (bytes.count < kHeaderBytes || p[0] != 'M' || p[1] != 'I' || p[2] != 'P' || p[3] != 'K' ||
        read_u16(p + 4) != 1 || read_u16(p + 8) != kIconPackGrid || read_u16(p + 10) != kIconPackUnits ||
        read_u32(p + 12) != bytes.count || read_u32(p + 36) != 0)
        return IconPackError::header;
    const IconPack pack{p, bytes.count, read_u16(p + 6)};
    const Header h = header(pack);
    if (h.count == 0 || h.count > kIconPackMaximumIcons)
        return IconPackError::header;
    if (h.records != kHeaderBytes || h.index != h.records + h.count * kRecordBytes ||
        h.commands != h.index + h.count * 2 || h.stems < h.commands || h.names < h.stems ||
        h.names > h.size || (h.names - h.stems) % kStemBytes != 0)
        return IconPackError::layout;
    const u32 command_bytes = h.stems - h.commands, stem_count = (h.names - h.stems) / kStemBytes,
              name_bytes = h.size - h.names;
    for (u32 i = 0; i < h.count; ++i) {
        const Record r = record(pack, i);
        if ((r.flags & ~1u) != 0 || p[kHeaderBytes + i * kRecordBytes + 15] != 0)
            return IconPackError::record;
        if (r.command_offset > command_bytes || r.command_bytes > command_bytes - r.command_offset)
            return IconPackError::commands;
        u32 outline_count = 0;
        bool closed = false;
        if (!walk_commands(p + h.commands + r.command_offset, r.command_bytes, &outline_count, &closed) ||
            outline_count != r.outline_count || closed != ((r.flags & 1u) != 0))
            return IconPackError::commands;
        if (r.stem_count > kIconMaximumStems || r.stem_first > stem_count ||
            r.stem_count > stem_count - r.stem_first)
            return IconPackError::stems;
        for (u32 s = 0; s < r.stem_count; ++s) {
            const u8* stem = p + h.stems + (r.stem_first + s) * kStemBytes;
            if (!coordinate_valid(read_i16(stem)) || stem[2] > 1)
                return IconPackError::stems;
        }
        if (r.name_offset > name_bytes || r.name_length > name_bytes - r.name_offset ||
            !name_valid(name_at(pack, r)))
            return IconPackError::names;
    }
    u8 seen[kIconPackMaximumIcons / 8]{};
    Utf8View previous{};
    for (u32 i = 0; i < h.count; ++i) {
        const u32 index = read_u16(p + h.index + i * 2);
        if (index >= h.count || (seen[index / 8] & (1u << (index % 8))))
            return IconPackError::index;
        seen[index / 8] = static_cast<u8>(seen[index / 8] | (1u << (index % 8)));
        const Utf8View name = name_at(pack, record(pack, index));
        if (i && compare(previous, name) >= 0)
            return IconPackError::index;
        previous = name;
    }
    *output = pack;
    return IconPackError::none;
}

const IconPack& icon_pack_builtin() noexcept {
    return builtin();
}

bool icon_pack_find(const IconPack& pack, Utf8View name, u32* index) noexcept {
    if (!pack.data || !index || !name.data)
        return false;
    const Header h = header(pack);
    u32 low = 0, high = h.count;
    while (low < high) {
        const u32 middle = (low + high) / 2;
        const u32 candidate = read_u16(pack.data + h.index + middle * 2);
        const i32 order = compare(name_at(pack, record(pack, candidate)), name);
        if (order == 0) {
            *index = candidate;
            return true;
        }
        if (order < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return false;
}

Utf8View icon_pack_name(const IconPack& pack, u32 index) noexcept {
    return pack.data && index < pack.icon_count ? name_at(pack, record(pack, index)) : Utf8View{};
}

u32 icon_pack_outline_count(const IconPack& pack, u32 index) noexcept {
    return pack.data && index < pack.icon_count ? record(pack, index).outline_count : 0;
}

u32 icon_pack_stems(const IconPack& pack, u32 index, IconStem* stems, u32 capacity) noexcept {
    if (!pack.data || index >= pack.icon_count || !stems)
        return 0;
    const Record r = record(pack, index);
    const u8* p = pack.data + header(pack).stems + r.stem_first * kStemBytes;
    const u32 count = minimum<u32>(r.stem_count, capacity);
    for (u32 i = 0; i < count; ++i, p += kStemBytes)
        stems[i] = {static_cast<float>(read_i16(p)) / kIconPackUnits, p[2], p[3]};
    return count;
}

bool icon_pack_decode(const IconPack& pack, u32 index, const IconOutlineTransform& transform,
                      GlyphOutline* outline) noexcept {
    if (!pack.data || index >= pack.icon_count || !outline || !outline->commands || !(transform.scale > 0))
        return false;
    const Record r = record(pack, index);
    if (r.outline_count > outline->capacity) {
        ++outline->rejected;
        return false;
    }
    const u8* p = pack.data + header(pack).commands + r.command_offset;
    const float unit = transform.scale / kIconPackUnits;
    float min_x = 3.4e38f, min_y = 3.4e38f, max_x = -3.4e38f, max_y = -3.4e38f;
    u32 count = 0, at = 0;
    bool open = false;
    GlyphPathCommandKind begin_kind{};
    const auto point = [&](float* x, float* y) noexcept {
        *x = static_cast<float>(read_i16(p + at)) * unit + transform.offset_x;
        *y = static_cast<float>(read_i16(p + at + 2)) * unit + transform.offset_y;
        at += 4;
        min_x = minimum(min_x, *x);
        min_y = minimum(min_y, *y);
        max_x = maximum(max_x, *x);
        max_y = maximum(max_y, *y);
    };
    const auto end_contour = [&](bool closed) noexcept {
        GlyphPathCommand& command = outline->commands[count++];
        command = {};
        // Filled contours close implicitly, as in SVG.
        command.kind = closed || begin_kind == GlyphPathCommandKind::begin_filled
                           ? GlyphPathCommandKind::end_closed
                           : GlyphPathCommandKind::end_open;
        open = false;
    };
    while (at < r.command_bytes) {
        const u8 op = p[at++];
        if (op == op_close) {
            end_contour(true);
            continue;
        }
        if ((op == op_move_stroke || op == op_move_fill) && open)
            end_contour(false);
        GlyphPathCommand& command = outline->commands[count++];
        command = {};
        if (op == op_move_stroke || op == op_move_fill) {
            const bool closed_follows = [&]() noexcept {
                // Scan ahead to this contour's terminator for the solid variant.
                u32 scan = at + 4;
                while (scan < r.command_bytes) {
                    const u8 next = p[scan];
                    if (next == op_close)
                        return true;
                    if (next == op_move_stroke || next == op_move_fill)
                        return false;
                    scan += next == op_line ? 5 : 13;
                }
                return false;
            }();
            begin_kind = op == op_move_fill || (transform.fill_closed && closed_follows)
                             ? GlyphPathCommandKind::begin_filled
                             : GlyphPathCommandKind::begin_hollow;
            command.kind = begin_kind;
            point(&command.x1, &command.y1);
            open = true;
        } else if (op == op_line) {
            command.kind = GlyphPathCommandKind::line;
            point(&command.x1, &command.y1);
        } else {
            command.kind = GlyphPathCommandKind::cubic;
            point(&command.x1, &command.y1);
            point(&command.x2, &command.y2);
            point(&command.x3, &command.y3);
        }
    }
    if (open)
        end_contour(false);
    outline->count = count;
    outline->minimum_x = min_x;
    outline->minimum_y = min_y;
    outline->maximum_x = max_x;
    outline->maximum_y = max_y;
    outline->fill_rule = GlyphFillRule::winding;
    outline->has_bounds = count != 0;
    return count == r.outline_count;
}

IconPackError icon_pack_set_overlay(ConstSlice<u8> bytes) noexcept {
    IconPack pack{};
    const IconPackError error = icon_pack_open(bytes, &pack);
    if (error != IconPackError::none)
        return error;
    g_overlay.pack = pack;
    g_overlay.active = true;
    ++g_overlay.generation;
    return IconPackError::none;
}

void icon_pack_clear_overlay() noexcept {
    if (!g_overlay.active)
        return;
    g_overlay.active = false;
    g_overlay.pack = {};
    ++g_overlay.generation;
}

u32 icon_pack_generation() noexcept {
    return g_overlay.generation;
}

bool icon_pack_resolve(u32 builtin_index, IconSource* source) noexcept {
    if (!source || builtin_index >= builtin().icon_count)
        return false;
    u32 index = 0;
    if (g_overlay.active &&
        icon_pack_find(g_overlay.pack, icon_pack_name(g_builtin, builtin_index), &index)) {
        *source = {&g_overlay.pack, index, true};
        return true;
    }
    *source = {&g_builtin, builtin_index, false};
    return true;
}

} // namespace maicord
