#include "maicord/core/memory.hpp"
#include "maicord/ui/text/regional_flag.hpp"
#include "scene_measurements.hpp"
#include "maicord/model/chat/message_poll_store.hpp"
#include "maicord/ui/chat/polls/controls.hpp"
#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/ui/text/measure_cache.hpp"
#include "maicord/ui/theme/styles.hpp"
#include "maicord/ui/chat/messages/unread_divider.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "scene_internal.hpp"

namespace maicord {
using namespace message_scene_detail;
namespace {
inline constexpr u32 kMessageRootWidgetId = 1;
MessageSceneResult scene_error(MessageScene* scene, MessageSceneError error) noexcept {
    saturating_increment_if(scene, scene->rejected);
    return MessageSceneResult{error, scene ? scene->first : 0, scene ? scene->count : 0, false};
}

void invalidate_geometry(MessageScene* scene) noexcept {
    scene->prepared_signature = 0;
    scene->dirty = true;
    message_scene_detail::clear_measurements(scene);
    for (u32 index = 0; index < scene->count; ++index) {
        auto& row = scene->rows[index];
        row.text_measured = false;
        row.rich_layout_measured = false;
        row.measured_width = -1;
        row.poll_content_revision = 0;
    }
    for (u32 index = 0; index < scene->snapshot_rows.count; ++index) {
        auto& row = scene->snapshot_rows[index];
        row.text_measured = false;
        row.rich_layout_measured = false;
        row.measured_width = -1;
    }
}
} // namespace

bool message_scene_bind_theme_styles(MessageScene* scene, const UiResolvedThemeStyles* styles) noexcept {
    if (!scene)
        return false;
    const u32 revision = styles && styles->ready ? styles->geometry_revision : 0;
    if (scene->theme_styles == styles && scene->theme_geometry_revision == revision)
        return false;
    scene->theme_styles = styles;
    scene->theme_geometry_revision = revision;
    invalidate_geometry(scene);
    return true;
}

void message_scene_invalidate_geometry(MessageScene* scene) noexcept {
    if (scene)
        invalidate_geometry(scene);
}

MessageMediaPolicyResult message_scene_media_policy(const MessageScene& scene,
                                                    const MessageMediaOwner& owner) noexcept {
    return scene.media_policy.query ? scene.media_policy.query(scene.media_policy.context, owner)
                                    : MessageMediaPolicyResult{MessageExplicitFilter::show, true};
}

bool message_scene_set_media_policy(MessageScene* scene, const MessageMediaPolicy& policy) noexcept {
    if (!scene ||
        (scene->media_policy.context == policy.context && scene->media_policy.query == policy.query &&
         scene->media_policy.revision == policy.revision))
        return false;
    scene->media_policy = policy;
    invalidate_geometry(scene);
    return true;
}

MessageMediaObscure message_scene_media_obscure(const MessageScene& scene, const MessageMediaOwner& owner,
                                                u64 message_id, u64 reveal_key, bool spoiler,
                                                bool explicit_media) noexcept {
    return message_scene_media_obscure(scene, message_scene_media_policy(scene, owner), message_id,
                                       reveal_key, spoiler, explicit_media);
}

MessageMediaObscure message_scene_media_obscure(const MessageScene& scene, MessageMediaPolicyResult policy,
                                                u64 message_id, u64 reveal_key, bool spoiler,
                                                bool explicit_media) noexcept {
    const MessageMediaObscure obscure = message_media_obscure(
        scene.media_reveals, reveal_key, spoiler, explicit_media, policy.filter,
        message_rich_display().spoilers == MessageSpoilerReveal::always, policy.explicit_reveal_allowed);
    if (obscure == MessageMediaObscure::blocked || !message_id || !scene.plugin_decorations ||
        !g_ui_plugin_render_query || !scene.timeline || !scene.timeline->store)
        return obscure;
    // Plugin media verdicts: hide replaces the preview like blocked media,
    // blur obscures it like explicit content until clicked.
    const MessageStoreResult found = message_store_find(*scene.timeline->store, message_id);
    const MessageRecord* record =
        found.ok() ? message_store_resolve(*scene.timeline->store, found.handle) : nullptr;
    UiPluginRenderDecoration decoration{};
    if (!record || !message_scene_plugin_decoration(scene, record->id, record->revision, &decoration))
        return obscure;
    if (decoration.media == ui_plugin_media_hide)
        return MessageMediaObscure::plugin_hidden;
    if (decoration.media == ui_plugin_media_blur && obscure == MessageMediaObscure::none &&
        !message_media_revealed(scene.media_reveals, reveal_key))
        return MessageMediaObscure::explicit_content;
    return obscure;
}

bool message_scene_reveal_media(MessageScene* scene, u64 reveal_key) noexcept {
    if (!scene || !message_media_reveal(&scene->media_reveals, reveal_key))
        return false;
    // Blocked notices and blur labels can differ in height from previews.
    invalidate_geometry(scene);
    return true;
}

bool message_scene_set_interaction(MessageScene* scene, const MessageSceneInteraction& interaction) noexcept {
    if (!scene)
        return false;
    const auto& current = scene->interaction;
    if (current.message_id == interaction.message_id && current.component_index == interaction.component_index &&
        current.pending == interaction.pending && current.failed == interaction.failed)
        return false;
    const bool notice_changed = current.failed != interaction.failed ||
                                (interaction.failed && current.message_id != interaction.message_id);
    scene->interaction = interaction;
    if (notice_changed)
        invalidate_geometry(scene);
    else
        scene->dirty = true;
    return true;
}

u32 message_scene_invite_codes(const MessageScene& scene, const MessageRecord& record,
                               Slice<InviteLink> output) noexcept {
    if (!scene.invite_cards || !scene.timeline || !scene.timeline->store || !output.data ||
        record.content_availability != MessageContentAvailability::available || record.content_bytes < 12 ||
        message_type_is_system(record.type) || plugin_folded(scene, record))
        return 0;
    const MessageStoreResult found = message_store_find(*scene.timeline->store, record.id);
    if (!found.ok())
        return 0;
    const Utf8View content = message_store_content(*scene.timeline->store, found.handle);
    return message_markdown_find_invites(
        content, {output.data, minimum<u32>(output.count, kMessageInviteCardMaximum)});
}

bool message_scene_set_content_gated(MessageScene* scene, bool gated) noexcept {
    if (!scene || scene->content_gated == gated)
        return false;
    scene->content_gated = gated;
    scene->dirty = true;
    return true;
}

bool message_scene_bind_invite_cards(MessageScene* scene, const InviteCardCache* cache) noexcept {
    if (!scene || scene->invite_cards == cache)
        return false;
    scene->invite_cards = cache;
    invalidate_geometry(scene);
    return true;
}

bool message_scene_bind_scheduled_events(MessageScene* scene, const ScheduledEventStore* events) noexcept {
    if (!scene || scene->scheduled_events == events)
        return false;
    scene->scheduled_events = events;
    return true;
}

bool message_scene_set_display(MessageScene* scene, MessageSceneDisplay value) noexcept {
    if (!scene || value.group_spacing >= 6 ||
        (scene->display.compact == value.compact && scene->display.group_spacing == value.group_spacing &&
         scene->display.clock == value.clock))
        return false;
    scene->display = value;
    invalidate_geometry(scene);
    return true;
}

float message_scene_reply_indent(const MessageScene& scene) noexcept {
    return scene.display.compact ? message_em(scene) * kMessageSceneCompactReplyIndentEm : 0.0f;
}

bool message_scene_bind_poll_source(MessageScene* scene, const MessageScenePollBinding* binding) noexcept {
    if (!scene || (binding && (!binding->measure || !binding->backend_revision)))
        return false;
    if (scene->polls == binding)
        return true;
    scene->polls = binding;
    scene->prepared_signature = 0;
    scene->dirty = true;
    return true;
}

bool message_scene_poll_projection(const MessageScene& scene, const MessageSceneRowWidget& row,
                                   const Theme& theme, Slice<UiPollAnswer> answers,
                                   UiPollProjection* out) noexcept {
    if (!out || !scene.active || row.forwarded_snapshot || row.redacted || !scene.polls ||
        !scene.timeline || !scene.timeline->store || !ui_widget_manager_resolve(scene.widgets, row.poll))
        return false;
    const auto* record = message_store_resolve(*scene.timeline->store, row.message);
    const auto* node = message_scene_layout_node(scene, row.poll);
    const auto* poll = record ? row_poll(scene, *record) : nullptr;
    UiPollState state{};
    if (!record || record->id != row.message_id || record->revision != row.message_revision || !poll ||
        row.poll_content_revision != poll->content_revision ||
        row.poll_result_revision != poll->result_revision || !node ||
        !poll_state(scene, *record, row.message, answers, &state))
        return false;
    UiPollStyle style;
    ui_theme_fill_poll_style(scene.theme_styles, &style);
    return ui_poll_controls_project(state, node->content_rectangle, style,
                                    theme, scene.polls->measure, scene.polls->backend_revision, out);
}

bool message_scene_initialize_owned(MessageScene* scene, MessageTimeline* timeline, u32 maximum_rows,
                                    u32 initial_rows, Slice<UiWidget> widget_storage,
                                    Slice<UiLayoutNode> layout_storage, bool defer_measurements) noexcept {
    if (!scene || scene->owned_rows.arena.base || !timeline || !timeline->store || !timeline->list ||
        !widget_storage.data || !layout_storage.data || widget_storage.count == 0 ||
        layout_storage.count == 0 || maximum_rows == 0 || initial_rows == 0 || initial_rows > maximum_rows) {
        return false;
    }
    StableStorage owned_rows{};
    if (maximum_rows > kInvalidUiWidgetSlot || maximum_rows > ~0u / 2u ||
        !stable_storage_initialize(&owned_rows, 2u * maximum_rows, initial_rows,
                                   sizeof(MessageSceneRowWidget), alignof(MessageSceneRowWidget), false)) {
        return false;
    }
    reset_value<MessageScene>(scene);
    scene->rows = Slice<MessageSceneRowWidget>{stable_storage_data<MessageSceneRowWidget>(owned_rows),
                                               owned_rows.capacity};
    scene->timeline = timeline;
    scene->root = ui_widget_invalid_handle();
    scene->unread_divider_style = kDefaultUiUnreadDividerStyle;
    scene->unread_prepared_height = kDefaultUiUnreadDividerStyle.height;
    if (!ui_widget_manager_initialize(&scene->widgets, widget_storage, layout_storage)) {
        stable_storage_release(&owned_rows);
        return false;
    }
    scene->owned_rows = owned_rows;
    scene->owns_rows = true;
    scene->snapshot_storage_offset = maximum_rows;
    if (!message_scene_detail::initialize_measurements(scene, defer_measurements)) {
        message_scene_release(scene);
        return false;
    }
    return true;
}

bool message_scene_reserve_measurements(MessageScene* scene) noexcept {
    return scene && scene->owned_measurements.arena.base && scene->timeline && scene->timeline->store &&
           stable_storage_reserve(&scene->owned_measurements, scene->timeline->store->capacity);
}

void message_scene_release(MessageScene* scene) noexcept {
    if (!scene) {
        return;
    }
    if (scene->owns_rows) {
        stable_storage_release(&scene->owned_rows);
    }
    stable_storage_release(&scene->owned_measurements);
    stable_storage_release(&scene->owned_item_layouts);
    reset_value<MessageScene>(scene);
}

bool message_scene_reserve_rows(MessageScene* scene, u32 required_rows) noexcept {
    if (!scene || required_rows == 0) {
        return false;
    }
    if (required_rows <= scene->rows.count) {
        return true;
    }
    if (!scene->owns_rows || required_rows > scene->snapshot_storage_offset ||
        !stable_storage_grow(&scene->owned_rows, required_rows)) {
        return false;
    }
    scene->rows =
        Slice<MessageSceneRowWidget>{stable_storage_data<MessageSceneRowWidget>(scene->owned_rows),
                                     minimum(scene->snapshot_storage_offset, scene->owned_rows.capacity)};
    return true;
}

bool message_scene_set_highlight(MessageScene* scene, u64 message_id, float level) noexcept {
    if (!scene)
        return false;
    if (!message_id || level <= 0.0f) {
        message_id = 0;
        level = 0.0f;
    }
    level = minimum(level, 1.0f);
    if (scene->highlight_message_id == message_id && scene->highlight_level == level)
        return false;
    scene->highlight_message_id = message_id;
    scene->highlight_level = level;
    saturating_increment(scene->highlight_revision);
    return true;
}

bool message_scene_set_unread_cursor(MessageScene* scene, u64 channel_id, u64 last_read_message_id,
                                     bool known, bool* changed,
                                     const UiUnreadDividerStyle* style) noexcept {
    if (changed)
        *changed = false;
    if (!scene || !scene->timeline || !scene->timeline->store || (known && !channel_id) ||
        (style && !ui_unread_divider_style_valid(*style)))
        return false;
    const auto next_style = style ? *style : scene->unread_divider_style;
    const bool style_changed =
        next_style.height != scene->unread_divider_style.height ||
        next_style.horizontal_inset != scene->unread_divider_style.horizontal_inset ||
        next_style.label_width != scene->unread_divider_style.label_width ||
        next_style.label_gap != scene->unread_divider_style.label_gap ||
        next_style.line_thickness != scene->unread_divider_style.line_thickness;
    if (!known) {
        channel_id = 0;
        last_read_message_id = 0;
    }
    const bool updated = style_changed || scene->unread_cursor_known != known ||
                         scene->unread_channel_id != channel_id ||
                         scene->unread_last_read_message_id != last_read_message_id;
    if (!updated)
        return true;
    scene->unread_cursor_known = known;
    scene->unread_channel_id = channel_id;
    scene->unread_last_read_message_id = last_read_message_id;
    scene->unread_divider_style = next_style;
    saturating_increment(scene->unread_revision);
    scene->dirty = true;
    if (changed)
        *changed = true;
    return true;
}

bool message_scene_set_result_card_style(MessageScene* scene, const MessageResultCardStyle* style) noexcept {
    if (!scene || !scene->timeline || !scene->owns_rows ||
        (style && !message_result_card_style_valid(*style)))
        return false;
    const auto next = style ? *style : MessageResultCardStyle{};
    if (scene->result_cards == (style != nullptr) &&
        scene->result_card_style.heading_height == next.heading_height &&
        scene->result_card_style.action_height == next.action_height &&
        scene->result_card_style.gap == next.gap &&
        scene->result_card_style.materialized_rows == next.materialized_rows)
        return true;
    scene->result_card_style = next;
    scene->result_cards = style != nullptr;
    saturating_increment(scene->result_card_revision);
    scene->dirty = true;
    return true;
}

MessageHandle message_scene_source_handle(const MessageScene& scene, u32 index) noexcept {
    if (!scene.timeline || !scene.timeline->store)
        return {};
    if (scene.source_order.count)
        return index < scene.source_order.count ? scene.source_order[index] : MessageHandle{};
    return message_store_ordered_handle(*scene.timeline->store, index);
}
u32 message_scene_source_count(const MessageScene& scene) noexcept {
    return scene.source_order.count                  ? scene.source_order.count
           : scene.timeline && scene.timeline->store ? scene.timeline->store->count
                                                     : 0;
}
bool message_scene_set_source_order(MessageScene* scene, ConstSlice<MessageHandle> order,
                                    u32 revision) noexcept {
    if (!scene || !scene->timeline || !scene->timeline->store || (!order.data && order.count) ||
        (order.count && (!scene->result_cards || order.count != scene->timeline->list->count)))
        return false;
    if (scene->source_order.data == order.data && scene->source_order.count == order.count &&
        scene->source_order_revision == revision)
        return true;
    for (u32 i = 0; i < order.count; ++i) {
        if (!message_store_resolve(*scene->timeline->store, order[i]))
            return false;
    }
    scene->source_order = order;
    scene->source_order_revision = revision;
    scene->prepared_signature = 0;
    scene->dirty = true;
    return true;
}

namespace {
// Channel icon 68 px (margin-top 16), thread icon 64 px (margin-top 16),
// DM and group DM avatar 80 px (no margin).
float intro_icon_extent(MessageSceneIntroKind kind) noexcept {
    switch (kind) {
    case MessageSceneIntroKind::channel:
        return 68.0f;
    case MessageSceneIntroKind::thread:
        return 64.0f;
    case MessageSceneIntroKind::direct_message:
    case MessageSceneIntroKind::group_direct_message:
        return 80.0f;
    default:
        return 0.0f;
    }
}
float intro_icon_margin(MessageSceneIntroKind kind) noexcept {
    return kind == MessageSceneIntroKind::channel || kind == MessageSceneIntroKind::thread ? 16.0f : 0.0f;
}
float intro_measure_text(MessageScene* scene, u16 item, Utf8View text, ConstSlice<TextStyleSpan> styles,
                         float width, float line, float em) noexcept {
    if (!text.bytes)
        return 0.0f;
    if (scene->intro_measure) {
        u64 signature = hash64_counted_bytes(kHash64Seed, text.data, text.bytes);
        for (u32 index = 0; index < styles.count; ++index)
            signature = hash_mix(signature, (static_cast<u64>(styles[index].byte_offset) << 32) ^
                                                styles[index].byte_count ^ (u64{styles[index].size} << 56));
        UiTextMeasureValue value{};
        if (ui_text_measure_cache_measure(scene->intro_measure,
                                          {{0x494E54524F000000ull ^ scene->intro.channel_id, signature, 1, item, 0},
                                           text,
                                           styles,
                                           {width, kMaximumUiCoordinate},
                                           false},
                                          &value)
                .ok() &&
            (value.line_count || value.size.height > 0.0f))
            return value.line_count ? static_cast<float>(value.line_count) * line : maximum(line, value.size.height);
    }
    // No text backend (tests, first frame): about half an em per byte.
    const float per_line = maximum(1.0f, width / (em * 0.5f));
    const u32 lines = static_cast<u32>(static_cast<float>(text.bytes) / per_line) + 1u;
    return static_cast<float>(lines) * line;
}
void refresh_intro_measurement(MessageScene* scene, float viewport_width) noexcept {
    const MessageSceneIntro& intro = scene->intro;
    if (intro.kind == MessageSceneIntroKind::none || !scene->owns_rows || scene->result_cards ||
        !(viewport_width > 0.0f)) {
        scene->intro_height = 0.0f;
        scene->intro_header_height = scene->intro_subtitle_height = scene->intro_description_height = 0.0f;
        return;
    }
    const float width = maximum(1.0f, viewport_width - 2.0f * kMessageSceneIntroMargin);
    if (scene->intro_measured_width == width)
        return;
    const Utf8View header = message_scene_intro_header(intro);
    const Utf8View subtitle = message_scene_intro_subtitle(intro);
    const Utf8View description = message_scene_intro_description(intro);
    scene->intro_header_height = intro_measure_text(scene, 1, header, {&scene->intro_header_style, 1}, width,
                                                    kMessageSceneIntroHeaderLine, 32.0f);
    scene->intro_subtitle_height = intro_measure_text(scene, 2, subtitle, {&scene->intro_subtitle_style, 1},
                                                      width, kMessageSceneIntroSubtitleLine, 20.0f);
    scene->intro_description_height =
        intro_measure_text(scene, 3, description, {intro.description_spans, intro.description_span_count}, width,
                           kMessageSceneIntroDescriptionLine, 16.0f);
    float height = 2.0f * kMessageSceneIntroMargin + intro_icon_margin(intro.kind) + intro_icon_extent(intro.kind) +
                   scene->intro_subtitle_height + scene->intro_description_height;
    if (header.bytes)
        height += 2.0f * kMessageSceneIntroHeaderMargin + scene->intro_header_height;
    scene->intro_height = height;
    scene->intro_measured_width = width;
}
// The intro occupies the top of the first row, or with no rows the bottom of
// the viewport (an empty channel's beginning).
UiRect intro_area(const MessageScene& scene) noexcept {
    if (!scene.active || !(scene.intro_height > 0.0f))
        return {};
    if (scene.timeline && scene.timeline->list && scene.timeline->list->count == 0)
        return {scene.viewport.left, scene.viewport.bottom - scene.intro_height, scene.viewport.right,
                scene.viewport.bottom};
    for (u32 index = 0; index < scene.count; ++index) {
        const auto& row = scene.rows[index];
        if (row.source_index != 0 || row.forwarded_snapshot)
            continue;
        const auto* node = message_scene_layout_node(scene, row.widget);
        if (!node)
            return {};
        UiRect area = node->rectangle;
        area.bottom = minimum(area.bottom, area.top + scene.intro_height);
        return area;
    }
    return {};
}
} // namespace

namespace {
struct IntroWriter {
    MessageSceneIntro* intro;
    u32 used;
    // Appends whole UTF-8 sequences while they fit.
    __declspec(noinline) void append(const char* source, u32 count) noexcept {
        for (u32 offset = 0; source && offset < count;) {
            const Utf8DecodeResult decoded = utf8_decode_one(source + offset, count - offset);
            if (!decoded.valid || decoded.width > kMessageSceneIntroTextBytes - used)
                return;
            copy_bytes(intro->text + used, source + offset, decoded.width);
            used += decoded.width;
            offset += decoded.width;
        }
    }
    void add(const Utf8View& text) noexcept {
        append(text.data, text.bytes);
    }
    template <usize Count> __forceinline void add(const char (&text)[Count]) noexcept {
        append(text, Count - 1);
    }
    void bold(u32 description_begin, Utf8View text) noexcept {
        const u32 begin = used;
        add(text);
        if (used > begin && intro->description_span_count < kMessageSceneIntroSpanMaximum)
            intro->description_spans[intro->description_span_count++] = {begin - description_begin, used - begin,
                                                                         text_style_bold, 0, {}};
    }
};
} // namespace

bool message_scene_intro_compose(MessageSceneIntro* intro, MessageSceneIntroKind kind, u64 channel_id,
                                 Utf8View name, Utf8View username, u64 user_id, bool locked) noexcept {
    if (!intro || kind == MessageSceneIntroKind::none || (!name.data && name.bytes) ||
        (!username.data && username.bytes))
        return false;
    reset_value<MessageSceneIntro>(intro);
    intro->kind = kind;
    intro->channel_id = channel_id;
    intro->locked = locked;
    IntroWriter out{intro, 0};
    // Header (channel welcome / thread name / conversation name).
    if (kind == MessageSceneIntroKind::channel) {
        out.add("Welcome to #");
        out.add(name);
        out.add("!");
    } else if (kind != MessageSceneIntroKind::no_history) {
        out.add(name);
    }
    intro->header_bytes = out.used;
    // Subtitle: a DM recipient's username.
    if (kind == MessageSceneIntroKind::direct_message) {
        out.add(username);
        intro->user_id = user_id;
    }
    intro->subtitle_bytes = out.used - intro->header_bytes;
    const u32 description = out.used;
    switch (kind) {
    case MessageSceneIntroKind::channel:
        out.add("This is the start of the #");
        out.add(name);
        if (locked) {
            out.add(" ");
            out.bold(description, utf8_literal("private"));
        }
        out.add(" channel.");
        break;
    case MessageSceneIntroKind::thread:
        if (locked)
            out.add("@mention users to add them to this private thread.");
        break;
    case MessageSceneIntroKind::direct_message:
        out.add("This is the beginning of your direct message history with ");
        out.bold(description, name);
        out.add(".");
        break;
    case MessageSceneIntroKind::group_direct_message:
        out.add("Welcome to the beginning of the ");
        out.bold(description, name);
        out.add(" group.");
        break;
    case MessageSceneIntroKind::no_history: {
        out.add("You do not have permission to view the message history of ");
        const u32 begin = out.used;
        out.add("#");
        out.add(name);
        if (out.used > begin && intro->description_span_count < kMessageSceneIntroSpanMaximum)
            intro->description_spans[intro->description_span_count++] = {begin - description, out.used - begin,
                                                                         text_style_bold, 0, {}};
        out.add(".");
        break;
    }
    default:
        break;
    }
    intro->description_bytes = out.used - description;
    return true;
}

bool message_scene_set_intro(MessageScene* scene, const MessageSceneIntro* intro,
                             UiTextMeasureCache* measure) noexcept {
    if (!scene)
        return false;
    MessageSceneIntro next{};
    if (intro && intro->kind != MessageSceneIntroKind::none &&
        intro->header_bytes + intro->subtitle_bytes + intro->description_bytes <= kMessageSceneIntroTextBytes &&
        intro->description_span_count <= kMessageSceneIntroSpanMaximum)
        copy_value(&next, *intro);
    u64 signature = hash_mix(kHash64Seed, static_cast<u64>(next.kind) | (u64{next.locked} << 8));
    signature = hash_mix(signature, next.user_id);
    signature = hash_mix(signature, next.channel_id);
    const u32 text_bytes = next.header_bytes + next.subtitle_bytes + next.description_bytes;
    signature = hash_mix(signature, (u64{next.header_bytes} << 40) ^ (u64{next.subtitle_bytes} << 20) ^
                                        next.description_bytes);
    signature = hash64_counted_bytes(signature, next.text, text_bytes);
    signature = hash64_counted_bytes(signature, next.description_spans,
                                     next.description_span_count * static_cast<u32>(sizeof(TextStyleSpan)));
    scene->intro_measure = measure;
    if (signature == scene->intro_signature)
        return false;
    copy_value(&scene->intro, next);
    scene->intro_signature = signature;
    scene->intro_header_style = {0, next.header_bytes, text_style_bold, text_size_xxl, {}};
    scene->intro_subtitle_style = {0, next.subtitle_bytes, text_style_bold, text_size_lg, {}};
    // Measured (width and text) by the next update, before row extents.
    scene->intro_measured_width = 0.0f;
    scene->intro_height = 0.0f;
    scene->intro_header_height = scene->intro_subtitle_height = scene->intro_description_height = 0.0f;
    scene->dirty = true;
    return true;
}

bool message_scene_intro_geometry(const MessageScene& scene, MessageSceneIntroGeometry* geometry) noexcept {
    if (!geometry)
        return false;
    reset_value(geometry);
    const UiRect area = intro_area(scene);
    if (!ui_rect_has_area(area))
        return false;
    const MessageSceneIntro& intro = scene.intro;
    geometry->area = area;
    const float left = area.left + kMessageSceneIntroMargin;
    const float right = maximum(left + 1.0f, area.right - kMessageSceneIntroMargin);
    float top = area.top + kMessageSceneIntroMargin + intro_icon_margin(intro.kind);
    const float icon = intro_icon_extent(intro.kind);
    if (icon > 0.0f)
        geometry->icon = {left, top, left + icon, top + icon};
    top += icon;
    if (intro.header_bytes) {
        top += kMessageSceneIntroHeaderMargin;
        geometry->header = {left, top, right, top + scene.intro_header_height};
        top += scene.intro_header_height + kMessageSceneIntroHeaderMargin;
    }
    if (intro.subtitle_bytes) {
        geometry->subtitle = {left, top, right, top + scene.intro_subtitle_height};
        top += scene.intro_subtitle_height;
    }
    if (intro.description_bytes)
        geometry->description = {left, top, right, top + scene.intro_description_height};
    return true;
}

UiRect message_scene_unread_divider_rectangle(const MessageScene& scene,
                                              const MessageSceneRowWidget& row) noexcept {
    if (!scene.active || !scene.unread_cursor_known || row.forwarded_snapshot || !row.message_id ||
        row.message_id != scene.unread_first_message_id)
        return {};
    const auto* node = message_scene_layout_node(scene, row.widget);
    if (!node)
        return {};
    auto rectangle = node->rectangle;
    rectangle.top = minimum(rectangle.bottom, rectangle.top + intro_row_height(scene, row));
    rectangle.bottom = minimum(rectangle.bottom, rectangle.top + scene.unread_divider_style.height);
    return rectangle;
}

UiRect message_scene_message_rectangle(const MessageScene& scene,
                                       const MessageSceneRowWidget& row) noexcept {
    const auto* node = message_scene_layout_node(scene, row.widget);
    if (!node)
        return {};
    auto rectangle = node->rectangle;
    rectangle.top = minimum(rectangle.bottom, rectangle.top + intro_row_height(scene, row));
    const auto divider = message_scene_unread_divider_rectangle(scene, row);
    if (ui_rect_has_area(divider))
        rectangle.top = divider.bottom;
    return rectangle;
}

MessageSceneResult message_scene_update(MessageScene* scene, const UiRect& viewport_view) noexcept {
    const UiRect viewport = viewport_view; // the scene is written below
    if (!scene || !scene->timeline || !scene->timeline->store || !scene->timeline->list ||
        !scene->rows.data || !scene->widgets.storage.data || !ui_rect_valid_or_empty(viewport) ||
        message_scene_source_count(*scene) != scene->timeline->list->count) {
        return scene_error(scene, MessageSceneError::invalid_input);
    }
    if (!ui_rect_has_area(viewport)) {
        const bool changed =
            scene->active || scene->count != 0 || !ui_rect_equal(scene->viewport, viewport);
        clear_materialization(scene);
        scene->viewport = viewport;
        scene->source_count = message_scene_source_count(*scene);
        return MessageSceneResult{MessageSceneError::none, 0, 0, changed};
    }
    const u32 viewport_units = ui_extent_units(ui_rect_height(viewport));
    if (viewport_units == 0 || !message_timeline_set_viewport(scene->timeline, viewport_units)) {
        return scene_error(scene, MessageSceneError::model);
    }
    refresh_intro_measurement(scene, ui_rect_width(viewport));
    const u64 prepared_before = scene->prepared_signature;
    if (!prepare_resident_measurements(scene, viewport))
        return scene_error(scene, MessageSceneError::model);
    UiVariableRange range =
        scene->result_cards ? ui_variable_list_range(*scene->timeline->list, scene->timeline->scroll_units,
                                                     viewport_units, 0)
        : scene->owns_rows
            ? ui_variable_list_buffered_range(*scene->timeline->list, scene->timeline->scroll_units,
                                              viewport_units,
                                              ui_extent_units(kMessageScenePreparationMarginDips))
            : ui_variable_list_range(*scene->timeline->list, scene->timeline->scroll_units, viewport_units,
                                     kMessageSceneOverscanRows);
    if (scene->result_cards)
        range.count = minimum(range.count, scene->result_card_style.materialized_rows);
    const UiVariableList& list = *scene->timeline->list;
    if (scene->owns_rows && !scene->result_cards && scene->active && !scene->dirty && scene->count &&
        scene->first + scene->count <= list.count && scene->source_count == message_scene_source_count(*scene) &&
        scene->observed_structural_updates == list.structural_updates) {
        // Scrolling keeps the materialized rows while they still reach half the
        // preparation margin past both viewport edges, so most scroll steps
        // move the same rows instead of rebuilding them.
        const u64 scroll = scene->timeline->scroll_units;
        const u64 half_margin = ui_extent_units(kMessageScenePreparationMarginDips * 0.5f);
        const u64 begin = ui_variable_list_prefix_units(list, scene->first);
        const u64 end = ui_variable_list_prefix_units(list, scene->first + scene->count);
        if ((scene->first == 0 || begin + half_margin <= scroll) &&
            (scene->first + scene->count == list.count || end >= scroll + viewport_units + half_margin))
            range = UiVariableRange{scene->first, scene->count,
                                    static_cast<i64>(begin) - static_cast<i64>(scroll)};
    }
    if (!scene->owns_rows && scene->active && message_scene_pending(*scene)) {
        const u32 retained_end = scene->first + scene->count;
        const u32 desired_end = range.first + range.count;
        const u32 union_first = minimum(scene->first, range.first);
        const u32 union_end = maximum(retained_end, desired_end);
        const u32 union_count = union_end - union_first;
        if (union_count > scene->rows.count) {
            message_scene_reserve_rows(scene, union_count);
        }
        if (union_count <= scene->rows.count) {
            const u64 first_prefix = ui_variable_list_prefix_units(*scene->timeline->list, union_first);
            range = UiVariableRange{union_first, union_count,
                                    static_cast<i64>(first_prefix) -
                                        static_cast<i64>(scene->timeline->scroll_units)};
        }
    }
    if (range.count > scene->rows.count && !message_scene_reserve_rows(scene, range.count)) {
        clear_materialization(scene);
        scene->viewport = viewport;
        return scene_error(scene, MessageSceneError::capacity);
    }

    // The same rows over the same data, viewport and preparation epoch need no
    // row pass: every input a row reads moves one of the observed counters,
    // the epoch or `dirty` (plugin verdicts, unread state and display changes
    // all do). Only a poll's vote revision moves none of them.
    const MessageStore& store = *scene->timeline->store;
    const bool unchanged =
        scene->owns_rows && !scene->result_cards && scene->active && !scene->dirty &&
        prepared_before == scene->prepared_signature && range.first == scene->first &&
        range.count == scene->count && scene->source_count == message_scene_source_count(*scene) &&
        scene->observed_created == store.created && scene->observed_updated == store.updated &&
        scene->observed_deleted == store.deleted && scene->observed_height_updates == list.height_updates &&
        scene->observed_structural_updates == list.structural_updates &&
        scene->observed_rich_updated == (scene->timeline->rich_store ? scene->timeline->rich_store->updated : 0) &&
        ui_rect_equal(scene->viewport, viewport);
    const bool retained =
        unchanged ||
        (materialization_matches(*scene, range.first, range.count) &&
         (scene->snapshot_count == 0 || ui_rect_width(scene->viewport) == ui_rect_width(viewport)));
    const bool viewport_changed = !ui_rect_equal(scene->viewport, viewport);
    scene->viewport = viewport;
    const bool was_dirty = scene->dirty;
    bool data_changed = false;
    if (retained) {
        for (u32 local = 0; local < range.count; ++local) {
            if (unchanged && !scene->rows[local].poll_content_revision)
                continue;
            if (!refresh_row(scene, &scene->rows[local], range.first + local, true, &data_changed) ||
                !apply_row_specs(scene, &scene->rows[local])) {
                return scene_error(scene, MessageSceneError::model);
            }
        }
    } else {
        const u64 restore_focus = focused_message(*scene);
        rebase_retained_rows(scene, range.first, range.count);
        scene->snapshot_count = 0;
        ui_widget_manager_reset(&scene->widgets);
        const UiWidgetResult root = ui_widget_manager_add(
            &scene->widgets, ui_widget_invalid_handle(),
            UiWidgetDesc{kMessageRootWidgetId, UiWidgetKind::scroll_view, ui_widget_enabled,
                         UiLayoutNodeSpec{utf8_literal("MessageTimeline"), ui_layout_scroll_root_style(), UiSize{}, 0.0f,
                                          kMessageRootWidgetId}});
        if (!root.ok()) {
            clear_materialization(scene);
            return scene_error(scene, MessageSceneError::widget);
        }
        scene->root = root.handle;
        for (u32 local = 0; local < range.count; ++local) {
            if (!build_row(scene, local, range.first + local)) {
                clear_materialization(scene);
                return scene_error(scene, MessageSceneError::widget);
            }
        }
        scene->first = range.first;
        scene->count = range.count;
        scene->focused_message_id = restore_focus;
        scene->active = true;
        saturating_increment(scene->rebuilds);
    }

    const float local_scroll = maximum(0.0f, -ui_signed_extent_dips(range.first_offset_units));
    // Short content sits against the bottom edge: rows are laid out below a
    // leading gap; scroll offsets are unaffected because such content has no
    // scroll range.
    const u64 total_units = scene->timeline->list->total_units;
    scene->lead_dips = scene->bottom_anchored && scene->timeline->list->count != 0 && total_units < viewport_units
                           ? ui_extent_dips(viewport_units - total_units)
                           : 0.0f;
    UiRect laid_out = viewport;
    laid_out.top = minimum(viewport.bottom - 1.0f, viewport.top + scene->lead_dips);
    const bool observed_change =
        scene->source_count != message_scene_source_count(*scene) ||
        scene->observed_created != scene->timeline->store->created ||
        scene->observed_updated != scene->timeline->store->updated ||
        scene->observed_deleted != scene->timeline->store->deleted ||
        scene->observed_height_updates != scene->timeline->list->height_updates ||
        scene->observed_structural_updates != scene->timeline->list->structural_updates;
    const bool scroll_changed = scene->observed_scroll_units != scene->timeline->scroll_units;
    const u32 rich_updated = scene->timeline->rich_store ? scene->timeline->rich_store->updated : 0;
    const bool paint_changed = scene->observed_rich_updated != rich_updated;
    if (retained && !viewport_changed && !data_changed && !observed_change && !scroll_changed &&
        !was_dirty) {
        // Paint-only rich metadata (such as an embed RGB color) must refresh
        // retained geometry without invalidating its measured text/layout.
        scene->observed_rich_updated = rich_updated;
        scene->scroll_dirty = false;
        saturating_increment_if(paint_changed, scene->data_updates);
        return MessageSceneResult{MessageSceneError::none, range.first, range.count, paint_changed};
    }
    if (!ui_widget_manager_set_scroll(&scene->widgets, scene->root, UiPoint{0.0f, local_scroll}) ||
        !ui_widget_manager_layout_rectangle(&scene->widgets, laid_out).ok()) {
        clear_materialization(scene);
        return scene_error(scene, MessageSceneError::layout);
    }
    saturating_increment(scene->layout_passes);
    saturating_increment_if(retained && (data_changed || paint_changed), scene->data_updates);
    scene->first = range.first;
    scene->count = range.count;
    scene->source_count = message_scene_source_count(*scene);
    scene->observed_created = scene->timeline->store->created;
    scene->observed_updated = scene->timeline->store->updated;
    scene->observed_deleted = scene->timeline->store->deleted;
    scene->observed_rich_updated = rich_updated;
    scene->observed_height_updates = scene->timeline->list->height_updates;
    scene->observed_structural_updates = scene->timeline->list->structural_updates;
    scene->observed_scroll_units = scene->timeline->scroll_units;
    scene->dirty = false;
    scene->scroll_dirty = false;
    if (scene->focused_message_id != 0) {
        message_scene_focus_message(scene, scene->focused_message_id);
    }
    const float rows_origin = laid_out.top - local_scroll;
    const bool scroll_only = retained && scroll_changed && !viewport_changed && !data_changed &&
                             !observed_change && !was_dirty && !paint_changed;
    const float scroll_shift = rows_origin - scene->rows_origin_dips;
    scene->rows_origin_dips = rows_origin;
    return MessageSceneResult{MessageSceneError::none,
                              range.first,
                              range.count,
                              !retained || viewport_changed || data_changed || observed_change ||
                                  scroll_changed || was_dirty || paint_changed,
                              scroll_only,
                              scroll_only ? scroll_shift : 0.0f};
}

bool message_scene_scroll_by(MessageScene* scene, i64 delta_units, u64 bottom_tolerance_units,
                             bool* changed) noexcept {
    if (!scene || !scene->timeline || !changed ||
        !message_timeline_scroll_by(scene->timeline, delta_units, bottom_tolerance_units, changed)) {
        return false;
    }
    scene->scroll_dirty = scene->scroll_dirty || *changed;
    return true;
}

bool message_scene_scroll_to_start(MessageScene* scene, bool* changed) noexcept {
    if (!scene || !scene->timeline || !changed ||
        !message_timeline_scroll_to_start(scene->timeline, changed)) {
        return false;
    }
    scene->scroll_dirty = scene->scroll_dirty || *changed;
    return true;
}

bool message_scene_scroll_to_bottom(MessageScene* scene, bool* changed) noexcept {
    if (!scene || !scene->timeline || !changed ||
        !message_timeline_scroll_to_bottom(scene->timeline, changed)) {
        return false;
    }
    scene->scroll_dirty = scene->scroll_dirty || *changed;
    return true;
}

bool message_scene_set_text_metrics(MessageScene* scene, u32 source_index, float reply_height,
                                    float content_height, bool content_present,
                                    const MessageSceneRichLayout* rich_layout,
                                    const MessageSceneRichItemLayout* rich_item_layout,
                                    ConstSlice<float> reaction_widths, bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!scene || !scene->timeline || !changed || !ui_extent_valid(reply_height) ||
        !ui_extent_valid(content_height) || (rich_layout && !message_rich_layout_valid(*rich_layout)) ||
        (rich_item_layout && !message_rich_item_layout_valid(*rich_item_layout)) ||
        (!reaction_widths.data && reaction_widths.count != 0) ||
        reaction_widths.count > kGatewayMessageReactionMaximum) {
        return false;
    }
    MessageSceneRowWidget* row = message_scene_find_row(scene, source_index);
    if (!row) {
        return false;
    }
    const MessageRecord* record = message_store_resolve(*scene->timeline->store, row->message);
    if (!record || record->id != row->message_id) {
        return false;
    }
    const MessageRichRecord* rich = row_rich(*scene, *record);
    const float measured_reply = rich && message_rich_has_header(*rich) ? reply_height : 0.0f;
    MessageSceneRichLayout measured_rich = rich_layout ? *rich_layout : row->rich_layout;
    const MessageSceneRichItemLayout& measured_items =
        rich_item_layout ? *rich_item_layout : row->rich_item_layout;
    float intrinsic_reaction_widths[kGatewayMessageReactionMaximum]{};
    if (rich && reaction_widths.count >= rich->reaction_count) {
        for (u32 index = 0; index < rich->reaction_count; ++index) {
            const float label_width = reaction_widths[index];
            intrinsic_reaction_widths[index] =
                kDefaultMessageReactionLayoutMetrics.content_left_padding +
                kDefaultMessageReactionLayoutMetrics.content_right_padding + label_width +
                (rich->reactions[index].emoji_id != 0 ||
                         (scene->timeline->rich_store &&
                          regional_flag_sequence(message_rich_store_string(*scene->timeline->rich_store,
                                                                           rich->reactions[index].name)))
                     ? kDefaultMessageReactionLayoutMetrics.emoji_extent +
                           kDefaultMessageReactionLayoutMetrics.emoji_gap
                     : kDefaultMessageReactionLayoutMetrics.unicode_text_inset);
        }
        measured_rich.reactions = message_rich_reaction_layout_height(
            *rich, available_text_width(scene, scene->viewport),
            ConstSlice<float>{intrinsic_reaction_widths, rich->reaction_count});
    }
    if (rich && rich_layout && rich_item_layout &&
        !message_rich_layout_equal(
            measured_rich, message_rich_aggregate_layout(
                               *rich, measured_items, available_text_width(scene, scene->viewport),
                               ConstSlice<float>{intrinsic_reaction_widths, rich->reaction_count}))) {
        return false;
    }
    const float measured_content =
        maximum(content_height, content_present ? 16.0f : 0.0f) +
        content_trailing(measured_rich.total() > 0.0f || row->poll_height > 0.0f || row->has_snapshot);
    bool reaction_metrics_changed = false;
    for (u32 index = 0; index < row->reaction_button_count; ++index) {
        const float width = index < reaction_widths.count
                                ? intrinsic_reaction_widths[index]
                                : message_rich_reaction_button_width(rich->reactions[index]);
        reaction_metrics_changed = reaction_metrics_changed || row->reaction_widths[index] != width;
        row->reaction_widths[index] = width;
    }
    const bool metrics_changed = reaction_metrics_changed || !row->text_measured ||
                                 row->measured_reply_height != measured_reply ||
                                 row->measured_content_height != measured_content ||
                                 !message_rich_layout_equal(row->rich_layout, measured_rich) ||
                                 !message_rich_item_layout_equal(row->rich_item_layout, measured_items);
    row->measured_reply_height = measured_reply;
    row->measured_content_height = measured_content;
    copy_value(&row->rich_layout, measured_rich);
    if (&measured_items != &row->rich_item_layout)
        copy_value(&row->rich_item_layout, measured_items);
    row->rich_layout_measured = row->rich_layout_measured || rich_layout != nullptr;
    row->measured_width = available_text_width(scene, scene->viewport);
    row->text_measured = true;
    message_scene_detail::retain_measurement(scene, *row);
    const u32 desired_units = ui_extent_units(row_required_height(*scene, *row, rich));
    const u32 resident_units = ui_variable_list_item_units(*scene->timeline->list, source_index);
    if (desired_units != resident_units) {
        if (!set_resident_height(scene, source_index, desired_units)) {
            return false;
        }
        scene->dirty = true;
        saturating_increment(scene->measurement_updates);
        *changed = true;
    } else {
        *changed = metrics_changed;
        scene->dirty = scene->dirty || metrics_changed;
    }
    return true;
}

UiWidgetHandle message_scene_rich_item_widget(const MessageSceneRowWidget& row,
                                              MessageSceneRichItemKind kind, u32 index) noexcept {
    if (kind == MessageSceneRichItemKind::poll)
        return index == 0 ? row.poll : ui_widget_invalid_handle();
    const u32 count = kind == MessageSceneRichItemKind::attachment  ? row.attachment_item_count
                      : kind == MessageSceneRichItemKind::embed     ? row.embed_item_count
                      : kind == MessageSceneRichItemKind::sticker   ? row.sticker_item_count
                      : kind == MessageSceneRichItemKind::component ? row.component_item_count
                      : kind == MessageSceneRichItemKind::invite    ? row.invite_item_count
                                                                    : row.notice_item_count;
    u32 slot = 0;
    if (index >= count || !message_rich_item_slot(kind, index, &slot)) {
        return ui_widget_invalid_handle();
    }
    return row.rich_items[slot];
}

const MessageSceneRowWidget* message_scene_snapshot_row(const MessageScene& scene,
                                                        const MessageSceneRowWidget& outer) noexcept {
    if (!outer.has_snapshot || outer.snapshot_slot >= scene.snapshot_count ||
        outer.snapshot_slot >= scene.snapshot_rows.count || !scene.snapshot_rows.data)
        return nullptr;
    const auto* child = &scene.snapshot_rows[outer.snapshot_slot];
    return child->forwarded_snapshot && child->message_id == outer.message_id &&
                   child->message_revision == outer.message_revision &&
                   child->message.generation == outer.message.generation
               ? child
               : nullptr;
}

bool message_scene_set_snapshot_text_metrics(MessageScene* scene, u32 source_index, float content_height,
                                             bool content_present,
                                             const MessageSceneRichLayout* rich_layout,
                                             const MessageSceneRichItemLayout* rich_item_layout,
                                             bool* changed) noexcept {
    if (changed)
        *changed = false;
    if (!scene || !changed || !ui_extent_valid(content_height) ||
        (rich_layout && !message_rich_layout_valid(*rich_layout)) ||
        (rich_item_layout && !message_rich_item_layout_valid(*rich_item_layout)))
        return false;
    auto* outer = message_scene_find_row(scene, source_index);
    auto* child =
        outer ? const_cast<MessageSceneRowWidget*>(message_scene_snapshot_row(*scene, *outer)) : nullptr;
    const auto* rich = child ? snapshot_rich(*scene, child->message_id) : nullptr;
    if (!child || !rich)
        return false;
    const float content = maximum(content_height, content_present ? 16.f : 0.f) +
                          content_trailing((rich_layout ? rich_layout->total() : child->rich_layout.total()) > 0.f);
    const MessageSceneRichItemLayout& items = rich_item_layout ? *rich_item_layout : child->rich_item_layout;
    const auto layout = rich_layout ? *rich_layout : child->rich_layout;
    if (rich_layout && rich_item_layout &&
        !message_rich_layout_equal(layout,
                                   message_rich_aggregate_layout(*rich, items, child->measured_width)))
        return false;
    *changed = !child->text_measured || child->measured_content_height != content ||
               !message_rich_layout_equal(child->rich_layout, layout) ||
               !message_rich_item_layout_equal(child->rich_item_layout, items);
    child->measured_content_height = content;
    child->rich_layout = layout;
    if (&items != &child->rich_item_layout)
        copy_value(&child->rich_item_layout, items);
    child->rich_layout_measured = rich_layout != nullptr || child->rich_layout_measured;
    child->text_measured = true;
    message_scene_detail::retain_measurement(scene, *child);
    const auto* record = message_store_resolve(*scene->timeline->store, outer->message);
    if (!record)
        return false;
    outer->snapshot_height = row_required_height(*scene, *child, rich) +
                             snapshot_following_gap(scene, row_rich(*scene, *record));
    const u32 units = ui_extent_units(row_required_height(*scene, *outer, row_rich(*scene, *record)));
    if (units != ui_variable_list_item_units(*scene->timeline->list, source_index)) {
        if (!set_resident_height(scene, source_index, units))
            return false;
        *changed = true;
    }
    scene->dirty = scene->dirty || *changed;
    saturating_increment_if(*changed, scene->measurement_updates);
    return true;
}

const UiLayoutNode* message_scene_rich_item_layout_node(const MessageScene& scene,
                                                        const MessageSceneRowWidget& row,
                                                        MessageSceneRichItemKind kind, u32 index) noexcept {
    return message_scene_layout_node(scene, message_scene_rich_item_widget(row, kind, index));
}

const MessageSceneRowWidget* message_scene_resolve(const MessageScene& scene,
                                                   UiWidgetHandle widget) noexcept {
    // Every row owns one fixed-stride widget ID range (build_row), so the
    // widget's ID names the only row that can hold its handle.
    const UiWidget* found = ui_widget_manager_resolve(scene.widgets, widget);
    if (!found || found->id < 2u) {
        return nullptr;
    }
    const MessageSceneRowWidget* row = message_scene_find_row(scene, (found->id - 2u) / kMessageSceneWidgetsPerRow);
    if (!row) {
        return nullptr;
    }
    const auto same = [&](UiWidgetHandle handle) noexcept {
        return handle.slot == widget.slot && handle.generation == widget.generation;
    };
    const UiWidgetHandle handles[] = {row->widget,   row->avatar,     row->body,        row->reply,
                                      row->author,   row->content,    row->attachments, row->embeds,
                                      row->stickers, row->components, row->reactions,   row->notice,
                                      row->poll,     row->thread};
    for (const auto handle : handles)
        if (same(handle))
            return row;
    for (u32 reaction = 0; reaction < row->reaction_button_count; ++reaction)
        if (same(row->reaction_buttons[reaction]))
            return row;
    for (const auto handle : row->rich_items)
        if (same(handle))
            return row;
    if (const auto* child = message_scene_snapshot_row(scene, *row)) {
        const UiWidgetHandle child_handles[]{child->widget,      child->reply,  child->content,
                                             child->attachments, child->embeds, child->stickers,
                                             child->components,  child->notice};
        for (const auto handle : child_handles)
            if (same(handle))
                return row;
        for (const auto handle : child->rich_items)
            if (same(handle))
                return row;
    }
    return nullptr;
}

MessageSceneRowWidget* message_scene_find_row(MessageScene* scene, u32 source_index) noexcept {
    if (!scene || source_index < scene->first || source_index - scene->first >= scene->count) {
        return nullptr;
    }
    MessageSceneRowWidget* row = &scene->rows[source_index - scene->first];
    return row->source_index == source_index ? row : nullptr;
}

const MessageSceneRowWidget* message_scene_find_row(const MessageScene& scene, u32 source_index) noexcept {
    if (source_index < scene.first || source_index - scene.first >= scene.count) {
        return nullptr;
    }
    const MessageSceneRowWidget* row = &scene.rows[source_index - scene.first];
    return row->source_index == source_index ? row : nullptr;
}

const UiLayoutNode* message_scene_layout_node(const MessageScene& scene, UiWidgetHandle widget) noexcept {
    return ui_widget_manager_layout_node(scene.widgets, widget);
}

MessageSceneHit message_scene_hit_test(const MessageScene& scene, float x, float y) noexcept {
    const UiWidgetHandle widget = ui_widget_manager_hit_test(scene.widgets, x, y);
    const MessageSceneRowWidget* row = message_scene_resolve(scene, widget);
    if (!row)
        return {};
    const auto divider = message_scene_unread_divider_rectangle(scene, *row);
    if (ui_rect_has_area(divider) && ui_rect_contains(divider, x, y))
        return {};
    // The channel-beginning block is not part of the first message.
    if (y < message_scene_message_rectangle(scene, *row).top)
        return {};
    // Rich children retain their own widget clipping during intrinsic-height
    // convergence; only the decoration strip adds a new hit exclusion.
    return MessageSceneHit{row->message_id, widget, row->source_index};
}

MessageReplyHit message_scene_reply_hit_test(const MessageScene& scene, float x, float y) noexcept {
    if (!scene.active || !scene.timeline || !scene.timeline->store || !scene.timeline->rich_store ||
        !ui_rect_contains(scene.viewport, x, y))
        return {};
    const auto hit = message_scene_hit_test(scene, x, y);
    const auto* row = hit.message_id ? message_scene_find_row(scene, hit.source_index) : nullptr;
    if (!row || row->message_id != hit.message_id)
        return {};
    const auto* preview = message_scene_layout_node(scene, row->reply);
    if (!preview ||
        !ui_rect_contains(ui_rect_intersection(preview->rectangle, preview->clip_rectangle), x, y))
        return {};
    const auto* record = message_store_resolve(*scene.timeline->store, row->message);
    const auto* rich = message_rich_store_find(*scene.timeline->rich_store, row->message_id);
    if (!record || record->id != row->message_id || !rich || rich->reference.message_id == 0 ||
        rich->reference.state == GatewayMessageReferenceState::none ||
        rich->reference.state == GatewayMessageReferenceState::deleted)
        return {};
    // A reference can retain its ID before its message has been downloaded.
    // The common navigation path fetches around that ID when it is not loaded.
    return {rich->reference.channel_id ? rich->reference.channel_id : record->channel_id,
            rich->reference.message_id};
}

bool message_scene_reaction_index(const MessageScene& scene, MessageSceneHit hit, u32* index) noexcept {
    if (!index || hit.message_id == 0 || !message_rich_display().reactions) {
        return false;
    }
    const MessageSceneRowWidget* row = message_scene_find_row(scene, hit.source_index);
    if (!row || row->message_id != hit.message_id ||
        !ui_widget_manager_resolve(scene.widgets, hit.widget)) {
        return false;
    }
    for (u32 reaction = 0; reaction < row->reaction_button_count; ++reaction) {
        if (row->reaction_buttons[reaction].slot == hit.widget.slot &&
            row->reaction_buttons[reaction].generation == hit.widget.generation) {
            *index = reaction;
            return true;
        }
    }
    return false;
}

bool message_scene_sticker_hit(const MessageScene& scene, float x, float y, MessageStickerHit* hit) noexcept {
    if (!hit)
        return false;
    *hit = {};
    const auto at = message_scene_hit_test(scene, x, y);
    const MessageSceneRowWidget* row = at.message_id ? message_scene_find_row(scene, at.source_index) : nullptr;
    if (!row || row->message_id != at.message_id)
        return false;
    for (u32 snapshot = 0; snapshot < 2; ++snapshot) {
        const MessageSceneRowWidget* items = snapshot ? message_scene_snapshot_row(scene, *row) : row;
        for (u32 index = 0; items && index < items->sticker_item_count; ++index) {
            const UiWidgetHandle widget =
                message_scene_rich_item_widget(*items, MessageSceneRichItemKind::sticker, index);
            if (!ui_widget_handle_matches(widget, at.widget))
                continue;
            const UiLayoutNode* node = message_scene_layout_node(scene, widget);
            if (!node)
                return false;
            // Drawn at the item's origin (presentation.cpp).
            const float extent = kDefaultMessageStickerLayoutMetrics.extent;
            const UiRect preview = ui_rect_intersection(
                {node->rectangle.left, node->rectangle.top, node->rectangle.left + extent, node->rectangle.top + extent},
                node->clip_rectangle);
            if (!ui_rect_has_area(preview) || !ui_rect_contains(preview, x, y))
                return false;
            *hit = {row->message_id, preview, widget, index, snapshot != 0};
            return true;
        }
    }
    return false;
}

bool message_scene_focus_message(MessageScene* scene, u64 message_id) noexcept {
    if (!scene) {
        return false;
    }
    if (message_id == 0) {
        scene->focused_message_id = 0;
        return ui_widget_manager_focus(&scene->widgets, ui_widget_invalid_handle());
    }
    for (u32 index = 0; index < scene->count; ++index) {
        const MessageSceneRowWidget& row = scene->rows[index];
        if (row.message_id == message_id && ui_widget_manager_focus(&scene->widgets, row.widget)) {
            scene->focused_message_id = message_id;
            return true;
        }
    }
    return false;
}

bool message_scene_plugin_decoration(const MessageScene& scene, u64 message_id, u32 revision,
                                     UiPluginRenderDecoration* output) noexcept {
    if (scene.plugin_decorations)
        return ui_plugin_render(message_id, revision, output);
    reset_value(output);
    return false;
}

u8 message_scene_plugin_visibility(const MessageScene& scene, const MessageRecord& record) noexcept {
    if (!scene.plugin_decorations || !g_ui_plugin_render_query)
        return ui_plugin_render_normal;
    UiPluginRenderDecoration decoration;
    return ui_plugin_render(record.id, record.revision, &decoration) ? decoration.visibility
                                                                     : ui_plugin_render_normal;
}

} // namespace maicord
