#include "store_model.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::app_detail {
namespace {

char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}
bool is_word(char c) noexcept {
    return c != ' ' && c != '\t' && c != '-' && c != '_' && c != '.' && c != ',' && c != '/' && c != '(' && c != ')';
}
// True when `word` is a prefix (ASCII case-insensitive) of a word of `text`.
bool word_prefix(Utf8View word, Utf8View text) noexcept {
    for (u32 i = 0; i < text.bytes; ++i) {
        if (!is_word(text.data[i]) || (i && is_word(text.data[i - 1])))
            continue;
        if (text.bytes - i < word.bytes)
            return false;
        u32 k = 0;
        while (k < word.bytes && lower(text.data[i + k]) == lower(word.data[k]))
            ++k;
        if (k == word.bytes)
            return true;
    }
    return false;
}
int compare_names(Utf8View a, Utf8View b) noexcept {
    const u32 n = minimum(a.bytes, b.bytes);
    for (u32 i = 0; i < n; ++i) {
        const char x = lower(a.data[i]), y = lower(b.data[i]);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return a.bytes == b.bytes ? 0 : a.bytes < b.bytes ? -1 : 1;
}
bool same(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && (!a.bytes || bytes_equal(a.data, b.data, a.bytes));
}

// Strict weak order of two items under a sort, then the name.
bool before(const StoreItem& a, const StoreItem& b, CatalogKind kind, StoreSort sort) noexcept {
    if (sort == StoreSort::installed && a.order != b.order)
        return a.order < b.order;
    if (sort == StoreSort::newest && a.released != b.released)
        return a.released > b.released;
    if (sort == StoreSort::lightest) {
        const u8 x = kind == CatalogKind::effect ? a.cost : a.risk, y = kind == CatalogKind::effect ? b.cost : b.risk;
        if (x != y)
            return x < y;
    }
    return compare_names(a.name, b.name) < 0;
}
void insert_sorted(const StoreModel& model, u16* order, u32* count, u16 index, CatalogKind kind, StoreSort sort) noexcept {
    u32 at = *count;
    while (at && before(model.items[index], model.items[order[at - 1]], kind, sort))
        --at;
    for (u32 i = *count; i > at; --i)
        order[i] = order[i - 1];
    order[at] = index;
    ++*count;
}

constexpr Utf8View kBaseNames[]{utf8_literal("Dark"), utf8_literal("Light"), utf8_literal("AMOLED")};

} // namespace

bool store_item_matches(const StoreItem& item, Utf8View query) noexcept {
    u32 pos = 0;
    while (pos < query.bytes) {
        while (pos < query.bytes && !is_word(query.data[pos]))
            ++pos;
        u32 end = pos;
        while (end < query.bytes && is_word(query.data[end]))
            ++end;
        if (end == pos)
            break;
        const Utf8View word{query.data + pos, end - pos};
        bool hit = word_prefix(word, item.name) || word_prefix(word, item.summary) || word_prefix(word, item.authors);
        for (u32 i = 0; !hit && i < item.category_count; ++i)
            hit = word_prefix(word, item.categories[i]);
        if (!hit)
            return false;
        pos = end;
    }
    return true;
}

void store_chips_build(const StoreModel& model, CatalogKind kind, StoreChips* out) noexcept {
    out->count = 1;
    out->chips[0] = {utf8_literal("All"), 0, 0, 0, 0};
    u32 listed = 0, base_count[3]{}, light = 0, low = 0;
    for (u32 i = 0; i < model.count; ++i) {
        const auto& item = model.items[i];
        if (item.kind != kind || item.item == ~0u)
            continue;
        ++listed;
        if (kind == CatalogKind::theme && item.base < 3)
            ++base_count[item.base];
        if (kind == CatalogKind::effect && item.cost == static_cast<u8>(CatalogCost::light))
            ++light;
        if (kind == CatalogKind::plugin && item.risk == 0)
            ++low;
    }
    out->chips[0].count = listed;
    const auto add = [&](Utf8View label, u32 count, u8 group, u8 type, u8 value) noexcept {
        if (out->count < kStoreChipMaximum)
            out->chips[out->count++] = {label, count, group, type, value};
    };
    if (kind == CatalogKind::theme)
        for (u32 b = 0; b < 3; ++b)
            if (base_count[b])
                add(kBaseNames[b], base_count[b], 0, 1, static_cast<u8>(b));
    if (kind == CatalogKind::effect && light)
        add(utf8_literal("Light cost"), light, 0, 2, 0);
    // Categories, distinct, sorted by name.
    const u32 first = out->count;
    for (u32 i = 0; i < model.count; ++i) {
        const auto& item = model.items[i];
        if (item.kind != kind || item.item == ~0u)
            continue;
        for (u32 c = 0; c < item.category_count; ++c) {
            u32 at = first;
            while (at < out->count && !same(out->chips[at].label, item.categories[c]))
                ++at;
            if (at < out->count)
                ++out->chips[at].count;
            else if (out->count < kStoreChipMaximum && item.categories[c].bytes)
                out->chips[out->count++] = {item.categories[c], 1, 1, 3, 0};
        }
    }
    for (u32 i = first + 1; i < out->count; ++i) {
        const StoreChip chip = out->chips[i];
        u32 at = i;
        while (at > first && compare_names(chip.label, out->chips[at - 1].label) < 0) {
            out->chips[at] = out->chips[at - 1];
            --at;
        }
        out->chips[at] = chip;
    }
    if (kind == CatalogKind::plugin && low)
        add(utf8_literal("Low access only"), low, 2, 4, 0);
}

u64 store_item_chips(const StoreItem& item, const StoreChips& chips) noexcept {
    u64 bits = 0;
    for (u32 i = 1; i < chips.count; ++i) {
        const auto& chip = chips.chips[i];
        bool on = false;
        switch (chip.type) {
        case 1:
            on = item.base == chip.value;
            break;
        case 2:
            on = item.cost == static_cast<u8>(CatalogCost::light);
            break;
        case 3:
            for (u32 c = 0; !on && c < item.category_count; ++c)
                on = same(item.categories[c], chip.label);
            break;
        case 4:
            on = item.risk == 0;
            break;
        default:
            break;
        }
        if (on)
            bits |= u64{1} << i;
    }
    return bits;
}

bool store_chips_match(u64 selected, u64 item, const StoreChips& chips) noexcept {
    if (!selected)
        return true;
    for (u32 group = 0; group < 3; ++group) {
        u64 mask = 0;
        for (u32 i = 1; i < chips.count; ++i)
            if (chips.chips[i].group == group)
                mask |= u64{1} << i;
        if ((selected & mask) && !(selected & mask & item))
            return false;
    }
    return true;
}

u64 store_chip_toggle(u64 selected, const StoreChips& chips, u32 index) noexcept {
    if (index == 0 || index >= chips.count)
        return 0;
    return selected ^ (u64{1} << index);
}

void store_browse_list(const StoreModel& model, const StorePageState& state, const StoreChips& chips,
                       StoreList* out) noexcept {
    out->count = out->total = 0;
    const Utf8View query{state.query, state.query_bytes};
    for (u32 i = 0; i < model.count; ++i) {
        const auto& item = model.items[i];
        if (item.kind != state.kind || item.item == ~0u)
            continue;
        ++out->total;
        if (!store_chips_match(state.chips, store_item_chips(item, chips), chips) || !store_item_matches(item, query))
            continue;
        insert_sorted(model, out->order, &out->count, static_cast<u16>(i), state.kind, state.sort);
    }
}

void store_installed_list(const StoreModel& model, const StorePageState& state, StoreList* out) noexcept {
    out->count = out->total = 0;
    out->counts[0] = out->counts[1] = out->counts[2] = out->counts[3] = 0;
    const Utf8View query{state.query, state.query_bytes};
    for (u32 i = 0; i < model.count; ++i) {
        const auto& item = model.items[i];
        if (item.kind != state.kind || !item.installed)
            continue;
        ++out->total;
        ++out->counts[0];
        ++out->counts[item.enabled ? 1 : 2];
        out->counts[3] += item.update ? 1 : 0;
        const bool segment = state.filter == StoreFilter::all || (state.filter == StoreFilter::on && item.enabled) ||
                             (state.filter == StoreFilter::off && !item.enabled) ||
                             (state.filter == StoreFilter::updates && item.update);
        if (!segment || !store_item_matches(item, query))
            continue;
        insert_sorted(model, out->order, &out->count, static_cast<u16>(i), state.kind, StoreSort::installed);
    }
}

void store_updates_list(const StoreModel& model, StoreList* out) noexcept {
    out->count = out->total = 0;
    for (u32 i = 0; i < model.count; ++i) {
        const auto& item = model.items[i];
        if (item.kind != CatalogKind::plugin || !item.installed || !item.update)
            continue;
        ++out->total;
        insert_sorted(model, out->order, &out->count, static_cast<u16>(i), CatalogKind::plugin, StoreSort::name);
    }
}

u32 store_update_count(const StoreModel& model, CatalogKind kind) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < model.count; ++i)
        count += model.items[i].kind == kind && model.items[i].installed && model.items[i].update ? 1 : 0;
    return count;
}

u32 store_installed_count(const StoreModel& model, CatalogKind kind) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < model.count; ++i)
        count += model.items[i].kind == kind && model.items[i].installed ? 1 : 0;
    return count;
}

u32 store_find_item(const StoreModel& model, CatalogKind kind, Utf8View slug) noexcept {
    for (u32 i = 0; i < model.count; ++i)
        if (model.items[i].kind == kind && same(model.items[i].slug, slug))
            return i;
    return ~0u;
}

StoreTab store_initial_tab(const StoreModel& model, CatalogKind kind) noexcept {
    return store_installed_count(model, kind) ? StoreTab::installed : StoreTab::browse;
}

} // namespace maicord::app_detail
