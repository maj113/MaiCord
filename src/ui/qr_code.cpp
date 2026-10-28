#include "maicord/ui/qr_code.hpp"

namespace maicord {
namespace {

inline constexpr u32 kDataCodewords = 86;
inline constexpr u32 kTotalCodewords = 134;
inline constexpr u32 kBlockDataCodewords = 43;
inline constexpr u32 kErrorCodewordsPerBlock = 24;
inline constexpr u32 kBlockCount = 2;
inline constexpr u32 kModuleCount = kQrCodeSize * kQrCodeSize;

struct BitBuffer {
    u8 data[kDataCodewords];
    u32 bits;
};

// The encoder's payload cap (84 bytes) keeps every call within the 86 data codewords.
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

u8 finite_field_multiply(u8 first, u8 second) noexcept {
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

void make_divisor(u8 divisor[kErrorCodewordsPerBlock]) noexcept {
    for (u32 index = 0; index < kErrorCodewordsPerBlock; ++index) {
        divisor[index] = 0;
    }
    divisor[kErrorCodewordsPerBlock - 1] = 1;
    u8 root = 1;
    for (u32 degree = 0; degree < kErrorCodewordsPerBlock; ++degree) {
        for (u32 index = 0; index < kErrorCodewordsPerBlock; ++index) {
            divisor[index] = finite_field_multiply(divisor[index], root);
            if (index + 1 < kErrorCodewordsPerBlock) {
                divisor[index] ^= divisor[index + 1];
            }
        }
        root = finite_field_multiply(root, 2);
    }
}

void make_remainder(const u8 data[kBlockDataCodewords],
                    const u8 divisor[kErrorCodewordsPerBlock],
                    u8 remainder[kErrorCodewordsPerBlock]) noexcept {
    for (u32 index = 0; index < kErrorCodewordsPerBlock; ++index) {
        remainder[index] = 0;
    }
    for (u32 data_index = 0; data_index < kBlockDataCodewords;
         ++data_index) {
        const u8 factor = data[data_index] ^ remainder[0];
        for (u32 index = 0; index + 1 < kErrorCodewordsPerBlock;
             ++index) {
            remainder[index] = remainder[index + 1];
        }
        remainder[kErrorCodewordsPerBlock - 1] = 0;
        for (u32 index = 0; index < kErrorCodewordsPerBlock; ++index) {
            remainder[index] ^=
                finite_field_multiply(divisor[index], factor);
        }
    }
}

u32 module_index(u32 x, u32 y) noexcept {
    return y * kQrCodeSize + x;
}

void set_function(u8 modules[kModuleCount], u8 functions[kModuleCount],
                  i32 x, i32 y, bool black) noexcept {
    if (x < 0 || y < 0 || x >= static_cast<i32>(kQrCodeSize) ||
        y >= static_cast<i32>(kQrCodeSize)) {
        return;
    }
    const u32 index = module_index(static_cast<u32>(x),
                                   static_cast<u32>(y));
    modules[index] = black ? 1 : 0;
    functions[index] = 1;
}

void draw_finder(u8 modules[kModuleCount], u8 functions[kModuleCount],
                 i32 center_x, i32 center_y) noexcept {
    for (i32 y = -4; y <= 4; ++y) {
        for (i32 x = -4; x <= 4; ++x) {
            const i32 distance = maximum(absolute_value(x), absolute_value(y));
            set_function(modules, functions, center_x + x, center_y + y,
                         distance != 2 && distance != 4);
        }
    }
}

void draw_alignment(u8 modules[kModuleCount], u8 functions[kModuleCount],
                    i32 center_x, i32 center_y) noexcept {
    for (i32 y = -2; y <= 2; ++y) {
        for (i32 x = -2; x <= 2; ++x) {
            set_function(modules, functions, center_x + x, center_y + y,
                         maximum(absolute_value(x), absolute_value(y)) != 1);
        }
    }
}

bool format_bit(u32 value, u32 bit) noexcept {
    return ((value >> bit) & 1u) != 0;
}

void draw_format(u8 modules[kModuleCount], u8 functions[kModuleCount],
                 u32 mask) noexcept {
    // Error-correction level M has format bits 00.
    const u32 data = mask;
    u32 remainder = data;
    for (u32 index = 0; index < 10; ++index) {
        remainder = (remainder << 1) ^
                    ((remainder >> 9) * 0x537u);
    }
    const u32 bits = ((data << 10) | remainder) ^ 0x5412u;
    for (u32 index = 0; index <= 5; ++index) {
        set_function(modules, functions, 8, static_cast<i32>(index),
                     format_bit(bits, index));
    }
    set_function(modules, functions, 8, 7, format_bit(bits, 6));
    set_function(modules, functions, 8, 8, format_bit(bits, 7));
    set_function(modules, functions, 7, 8, format_bit(bits, 8));
    for (u32 index = 9; index < 15; ++index) {
        set_function(modules, functions, static_cast<i32>(14 - index), 8,
                     format_bit(bits, index));
    }
    for (u32 index = 0; index < 8; ++index) {
        set_function(modules, functions,
                     static_cast<i32>(kQrCodeSize - 1 - index), 8,
                     format_bit(bits, index));
    }
    for (u32 index = 8; index < 15; ++index) {
        set_function(modules, functions, 8,
                     static_cast<i32>(kQrCodeSize - 15 + index),
                     format_bit(bits, index));
    }
    set_function(modules, functions, 8,
                 static_cast<i32>(kQrCodeSize - 8), true);
}

void draw_functions(u8 modules[kModuleCount],
                    u8 functions[kModuleCount]) noexcept {
    for (u32 index = 0; index < kQrCodeSize; ++index) {
        set_function(modules, functions, 6, static_cast<i32>(index),
                     (index & 1u) == 0);
        set_function(modules, functions, static_cast<i32>(index), 6,
                     (index & 1u) == 0);
    }
    draw_finder(modules, functions, 3, 3);
    draw_finder(modules, functions, static_cast<i32>(kQrCodeSize - 4), 3);
    draw_finder(modules, functions, 3, static_cast<i32>(kQrCodeSize - 4));
    draw_alignment(modules, functions, 30, 30);
    draw_format(modules, functions, 0);
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

void apply_mask(u8 modules[kModuleCount],
                const u8 functions[kModuleCount], u32 mask) noexcept {
    for (u32 y = 0; y < kQrCodeSize; ++y) {
        for (u32 x = 0; x < kQrCodeSize; ++x) {
            const u32 index = module_index(x, y);
            if (functions[index] == 0 && mask_value(mask, x, y)) {
                modules[index] ^= 1;
            }
        }
    }
}

void draw_codewords(u8 modules[kModuleCount],
                    const u8 functions[kModuleCount],
                    const u8 codewords[kTotalCodewords]) noexcept {
    u32 bit_index = 0;
    bool upward = true;
    for (i32 right = static_cast<i32>(kQrCodeSize) - 1; right >= 1;
         right -= 2) {
        if (right == 6) {
            right = 5;
        }
        for (u32 vertical = 0; vertical < kQrCodeSize; ++vertical) {
            const u32 y = upward ? kQrCodeSize - 1 - vertical : vertical;
            for (u32 column = 0; column < 2; ++column) {
                const u32 x = static_cast<u32>(right) - column;
                const u32 index = module_index(x, y);
                if (functions[index] != 0) {
                    continue;
                }
                bool black = false;
                if (bit_index < kTotalCodewords * 8) {
                    black = ((codewords[bit_index >> 3] >>
                              (7u - (bit_index & 7u))) &
                             1u) != 0;
                }
                modules[index] = black ? 1 : 0;
                ++bit_index;
            }
        }
        upward = !upward;
    }
}

void penalty_add_history(u32 run, u32 history[7]) noexcept {
    if (history[0] == 0) {
        run += kQrCodeSize;
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

u32 penalty_terminate(bool color, u32 run, u32 history[7]) noexcept {
    if (color) {
        penalty_add_history(run, history);
        run = 0;
    }
    run += kQrCodeSize;
    penalty_add_history(run, history);
    return penalty_count_patterns(history);
}

u32 penalty(const u8 modules[kModuleCount]) noexcept {
    u32 score = 0;
    for (u32 axis = 0; axis < 2; ++axis) {
        for (u32 line = 0; line < kQrCodeSize; ++line) {
            bool previous = false;
            u32 run = 0;
            u32 history[7]{};
            for (u32 offset = 0; offset < kQrCodeSize; ++offset) {
                const u32 x = axis == 0 ? offset : line;
                const u32 y = axis == 0 ? line : offset;
                const bool black = modules[module_index(x, y)] != 0;
                if (black == previous) {
                    ++run;
                    if (run == 5) {
                        score += 3;
                    } else if (run > 5) {
                        ++score;
                    }
                } else {
                    penalty_add_history(run, history);
                    if (!previous) {
                        score += penalty_count_patterns(history) * 40;
                    }
                    previous = black;
                    run = 1;
                }
            }
            score += penalty_terminate(previous, run, history) * 40;
        }
    }
    for (u32 y = 0; y + 1 < kQrCodeSize; ++y) {
        for (u32 x = 0; x + 1 < kQrCodeSize; ++x) {
            const u8 color = modules[module_index(x, y)];
            if (modules[module_index(x + 1, y)] == color &&
                modules[module_index(x, y + 1)] == color &&
                modules[module_index(x + 1, y + 1)] == color) {
                score += 3;
            }
        }
    }
    u32 black = 0;
    for (u32 index = 0; index < kModuleCount; ++index) {
        black += modules[index] != 0;
    }
    const u32 balance = black * 20u > kModuleCount * 10u
                            ? black * 20u - kModuleCount * 10u
                            : kModuleCount * 10u - black * 20u;
    const u32 quotient = (balance + kModuleCount - 1) / kModuleCount;
    score += (quotient == 0 ? 0 : quotient - 1) * 10u;
    return score;
}

} // namespace

bool qr_code_encode_v5_m(ConstSlice<u8> payload, QrCode* output) noexcept {
    if (!output || !payload.data || payload.count == 0 ||
        payload.count > kQrCodeMaximumPayloadBytes) {
        return false;
    }
    BitBuffer bits{};
    append_bits(&bits, 0x4, 4);
    append_bits(&bits, payload.count, 8);
    for (u32 index = 0; index < payload.count; ++index) {
        append_bits(&bits, payload[index], 8);
    }
    append_bits(&bits, 0, minimum(kDataCodewords * 8 - bits.bits, 4u));
    while ((bits.bits & 7u) != 0) {
        append_bits(&bits, 0, 1);
    }
    for (bool alternate = false; bits.bits < kDataCodewords * 8; alternate = !alternate) {
        append_bits(&bits, alternate ? 0x11u : 0xECu, 8);
    }

    u8 divisor[kErrorCodewordsPerBlock]{};
    u8 error[kBlockCount][kErrorCodewordsPerBlock]{};
    make_divisor(divisor);
    for (u32 block = 0; block < kBlockCount; ++block) {
        make_remainder(bits.data + block * kBlockDataCodewords, divisor,
                       error[block]);
    }
    u8 codewords[kTotalCodewords]{};
    u32 codeword_count = 0;
    for (u32 index = 0; index < kBlockDataCodewords; ++index) {
        for (u32 block = 0; block < kBlockCount; ++block) {
            codewords[codeword_count++] =
                bits.data[block * kBlockDataCodewords + index];
        }
    }
    for (u32 index = 0; index < kErrorCodewordsPerBlock; ++index) {
        for (u32 block = 0; block < kBlockCount; ++block) {
            codewords[codeword_count++] = error[block][index];
        }
    }

    u8 modules[kModuleCount]{};
    u8 functions[kModuleCount]{};
    draw_functions(modules, functions);
    draw_codewords(modules, functions, codewords);
    u32 best_mask = 0;
    u32 best_penalty = ~0u;
    for (u32 mask = 0; mask < 8; ++mask) {
        apply_mask(modules, functions, mask);
        draw_format(modules, functions, mask);
        const u32 current = penalty(modules);
        if (current < best_penalty) {
            best_penalty = current;
            best_mask = mask;
        }
        apply_mask(modules, functions, mask);
    }
    apply_mask(modules, functions, best_mask);
    draw_format(modules, functions, best_mask);

    QrCode encoded{};
    encoded.size = kQrCodeSize;
    encoded.mask = static_cast<u8>(best_mask);
    for (u32 index = 0; index < kModuleCount; ++index) {
        if (modules[index] != 0) {
            encoded.modules[index >> 3] |=
                static_cast<u8>(1u << (index & 7u));
        }
    }
    *output = encoded;
    return true;
}

bool qr_code_module(const QrCode& code, u32 x, u32 y) noexcept {
    if (code.size != kQrCodeSize || x >= kQrCodeSize || y >= kQrCodeSize) {
        return false;
    }
    const u32 index = module_index(x, y);
    return ((code.modules[index >> 3] >> (index & 7u)) & 1u) != 0;
}

namespace {

u32 compact_rect_pass(const QrCode& code, Slice<QrCodeRect> output,
                      bool write) noexcept {
    constexpr u32 kMaximumRunsPerRow = (kQrCodeSize + 1) / 2;
    QrCodeRect active[kMaximumRunsPerRow]{};
    u32 active_count = 0;
    u32 required = 0;
    for (u32 y = 0; y < kQrCodeSize; ++y) {
        QrCodeRect next[kMaximumRunsPerRow]{};
        bool matched[kMaximumRunsPerRow]{};
        u32 next_count = 0;
        u32 x = 0;
        while (x < kQrCodeSize) {
            while (x < kQrCodeSize && !qr_code_module(code, x, y)) {
                ++x;
            }
            if (x == kQrCodeSize) {
                break;
            }
            const u32 left = x;
            while (x < kQrCodeSize && qr_code_module(code, x, y)) {
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

QrCodeRectResult qr_code_compact_rects(const QrCode& code,
                                       Slice<QrCodeRect> output) noexcept {
    if (code.size != kQrCodeSize || (!output.data && output.count != 0)) {
        return QrCodeRectResult{0, true};
    }
    const u32 required = compact_rect_pass(code, Slice<QrCodeRect>{}, false);
    if (required > output.count) {
        return QrCodeRectResult{required, true};
    }
    compact_rect_pass(code, output, true);
    return QrCodeRectResult{required, false};
}

} // namespace maicord
