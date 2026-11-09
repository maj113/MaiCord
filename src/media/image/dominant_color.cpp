#include "maicord/media/image/dominant_color.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u32 kShift = 3; // 8 - 5 significant bits per channel
constexpr u32 kSampleStep = 10, kColors = 5, kIterations = 1000, kQueueCapacity = 8;
// Three quarters of the colours come from splitting by population (3.75, so 4 boxes), the rest by population
// times volume.
constexpr u32 kPopulationColors = 4;

u32 cell(i32 r, i32 g, i32 b) noexcept {
    return (static_cast<u32>(r) << 10) + (static_cast<u32>(g) << 5) + static_cast<u32>(b);
}

// A box of the 32x32x32 colour cube, bounds inclusive per channel (r, g, b). The first box's upper bound may lie
// below its lower one: an upper bound only rises for a pixel that did not lower the lower bound, so a lone pixel
// leaves it at 0 (an empty box).
struct Box {
    i32 low[3], high[3];
    u64 count;
    i64 volume;
};

void measure(const u32* histogram, Box* box) noexcept {
    box->volume = i64{box->high[0] - box->low[0] + 1} * (box->high[1] - box->low[1] + 1) *
                  (box->high[2] - box->low[2] + 1);
    u64 count = 0;
    for (i32 r = box->low[0]; r <= box->high[0]; ++r)
        for (i32 g = box->low[1]; g <= box->high[1]; ++g)
            for (i32 b = box->low[2]; b <= box->high[2]; ++b)
                count += histogram[cell(r, g, b)];
    box->count = count;
}

// Boxes by population (or population times volume). The order is a stable ascending sort made when a box is taken
// after a push, and the last box goes first: of equal boxes, the one pushed last.
struct Queue {
    Box boxes[kQueueCapacity];
    u32 size;
    bool sorted, by_volume;

    i64 key(const Box& box) const noexcept {
        return by_volume ? static_cast<i64>(box.count) * box.volume : static_cast<i64>(box.count);
    }
    void push(const Box& box) noexcept {
        if (size == kQueueCapacity)
            return;
        boxes[size++] = box;
        sorted = false;
    }
    Box pop() noexcept {
        if (!sorted) {
            for (u32 i = 1; i < size; ++i) {
                const Box moving = boxes[i];
                u32 j = i;
                for (; j && key(boxes[j - 1]) > key(moving); --j)
                    boxes[j] = boxes[j - 1];
                boxes[j] = moving;
            }
            sorted = true;
        }
        return boxes[--size];
    }
};

// Splits a populated box across its widest channel (red, then green, on ties) where its running population first
// passes half, moved halfway toward the farther end and then onto a populated slice. Returns the boxes that stand
// for it: the box itself when it holds one pixel, else its two halves (the second may be empty).
u32 cut(const u32* histogram, const Box& box, Box* out) noexcept {
    i32 width[3];
    for (u32 c = 0; c < 3; ++c)
        width[c] = box.high[c] - box.low[c] + 1;
    if (box.count == 1) {
        out[0] = box;
        return 1;
    }
    const i32 widest = maximum(width[0], maximum(width[1], width[2]));
    const u32 axis = widest == width[0] ? 0 : widest == width[1] ? 1 : 2;
    const u32 first = axis == 0 ? 1 : 0, second = axis == 2 ? 1 : 2;
    const i32 low = box.low[axis], high = box.high[axis];
    // Running population of the slices up to each one; zero outside the box, like a slice holding none.
    u64 partial[32]{};
    u64 total = 0;
    for (i32 i = low; i <= high; ++i) {
        u64 sum = 0;
        for (i32 j = box.low[first]; j <= box.high[first]; ++j)
            for (i32 k = box.low[second]; k <= box.high[second]; ++k) {
                i32 at[3];
                at[axis] = i;
                at[first] = j;
                at[second] = k;
                sum += histogram[cell(at[0], at[1], at[2])];
            }
        total += sum;
        partial[i] = total;
    }
    const auto running = [&](i32 i) noexcept { return i >= low && i <= high ? partial[i] : 0; };
    for (i32 i = low; i <= high; ++i) {
        if (2 * partial[i] <= total)
            continue;
        const i32 left = i - low, right = high - i;
        // Truncated toward zero.
        i32 split = left <= right ? minimum(high - 1, (2 * i + right) / 2) : maximum(low, (2 * (i - 1) - left) / 2);
        while (!running(split))
            ++split;
        u64 rest = total - partial[split];
        while (!rest && running(split - 1))
            rest = total - partial[--split];
        out[0] = out[1] = box;
        out[0].high[axis] = split;
        out[1].low[axis] = split + 1;
        measure(histogram, &out[0]);
        measure(histogram, &out[1]);
        return 2;
    }
    return 0;
}

// Splits the queue's boxes, the largest first, until it holds `target` more than one colour's worth or a thousand
// rounds pass.
void split(const u32* histogram, Queue* queue, u32 target) noexcept {
    u32 colors = 1;
    for (u32 iterations = 0; iterations < kIterations;) {
        const Box box = queue->pop();
        if (!box.count) {
            queue->push(box);
            ++iterations;
            continue;
        }
        Box parts[2];
        const u32 made = cut(histogram, box, parts);
        if (!made)
            return;
        queue->push(parts[0]);
        if (made == 2) {
            queue->push(parts[1]);
            ++colors;
        }
        if (colors >= target || iterations++ > kIterations)
            return;
    }
}

// The box's population-weighted mean of its cells' centres; its own centre when it holds no pixel.
u32 mean(const u32* histogram, const Box& box) noexcept {
    u64 total = 0, sum[3]{};
    for (i32 r = box.low[0]; r <= box.high[0]; ++r)
        for (i32 g = box.low[1]; g <= box.high[1]; ++g)
            for (i32 b = box.low[2]; b <= box.high[2]; ++b) {
                const u64 count = histogram[cell(r, g, b)];
                total += count;
                sum[0] += count * static_cast<u64>(8 * r + 4);
                sum[1] += count * static_cast<u64>(8 * g + 4);
                sum[2] += count * static_cast<u64>(8 * b + 4);
            }
    u32 rgb = 0;
    for (u32 c = 0; c < 3; ++c)
        rgb = (rgb << 8) |
              static_cast<u32>(total ? sum[c] / total : 4u * static_cast<u32>(box.low[c] + box.high[c] + 1));
    return rgb;
}
// JavaScript's Math.round (halves up) and the floor it rests on, for the values below (well inside i64).
double floor_of(double x) noexcept {
    const i64 i = static_cast<i64>(x);
    return static_cast<double>(i - (x < static_cast<double>(i)));
}
double round_half_up(double x) noexcept {
    return floor_of(x + .5);
}
// toFixed(1) read back as a number.
double one_decimal(double x) noexcept {
    return round_half_up(x * 10) / 10;
}
} // namespace

u32 image_palette(ConstSlice<u8> rgba, u32 width, u32 height, Slice<u32> histogram, u32* colors) noexcept {
    const u64 pixels = u64{width} * height;
    if (!colors || !rgba.data || rgba.count / 4 < pixels || !histogram.data ||
        histogram.count < kImageDominantColorHistogramEntries)
        return 0;
    u32* const h = histogram.data;
    clear_bytes(h, kImageDominantColorHistogramEntries * 4u);
    Box whole{{1'000'000, 1'000'000, 1'000'000}, {0, 0, 0}, 0, 0};
    u64 samples = 0;
    for (u64 i = 0; i < pixels; i += kSampleStep) {
        const u8* p = rgba.data + i * 4;
        if (p[3] < 125 || (p[0] > 250 && p[1] > 250 && p[2] > 250))
            continue;
        const i32 value[3]{p[0] >> kShift, p[1] >> kShift, p[2] >> kShift};
        ++h[cell(value[0], value[1], value[2])];
        for (u32 c = 0; c < 3; ++c) {
            if (value[c] < whole.low[c])
                whole.low[c] = value[c];
            else if (value[c] > whole.high[c])
                whole.high[c] = value[c];
        }
        ++samples;
    }
    if (!samples) {
        colors[0] = 0;
        return 1;
    }
    measure(h, &whole);
    Queue population{};
    population.push(whole);
    split(h, &population, kPopulationColors);
    Queue weighted{};
    weighted.by_volume = true;
    while (population.size)
        weighted.push(population.pop());
    split(h, &weighted, kColors - weighted.size);
    u32 count = 0;
    while (weighted.size && count < kImagePaletteColors)
        colors[count++] = mean(h, weighted.pop());
    return count;
}

u32 image_dominant_color(ConstSlice<u8> rgba, u32 width, u32 height, Slice<u32> histogram) noexcept {
    u32 colors[kImagePaletteColors];
    return image_palette(rgba, width, height, histogram, colors) ? colors[0] : 0;
}

u32 image_palette_companion(u32 rgb) noexcept {
    const double r = static_cast<double>(rgb >> 16 & 255) / 255, g = static_cast<double>(rgb >> 8 & 255) / 255,
                 b = static_cast<double>(rgb & 255) / 255;
    const double high = maximum(r, maximum(g, b)), low = minimum(r, minimum(g, b)), delta = high - low;
    // Hue in whole degrees; saturation and lightness in percent, one decimal.
    double hue = delta == 0 ? 0 : high == r ? (g - b) / delta : high == g ? (b - r) / delta + 2 : (r - g) / delta + 4;
    hue = round_half_up(60 * hue);
    if (hue < 0)
        hue += 360;
    const double light = (high + low) / 2, span = 1 - (2 * light - 1 < 0 ? 1 - 2 * light : 2 * light - 1);
    double saturation = one_decimal(100 * (delta == 0 ? 0 : delta / span)), lightness = one_decimal(100 * light);
    if (saturation < 30)
        saturation += 30;
    if (lightness > 80)
        lightness -= 40;
    if (lightness < 20)
        lightness += 15;
    // The first of three hues a quarter turn apart, going back from this one.
    hue -= 90;
    if (hue < 0)
        hue += 360;
    const double n = lightness / 100, a = saturation * minimum(n, 1 - n) / 100;
    constexpr double kOffsets[3]{0, 8, 4}; // red, green, blue
    u32 out = 0;
    for (const double offset : kOffsets) {
        double k = offset + hue / 30;
        k -= 12 * floor_of(k / 12);
        const double ramp = maximum(minimum(minimum(k - 3, 9 - k), 1.0), -1.0);
        out = (out << 8) | static_cast<u32>(round_half_up(255 * (n - a * ramp)));
    }
    return out;
}

} // namespace maicord
