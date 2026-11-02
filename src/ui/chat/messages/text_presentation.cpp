#include "maicord/ui/text/flow_layout.hpp"
#include "maicord/ui/chat/polls/presentation.hpp"

#include "maicord/ui/chat/messages/system_text.hpp"
#include "maicord/model/guild/scheduled_event_store.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/ui/shell/navigation_view.hpp"
#include "text_presentation_internal.hpp"

namespace maicord {
using namespace text_presentation_detail;
namespace {
MessageTextPresentationResult result(const MessageTextPresentation& presentation,
                                     MessageTextPresentationError error) noexcept {
    return MessageTextPresentationResult{presentation.command_count, presentation.measurement_count, error};
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
}

void clear_failure_context(MessageTextPresentation* presentation) noexcept {
    presentation->failure_message_id = 0;
    presentation->failure_source_index = ~0u;
    presentation->failure_item_index = kInvalidMessageTextMeasurement;
    presentation->failure_stage = MessageTextPresentationStage::none;
    presentation->failure_operation = MessageTextPresentationOperation::none;
    presentation->failure_flow_error = UiTextFlowError::none;
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
                                          const Slice<MessageTextCommand>& commands, const Slice<char>& utf8,
                                          const Slice<TextStyleSpan>& styles,
                                          const Slice<MessageMarkdownSemantic>& semantics,
                                          const Slice<MessageTextMeasurement>& measurements,
                                          const Slice<MessageTextSpoilerReveal>& spoiler_reveals,
                                          const Slice<u32>& byte_style_scratch) noexcept {
    if (!presentation || !commands.data || commands.count == 0 || !utf8.data || utf8.count == 0 ||
        !styles.data || styles.count == 0 || !semantics.data || semantics.count == 0 ||
        !measurements.data || measurements.count == 0 || measurements.count > 0xFFFFu ||
        !spoiler_reveals.data || spoiler_reveals.count == 0 || !byte_style_scratch.data ||
        byte_style_scratch.count == 0) {
        return false;
    }
    reset_value<MessageTextPresentation>(presentation);
    presentation->commands = commands;
    presentation->utf8 = utf8;
    presentation->styles = styles;
    presentation->semantics = semantics;
    presentation->measurements = measurements;
    presentation->spoiler_reveals = spoiler_reveals;
    presentation->byte_style_scratch = byte_style_scratch;
    clear_failure_context(presentation);
    return true;
}

bool message_markdown_cache_initialize(MessageMarkdownCache* cache, Slice<MessageMarkdownCacheEntry> entries,
                                       Slice<u8> arena) noexcept {
    if (!cache || !entries.data || entries.count == 0 || (entries.count & (entries.count - 1u)) != 0 ||
        !arena.data || arena.count < 64 || arena.count % 8u != 0 || reinterpret_cast<usize>(arena.data) % 8u != 0)
        return false;
    reset_value<MessageMarkdownCache>(cache);
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
    if (presentation)
        clear_failure_context(presentation);
    if (!presentation || !presentation->commands.data || !presentation->utf8.data ||
        !presentation->styles.data || !presentation->semantics.data || !presentation->measurements.data ||
        !presentation->spoiler_reveals.data || !presentation->byte_style_scratch.data || !scene.active ||
        !scene.timeline || !scene.timeline->store || scene.count > scene.rows.count ||
        !ui_rect_has_area(scene.viewport) || navigation.channel_count > navigation.channel_capacity) {
        reject_atomically(presentation, MessageTextPresentationError::invalid_input);
        return presentation
                   ? result(*presentation, presentation->last_error)
                   : MessageTextPresentationResult{0, 0, MessageTextPresentationError::invalid_input};
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
    reset_value<MessageTextRichMetrics>(&presentation->rich);
    presentation->next_timestamp_update_unix_seconds = 0;
    presentation->status_dots = false;
    presentation->projection_signature = 0;
    presentation->shift_layout_passes = 0;
    presentation->scroll_shift_dips = 0.0f;
    presentation->scroll_moving = false;
    presentation->last_error = MessageTextPresentationError::none;
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
    clear_failure_context(presentation);
    presentation->projection_signature = message_text_presentation_projection_signature(*presentation);
    presentation->shift_layout_passes = scene.layout_passes;
    saturating_accumulate(presentation->builds);
    return result(*presentation, MessageTextPresentationError::none);
}

void message_text_presentation_shift(MessageTextPresentation* presentation, float shift) noexcept {
    for (u32 index = 0; index < presentation->command_count; ++index) {
        MessageTextCommand& command = presentation->commands[index];
        command.rectangle.top += shift;
        command.rectangle.bottom += shift;
        command.clip.top += shift;
        command.clip.bottom += shift;
    }
    presentation->scroll_shift_dips += shift;
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
        if (presentation)
            presentation->last_error = MessageTextPresentationError::measurement;
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
            return false;
        }
    }
    for (u32 index = 0; index < presentation->command_count; ++index) {
        const MessageTextCommand& command = presentation->commands[index];
        const UiTextMeasureRequest request{
            UiTextMeasureKey{
                command.message_id,
                command.measurement_identity ? command.measurement_identity : command_content_signature(command),
                command.message_revision,
                static_cast<u16>(command.measurement_identity ^ (command.measurement_identity >> 16u) ^
                                 (command.measurement_identity >> 32u) ^
                                 (command.measurement_identity >> 48u)),
                backend_revision},
            command.text, command.styles,
            UiSize{ui_rect_width(command.rectangle), ui_rect_height(command.rectangle)},
            command.wrapping == TextWrapping::no_wrap};
        UiTextMeasureValue value{};
        if (!ui_text_measure_cache_measure(cache, request, &value).ok()) {
            presentation->last_error = MessageTextPresentationError::measurement;
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
            // Quote and code runs split the body into stacked commands; a
            // leading code block's box starts its inset above the text.
            const bool leading_box = measurement.content_height == 0.0f && message_text_command_code_block(command);
            measurement.content_height +=
                measured_height.value + command.gap_after + (leading_box ? kMessageCodeBlockInset : 0.0f);
        } else if (command.measurement_kind == MessageTextMeasurementKind::reaction &&
                   command.measurement_slot < kGatewayMessageReactionMaximum) {
            measurement.reaction_widths[command.measurement_slot] = measured_width.value;
        }
    }
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
    // Emoji and flags restyle nothing on hover, so they count as no hover and
    // moving over them rebuilds no text.
    const bool has_hover = hit.valid && hit.kind != MessageMarkdownSemanticKind::none &&
                           hit.kind != MessageMarkdownSemanticKind::custom_emoji &&
                           hit.kind != MessageMarkdownSemanticKind::regional_flag;
    *changed = presentation->has_hover != has_hover ||
               (has_hover && (presentation->hovered_message_id != hit.message_id ||
                              presentation->hovered_source_offset != hit.source_offset ||
                              presentation->hovered_kind != hit.kind));
    if (*changed)
        presentation->shift_layout_passes = 0;
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
        const auto* decoration = ui_plugin_render(record->id, record->revision);
        return decoration && decoration->replace_content ? decoration->content : Utf8View{};
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
    const u16 source = command.source_id & ~(kRichTextSource | kForwardedTextSource);
    if (source >= kRichTextEmbedBegin && source < kRichTextFieldBegin &&
        (source - kRichTextEmbedBegin) % kRichTextEmbedSlots == 6) {
        const u32 index = (source - kRichTextEmbedBegin) / kRichTextEmbedSlots;
        if (index < rich->embed_count)
            return message_rich_embed_provider_label(*rich_store, rich->embeds[index]);
    }
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
        // Custom emoji and link chips (message/channel references, attachment
        // URLs) copy their source when fully selected; generated rows have none.
        const bool chip = semantic.source_bytes != 0 &&
                          (semantic.kind == MessageMarkdownSemanticKind::message_link ||
                           semantic.kind == MessageMarkdownSemanticKind::channel_mention ||
                           (semantic.kind == MessageMarkdownSemanticKind::link &&
                            (semantic.reserved & kMessageMarkdownAttachmentChip)));
        if ((semantic.kind != MessageMarkdownSemanticKind::custom_emoji && !chip) || semantic.byte_count == 0 ||
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
            presentation->shift_layout_passes = 0;
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
    presentation->shift_layout_passes = 0;
    *changed = true;
    return true;
}

} // namespace maicord
