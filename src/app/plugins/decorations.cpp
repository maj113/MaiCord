// Plugin decorations: the per-plugin verdict caches filled by decorate hooks (pull, in the application's prepare
// pass) and by mc_api.decorate_push, their merge for the UI and the queries the scenes read. No query runs plugin code.
#include "surfaces_internal.hpp"
#include "maicord/app/plugins/text.hpp"
#include "maicord/app/plugins/call_ledger.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/plugins/render.hpp"
#include "maicord/ui/plugins/slots.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {

namespace plugin_surfaces_detail {
Plugin g_plugins[kMaximumPlugins];
u32 g_plugin_count;
} // namespace plugin_surfaces_detail

namespace {
using namespace plugin_surfaces_detail;

static_assert(kKinds == MC_HOOK_DECORATE_HEADER - MC_HOOK_DECORATE_MESSAGE + 1);
static_assert(kUiPluginRenderBadges == MC_DECORATION_BADGES && kUiPluginRenderAccessories == MC_DECORATION_ACCESSORIES &&
              kUiPluginRenderCardButtons == MC_CARD_BUTTONS);

// Entries and ring bytes per cache, by kind: about 150 KiB committed for a plugin that decorates messages.
constexpr u32 kEntries[kKinds] = {256, 128, 64, 32, 4};
constexpr u32 kArenaBytes[kKinds] = {96u << 10, 24u << 10, 8u << 10, 4u << 10, 3u << 10};
constexpr u32 kProbe = 4;
constexpr u32 kStale = ~0u;

// ---- Records ---------------------------------------------------------------------------------------
// A verdict is one record in a ring: Head, the badge and accessory nodes, an optional card, then every string
// back to back in a fixed order (see encode). Lengths are in the fixed parts, so reading needs no scanning.
constexpr u8 kReplaceContent = 1, kHasCard = 2;
struct Head {
    u16 bytes; // the whole record, a multiple of 8
    u8 visibility, media, name_tone, flags, badges, accessories;
    u16 label, prefix, suffix, content;
    u16 reserved[4];
};
struct NodeRecord {
    u64 tag;
    u16 text, tooltip, value, icon;
    u8 kind, tone;
    u16 reserved;
};
struct ButtonRecord {
    u64 tag;
    u16 label;
    u8 tone;
    u8 reserved[5];
};
struct CardRecord {
    u16 title, text, caption, link, thumbnail;
    u8 tone, buttons;
    ButtonRecord button[MC_CARD_BUTTONS];
};
static_assert(sizeof(Head) == 24 && sizeof(NodeRecord) == 24 && sizeof(ButtonRecord) == 16 && sizeof(CardRecord) == 48);

// A record read back: every view points into the ring.
struct Decoded {
    u8 visibility, media, name_tone;
    bool replace_content, has_card;
    Utf8View label, prefix, suffix, content;
    u32 badge_count, accessory_count;
    UiPluginRenderNode badges[MC_DECORATION_BADGES];
    UiPluginRenderNode accessories[MC_DECORATION_ACCESSORIES];
    UiPluginRenderCard card;
};

void decode(const u8* record, u64 owner, Decoded* out) noexcept {
    const Head* head = reinterpret_cast<const Head*>(record);
    const NodeRecord* nodes = reinterpret_cast<const NodeRecord*>(record + sizeof(Head));
    const u32 node_count = head->badges + head->accessories;
    const u8* after = record + sizeof(Head) + node_count * sizeof(NodeRecord);
    const CardRecord* card = (head->flags & kHasCard) ? reinterpret_cast<const CardRecord*>(after) : nullptr;
    const char* at = reinterpret_cast<const char*>(after + (card ? sizeof(CardRecord) : 0));
    const auto take = [&](u32 bytes) noexcept {
        const Utf8View view{at, bytes};
        at += bytes;
        return view;
    };
    out->visibility = head->visibility;
    out->media = head->media;
    out->name_tone = head->name_tone;
    out->replace_content = (head->flags & kReplaceContent) != 0;
    out->has_card = card != nullptr;
    out->label = take(head->label);
    out->prefix = take(head->prefix);
    out->suffix = take(head->suffix);
    out->content = take(head->content);
    out->badge_count = head->badges;
    out->accessory_count = head->accessories;
    for (u32 i = 0; i < node_count; ++i) {
        const NodeRecord& node = nodes[i];
        UiPluginRenderNode& view = i < head->badges ? out->badges[i] : out->accessories[i - head->badges];
        view.owner_id = owner;
        view.action_id = node.tag;
        view.text = take(node.text);
        view.tooltip = take(node.tooltip);
        view.value = take(node.value);
        view.icon = node.icon;
        view.kind = node.kind;
        view.tone = node.tone;
    }
    if (card) {
        UiPluginRenderCard& view = out->card;
        view.owner_id = owner;
        view.tone = card->tone;
        view.title = take(card->title);
        view.text = take(card->text);
        view.caption = take(card->caption);
        view.link = take(card->link);
        view.thumbnail = take(card->thumbnail);
        view.button_count = card->buttons;
        for (u32 i = 0; i < card->buttons; ++i) {
            view.buttons[i] = {card->button[i].tag, take(card->button[i].label), card->button[i].tone};
        }
    }
}

// ---- Validation and encoding -----------------------------------------------------------------------

// UTF-8 without control characters (a line feed stays in multi-line text), at most maximum bytes.
bool text_ok(const mc_str& text, u32 maximum, bool multiline = false) noexcept {
    return text.bytes <= maximum && plugin_text_clean(text.data, text.bytes, multiline ? kPluginTextLines : 0);
}
bool url_ok(mc_str url, u32 maximum) noexcept {
    constexpr char kScheme[] = "https://";
    if (url.bytes <= sizeof(kScheme) - 1 || !text_ok(url, maximum)) return false;
    for (u32 i = 0; i < url.bytes; ++i)
        if (i < sizeof(kScheme) - 1 ? url.data[i] != kScheme[i] : url.data[i] == ' ') return false;
    return true;
}
bool tag_ok(u64 tag) noexcept { return tag && !(tag & kButtonAction); }

bool node_ok(const mc_node& node) noexcept {
    if (node.kind > MC_NODE_LINK || node.tone > MC_TONE_CRITICAL) return false;
    const u32 limit = node.kind == MC_NODE_LABEL || node.kind == MC_NODE_LINK ? MC_NODE_TEXT_MAX : MC_NODE_SHORT_MAX;
    if (!text_ok(node.text, limit) || !text_ok(node.icon, 64) || !text_ok(node.tooltip, MC_NODE_TOOLTIP_MAX)) return false;
    const bool text = node.text.bytes != 0;
    switch (node.kind) {
    case MC_NODE_LINK: return text && url_ok(node.value, MC_URL_MAX);
    case MC_NODE_BUTTON: if (!text || !tag_ok(node.tag)) return false; break;
    case MC_NODE_LABEL: if (!text) return false; break;
    default: if (!text && !node.icon.bytes) return false; break;
    }
    return !node.value.bytes;
}

bool card_ok(const mc_card& card) noexcept {
    if (!text_ok(card.title, MC_CARD_TITLE_MAX) || !text_ok(card.text, MC_CARD_TEXT_MAX, true) ||
        !text_ok(card.caption, MC_CARD_TITLE_MAX) || card.tone > MC_TONE_CRITICAL || card.button_count > MC_CARD_BUTTONS ||
        (!card.title.bytes && !card.text.bytes))
        return false;
    if ((card.link.bytes && !url_ok(card.link, MC_URL_MAX)) || (card.thumbnail.bytes && !url_ok(card.thumbnail, MC_URL_MAX)))
        return false;
    for (u32 i = 0; i < card.button_count; ++i) {
        const mc_card_button& button = card.buttons[i];
        if (!button.label.bytes || !text_ok(button.label, MC_NODE_SHORT_MAX) || !tag_ok(button.tag) ||
            button.tone > MC_TONE_CRITICAL)
            return false;
    }
    return true;
}

bool decoration_ok(const mc_decoration& d) noexcept {
    if (d.visibility > MC_VIS_HIDDEN || d.media > MC_MEDIA_HIDE || d.name_tone > MC_TONE_ROLE ||
        (d.flags & ~MC_DECO_REPLACE_CONTENT) || d.badge_count > MC_DECORATION_BADGES ||
        d.accessory_count > MC_DECORATION_ACCESSORIES || d.has_card > 1)
        return false;
    if (!text_ok(d.collapsed_label, 64) || !text_ok(d.name_prefix, MC_NODE_SHORT_MAX) ||
        !text_ok(d.name_suffix, MC_NODE_SHORT_MAX) || !text_ok(d.content, MC_DECORATION_CONTENT_MAX, true) ||
        ((d.flags & MC_DECO_REPLACE_CONTENT) && !d.content.bytes))
        return false;
    for (u32 i = 0; i < d.badge_count; ++i)
        if (!node_ok(d.badges[i])) return false;
    for (u32 i = 0; i < d.accessory_count; ++i)
        if (!node_ok(d.accessories[i])) return false;
    return !d.has_card || card_ok(d.card);
}

// Whether the decoration changes anything on screen.
bool decoration_empty(const mc_decoration& d) noexcept {
    return !d.visibility && !d.media && !d.name_prefix.bytes && !d.name_suffix.bytes && !d.badge_count &&
           !d.accessory_count && !d.has_card && !(d.flags & MC_DECO_REPLACE_CONTENT);
}

// Writes into a record, or only counts when out is null.
struct Sink {
    u8* out;
    u32 size;
    void put(const void* data, u32 bytes) noexcept {
        if (out && bytes) copy_bytes(out + size, data, bytes);
        size += bytes;
    }
    void text(const mc_str& text) noexcept { put(text.data, text.bytes); }
};

void encode(Sink& s, const mc_decoration& d, u32 total) noexcept {
    Head head{};
    head.bytes = static_cast<u16>(total);
    head.visibility = static_cast<u8>(d.visibility);
    head.media = static_cast<u8>(d.media);
    head.name_tone = static_cast<u8>(d.name_tone);
    head.flags = static_cast<u8>(((d.flags & MC_DECO_REPLACE_CONTENT) ? kReplaceContent : 0) | (d.has_card ? kHasCard : 0));
    head.badges = static_cast<u8>(d.badge_count);
    head.accessories = static_cast<u8>(d.accessory_count);
    head.label = static_cast<u16>(d.collapsed_label.bytes);
    head.prefix = static_cast<u16>(d.name_prefix.bytes);
    head.suffix = static_cast<u16>(d.name_suffix.bytes);
    head.content = static_cast<u16>(d.content.bytes);
    s.put(&head, sizeof(head));
    const auto node_list = [&](const mc_node* nodes, u32 count) noexcept {
        for (u32 i = 0; i < count; ++i) {
            const mc_node& node = nodes[i];
            NodeRecord record{};
            record.tag = node.kind == MC_NODE_BUTTON ? node.tag : 0;
            record.text = static_cast<u16>(node.text.bytes);
            record.tooltip = static_cast<u16>(node.tooltip.bytes);
            record.value = static_cast<u16>(node.value.bytes);
            record.icon = node.icon.bytes ? plugin_icon({node.icon.data, node.icon.bytes}) : kNoIcon;
            record.kind = static_cast<u8>(node.kind);
            record.tone = static_cast<u8>(node.tone);
            s.put(&record, sizeof(record));
        }
    };
    node_list(d.badges, d.badge_count);
    node_list(d.accessories, d.accessory_count);
    if (d.has_card) {
        CardRecord card{};
        card.title = static_cast<u16>(d.card.title.bytes);
        card.text = static_cast<u16>(d.card.text.bytes);
        card.caption = static_cast<u16>(d.card.caption.bytes);
        card.link = static_cast<u16>(d.card.link.bytes);
        card.thumbnail = static_cast<u16>(d.card.thumbnail.bytes);
        card.tone = static_cast<u8>(d.card.tone);
        card.buttons = static_cast<u8>(d.card.button_count);
        for (u32 i = 0; i < d.card.button_count; ++i)
            card.button[i] = {d.card.buttons[i].tag, static_cast<u16>(d.card.buttons[i].label.bytes),
                              static_cast<u8>(d.card.buttons[i].tone), {}};
        s.put(&card, sizeof(card));
    }
    s.text(d.collapsed_label);
    s.text(d.name_prefix);
    s.text(d.name_suffix);
    s.text(d.content);
    const auto strings = [&](const mc_node* nodes, u32 count) noexcept {
        for (u32 i = 0; i < count; ++i) {
            s.text(nodes[i].text);
            s.text(nodes[i].tooltip);
            s.text(nodes[i].value);
        }
    };
    strings(d.badges, d.badge_count);
    strings(d.accessories, d.accessory_count);
    if (d.has_card) {
        s.text(d.card.title);
        s.text(d.card.text);
        s.text(d.card.caption);
        s.text(d.card.link);
        s.text(d.card.thumbnail);
        for (u32 i = 0; i < d.card.button_count; ++i) s.text(d.card.buttons[i].label);
    }
}

// ---- Caches ------------------------------------------------------------------------------------------

// Multiplicative (Fibonacci) hashing: consecutive snowflake ids spread over the table's low bits.
u32 home(u64 key, u64 scope) noexcept {
    return static_cast<u32>((key * 0x9E37'79B9'7F4A'7C15ull + (scope + 1) * 0xC2B2'AE3D'27D4'EB4Full) >> 32);
}

// An entry's record is still in the ring (or it has none).
bool resident(const Cache& cache, const Entry& entry) noexcept {
    if (!entry.state) return false;
    if (!entry.bytes) return true;
    return entry.lap == cache.lap || (entry.lap + 1 == cache.lap && entry.offset >= cache.head);
}

// Older entries rank lower: empty and overwritten first, then by position in the ring's history.
u64 rank(const Cache& cache, const Entry& entry) noexcept {
    if (!resident(cache, entry)) return 0;
    if (!entry.bytes) return 1;
    return (static_cast<u64>(entry.lap) << 32 | entry.offset) + 2;
}

Entry* find(Cache& cache, u64 key, u64 scope, bool create) noexcept {
    const u32 at = home(key, scope);
    Entry* victim = nullptr;
    u64 lowest = ~0ull;
    for (u32 i = 0; i < kProbe && i <= cache.mask; ++i) {
        Entry& entry = cache.entries[(at + i) & cache.mask];
        if (entry.state && entry.key == key && entry.scope == scope && resident(cache, entry)) return &entry;
        const u64 r = rank(cache, entry);
        if (r < lowest) {
            lowest = r;
            victim = &entry;
        }
    }
    if (!create) return nullptr;
    reset_value(victim);
    return victim;
}

bool ensure_memory(Plugin& plugin) noexcept {
    if (plugin.memory) return true;
    u32 total = 0;
    for (u32 k = 0; k < kKinds; ++k) total += kEntries[k] * sizeof(Entry) + kArenaBytes[k];
    u8* memory = static_cast<u8*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return false;
    u8* at = memory;
    for (u32 k = 0; k < kKinds; ++k) {
        Cache& cache = plugin.caches[k];
        cache.entries = reinterpret_cast<Entry*>(at);
        at += kEntries[k] * sizeof(Entry);
        cache.arena = at;
        at += kArenaBytes[k];
        cache.mask = kEntries[k] - 1;
        cache.arena_bytes = kArenaBytes[k];
        cache.lap = 1;
    }
    plugin.memory = memory;
    return true;
}

// Space for a record at the ring's head; wrapping counts a lap and strands what the head passed, so views the UI
// holds may dangle: the caller reports a change when this wraps.
u8* reserve(Cache& cache, u32 bytes, u32* offset, bool* wrapped) noexcept {
    if (bytes > cache.arena_bytes) return nullptr;
    if (cache.head + bytes > cache.arena_bytes) {
        cache.head = 0;
        ++cache.lap;
        *wrapped = true;
    }
    *offset = cache.head;
    cache.head += bytes;
    return cache.arena + *offset;
}

// Stores a verdict for (key, scope); a null or empty decoration is an entry without a record. State 1 is a pulled
// verdict, 2 a pushed one. Returns whether what the UI shows may have changed.
bool store(Plugin& plugin, u32 kind, u64 key, u64 scope, u32 revision, u8 state, const mc_decoration* d) noexcept {
    if (!ensure_memory(plugin)) return false;
    Cache& cache = plugin.caches[kind];
    Entry* existing = find(cache, key, scope, false);
    const bool had = existing && existing->bytes;
    u32 size = 0;
    if (d && !decoration_empty(*d)) {
        Sink measure{nullptr, 0};
        encode(measure, *d, 0);
        size = (measure.size + 7u) & ~7u;
    }
    u32 offset = 0;
    bool wrapped = false;
    u8* record = size ? reserve(cache, size, &offset, &wrapped) : nullptr;
    if (size && !record) return false;
    if (record) {
        Sink write{record, 0};
        encode(write, *d, size);
        clear_bytes(record + write.size, size - write.size);
    }
    Entry* entry = find(cache, key, scope, true);
    *entry = {key, scope, revision, plugin.generation[kind], offset, cache.lap, static_cast<u16>(size), state, 0};
    return had || size || wrapped;
}

// The entry holding the plugin's current verdict for the entity: a push wins, else a pulled one answering revision
// (revision == kStale: any, for the queries that don't know it).
const Entry* current_entry(Plugin& plugin, u32 kind, u64 key, u64 scope, u64 push_scope, u32 revision,
                           bool any_revision) noexcept {
    if (!plugin.memory) return nullptr;
    Cache& cache = plugin.caches[kind];
    if (plugin.pushed & (1u << kind)) {
        Entry* pushed = find(cache, key, push_scope, false);
        if (pushed && pushed->state == 2) return pushed;
    }
    Entry* pulled = find(cache, key, scope, false);
    if (!pulled) return nullptr;
    if (pulled->state == 2) return pulled;
    return any_revision || (pulled->revision == revision) ? pulled : nullptr;
}

const u8* record_of(const Plugin& plugin, u32 kind, const Entry& entry) noexcept {
    return entry.bytes ? plugin.caches[kind].arena + entry.offset : nullptr;
}

// ---- Session reveals of collapsed messages -------------------------------------------------------------------

constexpr u32 kReveals = 64;
u64 g_reveals[kReveals];
u32 g_reveal_count, g_reveal_cursor;

bool revealed(u64 message_id) noexcept {
    for (u32 i = 0; i < g_reveal_count; ++i)
        if (g_reveals[i] == message_id) return true;
    return false;
}

// ---- Merge ------------------------------------------------------------------------------------------------

// Joined name affixes live here; a wrap reports a change like a cache wrap does.
char g_join[4096];
u32 g_join_head;

Utf8View join(const Utf8View& first, const Utf8View& second) noexcept {
    if (!first.bytes) return second;
    if (!second.bytes) return first;
    constexpr u32 kJoined = 96;
    const u32 head = utf8_truncated_length(first, kJoined - 2);
    const u32 tail = utf8_truncated_length(second, kJoined - 1 - head);
    const u32 bytes = head + 1 + tail;
    if (g_join_head + bytes > sizeof(g_join)) {
        g_join_head = 0;
        changed(true);
    }
    char* at = g_join + g_join_head;
    copy_bytes(at, first.data, head);
    at[head] = ' ';
    copy_bytes(at + head + 1, second.data, tail);
    g_join_head += bytes;
    return {at, bytes};
}

// Which user wrote each message the application prepared a MESSAGE_AUTHOR decoration for, so the message's verdict
// can carry the author's name prefix and suffix. Direct-mapped; a lost entry just drops the affixes until the
// next prepare pass.
struct AuthorSlot {
    u64 message, author;
};
constexpr u32 kAuthorSlots = 512;
AuthorSlot g_authors[kAuthorSlots];
AuthorSlot& author_slot(u64 message_id) noexcept { return g_authors[home(message_id, 0) & (kAuthorSlots - 1)]; }
u64 author_of(u64 message_id) noexcept {
    const AuthorSlot& slot = author_slot(message_id);
    return slot.message == message_id ? slot.author : 0;
}

// A decoration's name prefix and suffix: joined after earlier plugins', the first affix's tone kept.
void merge_affix(UiPluginRenderDecoration* out, const Decoded& in, bool* have_affix) noexcept {
    if ((in.prefix.bytes || in.suffix.bytes) && !*have_affix) {
        out->name_tone = in.name_tone;
        *have_affix = true;
    }
    out->name_prefix = join(out->name_prefix, in.prefix);
    out->name_suffix = join(out->name_suffix, in.suffix);
}

// Message verdicts of every plugin, merged in enable order: the strongest visibility and media win (the
// collapsed label is the first one's), name affixes are joined, replacement content and the card are the first
// plugin's, nodes append up to the limits.
void merge(UiPluginRenderDecoration* out, const Decoded& in, bool* have_affix) noexcept {
    if (in.visibility > out->visibility) {
        out->visibility = in.visibility;
        if (in.label.bytes) out->collapsed_label = in.label;
    }
    if (in.media > out->media) out->media = in.media;
    merge_affix(out, in, have_affix);
    if (in.replace_content && !out->replace_content) {
        out->replace_content = true;
        out->content = in.content;
    }
    for (u32 i = 0; i < in.badge_count && out->badge_count < kUiPluginRenderBadges; ++i)
        out->badges[out->badge_count++] = in.badges[i];
    for (u32 i = 0; i < in.accessory_count && out->accessory_count < kUiPluginRenderAccessories; ++i)
        out->accessories[out->accessory_count++] = in.accessories[i];
    if (in.has_card && !out->has_card) {
        out->has_card = true;
        out->card = in.card;
    }
}

bool render_query(u64 message_id, u32 revision, UiPluginRenderDecoration* out) noexcept {
    bool any = false, affix = false;
    for (u32 i = 0; i < g_plugin_count; ++i) {
        Plugin& plugin = g_plugins[i];
        const Entry* entry = current_entry(plugin, kKindMessage, message_id, 0, 0, revision, false);
        const u8* record = entry ? record_of(plugin, kKindMessage, *entry) : nullptr;
        if (!record) continue;
        Decoded decoded;
        decode(record, plugin.binding->owner_id, &decoded);
        merge(out, decoded, &affix);
        any = true;
    }
    // The author's own decorations add their name prefix and suffix (a plugin that shows usernames).
    if (const u64 author = author_of(message_id)) {
        for (u32 i = 0; i < g_plugin_count; ++i) {
            Plugin& plugin = g_plugins[i];
            const Entry* entry = current_entry(plugin, kKindUser, author, kPluginUserMessageAuthor, 0, 0, true);
            const u8* record = entry ? record_of(plugin, kKindUser, *entry) : nullptr;
            if (!record) continue;
            Decoded decoded;
            decode(record, plugin.binding->owner_id, &decoded);
            if (!decoded.prefix.bytes && !decoded.suffix.bytes) continue;
            merge_affix(out, decoded, &affix);
            any = true;
        }
    }
    if (any && out->visibility == ui_plugin_render_collapsed && revealed(message_id))
        out->visibility = ui_plugin_render_normal;
    return any;
}

// The nodes of every plugin's verdict for one entity: badges (rows, names, icons) or accessories (profile rows).
u32 entity_nodes(u32 kind, u64 key, u64 scope, u64 push_scope, bool accessories, Slice<UiPluginSlotNode> output) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < g_plugin_count && count < output.count; ++i) {
        Plugin& plugin = g_plugins[i];
        const Entry* entry = current_entry(plugin, kind, key, scope, push_scope, 0, true);
        const u8* record = entry ? record_of(plugin, kind, *entry) : nullptr;
        if (!record) continue;
        Decoded decoded;
        decode(record, plugin.binding->owner_id, &decoded);
        const UiPluginRenderNode* nodes = accessories ? decoded.accessories : decoded.badges;
        const u32 n = accessories ? decoded.accessory_count : decoded.badge_count;
        for (u32 j = 0; j < n && count < output.count; ++j)
            output[count++] = {nodes[j].owner_id, nodes[j].action_id, nodes[j].text, nodes[j].tooltip, nodes[j].icon,
                               nodes[j].kind, nodes[j].tone, nodes[j].value};
    }
    return count;
}

// A member row's name prefix and suffix, joined across plugins: two label nodes (either text may be empty).
u32 member_name_nodes(u64 user_id, Slice<UiPluginSlotNode> output) noexcept {
    if (output.count < 2) return 0;
    Utf8View prefix{}, suffix{};
    u8 tone = 0;
    for (u32 i = 0; i < g_plugin_count; ++i) {
        Plugin& plugin = g_plugins[i];
        const Entry* entry = current_entry(plugin, kKindUser, user_id, kPluginUserMemberRow, 0, 0, true);
        const u8* record = entry ? record_of(plugin, kKindUser, *entry) : nullptr;
        if (!record) continue;
        Decoded decoded;
        decode(record, plugin.binding->owner_id, &decoded);
        if (!prefix.bytes && !suffix.bytes) tone = decoded.name_tone;
        prefix = join(prefix, decoded.prefix);
        suffix = join(suffix, decoded.suffix);
    }
    if (!prefix.bytes && !suffix.bytes) return 0;
    output[0] = {0, 0, prefix, {}, kNoIcon, ui_plugin_node_label, tone, {}};
    output[1] = {0, 0, suffix, {}, kNoIcon, ui_plugin_node_label, tone, {}};
    return 2;
}

u32 slot_query(u32 surface, u64 channel_id, u64 message_id, u64 user_id, u64 guild_id,
               Slice<UiPluginSlotNode> output) noexcept {
    (void)guild_id;
    if (!output.data || !output.count) return 0;
    u32 count = 0;
    switch (surface) {
    case kUiPluginSlotMessageAccessory:
        return entity_nodes(kKindUser, user_id, MC_USER_MESSAGE_AUTHOR, 0, false, output);
    case kUiPluginSlotMemberBadge:
        return entity_nodes(kKindUser, user_id, MC_USER_MEMBER_ROW, 0, false, output);
    case kUiPluginSlotMemberName: return member_name_nodes(user_id, output);
    case kUiPluginSlotProfileSection:
        return entity_nodes(kKindUser, user_id, MC_USER_PROFILE, 0, true, output);
    case kUiPluginSlotChannelRow: return entity_nodes(kKindChannel, channel_id, 0, 0, false, output);
    case kUiPluginSlotGuildRail: return entity_nodes(kKindGuild, guild_id, 0, 0, false, output);
    case kUiPluginSlotHeaderToolbar:
        // The header has room for one chip: a button, being actionable, comes before a decoration.
        count = button_slots(surface, 0, output);
        return count + entity_nodes(kKindHeader, MC_HEADER_CHANNEL, 0, 0, false, {output.data + count, output.count - count});
    case kUiPluginSlotMemberListHeader: return entity_nodes(kKindHeader, MC_HEADER_MEMBER_LIST, 0, 0, false, output);
    default: return button_slots(surface, message_id, output);
    }
}

// ---- Pull ---------------------------------------------------------------------------------------------------

template <typename In, typename Function> struct DecorateCall {
    Function function;
    const In* input;
    mc_decoration* output;
    static mc_status run(mc_ctx* ctx, void* argument) noexcept {
        const auto* call = static_cast<DecorateCall*>(argument);
        return call->function(ctx, call->input, call->output);
    }
};
template <typename In, typename Function>
PluginCallResult call_hook(PluginBinding* binding, Function function, const In* input, mc_decoration* output) noexcept {
    DecorateCall<In, Function> call{function, input, output};
    return plugin_binding_call(binding, PluginCallKind::decorate, PluginCallClass::decorate, 0,
                               DecorateCall<In, Function>::run, &call);
}

PluginCallResult call_kind(Plugin& plugin, u32 kind, const void* input, mc_decoration* output) noexcept {
    const mc_hook_fn fn = plugin.hooks[kind]->fn;
    PluginBinding* binding = plugin.binding;
    switch (kind) {
    case kKindMessage: return call_hook(binding, fn.decorate_message, static_cast<const mc_message*>(input), output);
    case kKindUser: return call_hook(binding, fn.decorate_user, static_cast<const mc_user*>(input), output);
    case kKindChannel: return call_hook(binding, fn.decorate_channel, static_cast<const mc_channel*>(input), output);
    case kKindGuild: return call_hook(binding, fn.decorate_guild, static_cast<const mc_guild*>(input), output);
    default: return call_hook(binding, fn.decorate_header, static_cast<const mc_header*>(input), output);
    }
}

// Whether the hook's filter lets the entity through. facts: message, MC_FILTER_* bits that hold; user, 1 << where.
bool accepts(u32 kind, const mc_hook& hook, u32 facts) noexcept {
    if (kind == kKindMessage) return (hook.filter & ~static_cast<u64>(facts)) == 0;
    if (kind == kKindUser) return !hook.filter || (hook.filter & facts);
    return true;
}

// Asks every plugin whose verdict for the entity is not current. True when none had to wait for budget.
bool evaluate(PluginDecoratePass* pass, u32 kind, u64 key, u64 scope, u32 revision, u32 facts, const void* input) noexcept {
    if (!pass->budget_us) pass->budget_us = kPluginDecoratePassBudgetUs;
    bool complete = true;
    for (u32 i = 0; i < g_plugin_count; ++i) {
        Plugin& plugin = g_plugins[i];
        if (!plugin.hooks[kind] || plugin.binding->struck_out || !accepts(kind, *plugin.hooks[kind], facts)) continue;
        if (ensure_memory(plugin)) {
            const Entry* entry = find(plugin.caches[kind], key, scope, false);
            if (entry && (entry->state == 2 || (entry->revision == revision && entry->generation == plugin.generation[kind])))
                continue;
        }
        if (pass->spent_us >= pass->budget_us) {
            pass->exhausted = true;
            complete = false;
            continue;
        }
        mc_decoration output{};
        // Presence and devices are activity: a plugin without MC_PERM_READ_ACTIVITY sees neither.
        mc_user hidden;
        const void* in = input;
        if (kind == kKindUser && !(plugin.binding->permissions & MC_PERM_READ_ACTIVITY)) {
            hidden = *static_cast<const mc_user*>(input);
            hidden.presence = MC_PRESENCE_UNKNOWN;
            hidden.platforms = 0;
            in = &hidden;
        }
        const i64 begin = clock_now();
        const PluginCallResult ran = call_kind(plugin, kind, in, &output);
        pass->spent_us += elapsed_us(begin);
        ++pass->calls;
        // A failed, overrunning or invalid answer is "decorates nothing": cached, so it isn't asked every pass.
        const bool good = !ran.overran && ran.status == MC_OK && decoration_ok(output);
        if (store(plugin, kind, key, scope, revision, 1, good ? &output : nullptr)) changed(kind <= kKindUser);
    }
    return complete;
}

bool prepare_message(PluginDecoratePass* pass, const PluginMessageInput& m) noexcept {
    mc_message in{};
    fill_message(m, &in);
    const u32 facts = (m.guild_id ? MC_FILTER_GUILD_ONLY : MC_FILTER_DM_ONLY) |
                      ((m.flags & MC_MESSAGE_MEDIA) ? MC_FILTER_HAS_MEDIA : 0u) |
                      ((m.flags & MC_MESSAGE_OWN) ? 0u : MC_FILTER_NOT_OWN);
    return evaluate(pass, kKindMessage, m.message_id, 0, m.revision, facts, &in);
}

// A user's revision is derived from what the hooks see, so a presence or name change asks again; a profile's
// opening time too, so every opening asks again.
u32 user_revision(const PluginUserInput& u) noexcept {
    u64 h = hash64_mix(kHash64Seed, u.guild_id);
    h = hash64_mix(h, u.channel_id);
    h = hash64_mix(h, (static_cast<u64>(u.flags) << 32) | u.relationship);
    h = hash64_mix(h, (static_cast<u64>(u.presence) << 32) | u.platforms);
    if (u.where == kPluginUserProfile) h = hash64_mix(h, u.message_id);
    for (u32 i = 0; i < u.name.bytes; ++i) h = hash64_mix(h, static_cast<u8>(u.name.data[i]));
    return static_cast<u32>(h >> 32);
}
bool prepare_user(PluginDecoratePass* pass, const PluginUserInput& u) noexcept {
    mc_user in{};
    in.user_id = u.user_id;
    in.guild_id = u.guild_id;
    in.channel_id = u.channel_id;
    in.where = u.where;
    in.flags = u.flags;
    in.relationship = u.relationship;
    in.presence = u.presence;
    in.platforms = u.platforms;
    in.name = {u.name.data, u.name.bytes, 0};
    in.username = {u.username.data, u.username.bytes, 0};
    // The author's name affixes ride on the message's decoration, found through this memo.
    if (u.where == kPluginUserMessageAuthor && u.message_id) author_slot(u.message_id) = {u.message_id, u.user_id};
    return evaluate(pass, kKindUser, u.user_id, u.where, user_revision(u), 1u << u.where, &in);
}

u32 entity_revision(u64 a, u64 b, u64 c, u64 d, Utf8View name) noexcept {
    u64 h = hash64_mix(hash64_mix(hash64_mix(hash64_mix(kHash64Seed, a), b), c), d);
    for (u32 i = 0; i < name.bytes; ++i) h = hash64_mix(h, static_cast<u8>(name.data[i]));
    return static_cast<u32>(h >> 32);
}
bool prepare_channel(PluginDecoratePass* pass, const PluginChannelInput& c) noexcept {
    mc_channel in{};
    in.channel_id = c.channel_id;
    in.guild_id = c.guild_id;
    in.parent_id = c.parent_id;
    in.self_permissions = c.self_permissions;
    in.type = c.type;
    in.flags = c.flags;
    in.mention_count = c.mention_count;
    in.name = {c.name.data, c.name.bytes, 0};
    return evaluate(pass, kKindChannel, c.channel_id, 0,
                    entity_revision(c.guild_id, c.self_permissions, (static_cast<u64>(c.type) << 32) | c.flags,
                                    c.mention_count, c.name),
                    0, &in);
}
bool prepare_guild(PluginDecoratePass* pass, const PluginGuildInput& g) noexcept {
    mc_guild in{};
    in.guild_id = g.guild_id;
    in.owner_id = g.owner_id;
    in.self_permissions = g.self_permissions;
    in.flags = g.flags;
    in.mention_count = g.mention_count;
    in.member_count = g.member_count;
    in.online_count = g.online_count;
    in.name = {g.name.data, g.name.bytes, 0};
    return evaluate(pass, kKindGuild, g.guild_id, 0,
                    entity_revision(g.owner_id, g.self_permissions, (static_cast<u64>(g.flags) << 32) | g.mention_count,
                                    (static_cast<u64>(g.member_count) << 32) | g.online_count, g.name),
                    0, &in);
}
bool prepare_header(PluginDecoratePass* pass, const PluginHeaderInput& h) noexcept {
    mc_header in{};
    in.channel_id = h.channel_id;
    in.guild_id = h.guild_id;
    in.where = h.where;
    in.member_count = h.member_count;
    in.online_count = h.online_count;
    in.name = {h.name.data, h.name.bytes, 0};
    return evaluate(pass, kKindHeader, h.where, 0,
                    entity_revision(h.channel_id, h.guild_id, (static_cast<u64>(h.member_count) << 32) | h.online_count,
                                    h.where, h.name),
                    0, &in);
}

void refresh_all() noexcept {
    for (u32 i = 0; i < g_plugin_count; ++i)
        for (u32 k = 0; k < kKinds; ++k) ++g_plugins[i].generation[k];
}

bool reveal(u64 message_id) noexcept {
    if (!message_id || revealed(message_id)) return false;
    if (g_reveal_count < kReveals) {
        g_reveals[g_reveal_count++] = message_id;
    } else {
        g_reveals[g_reveal_cursor] = message_id;
        g_reveal_cursor = (g_reveal_cursor + 1) % kReveals;
    }
    changed(true);
    return true;
}

// ---- Node actions -----------------------------------------------------------------------------------------------

// The decorations a UI surface's nodes come from: the cache kind, the key and scope the verdict is stored under,
// and which of its node lists the surface shows (1 badges, 2 accessories).
struct NodeSource {
    u32 kind;
    u64 key, scope;
    u32 lists;
};
// A click on a message surface may come from the author line (the author's user decoration), a badge or an
// accessory row or card of the message itself.
u32 node_sources(const PluginSurfaceTarget& t, NodeSource (&out)[2]) noexcept {
    u32 count = 0;
    const auto add = [&](bool valid, NodeSource source) noexcept {
        if (valid) out[count++] = source;
    };
    switch (t.surface) {
    case kUiPluginSlotMessageAccessory:
        add(t.user_id != 0, {kKindUser, t.user_id, MC_USER_MESSAGE_AUTHOR, 1});
        add(t.message_id != 0, {kKindMessage, t.message_id, 0, 3});
        break;
    case kUiPluginSlotMemberBadge: add(t.user_id != 0, {kKindUser, t.user_id, MC_USER_MEMBER_ROW, 1}); break;
    case kUiPluginSlotProfileSection: add(t.user_id != 0, {kKindUser, t.user_id, MC_USER_PROFILE, 2}); break;
    case kUiPluginSlotChannelRow: add(t.channel_id != 0, {kKindChannel, t.channel_id, 0, 1}); break;
    case kUiPluginSlotGuildRail: add(t.guild_id != 0, {kKindGuild, t.guild_id, 0, 1}); break;
    case kUiPluginSlotHeaderToolbar: add(true, {kKindHeader, MC_HEADER_CHANNEL, 0, 1}); break;
    case kUiPluginSlotMemberListHeader: add(true, {kKindHeader, MC_HEADER_MEMBER_LIST, 0, 1}); break;
    default: break;
    }
    return count;
}

struct ActionCall {
    mc_node_action_fn function;
    const mc_node_action* action;
    static mc_status run(mc_ctx* ctx, void* argument) noexcept {
        const auto* call = static_cast<ActionCall*>(argument);
        return call->function(ctx, call->action);
    }
};

bool activate(u64 owner_id, const PluginSurfaceTarget& target, u64 action_id, const PluginMessageInput* message) noexcept {
    if (!action_id) return false;
    if (action_id & kButtonAction) return buttons_activate(owner_id, target, action_id, message);
    Plugin* plugin = nullptr;
    for (u32 i = 0; i < g_plugin_count && !plugin; ++i)
        if (g_plugins[i].binding->owner_id == owner_id) plugin = &g_plugins[i];
    if (!plugin || !plugin->action || plugin->binding->struck_out) return false;
    NodeSource sources[2];
    const u32 source_count = node_sources(target, sources);
    for (u32 s = 0; s < source_count; ++s) {
        const NodeSource& source = sources[s];
        // The node must be in the plugin's current verdict: a click on something it no longer shows does nothing.
        const Entry* entry = current_entry(*plugin, source.kind, source.key, source.scope, 0, 0, true);
        const u8* record = entry ? record_of(*plugin, source.kind, *entry) : nullptr;
        if (!record) continue;
        Decoded decoded;
        decode(record, owner_id, &decoded);
        bool card = false, found = false;
        for (u32 list = 0; list < 2 && !found; ++list) {
            if (!(source.lists >> list & 1u)) continue;
            const u32 count = list ? decoded.accessory_count : decoded.badge_count;
            const UiPluginRenderNode* nodes = list ? decoded.accessories : decoded.badges;
            for (u32 i = 0; i < count && !found; ++i)
                found = nodes[i].kind == MC_NODE_BUTTON && nodes[i].action_id == action_id;
        }
        if (!found && source.kind == kKindMessage && decoded.has_card)
            for (u32 i = 0; i < decoded.card.button_count && !found; ++i)
                found = card = decoded.card.buttons[i].action_id == action_id;
        if (!found) continue;
        mc_node_action action{};
        action.tag = action_id;
        action.channel_id = target.channel_id;
        action.message_id = target.message_id;
        action.user_id = target.user_id;
        action.guild_id = target.guild_id;
        action.source = entry->state == 2 ? 0u : MC_HOOK_DECORATE_MESSAGE + source.kind;
        action.kind = card ? MC_NODE_CARD_BUTTON : MC_NODE_BUTTON;
        ActionCall call{plugin->action->fn.node_action, &action};
        plugin_binding_call(plugin->binding, PluginCallKind::slot_action, PluginCallClass::gesture, MC_CTX_GESTURE,
                            ActionCall::run, &call);
        return true;
    }
    return false;
}

const PluginSurfaceHost g_host{
    .wants = 0,
    .message_needs = 0,
    .message = prepare_message,
    .user = prepare_user,
    .channel = prepare_channel,
    .guild = prepare_guild,
    .header = prepare_header,
    .popover = buttons_popover,
    .activate = activate,
    .overflow = buttons_overflow,
    .refresh = refresh_all,
    .refresh_buttons = buttons_refresh,
    .reveal = reveal,
};
PluginSurfaceHost g_published;

} // namespace

namespace plugin_surfaces_detail {

u64 elapsed_us(i64 begin) noexcept {
    static Clock clock;
    if (!clock.frequency) clock = clock_initialize();
    return clock_microseconds(clock, begin, clock_now());
}

bool text_valid(const mc_str& text, u32 maximum) noexcept { return text_ok(text, maximum); }

void fill_message(const PluginMessageInput& m, mc_message* in) noexcept {
    in->message_id = m.message_id;
    in->channel_id = m.channel_id;
    in->guild_id = m.guild_id;
    in->author_id = m.author_id;
    in->reply_to = m.reply_to;
    in->timestamp_ms = m.timestamp_ms;
    in->edited_ms = m.edited_ms;
    in->content = {m.content.data, m.content.bytes, 0};
    in->author_name = {m.author_name.data, m.author_name.bytes, 0};
    in->flags = m.flags;
    in->type = m.type;
    in->revision = m.revision;
    in->relationship = m.relationship;
    in->attachment_count = m.attachment_count;
    in->embed_count = m.embed_count;
}

Plugin* find_plugin(const PluginBinding* binding) noexcept {
    for (u32 i = 0; i < g_plugin_count; ++i)
        if (g_plugins[i].binding == binding) return &g_plugins[i];
    return nullptr;
}

Plugin* ensure_plugin(PluginBinding* binding) noexcept {
    if (Plugin* plugin = find_plugin(binding)) return plugin;
    if (g_plugin_count == kMaximumPlugins) return nullptr;
    Plugin* plugin = &g_plugins[g_plugin_count++];
    reset_value(plugin);
    plugin->binding = binding;
    return plugin;
}

void changed(bool message) noexcept {
    ++g_plugin_surface_revision;
    if (message) {
        ++g_plugin_message_revision;
        g_ui_plugin_render_revision =
            hash64_mix(hash64_mix(kHash64Seed, g_plugin_message_revision), g_plugin_surface_revision);
    }
}

void republish() noexcept {
    u32 wants = 0, needs = 0, pushed = 0;
    for (u32 i = 0; i < g_plugin_count; ++i) {
        const Plugin& plugin = g_plugins[i];
        for (u32 k = 0; k < kKinds; ++k)
            if (plugin.hooks[k]) wants |= 1u << k;
        if (plugin.hooks[kKindMessage]) needs |= plugin.hooks[kKindMessage]->flags & (MC_NEED_CONTENT | MC_NEED_AUTHOR);
        pushed |= plugin.pushed;
    }
    g_published = g_host;
    g_published.wants = wants;
    g_published.message_needs = needs;
    const u32 decorated = wants | pushed;
    u32 mask = button_surfaces();
    if (decorated & (1u << kKindUser))
        mask |= (1u << kUiPluginSlotMemberBadge) | (1u << kUiPluginSlotProfileSection) |
                (1u << kUiPluginSlotMessageAccessory) | (1u << kUiPluginSlotMemberName);
    if (decorated & (1u << kKindChannel)) mask |= 1u << kUiPluginSlotChannelRow;
    if (decorated & (1u << kKindGuild)) mask |= 1u << kUiPluginSlotGuildRail;
    if (decorated & (1u << kKindHeader))
        mask |= (1u << kUiPluginSlotHeaderToolbar) | (1u << kUiPluginSlotMemberListHeader);
    g_ui_plugin_slot_surfaces = mask;
    if (g_plugin_count) {
        g_plugin_surfaces = &g_published;
        g_ui_plugin_render_query = render_query;
        g_ui_plugin_slot_query = slot_query;
    } else {
        if (g_plugin_surfaces == &g_published) g_plugin_surfaces = nullptr;
        if (g_ui_plugin_render_query == render_query) g_ui_plugin_render_query = nullptr;
        if (g_ui_plugin_slot_query == slot_query) g_ui_plugin_slot_query = nullptr;
        g_ui_plugin_slot_surfaces = ~0u;
        clear_bytes(g_authors, sizeof(g_authors));
    }
    changed(true);
}

} // namespace plugin_surfaces_detail

bool plugin_surfaces_add(PluginBinding* binding) noexcept {
    const mc_descriptor& descriptor = *binding->image.descriptor;
    bool decorates = false;
    for (u32 i = 0; i < descriptor.hook_count; ++i) {
        const u32 kind = descriptor.hooks[i].kind;
        decorates = decorates || (kind >= MC_HOOK_DECORATE_MESSAGE && kind <= MC_HOOK_NODE_ACTION);
    }
    if (!decorates && !descriptor.button_count) return true;
    Plugin* plugin = ensure_plugin(binding);
    if (!plugin) return false;
    for (u32 i = 0; i < descriptor.hook_count; ++i) {
        const mc_hook& hook = descriptor.hooks[i];
        if (hook.kind == MC_HOOK_NODE_ACTION) plugin->action = &hook;
        else if (hook.kind >= MC_HOOK_DECORATE_MESSAGE && hook.kind <= MC_HOOK_DECORATE_HEADER)
            plugin->hooks[hook.kind - MC_HOOK_DECORATE_MESSAGE] = &hook;
    }
    buttons_add(*plugin);
    republish();
    return true;
}

void plugin_surfaces_remove(PluginBinding* binding) noexcept {
    Plugin* plugin = find_plugin(binding);
    if (!plugin) return;
    if (plugin->memory) VirtualFree(plugin->memory, 0, MEM_RELEASE);
    const u32 index = static_cast<u32>(plugin - g_plugins);
    for (u32 i = index + 1; i < g_plugin_count; ++i) g_plugins[i - 1] = g_plugins[i];
    reset_value(&g_plugins[--g_plugin_count]);
    republish();
}

mc_status plugin_surfaces_detail::plugin_surfaces_push(PluginBinding* binding, u32 hook_kind, u64 id, u64 scope,
                                                       const mc_decoration* d) noexcept {
    (void)scope; // the owning channel or guild: the verdict is found by id alone
    if (hook_kind < MC_HOOK_DECORATE_MESSAGE || hook_kind > MC_HOOK_DECORATE_HEADER) return MC_E_INVALID;
    const u32 kind = hook_kind - MC_HOOK_DECORATE_MESSAGE;
    if (kind == kKindHeader ? id > MC_HEADER_MEMBER_LIST : !id) return MC_E_INVALID;
    if (d && !decoration_ok(*d)) return MC_E_INVALID;
    Plugin* plugin = ensure_plugin(binding);
    if (!plugin) return MC_E_BUDGET;
    // A header push with id 0 covers both headers.
    const u64 first = kind == kKindHeader && !id ? MC_HEADER_CHANNEL : id;
    const u64 last = kind == kKindHeader && !id ? MC_HEADER_MEMBER_LIST : id;
    for (u64 key = first; key <= last; ++key) {
        if (!d || decoration_empty(*d)) {
            Entry* entry = plugin->memory ? find(plugin->caches[kind], key, 0, false) : nullptr;
            if (entry && entry->state == 2) {
                const bool visible = entry->bytes != 0;
                reset_value(entry);
                if (visible) changed(kind <= kKindUser);
            }
        } else {
            if (!store(*plugin, kind, key, 0, 0, 2, d)) return MC_E_BUDGET;
            changed(kind <= kKindUser);
        }
    }
    if (d && !decoration_empty(*d) && !(plugin->pushed & (1u << kind))) {
        plugin->pushed |= 1u << kind;
        republish(); // a surface the UI skipped may now hold nodes
    }
    return MC_OK;
}

mc_status plugin_surfaces_detail::plugin_surfaces_invalidate(PluginBinding* binding, u32 what, u64 id) noexcept {
    if (what < MC_INVALIDATE_MESSAGE || what > MC_INVALIDATE_BUTTONS) return MC_E_INVALID;
    if (what == MC_INVALIDATE_BUTTONS) {
        buttons_refresh_remembered();
        changed(false);
        return MC_OK;
    }
    Plugin* plugin = find_plugin(binding);
    if (!plugin) return MC_OK; // nothing cached
    const u32 kind = what - MC_INVALIDATE_MESSAGE;
    if (!id) {
        ++plugin->generation[kind];
    } else if (plugin->memory) {
        // The entry stays on screen until the next pass answers again.
        Cache& cache = plugin->caches[kind];
        const u32 wheres = kind == kKindUser ? MC_USER_PROFILE : 0;
        for (u32 where = kind == kKindUser ? MC_USER_MEMBER_ROW : 0; where <= wheres; ++where)
            if (Entry* entry = find(cache, id, where, false); entry && entry->state == 1) entry->generation = kStale;
    }
    changed(kind <= kKindUser);
    return MC_OK;
}

} // namespace maicord
