#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/audio_presentation.hpp"
#include "maicord/ui/poll_presentation.hpp"

#include "maicord/ui/timestamp_format.hpp"
#include "maicord/ui/message_reaction_presentation.hpp"
#include "maicord/ui/message_component_layout.hpp"
#include "maicord/ui/message_system_text.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/model/invite_card_cache.hpp"
#include "maicord/model/message_poll_store.hpp"
#include "maicord/model/scheduled_event_store.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {

// What the Markdown callbacks consulted during one parse: the dependencies a
// cached result records, and a bound on the output it can need beyond its
// source (resolved labels, message-link suffixes, timestamps).
struct MarkdownParseTrace {
    u32 extra_bytes;
    u32 largest_emission;
    bool entities;
    bool spoilers;
    bool timestamps;
};

struct BuildContext {
    MessageTextPresentation* presentation;
    const ChatStateStore* state;
    const UserStore* users;
    const NavigationView* navigation;
    const MessageRecord* message;
    u64 now_unix_seconds;
    const MessageRichStore* rich_store{};
    const MessageRichRecord* rich{};
    u16 source_id{};
    bool forwarded{};
    // Embed markdown rule sets: 0 message/description, 1 title/field name
    // (no links, headings, subtext or lists), 2 field value (no headings).
    u8 markdown_rules{};
    MarkdownParseTrace trace{};
};

constexpr u16 kRichTextSource = 0x8000u;
constexpr u16 kForwardedTextSource = 0x4000u;
constexpr u16 kRichTextBody = 1u;
constexpr u16 kRichTextEmbedBegin = 2u;
// Per embed: author, title, description, footer, then the link targets
// (embed.url, author.url, provider.name, provider.url) whose bytes link
// semantics address.
constexpr u16 kRichTextEmbedSlots = 8u;
constexpr u16 kRichTextFieldBegin = 96u;
constexpr u16 kRichTextComponentBegin = 224u;
static_assert(kRichTextEmbedBegin + kGatewayMessageEmbedMaximum * kRichTextEmbedSlots <= kRichTextFieldBegin);
static_assert(kRichTextFieldBegin + kGatewayMessageEmbedFieldMaximum * 2u <= kRichTextComponentBegin);

u32 scoped_source_offset(u16 source_id, u32 offset) noexcept {
    return (static_cast<u32>(source_id) << 16u) | offset;
}

MessageRichString rich_source_string(const MessageRichRecord& rich, u16 source_id) noexcept {
    const u16 source = source_id & ~(kRichTextSource | kForwardedTextSource);
    if (source == kRichTextBody && rich.forwarded_snapshot_child)
        return rich.snapshot.content;
    if (source >= kRichTextEmbedBegin && source < kRichTextFieldBegin) {
        const u32 index = (source - kRichTextEmbedBegin) / kRichTextEmbedSlots;
        if (index >= rich.embed_count)
            return {};
        const auto& embed = rich.embeds[index];
        switch ((source - kRichTextEmbedBegin) % kRichTextEmbedSlots) {
        case 0:
            return embed.author_name;
        case 1:
            return embed.title;
        case 2:
            return embed.description;
        case 3:
            return embed.footer_text;
        case 4:
            return embed.url;
        case 5:
            return embed.author_url;
        case 6:
            return embed.provider_name;
        default:
            return embed.provider_url;
        }
    }
    if (source >= kRichTextFieldBegin && source < kRichTextComponentBegin) {
        const u32 index = (source - kRichTextFieldBegin) / 2u;
        if (index >= rich.embed_field_count)
            return {};
        return (source - kRichTextFieldBegin) % 2u ? rich.embed_fields[index].value
                                                   : rich.embed_fields[index].name;
    }
    if (source >= kRichTextComponentBegin) {
        const u32 index = (source - kRichTextComponentBegin) / 2u;
        if (index >= rich.component_count)
            return {};
        return (source - kRichTextComponentBegin) % 2u ? rich.components[index].content
                                                       : rich.components[index].label;
    }
    return {};
}

u16 identify_rich_source(const BuildContext& context, Utf8View source) noexcept {
    if (!context.rich || !context.rich_store || source.bytes == 0)
        return 0;
    const u16 scope = kRichTextSource | (context.forwarded ? kForwardedTextSource : 0u);
    const u16 starts[] = {kRichTextEmbedBegin, kRichTextFieldBegin, kRichTextComponentBegin};
    const u16 counts[] = {static_cast<u16>(context.rich->embed_count * kRichTextEmbedSlots),
                          static_cast<u16>(context.rich->embed_field_count * 2u),
                          static_cast<u16>(context.rich->component_count * 2u)};
    for (u32 group = 0; group < 3; ++group) {
        for (u16 slot = 0; slot < counts[group]; ++slot) {
            const u16 id = starts[group] + slot;
            const Utf8View candidate =
                message_rich_store_string(*context.rich_store, rich_source_string(*context.rich, id));
            if (candidate.data == source.data && candidate.bytes == source.bytes)
                return scope | id;
        }
    }
    return 0;
}

Color role_color(u32 packed) noexcept {
    constexpr float scale = 1.0f / 255.0f;
    return Color{static_cast<float>((packed >> 16u) & 0xFFu) * scale,
                 static_cast<float>((packed >> 8u) & 0xFFu) * scale,
                 static_cast<float>(packed & 0xFFu) * scale, 1.0f};
}

u64 command_content_signature(const MessageTextCommand& command) noexcept {
    u64 hash = hash64_counted_bytes(kHash64Seed, command.text.data, command.text.bytes);
    for (u32 index = 0; index < command.styles.count; ++index) {
        const TextStyleSpan& style = command.styles[index];
        hash = hash64_mix(hash, style.byte_offset);
        hash = hash64_mix(hash, style.byte_count);
        hash = hash64_mix(hash, style.flags);
        hash = hash64_mix(hash, style.size);
    }
    hash = hash64_mix(hash, command.styles.count);
    hash = hash64_mix(hash, static_cast<u8>(command.wrapping));
    return hash64_mix(hash, static_cast<u8>(command.overflow));
}

union FloatWord {
    float value;
    u32 word;
};

u64 command_measurement_identity(const MessageTextCommand& command, u32 owner_id) noexcept {
    u64 hash = command_content_signature(command);
    const FloatWord width{ui_rect_width(command.rectangle)};
    hash = hash64_mix(hash, command.message_id);
    hash = hash64_mix(hash, command.message_revision);
    hash = hash64_mix(hash, command.source_id);
    hash = hash64_mix(hash, owner_id);
    hash = hash64_mix(hash, static_cast<u8>(command.measurement_kind));
    hash = hash64_mix(hash, command.measurement_slot);
    return hash64_mix(hash, width.word);
}

// The previous build's measured extent of a command with this identity.
// Builds emit commands in nearly the same order, so the search resumes after
// the last match: a stable rebuild costs one comparison per command instead
// of a scan of the whole history.
void previous_measurement(MessageTextPresentation* presentation, u64 identity, float* height,
                          float* width) noexcept {
    *height = 0.0f;
    *width = 0.0f;
    const u32 count = presentation->measurement_history_count;
    u32 index = presentation->measurement_history_cursor < count ? presentation->measurement_history_cursor : 0;
    for (u32 step = 0; step < count; ++step, index = index + 1u == count ? 0u : index + 1u) {
        const u32 offset = presentation->measurement_history_offset + index * 4u;
        const u64 candidate = static_cast<u64>(presentation->byte_style_scratch[offset]) |
                              (static_cast<u64>(presentation->byte_style_scratch[offset + 1u]) << 32u);
        if (candidate != identity)
            continue;
        FloatWord value{};
        value.word = presentation->byte_style_scratch[offset + 2u];
        *height = value.value;
        value.word = presentation->byte_style_scratch[offset + 3u];
        *width = value.value;
        presentation->measurement_history_cursor = index + 1u;
        return;
    }
}

MessageTextPresentationResult result(const MessageTextPresentation& presentation,
                                     MessageTextPresentationError error) noexcept {
    return MessageTextPresentationResult{presentation.command_count, presentation.measurement_count,
                                         presentation.utf8_count, error};
}

void reject_atomically(MessageTextPresentation* presentation, MessageTextPresentationError error) noexcept {
    if (!presentation) {
        return;
    }
    presentation->command_count = 0;
    presentation->utf8_count = 0;
    presentation->style_count = 0;
    presentation->semantic_count = 0;
    presentation->measurement_count = 0;
    presentation->measurement_history_count = 0;
    presentation->measurement_history_offset = presentation->byte_style_scratch.count;
    presentation->projection_signature = 0;
    presentation->active = false;
    presentation->last_error = error;
    saturating_accumulate(presentation->rejected);
}

bool fail(MessageTextPresentation* presentation, MessageTextPresentationError error) noexcept {
    if (presentation && presentation->last_error == MessageTextPresentationError::none) {
        presentation->last_error = error;
    }
    return false;
}

void set_failure_context(MessageTextPresentation* presentation, MessageTextPresentationStage stage,
                         MessageTextPresentationOperation operation,
                         u16 item_index = kInvalidMessageTextMeasurement,
                         UiTextFlowError flow_error = UiTextFlowError::none) noexcept {
    if (!presentation) {
        return;
    }
    presentation->failure_stage = stage;
    presentation->failure_operation = operation;
    presentation->failure_item_index = item_index;
    presentation->failure_flow_error = flow_error;
}

void set_failure_operation(MessageTextPresentation* presentation,
                           MessageTextPresentationOperation operation,
                           UiTextFlowError flow_error = UiTextFlowError::none) noexcept {
    if (!presentation) {
        return;
    }
    presentation->failure_operation = operation;
    presentation->failure_flow_error = flow_error;
}

// Every emission that can outgrow its source passes a callback; one resolved
// entity adds at most its label plus a sigil and a message link's suffix.
constexpr u32 kMarkdownLabelMargin = 16;

void note_emission(MarkdownParseTrace* trace, u32 bytes) noexcept {
    trace->extra_bytes = trace->extra_bytes + bytes < trace->extra_bytes ? ~0u : trace->extra_bytes + bytes;
    trace->largest_emission = maximum(trace->largest_emission, bytes);
}

// The user's server nickname in the channel's guild, or empty.
Utf8View member_nickname(const MessageTextPresentation& presentation, const ChatStateStore& state,
                         u64 channel_id, u64 user_id) noexcept {
    const ChannelRecord* channel = chat_state_store_find_channel(state, channel_id);
    const GuildMemberRecord* member =
        presentation.guild_members && channel && channel->guild_id != 0 && user_id != 0
            ? guild_member_store_find(*presentation.guild_members, channel->guild_id, user_id)
            : nullptr;
    return member ? guild_member_store_nickname(*member) : Utf8View{};
}

Utf8View resolve_entity_name(const BuildContext* context, MessageMarkdownEntityKind kind, u64 id) noexcept {
    if (!context || !context->state || !context->navigation || id == 0) {
        return Utf8View{};
    }
    if (kind == MessageMarkdownEntityKind::user) {
        if (!context->users) {
            return Utf8View{};
        }
        if (context->presentation && context->message) {
            const Utf8View nickname =
                member_nickname(*context->presentation, *context->state, context->message->channel_id, id);
            if (nickname.bytes)
                return nickname;
        }
        const UserRecord* user = user_store_find(*context->users, id);
        return user ? user_store_name(*context->users, *user) : Utf8View{};
    }
    if (kind == MessageMarkdownEntityKind::channel) {
        const ChannelRecord* channel = chat_state_store_find_channel(*context->state, id);
        return channel ? chat_state_store_channel_name(*context->state, *channel) : Utf8View{};
    }
    const ChannelRecord* selected =
        chat_state_store_find_channel(*context->state, context->navigation->selected_channel_id);
    const RoleRecord* role =
        selected ? chat_state_store_find_role(*context->state, selected->guild_id, id) : nullptr;
    return role ? chat_state_store_role_name(*context->state, *role) : Utf8View{};
}

Utf8View resolve_entity(void* opaque, MessageMarkdownEntityKind kind, u64 id) noexcept {
    auto* context = static_cast<BuildContext*>(opaque);
    const Utf8View name = resolve_entity_name(context, kind, id);
    if (context) {
        context->trace.entities = true;
        note_emission(&context->trace, name.bytes + kMarkdownLabelMargin);
    }
    return name;
}

TimestampFormatResult format_timestamp(void* opaque, u64 unix_seconds, char style,
                                              Slice<char> output) noexcept {
    auto* context = static_cast<BuildContext*>(opaque);
    if (!context)
        return TimestampFormatResult{};
    const TimestampFormatResult formatted =
        format_timestamp_local(unix_seconds, style, context->now_unix_seconds, output);
    context->trace.timestamps = true;
    note_emission(&context->trace, formatted.bytes);
    return formatted;
}

bool spoiler_revealed(void* opaque, u32 source_offset) noexcept {
    auto* context = static_cast<BuildContext*>(opaque);
    if (!context || !context->presentation || !context->message) {
        return false;
    }
    context->trace.spoilers = true;
    const MessageTextPresentation& presentation = *context->presentation;
    if (presentation.reveal_spoilers)
        return true;
    for (u32 index = 0; index < presentation.spoiler_reveal_count; ++index) {
        const MessageTextSpoilerReveal& reveal = presentation.spoiler_reveals[index];
        if (reveal.message_id == context->message->id &&
            reveal.message_revision == context->message->revision &&
            reveal.source_offset == scoped_source_offset(context->source_id, source_offset)) {
            return true;
        }
    }
    return false;
}

// Markdown cache.
// Parse dependencies the callbacks observed, so an entry can record which
// state it was built against and how much output capacity it needed.
enum MarkdownCacheFlag : u8 {
    markdown_cache_entities = 1u << 0,
    markdown_cache_spoilers = 1u << 1,
    markdown_cache_timestamps = 1u << 2,
    markdown_cache_semantics_truncated = 1u << 3,
    markdown_cache_syntax_truncated = 1u << 4,
};
enum class MarkdownCacheMode : u8 { content = 1, reply = 2 };
constexpr u32 kMarkdownCacheProbes = 8;

u64 source_fingerprint(Utf8View source) noexcept {
    u64 hash = 0x9E3779B97F4A7C15ull ^ (static_cast<u64>(source.bytes) * 0xBF58476D1CE4E5B9ull);
    u32 index = 0;
    for (; index + 8u <= source.bytes; index += 8u) {
        u64 word;
        memcpy(&word, source.data + index, sizeof(word));
        hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 29u;
    }
    u64 tail = 0;
    for (u32 shift = 0; index < source.bytes; ++index, shift += 8u)
        tail |= static_cast<u64>(static_cast<u8>(source.data[index])) << shift;
    hash = (hash ^ tail) * 0xC4CEB9FE1A85EC53ull;
    return hash ^ (hash >> 32u);
}

u64 markdown_cache_key(Utf8View source, const MessageMarkdownOptions& options, MarkdownCacheMode mode) noexcept {
    const u64 switches = static_cast<u64>(mode) | static_cast<u64>(options.base_size) << 8u |
                         u64{options.no_links} << 16u | u64{options.no_headings} << 17u |
                         u64{options.no_lists} << 18u | u64{options.text_sizes} << 19u |
                         u64{options.profile_bio} << 20u | u64{options.preserve_entities} << 21u |
                         u64{options.spoiler_revealed != nullptr} << 22u | u64{options.has_hover} << 23u |
                         static_cast<u64>(timestamp_hour_cycle()) << 24u |
                         static_cast<u64>(options.base_style) << 32u;
    u64 key = hash64_mix(source_fingerprint(source), switches);
    if (options.has_hover)
        key = hash64_mix(hash64_mix(key, options.hovered_source_offset), static_cast<u8>(options.hovered_kind));
    return key ? key : 1u;
}

// Names the resolver can return: users (by server nickname), channels and the
// selected guild's roles.
u64 markdown_entity_epoch(const ChatStateStore& state, const UserStore* users,
                          const GuildMemberStore* members, const NavigationView& navigation) noexcept {
    const ChannelRecord* selected = chat_state_store_find_channel(state, navigation.selected_channel_id);
    u64 epoch = hash64_mix(kHash64Seed, reinterpret_cast<usize>(users));
    epoch = hash64_mix(epoch, users ? users->revision : 0u);
    epoch = hash64_mix(epoch, members ? members->revision : 0u);
    epoch = hash64_mix(epoch, reinterpret_cast<usize>(&state));
    epoch = hash64_mix(epoch, state.guild_mutations);
    epoch = hash64_mix(epoch, state.channel_mutations);
    epoch = hash64_mix(epoch, state.permission_mutations);
    epoch = hash64_mix(epoch, state.compactions);
    // Role inserts and renames bump no counter but always allocate a name.
    epoch = hash64_mix(epoch, state.role_count);
    epoch = hash64_mix(epoch, state.name_used);
    return hash64_mix(epoch, selected ? selected->guild_id : 0u);
}

u64 markdown_spoiler_signature(const BuildContext& context) noexcept {
    const MessageTextPresentation& presentation = *context.presentation;
    if (presentation.reveal_spoilers)
        return 1;
    u64 signature = 2;
    for (u32 index = 0; context.message && index < presentation.spoiler_reveal_count; ++index) {
        const MessageTextSpoilerReveal& reveal = presentation.spoiler_reveals[index];
        if (reveal.message_id == context.message->id && reveal.message_revision == context.message->revision &&
            (reveal.source_offset >> 16u) == context.source_id)
            signature += hash64_mix(kHash64Seed, reveal.source_offset); // reveal order is irrelevant
    }
    return signature;
}

bool markdown_payload_live(const MessageMarkdownCache& cache, const MessageMarkdownCacheEntry& entry) noexcept {
    return entry.position != 0 && cache.head <= entry.position - 1u + cache.arena_bytes;
}

MessageMarkdownCacheEntry* markdown_cache_find(MessageMarkdownCache* cache, u64 key) noexcept {
    const u32 mask = cache->slot_count - 1u;
    for (u32 probe = 0; probe < kMarkdownCacheProbes; ++probe) {
        MessageMarkdownCacheEntry& entry = cache->entries[(static_cast<u32>(key) + probe) & mask];
        if (entry.key == key && markdown_payload_live(*cache, entry))
            return &entry;
    }
    return nullptr;
}

MessageMarkdownCacheEntry* markdown_cache_slot(MessageMarkdownCache* cache, u64 key) noexcept {
    const u32 mask = cache->slot_count - 1u;
    MessageMarkdownCacheEntry* oldest = nullptr;
    for (u32 probe = 0; probe < kMarkdownCacheProbes; ++probe) {
        MessageMarkdownCacheEntry& entry = cache->entries[(static_cast<u32>(key) + probe) & mask];
        if (entry.key == key || !markdown_payload_live(*cache, entry))
            return &entry;
        if (!oldest || entry.position < oldest->position)
            oldest = &entry;
    }
    return oldest;
}

constexpr u32 markdown_payload_bytes(u32 semantics, u32 spans, u32 text) noexcept {
    return semantics * static_cast<u32>(sizeof(MessageMarkdownSemantic)) +
           spans * static_cast<u32>(sizeof(TextStyleSpan)) + text;
}

// Writes semantics, spans and text at the ring head; older payloads in the
// way are overwritten and their entries go stale.
bool markdown_cache_write(MessageMarkdownCache* cache, MessageMarkdownCacheEntry* entry,
                          ConstSlice<MessageMarkdownSemantic> semantics, ConstSlice<TextStyleSpan> spans,
                          Utf8View text) noexcept {
    const u32 bytes = (markdown_payload_bytes(semantics.count, spans.count, text.bytes) + 7u) & ~7u;
    if (bytes == 0 || bytes > cache->arena_bytes / 4u) {
        entry->position = 0;
        return false;
    }
    const u64 offset = cache->head % cache->arena_bytes;
    if (offset + bytes > cache->arena_bytes)
        cache->head += cache->arena_bytes - offset;
    u8* payload = cache->arena + cache->head % cache->arena_bytes;
    entry->position = cache->head + 1u;
    cache->head += bytes;
    const u32 semantic_bytes = semantics.count * static_cast<u32>(sizeof(MessageMarkdownSemantic));
    const u32 span_bytes = spans.count * static_cast<u32>(sizeof(TextStyleSpan));
    copy_bytes(payload, semantics.data, semantic_bytes);
    copy_bytes(payload + semantic_bytes, spans.data, span_bytes);
    copy_bytes(payload + semantic_bytes + span_bytes, text.data, text.bytes);
    return true;
}

bool markdown_cache_entry_current(const MessageMarkdownCacheEntry& entry, const BuildContext& context) noexcept {
    if ((entry.flags & markdown_cache_entities) &&
        entry.entity_epoch != context.presentation->markdown_entity_epoch)
        return false;
    if ((entry.flags & markdown_cache_timestamps) && entry.expires_unix_seconds != 0 &&
        context.now_unix_seconds >= entry.expires_unix_seconds)
        return false;
    return !(entry.flags & markdown_cache_spoilers) || entry.spoiler_signature == markdown_spoiler_signature(context);
}

// message_markdown_parse through the presentation's cache. A hit copies the
// retained output into the same caller buffers a parse would have written,
// and only when the parse itself would have produced that output there: the
// recorded capacity bound guarantees no label degraded and nothing failed.
MessageMarkdownResult cached_markdown_parse(MessageTextPresentation* presentation, BuildContext* context,
                                            MarkdownCacheMode mode, Utf8View source, Slice<char> text_output,
                                            Slice<u32> scratch, Slice<TextStyleSpan> span_output,
                                            const MessageMarkdownOptions& options,
                                            Slice<MessageMarkdownSemantic> semantic_output) noexcept {
    MessageMarkdownCache* cache = presentation->markdown_cache;
    const u32 text_capacity = minimum(text_output.count, scratch.count);
    const bool full_semantics =
        mode == MarkdownCacheMode::reply || semantic_output.count >= kMessageMarkdownSemanticMaximum;
    u64 key = 0;
    if (cache) {
        key = markdown_cache_key(source, options, mode);
        MessageMarkdownCacheEntry* entry = markdown_cache_find(cache, key);
        if (entry && markdown_cache_entry_current(*entry, *context) && entry->required_capacity <= text_capacity &&
            entry->text_bytes <= text_output.count && entry->span_count <= span_output.count &&
            entry->semantic_count <= semantic_output.count &&
            (full_semantics || !(entry->flags & markdown_cache_semantics_truncated))) {
            const u8* payload = cache->arena + (entry->position - 1u) % cache->arena_bytes;
            const u32 semantic_bytes = entry->semantic_count * static_cast<u32>(sizeof(MessageMarkdownSemantic));
            const u32 span_bytes = entry->span_count * static_cast<u32>(sizeof(TextStyleSpan));
            copy_bytes(semantic_output.data, payload, semantic_bytes);
            copy_bytes(span_output.data, payload + semantic_bytes, span_bytes);
            copy_bytes(text_output.data, payload + semantic_bytes + span_bytes, entry->text_bytes);
            MessageMarkdownResult result{};
            result.text = Utf8View{text_output.data, entry->text_bytes};
            result.span_count = entry->span_count;
            result.transformations = entry->transformations;
            result.syntax_tokens = entry->syntax_tokens;
            result.syntax_styled_bytes = entry->syntax_styled_bytes;
            result.resolved_entities = entry->resolved_entities;
            result.unresolved_entities = entry->unresolved_entities;
            result.timestamps = entry->timestamps;
            result.semantic_count = entry->semantic_count;
            result.next_timestamp_update_unix_seconds = entry->expires_unix_seconds;
            result.list_items = entry->list_items;
            result.syntax_truncated = (entry->flags & markdown_cache_syntax_truncated) != 0;
            result.semantics_truncated = (entry->flags & markdown_cache_semantics_truncated) != 0;
            // Copy a payload the ring is about to reach forward, from the
            // caller buffers it was just written to.
            if (cache->head - (entry->position - 1u) > cache->arena_bytes / 2u &&
                markdown_cache_write(cache, entry,
                                     ConstSlice<MessageMarkdownSemantic>{semantic_output.data, entry->semantic_count},
                                     ConstSlice<TextStyleSpan>{span_output.data, entry->span_count}, result.text))
                saturating_increment(cache->refreshes);
            saturating_increment(cache->hits);
            saturating_increment(presentation->markdown_cache_hits);
            return result;
        }
        saturating_increment(cache->misses);
    }
    context->trace = MarkdownParseTrace{};
    saturating_accumulate(presentation->markdown_parses);
    const MessageMarkdownResult parsed =
        message_markdown_parse(source, text_output, scratch, span_output, options, semantic_output);
    if (!cache || !parsed.ok())
        return parsed;
    const MarkdownParseTrace& trace = context->trace;
    const u64 required = u64{source.bytes} + trace.extra_bytes + trace.largest_emission + 1u;
    if (required > text_capacity || (parsed.semantics_truncated && !full_semantics) || parsed.span_count > 0xFFFFu ||
        parsed.semantic_count > 0xFFu) {
        saturating_increment(cache->uncacheable);
        return parsed;
    }
    MessageMarkdownCacheEntry* entry = markdown_cache_slot(cache, key);
    MessageMarkdownCacheEntry value{};
    value.key = key;
    value.expires_unix_seconds = parsed.next_timestamp_update_unix_seconds;
    value.entity_epoch = presentation->markdown_entity_epoch;
    value.spoiler_signature = trace.spoilers ? markdown_spoiler_signature(*context) : 0u;
    value.required_capacity = static_cast<u32>(required);
    value.text_bytes = parsed.text.bytes;
    value.transformations = parsed.transformations;
    value.syntax_tokens = parsed.syntax_tokens;
    value.syntax_styled_bytes = parsed.syntax_styled_bytes;
    value.span_count = static_cast<u16>(parsed.span_count);
    value.list_items = parsed.list_items;
    value.resolved_entities = static_cast<u16>(minimum(parsed.resolved_entities, 0xFFFFu));
    value.unresolved_entities = static_cast<u16>(minimum(parsed.unresolved_entities, 0xFFFFu));
    value.timestamps = static_cast<u16>(minimum(parsed.timestamps, 0xFFFFu));
    value.semantic_count = static_cast<u8>(parsed.semantic_count);
    value.flags = static_cast<u8>((trace.entities ? markdown_cache_entities : 0u) |
                                  (trace.spoilers ? markdown_cache_spoilers : 0u) |
                                  (trace.timestamps ? markdown_cache_timestamps : 0u) |
                                  (parsed.semantics_truncated ? markdown_cache_semantics_truncated : 0u) |
                                  (parsed.syntax_truncated ? markdown_cache_syntax_truncated : 0u));
    *entry = value;
    if (markdown_cache_write(cache, entry,
                             ConstSlice<MessageMarkdownSemantic>{semantic_output.data, parsed.semantic_count},
                             ConstSlice<TextStyleSpan>{span_output.data, parsed.span_count}, parsed.text))
        saturating_increment(cache->inserts);
    else
        saturating_increment(cache->uncacheable);
    return parsed;
}

bool append_command(MessageTextPresentation* presentation, Utf8View text, ConstSlice<TextStyleSpan> styles,
                    ConstSlice<MessageMarkdownSemantic> semantics, UiWidgetReference owner,
                    const UiRect& rectangle, const UiRect& clip, const Color& color, u64 message_id,
                    u32 message_revision, u16 measurement_index,
                    MessageTextMeasurementKind measurement_kind, float minimum_height, float gap_after,
                    bool themed_styles, UiTextAlignment horizontal_alignment = UiTextAlignment::start,
                    UiTextAlignment vertical_alignment = UiTextAlignment::start,
                    UiTextWrapping wrapping = UiTextWrapping::wrap, u8 measurement_slot = 0,
                    UiTextOverflow overflow = UiTextOverflow::clip,
                    const Color& inline_role_color = Color{}, u16 source_id = 0) noexcept {
    if (text.bytes == 0) {
        return true;
    }
    set_failure_operation(presentation, MessageTextPresentationOperation::append_command);
    if (!presentation || !presentation->commands.data || (!text.data && text.bytes != 0) ||
        (!styles.data && styles.count != 0) || (!semantics.data && semantics.count != 0)) {
        return fail(presentation, MessageTextPresentationError::model);
    }
    if (presentation->command_count >= presentation->commands.count) {
        return fail(presentation, MessageTextPresentationError::capacity);
    }
    const UiWidget* owner_widget =
        owner.manager ? ui_widget_manager_resolve(*owner.manager, owner.handle) : nullptr;
    if (!owner_widget || !ui_rect_has_area(rectangle) || !ui_rect_has_area(clip) || minimum_height < 0.0f ||
        gap_after < 0.0f) {
        return fail(presentation, MessageTextPresentationError::layout);
    }
    if (measurement_kind != MessageTextMeasurementKind::none &&
        measurement_index >= presentation->measurement_count) {
        return fail(presentation, MessageTextPresentationError::measurement);
    }
    MessageTextCommand command{text,
                               styles,
                               semantics,
                               owner,
                               rectangle,
                               clip,
                               color,
                               inline_role_color,
                               message_id,
                               0,
                               message_revision,
                               minimum_height,
                               gap_after,
                               0.0f,
                               0.0f,
                               measurement_index,
                               measurement_kind,
                               measurement_slot,
                               themed_styles,
                               horizontal_alignment,
                               vertical_alignment,
                               wrapping,
                               overflow,
                               source_id};
    command.measurement_identity = command_measurement_identity(command, owner_widget->id);
    previous_measurement(presentation, command.measurement_identity, &command.measured_height,
                         &command.measured_width);
    presentation->commands[presentation->command_count++] = command;
    return true;
}

UiRect line_rectangle(float x, float y, float width) noexcept {
    return UiRect{x, y, x + maximum(width, 1.0f), y + 4'096.0f};
}

bool begin_flow_item_clamped(UiTextFlowLayout* flow, float left_inset, float right_inset,
                             UiRect* rectangle) noexcept {
    if (!flow || !rectangle || !ui_extent_valid(left_inset) || !ui_extent_valid(right_inset)) {
        return false;
    }
    const float width = ui_rect_width(flow->content_rectangle);
    if (width <= 0.0f) {
        return false;
    }
    // Retained rows can briefly be narrower than the normal pane floor while DWM
    // drives an interactive resize. Fixed rich-content insets are decoration, not
    // a reason to reject the whole text stream: keep at least one positive DIP
    // for shaping and let the row clip hide the degenerate layout.
    const float inset_budget = maximum(0.0f, width - 1.0f);
    const float clamped_left = minimum(left_inset, inset_budget);
    const float clamped_right = minimum(right_inset, maximum(0.0f, inset_budget - clamped_left));
    return ui_text_flow_begin_item(flow, clamped_left, clamped_right, rectangle);
}

bool append_rich_line(MessageTextPresentation* presentation, Utf8View text, UiTextFlowLayout* flow,
                      float left_inset, float right_inset, const Color& color, UiWidgetReference owner,
                      u64 message_id, u32 message_revision,
                      u16 measurement_index = kInvalidMessageTextMeasurement,
                      MessageTextMeasurementKind measurement_kind = MessageTextMeasurementKind::none,
                      float gap_after = 3.0f) noexcept {
    if (!flow || (!text.data && text.bytes != 0)) {
        return fail(presentation, MessageTextPresentationError::model);
    }
    if (text.bytes == 0) {
        return true;
    }
    UiRect rectangle{};
    set_failure_operation(presentation, MessageTextPresentationOperation::begin_flow_item);
    if (!begin_flow_item_clamped(flow, left_inset, right_inset, &rectangle)) {
        if (presentation && flow) {
            presentation->failure_flow_error = flow->last_error;
        }
        return fail(presentation, MessageTextPresentationError::layout);
    }
    if (!append_command(presentation, text, ConstSlice<TextStyleSpan>{},
                        ConstSlice<MessageMarkdownSemantic>{}, owner, rectangle, flow->clip_rectangle,
                        color, message_id, message_revision, measurement_index, measurement_kind, 16.0f,
                        gap_after, false)) {
        return false;
    }
    const MessageTextCommand& command = presentation->commands[presentation->command_count - 1];
    set_failure_operation(presentation, MessageTextPresentationOperation::commit_flow_item);
    if (!ui_text_flow_commit_item(flow, command.measured_height, command.minimum_height,
                                  command.gap_after)) {
        presentation->failure_flow_error = flow->last_error;
        return fail(presentation, MessageTextPresentationError::layout);
    }
    saturating_accumulate(presentation->rich.flow_items);
    return true;
}

struct RetainedReplySummary {
    Utf8View text;
    ConstSlice<TextStyleSpan> styles;
};

Color member_role_color(const MessageTextPresentation& presentation, const ChatStateStore& state,
                        u64 channel_id, u64 user_id, const Color& fallback) noexcept {
    const ChannelRecord* channel = chat_state_store_find_channel(state, channel_id);
    const GuildMemberRecord* member =
        presentation.guild_members && channel && channel->guild_id != 0 && user_id != 0
            ? guild_member_store_find(*presentation.guild_members, channel->guild_id, user_id)
            : nullptr;
    return member && member->role_color != 0 ? role_color(member->role_color) : fallback;
}

struct RetainedAuthorHeader {
    Utf8View text;
    ConstSlice<TextStyleSpan> styles;
};

RetainedAuthorHeader retain_author_header(MessageTextPresentation* presentation, Utf8View name,
                                          u64 timestamp_ms, u64 now_unix_seconds) noexcept {
    if (!presentation || name.bytes == 0 || !name.data) {
        return RetainedAuthorHeader{};
    }
    char timestamp_storage[kMessageMarkdownTimestampMaximumBytes]{};
    const TimestampFormatResult timestamp =
        timestamp_ms != 0
            ? format_timestamp_local(timestamp_ms / 1'000u, 't', now_unix_seconds,
                                             Slice<char>{timestamp_storage, sizeof(timestamp_storage)})
            : TimestampFormatResult{};
    const u32 separator = timestamp.ok && timestamp.bytes != 0 ? 1u : 0u;
    const u32 timestamp_bytes = separator != 0 ? timestamp.bytes : 0u;
    if (name.bytes + separator + timestamp_bytes > presentation->utf8.count - presentation->utf8_count ||
        1u + (timestamp_bytes != 0 ? 1u : 0u) > presentation->styles.count - presentation->style_count) {
        return RetainedAuthorHeader{};
    }
    const u32 text_begin = presentation->utf8_count;
    const u32 style_begin = presentation->style_count;
    copy_bytes(presentation->utf8.data + presentation->utf8_count, name.data, name.bytes);
    presentation->utf8_count += name.bytes;
    presentation->styles[presentation->style_count++] = TextStyleSpan{0, name.bytes, text_style_bold};
    if (separator != 0) {
        presentation->utf8.data[presentation->utf8_count++] = ' ';
        for (u32 index = 0; index < timestamp_bytes; ++index) {
            presentation->utf8.data[presentation->utf8_count++] = timestamp_storage[index];
        }
        presentation->styles[presentation->style_count++] =
            TextStyleSpan{name.bytes + separator, timestamp_bytes, text_style_subtext};
    }
    return RetainedAuthorHeader{
        Utf8View{presentation->utf8.data + text_begin, presentation->utf8_count - text_begin},
        ConstSlice<TextStyleSpan>{presentation->styles.data + style_begin,
                                  presentation->style_count - style_begin}};
}

RetainedAuthorHeader retain_styled_text(MessageTextPresentation* presentation, Utf8View text, u32 flags) noexcept {
    if (!presentation || text.bytes == 0 || !text.data ||
        text.bytes > presentation->utf8.count - presentation->utf8_count ||
        presentation->style_count >= presentation->styles.count)
        return RetainedAuthorHeader{};
    const u32 text_begin = presentation->utf8_count;
    copy_bytes(presentation->utf8.data + text_begin, text.data, text.bytes);
    presentation->utf8_count += text.bytes;
    presentation->styles[presentation->style_count] = TextStyleSpan{0, text.bytes, flags};
    return RetainedAuthorHeader{Utf8View{presentation->utf8.data + text_begin, text.bytes},
                                ConstSlice<TextStyleSpan>{presentation->styles.data + presentation->style_count++, 1}};
}
float result_card_text_width(const MessageScene& scene, const MessageRecord& record, Utf8View text, u32 flags,
                             u16 item, float constraint) noexcept {
    UiTextMeasureCache* cache = scene.polls ? scene.polls->measure : nullptr;
    const TextStyleSpan span{0, text.bytes, flags};
    UiTextMeasureValue value{};
    u64 signature = hash64_counted_bytes(kHash64Seed, text.data, text.bytes);
    signature = hash64_mix(signature, flags);
    if (cache && constraint > 0.0f &&
        ui_text_measure_cache_measure(cache,
                                      {{record.id, signature, record.revision, item, scene.polls->backend_revision},
                                       text,
                                       ConstSlice<TextStyleSpan>{&span, 1},
                                       {constraint, kMaximumUiCoordinate},
                                       true},
                                      &value)
            .ok() &&
        ui_coordinate_valid(value.size.width) && value.size.width > 0.0f)
        return value.size.width;
    // Without a measurer, a generous per-byte estimate keeps the timestamp whole.
    return static_cast<float>(text.bytes) * ((flags & text_style_subtext) != 0 ? 7.0f : 9.0f);
}
// Search result cards have no day dividers, so the header carries the calendar
// timestamp ("Today at", "Yesterday at", or the short date) beside the name.
// Name and timestamp are separate commands laid out from measured widths: the
// timestamp always fits and follows the name, and a long name truncates first.
bool append_result_card_header(MessageTextPresentation* presentation, const MessageScene& scene,
                               const MessageSceneRowWidget& row, UiRect author, const UiRect& clip, Color color,
                               const MessageRecord& record, Utf8View name, u64 now_unix_seconds) noexcept {
    char storage[kMessageMarkdownTimestampMaximumBytes]{};
    const TimestampFormatResult stamp =
        record.timestamp_ms != 0
            ? format_timestamp_calendar(record.timestamp_ms / 1'000u, now_unix_seconds,
                                        Slice<char>{storage, sizeof(storage)})
            : TimestampFormatResult{};
    if (stamp.ok && stamp.next_update_unix_seconds != 0 &&
        (presentation->next_timestamp_update_unix_seconds == 0 ||
         stamp.next_update_unix_seconds < presentation->next_timestamp_update_unix_seconds))
        presentation->next_timestamp_update_unix_seconds = stamp.next_update_unix_seconds;
    const RetainedAuthorHeader retained_name = retain_styled_text(presentation, name, text_style_bold);
    const RetainedAuthorHeader retained_stamp =
        stamp.ok && stamp.bytes ? retain_styled_text(presentation, Utf8View{storage, stamp.bytes}, text_style_subtext)
                                : RetainedAuthorHeader{};
    if (retained_name.text.bytes == 0 || (stamp.ok && stamp.bytes && retained_stamp.text.bytes == 0))
        return fail(presentation, MessageTextPresentationError::capacity);
    const float width = ui_rect_width(author);
    const float stamp_width =
        retained_stamp.text.bytes
            ? result_card_text_width(scene, record, retained_stamp.text, text_style_subtext, 0x7473u, width)
            : 0.0f;
    const float name_width = result_card_text_width(scene, record, retained_name.text, text_style_bold, 0x6e6du, width);
    auto layout = message_result_card_header_layout(author.left, author.right, name_width, stamp_width);
    layout.name.top = layout.timestamp.top = author.top;
    layout.name.bottom = layout.timestamp.bottom = author.bottom;
    const UiWidgetReference owner{&scene.widgets, row.author};
    if (!append_command(presentation, retained_name.text, retained_name.styles, ConstSlice<MessageMarkdownSemantic>{},
                        owner, layout.name, clip, color, record.id, record.revision,
                        kInvalidMessageTextMeasurement, MessageTextMeasurementKind::none, 0.0f, 0.0f, true,
                        UiTextAlignment::start, UiTextAlignment::start, UiTextWrapping::no_wrap, 0,
                        UiTextOverflow::ellipsis))
        return false;
    return !ui_rect_has_area(layout.timestamp) ||
           append_command(presentation, retained_stamp.text, retained_stamp.styles,
                          ConstSlice<MessageMarkdownSemantic>{}, owner, layout.timestamp, clip, color, record.id,
                          record.revision, kInvalidMessageTextMeasurement, MessageTextMeasurementKind::none, 0.0f,
                          0.0f, true, UiTextAlignment::start, UiTextAlignment::start, UiTextWrapping::no_wrap, 1,
                          UiTextOverflow::clip);
}

// The reply preview ansi rule: /\x1B\[(\d+(?:[:;]\d+)*)m/ inside ```ansi.
u32 reply_sgr_bytes(const char* text, u32 bytes) noexcept {
    if (bytes < 4 || text[0] != '\x1B' || text[1] != '[' || text[2] < '0' || text[2] > '9')
        return 0;
    u32 at = 2;
    while (true) {
        while (at < bytes && text[at] >= '0' && text[at] <= '9')
            ++at;
        if (at < bytes && text[at] == 'm')
            return at + 1;
        if (at + 1 < bytes && (text[at] == ';' || text[at] == ':') && text[at + 1] >= '0' && text[at + 1] <= '9') {
            ++at;
            continue;
        }
        return 0;
    }
}

// The bounded parse source for a reply preview. SGR escapes inside ```ansi
// fences leave first (the inline reply rules drop them anyway), so a heavily
// coloured message still reaches its closing fence; a fence the cap cuts is
// closed so it stays one inline code run.
u32 reply_preview_source(Utf8View content, char* output, u32 capacity) noexcept {
    u32 used = 0, at = 0;
    bool fence = false, ansi = false;
    const auto lower = [](char value) noexcept {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value + 32) : value;
    };
    while (at < content.bytes) {
        const char* text = content.data + at;
        const u32 left = content.bytes - at;
        if (left >= 3 && text[0] == '`' && text[1] == '`' && text[2] == '`') {
            if (used + 3 > capacity)
                break;
            ansi = !fence && left >= 8 && lower(text[3]) == 'a' && lower(text[4]) == 'n' && lower(text[5]) == 's' &&
                   lower(text[6]) == 'i' && (text[7] == '\n' || (text[7] == '\r' && left >= 9 && text[8] == '\n'));
            fence = !fence;
            copy_bytes(output + used, text, 3);
            used += 3;
            at += 3;
            continue;
        }
        if (const u32 escape = ansi ? reply_sgr_bytes(text, left) : 0u) {
            at += escape;
            continue;
        }
        if (used == capacity)
            break;
        output[used++] = text[0];
        ++at;
    }
    if (at < content.bytes) {
        // Never end inside a UTF-8 sequence.
        u32 lead = used;
        while (lead && (static_cast<u8>(output[lead - 1]) & 0xC0u) == 0x80u)
            --lead;
        if (lead) {
            const u8 first = static_cast<u8>(output[lead - 1]);
            const u32 width = first < 0x80u ? 1u : first >= 0xF0u ? 4u : first >= 0xE0u ? 3u : 2u;
            if (used - (lead - 1) < width)
                used = lead - 1;
        }
        if (fence) {
            copy_bytes(output + used, "\n```", 4);
            used += 4;
        }
    }
    return used;
}

RetainedReplySummary retain_reply_summary(MessageTextPresentation* presentation, BuildContext* context,
                                          Utf8View author, Utf8View content, bool available = false,
                                          u8 content_flags = 0, u8 message_type = 0,
                                          Utf8View poll_question = {}) noexcept {
    if (!presentation || !context || (!author.data && author.bytes != 0) ||
        (!content.data && content.bytes != 0) || (!poll_question.data && poll_question.bytes != 0)) {
        return RetainedReplySummary{};
    }

    // The reply preview: the referenced text through the inline reply rules
    // (code fences inline, ```ansi escapes dropped, no block quotes), every
    // line break shown as a space, on one visual line. Without text it shows
    // the poll question or an italic placeholder.
    constexpr u32 maximum_source_bytes = 2'048;
    constexpr u32 maximum_display_bytes = 2'560;
    constexpr u32 maximum_styles = 128;
    // Outputs and scratch, written before they are read (the presentation's
    // own byte-style scratch is never cleared between parses either).
    char source[maximum_source_bytes + 4];
    char transformed[maximum_display_bytes];
    u32 byte_styles[maximum_display_bytes];
    TextStyleSpan transformed_styles[maximum_styles];

    const bool poll = (content_flags & gateway_reference_poll_question) != 0;
    Utf8View display_content{};
    u32 display_style_count = 0;
    Utf8View placeholder{};
    MessageMarkdownResult parsed{};
    bool parsed_markdown = false;
    if (available && message_type == kMessageTypePinned) {
        placeholder = utf8_literal("Message pinned.");
    } else if (poll) {
        display_content = content;
    } else if (content.bytes != 0) {
        const u32 source_bytes = reply_preview_source(content, source, maximum_source_bytes);
        MessageMarkdownOptions options{
            context, resolve_entity, format_timestamp, nullptr, 0, MessageMarkdownSemanticKind::none, false};
        options.inline_reply = true;
        parsed = cached_markdown_parse(
            presentation, context, MarkdownCacheMode::reply, Utf8View{source, source_bytes},
            Slice<char>{transformed, maximum_display_bytes}, Slice<u32>{byte_styles, maximum_display_bytes},
            Slice<TextStyleSpan>{transformed_styles, maximum_styles}, options, Slice<MessageMarkdownSemantic>{});
        parsed_markdown = true;
        if (parsed.ok()) {
            display_content = parsed.text;
            display_style_count = parsed.span_count;
        } else {
            display_content = Utf8View{source, source_bytes};
        }
    }
    if (available && !placeholder.bytes && !display_content.bytes) {
        if (poll_question.bytes)
            display_content = poll_question;
        else if (content_flags & gateway_reference_stickers)
            placeholder = utf8_literal("Click to see sticker");
        else if (content_flags & gateway_reference_interaction)
            placeholder = utf8_literal("Click to see command");
        else if (content_flags & gateway_reference_voice_message)
            placeholder = utf8_literal("Click to see voice message");
        else if (content_flags & gateway_reference_components_v2)
            placeholder = utf8_literal("Click to see message");
        else
            placeholder = utf8_literal("Click to see attachment");
    }
    if (placeholder.bytes) {
        display_content = placeholder;
        display_style_count = 0;
    }
    const u32 separator = author.bytes != 0 && display_content.bytes != 0 ? 1u : 0u;
    const u32 retained_style_count =
        (author.bytes != 0 ? 1u : 0u) + display_style_count + (placeholder.bytes ? 1u : 0u);
    if (author.bytes > presentation->utf8.count - presentation->utf8_count ||
        separator + display_content.bytes >
            presentation->utf8.count - presentation->utf8_count - author.bytes ||
        retained_style_count > presentation->styles.count - presentation->style_count) {
        return RetainedReplySummary{};
    }

    const u32 text_begin = presentation->utf8_count;
    const u32 style_begin = presentation->style_count;
    copy_bytes(presentation->utf8.data + presentation->utf8_count, author.data, author.bytes);
    presentation->utf8_count += author.bytes;
    if (author.bytes != 0) {
        presentation->styles[presentation->style_count++] =
            TextStyleSpan{0, author.bytes, text_style_bold | text_style_inline_role_color};
    }
    if (separator != 0) {
        presentation->utf8.data[presentation->utf8_count++] = ' ';
    }
    char* content_out = presentation->utf8.data + presentation->utf8_count;
    copy_bytes(content_out, display_content.data, display_content.bytes);
    // Newlines inside text, inline code and emphasis render as spaces.
    for (u32 index = 0; index < display_content.bytes; ++index)
        if (content_out[index] == '\n' || content_out[index] == '\r')
            content_out[index] = ' ';
    presentation->utf8_count += display_content.bytes;
    const u32 content_offset = author.bytes + separator;
    if (placeholder.bytes) {
        presentation->styles[presentation->style_count++] =
            TextStyleSpan{content_offset, placeholder.bytes, text_style_italic};
    }
    for (u32 index = 0; index < display_style_count; ++index) {
        const TextStyleSpan& source_span = transformed_styles[index];
        const u32 flags = source_span.flags & ~(text_style_jumbo_emoji | text_style_quote);
        // An emoji-only reply has no remaining style after removing its jumbo
        // presentation. Normal text is represented by a gap, not a zero-flag
        // span (which is invalid to both measurement and glyph shaping).
        if (!flags)
            continue;
        presentation->styles[presentation->style_count++] =
            TextStyleSpan{content_offset + source_span.byte_offset, source_span.byte_count, flags};
    }
    if (parsed_markdown && parsed.ok()) {
        saturating_accumulate(presentation->markdown_transformations, parsed.transformations);
        saturating_accumulate(presentation->markdown_spans_emitted, parsed.span_count);
        saturating_accumulate(presentation->markdown_resolved_entities, parsed.resolved_entities);
        saturating_accumulate(presentation->markdown_unresolved_entities, parsed.unresolved_entities);
        saturating_accumulate(presentation->markdown_timestamps, parsed.timestamps);
    } else if (parsed_markdown) {
        presentation->last_markdown_error = parsed.error;
        saturating_accumulate(presentation->markdown_rejected);
    }
    return RetainedReplySummary{
        Utf8View{presentation->utf8.data + text_begin, presentation->utf8_count - text_begin},
        ConstSlice<TextStyleSpan>{presentation->styles.data + style_begin,
                                  presentation->style_count - style_begin}};
}

// "Original message was deleted" / "Message could not be loaded": an italic
// placeholder without an author.
RetainedReplySummary retain_reply_placeholder(MessageTextPresentation* presentation, Utf8View text) noexcept {
    if (!text.bytes || text.bytes > presentation->utf8.count - presentation->utf8_count ||
        presentation->style_count >= presentation->styles.count)
        return {};
    char* out = presentation->utf8.data + presentation->utf8_count;
    copy_bytes(out, text.data, text.bytes);
    presentation->utf8_count += text.bytes;
    auto* style = presentation->styles.data + presentation->style_count++;
    *style = TextStyleSpan{0, text.bytes, text_style_italic};
    return {{out, text.bytes}, {style, 1}};
}

RetainedReplySummary retain_interaction_header(MessageTextPresentation* presentation, Utf8View user,
                                               Utf8View command) noexcept {
    const auto separator = utf8_literal(" used /");
    const u32 bytes = user.bytes + separator.bytes + command.bytes;
    if (bytes > presentation->utf8.count - presentation->utf8_count ||
        presentation->styles.count - presentation->style_count < 2)
        return {};
    char* text = presentation->utf8.data + presentation->utf8_count;
    copy_bytes(text, user.data, user.bytes);
    copy_bytes(text + user.bytes, separator.data, separator.bytes);
    copy_bytes(text + user.bytes + separator.bytes, command.data, command.bytes);
    auto* styles = presentation->styles.data + presentation->style_count;
    styles[0] = {0, user.bytes, text_style_inline_role_color};
    styles[1] = {user.bytes + separator.bytes - 1, command.bytes + 1, text_style_mention};
    presentation->utf8_count += bytes;
    presentation->style_count += 2;
    return {{text, bytes}, {styles, 2}};
}

bool append_reply_line(MessageTextPresentation* presentation, Utf8View text,
                       ConstSlice<TextStyleSpan> styles, UiTextFlowLayout* flow, const Color& color,
                       const Color& inline_role_color, UiWidgetReference owner, u64 message_id,
                       u32 message_revision, u16 measurement_index, UiTextOverflow overflow,
                       float right_inset = 0.0f) noexcept {
    if (!presentation || !flow || text.bytes == 0) {
        return false;
    }
    UiRect rectangle{};
    // Reserve the leading 16-DIP referenced-author avatar plus a 4-DIP gap,
    // and room for a trailing 20-DIP icon (4-DIP gap) when one follows.
    set_failure_operation(presentation, MessageTextPresentationOperation::begin_flow_item);
    if (!begin_flow_item_clamped(flow, 20.0f, right_inset, &rectangle)) {
        presentation->failure_flow_error = flow->last_error;
        return fail(presentation, MessageTextPresentationError::layout);
    }
    // Bound shaping to one line; the renderer applies character-granularity
    // ellipsis inside this retained width.
    rectangle.bottom = rectangle.top + 18.0f;
    if (!append_command(presentation, text, styles, ConstSlice<MessageMarkdownSemantic>{}, owner, rectangle,
                        flow->clip_rectangle, color, message_id, message_revision, measurement_index,
                        MessageTextMeasurementKind::reply, 16.0f, 6.0f, styles.count != 0,
                        UiTextAlignment::start, UiTextAlignment::start, UiTextWrapping::no_wrap, 0,
                        overflow, inline_role_color)) {
        return false;
    }
    const MessageTextCommand& command = presentation->commands[presentation->command_count - 1];
    set_failure_operation(presentation, MessageTextPresentationOperation::commit_flow_item);
    if (!ui_text_flow_commit_item(flow, command.measured_height, command.minimum_height,
                                  command.gap_after)) {
        presentation->failure_flow_error = flow->last_error;
        return fail(presentation, MessageTextPresentationError::layout);
    }
    saturating_accumulate(presentation->rich.flow_items);
    return true;
}

bool initialize_flow(MessageTextPresentation* presentation, const UiLayoutNode& region, float text_left,
                     float text_width, const UiRect& clip, UiTextFlowLayout* flow) noexcept {
    set_failure_operation(presentation, MessageTextPresentationOperation::initialize_flow);
    const UiRect content{text_left, region.content_rectangle.top, text_left + text_width,
                         region.content_rectangle.bottom};
    if (ui_text_flow_initialize(flow, content, clip)) {
        return true;
    }
    if (presentation && flow) {
        presentation->failure_flow_error = flow->last_error;
    }
    return fail(presentation, MessageTextPresentationError::layout);
}

bool finish_flow(MessageTextPresentation* presentation, const UiTextFlowLayout& flow,
                 float* height) noexcept {
    set_failure_operation(presentation, MessageTextPresentationOperation::finish_flow, flow.last_error);
    if (!height || !flow.active || flow.item_open || flow.last_error != UiTextFlowError::none) {
        return fail(presentation, MessageTextPresentationError::layout);
    }
    *height = ui_text_flow_required_height(flow);
    if (flow.overflow) {
        saturating_accumulate(presentation->rich.flow_overflow_regions);
    }
    return true;
}

float rich_item_total(const MessageSceneRichItemLayout& layout, u32 begin, u32 count) noexcept {
    float total = 0.0f;
    for (u32 index = 0; index < count; ++index) {
        total += layout.heights[begin + index];
    }
    return total;
}

bool parse_content(MessageTextPresentation* presentation, BuildContext* context, Utf8View source,
                   Utf8View* text, ConstSlice<TextStyleSpan>* styles,
                   ConstSlice<MessageMarkdownSemantic>* semantics, u32 base_style = 0,
                   u8 base_size = text_size_inherit) noexcept;

// Compact body: "Name content" as one flow. The name prefix precedes the
// parsed text in the retained buffers so styles and semantics shift by it.
// A plugin name prefix/suffix wraps the name as muted subtext (in the role
// colour for MC_TONE_ROLE); the name stays the bold role-coloured span.
bool compact_content(MessageTextPresentation* presentation, BuildContext* context, Utf8View name,
                     Utf8View source, Utf8View* text, ConstSlice<TextStyleSpan>* styles,
                     ConstSlice<MessageMarkdownSemantic>* semantics,
                     const UiPluginRenderDecoration* decoration = nullptr) noexcept {
    const Utf8View name_prefix = decoration ? decoration->name_prefix : Utf8View{};
    const Utf8View name_suffix = decoration ? decoration->name_suffix : Utf8View{};
    const u32 lead = name_prefix.bytes ? name_prefix.bytes + 1u : 0u;
    const u32 tail = name_suffix.bytes ? name_suffix.bytes + 1u : 0u;
    const u32 prefix = lead + name.bytes + tail + 1u;
    const u32 leading_spans = 1u + (lead ? 1u : 0u) + (tail ? 1u : 0u);
    if (presentation->utf8_count > presentation->utf8.count ||
        prefix > presentation->utf8.count - presentation->utf8_count ||
        leading_spans > presentation->styles.count - presentation->style_count) {
        return fail(presentation, MessageTextPresentationError::capacity);
    }
    const u32 text_begin = presentation->utf8_count;
    const u32 style_begin = presentation->style_count;
    const u32 decoration_flags =
        text_style_subtext | (decoration && decoration->name_tone == kUiPluginToneRole ? text_style_inline_role_color : 0u);
    char* out = presentation->utf8.data + text_begin;
    if (lead) {
        copy_bytes(out, name_prefix.data, name_prefix.bytes);
        out[name_prefix.bytes] = ' ';
        presentation->styles[presentation->style_count++] = TextStyleSpan{0, name_prefix.bytes, decoration_flags};
    }
    copy_bytes(out + lead, name.data, name.bytes);
    presentation->styles[presentation->style_count++] =
        TextStyleSpan{lead, name.bytes, text_style_bold | text_style_inline_role_color};
    if (tail) {
        out[lead + name.bytes] = ' ';
        copy_bytes(out + lead + name.bytes + 1u, name_suffix.data, name_suffix.bytes);
        presentation->styles[presentation->style_count++] =
            TextStyleSpan{lead + name.bytes + 1u, name_suffix.bytes, decoration_flags};
    }
    out[prefix - 1u] = ' ';
    presentation->utf8_count += prefix;
    const u32 semantic_begin = presentation->semantic_count;
    Utf8View parsed = source;
    ConstSlice<TextStyleSpan> parsed_styles{};
    ConstSlice<MessageMarkdownSemantic> parsed_semantics{};
    if (source.bytes && !parse_content(presentation, context, source, &parsed, &parsed_styles, &parsed_semantics))
        return false;
    if (parsed.bytes && parsed.data != presentation->utf8.data + text_begin + prefix) {
        // Markdown rejected the source: append it verbatim after the name.
        if (parsed.bytes > presentation->utf8.count - presentation->utf8_count)
            return fail(presentation, MessageTextPresentationError::capacity);
        copy_bytes(presentation->utf8.data + presentation->utf8_count, parsed.data, parsed.bytes);
        presentation->utf8_count += parsed.bytes;
    }
    for (u32 index = style_begin + leading_spans; index < presentation->style_count; ++index)
        presentation->styles[index].byte_offset += prefix;
    for (u32 index = semantic_begin; index < presentation->semantic_count; ++index)
        presentation->semantics[index].byte_offset += prefix;
    if (!parsed.bytes)
        --presentation->utf8_count; // no trailing separator after a bare name
    *text = Utf8View{presentation->utf8.data + text_begin, presentation->utf8_count - text_begin};
    *styles = ConstSlice<TextStyleSpan>{presentation->styles.data + style_begin,
                                        presentation->style_count - style_begin};
    *semantics = ConstSlice<MessageMarkdownSemantic>{presentation->semantics.data + semantic_begin,
                                                     presentation->semantic_count - semantic_begin};
    return true;
}

bool append_compact_timestamp(MessageTextPresentation* presentation, const MessageScene& scene,
                              const MessageSceneRowWidget& row, const UiRect& clip, const Theme& theme,
                              const MessageRecord& record, u64 now_unix_seconds) noexcept {
    const UiLayoutNode* gutter = message_scene_layout_node(scene, row.avatar);
    if (!record.timestamp_ms || !gutter || !ui_rect_has_area(gutter->rectangle))
        return true;
    char storage[kMessageMarkdownTimestampMaximumBytes]{};
    const auto formatted = format_timestamp_local(record.timestamp_ms / 1'000u, 't', now_unix_seconds,
                                                          Slice<char>{storage, sizeof(storage)});
    if (!formatted.ok || !formatted.bytes || formatted.bytes > presentation->utf8.count - presentation->utf8_count ||
        presentation->style_count >= presentation->styles.count)
        return true;
    const u32 text_begin = presentation->utf8_count;
    copy_bytes(presentation->utf8.data + text_begin, storage, formatted.bytes);
    presentation->utf8_count += formatted.bytes;
    TextStyleSpan* style = presentation->styles.data + presentation->style_count++;
    *style = TextStyleSpan{0, formatted.bytes, text_style_subtext};
    return append_command(presentation, Utf8View{presentation->utf8.data + text_begin, formatted.bytes}, {style, 1},
                          {}, UiWidgetReference{&scene.widgets, row.avatar}, gutter->rectangle, clip,
                          ui_background_muted_text_color(theme), record.id, record.revision,
                          kInvalidMessageTextMeasurement, MessageTextMeasurementKind::none, 0.0f, 0.0f, true,
                          UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap, 0,
                          UiTextOverflow::ellipsis);
}

bool parse_content(MessageTextPresentation* presentation, BuildContext* context, Utf8View source,
                   Utf8View* text, ConstSlice<TextStyleSpan>* styles,
                   ConstSlice<MessageMarkdownSemantic>* semantics, u32 base_style, u8 base_size) noexcept {
    if (!presentation || !context || !text || !styles || !semantics) {
        return fail(presentation, MessageTextPresentationError::model);
    }
    if (presentation->utf8_count > presentation->utf8.count ||
        presentation->style_count > presentation->styles.count ||
        presentation->semantic_count > presentation->semantics.count ||
        source.bytes > presentation->measurement_history_offset) {
        return fail(presentation, MessageTextPresentationError::capacity);
    }
    const u32 text_begin = presentation->utf8_count;
    const u32 style_begin = presentation->style_count;
    const u32 semantic_begin = presentation->semantic_count;
    MessageMarkdownOptions options{context,
                                   resolve_entity,
                                   format_timestamp,
                                   spoiler_revealed,
                                   presentation->hovered_source_offset & 0xFFFFu,
                                   presentation->hovered_kind,
                                   presentation->has_hover &&
                                       presentation->hovered_message_id == context->message->id &&
                                       (presentation->hovered_source_offset >> 16u) == context->source_id};
    options.base_style = base_style;
    options.no_links = context->markdown_rules == 1;
    options.no_headings = context->markdown_rules != 0;
    options.no_lists = context->markdown_rules == 1;
    options.text_sizes = true;
    options.base_size = base_size;
    const MessageMarkdownResult parsed = cached_markdown_parse(
        presentation, context, MarkdownCacheMode::content, source,
        Slice<char>{presentation->utf8.data + text_begin, presentation->utf8.count - text_begin},
        Slice<u32>{presentation->byte_style_scratch.data, presentation->measurement_history_offset},
        Slice<TextStyleSpan>{presentation->styles.data + style_begin,
                             presentation->styles.count - style_begin},
        options,
        Slice<MessageMarkdownSemantic>{
            presentation->semantics.data + semantic_begin,
            minimum(presentation->semantics.count - semantic_begin, kMessageMarkdownSemanticMaximum)});
    if (!parsed.ok()) {
        presentation->last_markdown_error = parsed.error;
        saturating_accumulate(presentation->markdown_rejected);
        return true;
    }
    presentation->utf8_count += parsed.text.bytes;
    presentation->style_count += parsed.span_count;
    presentation->semantic_count += parsed.semantic_count;
    for (u32 index = 0; index < parsed.semantic_count; ++index) {
        auto& semantic = presentation->semantics[semantic_begin + index];
        semantic.source_offset = scoped_source_offset(context->source_id, semantic.source_offset);
    }
    *text = parsed.text;
    *styles = ConstSlice<TextStyleSpan>{presentation->styles.data + style_begin, parsed.span_count};
    *semantics = ConstSlice<MessageMarkdownSemantic>{presentation->semantics.data + semantic_begin,
                                                     parsed.semantic_count};
    saturating_accumulate(presentation->markdown_transformations, parsed.transformations);
    saturating_accumulate(presentation->markdown_spans_emitted, parsed.span_count);
    saturating_accumulate(presentation->markdown_syntax_tokens, parsed.syntax_tokens);
    saturating_accumulate(presentation->markdown_syntax_styled_bytes, parsed.syntax_styled_bytes);
    saturating_accumulate(presentation->markdown_list_items, parsed.list_items);
    saturating_accumulate(presentation->markdown_resolved_entities, parsed.resolved_entities);
    saturating_accumulate(presentation->markdown_unresolved_entities, parsed.unresolved_entities);
    saturating_accumulate(presentation->markdown_timestamps, parsed.timestamps);
    if (parsed.syntax_truncated) {
        saturating_accumulate(presentation->markdown_syntax_truncated);
    }
    if (parsed.semantics_truncated) {
        saturating_accumulate(presentation->markdown_semantics_truncated);
    }
    if (parsed.next_timestamp_update_unix_seconds != 0 &&
        (presentation->next_timestamp_update_unix_seconds == 0 ||
         parsed.next_timestamp_update_unix_seconds < presentation->next_timestamp_update_unix_seconds)) {
        presentation->next_timestamp_update_unix_seconds = parsed.next_timestamp_update_unix_seconds;
    }
    return true;
}

// A quote renders as a block: a 4-DIP spine, 12/8-DIP inline padding, at most
// 90% of the markup width and 4-DIP block margins. Runs of quoted and plain
// lines become separate commands, so wrapped quote lines keep the indent; the
// newline between two runs belongs to neither.
constexpr float kQuoteSpine = 4.0f;
constexpr float kQuoteStartPadding = 12.0f;
constexpr float kQuoteEndPadding = 8.0f;
constexpr float kQuoteMargin = 4.0f;
constexpr u32 kQuoteSegmentMaximum = 32;

struct QuoteSegment {
    Utf8View text;
    ConstSlice<TextStyleSpan> styles;
    ConstSlice<MessageMarkdownSemantic> semantics;
    bool quote;
};

// Text width of a quote inside a markup box `width` wide; the text starts
// quote_text_inset() after the box.
float quote_text_width(float width) noexcept {
    const float block = minimum(width - kQuoteSpine, width * 0.9f);
    return maximum(1.0f, block - kQuoteStartPadding - kQuoteEndPadding);
}
constexpr float quote_text_inset() noexcept {
    return kQuoteSpine + kQuoteStartPadding;
}

// Splits presentation-owned parsed text at quote boundaries, rebasing its
// spans and semantics in place. Returns 0 (nothing changed) when the text has
// no quote, too many runs, or a span/semantic that crosses a boundary.
u32 split_quote_segments(Utf8View text, ConstSlice<TextStyleSpan> styles,
                         ConstSlice<MessageMarkdownSemantic> semantics, QuoteSegment* output) noexcept {
    struct Range {
        u32 begin, end;
        bool quote;
    } ranges[kQuoteSegmentMaximum];
    u32 count = 0;
    bool quoted = false;
    u32 style = 0;
    for (u32 line = 0; line < text.bytes;) {
        u32 end = line;
        while (end < text.bytes && text.data[end] != '\n')
            ++end;
        while (style < styles.count && styles[style].byte_offset + styles[style].byte_count <= line)
            ++style;
        const bool quote = style < styles.count && styles[style].byte_offset <= line &&
                           (styles[style].flags & text_style_quote) != 0;
        quoted = quoted || quote;
        if (count && ranges[count - 1].quote == quote) {
            ranges[count - 1].end = end;
        } else {
            if (count == kQuoteSegmentMaximum)
                return 0;
            ranges[count++] = {line, end, quote};
        }
        line = end + 1;
    }
    if (!quoted)
        return 0;
    // A span or semantic may reach at most the newline after its run.
    const auto segment_of = [&](u32 offset, u32 bytes, u32 from) noexcept {
        while (from < count && offset > ranges[from].end)
            ++from;
        return from < count && offset + bytes <= ranges[from].end + 1 ? from : count;
    };
    for (u32 i = 0, k = 0; i < styles.count; ++i)
        if ((k = segment_of(styles[i].byte_offset, styles[i].byte_count, k)) == count)
            return 0;
    for (u32 i = 0, k = 0; i < semantics.count; ++i)
        if ((k = segment_of(semantics[i].byte_offset, semantics[i].byte_count, k)) == count)
            return 0;
    auto* spans = const_cast<TextStyleSpan*>(styles.data);
    auto* marks = const_cast<MessageMarkdownSemantic*>(semantics.data);
    u32 read_style = 0, write_style = 0, read_mark = 0, write_mark = 0;
    for (u32 k = 0; k < count; ++k) {
        const Range range = ranges[k];
        const u32 first_style = write_style, first_mark = write_mark;
        for (; read_style < styles.count && spans[read_style].byte_offset <= range.end; ++read_style) {
            TextStyleSpan span = spans[read_style];
            const u32 begin = maximum(span.byte_offset, range.begin);
            const u32 end = minimum(span.byte_offset + span.byte_count, range.end);
            if (end <= begin)
                continue;
            span.byte_offset = begin - range.begin;
            span.byte_count = end - begin;
            spans[write_style++] = span;
        }
        for (; read_mark < semantics.count && marks[read_mark].byte_offset <= range.end; ++read_mark) {
            MessageMarkdownSemantic mark = marks[read_mark];
            const u32 begin = maximum(mark.byte_offset, range.begin);
            const u32 end = minimum(mark.byte_offset + mark.byte_count, range.end);
            if (end <= begin)
                continue;
            mark.byte_offset = begin - range.begin;
            mark.byte_count = end - begin;
            marks[write_mark++] = mark;
        }
        output[k] = QuoteSegment{{text.data + range.begin, range.end - range.begin},
                                 {spans + first_style, write_style - first_style},
                                 {marks + first_mark, write_mark - first_mark},
                                 range.quote};
    }
    return count;
}

// The margin after non-empty segment `index`: the 4-DIP quote margin
// when it or the next non-empty segment is a quote; `last` for the final one.
float quote_gap_after(const QuoteSegment* segments, u32 count, u32 index, float last) noexcept {
    for (u32 next = index + 1; next < count; ++next)
        if (segments[next].text.bytes)
            return segments[index].quote || segments[next].quote ? kQuoteMargin : 0.0f;
    return last;
}

bool append_rich_markdown_line(MessageTextPresentation* presentation, BuildContext* context,
                               Utf8View source, UiTextFlowLayout* flow, float left_inset, float right_inset,
                               const Color& color, UiWidgetReference owner, u64 message_id,
                               u32 message_revision, u16 measurement_index,
                               MessageTextMeasurementKind measurement_kind, float gap_after = 3.0f,
                               u32 base_style = 0, u8 base_size = text_size_inherit) noexcept {
    if (!presentation || !context || !flow || (!source.data && source.bytes != 0)) {
        return fail(presentation, MessageTextPresentationError::model);
    }
    if (source.bytes == 0) {
        return true;
    }
    Utf8View display = source;
    ConstSlice<TextStyleSpan> styles{};
    ConstSlice<MessageMarkdownSemantic> semantics{};
    context->source_id = identify_rich_source(*context, source);
    if (!parse_content(presentation, context, source, &display, &styles, &semantics, base_style, base_size)) {
        return false;
    }
    QuoteSegment segments[kQuoteSegmentMaximum];
    u32 segment_count = split_quote_segments(display, styles, semantics, segments);
    if (segment_count == 0) {
        segments[0] = QuoteSegment{display, styles, semantics, false};
        segment_count = 1;
    }
    for (u32 index = 0; index < segment_count; ++index) {
        const QuoteSegment& segment = segments[index];
        if (segment.text.bytes == 0)
            continue;
        float left = left_inset, right = right_inset;
        if (segment.quote) {
            const float width = maximum(1.0f, ui_rect_width(flow->content_rectangle) - left_inset - right_inset);
            left += quote_text_inset();
            right += maximum(0.0f, width - quote_text_inset() - quote_text_width(width));
        }
        UiRect rectangle{};
        set_failure_operation(presentation, MessageTextPresentationOperation::begin_flow_item);
        if (!begin_flow_item_clamped(flow, left, right, &rectangle)) {
            presentation->failure_flow_error = flow->last_error;
            return fail(presentation, MessageTextPresentationError::layout);
        }
        if (!append_command(presentation, segment.text, segment.styles, segment.semantics, owner, rectangle,
                            flow->clip_rectangle, color, message_id, message_revision, measurement_index,
                            measurement_kind, 16.0f, quote_gap_after(segments, segment_count, index, gap_after),
                            segment.styles.count != 0, UiTextAlignment::start, UiTextAlignment::start,
                            UiTextWrapping::wrap, 0, UiTextOverflow::clip, Color{}, context->source_id)) {
            return false;
        }
        const MessageTextCommand& command = presentation->commands[presentation->command_count - 1];
        set_failure_operation(presentation, MessageTextPresentationOperation::commit_flow_item);
        if (!ui_text_flow_commit_item(flow, command.measured_height, command.minimum_height,
                                      command.gap_after)) {
            presentation->failure_flow_error = flow->last_error;
            return fail(presentation, MessageTextPresentationError::layout);
        }
        saturating_accumulate(presentation->rich.flow_items);
    }
    return true;
}

// One embed text block that is not markdown (author, footer, provider, and
// every part of a non-rich embed): the whole line takes `flags`; a non-empty
// `link` makes it one link semantic addressing that rich source slot.
bool append_embed_plain_line(MessageTextPresentation* presentation, BuildContext* context, Utf8View text,
                             UiTextFlowLayout* flow, float left_inset, float right_inset, const Color& color,
                             UiWidgetReference owner, u64 message_id, u32 message_revision,
                             u16 measurement_index, u32 flags, Utf8View link, u16 link_source,
                             float minimum_height = 16.0f, u16 source_id = 0,
                             u8 size = text_size_inherit) noexcept {
    if (!presentation || !context || !flow || (!text.data && text.bytes != 0))
        return fail(presentation, MessageTextPresentationError::model);
    if (text.bytes == 0)
        return true;
    const bool linked = link.bytes != 0 && link_source != 0 && link.bytes <= 0xFFFFu;
    const u32 scoped = linked ? scoped_source_offset(link_source, 0) : 0;
    if (linked && presentation->has_hover && presentation->hovered_message_id == message_id &&
        presentation->hovered_kind == MessageMarkdownSemanticKind::link && presentation->hovered_source_offset == scoped)
        flags |= text_style_underline | text_style_interactive_hover;
    ConstSlice<TextStyleSpan> styles{};
    ConstSlice<MessageMarkdownSemantic> semantics{};
    if (flags || size) {
        if (presentation->style_count >= presentation->styles.count)
            return fail(presentation, MessageTextPresentationError::capacity);
        presentation->styles[presentation->style_count] = TextStyleSpan{0, text.bytes, flags, size};
        styles = {presentation->styles.data + presentation->style_count++, 1};
    }
    if (linked && presentation->semantic_count < presentation->semantics.count) {
        presentation->semantics[presentation->semantic_count] = MessageMarkdownSemantic{
            0, 0, text.bytes, scoped, static_cast<u16>(link.bytes), MessageMarkdownSemanticKind::link, 0};
        semantics = {presentation->semantics.data + presentation->semantic_count++, 1};
    }
    UiRect rectangle{};
    set_failure_operation(presentation, MessageTextPresentationOperation::begin_flow_item);
    if (!begin_flow_item_clamped(flow, left_inset, right_inset, &rectangle)) {
        presentation->failure_flow_error = flow->last_error;
        return fail(presentation, MessageTextPresentationError::layout);
    }
    if (!append_command(presentation, text, styles, semantics, owner, rectangle, flow->clip_rectangle, color,
                        message_id, message_revision, measurement_index, MessageTextMeasurementKind::embed,
                        minimum_height, 0.0f, styles.count != 0, UiTextAlignment::start, UiTextAlignment::start,
                        UiTextWrapping::wrap, 0, UiTextOverflow::clip, Color{}, source_id))
        return false;
    const MessageTextCommand& command = presentation->commands[presentation->command_count - 1];
    set_failure_operation(presentation, MessageTextPresentationOperation::commit_flow_item);
    if (!ui_text_flow_commit_item(flow, command.measured_height, command.minimum_height, 0.0f)) {
        presentation->failure_flow_error = flow->last_error;
        return fail(presentation, MessageTextPresentationError::layout);
    }
    saturating_accumulate(presentation->rich.flow_items);
    return true;
}

// A rich embed markdown block (title, description, field); non-rich embeds
// show the raw string. A title with embed.url becomes one link to it.
bool append_embed_block(MessageTextPresentation* presentation, BuildContext* context, Utf8View source,
                        UiTextFlowLayout* flow, float left_inset, float right_inset, const Color& color,
                        UiWidgetReference owner, u64 message_id, u32 message_revision, u16 measurement_index,
                        bool markdown, u8 rules, u32 base_style, u8 base_size, Utf8View link = {},
                        u16 link_source = 0) noexcept {
    if (source.bytes == 0)
        return true;
    if (!markdown) {
        return append_embed_plain_line(presentation, context, source, flow, left_inset, right_inset, color, owner,
                                       message_id, message_revision, measurement_index, base_style, link,
                                       link_source, 16.0f, identify_rich_source(*context, source), base_size);
    }
    const bool linked = link.bytes != 0 && link_source != 0 && link.bytes <= 0xFFFFu;
    const u32 scoped = linked ? scoped_source_offset(link_source, 0) : 0;
    if (linked && presentation->has_hover && presentation->hovered_message_id == message_id &&
        presentation->hovered_kind == MessageMarkdownSemanticKind::link && presentation->hovered_source_offset == scoped)
        base_style |= text_style_underline | text_style_interactive_hover;
    context->markdown_rules = rules;
    const u32 command_before = presentation->command_count;
    const bool appended = append_rich_markdown_line(presentation, context, source, flow, left_inset, right_inset,
                                                    color, owner, message_id, message_revision,
                                                    measurement_index, MessageTextMeasurementKind::embed, 0.0f,
                                                    base_style, base_size);
    context->markdown_rules = 0;
    if (!appended)
        return false;
    if (linked && presentation->command_count == command_before + 1 &&
        presentation->semantic_count < presentation->semantics.count) {
        MessageTextCommand& command = presentation->commands[presentation->command_count - 1];
        // The parsed semantics are the newest entries; extend that run by one.
        const bool contiguous = command.semantics.count == 0 ||
                                command.semantics.data + command.semantics.count ==
                                    presentation->semantics.data + presentation->semantic_count;
        if (contiguous) {
            MessageMarkdownSemantic* first = command.semantics.count
                                                 ? const_cast<MessageMarkdownSemantic*>(command.semantics.data)
                                                 : presentation->semantics.data + presentation->semantic_count;
            presentation->semantics[presentation->semantic_count++] = MessageMarkdownSemantic{
                0, 0, command.text.bytes, scoped, static_cast<u16>(link.bytes), MessageMarkdownSemanticKind::link, 0};
            command.semantics = {first, command.semantics.count + 1};
        }
    }
    return true;
}

bool append_embed_fields(MessageTextPresentation* presentation, BuildContext* context,
                         const MessageRichStore& store, const MessageRichRecord& rich,
                         const MessageRichEmbed& embed, UiTextFlowLayout* flow, const Color& name_color,
                         const Color& value_color, UiWidgetReference owner, u64 message_id,
                         u32 message_revision, u16 measurement_index, float* natural_width,
                         bool markdown) noexcept {
    if (!presentation || !context || !flow) {
        return false;
    }
    const u32 field_end =
        minimum(static_cast<u32>(rich.embed_field_count),
                static_cast<u32>(embed.field_begin) + static_cast<u32>(embed.field_count));
    // Embed fields: margin-top 8 and an 8-DIP grid gap; field names keep a
    // 2-DIP bottom margin.
    constexpr float column_gap = 8.0f;
    constexpr float minimum_column_width = 80.0f;
    const float outer_width = ui_rect_width(flow->content_rectangle);
    const float inset = minimum(kMessageEmbedContentInset, maximum(0.0f, (outer_width - 1.0f) * 0.5f));
    const float end_inset = minimum(kMessageEmbedEndInset, maximum(0.0f, outer_width - 1.0f - inset));
    const float inner_left = flow->content_rectangle.left + inset;
    const float inner_width = maximum(1.0f, outer_width - inset - end_inset);
    for (u32 field = embed.field_begin; field < field_end;) {
        if (!ui_text_flow_advance(flow, kMessageEmbedBlockGap)) {
            presentation->failure_flow_error = flow->last_error;
            return fail(presentation, MessageTextPresentationError::layout);
        }
        u32 columns = 1;
        if (rich.embed_fields[field].inline_field) {
            u32 inline_count = 0;
            while (field + inline_count < field_end && inline_count < 3 &&
                   rich.embed_fields[field + inline_count].inline_field) {
                ++inline_count;
            }
            const u32 fitting_columns = maximum<u32>(
                1u, static_cast<u32>((inner_width + column_gap) / (minimum_column_width + column_gap)));
            columns = minimum(inline_count, fitting_columns);
        }
        const float column_width =
            maximum(1.0f, (inner_width - column_gap * static_cast<float>(columns - 1)) /
                              static_cast<float>(columns));
        float row_height = 0.0f;
        float row_width = kMessageEmbedContentInset + kMessageEmbedEndInset + column_gap * (columns - 1);
        for (u32 column = 0; column < columns; ++column) {
            const u32 first_command = presentation->command_count;
            const float left = inner_left + static_cast<float>(column) * (column_width + column_gap);
            // The retained item initially owns only its estimated height. A
            // field can start below that estimate while the text pass is
            // discovering the real height, so do not bound shaping by it.
            const float content_bottom =
                minimum(kMaximumUiCoordinate, flow->cursor_y + kUiTextFlowItemLayoutHeight);
            const UiRect content{left, flow->cursor_y, left + column_width, content_bottom};
            const UiRect clip = ui_rect_intersection(
                flow->clip_rectangle,
                UiRect{left, flow->clip_rectangle.top, left + column_width, flow->clip_rectangle.bottom});
            UiTextFlowLayout column_flow{};
            if (!ui_rect_has_area(content) || !ui_rect_has_area(clip) ||
                !ui_text_flow_initialize(&column_flow, content, clip)) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            const MessageRichEmbedField& value = rich.embed_fields[field + column];
            if (!append_embed_block(presentation, context, message_rich_store_string(store, value.name),
                                    &column_flow, 0.0f, 0.0f, name_color, owner, message_id, message_revision,
                                    measurement_index, markdown, 1, text_style_bold, text_size_sm) ||
                !ui_text_flow_advance(&column_flow, 2.0f) ||
                !append_embed_block(presentation, context, message_rich_store_string(store, value.value),
                                    &column_flow, 0.0f, 0.0f, value_color, owner, message_id, message_revision,
                                    measurement_index, markdown, 2, 0, text_size_sm)) {
                return false;
            }
            row_height = maximum(row_height, ui_text_flow_required_height(column_flow));
            float measured_width = 0;
            for (u32 c = first_command; c < presentation->command_count; ++c)
                measured_width = maximum(measured_width, presentation->commands[c].measured_width);
            row_width += columns > 1 ? maximum(minimum_column_width, measured_width) : measured_width;
        }
        *natural_width = maximum(*natural_width, row_width);
        if (!ui_text_flow_advance(flow, row_height)) {
            presentation->failure_flow_error = flow->last_error;
            return fail(presentation, MessageTextPresentationError::layout);
        }
        field += columns;
    }
    return true;
}

// The "loading" (deferred interaction) and ephemeral message flags.
constexpr u32 kMessageFlagEphemeral = 1u << 6;
constexpr u32 kMessageFlagLoading = 1u << 7;
// Semantic target selecting the pinned-messages action of a system row.
constexpr u64 kSystemPinsTarget = 1;

Utf8View poll_result_field(const MessageRichStore& store, const MessageRichRecord& rich, Utf8View name) noexcept {
    for (u32 e = 0; e < rich.embed_count; ++e) {
        const auto& embed = rich.embeds[e];
        if (embed.type != GatewayMessageEmbedType::poll_result)
            continue;
        const u32 end = static_cast<u32>(embed.field_begin) + embed.field_count;
        for (u32 f = embed.field_begin; f < end && f < rich.embed_field_count; ++f) {
            const Utf8View key = message_rich_store_string(store, rich.embed_fields[f].name);
            if (key.bytes == name.bytes && bytes_equal(key.data, name.data, name.bytes))
                return message_rich_store_string(store, rich.embed_fields[f].value);
        }
    }
    return {};
}

u32 parse_u32(Utf8View text) noexcept {
    u32 value = 0;
    for (u32 i = 0; i < text.bytes && i < 9; ++i) {
        if (text.data[i] < '0' || text.data[i] > '9')
            break;
        value = value * 10u + static_cast<u32>(text.data[i] - '0');
    }
    return value;
}

// Generates the system line (with a trailing muted time) into the retained
// buffers; names open profiles and references become actions.
bool retain_system_text(MessageTextPresentation* presentation, const ChatStateStore& state, const UserStore* users,
                        const MessageRecord& record, const MessageRichStore* rich_store,
                        const MessageRichRecord* rich, Utf8View content, u64 now_unix_seconds, Utf8View* text,
                        ConstSlice<TextStyleSpan>* styles, ConstSlice<MessageMarkdownSemantic>* semantics) noexcept {
    MessageSystemTextInput input{};
    input.type = record.type;
    input.message_id = record.id;
    input.timestamp_ms = record.timestamp_ms;
    input.content = content;
    const UserRecord* author = users && record.author_id ? user_store_find(*users, record.author_id) : nullptr;
    input.author = member_nickname(*presentation, state, record.channel_id, record.author_id);
    if (!input.author.bytes)
        input.author = author ? user_store_name(*users, *author) : Utf8View{};
    const ChannelRecord* channel = chat_state_store_find_channel(state, record.channel_id);
    if (channel && channel->guild_id) {
        if (const GuildRecord* guild = chat_state_store_find_guild(state, channel->guild_id))
            input.guild = chat_state_store_guild_name(state, *guild);
        // Forum and media channel posts are threads whose parent is a forum.
        const ChannelRecord* parent = channel->parent_id ? chat_state_store_find_channel(state, channel->parent_id)
                                                         : nullptr;
        input.forum_post = parent && (parent->type == 15 || parent->type == 16);
    }
    u64 target_id = 0;
    if (rich && rich_store) {
        target_id = rich->system.target_user_id;
        input.target = message_rich_store_string(*rich_store, rich->system.target_name);
        const Utf8View target_nickname =
            member_nickname(*presentation, state, record.channel_id, target_id);
        if (target_nickname.bytes) {
            input.target = target_nickname;
        } else if (target_id && users) {
            if (const UserRecord* target = user_store_find(*users, target_id)) {
                const Utf8View name = user_store_name(*users, *target);
                if (name.bytes)
                    input.target = name;
            }
        }
        input.same_user = target_id && target_id == record.author_id;
        input.call_present = rich->system.call_present;
        input.call_ended = rich->system.call_ended;
        input.call_participated = rich->system.call_participated;
        input.call_ended_ms = rich->system.call_ended_ms;
        if (record.type == message_type_poll_result) {
            input.poll_question = poll_result_field(*rich_store, *rich, utf8_literal("poll_question_text"));
            input.poll_answer = poll_result_field(*rich_store, *rich, utf8_literal("victor_answer_text"));
            input.poll_votes = parse_u32(poll_result_field(*rich_store, *rich, utf8_literal("victor_answer_votes")));
            input.poll_total_votes = parse_u32(poll_result_field(*rich_store, *rich, utf8_literal("total_votes")));
        }
    }
    const u32 available = presentation->utf8.count - presentation->utf8_count;
    MessageSystemText system{};
    char* const begin = presentation->utf8.data + presentation->utf8_count;
    if (!message_system_text_build(input, {begin, available}, &system))
        return fail(presentation, MessageTextPresentationError::capacity);
    u32 bytes = system.text.bytes;
    char timestamp[kMessageMarkdownTimestampMaximumBytes]{};
    const TimestampFormatResult time =
        record.timestamp_ms ? format_timestamp_calendar(record.timestamp_ms / 1'000u, now_unix_seconds,
                                                        Slice<char>{timestamp, sizeof(timestamp)})
                            : TimestampFormatResult{};
    const u32 time_bytes = time.ok ? time.bytes : 0u;
    if (bytes + (time_bytes ? 2u + time_bytes : 0u) > available ||
        system.span_count + 1u > presentation->styles.count - presentation->style_count ||
        system.span_count > presentation->semantics.count - presentation->semantic_count)
        return fail(presentation, MessageTextPresentationError::capacity);
    const u32 style_begin = presentation->style_count;
    const u32 semantic_begin = presentation->semantic_count;
    for (u32 i = 0; i < system.span_count; ++i) {
        const MessageSystemSpan& span = system.spans[i];
        u64 target = 0;
        MessageMarkdownSemanticKind kind = MessageMarkdownSemanticKind::none;
        u32 flags = text_style_bold;
        switch (span.kind) {
        case MessageSystemSpanKind::author:
            target = record.author_id;
            kind = MessageMarkdownSemanticKind::user_mention;
            break;
        case MessageSystemSpanKind::target:
            target = target_id;
            kind = MessageMarkdownSemanticKind::user_mention;
            break;
        case MessageSystemSpanKind::message:
            target = rich ? rich->reference.message_id : 0;
            kind = MessageMarkdownSemanticKind::message_link;
            flags = text_style_bold | text_style_link;
            break;
        case MessageSystemSpanKind::thread:
            target = rich ? rich->reference.channel_id : 0;
            kind = MessageMarkdownSemanticKind::channel_mention;
            flags = text_style_bold | text_style_link;
            break;
        case MessageSystemSpanKind::pins:
            target = kSystemPinsTarget;
            kind = MessageMarkdownSemanticKind::link;
            flags = text_style_bold | text_style_link;
            break;
        case MessageSystemSpanKind::emphasis:
            break;
        }
        presentation->styles[presentation->style_count++] = TextStyleSpan{span.offset, span.bytes, flags};
        if (target && kind != MessageMarkdownSemanticKind::none)
            presentation->semantics[presentation->semantic_count++] =
                MessageMarkdownSemantic{target, span.offset, span.bytes, 0, 0, kind, 0};
    }
    if (time_bytes) {
        begin[bytes++] = ' ';
        begin[bytes++] = ' ';
        copy_bytes(begin + bytes, timestamp, time_bytes);
        presentation->styles[presentation->style_count++] =
            TextStyleSpan{bytes, time_bytes, text_style_subtext | text_style_timestamp};
        bytes += time_bytes;
    }
    presentation->utf8_count += bytes;
    *text = {begin, bytes};
    *styles = {presentation->styles.data + style_begin, presentation->style_count - style_begin};
    *semantics = {presentation->semantics.data + semantic_begin, presentation->semantic_count - semantic_begin};
    return true;
}

// Label of an obscured or blocked preview: inside the scrim pill, or across a
// blocked notice box.
bool append_obscure_label(MessageTextPresentation* presentation, MessageMediaObscure obscure, UiRect media,
                          UiWidgetReference owner, UiRect clip, const Theme& theme, const MessageRecord& record,
                          u16 measurement) noexcept {
    if (obscure == MessageMediaObscure::none)
        return true;
    const Utf8View label = message_media_obscure_label(obscure);
    const bool notice = message_media_obscure_notice(obscure);
    const UiRect box = notice
                           ? UiRect{media.left + 12.0f, media.top, media.right - 12.0f, media.bottom}
                           : message_media_obscure_pill(media, obscure);
    const UiRect visible = ui_rect_intersection(clip, box);
    if (!ui_rect_has_area(box) || !ui_rect_has_area(visible) || box.right - box.left < 8.0f)
        return true;
    if (presentation->style_count >= presentation->styles.count)
        return fail(presentation, MessageTextPresentationError::capacity);
    auto& style = presentation->styles[presentation->style_count++];
    style = {0, label.bytes, notice ? text_style_none : text_style_bold};
    return append_command(presentation, label, {&style, 1}, {}, owner, box, visible,
                          notice ? ui_background_muted_text_color(theme)
                                                                  : theme.tokens.text.on_accent,
                          record.id, record.revision, measurement, MessageTextMeasurementKind::notice, 0.0f, 0.0f,
                          false, UiTextAlignment::center, UiTextAlignment::center, UiTextWrapping::no_wrap, 0,
                          UiTextOverflow::ellipsis);
}

// Message annotations below the content: the ephemeral footer ("Only
// you can see this · Dismiss message") and the failed-interaction notice.
// They extend the notice item, after any truncation notice. Plugin decoration
// accessories (labels, buttons, links) follow as one wrapping row.
constexpr float kAnnotationLineHeight = 20.0f;
bool append_plugin_accessories(MessageTextPresentation* presentation, const UiPluginRenderDecoration& decoration,
                               const UiRect& region, const UiRect& row_clip, UiWidgetReference owner,
                               const Theme& theme, const MessageRecord& record, u16 measurement_index,
                               float* top) noexcept {
    // Every command measures against the full line width, so its identity (and
    // measured width) does not depend on where the previous node ended.
    const float line_width = maximum(1.0f, ui_rect_width(region));
    const UiRect clip = ui_rect_intersection(row_clip, {region.left, row_clip.top, region.right, row_clip.bottom});
    float x = region.left;
    bool placed = false;
    for (u32 index = 0; index < decoration.accessory_count && index < kUiPluginRenderAccessories; ++index) {
        const UiPluginRenderNode& node = decoration.accessories[index];
        const Utf8View text = node.text.bytes ? node.text : node.tooltip;
        if (!text.bytes)
            continue;
        if (presentation->style_count >= presentation->styles.count ||
            presentation->semantic_count >= presentation->semantics.count)
            return fail(presentation, MessageTextPresentationError::capacity);
        const bool button = node.kind == kUiPluginRenderButton;
        const bool link = node.kind == kUiPluginRenderLink;
        TextStyleSpan* style = presentation->styles.data + presentation->style_count++;
        *style = {0, text.bytes, button ? text_style_bold : link ? text_style_link : text_style_subtext};
        ConstSlice<MessageMarkdownSemantic> semantics{};
        if (button || link) {
            MessageMarkdownSemantic* semantic = presentation->semantics.data + presentation->semantic_count++;
            *semantic = {index + 1u, 0, text.bytes, scoped_source_offset(kUiPluginRenderGeneratedSource, 0), 0,
                         MessageMarkdownSemanticKind::link, 0};
            semantics = {semantic, 1};
        }
        const Color color = button ? ui_icon_role_color(theme, ui_plugin_slot_role(node.tone ? node.tone : 1))
                            : link ? theme.tokens.text.link
                                   : ui_background_muted_text_color(theme);
        const u32 before = presentation->command_count;
        if (!ui_rect_has_area(clip) ||
            !append_command(presentation, text, {style, 1}, semantics, owner,
                            {x, *top, x + line_width, *top + kAnnotationLineHeight}, clip, color, record.id,
                            record.revision, measurement_index, MessageTextMeasurementKind::notice, 0.0f, 0.0f,
                            link, UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap,
                            static_cast<u8>(8u + index), UiTextOverflow::ellipsis, Color{},
                            kUiPluginRenderGeneratedSource))
            return !ui_rect_has_area(clip);
        auto& command = presentation->commands[before];
        // The measured width is known from the previous pass: wrap a node that
        // would cross the line end onto the next line.
        const float width = maximum(command.measured_width, 0.0f);
        if (placed && x > region.left && x + width > region.right) {
            x = region.left;
            *top += kAnnotationLineHeight;
            command.rectangle = {x, *top, x + line_width, *top + kAnnotationLineHeight};
        }
        x += width + 12.0f;
        placed = true;
    }
    if (placed)
        *top += kAnnotationLineHeight;
    return true;
}

bool append_annotations(MessageTextPresentation* presentation, const MessageScene& scene,
                        const MessageSceneRowWidget& row, const MessageRecord& record, const Theme& theme,
                        const UiRect& row_clip, u16 measurement_index, bool forwarded, float offset,
                        const UiPluginRenderDecoration* decoration = nullptr) noexcept {
    if (forwarded)
        return true;
    const bool failed = scene.interaction.failed && scene.interaction.message_id == record.id;
    const bool ephemeral = (record.flags & kMessageFlagEphemeral) != 0;
    const bool accessories = decoration && decoration->accessory_count;
    if (!failed && !ephemeral && !accessories)
        return true;
    const UiLayoutNode* notice = message_scene_layout_node(scene, row.notice);
    if (!notice)
        return fail(presentation, MessageTextPresentationError::layout);
    const UiRect region = notice->content_rectangle;
    float top = region.top + offset;
    const UiWidgetReference owner{&scene.widgets, row.notice};
    if (failed) {
        const UiRect line{region.left, top, maximum(region.left + 1.0f, region.right), top + kAnnotationLineHeight};
        if (ui_rect_has_area(ui_rect_intersection(line, row_clip)) &&
            !append_command(presentation, utf8_literal("This interaction failed"), {}, {}, owner, line, row_clip,
                            theme.tokens.text.danger, record.id, record.revision, kInvalidMessageTextMeasurement,
                            MessageTextMeasurementKind::none, 0.0f, 0.0f, false, UiTextAlignment::start,
                            UiTextAlignment::center, UiTextWrapping::no_wrap, 0, UiTextOverflow::ellipsis))
            return false;
        top += kAnnotationLineHeight;
    }
    if (ephemeral) {
        // Two measured commands: the muted notice, then the dismiss link placed
        // after its shaped width ("Only you can see this · Dismiss message").
        const auto notice_text = utf8_literal("Only you can see this \xC2\xB7 ");
        const auto dismiss_text = utf8_literal("Dismiss message");
        if (presentation->style_count + 2 > presentation->styles.count ||
            presentation->semantic_count >= presentation->semantics.count)
            return fail(presentation, MessageTextPresentationError::capacity);
        const UiRect line{region.left, top, maximum(region.left + 1.0f, region.right), top + kAnnotationLineHeight};
        if (ui_rect_has_area(ui_rect_intersection(line, row_clip))) {
            // The footer is set in the small muted type.
            TextStyleSpan* small = presentation->styles.data + presentation->style_count++;
            *small = {0, notice_text.bytes, text_style_subtext};
            if (!append_command(presentation, notice_text, {small, 1}, {}, owner, line, row_clip,
                                ui_background_muted_text_color(theme), record.id, record.revision,
                                measurement_index, MessageTextMeasurementKind::notice, 0.0f, 0.0f, false,
                                UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap, 0,
                                UiTextOverflow::ellipsis))
                return false;
            const float notice_width = presentation->commands[presentation->command_count - 1].measured_width;
            if (notice_width > 0.0f && line.left + notice_width + 8.0f < line.right) {
                TextStyleSpan* link = presentation->styles.data + presentation->style_count++;
                *link = {0, dismiss_text.bytes, text_style_link | text_style_subtext};
                MessageMarkdownSemantic* semantic = presentation->semantics.data + presentation->semantic_count++;
                // The dismiss action is a message link to the ephemeral message itself.
                *semantic = {record.id, 0, dismiss_text.bytes, 0, 0, MessageMarkdownSemanticKind::message_link, 0};
                const UiRect dismiss{line.left + notice_width, line.top, line.right, line.bottom};
                if (!append_command(presentation, dismiss_text, {link, 1}, {semantic, 1}, owner, dismiss, row_clip,
                                    theme.tokens.text.link, record.id, record.revision, measurement_index,
                                    MessageTextMeasurementKind::notice, 0.0f, 0.0f, true, UiTextAlignment::start,
                                    UiTextAlignment::center, UiTextWrapping::no_wrap, 1, UiTextOverflow::ellipsis))
                    return false;
            }
        }
        top += kAnnotationLineHeight;
    }
    if (accessories && !append_plugin_accessories(presentation, *decoration, region, row_clip, owner, theme, record,
                                                  measurement_index, &top))
        return false;
    auto& measured = presentation->measurements[measurement_index];
    measured.rich_item_layout.heights[kMessageSceneNoticeItemOffset] = top - region.top;
    measured.rich_layout.notice = measured.rich_item_layout.heights[kMessageSceneNoticeItemOffset];
    return true;
}

u32 format_count(u32 value, char* output) noexcept {
    char digits[16];
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value);
    u32 bytes = 0;
    for (u32 i = 0; i < count; ++i) {
        if (i && (count - i) % 3u == 0)
            output[bytes++] = ',';
        output[bytes++] = digits[count - 1u - i];
    }
    return bytes;
}

bool retain_text(MessageTextPresentation* presentation, Utf8View text, Utf8View* output) noexcept {
    if (text.bytes > presentation->utf8.count - presentation->utf8_count)
        return fail(presentation, MessageTextPresentationError::capacity);
    char* at = presentation->utf8.data + presentation->utf8_count;
    copy_bytes(at, text.data, text.bytes);
    presentation->utf8_count += text.bytes;
    *output = {at, text.bytes};
    return true;
}

bool append_card_line(MessageTextPresentation* presentation, Utf8View text, u32 style, UiWidgetReference owner,
                      UiRect rectangle, const UiRect& clip, Color color, const MessageRecord& record, u8 slot,
                      u16 measurement, UiTextAlignment alignment = UiTextAlignment::start) noexcept {
    const UiRect visible = ui_rect_intersection(clip, rectangle);
    if (!text.bytes || !ui_rect_has_area(rectangle) || !ui_rect_has_area(visible))
        return true;
    ConstSlice<TextStyleSpan> styles{};
    if (style) {
        if (presentation->style_count >= presentation->styles.count)
            return fail(presentation, MessageTextPresentationError::capacity);
        auto& span = presentation->styles[presentation->style_count++];
        span = {0, text.bytes, style};
        styles = {&span, 1};
    }
    // Measured (notice kind: no height contribution) so centred labels align.
    return append_command(presentation, text, styles, {}, owner, rectangle, visible, color, record.id,
                          record.revision, measurement, MessageTextMeasurementKind::notice, 0.0f, 0.0f, false,
                          alignment, UiTextAlignment::center, UiTextWrapping::no_wrap, slot,
                          UiTextOverflow::ellipsis);
}

// Invite cards: header, server name, counts and the Join action, from the
// session invite cache (loading and invalid states included).
// Event link cards share the invite card frame: when, name, place and the
// interested count from the scheduled events store, and a View action.
bool append_event_card(MessageTextPresentation* presentation, const MessageScene& scene, const InviteLink& link,
                       const MessageInviteCardGeometry& geometry, UiWidgetReference owner,
                       const MessageRecord& record, const ChatStateStore& state, const Theme& theme,
                       const UiRect& row_clip, u8 ordinal, u16 measurement_index, u64 now_unix_seconds) noexcept {
    const ScheduledEvent* event =
        scene.scheduled_events ? scheduled_event_store_find(*scene.scheduled_events, link.event_id) : nullptr;
    const bool member = chat_state_store_find_guild(state, link.event_guild_id) != nullptr;
    char header_storage[96];
    Utf8View header = utf8_literal("Event");
    if (event && event->status == ScheduledEventStatus::active) {
        header = utf8_literal("Live now");
    } else if (event) {
        const auto formatted = format_timestamp_local(event->start_ms / 1'000u, 'f', now_unix_seconds,
                                                      {header_storage, sizeof(header_storage)});
        if (formatted.ok && !retain_text(presentation, {header_storage, formatted.bytes}, &header))
            return false;
    }
    const Utf8View name = event ? (event->name_bytes ? Utf8View{event->name, event->name_bytes}
                                                     : utf8_literal("Untitled event"))
                          : member ? utf8_literal("Loading event…")
                                   : utf8_literal("Join the server to see this event.");
    char detail_storage[192];
    u32 detail_bytes = 0;
    const auto add = [&](Utf8View text) noexcept {
        const u32 take = minimum<u32>(text.bytes, sizeof(detail_storage) - detail_bytes);
        copy_bytes(detail_storage + detail_bytes, text.data, take);
        detail_bytes += take;
    };
    if (event) {
        if (event->entity == ScheduledEventEntity::external)
            add({event->location, event->location_bytes});
        else if (const auto* channel = chat_state_store_find_channel(state, event->channel_id))
            add(chat_state_store_channel_name(state, *channel));
        if (event->user_count_known) {
            if (detail_bytes)
                add(utf8_literal(" \xC2\xB7 "));
            char count[16];
            add({count, format_count(event->user_count, count)});
            add(utf8_literal(" interested"));
        }
    }
    Utf8View detail{};
    if (detail_bytes && !retain_text(presentation, {detail_storage, detail_bytes}, &detail))
        return false;
    return append_card_line(presentation, header, text_style_bold, owner, geometry.header, row_clip,
                            event && event->status == ScheduledEventStatus::active
                                ? theme.tokens.text.positive
                                : ui_background_muted_text_color(theme),
                            record, ordinal, measurement_index) &&
           append_card_line(presentation, name, event ? text_style_bold : 0, owner, geometry.name, row_clip,
                            theme.text_primary, record, ordinal, measurement_index) &&
           append_card_line(presentation, detail, 0, owner, geometry.detail, row_clip,
                            ui_background_muted_text_color(theme), record, ordinal, measurement_index) &&
           (!event || append_card_line(presentation, event->interested ? utf8_literal("Interested")
                                                                       : utf8_literal("View"),
                                       text_style_bold, owner, geometry.button, row_clip,
                                       event->interested ? theme.text_primary : theme.tokens.text.on_accent, record,
                                       ordinal, measurement_index, UiTextAlignment::center));
}

bool append_invite_cards(MessageTextPresentation* presentation, const MessageScene& scene,
                         const MessageSceneRowWidget& row, const MessageRecord& record, const ChatStateStore& state,
                         const Theme& theme, const UiRect& row_clip, u16 measurement_index,
                         u64 now_unix_seconds) noexcept {
    MessageTextMeasurement* measurement = &presentation->measurements[measurement_index];
    InviteLink links[kMessageInviteCardMaximum];
    const u32 count = minimum<u32>(row.invite_item_count,
                                   message_scene_invite_codes(scene, record, {links, kMessageInviteCardMaximum}));
    float total = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 slot = kMessageRichInviteItemOffset + i;
        measurement->rich_item_layout.heights[slot] = kMessageInviteCardHeight;
        total += kMessageInviteCardHeight + (i ? kMessageInviteCardGap : 0.0f);
        const UiLayoutNode* node = message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::invite, i);
        if (!node)
            return fail(presentation, MessageTextPresentationError::layout);
        const auto geometry = message_invite_card_geometry(node->rectangle);
        if (!ui_rect_has_area(ui_rect_intersection(geometry.card, row_clip)))
            continue;
        const UiWidgetReference owner{&scene.widgets, message_scene_rich_item_widget(row, MessageSceneRichItemKind::invite, i)};
        if (invite_link_event(links[i])) {
            if (!append_event_card(presentation, scene, links[i], geometry, owner, record, state, theme, row_clip,
                                   static_cast<u8>(i), measurement_index, now_unix_seconds))
                return false;
            continue;
        }
        const Utf8View code{links[i].code, links[i].code_bytes};
        const InviteCardEntry* entry = scene.invite_cards ? invite_card_cache_find(*scene.invite_cards, code) : nullptr;
        const bool known = entry && entry->invite.guild_id;
        const bool invalid = entry && entry->state == InviteCardState::invalid;
        Utf8View name = known ? Utf8View{entry->invite.name, entry->invite.name_bytes}
                        : invalid ? utf8_literal("Invalid invite")
                                  : utf8_literal("Loading invite\xE2\x80\xA6");
        if (known && !name.bytes)
            name = utf8_literal("Unnamed server");
        char detail_storage[96];
        u32 detail_bytes = 0;
        if (known && entry->invite.counts_known) {
            detail_bytes += format_count(entry->invite.presence_count, detail_storage);
            const auto online = utf8_literal(" online \xC2\xB7 ");
            copy_bytes(detail_storage + detail_bytes, online.data, online.bytes);
            detail_bytes += online.bytes;
            detail_bytes += format_count(entry->invite.member_count, detail_storage + detail_bytes);
            const auto members = utf8_literal(" members");
            copy_bytes(detail_storage + detail_bytes, members.data, members.bytes);
            detail_bytes += members.bytes;
        }
        Utf8View detail{};
        if (detail_bytes && !retain_text(presentation, {detail_storage, detail_bytes}, &detail))
            return false;
        if (invalid)
            detail = utf8_literal("This invite may have expired.");
        const bool joined = known && (entry->joined || chat_state_store_find_guild(state, entry->invite.guild_id));
        const u8 ordinal = static_cast<u8>(i);
        if (!append_card_line(presentation, invalid ? utf8_literal("You received an invite, but\xE2\x80\xA6")
                                                    : utf8_literal("You've been invited to join a server"),
                              text_style_bold, owner, geometry.header, row_clip,
                              ui_background_muted_text_color(theme), record, ordinal, measurement_index) ||
            !append_card_line(presentation, name, known ? text_style_bold : 0, owner, geometry.name, row_clip,
                              invalid ? theme.tokens.text.danger : theme.text_primary, record, ordinal,
                              measurement_index) ||
            !append_card_line(presentation, detail, 0, owner, geometry.detail, row_clip,
                              ui_background_muted_text_color(theme), record, ordinal, measurement_index) ||
            (known && !append_card_line(presentation, joined ? utf8_literal("Joined") : utf8_literal("Join"),
                                        text_style_bold, owner, geometry.button, row_clip,
                                        joined ? theme.text_primary : theme.tokens.text.on_accent, record, ordinal,
                                        measurement_index, UiTextAlignment::center)))
            return false;
        // A server without an icon shows its initials in the tile.
        if (known && !entry->invite.icon_bytes) {
            char initials_storage[16];
            const Utf8View initials = ui_admin_initials(name, {initials_storage, sizeof(initials_storage)});
            Utf8View retained{};
            if (initials.bytes &&
                (!retain_text(presentation, initials, &retained) ||
                 !append_card_line(presentation, retained, text_style_bold, owner, geometry.icon, row_clip,
                                   theme.text_primary, record, ordinal, measurement_index, UiTextAlignment::center)))
                return false;
        }
    }
    measurement->rich_layout.invites = total;
    return true;
}

// Collapsed plugin rows: "<label> · Show" after any compact name prefix. The
// Show link carries a plugin semantic the host routes to a session reveal.
bool append_plugin_collapsed(MessageTextPresentation* presentation, const UiPluginRenderDecoration& decoration,
                             Utf8View* text, ConstSlice<TextStyleSpan>* styles,
                             ConstSlice<MessageMarkdownSemantic>* semantics) noexcept {
    const Utf8View label = ui_plugin_render_collapsed_label(decoration);
    const auto separator = utf8_literal(" \xC2\xB7 ");
    const auto show = utf8_literal("Show");
    const u32 lead = text->bytes ? 1u : 0u;
    const u32 bytes = lead + label.bytes + separator.bytes + show.bytes;
    if (presentation->utf8_count > presentation->utf8.count ||
        bytes > presentation->utf8.count - presentation->utf8_count ||
        2u > presentation->styles.count - presentation->style_count ||
        presentation->semantic_count >= presentation->semantics.count)
        return fail(presentation, MessageTextPresentationError::capacity);
    // A compact name prefix was retained just before: extend it in place.
    const u32 text_begin = text->bytes ? static_cast<u32>(text->data - presentation->utf8.data) : presentation->utf8_count;
    const u32 style_begin = styles->count ? static_cast<u32>(styles->data - presentation->styles.data) : presentation->style_count;
    const u32 semantic_begin =
        semantics->count ? static_cast<u32>(semantics->data - presentation->semantics.data) : presentation->semantic_count;
    if (text_begin + text->bytes != presentation->utf8_count || style_begin + styles->count != presentation->style_count ||
        semantic_begin + semantics->count != presentation->semantic_count)
        return fail(presentation, MessageTextPresentationError::model);
    char* out = presentation->utf8.data + presentation->utf8_count;
    const u32 base = presentation->utf8_count - text_begin;
    u32 at = 0;
    if (lead)
        out[at++] = ' ';
    copy_bytes(out + at, label.data, label.bytes);
    copy_bytes(out + at + label.bytes, separator.data, separator.bytes);
    copy_bytes(out + at + label.bytes + separator.bytes, show.data, show.bytes);
    const u32 label_offset = base + at;
    const u32 show_offset = label_offset + label.bytes + separator.bytes;
    presentation->utf8_count += bytes;
    presentation->styles[presentation->style_count++] = {label_offset, label.bytes + separator.bytes, text_style_italic};
    presentation->styles[presentation->style_count++] = {show_offset, show.bytes, text_style_link};
    presentation->semantics[presentation->semantic_count++] = {
        kUiPluginRenderShowTarget, show_offset, show.bytes, scoped_source_offset(kUiPluginRenderGeneratedSource, 0),
        0, MessageMarkdownSemanticKind::link, 0};
    *text = {presentation->utf8.data + text_begin, presentation->utf8_count - text_begin};
    *styles = {presentation->styles.data + style_begin, presentation->style_count - style_begin};
    *semantics = {presentation->semantics.data + semantic_begin, presentation->semantic_count - semantic_begin};
    return true;
}

// Channel-beginning header text: bold 2rem title, optional DM username, and
// the description with its bold name runs (scene-owned text and spans).
bool append_intro(MessageTextPresentation* presentation, const MessageScene& scene, const Theme& theme) noexcept {
    MessageSceneIntroGeometry geometry{};
    if (scene.content_gated || !message_scene_intro_geometry(scene, &geometry))
        return true;
    const UiRect clip = ui_rect_intersection(scene.viewport, geometry.area);
    if (!ui_rect_has_area(clip))
        return true;
    const UiWidgetReference owner{&scene.widgets, scene.root};
    const MessageSceneIntro& intro = scene.intro;
    const struct {
        Utf8View text;
        ConstSlice<TextStyleSpan> styles;
        UiRect rectangle;
    } parts[]{
        {message_scene_intro_header(intro), {&scene.intro_header_style, 1}, geometry.header},
        {message_scene_intro_subtitle(intro), {&scene.intro_subtitle_style, 1}, geometry.subtitle},
        {message_scene_intro_description(intro), {intro.description_spans, intro.description_span_count},
         geometry.description},
    };
    for (const auto& part : parts) {
        if (!part.text.bytes || !ui_rect_has_area(part.rectangle) ||
            !ui_rect_has_area(ui_rect_intersection(clip, part.rectangle)))
            continue;
        if (!append_command(presentation, part.text, part.styles, {}, owner, part.rectangle, clip,
                            theme.text_primary, 0, 0, kInvalidMessageTextMeasurement,
                            MessageTextMeasurementKind::none, 0, 0, false))
            return false;
    }
    return true;
}

bool append_row(MessageTextPresentation* presentation, const MessageScene& scene,
                const MessageSceneRowWidget& row, const ChatStateStore& state,
                const UserStore* users, const NavigationView& navigation, const Theme& theme,
                u64 now_unix_seconds, bool forwarded = false) noexcept {
    if (row.redacted || scene.content_gated)
        return true;
    const MessageStore* const store = scene.timeline ? scene.timeline->store : nullptr;
    const MessageRecord* record = store ? message_store_resolve(*store, row.message) : nullptr;
    if (!record || record->id != row.message_id) {
        return fail(presentation, MessageTextPresentationError::model);
    }
    presentation->failure_message_id = record->id;
    presentation->failure_source_index = row.source_index;
    set_failure_context(presentation, MessageTextPresentationStage::row,
                        MessageTextPresentationOperation::resolve_layout);
    if (presentation->measurement_count >= presentation->measurements.count) {
        return fail(presentation, MessageTextPresentationError::capacity);
    }
    const UiLayoutNode* row_layout = message_scene_layout_node(scene, row.widget);
    const UiLayoutNode* reply = message_scene_layout_node(scene, row.reply);
    const UiLayoutNode* author = message_scene_layout_node(scene, row.author);
    const UiLayoutNode* content_layout = message_scene_layout_node(scene, row.content);
    const UiLayoutNode* attachments = message_scene_layout_node(scene, row.attachments);
    const UiLayoutNode* embeds = message_scene_layout_node(scene, row.embeds);
    const UiLayoutNode* stickers = message_scene_layout_node(scene, row.stickers);
    const UiLayoutNode* components = message_scene_layout_node(scene, row.components);
    const UiLayoutNode* reactions = message_scene_layout_node(scene, row.reactions);
    const UiLayoutNode* notice = message_scene_layout_node(scene, row.notice);
    if (!row_layout || !reply || (!forwarded && !author) || !content_layout || !attachments || !embeds ||
        !stickers || !components || !reactions || !notice) {
        return fail(presentation, MessageTextPresentationError::layout);
    }

    const UiRect visible_row_clip = ui_rect_intersection(scene.viewport, row_layout->rectangle);
    // Overscan rows are measured before they cross the viewport edge, so keep a
    // positive offscreen clip for shaping and measurement. The renderer rejects
    // commands whose clip misses the message viewport: they never become paint.
    const UiRect row_clip = ui_rect_has_area(visible_row_clip) ? visible_row_clip : row_layout->rectangle;
    const float text_left = content_layout->content_rectangle.left;
    const float text_width = ui_rect_width(content_layout->content_rectangle);
    // Transient zero-area rows are valid retained states. They must not poison
    // every other message by failing the atomic build.
    if (!ui_rect_has_area(row_clip) || text_width <= 0.0f) {
        return true;
    }
    const auto divider = message_scene_unread_divider_rectangle(scene, row);
    if (ui_rect_has_area(divider)) {
        const auto geometry = ui_unread_divider_geometry(divider, scene.unread_divider_style);
        if (!append_command(presentation, kUiUnreadDividerLabel, {}, {},
                            UiWidgetReference{&scene.widgets, row.widget}, geometry.label, row_clip,
                            ui_unread_divider_text_color(theme), 0, 0, kInvalidMessageTextMeasurement,
                            MessageTextMeasurementKind::none, 0, 0, false, UiTextAlignment::end,
                            UiTextAlignment::center, UiTextWrapping::no_wrap, 0, UiTextOverflow::ellipsis))
            return false;
    }

    // Plugin decoration verdict cached for this revision (no plugin code runs).
    UiPluginRenderDecoration decoration{};
    const bool decorated =
        !forwarded && message_scene_plugin_decoration(scene, record->id, record->revision, &decoration);
    const bool collapsed = decorated && decoration.visibility == ui_plugin_render_collapsed;
    const bool replaced = decorated && !collapsed && decoration.replace_content &&
                          record->content_availability == MessageContentAvailability::available;
    Utf8View content{};
    Color content_color = theme.text_primary;
    if (collapsed) {
        content_color = ui_background_muted_text_color(theme);
    } else if (replaced) {
        // Display-only markdown; the stored message text is untouched.
        content = decoration.content;
    } else if (record->content_availability == MessageContentAvailability::available) {
        content = message_store_content(*store, row.message);
    } else if (record->content_availability == MessageContentAvailability::restricted) {
        content = utf8_literal("Message content is unavailable");
        content_color = ui_background_muted_text_color(theme);
    } else {
        content = utf8_literal("Loading…");
        content_color = ui_background_muted_text_color(theme);
    }
    if (record->delivery == MessageDeliveryState::pending) {
        content_color = ui_background_muted_text_color(theme);
    } else if (record->delivery == MessageDeliveryState::failed) {
        content_color = theme.notification;
    }
    const u16 measurement_index = static_cast<u16>(presentation->measurement_count++);
    presentation->measurements[measurement_index] = MessageTextMeasurement{};
    MessageTextMeasurement& row_measurement = presentation->measurements[measurement_index];
    row_measurement.message_id = record->id;
    row_measurement.message_revision = record->revision;
    row_measurement.source_index = row.source_index;
    row_measurement.forwarded = forwarded;
    row_measurement.rich_layout = row.rich_layout;
    row_measurement.rich_item_layout = row.rich_item_layout;
    row_measurement.content_present = content.bytes != 0;
    for (u32 index = 0; index < kGatewayMessageReactionMaximum; ++index) {
        row_measurement.reaction_widths[index] = row.reaction_widths[index];
    }

    const MessageRichStore* rich_store = scene.timeline->rich_store;
    const MessageRichRecord* rich = rich_store
                                        ? (forwarded ? message_rich_store_snapshot(*rich_store, record->id)
                                                     : message_rich_store_find(*rich_store, record->id))
                                        : nullptr;
    if (rich && rich->message_revision != record->revision) {
        rich = nullptr;
    }
    if (collapsed)
        rich = nullptr; // the scene built no rich widgets for this row
    if (forwarded) {
        if (!rich)
            return fail(presentation, MessageTextPresentationError::model);
        content = message_rich_store_string(*rich_store, rich->snapshot.content);
        content_color = theme.text_primary;
        row_measurement.content_present = content.bytes != 0;
        UiRect label_rectangle = reply->content_rectangle;
        label_rectangle.left = minimum(label_rectangle.right - 1.0f, label_rectangle.left + 22.0f);
        if (!append_command(presentation, utf8_literal("Forwarded"), {}, {},
                            UiWidgetReference{&scene.widgets, row.reply}, label_rectangle, row_clip,
                            theme.text_secondary, record->id, record->revision,
                            kInvalidMessageTextMeasurement, MessageTextMeasurementKind::none, 0.0f, 0.0f,
                            false, UiTextAlignment::start, UiTextAlignment::center,
                            UiTextWrapping::no_wrap))
            return false;
    }
    if (rich && !replaced && record->content_availability == MessageContentAvailability::available &&
        message_rich_embed_suppresses_content(*rich_store, *rich, content)) {
        content = Utf8View{};
        row_measurement.content_present = false;
    }
    if (!forwarded && rich && message_rich_has_header(*rich)) {
        set_failure_context(presentation, MessageTextPresentationStage::reply,
                            MessageTextPresentationOperation::initialize_flow);
        const u32 reply_begin = presentation->command_count;
        UiTextFlowLayout flow{};
        if (!initialize_flow(presentation, *reply, text_left, text_width, row_clip, &flow)) {
            return false;
        }
        Utf8View label{};
        const bool interaction_header = rich->reference.state == GatewayMessageReferenceState::none;
        if (interaction_header) {
            label = message_rich_store_string(*rich_store, rich->interaction.user_name);
            if (label.bytes == 0)
                label = utf8_literal("Unknown user");
        } else if (rich->reference.state == GatewayMessageReferenceState::available) {
            label = member_nickname(*presentation, state, record->channel_id, rich->reference.author_id);
            if (label.bytes == 0)
                label = message_rich_store_string(*rich_store, rich->reference.author_name);
            if (label.bytes == 0 && users) {
                const auto* user = user_store_find(*users, rich->reference.author_id);
                if (user)
                    label = user_store_name(*users, *user);
            }
            if (label.bytes == 0) {
                label = utf8_literal("Reply");
            }
        } else if (rich->reference.state == GatewayMessageReferenceState::deleted) {
            label = utf8_literal("Original message was deleted");
        } else {
            label = utf8_literal("Message could not be loaded");
        }
        const bool reference_available = rich->reference.state == GatewayMessageReferenceState::available;
        const Utf8View reference_content = message_rich_store_string(*rich_store, rich->reference.content);
        // A resident poll keeps its question in the poll store, not the quote.
        Utf8View poll_question{};
        if (reference_available && !interaction_header && reference_content.bytes == 0 && scene.polls &&
            scene.polls->store) {
            if (const auto* poll = message_poll_store_find(*scene.polls->store, rich->reference.message_id))
                poll_question = message_poll_store_string(*scene.polls->store, poll->question);
        }
        BuildContext reply_context{presentation, &state, users, &navigation, record, now_unix_seconds};
        const RetainedReplySummary summary =
            interaction_header
                ? retain_interaction_header(
                      presentation, label,
                      message_rich_store_string(*rich_store, rich->interaction.command_name))
            : reference_available
                ? retain_reply_summary(presentation, &reply_context, label, reference_content, true,
                                       rich->reference.content_flags, rich->reference.message_type, poll_question)
                : retain_reply_placeholder(presentation, label);
        const MessageReplyIcon reply_icon =
            reference_available && !interaction_header
                ? message_rich_reply_icon(rich->reference.content_flags, rich->reference.message_type,
                                          reference_content.bytes == 0,
                                          poll_question.bytes != 0 ||
                                              (rich->reference.content_flags & gateway_reference_poll_question))
                : MessageReplyIcon::none;
        const UiWidgetReference owner{&scene.widgets, row.reply};
        // Role colours are server data and pass through every theme unchanged.
        const Color reply_author_color = member_role_color(*presentation, state, record->channel_id,
                                                           message_rich_header_user(*rich), theme.text_primary);
        if (summary.text.bytes == 0 ||
            !append_reply_line(presentation, summary.text, summary.styles, &flow, theme.text_primary,
                               reply_author_color, owner, record->id, record->revision, measurement_index,
                               UiTextOverflow::ellipsis,
                               reply_icon != MessageReplyIcon::none ? kMessageReplyIconGap + kMessageReplyIcon
                                                                    : 0.0f)) {
            return false;
        }
        if (presentation->command_count == reply_begin) {
            return fail(presentation, MessageTextPresentationError::model);
        }
        {
            const MessageTextCommand& line = presentation->commands[presentation->command_count - 1];
            presentation->measurements[measurement_index].rich_item_layout.reply_width =
                reply_icon != MessageReplyIcon::none
                    ? minimum(line.measured_width, ui_rect_width(line.rectangle))
                    : 0.0f;
        }
        if (!finish_flow(presentation, flow, &presentation->measurements[measurement_index].reply_height)) {
            return false;
        }
        saturating_accumulate(presentation->rich.replies);
    }

    // Compact rows: timestamp in the hanging-indent gutter, name inline.
    Utf8View compact_name{};
    Color compact_color{};
    if (!forwarded && !row.grouped && users && record->author_id != 0) {
        set_failure_context(presentation, MessageTextPresentationStage::author,
                            MessageTextPresentationOperation::append_command);
        const UserRecord* user = user_store_find(*users, record->author_id);
        Utf8View name = member_nickname(*presentation, state, record->channel_id, record->author_id);
        if (name.bytes == 0 && user)
            name = user_store_name(*users, *user);
        if (name.bytes == 0 && record->delivery != MessageDeliveryState::authoritative) {
            name = utf8_literal("You");
        }
        Color author_color = record->delivery == MessageDeliveryState::failed ? theme.notification
                             : record->delivery == MessageDeliveryState::pending
                                 ? ui_background_muted_text_color(theme)
                                 : theme.text_primary;
        author_color = ui_foreground_color(theme, author_color);
        // Role colours are server data and pass through every theme unchanged.
        if (record->delivery == MessageDeliveryState::authoritative) {
            author_color = member_role_color(*presentation, state, record->channel_id, record->author_id,
                                             author_color);
        }
        if (scene.display.compact) {
            compact_name = name;
            compact_color = author_color;
            if (!append_compact_timestamp(presentation, scene, row, row_clip, theme, *record, now_unix_seconds))
                return false;
        }
        const bool result_card_header = scene.result_cards && !scene.display.compact && name.bytes != 0;
        if (result_card_header && !append_result_card_header(presentation, scene, row, author->content_rectangle,
                                                             row_clip, author_color, *record, name,
                                                             now_unix_seconds))
            return false;
        const RetainedAuthorHeader header = scene.display.compact || result_card_header
                                                ? RetainedAuthorHeader{}
                                                : retain_author_header(presentation, name, record->timestamp_ms,
                                                                       now_unix_seconds);
        if (name.bytes != 0 && !scene.display.compact && !result_card_header &&
            (header.text.bytes == 0 ||
             !append_command(presentation, header.text, header.styles,
                             ConstSlice<MessageMarkdownSemantic>{},
                             UiWidgetReference{&scene.widgets, row.author}, author->content_rectangle,
                             row_clip, author_color, record->id, record->revision,
                             kInvalidMessageTextMeasurement, MessageTextMeasurementKind::none, 0.0f, 0.0f,
                             header.styles.count != 0, UiTextAlignment::start, UiTextAlignment::start,
                             UiTextWrapping::no_wrap, 0, UiTextOverflow::ellipsis))) {
            return false;
        }
    }

    Utf8View display_content = content;
    ConstSlice<TextStyleSpan> styles{};
    ConstSlice<MessageMarkdownSemantic> semantics{};
    set_failure_context(presentation, MessageTextPresentationStage::content,
                        MessageTextPresentationOperation::append_command);
    BuildContext context{presentation, &state, users, &navigation, record, now_unix_seconds};
    context.rich_store = rich_store;
    context.rich = rich;
    context.forwarded = forwarded;
    context.source_id = forwarded  ? kRichTextSource | kForwardedTextSource | kRichTextBody
                        : replaced  ? kUiPluginRenderContentSource
                        : collapsed ? kUiPluginRenderGeneratedSource
                                    : 0;
    const bool parse = (forwarded || record->content_availability == MessageContentAvailability::available) &&
                       content.bytes != 0;
    const bool system_row = !forwarded && !replaced && !collapsed && message_type_is_system(record->type) &&
                            record->content_availability == MessageContentAvailability::available;
    const bool thinking =
        !forwarded && !collapsed && (record->flags & kMessageFlagLoading) != 0 && content.bytes == 0;
    if (system_row) {
        if (!retain_system_text(presentation, state, users, *record, rich_store, rich, content, now_unix_seconds,
                                &display_content, &styles, &semantics))
            return false;
        content_color = ui_background_muted_text_color(theme);
        row_measurement.content_present = true;
    } else if (thinking) {
        // Deferred interaction response: the app is shown "thinking".
        const UserRecord* app_user = users ? user_store_find(*users, record->author_id) : nullptr;
        const Utf8View name = app_user ? user_store_name(*users, *app_user) : utf8_literal("The app");
        const auto suffix = utf8_literal(" is thinking\xE2\x80\xA6");
        if (name.bytes + suffix.bytes > presentation->utf8.count - presentation->utf8_count ||
            presentation->style_count >= presentation->styles.count)
            return fail(presentation, MessageTextPresentationError::capacity);
        char* retained = presentation->utf8.data + presentation->utf8_count;
        copy_bytes(retained, name.data, name.bytes);
        copy_bytes(retained + name.bytes, suffix.data, suffix.bytes);
        presentation->utf8_count += name.bytes + suffix.bytes;
        display_content = {retained, name.bytes + suffix.bytes};
        auto& italic = presentation->styles[presentation->style_count++];
        italic = {0, display_content.bytes, text_style_italic};
        styles = {&italic, 1};
        content_color = ui_background_muted_text_color(theme);
        row_measurement.content_present = true;
    } else if (compact_name.bytes) {
        if (!compact_content(presentation, &context, compact_name, parse ? content : Utf8View{}, &display_content,
                             &styles, &semantics, decorated ? &decoration : nullptr))
            return false;
    } else if (parse && !parse_content(presentation, &context, content, &display_content, &styles, &semantics)) {
        return false;
    }
    if (collapsed) {
        if (!append_plugin_collapsed(presentation, decoration, &display_content, &styles, &semantics))
            return false;
        row_measurement.content_present = true;
    }
    QuoteSegment segments[kQuoteSegmentMaximum];
    // Compact rows keep the name inline with the first line of the body.
    u32 segment_count = parse && !system_row && !thinking
                            ? split_quote_segments(display_content, styles, semantics, segments)
                            : 0u;
    if (segment_count == 0) {
        segments[0] = QuoteSegment{display_content, styles, semantics, false};
        segment_count = 1;
    }
    float content_top = content_layout->content_rectangle.top;
    for (u32 index = 0; index < segment_count; ++index) {
        const QuoteSegment& segment = segments[index];
        if (segment.text.bytes == 0)
            continue;
        const float inset = segment.quote ? quote_text_inset() : 0.0f;
        const UiRect content_rectangle = line_rectangle(
            text_left + inset, content_top, segment.quote ? quote_text_width(text_width) : text_width);
        if (!append_command(presentation, segment.text, segment.styles, segment.semantics,
                            UiWidgetReference{&scene.widgets, row.content}, content_rectangle, row_clip,
                            content_color, record->id, record->revision, measurement_index,
                            MessageTextMeasurementKind::content, 0.0f,
                            quote_gap_after(segments, segment_count, index, 0.0f), segment.styles.count != 0,
                            UiTextAlignment::start, UiTextAlignment::start, UiTextWrapping::wrap, 0,
                            UiTextOverflow::clip, compact_name.bytes ? compact_color : Color{},
                            context.source_id)) {
            return false;
        }
        const MessageTextCommand& command = presentation->commands[presentation->command_count - 1];
        content_top += command.measured_height + command.gap_after;
    }

    if (!forwarded && row.poll_unavailable) {
        const auto* node = message_scene_layout_node(scene, row.poll);
        if (!node ||
            !append_command(
                presentation, ui_theme_poll_style(scene.theme_styles).unavailable, {}, {},
                {&scene.widgets, row.poll},
                {node->rectangle.left + ui_theme_poll_style(scene.theme_styles).padding,
                 node->rectangle.top + ui_theme_poll_style(scene.theme_styles).padding,
                 minimum(node->rectangle.right,
                         node->rectangle.left + ui_theme_poll_style(scene.theme_styles).maximum_width) -
                     ui_theme_poll_style(scene.theme_styles).padding,
                 node->rectangle.top + row.poll_height - kDefaultMessageRichBlockGapDips -
                     ui_theme_poll_style(scene.theme_styles).padding},
                row_clip, theme.text_secondary, record->id, record->revision,
                kInvalidMessageTextMeasurement, MessageTextMeasurementKind::none, 0, 0, false,
                UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::wrap))
            return fail(presentation, MessageTextPresentationError::layout);
    } else if (!forwarded && row.poll_content_revision) {
        UiPollAnswer answers[kUiPollMaximumAnswers]{};
        UiPollProjection poll{};
        struct PollTextContext {
            MessageTextPresentation* presentation;
            const MessageRecord* record;
        } poll_context{presentation, record};
        const auto append_poll = +[](void* source, const UiTextCommand& command, u32 ordinal) noexcept {
            const auto& poll_text = *static_cast<PollTextContext*>(source);
            auto* p = poll_text.presentation;
            if (command.text.bytes > p->utf8.count - p->utf8_count ||
                (command.style_flags != text_style_none && p->style_count >= p->styles.count))
                return fail(p, MessageTextPresentationError::capacity);
            const u32 offset = p->utf8_count;
            copy_bytes(p->utf8.data + offset, command.text.data, command.text.bytes);
            p->utf8_count += command.text.bytes;
            ConstSlice<TextStyleSpan> poll_styles{};
            if (command.style_flags != text_style_none) {
                auto& span = p->styles[p->style_count++];
                span = {0, command.text.bytes, command.style_flags};
                poll_styles = {&span, 1};
            }
            return append_command(p, {p->utf8.data + offset, command.text.bytes}, poll_styles, {},
                                  command.owner, command.rectangle, command.clip, command.color,
                                  poll_text.record->id, poll_text.record->revision,
                                  kInvalidMessageTextMeasurement, MessageTextMeasurementKind::none, 0, 0,
                                  false, command.horizontal_alignment, command.vertical_alignment,
                                  command.wrapping, static_cast<u8>(ordinal), command.overflow);
        };
        UiTextEmitter emitter{};
        UiTextOutput output{nullptr, &emitter};
        if (!message_scene_poll_projection(scene, row, theme, {answers, kUiPollMaximumAnswers}, &poll) ||
            !ui_text_emitter_initialize(&emitter, append_poll, &poll_context) ||
            !ui_poll_controls_text_output(poll, ui_theme_poll_style(scene.theme_styles), row_clip,
                                          {&scene.widgets, row.poll}, &output))
            return fail(presentation, MessageTextPresentationError::layout);
    }
    if (!forwarded && row.invite_item_count &&
        !append_invite_cards(presentation, scene, row, *record, state, theme, row_clip, measurement_index,
                             now_unix_seconds))
        return false;
    if (!rich) {
        return append_annotations(presentation, scene, row, *record, theme, row_clip, measurement_index, forwarded,
                                  0.0f, decorated && !collapsed ? &decoration : nullptr);
    }
    if (rich->attachment_count != 0) {
        for (u32 index = 0; index < rich->attachment_count; ++index) {
            set_failure_context(presentation, MessageTextPresentationStage::attachment,
                                MessageTextPresentationOperation::resolve_layout, static_cast<u16>(index));
            const UiWidgetHandle item_widget =
                message_scene_rich_item_widget(row, MessageSceneRichItemKind::attachment, index);
            const UiLayoutNode* item_layout = message_scene_rich_item_layout_node(
                scene, row, MessageSceneRichItemKind::attachment, index);
            if (!item_layout) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            const UiWidgetReference owner{&scene.widgets, item_widget};
            const float item_left = item_layout->content_rectangle.left;
            const auto& attachment = rich->attachments[index];
            const float audio_height =
                message_rich_attachment_audio_supported(*rich_store, attachment)
                    ? ui_audio_card_height(ui_theme_audio_style(scene.theme_styles),
                                           ui_rect_width(item_layout->content_rectangle),
                                           message_rich_record_voice(*rich))
                    : 0;
            const float item_width =
                minimum(audio_height > 0 ? ui_theme_audio_style(scene.theme_styles).maximum_width : 420.0f,
                        ui_rect_width(item_layout->content_rectangle));
            UiTextFlowLayout flow{};
            const UiRect attachment_clip = ui_rect_intersection(
                row_clip, {item_left, row_clip.top, item_left + item_width, row_clip.bottom});
            if (!initialize_flow(presentation, *item_layout, item_left, item_width, attachment_clip,
                                 &flow)) {
                return false;
            }
            float preview_width = 0.0f;
            float preview_height = 0.0f;
            const bool preview = message_rich_attachment_preview_extent(
                *rich_store, rich->attachments[index], text_width, &preview_width, &preview_height);
            float required_extent = 42.0f;
            if (audio_height > 0) {
                required_extent = audio_height + 6;
                UiAudioControlsProjection projection{};
                const UiRect card{item_left, item_layout->rectangle.top, item_left + item_width,
                                  item_layout->rectangle.top + audio_height};
                struct AudioTextContext {
                    MessageTextPresentation* presentation;
                    const MessageRecord* record;
                    u16 measurement;
                    u8 slot;
                } audio_context{presentation, record, measurement_index, static_cast<u8>(index)};
                UiTextEmitter emitter{};
                const auto append_audio = +[](void* source, const UiTextCommand& command, u32) noexcept {
                    auto& audio = *static_cast<AudioTextContext*>(source);
                    auto* p = audio.presentation;
                    if (command.text.bytes > p->utf8.count - p->utf8_count ||
                        p->style_count >= p->styles.count)
                        return fail(p, MessageTextPresentationError::capacity);
                    const u32 offset = p->utf8_count;
                    copy_bytes(p->utf8.data + offset, command.text.data, command.text.bytes);
                    p->utf8_count += command.text.bytes;
                    auto& span = p->styles[p->style_count++];
                    span = {0, command.text.bytes, command.style_flags};
                    return append_command(
                        p, {p->utf8.data + offset, command.text.bytes}, {&span, 1}, {}, command.owner,
                        command.rectangle, command.clip, command.color, audio.record->id,
                        audio.record->revision, audio.measurement, MessageTextMeasurementKind::attachment,
                        ui_rect_height(command.rectangle), 0, false, command.horizontal_alignment,
                        command.vertical_alignment, command.wrapping, audio.slot, command.overflow);
                };
                UiTextOutput output{nullptr, &emitter};
                if (!ui_audio_attachment_project(*rich_store, attachment, rich->message_id,
                                                 rich->message_revision, message_rich_record_voice(*rich),
                                                 card, theme, {}, &projection) ||
                    !ui_text_emitter_initialize(&emitter, append_audio, &audio_context,
                                                kUiAudioControlCount) ||
                    !ui_audio_controls_text_output(projection, attachment_clip, owner, &output) ||
                    !ui_text_flow_advance(&flow, required_extent))
                    return fail(presentation, MessageTextPresentationError::layout);
            } else if (preview) {
                required_extent = preview_height + 6.0f;
                const MessageMediaObscure obscure = message_scene_media_obscure(
                    scene, rich->message_id, message_media_reveal_key(rich->message_id, MessageMediaSource::attachment, index),
                    message_rich_attachment_spoiler(*rich_store, attachment),
                    message_rich_attachment_explicit(attachment));
                const UiRect media{item_left, item_layout->rectangle.top, item_left + preview_width,
                                   item_layout->rectangle.top + preview_height};
                if (!append_obscure_label(presentation, obscure, media, owner, attachment_clip, theme, *record,
                                          measurement_index) ||
                    !ui_text_flow_advance(&flow, required_extent)) {
                    return fail(presentation, MessageTextPresentationError::layout);
                }
            } else {
                Utf8View label = message_rich_store_string(*rich_store, rich->attachments[index].filename);
                if (label.bytes == 0) {
                    label = utf8_literal("Attachment");
                }
                UiRect label_rectangle{};
                if (!ui_text_flow_advance(&flow, 7.0f) ||
                    !begin_flow_item_clamped(&flow, 36.0f, 44.0f, &label_rectangle) ||
                    !append_command(presentation, label, {}, {}, owner, label_rectangle, attachment_clip,
                                    theme.text_primary, record->id, record->revision, measurement_index,
                                    MessageTextMeasurementKind::attachment, 20.0f, 0.0f, false,
                                    UiTextAlignment::start, UiTextAlignment::start, UiTextWrapping::no_wrap,
                                    0, UiTextOverflow::ellipsis) ||
                    !ui_text_flow_commit_item(&flow, 20.0f, 20.0f, 8.0f)) {
                    return fail(presentation, MessageTextPresentationError::layout);
                }
            }
            const u32 slot = kMessageSceneAttachmentItemOffset + index;
            if (!ui_text_flow_ensure_extent(&flow, required_extent) ||
                !finish_flow(
                    presentation, flow,
                    &presentation->measurements[measurement_index].rich_item_layout.heights[slot])) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            saturating_accumulate(presentation->rich.attachments);
        }
        presentation->measurements[measurement_index].rich_layout.attachments =
            rich_item_total(presentation->measurements[measurement_index].rich_item_layout,
                            kMessageSceneAttachmentItemOffset, rich->attachment_count);
    }

    if (rich->embed_count != 0 && message_rich_display().embeds) {
        for (u32 index = 0; index < rich->embed_count; ++index) {
            set_failure_context(presentation, MessageTextPresentationStage::embed,
                                MessageTextPresentationOperation::resolve_layout, static_cast<u16>(index));
            const UiWidgetHandle item_widget =
                message_scene_rich_item_widget(row, MessageSceneRichItemKind::embed, index);
            const UiLayoutNode* item_layout =
                message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::embed, index);
            if (!item_layout) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            const UiWidgetReference owner{&scene.widgets, item_widget};
            MessageRichItemLayout& merged_items = presentation->measurements[measurement_index].rich_item_layout;
            if (message_rich_embed_merge_host(*rich_store, *rich, index) != index) {
                // A merged embed's image is drawn in the first embed's gallery.
                merged_items.heights[kMessageSceneEmbedItemOffset + index] = 0.0f;
                merged_items.embed_widths[index] = merged_items.embed_media_tops[index] = 0.0f;
                merged_items.embed_media_widths[index] = merged_items.embed_media_heights[index] = 0.0f;
                merged_items.embed_author_tops[index] = merged_items.embed_footer_tops[index] = 0.0f;
                continue;
            }
            UiTextFlowLayout flow{};
            const float embed_left = item_layout->content_rectangle.left;
            const float maximum_width =
                minimum(ui_rect_width(item_layout->content_rectangle), kMessageEmbedMaximumWidth);
            const float retained_width = row.rich_item_layout.embed_widths[index];
            const float embed_width =
                minimum(maximum_width, retained_width > 0 ? retained_width : maximum_width);
            const MessageRichEmbed& embed = rich->embeds[index];
            const MessageRichEmbedPlan plan = message_rich_embed_plan(*rich_store, embed);
            const bool media_only = plan.media_only;
            float thumb_width = 0, thumb_height = 0;
            const bool thumbnail = message_rich_embed_thumbnail_extent(
                *rich_store, embed, maximum(1.f, embed_width - kMessageEmbedContentInset - kMessageEmbedEndInset),
                &thumb_width, &thumb_height);
            const float thumbnail_inset = thumbnail ? thumb_width + kMessageEmbedThumbnailGap : 0;
            const float body_width = maximum(1.f, embed_width - thumbnail_inset);
            const UiRect embed_clip = ui_rect_intersection(
                row_clip, UiRect{embed_left, row_clip.top, embed_left + embed_width, row_clip.bottom});
            if (!ui_rect_has_area(embed_clip) ||
                !initialize_flow(presentation, *item_layout, embed_left, body_width, embed_clip, &flow)) {
                return false;
            }
            const u32 embed_command_begin = presentation->command_count;
            float natural_width = 0;
            MessageRichItemLayout& measured_items =
                presentation->measurements[measurement_index].rich_item_layout;
            measured_items.embed_author_tops[index] = 0.0f;
            measured_items.embed_footer_tops[index] = 0.0f;
            const auto measure_from = [&](u32 first_command, float inset) noexcept {
                for (u32 c = first_command; c < presentation->command_count; ++c)
                    natural_width = maximum(natural_width, presentation->commands[c].measured_width + inset);
            };
            const auto block_gap = [&]() noexcept { return ui_text_flow_advance(&flow, kMessageEmbedBlockGap); };
            const auto source = [&](u32 slot) noexcept {
                return static_cast<u16>(kRichTextSource | (context.forwarded ? kForwardedTextSource : 0u) |
                                        (kRichTextEmbedBegin + index * kRichTextEmbedSlots + slot));
            };
            const Color muted = ui_background_muted_text_color(theme);
            constexpr float text_insets = kMessageEmbedContentInset + kMessageEmbedEndInset;
            if (!media_only) {
                if (!ui_text_flow_advance(&flow, kMessageEmbedTopInset))
                    return fail(presentation, MessageTextPresentationError::layout);
                // Provider: small subtext (12 DIP).
                const Utf8View provider = message_rich_store_string(*rich_store, embed.provider_name);
                const Utf8View provider_url = message_rich_store_string(*rich_store, embed.provider_url);
                if (provider.bytes) {
                    const u32 first = presentation->command_count;
                    if (!block_gap() ||
                        !append_embed_plain_line(presentation, &context, provider, &flow, kMessageEmbedContentInset,
                                                 kMessageEmbedEndInset, muted, owner, record->id, record->revision,
                                                 measurement_index, text_style_subtext, provider_url, source(7),
                                                 0.0f, source(6), text_size_xs))
                        return false;
                    measure_from(first, text_insets);
                }
                // Author: optional 24-DIP round icon, semibold name, author.url link.
                const Utf8View author_name = message_rich_store_string(*rich_store, embed.author_name);
                if (author_name.bytes) {
                    if (!block_gap())
                        return fail(presentation, MessageTextPresentationError::layout);
                    const bool icon = message_rich_embed_author_icon(*rich_store, embed).bytes != 0;
                    const float inset =
                        kMessageEmbedContentInset + (icon ? kMessageEmbedAuthorIcon + kMessageEmbedIconGap : 0.0f);
                    measured_items.embed_author_tops[index] = flow.cursor_y - flow.start_y;
                    const u32 first = presentation->command_count;
                    if (!append_embed_plain_line(presentation, &context, author_name, &flow, inset, kMessageEmbedEndInset,
                                                 theme.text_primary, owner, record->id, record->revision,
                                                 measurement_index, text_style_bold,
                                                 message_rich_store_string(*rich_store, embed.author_url),
                                                 source(5), icon ? kMessageEmbedAuthorIcon : 0.0f, source(0),
                                                 text_size_sm))
                        return false;
                    measure_from(first, inset + kMessageEmbedEndInset);
                }
                // Title: semibold, link-coloured when embed.url is set; title rules.
                const Utf8View title = message_rich_store_string(*rich_store, embed.title);
                if (title.bytes) {
                    const Utf8View url = message_rich_store_string(*rich_store, embed.url);
                    const u32 first = presentation->command_count;
                    if (!block_gap() ||
                        !append_embed_block(presentation, &context, title, &flow, kMessageEmbedContentInset,
                                            kMessageEmbedEndInset, theme.text_primary, owner, record->id,
                                            record->revision, measurement_index, plan.markdown, 1,
                                            text_style_bold | (url.bytes ? text_style_link : 0u), text_size_md, url,
                                            source(4)))
                        return false;
                    measure_from(first, text_insets);
                }
                const Utf8View description = message_rich_store_string(*rich_store, embed.description);
                if (description.bytes && plan.description) {
                    const u32 first = presentation->command_count;
                    if (!block_gap() ||
                        !append_embed_block(presentation, &context, description, &flow, kMessageEmbedContentInset,
                                            kMessageEmbedEndInset, theme.text_primary, owner, record->id,
                                            record->revision, measurement_index, plan.markdown, 0, 0,
                                            plan.components ? text_size_inherit : text_size_sm))
                        return false;
                    measure_from(first, text_insets);
                }
                if (plan.fields &&
                    !append_embed_fields(presentation, &context, *rich_store, *rich, embed, &flow,
                                         theme.text_primary, theme.text_primary, owner, record->id,
                                         record->revision, measurement_index, &natural_width, plan.markdown)) {
                    return false;
                }
                natural_width += thumbnail_inset;
                if (thumbnail &&
                    !ui_text_flow_ensure_extent(&flow, kMessageEmbedTopInset + kMessageEmbedBlockGap + thumb_height))
                    return false;
            }
            flow.content_rectangle.right = embed_left + embed_width;
            measured_items.embed_media_tops[index] = 0.0f;
            measured_items.embed_media_widths[index] = 0.0f;
            measured_items.embed_media_heights[index] = 0.0f;
            MessageRichEmbedMediaSelection media{};
            MessageRichEmbedGallery gallery{};
            float media_width = 0.0f;
            float media_height = 0.0f;
            // A gallery sits in the text column, beside any corner thumbnail.
            if (message_rich_embed_gallery(*rich_store, *rich, index, &gallery)) {
                media_width = message_rich_embed_gallery_width(
                    *rich_store, embed, maximum(0.0f, body_width - (media_only ? 0.0f : text_insets)));
                media_height = kMessageEmbedGalleryHeight;
            }
            if ((media_width > 0.0f && media_height > 0.0f) ||
                (!gallery.count && message_rich_embed_media_select(*rich_store, embed, &media) &&
                 message_rich_embed_media_extent(media, maximum(0.0f, embed_width - (media_only ? 0.0f : text_insets)),
                                                 &media_width, &media_height))) {
                if (!media_only && !ui_text_flow_advance(&flow, kMessageEmbedMediaGap))
                    return fail(presentation, MessageTextPresentationError::layout);
                measured_items.embed_media_tops[index] = flow.cursor_y - flow.start_y;
                measured_items.embed_media_widths[index] = media_width;
                measured_items.embed_media_heights[index] = media_height;
                const MessageMediaObscure obscure = message_scene_media_obscure(
                    scene, record->id, message_media_reveal_key(record->id, MessageMediaSource::embed, index), false,
                    message_rich_embed_explicit(embed));
                const float media_left = embed_left + message_rich_embed_media_left(media_only);
                const UiRect media_box{media_left, flow.cursor_y, media_left + media_width,
                                       flow.cursor_y + media_height};
                if (!append_obscure_label(presentation, obscure, media_box, owner, embed_clip, theme, *record,
                                          measurement_index))
                    return false;
                if (!ui_text_flow_advance(&flow, media_height)) {
                    return fail(presentation, MessageTextPresentationError::layout);
                }
            }
            if (media_only) {
                if (!ui_text_flow_ensure_extent(&flow, media_height)) {
                    return fail(presentation, MessageTextPresentationError::layout);
                }
            } else {
                // Footer: optional 20-DIP round icon, "text \u2022 Yesterday at 10:08 PM".
                const Utf8View footer = message_rich_store_string(*rich_store, embed.footer_text);
                char stamp[kMessageMarkdownTimestampMaximumBytes]{};
                const TimestampFormatResult formatted =
                    embed.timestamp_ms ? format_timestamp_calendar(embed.timestamp_ms / 1'000u,
                                                                   context.now_unix_seconds, {stamp, sizeof(stamp)})
                                       : TimestampFormatResult{};
                const u32 stamp_bytes = formatted.ok ? formatted.bytes : 0u;
                if (stamp_bytes && formatted.next_update_unix_seconds &&
                    (presentation->next_timestamp_update_unix_seconds == 0 ||
                     formatted.next_update_unix_seconds < presentation->next_timestamp_update_unix_seconds))
                    presentation->next_timestamp_update_unix_seconds = formatted.next_update_unix_seconds;
                Utf8View line = footer;
                if (stamp_bytes) {
                    const Utf8View separator = utf8_literal(" \xE2\x80\xA2 ");
                    const u32 bytes = footer.bytes + (footer.bytes ? separator.bytes : 0u) + stamp_bytes;
                    if (bytes > presentation->utf8.count - presentation->utf8_count)
                        return fail(presentation, MessageTextPresentationError::capacity);
                    char* out = presentation->utf8.data + presentation->utf8_count;
                    u32 at = 0;
                    copy_bytes(out, footer.data, footer.bytes);
                    at += footer.bytes;
                    if (footer.bytes) {
                        copy_bytes(out + at, separator.data, separator.bytes);
                        at += separator.bytes;
                    }
                    copy_bytes(out + at, stamp, stamp_bytes);
                    presentation->utf8_count += bytes;
                    line = Utf8View{out, bytes};
                }
                if (line.bytes) {
                    if (!block_gap())
                        return fail(presentation, MessageTextPresentationError::layout);
                    const bool icon = message_rich_embed_footer_icon(*rich_store, embed).bytes != 0;
                    const float inset =
                        kMessageEmbedContentInset + (icon ? kMessageEmbedFooterIcon + kMessageEmbedIconGap : 0.0f);
                    measured_items.embed_footer_tops[index] = flow.cursor_y - flow.start_y;
                    const u32 first = presentation->command_count;
                    if (!append_embed_plain_line(presentation, &context, line, &flow, inset, kMessageEmbedEndInset,
                                                 muted, owner, record->id, record->revision, measurement_index,
                                                 text_style_subtext, {}, 0, icon ? kMessageEmbedFooterIcon : 0.0f,
                                                 footer.bytes ? source(3) : 0, text_size_xs))
                        return false;
                    measure_from(first, inset + kMessageEmbedEndInset);
                }
                if (!ui_text_flow_advance(&flow, kMessageEmbedBottomInset + kMessageEmbedItemGap) ||
                    !ui_text_flow_ensure_extent(&flow, 52.0f + kMessageEmbedItemGap)) {
                    return fail(presentation, MessageTextPresentationError::layout);
                }
            }
            const u32 slot = kMessageSceneEmbedItemOffset + index;
            // The content width comes from the initial maximum-width pass.
            // Measuring again after wrapping at that width feeds the shorter
            // wrapped lines back and keeps shrinking the card. A new
            // viewport/content identity resets retained_width.
            bool measured = true;
            for (u32 c = embed_command_begin; c < presentation->command_count; ++c)
                measured = measured && presentation->commands[c].measured_height > 0;
            natural_width = maximum(natural_width, media_width + (media_only ? 0 : text_insets) +
                                                       (gallery.count ? thumbnail_inset : 0.0f));
            if (measured && retained_width <= 0)
                measured_items.embed_widths[index] = minimum(maximum_width, maximum(64.f, natural_width));
            if (!finish_flow(
                    presentation, flow,
                    &presentation->measurements[measurement_index].rich_item_layout.heights[slot])) {
                return false;
            }
            saturating_accumulate(presentation->rich.embeds);
        }
        presentation->measurements[measurement_index].rich_layout.embeds =
            rich_item_total(presentation->measurements[measurement_index].rich_item_layout,
                            kMessageSceneEmbedItemOffset, rich->embed_count);
    }

    if (rich->sticker_count != 0) {
        for (u32 index = 0; index < rich->sticker_count; ++index) {
            set_failure_context(presentation, MessageTextPresentationStage::sticker,
                                MessageTextPresentationOperation::resolve_layout, static_cast<u16>(index));
            const UiWidgetHandle item_widget =
                message_scene_rich_item_widget(row, MessageSceneRichItemKind::sticker, index);
            const UiLayoutNode* item_layout =
                message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::sticker, index);
            if (!item_layout) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            const UiWidgetReference owner{&scene.widgets, item_widget};
            UiTextFlowLayout flow{};
            if (!initialize_flow(presentation, *item_layout, item_layout->content_rectangle.left,
                                 ui_rect_width(item_layout->content_rectangle), row_clip, &flow)) {
                return false;
            }
            Utf8View label = message_rich_store_string(*rich_store, rich->stickers[index].name);
            if (label.bytes == 0) {
                label = utf8_literal("Sticker");
            }
            if (!ui_text_flow_advance(&flow, kDefaultMessageStickerLayoutMetrics.caption_offset()) ||
                !append_rich_line(presentation, label, &flow, 0.0f, 0.0f, theme.text_primary, owner,
                                  record->id, record->revision, measurement_index,
                                  MessageTextMeasurementKind::sticker) ||
                !ui_text_flow_ensure_extent(&flow, kDefaultMessageStickerLayoutMetrics.minimum_height())) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            const u32 slot = kMessageSceneStickerItemOffset + index;
            if (!finish_flow(
                    presentation, flow,
                    &presentation->measurements[measurement_index].rich_item_layout.heights[slot])) {
                return false;
            }
            saturating_accumulate(presentation->rich.stickers);
        }
        presentation->measurements[measurement_index].rich_layout.stickers =
            rich_item_total(presentation->measurements[measurement_index].rich_item_layout,
                            kMessageSceneStickerItemOffset, rich->sticker_count);
    }

    if (rich->component_count != 0) {
        for (u32 index = 0; index < rich->component_count; ++index) {
            set_failure_context(presentation, MessageTextPresentationStage::component,
                                MessageTextPresentationOperation::resolve_layout, static_cast<u16>(index));
            const MessageRichComponent& component = rich->components[index];
            const MessageComponentKind kind = message_component_kind(component);
            const u32 slot = kMessageSceneComponentItemOffset + index;
            auto& measured_items = row_measurement.rich_item_layout;
            const bool text_leaf = kind == MessageComponentKind::text || kind == MessageComponentKind::button ||
                                   kind == MessageComponentKind::select || kind == MessageComponentKind::file;
            // Parents are folded from their measured children below; media
            // leaves and separators have fixed extents.
            if (!text_leaf) {
                if (kind == MessageComponentKind::thumbnail || kind == MessageComponentKind::gallery_item) {
                    const MessageMediaObscure obscure = message_scene_media_obscure(
                        scene, 0, message_media_reveal_key(record->id, MessageMediaSource::component, index),
                        message_rich_component_spoiler(component), false);
                    const UiLayoutNode* media =
                        message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::component, index);
                    if (media && obscure != MessageMediaObscure::none &&
                        !append_obscure_label(
                            presentation, obscure, media->rectangle,
                            {&scene.widgets, message_scene_rich_item_widget(row, MessageSceneRichItemKind::component,
                                                                            index)},
                            row_clip, theme, *record, measurement_index))
                        return false;
                }
                measured_items.heights[slot] = message_component_leaf_height(component);
                if (kind == MessageComponentKind::thumbnail)
                    measured_items.component_widths[index] = kMessageComponentThumbnailExtent;
                if (kind != MessageComponentKind::thumbnail && kind != MessageComponentKind::separator)
                    saturating_accumulate(presentation->rich.skipped_components);
                if (kind == MessageComponentKind::gallery) {
                    const UiLayoutNode* gallery =
                        message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::component, index);
                    const UiLayoutNode* region = message_scene_layout_node(scene, row.components);
                    const float available = region ? ui_rect_width(region->content_rectangle)
                                            : gallery ? ui_rect_width(gallery->rectangle)
                                                      : text_width;
                    // Containers cap their width; galleries inside them fit the padded lane.
                    float ancestors = 0;
                    bool contained = false;
                    for (u16 at = component.parent; at != 0xFFFFu; at = rich->components[at].parent)
                        if (rich->components[at].type == gateway_component_container) {
                            ancestors += 2 * kMessageComponentPadding;
                            contained = true;
                        }
                    const float lane = contained ? minimum(available, kMessageComponentMaximumWidth) : available;
                    measured_items.component_widths[index] =
                        message_component_leaf_width(component, maximum(1.0f, lane - ancestors));
                }
                continue;
            }
            const UiWidgetHandle item_widget =
                message_scene_rich_item_widget(row, MessageSceneRichItemKind::component, index);
            const UiLayoutNode* item_layout =
                message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::component, index);
            if (!item_layout) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            const UiWidgetReference owner{&scene.widgets, item_widget};
            const UiRect box = item_layout->rectangle;
            const UiRect item_clip = ui_rect_intersection(row_clip, box);
            const u32 command_begin = presentation->command_count;
            if (kind == MessageComponentKind::text) {
                UiTextFlowLayout flow{};
                if (!initialize_flow(presentation, *item_layout, item_layout->content_rectangle.left,
                                     ui_rect_width(item_layout->content_rectangle), row_clip, &flow) ||
                    !append_rich_markdown_line(
                        presentation, &context, message_rich_store_string(*rich_store, component.content),
                        &flow, 0.0f, 0.0f, theme.text_primary, owner, record->id, record->revision,
                        measurement_index, MessageTextMeasurementKind::component)) {
                    return false;
                }
                set_failure_operation(presentation, MessageTextPresentationOperation::ensure_extent);
                if (!ui_text_flow_ensure_extent(&flow, message_component_leaf_height(component))) {
                    presentation->failure_flow_error = flow.last_error;
                    return fail(presentation, MessageTextPresentationError::layout);
                }
                if (!finish_flow(presentation, flow, &measured_items.heights[slot]))
                    return false;
                float measured_width = 0;
                for (u32 c = command_begin; c < presentation->command_count; ++c)
                    measured_width = maximum(measured_width, presentation->commands[c].measured_width);
                if (measured_width > 0)
                    measured_items.component_widths[index] =
                        static_cast<float>(static_cast<u32>(measured_width + 0.999f));
                saturating_accumulate(presentation->rich.components);
                continue;
            }
            // Single-line controls: button labels, select placeholders and file names.
            Utf8View label{};
            Color color = theme.text_primary;
            float left_inset = kMessageComponentButtonPadding, right_inset = kMessageComponentButtonPadding;
            UiTextAlignment alignment = UiTextAlignment::start;
            float fixed_height = message_component_leaf_height(component);
            if (kind == MessageComponentKind::button) {
                label = message_rich_store_string(*rich_store, component.label);
                color = message_component_button_colors(theme, component.style, component.disabled).text;
                const bool emoji = component.emoji_id != 0 || component.emoji_name.bytes != 0;
                const bool link = message_component_is_link(component);
                // Unicode emoji render as text ahead of the label; custom
                // emoji are media painted in the leading inset.
                Utf8View unicode = component.emoji_id ? Utf8View{} :
                                       message_rich_store_string(*rich_store, component.emoji_name);
                const Utf8View suffix{};
                if (link)
                    right_inset += 18.0f; // external-link glyph lane
                if (unicode.bytes) {
                    const u32 bytes = unicode.bytes + (unicode.bytes && label.bytes ? 1u : 0u) + label.bytes +
                                      suffix.bytes;
                    if (bytes > presentation->utf8.count - presentation->utf8_count)
                        return fail(presentation, MessageTextPresentationError::capacity);
                    char* retained = presentation->utf8.data + presentation->utf8_count;
                    u32 at = 0;
                    copy_bytes(retained, unicode.data, unicode.bytes);
                    at += unicode.bytes;
                    if (unicode.bytes && label.bytes)
                        retained[at++] = ' ';
                    copy_bytes(retained + at, label.data, label.bytes);
                    at += label.bytes;
                    copy_bytes(retained + at, suffix.data, suffix.bytes);
                    label = {retained, bytes};
                    presentation->utf8_count += bytes;
                }
                if (emoji && component.emoji_id)
                    left_inset += kMessageComponentEmojiExtent + (label.bytes ? 4.0f : 0.0f);
                alignment = UiTextAlignment::center;
                // A pending interaction replaces the label with a spinner.
                if (scene.interaction.pending && scene.interaction.message_id == record->id &&
                    scene.interaction.component_index == index)
                    label = {};
            } else if (kind == MessageComponentKind::select) {
                // Default options read as the current choice, else the placeholder.
                color = ui_background_muted_text_color(theme);
                for (u32 option = index + 1u; option < rich->component_count && !label.bytes; ++option)
                    if (rich->components[option].parent == index &&
                        rich->components[option].type == gateway_component_select_option &&
                        (rich->components[option].flags & gateway_message_component_default)) {
                        label = message_rich_store_string(*rich_store, rich->components[option].label);
                        color = theme.text_primary;
                    }
                if (!label.bytes)
                    label = message_rich_store_string(*rich_store, component.content);
                if (!label.bytes)
                    label = utf8_literal("Make a selection");
                right_inset += 24.0f; // chevron lane
            } else {
                label = message_rich_store_string(*rich_store, component.label);
                if (!label.bytes)
                    label = utf8_literal("File");
                left_inset += 36.0f; // file glyph
                color = theme.tokens.text.link;
            }
            const float available = ui_rect_width(box);
            const float clamped_left = minimum(left_inset, maximum(0.0f, available - 1.0f));
            const float clamped_right = minimum(right_inset, maximum(0.0f, available - 1.0f - clamped_left));
            const UiRect label_rectangle{box.left + clamped_left, box.top, box.right - clamped_right,
                                         box.top + fixed_height};
            if (label.bytes && ui_rect_has_area(item_clip) &&
                !append_command(presentation, label, {}, {}, owner, label_rectangle, item_clip, color, record->id,
                                record->revision, measurement_index, MessageTextMeasurementKind::component, 0.0f,
                                0.0f, false, alignment, UiTextAlignment::center, UiTextWrapping::no_wrap,
                                static_cast<u8>(index), UiTextOverflow::ellipsis))
                return false;
            measured_items.heights[slot] = fixed_height;
            if (kind == MessageComponentKind::button) {
                float measured_width = 0;
                for (u32 c = command_begin; c < presentation->command_count; ++c)
                    measured_width = maximum(measured_width, presentation->commands[c].measured_width);
                if (measured_width > 0)
                    // Round outward so a fractional glyph advance cannot clip
                    // when the hugging row is rebuilt from this measurement.
                    measured_items.component_widths[index] = maximum(
                        kMessageComponentButtonMinimumWidth,
                        static_cast<float>(static_cast<u32>(measured_width + 0.999f)) + left_inset + right_inset);
                else if (!label.bytes && !component.label.bytes)
                    measured_items.component_widths[index] =
                        kMessageComponentEmojiExtent + 2.0f * kMessageComponentButtonPadding;
            }
            saturating_accumulate(presentation->rich.components);
        }
        // Rows wrap within the components region (or the text column before it lays out).
        const UiLayoutNode* components_region = message_scene_layout_node(scene, row.components);
        const float components_available =
            components_region ? ui_rect_width(components_region->content_rectangle) : text_width;
        presentation->measurements[measurement_index].rich_layout.components = message_component_measure_tree(
            *rich, &row_measurement.rich_item_layout, maximum(1.0f, components_available));
    }

    MessageReactionTextItem reaction_items[kGatewayMessageReactionMaximum]{};
    set_failure_context(presentation, MessageTextPresentationStage::reaction,
                        MessageTextPresentationOperation::build_reactions);
    const MessageReactionPresentationResult reaction_result =
        !message_rich_display().reactions ? MessageReactionPresentationResult{} : message_reaction_presentation_build(
        scene, row, *rich_store, *rich,
        Slice<char>{presentation->utf8.data + presentation->utf8_count,
                    presentation->utf8.count - presentation->utf8_count},
        Slice<MessageReactionTextItem>{reaction_items, kGatewayMessageReactionMaximum});
    if (!reaction_result.ok()) {
        const MessageTextPresentationError reaction_error =
            reaction_result.error == MessageReactionPresentationError::capacity
                ? MessageTextPresentationError::capacity
            : reaction_result.error == MessageReactionPresentationError::layout
                ? MessageTextPresentationError::layout
                : MessageTextPresentationError::model;
        return fail(presentation, reaction_error);
    }
    presentation->utf8_count += reaction_result.utf8_bytes;
    for (u32 index = 0; index < reaction_result.item_count; ++index) {
        const MessageReactionTextItem& item = reaction_items[index];
        if (!append_command(
                presentation, item.text, ConstSlice<TextStyleSpan>{}, ConstSlice<MessageMarkdownSemantic>{},
                UiWidgetReference{&scene.widgets, item.widget}, item.content_rectangle, row_clip,
                item.selected ? theme.text_primary : ui_background_muted_text_color(theme), record->id,
                record->revision, measurement_index, MessageTextMeasurementKind::reaction, 0.0f, 0.0f,
                false, UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap,
                static_cast<u8>(index))) {
            return false;
        }
        saturating_accumulate(presentation->rich.reactions);
    }

    if (row.rich_layout.thread > 0.0f && rich->thread.id) {
        const auto* node = message_scene_layout_node(scene, row.thread);
        if (!node)
            return fail(presentation, MessageTextPresentationError::layout);
        const UiRect card = message_thread_card_rectangle(node->rectangle);
        const float left = card.left + kMessageThreadCardPaddingDips + 30.0f;
        const float right = maximum(left + 1.0f, card.right - kMessageThreadCardPaddingDips);
        const UiWidgetReference owner{&scene.widgets, row.thread};
        Utf8View name = message_rich_store_string(*scene.timeline->rich_store, rich->thread.name);
        if (!name.bytes)
            name = utf8_literal("Thread");
        char detail[96];
        const u32 detail_bytes = message_thread_card_detail(rich->thread, {detail, sizeof(detail)});
        const Utf8View meta = detail_bytes ? Utf8View{detail, detail_bytes} : utf8_literal("Open thread");
        if (meta.bytes > presentation->utf8.count - presentation->utf8_count ||
            presentation->style_count >= presentation->styles.count)
            return fail(presentation, MessageTextPresentationError::capacity);
        const u32 offset = presentation->utf8_count;
        copy_bytes(presentation->utf8.data + offset, meta.data, meta.bytes);
        presentation->utf8_count += meta.bytes;
        auto& bold = presentation->styles[presentation->style_count++];
        bold = {0, name.bytes, text_style_bold};
        const bool single = !detail_bytes && !rich->thread.name.bytes;
        if (!append_command(presentation, name, {&bold, 1}, {}, owner,
                            {left, card.top + (single ? 16.0f : 8.0f), right, card.top + (single ? 34.0f : 26.0f)},
                            row_clip, theme.text_primary, record->id, record->revision,
                            kInvalidMessageTextMeasurement, MessageTextMeasurementKind::none, 0, 0, false,
                            UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap, 0,
                            UiTextOverflow::ellipsis) ||
            (!single &&
             !append_command(presentation, {presentation->utf8.data + offset, meta.bytes}, {}, {}, owner,
                             {left, card.top + 27.0f, right, card.top + 43.0f}, row_clip, theme.text_secondary,
                             record->id, record->revision, kInvalidMessageTextMeasurement,
                             MessageTextMeasurementKind::none, 0, 0, false, UiTextAlignment::start,
                             UiTextAlignment::center, UiTextWrapping::no_wrap, 1, UiTextOverflow::ellipsis)))
            return false;
    }
    if (rich->truncations != 0) {
        set_failure_context(presentation, MessageTextPresentationStage::notice,
                            MessageTextPresentationOperation::resolve_layout, 0);
        const UiWidgetHandle item_widget =
            message_scene_rich_item_widget(row, MessageSceneRichItemKind::notice, 0);
        const UiLayoutNode* item_layout =
            message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::notice, 0);
        UiTextFlowLayout flow{};
        if (!item_layout ||
            !initialize_flow(presentation, *item_layout, item_layout->content_rectangle.left,
                             ui_rect_width(item_layout->content_rectangle), row_clip, &flow) ||
            !ui_text_flow_advance(&flow, 4.0f) ||
            !append_rich_line(presentation, utf8_literal("Some content can't be shown"), &flow, 0.0f,
                              0.0f, ui_background_muted_text_color(theme),
                              UiWidgetReference{&scene.widgets, item_widget}, record->id, record->revision,
                              measurement_index, MessageTextMeasurementKind::notice) ||
            !finish_flow(presentation, flow,
                         &presentation->measurements[measurement_index]
                              .rich_item_layout.heights[kMessageSceneNoticeItemOffset])) {
            return false;
        }
        presentation->measurements[measurement_index].rich_layout.notice =
            presentation->measurements[measurement_index]
                .rich_item_layout.heights[kMessageSceneNoticeItemOffset];
        saturating_accumulate(presentation->rich.truncated_records);
    }
    return append_annotations(presentation, scene, row, *record, theme, row_clip, measurement_index, forwarded,
                              rich->truncations != 0
                                  ? presentation->measurements[measurement_index]
                                        .rich_item_layout.heights[kMessageSceneNoticeItemOffset]
                                  : 0.0f,
                              decorated && !collapsed ? &decoration : nullptr);
}

} // namespace

u64 message_text_presentation_projection_signature(const MessageTextPresentation& presentation) noexcept {
    if ((!presentation.commands.data && presentation.commands.count != 0) ||
        presentation.command_count > presentation.commands.count) {
        return 0;
    }
    u64 hash = kHash64Seed;
    const auto mix = [](u64 value, u64 input) noexcept { return hash64_mix(value, input); };
    const auto rectangle = [&mix](u64 value, const UiRect& bounds) noexcept {
        value = mix(value, FloatWord{bounds.left}.word);
        value = mix(value, FloatWord{bounds.top}.word);
        value = mix(value, FloatWord{bounds.right}.word);
        return mix(value, FloatWord{bounds.bottom}.word);
    };
    hash = mix(hash, presentation.command_count);
    // Author headers paint plugin name decorations and badges read at paint
    // time; a decoration change must refresh the retained text.
    hash = mix(hash, g_ui_plugin_render_revision);
    for (u32 index = 0; index < presentation.command_count; ++index) {
        const MessageTextCommand& command = presentation.commands[index];
        hash = mix(hash, command.measurement_identity);
        hash = mix(hash, static_cast<u64>(reinterpret_cast<ULONG_PTR>(command.owner.manager)));
        hash = mix(hash, command.owner.handle.slot);
        hash = mix(hash, command.owner.handle.generation);
        hash = rectangle(hash, command.rectangle);
        hash = rectangle(hash, command.clip);
        hash = mix(hash, FloatWord{command.measured_height}.word);
        hash = mix(hash, FloatWord{command.measured_width}.word);
        hash = mix(hash, FloatWord{command.color.red}.word);
        hash = mix(hash, FloatWord{command.color.green}.word);
        hash = mix(hash, FloatWord{command.color.blue}.word);
        hash = mix(hash, FloatWord{command.color.alpha}.word);
        hash = mix(hash, FloatWord{command.inline_role_color.red}.word);
        hash = mix(hash, FloatWord{command.inline_role_color.green}.word);
        hash = mix(hash, FloatWord{command.inline_role_color.blue}.word);
        hash = mix(hash, FloatWord{command.inline_role_color.alpha}.word);
        hash = mix(hash, command.themed_styles);
        hash = mix(hash, static_cast<u8>(command.horizontal_alignment));
        hash = mix(hash, static_cast<u8>(command.vertical_alignment));
        hash = mix(hash, static_cast<u8>(command.wrapping));
        hash = mix(hash, static_cast<u8>(command.overflow));
        hash = mix(hash, command.semantics.count);
        for (u32 semantic_index = 0; semantic_index < command.semantics.count; ++semantic_index) {
            const MessageMarkdownSemantic& semantic = command.semantics[semantic_index];
            hash = mix(hash, semantic.target);
            hash = mix(hash, semantic.source_offset);
            hash = mix(hash, semantic.source_bytes);
            hash = mix(hash, static_cast<u8>(semantic.kind));
            hash = mix(hash, semantic.reserved);
        }
    }
    return hash;
}

bool message_text_presentation_initialize(MessageTextPresentation* presentation,
                                          Slice<MessageTextCommand> commands, Slice<char> utf8,
                                          Slice<TextStyleSpan> styles,
                                          Slice<MessageMarkdownSemantic> semantics,
                                          Slice<MessageTextMeasurement> measurements,
                                          Slice<MessageTextSpoilerReveal> spoiler_reveals,
                                          Slice<u32> byte_style_scratch) noexcept {
    if (!presentation || !commands.data || commands.count == 0 || !utf8.data || utf8.count == 0 ||
        !styles.data || styles.count == 0 || !semantics.data || semantics.count == 0 ||
        !measurements.data || measurements.count == 0 || measurements.count > 0xFFFFu ||
        !spoiler_reveals.data || spoiler_reveals.count == 0 || !byte_style_scratch.data ||
        byte_style_scratch.count == 0) {
        return false;
    }
    *presentation = MessageTextPresentation{};
    presentation->commands = commands;
    presentation->utf8 = utf8;
    presentation->styles = styles;
    presentation->semantics = semantics;
    presentation->measurements = measurements;
    presentation->spoiler_reveals = spoiler_reveals;
    presentation->byte_style_scratch = byte_style_scratch;
    presentation->last_markdown_error = MessageMarkdownError::none;
    presentation->last_error = MessageTextPresentationError::none;
    presentation->failure_message_id = 0;
    presentation->failure_source_index = ~0u;
    presentation->failure_item_index = kInvalidMessageTextMeasurement;
    presentation->failure_stage = MessageTextPresentationStage::none;
    presentation->failure_operation = MessageTextPresentationOperation::none;
    presentation->failure_flow_error = UiTextFlowError::none;
    return true;
}

bool message_markdown_cache_initialize(MessageMarkdownCache* cache, Slice<MessageMarkdownCacheEntry> entries,
                                       Slice<u8> arena) noexcept {
    if (!cache || !entries.data || entries.count == 0 || (entries.count & (entries.count - 1u)) != 0 ||
        !arena.data || arena.count < 64 || arena.count % 8u != 0 || reinterpret_cast<usize>(arena.data) % 8u != 0)
        return false;
    *cache = MessageMarkdownCache{};
    cache->entries = entries.data;
    cache->arena = arena.data;
    cache->slot_count = entries.count;
    cache->arena_bytes = arena.count;
    return true;
}

void message_markdown_cache_clear(MessageMarkdownCache* cache) noexcept {
    if (!cache || !cache->entries)
        return;
    clear_bytes(cache->entries, cache->slot_count * static_cast<u32>(sizeof(MessageMarkdownCacheEntry)));
    cache->head = 0;
}

bool message_text_presentation_bind_markdown_cache(MessageTextPresentation* presentation,
                                                   MessageMarkdownCache* cache) noexcept {
    if (!presentation || (cache && (!cache->entries || !cache->arena)))
        return false;
    presentation->markdown_cache = cache;
    return true;
}

bool message_text_presentation_bind_guild_members(MessageTextPresentation* presentation,
                                                  const GuildMemberStore* members) noexcept {
    if (!presentation || (members && (!members->records || !members->order))) {
        return false;
    }
    presentation->guild_members = members;
    return true;
}

MessageTextPresentationResult message_text_presentation_build(
    MessageTextPresentation* presentation, const MessageScene& scene, const ChatStateStore& state,
    const UserStore* users, const NavigationView& navigation, const Theme& theme,
    u64 now_unix_seconds) noexcept {
    if (presentation) {
        presentation->failure_message_id = 0;
        presentation->failure_source_index = ~0u;
        presentation->failure_item_index = kInvalidMessageTextMeasurement;
        presentation->failure_stage = MessageTextPresentationStage::none;
        presentation->failure_operation = MessageTextPresentationOperation::none;
        presentation->failure_flow_error = UiTextFlowError::none;
    }
    if (!presentation || !presentation->commands.data || !presentation->utf8.data ||
        !presentation->styles.data || !presentation->semantics.data || !presentation->measurements.data ||
        !presentation->spoiler_reveals.data || !presentation->byte_style_scratch.data || !scene.active ||
        !scene.timeline || !scene.timeline->store || scene.count > scene.rows.count ||
        !ui_rect_has_area(scene.viewport) || navigation.channel_count > navigation.channel_capacity) {
        reject_atomically(presentation, MessageTextPresentationError::invalid_input);
        return presentation
                   ? result(*presentation, presentation->last_error)
                   : MessageTextPresentationResult{0, 0, 0, MessageTextPresentationError::invalid_input};
    }
    const u32 previous_count = presentation->command_count;
    if (previous_count > presentation->commands.count ||
        previous_count > presentation->byte_style_scratch.count / 4u) {
        reject_atomically(presentation, MessageTextPresentationError::capacity);
        return result(*presentation, presentation->last_error);
    }
    presentation->measurement_history_count = previous_count;
    presentation->measurement_history_cursor = 0;
    presentation->measurement_history_offset = presentation->byte_style_scratch.count - previous_count * 4u;
    for (u32 index = 0; index < previous_count; ++index) {
        const MessageTextCommand& command = presentation->commands[index];
        const u32 offset = presentation->measurement_history_offset + index * 4u;
        const FloatWord height{command.measured_height};
        const FloatWord width{command.measured_width};
        presentation->byte_style_scratch[offset] = static_cast<u32>(command.measurement_identity);
        presentation->byte_style_scratch[offset + 1u] =
            static_cast<u32>(command.measurement_identity >> 32u);
        presentation->byte_style_scratch[offset + 2u] = height.word;
        presentation->byte_style_scratch[offset + 3u] = width.word;
    }
    presentation->command_count = 0;
    presentation->utf8_count = 0;
    presentation->style_count = 0;
    presentation->semantic_count = 0;
    presentation->measurement_count = 0;
    presentation->markdown_entity_epoch =
        markdown_entity_epoch(state, users, presentation->guild_members, navigation);
    presentation->rich = MessageTextRichMetrics{};
    presentation->next_timestamp_update_unix_seconds = 0;
    presentation->projection_signature = 0;
    presentation->last_markdown_error = MessageMarkdownError::none;
    presentation->last_error = MessageTextPresentationError::none;
    presentation->command_measurements_changed = false;
    presentation->active = false;
    if (!append_intro(presentation, scene, theme)) {
        reject_atomically(presentation, presentation->last_error == MessageTextPresentationError::none
                                            ? MessageTextPresentationError::capacity
                                            : presentation->last_error);
        return result(*presentation, presentation->last_error);
    }
    for (u32 index = 0; index < scene.count; ++index) {
        const MessageSceneRowWidget* snapshot = message_scene_snapshot_row(scene, scene.rows[index]);
        if (!append_row(presentation, scene, scene.rows[index], state, users, navigation, theme,
                        now_unix_seconds) ||
            (snapshot && !append_row(presentation, scene, *snapshot, state, users, navigation, theme,
                                     now_unix_seconds, true))) {
            const MessageTextPresentationError error =
                presentation->last_error == MessageTextPresentationError::none
                    ? MessageTextPresentationError::capacity
                    : presentation->last_error;
            reject_atomically(presentation, error);
            return result(*presentation, presentation->last_error);
        }
    }
    presentation->active = true;
    presentation->failure_message_id = 0;
    presentation->failure_source_index = ~0u;
    presentation->failure_item_index = kInvalidMessageTextMeasurement;
    presentation->failure_stage = MessageTextPresentationStage::none;
    presentation->failure_operation = MessageTextPresentationOperation::none;
    presentation->failure_flow_error = UiTextFlowError::none;
    presentation->projection_signature = message_text_presentation_projection_signature(*presentation);
    saturating_accumulate(presentation->builds);
    return result(*presentation, MessageTextPresentationError::none);
}

bool message_text_presentation_measure(MessageTextPresentation* presentation, UiTextMeasureCache* cache,
                                       u16 backend_revision, bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!presentation || !cache || !changed || !presentation->active ||
        presentation->command_count > presentation->commands.count ||
        presentation->measurement_count > presentation->measurements.count ||
        presentation->command_count > presentation->byte_style_scratch.count / 2u ||
        presentation->command_count > 0xFFFFu) {
        if (presentation) {
            presentation->last_error = MessageTextPresentationError::measurement;
            saturating_accumulate(presentation->rejected);
        }
        return false;
    }
    for (u32 index = 0; index < presentation->command_count; ++index) {
        const MessageTextCommand& command = presentation->commands[index];
        if ((command.message_id == 0 && (command.measurement_kind != MessageTextMeasurementKind::none ||
                                         command.semantics.count != 0)) ||
            !ui_rect_has_area(command.rectangle) || (!command.text.data && command.text.bytes != 0) ||
            (!command.styles.data && command.styles.count != 0) ||
            command.styles.count > kTextStyleSpanMaximum ||
            command.measurement_kind > MessageTextMeasurementKind::notice ||
            (command.measurement_kind != MessageTextMeasurementKind::none &&
             command.measurement_index >= presentation->measurement_count)) {
            presentation->last_error = MessageTextPresentationError::measurement;
            saturating_accumulate(presentation->rejected);
            return false;
        }
    }
    for (u32 index = 0; index < presentation->command_count; ++index) {
        const MessageTextCommand& command = presentation->commands[index];
        const UiTextMeasureRequest request{
            UiTextMeasureKey{
                command.message_id, command_content_signature(command), command.message_revision,
                static_cast<u16>(command.measurement_identity ^ (command.measurement_identity >> 16u) ^
                                 (command.measurement_identity >> 32u) ^
                                 (command.measurement_identity >> 48u)),
                backend_revision},
            command.text, command.styles,
            UiSize{ui_rect_width(command.rectangle), ui_rect_height(command.rectangle)},
            command.wrapping == UiTextWrapping::no_wrap};
        UiTextMeasureValue value{};
        if (!ui_text_measure_cache_measure(cache, request, &value).ok()) {
            presentation->last_error = MessageTextPresentationError::measurement;
            saturating_accumulate(presentation->rejected);
            return false;
        }
        const FloatWord measured_height{value.size.height};
        const FloatWord measured_width{value.size.width};
        presentation->byte_style_scratch[index * 2u] = measured_height.word;
        presentation->byte_style_scratch[index * 2u + 1u] = measured_width.word;
    }
    for (u32 index = 0; index < presentation->measurement_count; ++index) {
        presentation->measurements[index].reply_height = 0.0f;
        presentation->measurements[index].content_height = 0.0f;
    }
    bool local_changed = false;
    for (u32 index = 0; index < presentation->command_count; ++index) {
        MessageTextCommand& command = presentation->commands[index];
        FloatWord measured_height{};
        FloatWord measured_width{};
        measured_height.word = presentation->byte_style_scratch[index * 2u];
        measured_width.word = presentation->byte_style_scratch[index * 2u + 1u];
        local_changed = local_changed || command.measured_height != measured_height.value ||
                        command.measured_width != measured_width.value;
        command.measured_height = measured_height.value;
        command.measured_width = measured_width.value;
        if (command.measurement_kind == MessageTextMeasurementKind::none) {
            continue;
        }
        MessageTextMeasurement& measurement = presentation->measurements[command.measurement_index];
        if (command.measurement_kind == MessageTextMeasurementKind::reply) {
            measurement.reply_height +=
                maximum(measured_height.value, command.minimum_height) + command.gap_after;
        } else if (command.measurement_kind == MessageTextMeasurementKind::content) {
            // Quote runs split the body into stacked commands.
            measurement.content_height += measured_height.value + command.gap_after;
        } else if (command.measurement_kind == MessageTextMeasurementKind::reaction &&
                   command.measurement_slot < kGatewayMessageReactionMaximum) {
            measurement.reaction_widths[command.measurement_slot] = measured_width.value;
        }
    }
    presentation->command_measurements_changed = local_changed;
    presentation->projection_signature = message_text_presentation_projection_signature(*presentation);
    presentation->last_error = MessageTextPresentationError::none;
    *changed = local_changed;
    return true;
}

bool message_text_presentation_apply_measurements(MessageScene* scene,
                                                  const MessageTextPresentation& presentation,
                                                  bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!scene || !changed || !presentation.active ||
        presentation.measurement_count > presentation.measurements.count) {
        return false;
    }
    for (u32 index = 0; index < presentation.measurement_count; ++index) {
        const MessageTextMeasurement& measurement = presentation.measurements[index];
        const MessageSceneRowWidget* row = message_scene_find_row(*scene, measurement.source_index);
        if (!row || row->message_id != measurement.message_id ||
            row->message_revision != measurement.message_revision) {
            return false;
        }
        bool row_changed = false;
        if (measurement.forwarded) {
            if (!message_scene_set_snapshot_text_metrics(
                    scene, measurement.source_index, measurement.content_height,
                    measurement.content_present, &measurement.rich_layout, &measurement.rich_item_layout,
                    &row_changed))
                return false;
            *changed = *changed || row_changed;
            continue;
        }
        if (!message_scene_set_text_metrics(
                scene, measurement.source_index, measurement.reply_height, measurement.content_height,
                measurement.content_present, &measurement.rich_layout, &measurement.rich_item_layout,
                ConstSlice<float>{measurement.reaction_widths, kGatewayMessageReactionMaximum},
                &row_changed)) {
            return false;
        }
        *changed = *changed || row_changed;
    }
    return true;
}

bool message_text_presentation_set_hover(MessageTextPresentation* presentation, const MessageTextHit& hit,
                                         bool* changed) noexcept {
    if (!presentation || !changed) {
        return false;
    }
    const bool has_hover = hit.valid && hit.kind != MessageMarkdownSemanticKind::none;
    *changed = presentation->has_hover != has_hover ||
               (has_hover && (presentation->hovered_message_id != hit.message_id ||
                              presentation->hovered_source_offset != hit.source_offset ||
                              presentation->hovered_kind != hit.kind));
    presentation->has_hover = has_hover;
    presentation->hovered_message_id = has_hover ? hit.message_id : 0;
    presentation->hovered_source_offset = has_hover ? hit.source_offset : 0;
    presentation->hovered_kind = has_hover ? hit.kind : MessageMarkdownSemanticKind::none;
    return true;
}

Utf8View message_text_presentation_command_source(const MessageStore& store,
                                                  const MessageRichStore* rich_store,
                                                  const MessageTextCommand& command) noexcept {
    const MessageStoreResult found = message_store_find(store, command.message_id);
    const MessageRecord* record = found.ok() ? message_store_resolve(store, found.handle) : nullptr;
    if (!record || record->revision != command.message_revision)
        return {};
    if (command.source_id == 0)
        return message_store_content(store, found.handle);
    // Plugin replacement markdown maps to the cached replacement, never the
    // stored message; generated plugin text is its own source.
    if (command.source_id == kUiPluginRenderContentSource) {
        UiPluginRenderDecoration decoration{};
        return ui_plugin_render(record->id, record->revision, &decoration) && decoration.replace_content
                   ? decoration.content
                   : Utf8View{};
    }
    if (ui_plugin_render_source(command.source_id))
        return command.text;
    if (!rich_store || !(command.source_id & kRichTextSource))
        return {};
    const MessageRichRecord* rich = command.source_id & kForwardedTextSource
                                        ? message_rich_store_snapshot(*rich_store, command.message_id)
                                        : message_rich_store_find(*rich_store, command.message_id);
    if (!rich || rich->message_revision != command.message_revision)
        return {};
    return message_rich_store_string(*rich_store, rich_source_string(*rich, command.source_id));
}

Utf8View message_text_presentation_hit_source(const MessageStore& store, const MessageRichStore* rich_store,
                                              const MessageTextHit& hit) noexcept {
    if (!hit.valid || hit.message_id == 0 || hit.source_bytes == 0)
        return {};
    MessageTextCommand source_command{};
    source_command.message_id = hit.message_id;
    source_command.message_revision = hit.message_revision;
    source_command.source_id = static_cast<u16>(hit.source_offset >> 16u);
    const Utf8View content = message_text_presentation_command_source(store, rich_store, source_command);
    const u32 offset = hit.source_offset & 0xFFFFu;
    if (offset > content.bytes || hit.source_bytes > content.bytes - offset)
        return {};
    return Utf8View{content.data + offset, hit.source_bytes};
}

Utf8View message_text_presentation_hit_source(const MessageStore& store,
                                              const MessageTextHit& hit) noexcept {
    return message_text_presentation_hit_source(store, nullptr, hit);
}

MessageTextCopyProjectionResult message_text_project_selection_for_copy(const MessageTextCommand& command,
                                                                        Utf8View source, u32 first,
                                                                        u32 last,
                                                                        Slice<char> output) noexcept {
    if ((!command.text.data && command.text.bytes != 0) ||
        (!command.semantics.data && command.semantics.count != 0) || (!source.data && source.bytes != 0) ||
        (!output.data && output.count != 0) || first >= last || last > command.text.bytes) {
        return MessageTextCopyProjectionResult{};
    }
    u32 cursor = first;
    u32 written = 0;
    bool transformed = false;
    for (u32 index = 0; index < command.semantics.count; ++index) {
        const MessageMarkdownSemantic& semantic = command.semantics[index];
        if (semantic.kind != MessageMarkdownSemanticKind::custom_emoji || semantic.byte_count == 0 ||
            semantic.byte_offset > command.text.bytes ||
            semantic.byte_count > command.text.bytes - semantic.byte_offset) {
            continue;
        }
        const u32 semantic_end = semantic.byte_offset + semantic.byte_count;
        if (semantic.byte_offset >= last || semantic_end <= first) {
            continue;
        }
        const bool fully_selected = semantic.byte_offset >= first && semantic_end <= last;
        const u32 source_offset = semantic.source_offset & 0xFFFFu;
        if (!fully_selected || semantic.byte_offset < cursor ||
            (semantic.source_offset >> 16u) != command.source_id || source_offset > source.bytes ||
            semantic.source_bytes > source.bytes - source_offset) {
            continue;
        }
        const u32 plain_bytes = semantic.byte_offset - cursor;
        if (plain_bytes > output.count - written ||
            semantic.source_bytes > output.count - written - plain_bytes) {
            return MessageTextCopyProjectionResult{};
        }
        for (u32 byte = 0; byte < plain_bytes; ++byte) {
            output[written++] = command.text.data[cursor + byte];
        }
        for (u32 byte = 0; byte < semantic.source_bytes; ++byte) {
            output[written++] = source.data[source_offset + byte];
        }
        cursor = semantic_end;
        transformed = true;
    }
    const u32 tail_bytes = last - cursor;
    if (tail_bytes > output.count - written) {
        return MessageTextCopyProjectionResult{};
    }
    for (u32 byte = 0; byte < tail_bytes; ++byte) {
        output[written++] = command.text.data[cursor + byte];
    }
    return MessageTextCopyProjectionResult{written, transformed, true};
}

bool message_text_presentation_toggle_spoiler(MessageTextPresentation* presentation,
                                              const MessageStore& store, const MessageTextHit& hit,
                                              bool* changed) noexcept {
    return message_text_presentation_toggle_spoiler(presentation, store, nullptr, hit, changed);
}

bool message_text_presentation_toggle_spoiler(MessageTextPresentation* presentation,
                                              const MessageStore& store, const MessageRichStore* rich_store,
                                              const MessageTextHit& hit, bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!presentation || !changed || !hit.valid || hit.kind != MessageMarkdownSemanticKind::spoiler ||
        message_text_presentation_hit_source(store, rich_store, hit).bytes == 0 ||
        presentation->spoiler_reveals.count == 0 || !presentation->spoiler_reveals.data) {
        return false;
    }
    for (u32 index = 0; index < presentation->spoiler_reveal_count; ++index) {
        const MessageTextSpoilerReveal& reveal = presentation->spoiler_reveals[index];
        if (reveal.message_id == hit.message_id && reveal.message_revision == hit.message_revision &&
            reveal.source_offset == hit.source_offset) {
            presentation->spoiler_reveals[index] =
                presentation->spoiler_reveals[presentation->spoiler_reveal_count - 1];
            --presentation->spoiler_reveal_count;
            *changed = true;
            return true;
        }
    }
    const MessageTextSpoilerReveal reveal{hit.message_id, hit.message_revision, hit.source_offset};
    if (presentation->spoiler_reveal_count < presentation->spoiler_reveals.count) {
        presentation->spoiler_reveals[presentation->spoiler_reveal_count++] = reveal;
    } else {
        const u32 slot = presentation->spoiler_reveal_cursor % presentation->spoiler_reveals.count;
        presentation->spoiler_reveals[slot] = reveal;
        presentation->spoiler_reveal_cursor = (slot + 1) % presentation->spoiler_reveals.count;
    }
    *changed = true;
    return true;
}

} // namespace maicord
