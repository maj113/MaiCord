#include "maicord/ui/widgets/qr_code.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

inline constexpr u32 kMaximumModules = kQrCodeMaximumSize * kQrCodeMaximumSize;
inline constexpr u32 kMaximumCodewords = 346;
inline constexpr u32 kMaximumDataCodewords = 216;
inline constexpr u32 kMaximumErrorPerBlock = 26;
inline constexpr u32 kMaximumBlocks = 5;

// Level M of versions 1 to 10 (ISO/IEC 18004 table 9): every codeword, the error codewords of each block and the
// blocks. Blocks share the codewords evenly; the last `codewords % blocks` hold one data codeword more.
struct Version {
    u16 codewords;
    u8 error_per_block, blocks;
};
constexpr Version kVersions[kQrCodeMaximumVersion]{{26, 10, 1},  {44, 16, 1},  {70, 26, 1},  {100, 18, 2},
                                                   {134, 24, 2}, {172, 16, 4}, {196, 18, 4}, {242, 22, 4},
                                                   {292, 22, 5}, {346, 26, 5}};
constexpr u32 data_codewords(const Version& version) noexcept {
    return version.codewords - version.error_per_block * version.blocks;
}
static_assert(data_codewords(kVersions[4]) == 86 && data_codewords(kVersions[9]) == kMaximumDataCodewords);

struct BitBuffer {
    u8 data[kMaximumDataCodewords];
    u32 bits;
};

// The payload is checked against the version's capacity first, so every call stays within its data codewords.
void append_bits(BitBuffer* buffer, u32 value, u32 count) noexcept {
    for (u32 remaining = count; remaining != 0; --remaining) {
        const bool bit = ((value >> (remaining - 1)) & 1u) != 0;
        const u32 index = buffer->bits++;
        if (bit) {
            buffer->data[index >> 3] |=
                static_cast<u8>(1u << (7u - (index & 7u)));
        }
    }
}

constexpr u8 finite_field_multiply(u8 first, u8 second) noexcept {
    u8 result = 0;
    for (u32 bit = 0; bit < 8; ++bit) {
        result ^= static_cast<u8>((second & 1u) != 0 ? 0xFFu : 0u) & first;
        const bool high = (first & 0x80u) != 0;
        first = static_cast<u8>(first << 1);
        if (high) {
            first ^= 0x1Du;
        }
        second >>= 1;
    }
    return result;
}

// The generator polynomial of `degree` error codewords, from the roots 2^0 .. 2^(degree-1).
void make_divisor(u32 degree, u8 divisor[kMaximumErrorPerBlock]) noexcept {
    for (u32 index = 0; index < degree; ++index) {
        divisor[index] = 0;
    }
    divisor[degree - 1] = 1;
    u8 root = 1;
    for (u32 step = 0; step < degree; ++step) {
        for (u32 index = 0; index < degree; ++index) {
            divisor[index] = finite_field_multiply(divisor[index], root);
            if (index + 1 < degree) {
                divisor[index] ^= divisor[index + 1];
            }
        }
        root = finite_field_multiply(root, 2);
    }
}

void make_remainder(const u8* data, u32 data_count, const u8* divisor, u32 degree, u8* remainder) noexcept {
    for (u32 index = 0; index < degree; ++index) {
        remainder[index] = 0;
    }
    for (u32 data_index = 0; data_index < data_count; ++data_index) {
        const u8 factor = data[data_index] ^ remainder[0];
        for (u32 index = 0; index + 1 < degree; ++index) {
            remainder[index] = remainder[index + 1];
        }
        remainder[degree - 1] = 0;
        for (u32 index = 0; index < degree; ++index) {
            remainder[index] ^= finite_field_multiply(divisor[index], factor);
        }
    }
}

// One symbol under construction: its side, its modules and which of them are function patterns.
struct Grid {
    u32 size;
    u8 modules[kMaximumModules];
    u8 functions[kMaximumModules];
};

u32 module_index(u32 size, u32 x, u32 y) noexcept {
    return y * size + x;
}

void set_function(Grid* grid, i32 x, i32 y, bool black) noexcept {
    if (x < 0 || y < 0 || x >= static_cast<i32>(grid->size) || y >= static_cast<i32>(grid->size)) {
        return;
    }
    const u32 index = module_index(grid->size, static_cast<u32>(x), static_cast<u32>(y));
    grid->modules[index] = black ? 1 : 0;
    grid->functions[index] = 1;
}

void draw_finder(Grid* grid, i32 center_x, i32 center_y) noexcept {
    for (i32 y = -4; y <= 4; ++y) {
        for (i32 x = -4; x <= 4; ++x) {
            const i32 distance = maximum(absolute_value(x), absolute_value(y));
            set_function(grid, center_x + x, center_y + y, distance != 2 && distance != 4);
        }
    }
}

void draw_alignment(Grid* grid, i32 center_x, i32 center_y) noexcept {
    for (i32 y = -2; y <= 2; ++y) {
        for (i32 x = -2; x <= 2; ++x) {
            set_function(grid, center_x + x, center_y + y, maximum(absolute_value(x), absolute_value(y)) != 1);
        }
    }
}

bool format_bit(u32 value, u32 bit) noexcept {
    return ((value >> bit) & 1u) != 0;
}

void draw_format(Grid* grid, u32 mask) noexcept {
    // Error-correction level M has format bits 00.
    const u32 data = mask;
    u32 remainder = data;
    for (u32 index = 0; index < 10; ++index) {
        remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537u);
    }
    const u32 bits = ((data << 10) | remainder) ^ 0x5412u;
    const i32 size = static_cast<i32>(grid->size);
    for (u32 index = 0; index <= 5; ++index) {
        set_function(grid, 8, static_cast<i32>(index), format_bit(bits, index));
    }
    set_function(grid, 8, 7, format_bit(bits, 6));
    set_function(grid, 8, 8, format_bit(bits, 7));
    set_function(grid, 7, 8, format_bit(bits, 8));
    for (u32 index = 9; index < 15; ++index) {
        set_function(grid, static_cast<i32>(14 - index), 8, format_bit(bits, index));
    }
    for (u32 index = 0; index < 8; ++index) {
        set_function(grid, size - 1 - static_cast<i32>(index), 8, format_bit(bits, index));
    }
    for (u32 index = 8; index < 15; ++index) {
        set_function(grid, 8, size - 15 + static_cast<i32>(index), format_bit(bits, index));
    }
    set_function(grid, 8, size - 8, true);
}

// Versions 7 and up carry their number twice, in 6 x 3 blocks beside the upper-right and lower-left finders.
void draw_version(Grid* grid, u32 version) noexcept {
    if (version < 7) {
        return;
    }
    u32 remainder = version;
    for (u32 index = 0; index < 12; ++index) {
        remainder = (remainder << 1) ^ ((remainder >> 11) * 0x1F25u);
    }
    const u32 bits = (version << 12) | remainder;
    for (u32 index = 0; index < 18; ++index) {
        const bool black = format_bit(bits, index);
        const i32 a = static_cast<i32>(grid->size) - 11 + static_cast<i32>(index % 3);
        const i32 b = static_cast<i32>(index / 3);
        set_function(grid, a, b, black);
        set_function(grid, b, a, black);
    }
}

void draw_functions(Grid* grid, u32 version) noexcept {
    const u32 size = grid->size;
    for (u32 index = 0; index < size; ++index) {
        set_function(grid, 6, static_cast<i32>(index), (index & 1u) == 0);
        set_function(grid, static_cast<i32>(index), 6, (index & 1u) == 0);
    }
    draw_finder(grid, 3, 3);
    draw_finder(grid, static_cast<i32>(size - 4), 3);
    draw_finder(grid, 3, static_cast<i32>(size - 4));
    // Alignment patterns sit on every pair of these centres but the finders' corners: none in version 1, the far
    // corner up to version 6, a 3 x 3 grid from version 7.
    if (version >= 2) {
        i32 centres[3]{6, 0, static_cast<i32>(size) - 7};
        u32 count = 2;
        if (version >= 7) {
            centres[1] = (6 + centres[2]) / 2;
            count = 3;
        } else {
            centres[1] = centres[2];
        }
        for (u32 row = 0; row < count; ++row) {
            for (u32 column = 0; column < count; ++column) {
                const bool corner = (row == 0 && column == 0) || (row == 0 && column == count - 1) ||
                                    (row == count - 1 && column == 0);
                if (!corner) {
                    draw_alignment(grid, centres[column], centres[row]);
                }
            }
        }
    }
    draw_format(grid, 0);
    draw_version(grid, version);
}

bool mask_value(u32 mask, u32 x, u32 y) noexcept {
    switch (mask) {
    case 0:
        return ((x + y) & 1u) == 0;
    case 1:
        return (y & 1u) == 0;
    case 2:
        return x % 3u == 0;
    case 3:
        return (x + y) % 3u == 0;
    case 4:
        return (x / 3u + y / 2u) % 2u == 0;
    case 5:
        return x * y % 2u + x * y % 3u == 0;
    case 6:
        return (x * y % 2u + x * y % 3u) % 2u == 0;
    default:
        return ((x + y) % 2u + x * y % 3u) % 2u == 0;
    }
}

void apply_mask(Grid* grid, u32 mask) noexcept {
    for (u32 y = 0; y < grid->size; ++y) {
        for (u32 x = 0; x < grid->size; ++x) {
            const u32 index = module_index(grid->size, x, y);
            if (grid->functions[index] == 0 && mask_value(mask, x, y)) {
                grid->modules[index] ^= 1;
            }
        }
    }
}

void draw_codewords(Grid* grid, const u8* codewords, u32 count) noexcept {
    const u32 size = grid->size;
    u32 bit_index = 0;
    bool upward = true;
    for (i32 right = static_cast<i32>(size) - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5;
        }
        for (u32 vertical = 0; vertical < size; ++vertical) {
            const u32 y = upward ? size - 1 - vertical : vertical;
            for (u32 column = 0; column < 2; ++column) {
                const u32 x = static_cast<u32>(right) - column;
                const u32 index = module_index(size, x, y);
                if (grid->functions[index] != 0) {
                    continue;
                }
                // The remainder bits after the last codeword stay light.
                bool black = false;
                if (bit_index < count * 8) {
                    black = ((codewords[bit_index >> 3] >> (7u - (bit_index & 7u))) & 1u) != 0;
                }
                grid->modules[index] = black ? 1 : 0;
                ++bit_index;
            }
        }
        upward = !upward;
    }
}

void penalty_add_history(u32 size, u32 run, u32 history[7]) noexcept {
    if (history[0] == 0) {
        run += size;
    }
    for (u32 index = 6; index != 0; --index) {
        history[index] = history[index - 1];
    }
    history[0] = run;
}

u32 penalty_count_patterns(const u32 history[7]) noexcept {
    const u32 unit = history[1];
    const bool core = unit > 0 && history[2] == unit &&
                      history[3] == unit * 3 && history[4] == unit &&
                      history[5] == unit;
    return (core && history[0] >= unit * 4 && history[6] >= unit ? 1u
                                                                 : 0u) +
           (core && history[6] >= unit * 4 && history[0] >= unit ? 1u
                                                                 : 0u);
}

u32 penalty_terminate(u32 size, bool color, u32 run, u32 history[7]) noexcept {
    if (color) {
        penalty_add_history(size, run, history);
        run = 0;
    }
    run += size;
    penalty_add_history(size, run, history);
    return penalty_count_patterns(history);
}

u32 penalty(const Grid& grid) noexcept {
    const u32 size = grid.size;
    const u32 modules = size * size;
    u32 score = 0;
    for (u32 axis = 0; axis < 2; ++axis) {
        for (u32 line = 0; line < size; ++line) {
            bool previous = false;
            u32 run = 0;
            u32 history[7]{};
            for (u32 offset = 0; offset < size; ++offset) {
                const u32 x = axis == 0 ? offset : line;
                const u32 y = axis == 0 ? line : offset;
                const bool black = grid.modules[module_index(size, x, y)] != 0;
                if (black == previous) {
                    ++run;
                    if (run == 5) {
                        score += 3;
                    } else if (run > 5) {
                        ++score;
                    }
                } else {
                    penalty_add_history(size, run, history);
                    if (!previous) {
                        score += penalty_count_patterns(history) * 40;
                    }
                    previous = black;
                    run = 1;
                }
            }
            score += penalty_terminate(size, previous, run, history) * 40;
        }
    }
    for (u32 y = 0; y + 1 < size; ++y) {
        for (u32 x = 0; x + 1 < size; ++x) {
            const u8 color = grid.modules[module_index(size, x, y)];
            if (grid.modules[module_index(size, x + 1, y)] == color &&
                grid.modules[module_index(size, x, y + 1)] == color &&
                grid.modules[module_index(size, x + 1, y + 1)] == color) {
                score += 3;
            }
        }
    }
    u32 black = 0;
    for (u32 index = 0; index < modules; ++index) {
        black += grid.modules[index] != 0;
    }
    const u32 balance = black * 20u > modules * 10u ? black * 20u - modules * 10u : modules * 10u - black * 20u;
    const u32 quotient = (balance + modules - 1) / modules;
    score += (quotient == 0 ? 0 : quotient - 1) * 10u;
    return score;
}

bool valid_size(u32 size) noexcept {
    return size >= 21 && size <= kQrCodeMaximumSize && (size - 17) % 4 == 0;
}

// Bit y * size + x of the packed modules, inside the symbol.
bool packed_module(const u8* modules, u32 size, u32 x, u32 y) noexcept {
    const u32 index = module_index(size, x, y);
    return ((modules[index >> 3] >> (index & 7u)) & 1u) != 0;
}

} // namespace

bool qr_code_encode_m(ConstSlice<u8> payload, u32 minimum_version, u32 maximum_size, u8* modules, u8* size,
                      u8* mask) noexcept {
    // Only the QrCodeOf overloads call this: their pointers are the symbol's own and their size is a checked constant.
    const u32 maximum_version = (maximum_size - 17) / 4;
    if (!payload.data || payload.count == 0 || minimum_version == 0 || minimum_version > maximum_version) {
        return false;
    }
    // The smallest version whose data codewords hold the mode, the count and the bytes.
    u32 version = minimum_version;
    for (; version <= maximum_version; ++version) {
        const u32 count_bits = version < 10 ? 8 : 16;
        if (4 + count_bits + payload.count * 8 <= data_codewords(kVersions[version - 1]) * 8) {
            break;
        }
    }
    if (version > maximum_version) {
        return false;
    }
    const Version& shape = kVersions[version - 1];
    const u32 data_count = data_codewords(shape);
    BitBuffer bits{};
    append_bits(&bits, 0x4, 4);
    append_bits(&bits, payload.count, version < 10 ? 8 : 16);
    for (u32 index = 0; index < payload.count; ++index) {
        append_bits(&bits, payload[index], 8);
    }
    append_bits(&bits, 0, minimum(data_count * 8 - bits.bits, 4u));
    while ((bits.bits & 7u) != 0) {
        append_bits(&bits, 0, 1);
    }
    for (bool alternate = false; bits.bits < data_count * 8; alternate = !alternate) {
        append_bits(&bits, alternate ? 0x11u : 0xECu, 8);
    }

    // Split into blocks (short ones first), add each block's error codewords, then interleave.
    const u32 blocks = shape.blocks;
    const u32 error_count = shape.error_per_block;
    const u32 short_blocks = blocks - shape.codewords % blocks;
    const u32 short_data = shape.codewords / blocks - error_count;
    u8 divisor[kMaximumErrorPerBlock];
    make_divisor(error_count, divisor);
    u8 error[kMaximumBlocks][kMaximumErrorPerBlock]{};
    u32 block_start[kMaximumBlocks]{};
    for (u32 block = 0, at = 0; block < blocks; ++block) {
        const u32 length = short_data + (block < short_blocks ? 0u : 1u);
        block_start[block] = at;
        make_remainder(bits.data + at, length, divisor, error_count, error[block]);
        at += length;
    }
    u8 codewords[kMaximumCodewords]{};
    u32 codeword_count = 0;
    for (u32 index = 0; index <= short_data; ++index) {
        for (u32 block = 0; block < blocks; ++block) {
            if (index < short_data || block >= short_blocks) {
                codewords[codeword_count++] = bits.data[block_start[block] + index];
            }
        }
    }
    for (u32 index = 0; index < error_count; ++index) {
        for (u32 block = 0; block < blocks; ++block) {
            codewords[codeword_count++] = error[block][index];
        }
    }

    // About 6.5 KB: kept off the caller's frame.
    static Grid grid;
    reset_value(&grid);
    grid.size = 17 + 4 * version;
    draw_functions(&grid, version);
    draw_codewords(&grid, codewords, codeword_count);
    u32 best_mask = 0;
    u32 best_penalty = ~0u;
    for (u32 candidate = 0; candidate < 8; ++candidate) {
        apply_mask(&grid, candidate);
        draw_format(&grid, candidate);
        const u32 current = penalty(grid);
        if (current < best_penalty) {
            best_penalty = current;
            best_mask = candidate;
        }
        apply_mask(&grid, candidate);
    }
    apply_mask(&grid, best_mask);
    draw_format(&grid, best_mask);

    clear_bytes(modules, (maximum_size * maximum_size + 7) / 8);
    for (u32 index = 0; index < grid.size * grid.size; ++index) {
        if (grid.modules[index] != 0) {
            modules[index >> 3] |= static_cast<u8>(1u << (index & 7u));
        }
    }
    *size = static_cast<u8>(grid.size);
    *mask = static_cast<u8>(best_mask);
    return true;
}

bool qr_code_module(QrCodeModules code, u32 x, u32 y) noexcept {
    return code.modules && valid_size(code.size) && x < code.size && y < code.size &&
           packed_module(code.modules, code.size, x, y);
}

namespace {

u32 compact_rect_pass(QrCodeModules code, const Slice<QrCodeRect>& output,
                      bool write) noexcept {
    constexpr u32 kMaximumRunsPerRow = (kQrCodeMaximumSize + 1) / 2;
    const u32 size = code.size;
    QrCodeRect active[kMaximumRunsPerRow]{};
    u32 active_count = 0;
    u32 required = 0;
    for (u32 y = 0; y < size; ++y) {
        QrCodeRect next[kMaximumRunsPerRow]{};
        bool matched[kMaximumRunsPerRow]{};
        u32 next_count = 0;
        u32 x = 0;
        while (x < size) {
            while (x < size && !packed_module(code.modules, size, x, y)) {
                ++x;
            }
            if (x == size) {
                break;
            }
            const u32 left = x;
            while (x < size && packed_module(code.modules, size, x, y)) {
                ++x;
            }
            QrCodeRect rectangle{static_cast<u8>(left),
                                 static_cast<u8>(y),
                                 static_cast<u8>(x),
                                 static_cast<u8>(y + 1)};
            for (u32 index = 0; index < active_count; ++index) {
                if (!matched[index] && active[index].left == rectangle.left &&
                    active[index].right == rectangle.right) {
                    rectangle = active[index];
                    rectangle.bottom = static_cast<u8>(y + 1);
                    matched[index] = true;
                    break;
                }
            }
            next[next_count++] = rectangle;
        }
        for (u32 index = 0; index < active_count; ++index) {
            if (matched[index]) {
                continue;
            }
            if (write) {
                output[required] = active[index];
            }
            ++required;
        }
        active_count = next_count;
        for (u32 index = 0; index < next_count; ++index) {
            active[index] = next[index];
        }
    }
    for (u32 index = 0; index < active_count; ++index) {
        if (write) {
            output[required] = active[index];
        }
        ++required;
    }
    return required;
}

} // namespace

QrCodeRectResult qr_code_compact_rects(QrCodeModules code, Slice<QrCodeRect> output) noexcept {
    if (!code.modules || !valid_size(code.size) || (!output.data && output.count != 0)) {
        return QrCodeRectResult{0, true};
    }
    const u32 required = compact_rect_pass(code, Slice<QrCodeRect>{}, false);
    if (required > output.count) {
        return QrCodeRectResult{required, true};
    }
    compact_rect_pass(code, output, true);
    return QrCodeRectResult{required, false};
}

bool qr_code_paint(PaintStream* stream, const UiRect& area, QrCodeModules code, float dpi_scale, float quiet_dip,
                   Color dark) noexcept {
    // A version 10 symbol coalesces to well under this many runs.
    QrCodeRect rectangles[1024];
    const QrCodeRectResult compacted =
        qr_code_compact_rects(code, Slice<QrCodeRect>{rectangles, static_cast<u32>(sizeof(rectangles) / sizeof(rectangles[0]))});
    const i32 modules = static_cast<i32>(code.size);
    const i32 left_px = static_cast<i32>(area.left * dpi_scale + 0.5f);
    const i32 top_px = static_cast<i32>(area.top * dpi_scale + 0.5f);
    const i32 extent_px = static_cast<i32>((area.right - area.left) * dpi_scale + 0.5f);
    const i32 quiet_px = static_cast<i32>(quiet_dip * dpi_scale + 0.5f);
    const i32 module_px = modules ? (extent_px - 2 * quiet_px) / modules : 0;
    if (!stream || !compacted.ok() || module_px <= 0 || dpi_scale <= 0) {
        return false;
    }
    const i32 symbol_px = module_px * modules;
    const i32 origin_x_px = left_px + (extent_px - symbol_px) / 2;
    const i32 origin_y_px = top_px + (extent_px - symbol_px) / 2;
    const float dip_per_pixel = 1.0f / dpi_scale;
    for (u32 index = 0; index < compacted.count; ++index) {
        const QrCodeRect rectangle = rectangles[index];
        const float left = static_cast<float>(origin_x_px + rectangle.left * module_px) * dip_per_pixel;
        const float top = static_cast<float>(origin_y_px + rectangle.top * module_px) * dip_per_pixel;
        const float right = static_cast<float>(origin_x_px + rectangle.right * module_px) * dip_per_pixel;
        const float bottom = static_cast<float>(origin_y_px + rectangle.bottom * module_px) * dip_per_pixel;
        if (!paint_stream_push(stream, left, top, right, bottom, dark)) {
            return false;
        }
    }
    return true;
}

} // namespace maicord
