#include "text_presentation_internal.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/ui/text/ansi_text.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/model/chat/game_store.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/ui/shell/navigation_view.hpp"

namespace maicord {
using namespace text_presentation_detail;

namespace text_presentation_detail {
u64 command_content_signature(const MessageTextCommand& command) noexcept {
    u64 hash = hash64_words(kHash64Seed, command.text.data, command.text.bytes);
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

namespace {
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
UiSize previous_measurement(MessageTextPresentation* presentation, u64 identity) noexcept {
    const u32 count = presentation->measurement_history_count;
    u32 index = presentation->measurement_history_cursor < count ? presentation->measurement_history_cursor : 0;
    for (u32 step = 0; step < count; ++step, index = index + 1u == count ? 0u : index + 1u) {
        const u32 offset = presentation->measurement_history_offset + index * 4u;
        const u64 candidate = static_cast<u64>(presentation->byte_style_scratch[offset]) |
                              (static_cast<u64>(presentation->byte_style_scratch[offset + 1u]) << 32u);
        if (candidate != identity)
            continue;
        FloatWord height{}, width{};
        height.word = presentation->byte_style_scratch[offset + 2u];
        width.word = presentation->byte_style_scratch[offset + 3u];
        presentation->measurement_history_cursor = index + 1u;
        return {width.value, height.value};
    }
    return {};
}

} // namespace
bool fail(MessageTextPresentation* presentation, MessageTextPresentationError error) noexcept {
    if (presentation->last_error == MessageTextPresentationError::none) {
        presentation->last_error = error;
    }
    return false;
}

void set_failure_operation(MessageTextPresentation* presentation,
                           MessageTextPresentationOperation operation,
                           UiTextFlowError flow_error) noexcept {
    presentation->failure_operation = operation;
    presentation->failure_flow_error = flow_error;
}

namespace {
// Every emission that can outgrow its source passes a callback; one resolved
// entity adds at most its label plus a sigil and a message link's suffix.
constexpr u32 kMarkdownLabelMargin = 16;

void note_emission(MarkdownParseTrace* trace, u32 bytes) noexcept {
    saturating_accumulate(trace->extra_bytes, bytes);
    trace->largest_emission = maximum(trace->largest_emission, bytes);
}

} // namespace
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

namespace {
Utf8View resolve_entity_name(const BuildContext* context, MessageMarkdownEntityKind kind, u64 id) noexcept {
    if (id == 0) {
        return Utf8View{};
    }
    if (kind == MessageMarkdownEntityKind::game)
        return context->presentation->games ? game_store_name(*context->presentation->games, id) : Utf8View{};
    if (kind == MessageMarkdownEntityKind::user) {
        if (!context->users) {
            return Utf8View{};
        }
        const Utf8View nickname =
            member_nickname(*context->presentation, *context->state, context->message->channel_id, id);
        if (nickname.bytes)
            return nickname;
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

} // namespace
Utf8View resolve_entity(void* opaque, MessageMarkdownEntityKind kind, u64 id) noexcept {
    auto* context = static_cast<BuildContext*>(opaque);
    const Utf8View name = resolve_entity_name(context, kind, id);
    context->trace.entities = true;
    note_emission(&context->trace, name.bytes + kMarkdownLabelMargin);
    return name;
}

namespace {
// The channel icon types: voice and stage lock without Connect,
// forum posts show the post (speech bubble) icon, everything text-like '#'.
MessageMarkdownChipIcon chip_channel_icon(const ChatStateStore& state, const ChannelRecord& channel) noexcept {
    const bool connect = !channel.permissions_resolved || (channel.actions & channel_action_connect);
    switch (channel.type) {
    case 2: return connect ? MessageMarkdownChipIcon::voice : MessageMarkdownChipIcon::locked;
    case 13: return connect ? MessageMarkdownChipIcon::stage : MessageMarkdownChipIcon::locked;
    case 5: return MessageMarkdownChipIcon::announcement;
    case 10:
    case 11:
    case 12: {
        const ChannelRecord* parent = chat_state_store_find_channel(state, channel.parent_id);
        return parent && (parent->type == 15 || parent->type == 16) ? MessageMarkdownChipIcon::message
                                                                    : MessageMarkdownChipIcon::thread;
    }
    case 15: return MessageMarkdownChipIcon::forum;
    case 16: return MessageMarkdownChipIcon::media;
    default: return MessageMarkdownChipIcon::text_channel;
    }
}

} // namespace
// Channel references become chips of what the store knows: the channel, its
// server (named when it is not the message's own) and a forum post's forum.
bool resolve_channel(void* opaque, u64 guild_id, u64 channel_id, MessageMarkdownChannelTarget* target) noexcept {
    auto* context = static_cast<BuildContext*>(opaque);
    const ChatStateStore& state = *context->state;
    context->trace.entities = true;
    const ChannelRecord* channel = chat_state_store_find_channel(state, channel_id);
    if (channel && !channel->occupied)
        channel = nullptr;
    u8 flags = 0;
    if (channel) {
        const u8 type = channel->type;
        const bool direct = type == 1 || type == 3;
        flags |= message_markdown_channel_known;
        if (direct || (!(channel->flags & kChannelObfuscatedFlag) &&
                       (!channel->permissions_resolved || (channel->actions & channel_action_view))))
            flags |= message_markdown_channel_viewable;
        if (type != 4 && (type <= 3 || type == 5 || type == 6 || (type >= 10 && type <= 16)))
            flags |= message_markdown_channel_mentionable;
        if (direct)
            flags |= message_markdown_channel_private;
        guild_id = channel->guild_id;
        target->name = chat_state_store_channel_name(state, *channel);
        target->icon = chip_channel_icon(state, *channel);
        if (target->icon == MessageMarkdownChipIcon::message) {
            flags |= message_markdown_channel_forum_post;
            if (const ChannelRecord* parent = chat_state_store_find_channel(state, channel->parent_id)) {
                flags |= message_markdown_channel_parent_known;
                target->parent_name = chat_state_store_channel_name(state, *parent);
                target->parent_icon = chip_channel_icon(state, *parent);
            }
        }
    }
    const GuildRecord* guild = guild_id ? chat_state_store_find_guild(state, guild_id) : nullptr;
    if (guild && guild->occupied && !guild->unavailable) {
        flags |= message_markdown_channel_guild_known;
        target->guild_id = guild->id;
        target->guild_name = chat_state_store_guild_name(state, *guild);
        target->guild_icon = chat_state_store_guild_icon_hash(*guild).bytes != 0 && guild->icon_variant != 0;
        const ChannelRecord* own = chat_state_store_find_channel(state, context->message->channel_id);
        if (own && own->guild_id == guild->id)
            flags |= message_markdown_channel_same_guild;
    }
    target->flags = flags;
    // Icon cells, margins, the chevron and an ellipsis around three labels.
    constexpr u32 kChipMarkup = 64;
    note_emission(&context->trace,
                  target->name.bytes + target->guild_name.bytes + target->parent_name.bytes + kChipMarkup);
    return flags != 0;
}

TimestampFormatResult format_timestamp(void* opaque, u64 unix_seconds, char style,
                                              Slice<char> output) noexcept {
    auto* context = static_cast<BuildContext*>(opaque);
    const TimestampFormatResult formatted =
        format_timestamp_local(unix_seconds, style, context->now_unix_seconds, output);
    context->trace.timestamps = true;
    note_emission(&context->trace, formatted.bytes);
    return formatted;
}

namespace {
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
constexpr u32 kMarkdownCacheProbes = 8;

u64 markdown_cache_key(Utf8View source, const MessageMarkdownOptions& options, MarkdownCacheMode mode) noexcept {
    const u64 switches = static_cast<u64>(mode) | static_cast<u64>(options.base_size) << 8u |
                         u64{options.no_links} << 16u | u64{options.no_headings} << 17u |
                         u64{options.no_lists} << 18u | u64{options.text_sizes} << 19u |
                         u64{options.profile_bio} << 20u | u64{options.preserve_entities} << 21u |
                         u64{options.spoiler_revealed != nullptr} << 22u | u64{options.has_hover} << 23u |
                         static_cast<u64>(timestamp_hour_cycle()) << 24u |
                         static_cast<u64>(options.base_style) << 32u;
    u64 key = hash64_mix(hash64_words(kHash64Seed, source.data, source.bytes), switches);
    if (options.has_hover)
        key = hash64_mix(hash64_mix(key, options.hovered_source_offset), static_cast<u8>(options.hovered_kind));
    return key ? key : 1u;
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
                          const ConstSlice<MessageMarkdownSemantic>& semantics, const ConstSlice<TextStyleSpan>& spans,
                          const Utf8View& text) noexcept {
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
MessageMarkdownResult markdown_parse_cached(MessageTextPresentation* presentation, BuildContext* context,
                                            MarkdownCacheMode mode, const Utf8View& source, const Slice<char>& text_output,
                                            const Slice<u32>& scratch, const Slice<TextStyleSpan>& span_output,
                                            const MessageMarkdownOptions& options,
                                            const Slice<MessageMarkdownSemantic>& semantic_output) noexcept {
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
            result.timestamps = entry->timestamps;
            result.semantic_count = entry->semantic_count;
            result.next_timestamp_update_unix_seconds = entry->expires_unix_seconds;
            result.list_items = entry->list_items;
            result.syntax_truncated = (entry->flags & markdown_cache_syntax_truncated) != 0;
            result.semantics_truncated = (entry->flags & markdown_cache_semantics_truncated) != 0;
            // Copy a payload the ring is about to reach forward, from the
            // caller buffers it was just written to.
            if (cache->head - (entry->position - 1u) > cache->arena_bytes / 2u)
                markdown_cache_write(cache, entry,
                                     ConstSlice<MessageMarkdownSemantic>{semantic_output.data, entry->semantic_count},
                                     ConstSlice<TextStyleSpan>{span_output.data, entry->span_count}, result.text);
            saturating_increment(presentation->markdown_cache_hits);
            return result;
        }
    }
    reset_value<MarkdownParseTrace>(&context->trace);
    saturating_accumulate(presentation->markdown_parses);
    const MessageMarkdownResult parsed =
        message_markdown_parse(source, text_output, scratch, span_output, options, semantic_output);
    if (!cache || !parsed.ok())
        return parsed;
    const MarkdownParseTrace& trace = context->trace;
    const u64 required = u64{source.bytes} + trace.extra_bytes + trace.largest_emission + 1u;
    if (required > text_capacity || (parsed.semantics_truncated && !full_semantics) || parsed.span_count > 0xFFFFu ||
        parsed.semantic_count > 0xFFu)
        return parsed;
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
    return parsed;
}
} // namespace
// The cached parse, with every link underlined when Accessibility's "Always underline links" is on (the cache keeps
// the parser's own spans).
MessageMarkdownResult cached_markdown_parse(MessageTextPresentation* presentation, BuildContext* context,
                                            MarkdownCacheMode mode, const Utf8View& source, const Slice<char>& text_output,
                                            const Slice<u32>& scratch, const Slice<TextStyleSpan>& span_output,
                                            const MessageMarkdownOptions& options,
                                            const Slice<MessageMarkdownSemantic>& semantic_output) noexcept {
    const MessageMarkdownResult result = markdown_parse_cached(presentation, context, mode, source, text_output,
                                                               scratch, span_output, options, semantic_output);
    if (presentation->underline_links && result.ok())
        for (u32 i = 0; i < result.span_count && i < span_output.count; ++i)
            if (span_output[i].flags & text_style_link)
                span_output[i].flags |= text_style_underline;
    return result;
}

bool append_command(MessageTextPresentation* presentation, const Utf8View& text,
                    const ConstSlice<TextStyleSpan>& styles, const ConstSlice<MessageMarkdownSemantic>& semantics,
                    const UiWidgetReference& owner,
                    const UiRect& rectangle, const UiRect& clip, const Color& color,
                    const MessageRecord* record, u16 measurement_index, const CommandLook& look) noexcept {
    if (text.bytes == 0) {
        return true;
    }
    set_failure_operation(presentation, MessageTextPresentationOperation::append_command);
    if (presentation->command_count >= presentation->commands.count) {
        return fail(presentation, MessageTextPresentationError::capacity);
    }
    const UiWidget* owner_widget = ui_widget_manager_resolve(*owner.manager, owner.handle);
    if (!owner_widget || !ui_rect_has_area(rectangle) || !ui_rect_has_area(clip) || look.minimum_height < 0.0f ||
        look.gap_after < 0.0f) {
        return fail(presentation, MessageTextPresentationError::layout);
    }
    MessageTextCommand& command = presentation->commands[presentation->command_count];
    command = MessageTextCommand{text,
                                 styles,
                                 semantics,
                                 owner,
                                 rectangle,
                                 clip,
                                 color,
                                 kNoRoleColor,
                                 record ? record->id : 0,
                                 0,
                                 record ? record->revision : 0,
                                 look.minimum_height,
                                 look.gap_after,
                                 0.0f,
                                 0.0f,
                                 measurement_index,
                                 look.kind,
                                 look.slot,
                                 look.themed,
                                 look.horizontal,
                                 look.vertical,
                                 look.wrapping,
                                 look.overflow,
                                 look.interface_text,
                                 look.source_id};
    command.measurement_identity = command_measurement_identity(command, owner_widget->id);
    const UiSize measured = previous_measurement(presentation, command.measurement_identity);
    command.measured_height = measured.height;
    command.measured_width = measured.width;
    ++presentation->command_count;
    return true;
}

bool append_command_role(MessageTextPresentation* presentation, const Utf8View& text,
                         const ConstSlice<TextStyleSpan>& styles,
                         const ConstSlice<MessageMarkdownSemantic>& semantics, const UiWidgetReference& owner,
                         const UiRect& rectangle, const UiRect& clip, const Color& color,
                         const MessageRecord* record, u16 measurement_index, const CommandLook& look,
                         const Color& inline_role_color) noexcept {
    const u32 before = presentation->command_count;
    if (!append_command(presentation, text, styles, semantics, owner, rectangle, clip, color, record,
                        measurement_index, look))
        return false;
    if (presentation->command_count != before)
        presentation->commands[before].inline_role_color = inline_role_color;
    return true;
}

bool retain_text(MessageTextPresentation* presentation, const Utf8View& text, Utf8View* output) noexcept {
    if (text.bytes > presentation->utf8.count - presentation->utf8_count)
        return fail(presentation, MessageTextPresentationError::capacity);
    char* at = presentation->utf8.data + presentation->utf8_count;
    copy_bytes(at, text.data, text.bytes);
    presentation->utf8_count += text.bytes;
    *output = {at, text.bytes};
    return true;
}

bool append_marked(MessageTextPresentation* presentation, const Utf8View& text, const TextStyleSpan& span,
                   const MessageMarkdownSemantic* semantic, const UiWidgetReference& owner, const UiRect& rectangle,
                   const UiRect& clip, const Color& color, const MessageRecord* record, u16 measurement_index,
                   const CommandLook& look) noexcept {
    if ((span.byte_count && presentation->style_count >= presentation->styles.count) ||
        (semantic && presentation->semantic_count >= presentation->semantics.count))
        return fail(presentation, MessageTextPresentationError::capacity);
    ConstSlice<TextStyleSpan> styles{};
    ConstSlice<MessageMarkdownSemantic> semantics{};
    if (span.byte_count) {
        TextStyleSpan* retained = presentation->styles.data + presentation->style_count++;
        *retained = span;
        styles = {retained, 1};
    }
    if (semantic) {
        MessageMarkdownSemantic* retained = presentation->semantics.data + presentation->semantic_count++;
        *retained = *semantic;
        semantics = {retained, 1};
    }
    return append_command(presentation, text, styles, semantics, owner, rectangle, clip, color, record,
                          measurement_index, look);
}

bool begin_flow_item_clamped(UiTextFlowLayout* flow, float left_inset, float right_inset,
                             UiRect* rectangle) noexcept {
    if (!ui_extent_valid(left_inset) || !ui_extent_valid(right_inset)) {
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

// Opens the next flow item; a failure records the flow's error.
bool begin_item(MessageTextPresentation* presentation, UiTextFlowLayout* flow, float left_inset,
                float right_inset, UiRect* rectangle) noexcept {
    set_failure_operation(presentation, MessageTextPresentationOperation::begin_flow_item);
    if (begin_flow_item_clamped(flow, left_inset, right_inset, rectangle)) {
        return true;
    }
    presentation->failure_flow_error = flow->last_error;
    return fail(presentation, MessageTextPresentationError::layout);
}

// Commits the command just appended as the flow's next item.
bool commit_last_command(MessageTextPresentation* presentation, UiTextFlowLayout* flow) noexcept {
    const MessageTextCommand& command = presentation->commands[presentation->command_count - 1];
    set_failure_operation(presentation, MessageTextPresentationOperation::commit_flow_item);
    if (!ui_text_flow_commit_item(flow, command.measured_height, command.minimum_height, command.gap_after)) {
        presentation->failure_flow_error = flow->last_error;
        return fail(presentation, MessageTextPresentationError::layout);
    }
    return true;
}

namespace {
// The bounded parse source for a reply preview. SGR escapes inside ```ansi
// fences leave first (the inline reply rules drop them anyway), so a heavily
// coloured message still reaches its closing fence; a fence the cap cuts is
// closed so it stays one inline code run.
u32 reply_preview_source(Utf8View content, char* output, u32 capacity) noexcept {
    u32 used = 0, at = 0;
    bool fence = false, ansi = false;
    while (at < content.bytes) {
        const char* text = content.data + at;
        const u32 left = content.bytes - at;
        if (left >= 3 && text[0] == '`' && text[1] == '`' && text[2] == '`') {
            if (used + 3 > capacity)
                break;
            ansi = !fence && left >= 8 && ascii_lower(text[3]) == 'a' && ascii_lower(text[4]) == 'n' &&
                   ascii_lower(text[5]) == 's' && ascii_lower(text[6]) == 'i' &&
                   (text[7] == '\n' || (text[7] == '\r' && left >= 9 && text[8] == '\n'));
            fence = !fence;
            copy_bytes(output + used, text, 3);
            used += 3;
            at += 3;
            continue;
        }
        if (const u32 escape = ansi ? ansi_text_sgr_bytes(text, left) : 0u) {
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
} // namespace
__declspec(noinline) RetainedReplySummary
retain_reply_summary(MessageTextPresentation* presentation, BuildContext* context, Utf8View author,
                     Utf8View content, bool available, u8 content_flags, u8 message_type,
                     Utf8View poll_question, u8 quote_flags, u64 sent_unix_seconds) noexcept {

    // The reply preview: the referenced text through the inline reply rules
    // (code fences inline, ```ansi escapes dropped, no block quotes), every
    // line break shown as a space, on one visual line. Without text it shows
    // the poll question or an italic placeholder.
    constexpr u32 maximum_source_bytes = 2'048;
    constexpr u32 maximum_display_bytes = 2'560;
    constexpr u32 maximum_styles = 128;
    // The byte styles and both text buffers live in the presentation's byte-style
    // scratch below the measurement history (as parse_content's do), written
    // before they are read; only the span and chip records stay on the stack.
    constexpr u32 scratch_words = maximum_display_bytes + (maximum_source_bytes + 4 + maximum_display_bytes + 3) / 4;
    if (presentation->measurement_history_offset < scratch_words)
        return RetainedReplySummary{};
    u32* const byte_styles = presentation->byte_style_scratch.data;
    char* const source = reinterpret_cast<char*>(byte_styles + maximum_display_bytes);
    char* const transformed = source + maximum_source_bytes + 4;
    TextStyleSpan transformed_styles[maximum_styles];
    // Reply rules keep channel, message and attachment chips;
    // only their icon cells are kept, so the line stays one jump target.
    MessageMarkdownSemantic chip_semantics[64];

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
        options.resolve_channel = resolve_channel;
        parsed = cached_markdown_parse(
            presentation, context, MarkdownCacheMode::reply, Utf8View{source, source_bytes},
            Slice<char>{transformed, maximum_display_bytes},
            Slice<u32>{byte_styles, maximum_display_bytes},
            Slice<TextStyleSpan>{transformed_styles, maximum_styles}, options,
            Slice<MessageMarkdownSemantic>{chip_semantics, 64});
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
    // A reply that pings its quoted author names them "@Author".
    const u32 at_sign = author.bytes != 0 && (content_flags & gateway_reference_mentions_author) ? 1u : 0u;
    const u32 author_bytes = author.bytes + at_sign;
    // Reply timestamps: the quoted message's sent time follows its author. It is formatted into the byte
    // styles, which the parse no longer needs, so the frame stays under the 4 KiB that takes a stack probe.
    char* const time_text = reinterpret_cast<char*>(byte_styles);
    static_assert(kMessageMarkdownTimestampMaximumBytes <= maximum_display_bytes * sizeof(u32));
    u32 time_bytes = 0;
    if (sent_unix_seconds != 0 && author.bytes != 0) {
        const TimestampFormatResult sent =
            format_timestamp_calendar(sent_unix_seconds, context->now_unix_seconds,
                                      Slice<char>{time_text, kMessageMarkdownTimestampMaximumBytes});
        time_bytes = sent.ok ? sent.bytes : 0u;
    }
    const u32 time_gap = time_bytes != 0 ? 1u + time_bytes : 0u;
    const u32 separator = author.bytes != 0 && display_content.bytes != 0 ? 1u : 0u;
    // A quoted forward leads its text with the forward icon (a 1em cell and
    // a 4 px margin, like a chip icon).
    constexpr Utf8View forward_cell = utf8_literal("\xE2\x80\x83\xE2\x80\x85\xE2\x80\x8D");
    const u32 forward_bytes =
        (quote_flags & gateway_quote_forwarded) && display_content.bytes != 0 ? forward_cell.bytes : 0u;
    const u32 retained_style_count =
        (author.bytes != 0 ? 1u : 0u) + (time_bytes != 0 ? 1u : 0u) + display_style_count +
        (placeholder.bytes ? 1u : 0u);
    if (author_bytes + time_gap > presentation->utf8.count - presentation->utf8_count ||
        separator + forward_bytes + display_content.bytes >
            presentation->utf8.count - presentation->utf8_count - author_bytes - time_gap ||
        retained_style_count > presentation->styles.count - presentation->style_count) {
        return RetainedReplySummary{};
    }

    const u32 text_begin = presentation->utf8_count;
    const u32 style_begin = presentation->style_count;
    if (at_sign)
        presentation->utf8.data[presentation->utf8_count++] = '@';
    copy_bytes(presentation->utf8.data + presentation->utf8_count, author.data, author.bytes);
    presentation->utf8_count += author.bytes;
    if (author.bytes != 0) {
        presentation->styles[presentation->style_count++] =
            TextStyleSpan{0, author_bytes, text_style_bold | text_style_inline_role_color};
    }
    if (time_bytes != 0) {
        presentation->utf8.data[presentation->utf8_count++] = ' ';
        copy_bytes(presentation->utf8.data + presentation->utf8_count, time_text, time_bytes);
        presentation->utf8_count += time_bytes;
        presentation->styles[presentation->style_count++] =
            TextStyleSpan{author_bytes + 1u, time_bytes, text_style_subtext};
    }
    if (separator != 0) {
        presentation->utf8.data[presentation->utf8_count++] = ' ';
    }
    copy_bytes(presentation->utf8.data + presentation->utf8_count, forward_cell.data, forward_bytes);
    presentation->utf8_count += forward_bytes;
    char* content_out = presentation->utf8.data + presentation->utf8_count;
    copy_bytes(content_out, display_content.data, display_content.bytes);
    // Newlines inside text, inline code and emphasis render as spaces.
    for (u32 index = 0; index < display_content.bytes; ++index)
        if (content_out[index] == '\n' || content_out[index] == '\r')
            content_out[index] = ' ';
    presentation->utf8_count += display_content.bytes;
    const u32 content_offset = author_bytes + time_gap + separator + forward_bytes;
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
    // The forward icon and chip icon cells, in the line's coordinates.
    const u32 semantic_begin = presentation->semantic_count;
    if (forward_bytes && presentation->semantic_count < presentation->semantics.count)
        presentation->semantics[presentation->semantic_count++] =
            MessageMarkdownSemantic{static_cast<u64>(MessageMarkdownChipIcon::forward),
                                    author_bytes + time_gap + separator, 3,
                                    0, 0, MessageMarkdownSemanticKind::chip_icon, 0};
    for (u32 index = 0; parsed_markdown && parsed.ok() && !placeholder.bytes && index < parsed.semantic_count &&
                        presentation->semantic_count < presentation->semantics.count;
         ++index) {
        MessageMarkdownSemantic semantic = chip_semantics[index];
        if (semantic.kind != MessageMarkdownSemanticKind::chip_icon &&
            semantic.kind != MessageMarkdownSemanticKind::guild_icon &&
            semantic.kind != MessageMarkdownSemanticKind::game_icon)
            continue;
        semantic.byte_offset += content_offset;
        presentation->semantics[presentation->semantic_count++] = semantic;
    }
    if (parsed_markdown && parsed.ok()) {
        saturating_accumulate(presentation->markdown_transformations, parsed.transformations);
        saturating_accumulate(presentation->markdown_spans_emitted, parsed.span_count);
        saturating_accumulate(presentation->markdown_resolved_entities, parsed.resolved_entities);
        saturating_accumulate(presentation->markdown_timestamps, parsed.timestamps);
    } else if (parsed_markdown) {
        saturating_accumulate(presentation->markdown_rejected);
    }
    return RetainedReplySummary{
        Utf8View{presentation->utf8.data + text_begin, presentation->utf8_count - text_begin},
        ConstSlice<TextStyleSpan>{presentation->styles.data + style_begin,
                                  presentation->style_count - style_begin},
        ConstSlice<MessageMarkdownSemantic>{presentation->semantics.data + semantic_begin,
                                            presentation->semantic_count - semantic_begin}};
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

// A quoted system notice (a join line, an app subscription, a closed poll):
// plain text whose highlighted part (the poll's name) is bold.
RetainedReplySummary retain_reply_plain(MessageTextPresentation* presentation, const MessageSystemText& system) noexcept {
    const Utf8View text = system.text;
    const auto highlighted = [&](u32 i) noexcept {
        const auto& span = system.spans[i];
        return (span.kind == MessageSystemSpanKind::emphasis || span.kind == MessageSystemSpanKind::message) &&
               span.offset <= text.bytes && span.bytes <= text.bytes - span.offset;
    };
    u32 count = 0;
    for (u32 i = 0; i < system.span_count && i < kMessageSystemSpanMaximum; ++i)
        count += highlighted(i) ? 1u : 0u;
    if (!text.bytes || text.bytes > presentation->utf8.count - presentation->utf8_count ||
        count > presentation->styles.count - presentation->style_count)
        return {};
    char* out = presentation->utf8.data + presentation->utf8_count;
    copy_bytes(out, text.data, text.bytes);
    presentation->utf8_count += text.bytes;
    auto* styles = presentation->styles.data + presentation->style_count;
    count = 0;
    for (u32 i = 0; i < system.span_count && i < kMessageSystemSpanMaximum; ++i)
        if (highlighted(i))
            styles[count++] = TextStyleSpan{system.spans[i].offset, system.spans[i].bytes, text_style_bold};
    presentation->style_count += count;
    return {{out, text.bytes}, {styles, count}};
}

// A reply that points into a clip: "<author> <title> @1:23", the title bold
// ("Untitled" without one), the moment in link colour. content is the decoded
// "<title>\x1f@<time>" of the quote.
RetainedReplySummary retain_reply_media_mention(MessageTextPresentation* presentation, Utf8View author,
                                                bool at_sign, Utf8View content) noexcept {
    u32 split = 0;
    while (split < content.bytes && content.data[split] != '\x1f')
        ++split;
    if (split >= content.bytes || !author.bytes)
        return {};
    Utf8View title{content.data, split};
    const Utf8View stamp{content.data + split + 1, content.bytes - split - 1};
    if (!title.bytes)
        title = utf8_literal("Untitled");
    const u32 author_bytes = author.bytes + (at_sign ? 1u : 0u);
    const u32 bytes = author_bytes + 1 + title.bytes + 1 + stamp.bytes;
    if (!stamp.bytes || bytes > presentation->utf8.count - presentation->utf8_count ||
        presentation->styles.count - presentation->style_count < 3)
        return {};
    char* out = presentation->utf8.data + presentation->utf8_count;
    u32 at = 0;
    if (at_sign)
        out[at++] = '@';
    copy_bytes(out + at, author.data, author.bytes);
    at += author.bytes;
    out[at++] = ' ';
    const u32 title_at = at;
    copy_bytes(out + at, title.data, title.bytes);
    at += title.bytes;
    out[at++] = ' ';
    const u32 stamp_at = at;
    copy_bytes(out + at, stamp.data, stamp.bytes);
    presentation->utf8_count += bytes;
    auto* styles = presentation->styles.data + presentation->style_count;
    styles[0] = {0, author_bytes, text_style_bold | text_style_inline_role_color};
    styles[1] = {title_at, title.bytes, text_style_bold};
    styles[2] = {stamp_at, stamp.bytes, text_style_link};
    presentation->style_count += 3;
    return {{out, bytes}, {styles, 3}};
}

// "<user> used <command>": the spans name the user and the command so the
// header layout (message_interaction_layout) can place the avatar-line pieces
// itself; a slash command is a chip (no leading slash), any other command is
// plain link-coloured text.
RetainedReplySummary retain_interaction_header(MessageTextPresentation* presentation, Utf8View user,
                                               Utf8View command, bool chip) noexcept {
    const auto separator = utf8_literal(" used ");
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
    styles[1] = {user.bytes + separator.bytes, command.bytes, chip ? text_style_mention : text_style_link};
    presentation->utf8_count += bytes;
    presentation->style_count += 2;
    return {{text, bytes}, {styles, 2}};
}

bool append_reply_line(MessageTextPresentation* presentation, const Utf8View& text,
                       const ConstSlice<TextStyleSpan>& styles, UiTextFlowLayout* flow, const Color& color,
                       const Color& inline_role_color, const UiWidgetReference& owner, const MessageRecord& record,
                       u16 measurement_index, TextOverflow overflow,
                       float right_inset, float left_inset,
                       ConstSlice<MessageMarkdownSemantic> semantics, float line_height) noexcept {
    UiRect rectangle{};
    // Reserve the leading 16-DIP referenced-author avatar plus a 4-DIP gap,
    // and room for a trailing 20-DIP icon (4-DIP gap) when one follows.
    if (!begin_item(presentation, flow, left_inset, right_inset, &rectangle)) {
        return false;
    }
    // Bound shaping to one line (the box of one quoted line); the renderer applies character-granularity
    // ellipsis inside this retained width.
    rectangle.bottom = rectangle.top + line_height;
    if (!append_command_role(presentation, text, styles, semantics, owner, rectangle,
                             flow->clip_rectangle, color, &record, measurement_index,
                             {.kind = MessageTextMeasurementKind::reply,
                              .minimum_height = 16.0f,
                              .gap_after = 6.0f,
                              .themed = styles.count != 0,
                              .wrapping = TextWrapping::no_wrap,
                              .overflow = overflow},
                             inline_role_color)) {
        return false;
    }
    return commit_last_command(presentation, flow);
}

bool initialize_flow(MessageTextPresentation* presentation, const UiLayoutNode& region, float text_left,
                     float text_width, const UiRect& clip, UiTextFlowLayout* flow) noexcept {
    set_failure_operation(presentation, MessageTextPresentationOperation::initialize_flow);
    const UiRect content{text_left, region.content_rectangle.top, text_left + text_width,
                         region.content_rectangle.bottom};
    if (ui_text_flow_initialize(flow, content, clip)) {
        return true;
    }
    presentation->failure_flow_error = flow->last_error;
    return fail(presentation, MessageTextPresentationError::layout);
}

bool finish_flow(MessageTextPresentation* presentation, const UiTextFlowLayout& flow,
                 float* height) noexcept {
    set_failure_operation(presentation, MessageTextPresentationOperation::finish_flow, flow.last_error);
    if (!flow.active || flow.item_open || flow.last_error != UiTextFlowError::none) {
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

} // namespace text_presentation_detail
} // namespace maicord
