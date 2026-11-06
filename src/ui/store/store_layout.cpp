#include "store_internal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/shell/presentation.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/ui/widgets/kit_markdown.hpp"

namespace maicord {
using namespace store_detail;
namespace store_detail {

const TypeMetrics& type_metrics(u8 type) noexcept {
    static constexpr TypeMetrics kTable[]{
        {16, 20, text_style_none, false, false},
        {16, 20, text_style_bold, true, false},
        {13, 18, text_style_subtext, false, false},
        {13, 18, text_style_subtext | text_style_bold, true, false},
        {14, 18, kTextStyleSizeCodeSm, false, false},
        {14, 18, kTextStyleSizeCodeSm | text_style_bold, true, false},
        {12, 16, kTextStyleSizeCodeXs | text_style_bold, true, false},
        {20, 24, text_style_heading_2 | text_style_bold, true, false},
        {24, 30, text_style_heading_1 | text_style_bold, true, false},
        {12, 18, kTextStyleSizeCodeXs, false, true},
        {16, 22, text_style_none, false, false},
    };
    static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<u32>(UiStoreType::count));
    return kTable[type < static_cast<u8>(UiStoreType::count) ? type : 0];
}
const UiStoreMeasure* g_measure;
float text_width(u8 type, const Utf8View& text) noexcept {
    const auto& m = type_metrics(type);
    // The code face advances a fixed .6 em a character.
    if (m.mono)
        return text.bytes * m.em * .6f + 1.f;
    if (g_measure && g_measure->width)
        return g_measure->width(g_measure->context, text, m.style) + 1.f;
    // The estimate runs narrow for the UI face: widen it so a label is not cut.
    const float width = ui_settings_text_width(text, m.em) * 1.12f;
    return m.bold ? width * 1.06f : width;
}
u32 text_lines(u8 type, const Utf8View& text, float width) noexcept {
    const auto& m = type_metrics(type);
    return ui_settings_text_lines(text, (m.bold ? m.em * 1.06f : m.em) * 1.1f, width);
}
bool interactive(const UiStoreNode& n) noexcept {
    return n.action && !(n.flags & (ui_store_hidden | ui_store_disabled));
}

} // namespace store_detail

namespace {
constexpr float kIconBox = 16;
u8 icon_byte(UiIcon icon) noexcept {
    return icon < UiIcon::count ? static_cast<u8>(icon) : static_cast<u8>(kNoIcon);
}

UiStoreNode& at(const UiStoreScene& s, u16 i) noexcept {
    return s.nodes[i];
}

void shift(const UiStoreScene& s, u16 i, float dx, float dy) noexcept {
    auto& n = at(s, i);
    n.x += dx;
    n.y += dy;
    for (u16 c = n.first; c != kUiStoreNone; c = at(s, c).next)
        shift(s, c, dx, dy);
}

float intrinsic(const UiStoreScene& s, u16 i) noexcept;

float leaf_width(const UiStoreNode& n) noexcept {
    switch (n.kind) {
    case UiStoreKind::text:
        return n.rw ? n.rw : text_width(n.type, n.text);
    case UiStoreKind::markdown:
        return 0;
    case UiStoreKind::icon:
        return n.rw ? n.rw : kIconBox;
    case UiStoreKind::tile:
        return n.rw ? n.rw : 40;
    case UiStoreKind::chip: {
        const auto tone = static_cast<UiStoreTone>(n.variant);
        float width = text_width(static_cast<u8>(tone == UiStoreTone::mono ? UiStoreType::mono : UiStoreType::small), n.text) + 16;
        if (tone >= UiStoreTone::risk_low && tone <= UiStoreTone::risk_high)
            width += 14;
        else if (n.icon != kNoIcon)
            width += 18;
        return width;
    }
    case UiStoreKind::button: {
        const float width = leaf_text_content(n);
        return n.variant == static_cast<u8>(UiStoreButton::link) ? width : width + (n.type ? 32 : 24);
    }
    case UiStoreKind::icon_button:
        return n.rw ? n.rw : 32;
    case UiStoreKind::toggle:
        return kSwitchWidth;
    case UiStoreKind::check:
        return 28 + text_width(static_cast<u8>(UiStoreType::body), n.text);
    case UiStoreKind::filter: {
        float width = text_width(static_cast<u8>(UiStoreType::label_bold), n.text) + 24;
        if (n.text2.bytes)
            width += 5 + text_width(static_cast<u8>(UiStoreType::small), n.text2);
        if (n.icon != kNoIcon || (n.flags & ui_store_selected))
            width += 20;
        return width;
    }
    case UiStoreKind::tab: {
        float width = text_width(static_cast<u8>(UiStoreType::body), n.text);
        if (n.text2.bytes)
            width += 4 + tab_badge_width(n);
        return width;
    }
    case UiStoreKind::search:
    case UiStoreKind::dropdown:
        return n.rw ? n.rw : 184;
    case UiStoreKind::segment:
        return text_width(static_cast<u8>(UiStoreType::label_bold), n.text) + 28;
    case UiStoreKind::slider:
        return n.rw ? n.rw : 160;
    case UiStoreKind::swatch:
        return n.rw ? n.rw : 16;
    case UiStoreKind::bar:
        return n.rw;
    case UiStoreKind::rule:
        return n.variant ? 1 : 0;
    case UiStoreKind::spacer:
        return n.rw;
    case UiStoreKind::disc:
        return 64;
    case UiStoreKind::spinner:
        return 16;
    default:
        return 0;
    }
}

float leaf_height(const UiStoreNode& n, float width) noexcept {
    switch (n.kind) {
    case UiStoreKind::text: {
        const auto& m = type_metrics(n.type);
        u32 lines = (n.flags & ui_store_wrap) ? text_lines(n.type, n.text, width) : n.text.bytes ? 1 : 0;
        if (n.count && lines > n.count)
            lines = n.count;
        if (n.value && lines < n.value)
            lines = n.value;
        return lines * m.line;
    }
    case UiStoreKind::markdown: {
        if (n.data)
            return ui_markdown_height(const_cast<UiMarkdown*>(static_cast<const UiMarkdown*>(n.data)), width, nullptr);
        return text_lines(n.type, n.text, width) * type_metrics(n.type).line;
    }
    case UiStoreKind::icon:
        return n.rh ? n.rh : n.rw ? n.rw : kIconBox;
    case UiStoreKind::tile:
        return n.rw ? n.rw : 40;
    case UiStoreKind::chip:
        return kChipHeight;
    case UiStoreKind::button:
        return n.type ? kButtonLarge : kButtonSmall;
    case UiStoreKind::icon_button:
        return n.rw ? n.rw : 32;
    case UiStoreKind::toggle:
        return kSwitchHeight;
    case UiStoreKind::check:
        return 24;
    case UiStoreKind::filter:
        return kFilterHeight;
    case UiStoreKind::tab:
        return kTabHeight;
    case UiStoreKind::search:
    case UiStoreKind::dropdown:
        return kFieldHeight;
    case UiStoreKind::segment:
        return 32;
    case UiStoreKind::slider:
        return 24;
    case UiStoreKind::swatch:
        return n.rh ? n.rh : n.rw ? n.rw : 16;
    case UiStoreKind::bar:
        return n.rh ? n.rh : 12;
    case UiStoreKind::rule:
        return n.variant ? 20 : 1;
    case UiStoreKind::spacer:
        return n.rh;
    case UiStoreKind::disc:
        return 64;
    case UiStoreKind::spinner:
        return 16;
    default:
        return 0;
    }
}

bool container(UiStoreKind kind) noexcept {
    return kind <= UiStoreKind::preview;
}

float intrinsic(const UiStoreScene& s, u16 i) noexcept {
    const auto& n = at(s, i);
    if (hidden(n))
        return 0;
    if (!container(n.kind))
        return leaf_width(n);
    float total = 0;
    u32 shown = 0;
    const bool horizontal = n.kind == UiStoreKind::row;
    for (u16 c = n.first; c != kUiStoreNone; c = at(s, c).next) {
        if (hidden(at(s, c)))
            continue;
        const float w = intrinsic(s, c);
        total = horizontal ? total + w : maximum(total, w);
        ++shown;
    }
    if (horizontal && shown)
        total += n.gap * (shown - 1);
    return n.rw ? n.rw : total + n.pad_x * 2;
}

void place(const UiStoreScene& s, u16 i, float x, float y, float width) noexcept;

// Children of a column-like container.
float place_stack(const UiStoreScene& s, UiStoreNode& n, float x, float y, float width) noexcept {
    const float cx = x + n.pad_x, cw = maximum(1.f, width - n.pad_x * 2);
    float cy = y + n.pad_y;
    bool any = false;
    for (u16 c = n.first; c != kUiStoreNone; c = at(s, c).next) {
        auto& child = at(s, c);
        if (child.next != kUiStoreNone && (at(s, child.next).flags & ui_store_fallback)) {
            // A segmented track and the dropdown standing in for it: the track while its options, each at its
            // label's width, fit the width; else the dropdown. Decided at every layout, so it follows the window.
            auto& menu = at(s, child.next);
            child.flags &= ~ui_store_hidden;
            menu.flags &= ~ui_store_hidden;
            (intrinsic(s, c) <= cw ? menu : child).flags |= ui_store_hidden;
        }
        if (hidden(child))
            continue;
        if (any)
            cy += n.gap;
        place(s, c, cx, cy, cw);
        if (child.flags & ui_store_end)
            shift(s, c, cw - child.w, 0);
        else if (child.flags & ui_store_center)
            shift(s, c, (cw - child.w) * .5f, 0);
        cy += child.h;
        any = true;
    }
    return any ? cy - y + n.pad_y : n.pad_y * 2;
}

float place_row(const UiStoreScene& s, UiStoreNode& n, float x, float y, float width) noexcept {
    const float cx = x + n.pad_x, cw = maximum(1.f, width - n.pad_x * 2);
    u32 shown = 0, flex = 0;
    float fixed = 0;
    for (u16 c = n.first; c != kUiStoreNone; c = at(s, c).next) {
        const auto& child = at(s, c);
        if (hidden(child))
            continue;
        ++shown;
        if (child.flags & ui_store_flex)
            ++flex;
        else
            fixed += intrinsic(s, c);
    }
    // A segmented track with a width of its own (`rw`) shares it equally; any other row's options take their own.
    const bool shared = n.variant == 1 && n.rw;
    const float gaps = shown > 1 ? n.gap * (shown - 1) : 0;
    const float free = maximum(0.f, cw - fixed - gaps);
    float cursor = cx, height = 0;
    for (u16 c = n.first; c != kUiStoreNone; c = at(s, c).next) {
        auto& child = at(s, c);
        if (hidden(child))
            continue;
        const float w = shared ? cw / shown : (child.flags & ui_store_flex) ? free / flex : intrinsic(s, c);
        place(s, c, cursor, y + n.pad_y, w);
        cursor += child.w + n.gap;
        height = maximum(height, child.h);
    }
    if (n.flags & ui_store_center)
        for (u16 c = n.first; c != kUiStoreNone; c = at(s, c).next) {
            auto& child = at(s, c);
            if (!hidden(child))
                shift(s, c, 0, (height - child.h) * .5f);
        }
    if ((n.flags & ui_store_justify) && !flex && cursor - n.gap < cx + cw) {
        const float used = cursor - n.gap - cx;
        for (u16 c = n.first; c != kUiStoreNone; c = at(s, c).next)
            if (!hidden(at(s, c)))
                shift(s, c, (cw - used) * .5f, 0);
    }
    return height + n.pad_y * 2;
}

float place_flow(const UiStoreScene& s, UiStoreNode& n, float x, float y, float width) noexcept {
    const float cx = x + n.pad_x, cw = maximum(1.f, width - n.pad_x * 2);
    float cursor = 0, top = y + n.pad_y, line = 0;
    bool any = false;
    for (u16 c = n.first; c != kUiStoreNone; c = at(s, c).next) {
        auto& child = at(s, c);
        if (hidden(child))
            continue;
        const float w = intrinsic(s, c);
        const bool divider = child.kind == UiStoreKind::rule;
        if (cursor > 0 && cursor + w > cw + .01f) {
            top += line + n.gap;
            cursor = line = 0;
        }
        if (divider && cursor == 0) {
            // A divider never starts a line.
            child.x = cx;
            child.y = top;
            child.w = child.h = 0;
            continue;
        }
        place(s, c, cx + cursor, top, w);
        cursor += child.w + n.gap;
        line = maximum(line, child.h);
        any = true;
    }
    return any ? top + line - y + n.pad_y : n.pad_y * 2;
}

float place_grid(const UiStoreScene& s, UiStoreNode& n, float x, float y, float width) noexcept {
    const float cx = x + n.pad_x, cw = maximum(1.f, width - n.pad_x * 2);
    const float minimum_width = n.rw ? n.rw : kCardMinWidth;
    u32 columns = static_cast<u32>((cw + n.gap) / (minimum_width + n.gap));
    columns = columns < 1 ? 1 : columns > 3 ? 3 : columns;
    n.count = static_cast<u16>(columns);
    const float cell = (cw - n.gap * (columns - 1)) / columns;
    float top = y + n.pad_y;
    u16 row_first[3]{};
    u32 in_row = 0;
    float row_height = 0;
    bool any = false;
    const auto close_row = [&]() noexcept {
        for (u32 k = 0; k < in_row; ++k)
            at(s, row_first[k]).h = row_height;
        top += row_height + n.gap;
        in_row = 0;
        row_height = 0;
    };
    for (u16 c = n.first; c != kUiStoreNone; c = at(s, c).next) {
        auto& child = at(s, c);
        if (hidden(child))
            continue;
        place(s, c, cx + in_row * (cell + n.gap), top, cell);
        child.w = cell;
        row_first[in_row++] = c;
        row_height = maximum(row_height, child.h);
        any = true;
        if (in_row == columns)
            close_row();
    }
    if (in_row)
        close_row();
    return any ? top - n.gap - y + n.pad_y : n.pad_y * 2;
}

float place_split(const UiStoreScene& s, UiStoreNode& n, float x, float y, float width) noexcept {
    const u16 main = n.first, rail = main != kUiStoreNone ? at(s, main).next : kUiStoreNone;
    if (main == kUiStoreNone)
        return 0;
    const float rail_width = n.rw ? n.rw : 264;
    if (rail == kUiStoreNone || width < 640) {
        // Narrow: the rail sits above the main column.
        float cy = y;
        if (rail != kUiStoreNone) {
            place(s, rail, x, cy, width);
            cy += at(s, rail).h + n.gap;
        }
        place(s, main, x, cy, width);
        return cy + at(s, main).h - y;
    }
    place(s, main, x, y, width - rail_width - n.gap);
    place(s, rail, x + width - rail_width, y, rail_width);
    return maximum(at(s, main).h, at(s, rail).h);
}

float place_preview(const UiStoreScene& s, UiStoreNode& n, float x, float y, float width) noexcept {
    const float height = n.variant == static_cast<u8>(UiStoreAspect::r16x10) ? width * .625f
                         : n.variant == static_cast<u8>(UiStoreAspect::r16x9) ? width * .5625f
                                                                              : n.rh;
    const float inset_x = n.pad_x ? n.pad_x : 8, inset_y = n.pad_y ? n.pad_y : 8;
    for (u16 c = n.first; c != kUiStoreNone; c = at(s, c).next) {
        auto& child = at(s, c);
        if (hidden(child))
            continue;
        place(s, c, 0, 0, intrinsic(s, c));
        float cx = x + inset_x, cy = y + inset_y;
        switch (static_cast<UiStoreAnchor>(child.anchor)) {
        case UiStoreAnchor::top_right:
            cx = x + width - inset_x - child.w;
            break;
        case UiStoreAnchor::bottom_left:
            cy = y + height - inset_y - child.h;
            break;
        case UiStoreAnchor::bottom_right:
            cx = x + width - inset_x - child.w;
            cy = y + height - inset_y - child.h;
            break;
        case UiStoreAnchor::center:
            cx = x + (width - child.w) * .5f;
            cy = y + (height - child.h) * .5f;
            break;
        default:
            break;
        }
        shift(s, c, cx, cy);
    }
    return height;
}

void place(const UiStoreScene& s, u16 i, float x, float y, float width) noexcept {
    auto& n = at(s, i);
    n.x = x;
    n.y = y;
    if (hidden(n)) {
        n.w = n.h = 0;
        return;
    }
    if (container(n.kind)) {
        n.w = n.rw && n.kind != UiStoreKind::split && n.kind != UiStoreKind::grid ? minimum(n.rw, width)
              : n.kind == UiStoreKind::row && n.variant == 1                      ? minimum(intrinsic(s, i), width)
                                                                                  : width;
        switch (n.kind) {
        case UiStoreKind::column:
        case UiStoreKind::card:
            n.h = place_stack(s, n, x, y, n.w);
            break;
        case UiStoreKind::row:
            n.h = place_row(s, n, x, y, n.w);
            break;
        case UiStoreKind::flow:
            n.h = place_flow(s, n, x, y, n.w);
            break;
        case UiStoreKind::grid:
            n.h = place_grid(s, n, x, y, n.w);
            break;
        case UiStoreKind::split:
            n.h = place_split(s, n, x, y, n.w);
            break;
        default:
            n.h = place_preview(s, n, x, y, n.w);
            break;
        }
        if (n.rh && n.kind != UiStoreKind::preview)
            n.h = maximum(n.h, n.rh);
        return;
    }
    const bool fill = (n.flags & (ui_store_stretch | ui_store_flex)) || (n.kind == UiStoreKind::text && (n.flags & ui_store_wrap)) ||
                      (n.kind == UiStoreKind::rule && !n.variant) || n.kind == UiStoreKind::slider || n.kind == UiStoreKind::markdown;
    float w = fill ? width : minimum(leaf_width(n), width);
    if (fill && n.kind == UiStoreKind::text && n.rw)
        w = minimum(width, n.rw);
    if (n.kind == UiStoreKind::bar && n.count)
        w = width * n.count / 100.f;
    if (n.kind == UiStoreKind::spacer && !(n.flags & ui_store_flex))
        w = n.rw;
    n.w = w;
    n.h = leaf_height(n, w);
}

} // namespace

// ---- Builder ---------------------------------------------------------------------------------------------------

UiStoreBuilder::UiStoreBuilder(UiStoreScene* scene, bool dialog, bool bar) noexcept
    : scene_(scene), current_(nullptr), depth_(0), last_(kUiStoreNone), ok_(scene && scene->nodes) {
    if (!ok_)
        return;
    if (dialog)
        scene->dialog_bar = bar;
    open(UiStoreKind::column);
    (dialog ? scene->dialog_root : scene->page_root) = static_cast<u16>(scene->count - 1);
}

UiStoreNode& UiStoreBuilder::add(UiStoreKind kind) noexcept {
    static UiStoreNode sink;
    if (!ok_ || scene_->count >= kUiStoreNodeCapacity) {
        ok_ = false;
        sink = {};
        current_ = &sink;
        return sink;
    }
    const u16 index = static_cast<u16>(scene_->count++);
    auto& n = scene_->nodes[index];
    n = {};
    n.kind = kind;
    n.icon = n.icon2 = kNoIcon;
    n.first = n.next = kUiStoreNone;
    n.ink = static_cast<u8>(UiStoreInk::normal);
    if (depth_) {
        auto& parent = scene_->nodes[stack_[depth_ - 1]];
        // Append: the previous sibling is the last child; found by walking from the parent's last link.
        if (parent.first == kUiStoreNone)
            parent.first = index;
        else
            scene_->nodes[last_].next = index;
    }
    last_ = index;
    return *(current_ = &n);
}

UiStoreNode& UiStoreBuilder::open(UiStoreKind kind) noexcept {
    auto& n = add(kind);
    if (!ok_ || depth_ >= 24) {
        ok_ = false;
        return n;
    }
    const u16 index = static_cast<u16>(&n - scene_->nodes);
    stack_[depth_++] = index;
    last_ = kUiStoreNone;
    return n;
}

void UiStoreBuilder::close() noexcept {
    if (!depth_) {
        ok_ = false;
        return;
    }
    // After closing, the next sibling appends after this container: find the container's own position in
    // its parent chain.
    const u16 index = stack_[--depth_];
    last_ = index;
    current_ = &scene_->nodes[index];
}

UiStoreNode& UiStoreBuilder::leaf(UiStoreKind kind) noexcept {
    return add(kind);
}

UiStoreNode& UiStoreBuilder::text(const Utf8View& value, UiStoreType type, UiStoreInk ink, u16 flags) noexcept {
    auto& n = add(UiStoreKind::text);
    n.text = value;
    n.type = static_cast<u8>(type);
    n.ink = static_cast<u8>(ink);
    n.flags = flags;
    return n;
}

UiStoreNode& UiStoreBuilder::chip(const Utf8View& value, UiStoreTone tone, UiIcon icon) noexcept {
    auto& n = add(UiStoreKind::chip);
    n.text = value;
    n.variant = static_cast<u8>(tone);
    n.icon = icon_byte(icon);
    return n;
}

UiStoreNode& UiStoreBuilder::button(const Utf8View& value, UiStoreButton variant, u32 action, UiIcon icon, bool large) noexcept {
    auto& n = add(UiStoreKind::button);
    n.text = value;
    n.variant = static_cast<u8>(variant);
    n.action = action;
    n.icon = icon_byte(icon);
    n.type = large ? 1 : 0;
    return n;
}

UiStoreNode& UiStoreBuilder::icon_button(UiIcon icon, const Utf8View& name, u32 action, float size) noexcept {
    auto& n = add(UiStoreKind::icon_button);
    n.icon = icon_byte(icon);
    n.text = name;
    n.action = action;
    n.rw = size;
    return n;
}

__declspec(noinline) UiStoreNode& UiStoreBuilder::text(const Utf8View& value, UiStoreType type, UiStoreInk ink) noexcept {
    return text(value, type, ink, u16{0});
}

__declspec(noinline) UiStoreNode& UiStoreBuilder::button(const Utf8View& value, UiStoreButton variant, u32 action) noexcept {
    return button(value, variant, action, UiIcon::count, false);
}

__declspec(noinline) UiStoreNode& UiStoreBuilder::text(const char* value, UiStoreType type, UiStoreInk ink) noexcept {
    return text(Utf8View{value, c_string_length(value)}, type, ink, u16{0});
}

__declspec(noinline) UiStoreNode& UiStoreBuilder::button(const char* value, UiStoreButton variant, u32 action) noexcept {
    return button(Utf8View{value, c_string_length(value)}, variant, action, UiIcon::count, false);
}

__declspec(noinline) UiStoreNode& UiStoreBuilder::icon_button(UiIcon icon, const char* name, u32 action) noexcept {
    return icon_button(icon, Utf8View{name, c_string_length(name)}, action, 32.f);
}

__declspec(noinline) UiStoreNode& UiStoreBuilder::text(const char* value, UiStoreType type, UiStoreInk ink, u16 flags) noexcept {
    return text(Utf8View{value, c_string_length(value)}, type, ink, flags);
}

__declspec(noinline) UiStoreNode& UiStoreBuilder::chip(const char* value, UiStoreTone tone, UiIcon icon) noexcept {
    return chip(Utf8View{value, c_string_length(value)}, tone, icon);
}

__declspec(noinline) UiStoreNode& UiStoreBuilder::button(const char* value, UiStoreButton variant, u32 action, UiIcon icon,
                                                          bool large) noexcept {
    return button(Utf8View{value, c_string_length(value)}, variant, action, icon, large);
}

__declspec(noinline) UiStoreNode& UiStoreBuilder::icon_button(UiIcon icon, const char* name, u32 action, float size) noexcept {
    return icon_button(icon, Utf8View{name, c_string_length(name)}, action, size);
}

UiStoreNode& UiStoreBuilder::tile(UiIcon icon, float size, UiStoreTone tone) noexcept {
    auto& n = add(UiStoreKind::tile);
    n.icon = icon_byte(icon);
    n.rw = size;
    n.variant = static_cast<u8>(tone);
    return n;
}

UiStoreNode& UiStoreBuilder::spacer(float width, float height, bool flex) noexcept {
    auto& n = add(UiStoreKind::spacer);
    n.rw = width;
    n.rh = height;
    n.flags = flex ? ui_store_flex : 0;
    return n;
}

Utf8View UiStoreBuilder::gather(const Utf8View* const* parts, u32 count) noexcept {
    if (!ok_ || !scene_->text)
        return {};
    u32 bytes = 0;
    for (u32 i = 0; i < count; ++i)
        bytes += parts[i]->bytes;
    if (scene_->text_used + bytes > kUiStoreTextBytes) {
        ok_ = false;
        return {};
    }
    char* out = scene_->text + scene_->text_used;
    u32 at_byte = 0;
    for (u32 i = 0; i < count; ++i) {
        const Utf8View& part = *parts[i];
        if (part.bytes)
            copy_bytes(out + at_byte, part.data, part.bytes);
        at_byte += part.bytes;
    }
    scene_->text_used += bytes;
    return {out, bytes};
}

Utf8View UiStoreBuilder::copy(const Utf8View& a) noexcept {
    const Utf8View* const parts[]{&a};
    return gather(parts, 1);
}

Utf8View UiStoreBuilder::copy(const Utf8View& a, const Utf8View& b) noexcept {
    const Utf8View* const parts[]{&a, &b};
    return gather(parts, 2);
}

Utf8View UiStoreBuilder::copy(const Utf8View& a, const Utf8View& b, const Utf8View& c) noexcept {
    const Utf8View* const parts[]{&a, &b, &c};
    return gather(parts, 3);
}

Utf8View UiStoreBuilder::copy(const Utf8View& a, const Utf8View& b, const Utf8View& c, const Utf8View& d) noexcept {
    const Utf8View* const parts[]{&a, &b, &c, &d};
    return gather(parts, 4);
}

Utf8View UiStoreBuilder::number(u64 value) noexcept {
    char digits[20];
    return copy({digits, decimal_write(digits, value)});
}

// ---- Scene -----------------------------------------------------------------------------------------------------

UiStoreMeasure ui_store_cache_measure(UiTextMeasureCache* cache) noexcept {
    return {[](void* context, const Utf8View& text, u32 style) noexcept {
                return shell_text_width(static_cast<UiTextMeasureCache*>(context), 0x57A0'0001u,
                                        // The cache keys a width by source, text and item: one item per style.
                                        static_cast<u16>((style * 2654435761u) >> 16), text, style, 4096.f);
            },
            cache};
}

bool ui_store_scene_open(UiStoreScene* s) noexcept {
    if (!s || s->nodes)
        return s && s->nodes;
    const u32 sidecar = kUiStoreNodeCapacity * sizeof(UiStoreNode) + kUiStoreTextBytes;
    if (!ui_scene_open(&s->storage, &s->widgets, 8, sidecar, alignof(UiStoreNode)))
        return false;
    s->nodes = static_cast<UiStoreNode*>(s->storage.sidecar);
    s->text = reinterpret_cast<char*>(s->nodes + kUiStoreNodeCapacity);
    s->page_root = s->dialog_root = kUiStoreNone;
    s->root = ui_scene_root(&s->widgets, utf8_literal("Store"));
    // The text owner: its clip must not cut the page (the renderer clips a command to its owner's node).
    if (s->root.generation && ui_widget_manager_layout_rectangle(&s->widgets, {0, 0, 16384, 16384}).ok())
        return true;
    ui_scene_release(s);
    return false;
}

void ui_store_scene_release(UiStoreScene* s) noexcept {
    if (s)
        ui_scene_release(s);
}

void ui_store_scene_begin(UiStoreScene* s) noexcept {
    s->count = s->text_used = 0;
    s->page_root = s->dialog_root = s->dialog_footer = kUiStoreNone;
}

namespace {
float visible_height(const UiStoreScene& s) noexcept {
    return maximum(1.f, s.panel.bottom - s.column.top);
}
float page_most(const UiStoreScene& s) noexcept {
    return maximum(0.f, s.content_height - visible_height(s));
}
// The dialog body's height above its pinned footer.
float dialog_body(const UiStoreScene& s) noexcept {
    return s.dialog_rect.bottom - s.dialog_rect.top - (s.dialog_footer != kUiStoreNone ? s.nodes[s.dialog_footer].h : 0);
}
float dialog_most(const UiStoreScene& s) noexcept {
    return maximum(0.f, s.dialog_height - dialog_body(s));
}
void clamp_scroll(UiStoreScene* s) noexcept {
    const float most = page_most(*s);
    s->scroll = s->scroll < 0 ? 0 : s->scroll > most ? most : s->scroll;
    const float dmost = dialog_most(*s);
    s->dialog_scroll = s->dialog_scroll < 0 ? 0 : s->dialog_scroll > dmost ? dmost : s->dialog_scroll;
}

// The dialog column: 600 wide, centred in the modal; a body that scrolls above a pinned footer when it does not fit.
void layout_dialog(UiStoreScene* s) noexcept {
    s->dialog_rect = {};
    s->dialog_height = 0;
    if (s->dialog_root == kUiStoreNone)
        return;
    auto& root = s->nodes[s->dialog_root];
    if (s->dialog_bar) {
        // A floating bar at the bottom of the modal.
        const float limit = maximum(200.f, ui_rect_width(s->modal) - 48);
        const float bar = minimum(limit, intrinsic(*s, s->dialog_root));
        place(*s, s->dialog_root, 0, 0, bar);
        const float bar_left = s->modal.left + (ui_rect_width(s->modal) - bar) * .5f, bar_top = s->modal.bottom - 24 - root.h;
        s->dialog_rect = {bar_left, bar_top, bar_left + bar, bar_top + root.h};
        s->dialog_footer = kUiStoreNone;
        shift(*s, s->dialog_root, bar_left, bar_top);
        return;
    }
    const float width = minimum(600.f, maximum(320.f, ui_rect_width(s->modal) - 48));
    place(*s, s->dialog_root, 0, 0, width);
    u16 footer = kUiStoreNone;
    for (u16 c = root.first; c != kUiStoreNone; c = s->nodes[c].next)
        footer = c;
    const bool split = footer != kUiStoreNone && footer != root.first;
    s->dialog_footer = split ? footer : kUiStoreNone;
    const float footer_height = split ? s->nodes[footer].h : 0;
    const float limit = maximum(200.f, ui_rect_height(s->modal) - 48);
    const float total = root.h;
    const float height = minimum(total, limit);
    s->dialog_height = total - footer_height;
    const float left = s->modal.left + (ui_rect_width(s->modal) - width) * .5f;
    const float top = s->modal.top + (ui_rect_height(s->modal) - height) * .5f;
    s->dialog_rect = {left, top, left + width, top + height};
    shift(*s, s->dialog_root, left, top);
    if (split && total > height)
        shift(*s, footer, 0, height - total);
    root.h = height;
}
} // namespace

bool ui_store_scene_commit(UiStoreScene* s, const UiRect& panel, const UiRect& column, const UiRect& modal) noexcept {
    if (s)
        g_measure = s->measure;
    if (!s || !s->nodes || s->page_root == kUiStoreNone || !ui_rect_has_area(column))
        return false;
    s->panel = panel;
    s->column = column;
    s->modal = modal;
    place(*s, s->page_root, column.left, 0, ui_rect_width(column));
    s->content_height = s->nodes[s->page_root].h + kTailPadding;
    layout_dialog(s);
    s->dialog_open = s->dialog_root != kUiStoreNone;
    clamp_scroll(s);
    ++s->revision;
    return true;
}

bool ui_store_scene_scroll_to(UiStoreScene* s, float offset) noexcept {
    if (!s || !s->active)
        return false;
    const float before = s->scroll;
    s->scroll = offset;
    clamp_scroll(s);
    if (s->scroll == before)
        return false;
    ++s->revision;
    return true;
}

bool ui_store_scene_scroll(UiStoreScene* s, float delta) noexcept {
    if (!s)
        return false;
    if (s->dialog_open) {
        const float before = s->dialog_scroll;
        s->dialog_scroll += delta;
        clamp_scroll(s);
        if (s->dialog_scroll == before)
            return false;
        ++s->revision;
        return true;
    }
    return ui_store_scene_scroll_to(s, s->scroll + delta);
}

bool ui_store_scene_scroll_range(const UiStoreScene& s, bool dialog, UiRect* viewport, float* offset, float* most) noexcept {
    if (!s.active || (dialog && !s.dialog_open))
        return false;
    const UiRect& d = s.dialog_rect;
    *viewport = dialog ? UiRect{d.left, d.top, d.right, d.top + dialog_body(s)} : UiRect{s.panel.left, s.column.top, s.panel.right, s.panel.bottom};
    *offset = dialog ? s.dialog_scroll : s.scroll;
    *most = dialog ? dialog_most(s) : page_most(s);
    return ui_rect_has_area(*viewport);
}

namespace store_detail {
UiRect screen_rect(const UiStoreScene& s, u16 index) noexcept {
    const auto& n = s.nodes[index];
    float dy = 0;
    if (in_dialog(s, index))
        dy = s.dialog_footer != kUiStoreNone && index > s.dialog_root && index < s.dialog_footer ? -s.dialog_scroll : 0;
    else
        dy = s.column.top - s.scroll;
    return {n.x, n.y + dy, n.x + n.w, n.y + dy + n.h};
}
UiRect clip_of(const UiStoreScene& s, u16 index) noexcept {
    if (!in_dialog(s, index))
        return s.panel;
    UiRect clip = s.dialog_rect;
    if (s.dialog_footer != kUiStoreNone && index > s.dialog_root && index < s.dialog_footer)
        clip.bottom = s.nodes[s.dialog_footer].y;
    return clip;
}
u16 find_action(const UiStoreScene& s, u32 action) noexcept {
    for (u32 i = 0; action && i < s.count; ++i)
        if (s.nodes[i].action == action && interactive(s.nodes[i]))
            return static_cast<u16>(i);
    return kUiStoreNone;
}
} // namespace store_detail
} // namespace maicord
