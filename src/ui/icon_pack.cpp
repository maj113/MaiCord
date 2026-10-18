#include "maicord/ui/icon_pack.hpp"

#include "maicord/media/deflate.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
#include "ui_icon_pack.inc"

constexpr u32 kHeaderBytes = 40;
constexpr u32 kRecordBytes = 16;
constexpr u32 kStemBytes = 4;
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
constexpr u32 kBuiltinIconCount = read_u16(kUiIconPackData + 6);
IconPack g_builtin;
volatile LONG builtin_state; // 0 packed, 1 inflating, 2 published

struct Header {
    u32 commands, stems;
};
Header header(const IconPack& pack) noexcept {
    return {read_u32(pack.data + 24), read_u32(pack.data + 28)};
}

struct Record {
    u32 command_offset;
    u16 command_bytes, outline_count, stem_first;
    u8 stem_count;
};
Record record(const IconPack& pack, u32 index) noexcept {
    const u8* p = pack.data + kHeaderBytes + index * kRecordBytes;
    return {read_u32(p), read_u16(p + 4), read_u16(p + 6), read_u16(p + 8), p[10]};
}
} // namespace

// The built-in pack ships zlib-compressed and is inflated once, on first use,
// into private pages kept for the process lifetime. Allocation or inflation
// failure leaves an empty pack.
const IconPack& icon_pack_builtin() noexcept {
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

} // namespace maicord
