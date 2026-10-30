#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/ui/surface_geometry.hpp"
#include "maicord/ui/widget_builder.hpp"

#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/message_typing_presentation.hpp"
#include "maicord/ui/typing_indicator.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
namespace {

struct MessageComposerPresentationStorage {
    MessageComposerAtomicSpan atomic_spans[kMessageComposerProjectionSpanCapacity];
    TextStyleSpan source_styles[kTextStyleSpanMaximum];
    TextStyleSpan display_styles[kTextStyleSpanMaximum];
    char display[kMessageComposerDisplayBytes];
    char reply_label[kMessageComposerReplyLabelBytes];
    MessageComposerAttachmentStrip attachment_strip;
    char typing_label[kMessageTypingLabelMaximumBytes];
    char placeholder[kMessageComposerPlaceholderBytes];
    char command_bar[kMessageComposerCommandBarBytes];
    MessageComposerProjectionSpan projection_spans[kMessageComposerProjectionSpanCapacity];
};

inline constexpr usize kMessageComposerSceneStorageBytes =
    sizeof(MessageComposerPresentationStorage) +
    static_cast<usize>(sizeof(UiWidget) + sizeof(UiLayoutNode)) * kMessageComposerWidgetCapacity +
    alignof(UiWidget) + alignof(UiLayoutNode);

bool initialize_owned_storage(MessageComposerScene* scene) noexcept {
    if (!arena_initialize(&scene->owned_storage, kMessageComposerSceneStorageBytes, 0, 4 * 1024)) {
        return false;
    }
    scene->owns_storage = true;
    // The new arena already supplies zero-filled presentation and widget storage.
    auto* storage = arena_allocate<MessageComposerPresentationStorage>(&scene->owned_storage);
    if (!storage) {
        return false;
    }
    scene->display = storage->display;
    scene->reply_label = storage->reply_label;
    scene->attachment_strip = &storage->attachment_strip;
    storage->attachment_strip.hovered = kUiComposerAttachmentNoTile;
    scene->typing_label = storage->typing_label;
    scene->placeholder = storage->placeholder;
    scene->command_bar_text = storage->command_bar;
    scene->projection_spans = storage->projection_spans;
    scene->atomic_spans = storage->atomic_spans;
    scene->source_styles = storage->source_styles;
    scene->display_styles = storage->display_styles;
    return true;
}

inline constexpr u32 kComposerRootId = 1;
inline constexpr u32 kComposerReplyId = 2;
inline constexpr u32 kComposerControlsId = 3;
inline constexpr u32 kComposerAttachmentId = 4;
inline constexpr u32 kComposerEditId = 5;
inline constexpr u32 kComposerEmojiId = 6;
inline constexpr u32 kComposerSendId = 7;
inline constexpr u32 kComposerTypingId = 8;
inline constexpr u32 kComposerSurfaceId = 9;
inline constexpr u32 kComposerAttachmentsId = 10;
inline constexpr u32 kComposerGifId = 11;
inline constexpr u32 kComposerStickerId = 12;
inline constexpr u32 kComposerReplyMentionId = 13;
inline constexpr u32 kComposerReplyCancelId = 14;
inline constexpr u32 kComposerPluginId = 15; // through 15 + kMessageComposerPluginSlots - 1
inline constexpr u32 kComposerCommandBarId = kComposerPluginId + kMessageComposerPluginSlots;
inline constexpr u32 kComposerCommandCloseId = kComposerCommandBarId + 1;
inline constexpr float kComposerRowGap = kUiComposerPadding;

const UiThemeMetrics& composer_metrics(const UiResolvedThemeStyles* styles) noexcept {
    return styles && styles->ready ? styles->metrics : kUiThemeMetricsDefault;
}

float accessory_width(const UiResolvedThemeStyles* styles) noexcept {
    return maximum(32.f, composer_metrics(styles).menu_row_height);
}
float accessory_height(const UiResolvedThemeStyles* styles) noexcept {
    return maximum(32.f, composer_metrics(styles).control_height);
}
// Plugin composer-toolbar buttons currently published; global, so the static
// width functions and the scene agree without a scene argument.
u32 plugin_slot_count() noexcept {
    UiPluginSlotNode nodes[kMessageComposerPluginSlots]{};
    return ui_plugin_slot_toolbar(kUiPluginSlotComposerToolbar, {nodes, kMessageComposerPluginSlots});
}
inline constexpr float kComposerReplyHeight = 28.0f;
inline constexpr float kComposerTypingHeight = 20.0f;

bool reply_label_equal(const MessageComposerScene& scene, Utf8View label) noexcept {
    return utf8_equal(Utf8View{scene.reply_label, scene.reply_label_bytes}, label);
}

bool composer_can_send(const MessageComposer& model, bool channel_allows,
                       MessageComposerAttachmentInput attachments, u8 premium_type) noexcept {
    const bool present = attachments.label.bytes != 0;
    return (!present || (attachments.ready && attachments.can_attach)) &&
           message_composer_can_send(model, channel_allows, message_composer_maximum_codepoints(premium_type),
                                     present || attachments.retained_content);
}

bool typing_label_equal(const MessageComposerScene& scene, Utf8View label) noexcept {
    return utf8_equal(Utf8View{scene.typing_label, scene.typing_label_bytes}, label);
}

bool update_placeholder(MessageComposerScene* scene, const MessageComposer& model,
                        const MessageComposerProjectionContext* context) noexcept {
    const ChannelRecord* channel =
        context && context->state ? chat_state_store_find_channel(*context->state, model.channel_id)
                                  : nullptr;
    const Utf8View name =
        channel ? chat_state_store_channel_name(*context->state, *channel) : Utf8View{};
    const bool replaced = context && context->placeholder_override.bytes;
    const Utf8View prefix = replaced        ? context->placeholder_override
                            : name.bytes != 0 ? utf8_literal("Message #")
                                              : utf8_literal("Message");
    char text[kMessageComposerPlaceholderBytes];
    u32 used = minimum<u32>(prefix.bytes, sizeof(text));
    copy_bytes(text, prefix.data, used);
    for (u32 offset = 0; !replaced && offset < name.bytes;) {
        const Utf8DecodeResult decoded = utf8_decode_one(name.data + offset, name.bytes - offset);
        if (!decoded.valid || decoded.width > sizeof(text) - used)
            break;
        copy_bytes(text + used, name.data + offset, decoded.width);
        used += decoded.width;
        offset += decoded.width;
    }
    if (utf8_equal(Utf8View{scene->placeholder, scene->placeholder_bytes}, Utf8View{text, used})) {
        return false;
    }
    copy_bytes(scene->placeholder, text, used);
    scene->placeholder_bytes = static_cast<u16>(used);
    return true;
}

UiLayoutStyle root_style(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.flow = UiLayoutFlow::vertical;
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.gap = composer_metrics(styles).composer_padding;
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children);
    return style;
}

UiLayoutStyle surface_style(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = root_style(styles);
    style.padding =
        UiInsets{composer_metrics(styles).composer_padding, composer_metrics(styles).composer_padding,
                 composer_metrics(styles).composer_padding, composer_metrics(styles).composer_padding};
    return style;
}

UiLayoutStyle controls_style(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.flow = UiLayoutFlow::horizontal;
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.gap = composer_metrics(styles).composer_padding;
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    return style;
}

UiLayoutStyle reply_style(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(0.0f);
    style.padding =
        UiInsets{composer_metrics(styles).menu_gap, composer_metrics(styles).menu_padding * .5f,
                 composer_metrics(styles).menu_gap, composer_metrics(styles).menu_padding * .5f};
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_allow_zero_size | ui_layout_node_clip_children |
                                   ui_layout_node_text_content);
    return style;
}

UiLayoutStyle fixed_control_style(float width, const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(width);
    // The editable surface grows; native accessory controls use the compact row height
    // and remain vertically centered in the current composer row.
    style.height = ui_layout_fixed(accessory_height(styles));
    style.vertical_alignment = UiLayoutAlignment::center;
    return style;
}

UiLayoutStyle edit_style(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.padding = UiInsets{composer_metrics(styles).menu_gap, composer_metrics(styles).menu_padding,
                             composer_metrics(styles).menu_gap, composer_metrics(styles).menu_padding};
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                                   ui_layout_node_text_content);
    return style;
}

// Inline text gets at least the width of the complete trailing action group.
// Below that width the four trailing actions wrap after the editor instead.
float full_controls_minimum_width(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    const auto& metrics = composer_metrics(styles);
    const float accessory = accessory_width(styles), gap = metrics.composer_padding;
    const float plugins = static_cast<float>(plugin_slot_count());
    const float trailing_width = (4.f + plugins) * accessory + (3.f + plugins) * gap;
    return gap + gap + (5.f + plugins) * (accessory + gap) + trailing_width + metrics.menu_gap + metrics.menu_gap;
}

// Between the compact and the full width the GIF and sticker buttons fold
// into the emoji picker (its tabs reach both), so attach, editor, plugins,
// emoji and send keep one row with at least this much editable width.
inline constexpr float kComposerCompactEditorMinimum = 120.0f;
float compact_controls_minimum_width(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    const auto& metrics = composer_metrics(styles);
    const float plugins = static_cast<float>(plugin_slot_count());
    return metrics.composer_padding + metrics.composer_padding +
           (3.f + plugins) * (accessory_width(styles) + metrics.composer_padding) + kComposerCompactEditorMinimum +
           metrics.menu_gap + metrics.menu_gap;
}
bool compact_controls(float composer_width, const UiResolvedThemeStyles* styles) noexcept {
    return composer_width >= compact_controls_minimum_width(styles) &&
           composer_width < full_controls_minimum_width(styles);
}

float editor_padding_height(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    const auto& metrics = composer_metrics(styles);
    return metrics.composer_padding + metrics.composer_padding + metrics.menu_padding + metrics.menu_padding;
}

bool configure_controls(MessageComposerScene* scene, UiRect viewport, float chrome) noexcept {
    auto* controls_widget = ui_widget_manager_resolve(&scene->widgets, scene->controls);
    auto* edit_widget = ui_widget_manager_resolve(&scene->widgets, scene->edit);
    auto* controls = controls_widget
                         ? ui_layout_engine_resolve(&scene->widgets.layout, controls_widget->layout)
                         : nullptr;
    auto* edit =
        edit_widget ? ui_layout_engine_resolve(&scene->widgets.layout, edit_widget->layout) : nullptr;
    if (!controls || !edit)
        return false;
    const auto* styles = scene->theme_styles;
    const auto layout = [&](UiWidgetHandle handle) noexcept {
        auto* widget = ui_widget_manager_resolve(&scene->widgets, handle);
        return widget ? ui_layout_engine_resolve(&scene->widgets.layout, widget->layout) : nullptr;
    };
    auto* root = layout(scene->root);
    auto* surface_node = layout(scene->surface);
    auto* reply = layout(scene->reply);
    auto* mention = layout(scene->reply_mention);
    auto* attachments = layout(scene->attachments);
    auto* typing = layout(scene->typing_indicator);
    if (!root || !surface_node || !reply || !mention || !attachments || !typing)
        return false;
    root->spec.style = root_style(styles);
    surface_node->spec.style = surface_style(styles);
    reply->spec.style.padding = UiInsets{composer_metrics(styles).menu_gap, 0.f, 0.f, 0.f};
    // The right margin clears Cancel and a 1 DIP divider.
    mention->spec.style.margin.right = kComposerReplyHeight + 2 * composer_metrics(styles).composer_padding + 1;
    // The upload strip spans the surface; the label row keeps its inset.
    attachments->spec.style.padding =
        scene->attachment_strip && scene->attachment_strip->count ? UiInsets{} : reply_style(styles).padding;
    typing->spec.style.padding.right = composer_metrics(styles).composer_padding;
    const float width = ui_rect_width(viewport);
    // The recording bar folds the media pickers and plugins away.
    const bool voice_bar = ui_composer_voice_bar_visible(scene->voice_mode);
    const bool compact = compact_controls(width, styles) || voice_bar;
    const UiWidgetHandle accessories[]{scene->add_attachment, scene->gif, scene->sticker, scene->emoji,
                                       scene->send};
    const UiLayoutStyle accessory = fixed_control_style(accessory_width(styles), styles);
    for (u32 i = 0; i < 5; ++i) {
        auto* node = layout(accessories[i]);
        if (!node)
            return false;
        node->spec.style = accessory;
        if (compact && (i == 1 || i == 2)) // GIF and sticker
            node->spec.style.flags = static_cast<u16>(node->spec.style.flags & ~ui_layout_node_visible);
    }
    for (u32 i = 0; i < kMessageComposerPluginSlots; ++i) {
        auto* plugin = layout(scene->plugin_slots[i]);
        if (!plugin)
            return false;
        plugin->spec.style = accessory;
        if (i >= scene->plugin_count || voice_bar)
            plugin->spec.style.flags = static_cast<u16>(plugin->spec.style.flags & ~ui_layout_node_visible);
    }
    const float toolbar = message_composer_scene_toolbar_height(width, styles);
    controls->spec.style = controls_style(styles);
    edit->spec.style = edit_style(styles);
    scene->widgets.layout.wrap_nodes = toolbar != 0.f ? 1u : 0u;
    if (toolbar != 0.f) {
        const auto surface = surface_style(styles);
        controls->spec.style.flow = UiLayoutFlow::horizontal_wrap;
        // A fixed editor consumes the remainder of the first row; the wrap
        // engine then fits the trailing actions into the available width.
        edit->spec.style.width =
            ui_layout_fixed(message_composer_scene_text_width(width, styles) +
                            edit->spec.style.padding.left + edit->spec.style.padding.right);
        edit->spec.style.height = ui_layout_fixed(
            maximum(accessory.height.value, ui_rect_height(viewport) - chrome - toolbar -
                                                surface.padding.top - surface.padding.bottom));
        controls->measure_constraint = {maximum(0.f, width - surface.padding.left - surface.padding.right),
                                        kMaximumUiCoordinate};
    }
    return true;
}

u64 projection_input_signature(const MessageComposerScene& scene, const MessageComposer& model,
                               const MessageComposerProjectionContext* context) noexcept {
    u64 signature = hash64_mix(kHash64Seed, model.document_identity);
    signature = hash64_mix(signature, model.text_revision);
    signature = hash64_mix(signature, model.channel_id);
    signature = hash64_mix(signature, model.anchor);
    signature = hash64_mix(signature, model.caret);
    signature = hash64_mix(signature, context ? context->guild_id : 0);
    signature = hash64_mix(signature, context ? context->command.name_bytes : 0);
    for (u32 n = 0; context && n < context->option_chip_count; ++n) {
        const auto& chip = context->option_chips[n];
        signature = hash64_mix(signature, (u64{chip.head_first} << 48) | (u64{chip.name_first} << 32) |
                                              (u64{chip.key_last} << 16) | chip.value_last);
        signature = hash64_mix(signature, (u64{chip.end_last} << 8) | chip.flags);
    }
    signature = hash64_mix(signature, context ? reinterpret_cast<usize>(context->users) : 0);
    signature = hash64_mix(signature, context ? reinterpret_cast<usize>(context->state) : 0);
    if (context && context->users)
        signature = hash64_mix(signature, context->users->revision);
    if (context && context->state) {
        signature = hash64_mix(signature, context->state->guild_mutations);
        signature = hash64_mix(signature, context->state->channel_mutations);
        signature = hash64_mix(signature, context->state->permission_mutations);
        signature = hash64_mix(signature, context->state->emoji_mutations);
        signature = hash64_mix(signature, context->state->compactions);
        // Role upserts have no store-wide revision. Name allocation/counts
        // also reveal newly resolvable tokens; retained visible role records
        // provide bounded dependencies for in-place color/name changes.
        signature = hash64_mix(signature, context->state->role_count);
        signature = hash64_mix(signature, context->state->name_used);
        signature = hash64_mix(signature, context->state->name_wasted);
        for (u32 index = 0; index < scene.projection_span_count; ++index) {
            const auto& span = scene.projection_spans[index];
            if (span.kind != MessageComposerProjectionKind::role)
                continue;
            const auto* role =
                chat_state_store_find_role(*context->state, context->guild_id, span.target);
            signature = hash64_mix(signature, span.target);
            signature = hash64_mix(signature, role ? role->guild_id : 0);
            signature = hash64_mix(signature, role ? role->id : 0);
            signature = hash64_mix(signature, role ? role->color : 0);
            signature = hash64_mix(signature, role ? role->name_offset : 0);
            signature = hash64_mix(signature, role ? role->name_bytes : 0);
        }
    }
    return signature;
}

UiLayoutStyle typing_style(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(kComposerTypingHeight);
    style.padding = UiInsets{kUiTypingLabelInset, 0.0f, composer_metrics(styles).composer_padding, 0.0f};
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                                   ui_layout_node_text_content);
    return style;
}

// A collapsible banner row (reply, attachments, typing): zero height unless shown.
constexpr u16 kComposerInteractiveFlags = static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable);
void banner_row(UiLayoutNode* node, bool shown, float height) noexcept {
    node->spec.style.height = ui_layout_fixed(shown ? height : 0.0f);
    node->spec.style.flags = static_cast<u16>(
        ui_layout_node_allow_zero_size | ui_layout_node_clip_children | ui_layout_node_text_content |
        (shown ? ui_layout_node_visible : 0));
}

UiWidgetHandle control_handle(const MessageComposerScene& scene, MessageComposerControl control) noexcept {
    switch (control) {
    case MessageComposerControl::add_attachment:
        return scene.add_attachment;
    case MessageComposerControl::attachments:
        return scene.attachments;
    case MessageComposerControl::edit:
        return scene.edit;
    case MessageComposerControl::emoji:
        return scene.emoji;
    case MessageComposerControl::gif:
        return scene.gif;
    case MessageComposerControl::sticker:
        return scene.sticker;
    case MessageComposerControl::send:
        return scene.send;
    case MessageComposerControl::cancel_reply:
        return scene.reply_cancel;
    case MessageComposerControl::command_close:
        return scene.command_close;
    case MessageComposerControl::reply_mention:
        return scene.reply_mention;
    case MessageComposerControl::typing_indicator:
        return scene.typing_indicator;
    default:
        return ui_widget_invalid_handle();
    }
}

MessageComposerSceneResult failure(MessageComposerScene* scene, MessageComposerSceneError error) noexcept {
    saturating_increment_if(scene, scene->rejected);
    return MessageComposerSceneResult{error, false};
}

struct ProjectedToken {
    Utf8View label;
    u64 id;
    u32 source_last;
    u32 color;
    MessageComposerProjectionKind kind;
    char marker;
    bool animated;
};

bool decimal_id(Utf8View source, u32 first, u32 last, u64* value) noexcept {
    if (first >= last || last > source.bytes) {
        return false;
    }
    u64 parsed = 0;
    for (u32 index = first; index < last; ++index) {
        const char character = source.data[index];
        if (character < '0' || character > '9') {
            return false;
        }
        const u64 digit = static_cast<u64>(character - '0');
        if (parsed > (~0ull - digit) / 10ull) {
            return false;
        }
        parsed = parsed * 10ull + digit;
    }
    *value = parsed;
    return parsed != 0;
}

bool projected_token_at(Utf8View source, u32 first, const MessageComposerProjectionContext* context,
                        ProjectedToken* token) noexcept {
    if (first >= source.bytes || source.data[first] != '<' || source.bytes - first < 4) {
        return false;
    }
    u32 cursor = first + 1;
    MessageComposerProjectionKind kind{};
    char marker = 0;
    bool animated = false;
    if (source.data[cursor] == '#') {
        kind = MessageComposerProjectionKind::channel;
        marker = '#';
        ++cursor;
    } else if (source.data[cursor] == '@') {
        marker = '@';
        ++cursor;
        if (cursor < source.bytes && source.data[cursor] == '!') {
            ++cursor;
        }
        if (cursor < source.bytes && source.data[cursor] == '&') {
            kind = MessageComposerProjectionKind::role;
            ++cursor;
        } else {
            kind = MessageComposerProjectionKind::user;
        }
    } else if (source.data[cursor] == ':' || (source.data[cursor] == 'a' && cursor + 1 < source.bytes &&
                                              source.data[cursor + 1] == ':')) {
        kind = MessageComposerProjectionKind::emoji;
        animated = source.data[cursor] == 'a';
        cursor += animated ? 2 : 1;
        const u32 name_first = cursor;
        while (cursor < source.bytes && source.data[cursor] != ':') {
            const char c = source.data[cursor++];
            if (!(ascii_is_alnum(c) || c == '_'))
                return false;
        }
        if (cursor == name_first || cursor == source.bytes)
            return false;
        ++cursor;
    } else {
        return false;
    }
    const u32 digits_first = cursor;
    while (cursor < source.bytes && source.data[cursor] >= '0' && source.data[cursor] <= '9') {
        ++cursor;
    }
    if (cursor >= source.bytes || source.data[cursor] != '>') {
        return false;
    }
    u64 id = 0;
    if (!decimal_id(source, digits_first, cursor, &id)) {
        return false;
    }
    if (kind != MessageComposerProjectionKind::emoji && (!context || !context->state))
        return false;
    Utf8View label{};
    u32 color = 0;
    if (kind == MessageComposerProjectionKind::user) {
        const UserRecord* user =
            context->users ? user_store_find(*context->users, id) : nullptr;
        label = user ? user_store_name(*context->users, *user) : Utf8View{};
    } else if (kind == MessageComposerProjectionKind::channel) {
        const ChannelRecord* channel = chat_state_store_find_channel(*context->state, id);
        label = channel ? chat_state_store_channel_name(*context->state, *channel) : Utf8View{};
    } else if (kind == MessageComposerProjectionKind::emoji) {
        label = utf8_literal("\xEF\xBF\xBC");
    } else {
        const RoleRecord* role =
            chat_state_store_find_role(*context->state, context->guild_id, id);
        label = role ? chat_state_store_role_name(*context->state, *role) : Utf8View{};
        color = role ? role->color : 0;
    }
    if (label.bytes == 0 || !label.data) {
        return false;
    }
    *token = ProjectedToken{label, id, cursor + 1, color, kind, marker, animated};
    return true;
}

u32 display_offset_from_source(const MessageComposerScene& scene, u32 source) noexcept {
    if (source <= scene.display_first) {
        return 0;
    }
    const u32 source_last = scene.display_source_last;
    if (source >= source_last) {
        return scene.display_bytes;
    }
    i64 adjustment = -static_cast<i64>(scene.display_first);
    for (u32 index = 0; index < scene.projection_span_count; ++index) {
        const MessageComposerProjectionSpan& span = scene.projection_spans[index];
        if (source <= span.source_first) {
            break;
        }
        if (source < span.source_last) {
            if (span.kind == MessageComposerProjectionKind::regional_flag)
                return span.display_first + source - span.source_first;
            const u32 source_middle = span.source_first + (span.source_last - span.source_first) / 2;
            return source <= source_middle ? span.display_first : span.display_last;
        }
        adjustment += static_cast<i64>(span.display_last - span.display_first) -
                      static_cast<i64>(span.source_last - span.source_first);
    }
    const i64 projected = static_cast<i64>(source) + adjustment;
    return projected <= 0 ? 0 : minimum(scene.display_bytes, static_cast<u32>(projected));
}

void update_source_styles(MessageComposerScene* scene, const MessageComposer& model) noexcept {
    if (scene->styles_document == model.document_identity && scene->styles_revision == model.text_revision)
        return;
    scene->source_style_count = message_markdown_source_styles(
        message_composer_text(model), {scene->source_styles, kTextStyleSpanMaximum});
    scene->styles_document = model.document_identity;
    scene->styles_revision = model.text_revision;
}
void append_display_style(MessageComposerScene* scene, u32 first, u32 count, u32 flags) noexcept {
    if (!count || !flags)
        return;
    if (scene->display_style_count) {
        auto& last = scene->display_styles[scene->display_style_count - 1];
        if (last.flags == flags && last.byte_offset + last.byte_count == first) {
            last.byte_count += count;
            return;
        }
    }
    if (scene->display_style_count < kTextStyleSpanMaximum)
        scene->display_styles[scene->display_style_count++] = {first, count, flags};
}
bool display_style_available(const MessageComposerScene& scene, u32 first, u32 flags) noexcept {
    if (!flags || scene.display_style_count < kTextStyleSpanMaximum)
        return true;
    const auto& last = scene.display_styles[scene.display_style_count - 1];
    return last.flags == flags && last.byte_offset + last.byte_count == first;
}
// Start of the UTF-8 scalar that ends at offset (offset > 0).
u32 scalar_start_before(const char* text, u32 offset) noexcept {
    u32 start = offset - 1;
    while (start != 0 && (static_cast<u8>(text[start]) & 0xC0u) == 0x80u) {
        --start;
    }
    return start;
}
void project_display(MessageComposerScene* scene, const MessageComposer& model,
                     const MessageComposerProjectionContext* context) noexcept {
    update_source_styles(scene, model);
    scene->display_style_count = 0;
    u32 first = model.caret;
    u32 before = 0;
    while (first != 0 && before < kMessageComposerDisplayBytes / 2) {
        const u32 previous = scalar_start_before(model.text, first);
        if (model.caret - previous > kMessageComposerDisplayBytes / 2) {
            break;
        }
        first = previous;
        before = model.caret - first;
    }
    u32 end = model.caret;
    while (end < model.bytes) {
        const Utf8DecodeResult decoded = utf8_decode_one(model.text + end, model.bytes - end);
        if (!decoded.valid || decoded.width > kMessageComposerDisplayBytes - (end - first)) {
            break;
        }
        end += decoded.width;
    }
    // When the caret is at the end, use the complete display window rather
    // than leaving half of it empty.
    while (end == model.bytes && first != 0 && end - first < kMessageComposerDisplayBytes) {
        const u32 previous = scalar_start_before(model.text, first);
        if (end - previous > kMessageComposerDisplayBytes) {
            break;
        }
        first = previous;
    }
    scene->display_first = first;
    scene->display_bytes = 0;
    scene->projection_span_count = 0;
    u64 signature = kHash64Seed;
    const Utf8View source{model.text, model.bytes};
    u32 cursor = first;
    u32 style_index = 0;
    u32 flag_cursor = 0;
    RegionalFlagRange flag{};
    bool have_flag = regional_flag_next(source, &flag_cursor, &flag);
    while (cursor < end) {
        while (have_flag && flag.first_byte < cursor)
            have_flag = regional_flag_next(source, &flag_cursor, &flag);
        while (style_index < scene->source_style_count &&
               scene->source_styles[style_index].byte_offset +
                       scene->source_styles[style_index].byte_count <=
                   cursor)
            ++style_index;
        const u32 style = (context && cursor < context->command.name_bytes ? text_style_bold : 0) |
                          (style_index < scene->source_style_count &&
                                  scene->source_styles[style_index].byte_offset <= cursor
                              ? scene->source_styles[style_index].flags
                              : 0);
        // A typed option of the selected command: its head (blank and "name:")
        // shows the name between no-break spaces that pad it, its end marker the
        // value's end padding.
        u32 chip = 0;
        bool head = false;
        while (context && chip < context->option_chip_count) {
            const auto& option = context->option_chips[chip];
            if (option.head_first == cursor || (option.end_last > option.value_last && option.value_last == cursor)) {
                head = option.head_first == cursor;
                break;
            }
            ++chip;
        }
        if (context && chip < context->option_chip_count) {
            const auto& option = context->option_chips[chip];
            const Utf8View name{source.data + option.name_first, option.key_last - option.name_first - 1u};
            const u32 last = head ? option.key_last : option.end_last;
            const u32 spaces = head ? kMessageComposerChipMargin + 3 * kMessageComposerChipPadding
                                    : kMessageComposerChipPadding + kMessageComposerChipMargin;
            if (last <= end && option.key_last > option.name_first &&
                scene->projection_span_count < kMessageComposerProjectionSpanCapacity &&
                spaces * 2 + (head ? name.bytes : 0) <= kMessageComposerDisplayBytes - scene->display_bytes) {
                const u32 display_first = scene->display_bytes;
                const auto space = [&](u32 count) noexcept {
                    for (u32 n = 0; n < count; ++n) {
                        scene->display[scene->display_bytes++] = '\xC2';
                        scene->display[scene->display_bytes++] = '\xA0';
                    }
                };
                if (head) {
                    space(kMessageComposerChipMargin + kMessageComposerChipPadding);
                    copy_bytes(scene->display + scene->display_bytes, name.data, name.bytes);
                    scene->display_bytes += name.bytes;
                    space(2 * kMessageComposerChipPadding);
                } else {
                    space(kMessageComposerChipPadding + kMessageComposerChipMargin);
                }
                signature = hash64_bytes(signature, scene->display + display_first, scene->display_bytes - display_first);
                scene->projection_spans[scene->projection_span_count] = MessageComposerProjectionSpan{
                    cursor, last, display_first, scene->display_bytes, 0,
                    head ? MessageComposerProjectionKind::command_option
                         : MessageComposerProjectionKind::command_option_end,
                    option.flags, false};
                scene->atomic_spans[scene->projection_span_count++] = MessageComposerAtomicSpan{cursor, last};
                append_display_style(scene, display_first, scene->display_bytes - display_first, style);
                cursor = last;
                continue;
            }
        }
        ProjectedToken token{};
        const bool flag_token = have_flag && flag.first_byte == cursor && flag.end_byte <= end;
        if (flag_token)
            token = {{source.data + cursor, flag.end_byte - cursor},
                     flag.sequence,
                     flag.end_byte,
                     0,
                     MessageComposerProjectionKind::regional_flag,
                     0,
                     false};
        const bool projected = !(style & text_style_monospace) &&
                               (flag_token || projected_token_at(source, cursor, context, &token)) &&
                               (!flag_token || display_style_available(*scene, scene->display_bytes,
                                                                       style | text_style_custom_emoji)) &&
                               token.source_last <= end &&
                               scene->projection_span_count < kMessageComposerProjectionSpanCapacity &&
                               token.label.bytes + 1 <= kMessageComposerDisplayBytes - scene->display_bytes;
        if (projected) {
            const u32 display_first = scene->display_bytes;
            if (token.marker) {
                scene->display[scene->display_bytes++] = token.marker;
                signature = hash64_mix(signature, static_cast<u8>(token.marker));
            }
            for (u32 byte = 0; byte < token.label.bytes; ++byte) {
                const char value = token.label.data[byte];
                scene->display[scene->display_bytes++] = value;
                signature = hash64_mix(signature, static_cast<u8>(value));
            }
            scene->projection_spans[scene->projection_span_count++] = MessageComposerProjectionSpan{
                cursor,      token.source_last, display_first, scene->display_bytes,
                token.color, token.kind,        token.id,      token.animated};
            // Flags retain the model's existing Unicode caret/delete policy.
            // The media projection changes no source bytes or atomic boundary.
            scene->atomic_spans[scene->projection_span_count - 1] =
                token.kind == MessageComposerProjectionKind::regional_flag
                    ? MessageComposerAtomicSpan{}
                    : MessageComposerAtomicSpan{cursor, token.source_last};
            append_display_style(scene, display_first, scene->display_bytes - display_first,
                                 style | ((token.kind == MessageComposerProjectionKind::emoji ||
                                           token.kind == MessageComposerProjectionKind::regional_flag)
                                              ? text_style_custom_emoji
                                              : text_style_mention));
            cursor = token.source_last;
            continue;
        }
        const Utf8DecodeResult decoded = utf8_decode_one(model.text + cursor, end - cursor);
        if (!decoded.valid || decoded.width > kMessageComposerDisplayBytes - scene->display_bytes) {
            break;
        }
        append_display_style(scene, scene->display_bytes, decoded.width, style);
        // A command option end marker (U+2063) outside a chip, until the next
        // command sync removes it, shows as a hair space of the same UTF-8 and
        // UTF-16 length: the edit layout cannot place a caret after a hidden
        // default-ignorable character.
        const bool marker = decoded.codepoint == 0x2063u;
        for (u32 byte = 0; byte < decoded.width; ++byte) {
            const char value = marker ? "\xE2\x80\x8A"[byte] : model.text[cursor + byte];
            scene->display[scene->display_bytes++] = value;
            signature = hash64_mix(signature, static_cast<u8>(value));
        }
        cursor += decoded.width;
    }
    scene->display_source_last = cursor;
    scene->display_caret = display_offset_from_source(*scene, model.caret);
    scene->display_anchor = display_offset_from_source(*scene, model.anchor);
    signature = hash64_mix(signature, scene->display_bytes);
    for (u32 index = 0; index < scene->projection_span_count; ++index) {
        const MessageComposerProjectionSpan& span = scene->projection_spans[index];
        signature = hash64_mix(signature, span.source_first);
        signature = hash64_mix(signature, span.source_last);
        signature = hash64_mix(signature, span.display_first);
        signature = hash64_mix(signature, span.display_last);
        signature = hash64_mix(signature, span.color);
        signature = hash64_mix(signature, static_cast<u8>(span.kind));
        signature = hash64_mix(signature, span.target);
        signature = hash64_mix(signature, span.animated);
    }
    signature = hash64_mix(signature, context ? context->command.name_bytes : 0);
    scene->display_signature = hash64_mix(signature, scene->projection_span_count);
}

// Copies tile strings into scene storage; true when presentation differs.
bool store_attachment_tiles(MessageComposerScene* scene, ConstSlice<UiComposerAttachmentTile> tiles) noexcept {
    auto& strip = *scene->attachment_strip;
    u64 signature = hash64_mix(kHash64Seed, tiles.count);
    for (u32 i = 0; i < tiles.count; ++i) {
        const auto& source = tiles[i];
        u32 bytes = 0;
        while (source.filename.data && bytes < source.filename.bytes) {
            const auto scalar = utf8_decode_one(source.filename.data + bytes, source.filename.bytes - bytes);
            if (!scalar.valid || bytes + scalar.width > kMessageComposerAttachmentNameBytes)
                break;
            bytes += scalar.width;
        }
        auto& target = strip.tiles[i];
        target = source;
        copy_bytes(strip.names[i], source.filename.data, bytes);
        target.filename = {strip.names[i], bytes};
        signature = hash64_mix(signature, source.id);
        signature = hash64_mix(signature, source.bytes);
        signature = hash64_mix(signature, source.uploaded_bytes);
        signature = hash64_mix(signature, media_asset_hash(source.preview));
        signature = hash64_mix(signature, source.duration_ms);
        signature = hash64_mix(signature, static_cast<u64>(source.kind) | static_cast<u64>(source.phase) << 8 |
                                              static_cast<u64>(source.spoiler) << 16 |
                                              static_cast<u64>(source.described) << 17 |
                                              static_cast<u64>(source.animated) << 18 |
                                              static_cast<u64>(source.preview_ready) << 19 |
                                              static_cast<u64>(source.can_retry) << 20);
        for (u32 b = 0; b < bytes; ++b)
            signature = hash64_mix(signature, static_cast<u8>(strip.names[i][b]));
    }
    const bool changed = strip.signature != signature || strip.count != tiles.count;
    strip.count = tiles.count;
    strip.signature = signature;
    if (strip.hovered >= strip.count) {
        strip.hovered = kUiComposerAttachmentNoTile;
        strip.hovered_part = UiComposerAttachmentPart::none;
    }
    return changed;
}

void clamp_attachment_scroll(MessageComposerScene* scene) noexcept {
    auto& strip = *scene->attachment_strip;
    const auto* node = ui_widget_manager_layout_node(scene->widgets, scene->attachments);
    strip.scroll = node && strip.count
                       ? ui_composer_attachment_strip_clamp_scroll(node->content_rectangle, strip.count, strip.scroll)
                       : 0.f;
}

} // namespace

bool message_composer_scene_initialize(MessageComposerScene* scene, Slice<UiWidget> widgets,
                                       Slice<UiLayoutNode> layout_nodes) noexcept {
    const bool own_widgets =
        !widgets.data && widgets.count == 0 && !layout_nodes.data && layout_nodes.count == 0;
    if (!scene || scene->owned_storage.base ||
        (!own_widgets && (widgets.count < kMessageComposerWidgetCapacity ||
                          layout_nodes.count < kMessageComposerWidgetCapacity))) {
        return false;
    }
    reset_value<MessageComposerScene>(scene);
    if (!initialize_owned_storage(scene)) {
        message_composer_scene_release(scene);
        return false;
    }
    if (own_widgets) {
        widgets =
            Slice<UiWidget>{arena_allocate<UiWidget>(&scene->owned_storage, kMessageComposerWidgetCapacity),
                            kMessageComposerWidgetCapacity};
        layout_nodes = Slice<UiLayoutNode>{
            arena_allocate<UiLayoutNode>(&scene->owned_storage, kMessageComposerWidgetCapacity),
            kMessageComposerWidgetCapacity};
    }
    if (!widgets.data || !layout_nodes.data ||
        !ui_widget_manager_initialize(&scene->widgets, widgets, layout_nodes)) {
        message_composer_scene_release(scene);
        return false;
    }
    UiWidgetBuilder b(scene->widgets);
    constexpr u16 control_flags = static_cast<u16>(ui_widget_enabled | ui_widget_hit_test);
    constexpr u16 focus_flags = static_cast<u16>(control_flags | ui_widget_focusable);
    const UiWidgetResult root = b.widget(ui_widget_invalid_handle(), {kComposerRootId, utf8_literal("MessageComposer")},
                                         UiWidgetKind::container, ui_widget_none, root_style());
    const UiWidgetResult typing = b.widget(root.handle, {kComposerTypingId, utf8_literal("TypingIndicator")},
                                           UiWidgetKind::text, ui_widget_none, typing_style());
    const UiWidgetResult surface = b.widget(root.handle, {kComposerSurfaceId, utf8_literal("ComposerSurface")},
                                            UiWidgetKind::container, ui_widget_none, surface_style());
    UiLayoutStyle banner_style = reply_style();
    banner_style.padding = UiInsets{kUiMenuRowHorizontalPadding, 0.f, 0.f, 0.f};
    // The option bar leads the client's stacked bars (845207 tB before the reply bar).
    const UiWidgetResult command_bar = b.widget(surface.handle, {kComposerCommandBarId, utf8_literal("CommandBar")},
                                                UiWidgetKind::container, ui_widget_none, banner_style);
    UiLayoutStyle command_close_style = fixed_control_style(kMessageComposerCommandBarHeight);
    command_close_style.height = ui_layout_fixed(kMessageComposerCommandBarHeight);
    command_close_style.horizontal_alignment = UiLayoutAlignment::end;
    command_close_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children);
    const UiWidgetResult command_close = b.widget(command_bar.handle, {kComposerCommandCloseId, utf8_literal("Close")},
                                                  UiWidgetKind::button, ui_widget_none, command_close_style);
    const UiWidgetResult reply = b.widget(surface.handle, {kComposerReplyId, utf8_literal("ReplyBanner")},
                                          UiWidgetKind::container, ui_widget_none, banner_style);
    // The "@ ON" switch is padded 8x12: 16 DIP icon, 4 DIP gap, bold label.
    UiLayoutStyle mention_style = fixed_control_style(kMessageComposerReplyMentionWidth);
    mention_style.height = ui_layout_fixed(kComposerReplyHeight);
    mention_style.horizontal_alignment = UiLayoutAlignment::end;
    mention_style.margin.right = kComposerReplyHeight + 2 * kComposerRowGap + 1;
    mention_style.flags = ui_layout_node_clip_children;
    const UiWidgetResult mention = b.widget(reply.handle, {kComposerReplyMentionId, utf8_literal("ReplyMention")},
                                            UiWidgetKind::button, ui_widget_none, mention_style);
    UiLayoutStyle cancel_style = fixed_control_style(kComposerReplyHeight);
    cancel_style.height = ui_layout_fixed(kComposerReplyHeight);
    cancel_style.horizontal_alignment = UiLayoutAlignment::end;
    cancel_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children);
    const UiWidgetResult cancel = b.widget(reply.handle, {kComposerReplyCancelId, utf8_literal("CancelReply")},
                                           UiWidgetKind::button, ui_widget_none, cancel_style);
    const UiWidgetResult attachments = b.widget(surface.handle, {kComposerAttachmentsId, utf8_literal("Attachments")},
                                                UiWidgetKind::button, ui_widget_none, reply_style());
    const UiWidgetResult controls = b.widget(surface.handle, {kComposerControlsId, utf8_literal("ComposerControls")},
                                             UiWidgetKind::container, ui_widget_enabled, controls_style());
    const UiLayoutStyle row_control = fixed_control_style(kUiMenuRowHeight);
    const UiWidgetResult attachment = b.widget(controls.handle, {kComposerAttachmentId, utf8_literal("AddAttachment")},
                                               UiWidgetKind::button, control_flags, row_control);
    const UiWidgetResult edit = b.widget(controls.handle, {kComposerEditId, utf8_literal("MessageEdit")},
                                         UiWidgetKind::text_field, focus_flags, edit_style());
    UiWidgetResult plugins[kMessageComposerPluginSlots]{};
    UiLayoutStyle hidden = row_control;
    hidden.flags = static_cast<u16>(hidden.flags & ~ui_layout_node_visible);
    for (u32 i = 0; i < kMessageComposerPluginSlots; ++i)
        plugins[i] = b.widget(controls.handle, {kComposerPluginId + i, utf8_literal("PluginSlot")},
                              UiWidgetKind::button, ui_widget_none, hidden);
    const UiWidgetResult gif = b.widget(controls.handle, {kComposerGifId, utf8_literal("GIFs")},
                                        UiWidgetKind::button, focus_flags, row_control);
    const UiWidgetResult sticker = b.widget(controls.handle, {kComposerStickerId, utf8_literal("Stickers")},
                                            UiWidgetKind::button, focus_flags, row_control);
    const UiWidgetResult emoji = b.widget(controls.handle, {kComposerEmojiId, utf8_literal("Emoji")},
                                          UiWidgetKind::button, focus_flags, row_control);
    const UiWidgetResult send = b.widget(controls.handle, {kComposerSendId, utf8_literal("Send")},
                                         UiWidgetKind::button, focus_flags, row_control);
    if (!b.ok()) {
        message_composer_scene_release(scene);
        return false;
    }
    scene->root = root.handle;
    scene->surface = surface.handle;
    scene->reply = reply.handle;
    scene->reply_mention = mention.handle;
    scene->reply_cancel = cancel.handle;
    scene->command_bar = command_bar.handle;
    scene->command_close = command_close.handle;
    scene->attachments = attachments.handle;
    scene->controls = controls.handle;
    scene->add_attachment = attachment.handle;
    scene->edit = edit.handle;
    scene->emoji = emoji.handle;
    scene->gif = gif.handle;
    scene->sticker = sticker.handle;
    scene->send = send.handle;
    scene->typing_indicator = typing.handle;
    for (u32 i = 0; i < kMessageComposerPluginSlots; ++i)
        scene->plugin_slots[i] = plugins[i].handle;
    return true;
}

bool message_composer_scene_initialize_owned(MessageComposerScene* scene) noexcept {
    return message_composer_scene_initialize(scene, {}, {});
}

bool message_composer_scene_bind_theme_styles(MessageComposerScene* scene,
                                              const UiResolvedThemeStyles* styles) noexcept {
    if (!scene || (styles && !styles->ready))
        return false;
    const u32 revision = styles ? styles->geometry_revision : 0;
    if (scene->theme_styles == styles && scene->observed_geometry_revision == revision)
        return false;
    scene->theme_styles = styles;
    scene->observed_geometry_revision = revision;
    scene->measured_row_height = 0.f;
    scene->geometry_dirty = true;
    return true;
}

float message_composer_scene_minimum_inline_width(const UiResolvedThemeStyles* styles) noexcept {
    return full_controls_minimum_width(styles);
}
float message_composer_scene_minimum_compact_width(const UiResolvedThemeStyles* styles) noexcept {
    return compact_controls_minimum_width(styles);
}
bool message_composer_scene_control_visible(const MessageComposerScene& scene,
                                            MessageComposerControl control) noexcept {
    const auto* node = message_composer_scene_layout_node(scene, control);
    return node && node->visible;
}

float message_composer_scene_text_width(float composer_width,
                                        const UiResolvedThemeStyles* styles) noexcept {
    const auto& metrics = composer_metrics(styles);
    const float plugins = static_cast<float>(plugin_slot_count());
    const float actions = composer_width >= full_controls_minimum_width(styles) ? 5.f + plugins
                          : compact_controls(composer_width, styles)            ? 3.f + plugins
                                                                                : 1.f;
    return maximum(0.f, composer_width - metrics.composer_padding - metrics.composer_padding -
                            actions * (accessory_width(styles) + metrics.composer_padding) - metrics.menu_gap -
                            metrics.menu_gap);
}

float message_composer_scene_toolbar_height(float composer_width,
                                            const UiResolvedThemeStyles* styles) noexcept {
    if (composer_width <= 0.f || composer_width >= compact_controls_minimum_width(styles))
        return 0.f;
    const float gap = composer_metrics(styles).composer_padding;
    const float available = maximum(0.f, composer_width - gap - gap);
    const u32 trailing = 4u + plugin_slot_count();
    const u32 columns =
        minimum(trailing, maximum(1u, static_cast<u32>((available + gap) / (accessory_width(styles) + gap))));
    const u32 rows = (trailing + columns - 1u) / columns;
    return static_cast<float>(rows) * (accessory_height(styles) + gap);
}

static float composer_row_height(float natural, const UiResolvedThemeStyles* styles) noexcept {
    const auto& metrics = composer_metrics(styles);
    const float minimum_height =
        maximum(kMessageComposerMinimumHeight,
                maximum(32.f, metrics.control_height) + 2.f * metrics.composer_padding);
    return minimum(kMessageComposerMaximumHeight, maximum(minimum_height, natural));
}

float message_composer_scene_measured_height(const MessageComposerScene& scene,
                                             float text_height) noexcept {
    const auto* styles = scene.theme_styles;
    const float extras = message_composer_scene_chrome_height(
        scene.reply_message_id != 0, scene.typing_label_bytes != 0, message_composer_scene_attachment_row(scene),
        styles, scene.command_bar_name_bytes != 0);
    return composer_row_height(text_height + editor_padding_height(styles), styles) +
           message_composer_scene_toolbar_height(ui_rect_width(scene.viewport), styles) + extras;
}

float message_composer_scene_preferred_height(const MessageComposer& model, float composer_width,
                                              bool reply_visible, bool typing_visible,
                                              u8 attachments_visible,
                                              const UiResolvedThemeStyles* styles) noexcept {
    const float natural =
        message_composer_preferred_height(model, message_composer_scene_text_width(composer_width, styles));
    return composer_row_height(natural + editor_padding_height(styles) - editor_padding_height(), styles) +
           message_composer_scene_toolbar_height(composer_width, styles) +
           message_composer_scene_chrome_height(reply_visible, typing_visible, attachments_visible, styles);
}

float message_composer_scene_cached_height(const MessageComposerScene& scene, bool reply, bool typing,
                                           u8 attachments, float composer_width, bool command_bar) noexcept {
    const auto* styles = scene.theme_styles;
    const float width = composer_width > 0.f ? composer_width : ui_rect_width(scene.viewport);
    return composer_row_height(scene.measured_row_height, styles) +
           message_composer_scene_toolbar_height(width, styles) +
           message_composer_scene_chrome_height(reply, typing, attachments, styles, command_bar);
}

MessageComposerSceneResult message_composer_scene_update(
    MessageComposerScene* scene, MessageComposer* model, UiRect viewport, bool channel_allows_send,
    u64 reply_message_id, Utf8View reply_label, const MessageComposerProjectionContext* projection,
    Utf8View typing_label, MessageComposerAttachmentInput attachments, bool sending_restricted,
    bool mention_reply_author, bool reply_mention_visible, bool status_is_error,
    bool reply_sendable, u8 status_tone) noexcept {
    if (!scene || !scene->widgets.storage.data || !scene->display || !scene->reply_label ||
        !scene->typing_label || !scene->attachment_strip || !scene->placeholder ||
        !scene->projection_spans || !ui_rect_valid_or_empty(viewport) ||
        (model && (!model->text || model->bytes > model->capacity || model->anchor > model->bytes ||
                   model->caret > model->bytes)) ||
        (!reply_label.data && reply_label.bytes != 0) ||
        reply_label.bytes > kMessageComposerReplyLabelBytes ||
        (reply_label.bytes != 0 && !utf8_validate(reply_label).valid) ||
        (!typing_label.data && typing_label.bytes != 0) ||
        typing_label.bytes > kMessageTypingLabelMaximumBytes ||
        (typing_label.bytes != 0 && !utf8_validate(typing_label).valid) ||
        ((reply_message_id == 0) != (reply_label.bytes == 0)) ||
        attachments.label.bytes > kMessageComposerReplyLabelBytes ||
        (!attachments.label.data && attachments.label.bytes) ||
        (attachments.label.bytes && !utf8_validate(attachments.label).valid) ||
        attachments.tiles.count > kUiComposerAttachmentMaximum ||
        (!attachments.tiles.data && attachments.tiles.count)) {
        return failure(scene, MessageComposerSceneError::invalid_input);
    }
    message_composer_scene_bind_theme_styles(scene, scene->theme_styles);
    if (projection && projection->state)
        scene->premium_type = projection->state->self_premium_type;
    if (scene->model != model) {
        if (scene->model && scene->model->atomic_spans.data == scene->atomic_spans)
            reset_value(&scene->model->atomic_spans);
        scene->styles_document = scene->styles_revision = 0;
    }
    if (!model || !ui_rect_has_area(viewport)) {
        ui_widget_manager_block_input(&scene->widgets, scene->root, true);
        const bool changed =
            scene->active || scene->model != model || !ui_rect_equal(scene->viewport, viewport);
        scene->model = model;
        scene->viewport = viewport;
        scene->active = false;
        reset_value(&scene->command);
        scene->channel_allows_send = false;
        scene->reply_message_id = 0;
        scene->reply_mention_visible = false;
        scene->display_signature = 0;
        scene->reply_label_bytes = 0;
        scene->typing_label_bytes = 0;
        scene->attachment_label_bytes = 0;
        scene->attachment_strip->count = 0;
        scene->attachment_strip->hovered = kUiComposerAttachmentNoTile;
        scene->attachments_ready = scene->can_attach = false;
        scene->retained_content = scene->status_is_error = false;
        scene->status_tone = 0;
        return MessageComposerSceneResult{MessageComposerSceneError::none, changed};
    }
    ui_widget_manager_block_input(&scene->widgets, scene->root, false);
    UiWidget* send = ui_widget_manager_resolve(&scene->widgets, scene->send);
    UiWidget* edit = ui_widget_manager_resolve(&scene->widgets, scene->edit);
    UiWidget* emoji = ui_widget_manager_resolve(&scene->widgets, scene->emoji);
    UiWidget* gif = ui_widget_manager_resolve(&scene->widgets, scene->gif);
    UiWidget* sticker = ui_widget_manager_resolve(&scene->widgets, scene->sticker);
    UiWidget* add = ui_widget_manager_resolve(&scene->widgets, scene->add_attachment);
    if (!send || !edit || !emoji || !gif || !sticker || !add) {
        return failure(scene, MessageComposerSceneError::widget);
    }
    const bool can_send = channel_allows_send && !sending_restricted;
    attachments.can_attach &= !sending_restricted;
    const UiComposerVoiceMode voice = scene->voice_mode;
    const bool voice_bar = ui_composer_voice_bar_visible(voice);
    const bool voice_controls = voice_bar && voice != UiComposerVoiceMode::sending;
    const bool send_enabled = voice == UiComposerVoiceMode::available ? can_send
                              : voice_bar ? voice_controls
                                          : composer_can_send(*model, can_send, attachments, scene->premium_type) &&
                                                (!reply_message_id || !reply_mention_visible || reply_sendable);
    const auto enabled_if = [](bool enabled) noexcept {
        return static_cast<u16>(ui_widget_hit_test | ui_widget_focusable | (enabled ? ui_widget_enabled : 0));
    };
    // While recording, Add deletes the recording and Emoji stops or plays it.
    add->flags = projection && projection->command.name_bytes ? ui_widget_none :
        enabled_if(voice_bar ? voice_controls : attachments.can_attach);
    send->flags = enabled_if(send_enabled);
    emoji->flags = enabled_if(voice_bar ? voice_controls : channel_allows_send);
    gif->flags = sticker->flags =
        compact_controls(ui_rect_width(viewport), scene->theme_styles) || voice_bar ? ui_widget_none
                                                                                    : enabled_if(can_send);
    edit->flags = voice_bar ? static_cast<u16>(ui_widget_hit_test) : enabled_if(channel_allows_send);
    const u32 plugin_count = plugin_slot_count();
    for (u32 i = 0; i < kMessageComposerPluginSlots; ++i)
        if (UiWidget* plugin = ui_widget_manager_resolve(&scene->widgets, scene->plugin_slots[i]))
            plugin->flags = i < plugin_count && !voice_bar
                                ? static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable)
                                : ui_widget_none;
    const bool plugins_changed = scene->plugin_count != plugin_count;
    scene->plugin_count = static_cast<u8>(plugin_count);
    ui_widget_manager_reconcile_input(&scene->widgets);
    UiWidget* reply = ui_widget_manager_resolve(&scene->widgets, scene->reply);
    UiLayoutNode* reply_layout =
        reply ? ui_layout_engine_resolve(&scene->widgets.layout, reply->layout) : nullptr;
    UiWidget* mention = ui_widget_manager_resolve(&scene->widgets, scene->reply_mention);
    UiWidget* cancel = ui_widget_manager_resolve(&scene->widgets, scene->reply_cancel);
    UiLayoutNode* mention_layout =
        mention ? ui_layout_engine_resolve(&scene->widgets.layout, mention->layout) : nullptr;
    UiWidget* typing = ui_widget_manager_resolve(&scene->widgets, scene->typing_indicator);
    UiLayoutNode* typing_layout =
        typing ? ui_layout_engine_resolve(&scene->widgets.layout, typing->layout) : nullptr;
    UiWidget* attachment_widget = ui_widget_manager_resolve(&scene->widgets, scene->attachments);
    UiLayoutNode* attachment_layout =
        attachment_widget ? ui_layout_engine_resolve(&scene->widgets.layout, attachment_widget->layout)
                          : nullptr;
    UiWidget* command_bar = ui_widget_manager_resolve(&scene->widgets, scene->command_bar);
    UiLayoutNode* command_bar_layout =
        command_bar ? ui_layout_engine_resolve(&scene->widgets.layout, command_bar->layout) : nullptr;
    UiWidget* command_close = ui_widget_manager_resolve(&scene->widgets, scene->command_close);
    if (!reply || !reply_layout || !mention || !mention_layout || !cancel || !typing || !typing_layout ||
        !attachment_widget || !attachment_layout || !command_bar || !command_bar_layout || !command_close) {
        return failure(scene, MessageComposerSceneError::widget);
    }
    // The option bar keeps a copy of its name and detail.
    const Utf8View bar_name = projection ? projection->command_bar_name : Utf8View{};
    const Utf8View bar_detail = projection ? projection->command_bar_detail : Utf8View{};
    const bool bar_error = projection && projection->command_bar_error;
    const bool bar_resized = (scene->command_bar_name_bytes != 0) != (bar_name.bytes != 0);
    const bool bar_changed = !utf8_equal(message_composer_scene_command_bar_name(*scene), bar_name) ||
                             !utf8_equal(message_composer_scene_command_bar_detail(*scene), bar_detail) ||
                             scene->command_bar_error != bar_error;
    if (bar_changed) {
        const u32 name_bytes = utf8_copy_truncated(scene->command_bar_text, kMessageComposerCommandBarBytes / 4,
                                                   bar_name);
        scene->command_bar_name_bytes = static_cast<u16>(name_bytes);
        scene->command_bar_detail_bytes = static_cast<u16>(utf8_copy_truncated(
            scene->command_bar_text + name_bytes, kMessageComposerCommandBarBytes - name_bytes, bar_detail));
        scene->command_bar_error = bar_error;
    }
    banner_row(command_bar_layout, scene->command_bar_name_bytes != 0, kMessageComposerCommandBarHeight);
    command_bar->flags = ui_widget_none;
    command_close->flags = scene->command_bar_name_bytes != 0 ? kComposerInteractiveFlags : ui_widget_none;
    const bool reply_changed =
        scene->reply_message_id != reply_message_id || !reply_label_equal(*scene, reply_label);
    reply_mention_visible &= reply_message_id != 0;
    const bool mention_visibility_changed = scene->reply_mention_visible != reply_mention_visible;
    mention_layout->spec.style.flags = static_cast<u16>(
        ui_layout_node_clip_children | (reply_mention_visible ? ui_layout_node_visible : 0));
    mention->flags = reply_mention_visible
                         ? static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable)
                         : ui_widget_none;
    const bool typing_changed = !typing_label_equal(*scene, typing_label);
    const u8 attachment_row = message_composer_attachment_row(attachments);
    const u8 previous_attachment_row = message_composer_scene_attachment_row(*scene);
    const bool tiles_changed = store_attachment_tiles(scene, attachments.tiles);
    const bool attachments_changed =
        !utf8_equal(message_composer_scene_attachment_label(*scene), attachments.label) ||
        scene->attachments_ready != attachments.ready || scene->can_attach != attachments.can_attach ||
        scene->retained_content != attachments.retained_content || tiles_changed;
    banner_row(attachment_layout, attachment_row != 0,
               attachment_row == message_composer_attachment_row_strip ? ui_composer_attachment_strip_height()
                                                                       : kComposerReplyHeight);
    attachment_layout->spec.style.padding = attachment_row == message_composer_attachment_row_strip
                                                ? UiInsets{}
                                                : reply_style(scene->theme_styles).padding;
    attachment_widget->flags = attachment_row ? kComposerInteractiveFlags : ui_widget_none;
    banner_row(reply_layout, reply_message_id != 0, kComposerReplyHeight);
    reply->flags = ui_widget_none;
    cancel->flags = reply_message_id != 0 ? kComposerInteractiveFlags : ui_widget_none;
    banner_row(typing_layout, typing_label.bytes != 0, kComposerTypingHeight);
    ui_widget_manager_reconcile_input(&scene->widgets);
    const bool attachments_resized = previous_attachment_row != attachment_row;
    const bool relayout = scene->geometry_dirty || !scene->active || reply_changed || plugins_changed ||
                          mention_visibility_changed || typing_changed || attachments_resized || bar_resized ||
                          !ui_rect_equal(scene->viewport, viewport);
    bool changed = relayout || scene->model != model || scene->observed_revision != model->revision ||
                   scene->command.name_bytes != (projection ? projection->command.name_bytes : 0) ||
                   scene->command.bot_id != (projection ? projection->command.bot_id : 0) ||
                   scene->command.avatar_variant != (projection ? projection->command.avatar_variant : 0) ||
                   scene->command.animated != (projection && projection->command.animated) ||
                   scene->channel_allows_send != can_send || reply_changed || typing_changed || bar_changed ||
                   attachments_changed || scene->mention_reply_author != mention_reply_author ||
                   scene->status_is_error != status_is_error || scene->reply_sendable != reply_sendable ||
                   scene->status_tone != status_tone;
    changed = update_placeholder(scene, *model, projection) || changed;
    if (relayout && (!configure_controls(scene, viewport,
                                         message_composer_scene_chrome_height(
                                             reply_message_id != 0, typing_label.bytes != 0,
                                             attachment_row, scene->theme_styles,
                                             scene->command_bar_name_bytes != 0)) ||
                     !ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok())) {
        return failure(scene, MessageComposerSceneError::layout);
    }
    if (relayout) {
        ++scene->layout_passes;
        scene->geometry_dirty = false;
    }
    const u64 previous_display_signature = scene->display_signature;
    const u64 input_signature = projection_input_signature(*scene, *model, projection);
    if (!scene->active || scene->model != model || scene->projection_input_signature != input_signature) {
        project_display(scene, *model, projection);
        scene->projection_input_signature = projection_input_signature(*scene, *model, projection);
        ++scene->projection_passes;
    }
    scene->model = model;
    scene->command = projection ? projection->command : MessageComposerCommand{};
    model->atomic_spans = {scene->atomic_spans, scene->projection_span_count};
    model->atomic_revision = model->text_revision;
    changed = changed || scene->display_signature != previous_display_signature;
    scene->viewport = viewport;
    scene->observed_revision = model->revision;
    scene->channel_allows_send = can_send;
    scene->reply_message_id = reply_message_id;
    scene->mention_reply_author = mention_reply_author;
    scene->reply_mention_visible = reply_mention_visible;
    scene->status_is_error = status_is_error;
    scene->reply_sendable = reply_sendable;
    scene->status_tone = status_tone;
    scene->reply_label_bytes = static_cast<u16>(reply_label.bytes);
    copy_bytes(scene->reply_label, reply_label.data, reply_label.bytes);
    scene->typing_label_bytes = static_cast<u16>(typing_label.bytes);
    copy_bytes(scene->typing_label, typing_label.data, typing_label.bytes);
    scene->attachment_label_bytes = static_cast<u16>(attachments.label.bytes);
    copy_bytes(scene->attachment_strip->label, attachments.label.data, attachments.label.bytes);
    clamp_attachment_scroll(scene);
    scene->attachments_ready = attachments.ready;
    scene->can_attach = attachments.can_attach;
    scene->retained_content = attachments.retained_content;
    scene->active = true;
    return MessageComposerSceneResult{MessageComposerSceneError::none, changed};
}

UiRect message_composer_scene_command_avatar(const MessageComposerScene& scene) noexcept {
    const auto* attachment = message_composer_scene_layout_node(scene, MessageComposerControl::add_attachment);
    if (!scene.active || !scene.command.bot_id || !attachment)
        return {};
    const float extent = composer_metrics(scene.theme_styles).menu_icon;
    const float x = (attachment->rectangle.left + attachment->rectangle.right) * .5f;
    const float y = (attachment->rectangle.top + attachment->rectangle.bottom) * .5f;
    return {x - extent * .5f, y - extent * .5f, x + extent * .5f, y + extent * .5f};
}

MessageComposerControl message_composer_scene_hit_test(const MessageComposerScene& scene, float x,
                                                       float y) noexcept {
    if (!scene.active) {
        return MessageComposerControl::none;
    }
    const UiWidgetHandle hit = ui_widget_manager_hit_test(scene.widgets, x, y);
    const UiWidget* widget = ui_widget_manager_resolve(scene.widgets, hit);
    if (!widget) {
        return MessageComposerControl::none;
    }
    switch (widget->id) {
    case kComposerAttachmentId:
        return MessageComposerControl::add_attachment;
    case kComposerEditId:
        return MessageComposerControl::edit;
    case kComposerEmojiId:
        return MessageComposerControl::emoji;
    case kComposerGifId:
        return MessageComposerControl::gif;
    case kComposerStickerId:
        return MessageComposerControl::sticker;
    case kComposerSendId:
        return MessageComposerControl::send;
    case kComposerReplyCancelId:
        return MessageComposerControl::cancel_reply;
    case kComposerCommandCloseId:
        return MessageComposerControl::command_close;
    case kComposerReplyMentionId:
        return MessageComposerControl::reply_mention;
    case kComposerAttachmentsId:
        return MessageComposerControl::attachments;
    default:
        return widget->id >= kComposerPluginId && widget->id < kComposerPluginId + scene.plugin_count
                   ? MessageComposerControl::plugin_slot
                   : MessageComposerControl::none;
    }
}

u32 message_composer_scene_plugin_slot(const MessageComposerScene& scene, float x, float y) noexcept {
    if (!scene.active)
        return kMessageComposerPluginSlots;
    const UiWidget* widget = ui_widget_manager_resolve(scene.widgets, ui_widget_manager_hit_test(scene.widgets, x, y));
    return widget && widget->id >= kComposerPluginId && widget->id < kComposerPluginId + scene.plugin_count
               ? widget->id - kComposerPluginId
               : kMessageComposerPluginSlots;
}

const UiLayoutNode* message_composer_scene_layout_node(const MessageComposerScene& scene,
                                                       MessageComposerControl control) noexcept {
    return ui_widget_manager_layout_node(scene.widgets, control_handle(scene, control));
}

Utf8View message_composer_scene_attachment_label(const MessageComposerScene& scene) noexcept {
    return scene.attachment_strip ? Utf8View{scene.attachment_strip->label, scene.attachment_label_bytes}
                                  : Utf8View{};
}
u8 message_composer_scene_attachment_row(const MessageComposerScene& scene) noexcept {
    return scene.attachment_strip && scene.attachment_strip->count ? message_composer_attachment_row_strip
           : scene.attachment_label_bytes                           ? message_composer_attachment_row_label
                                                                    : message_composer_attachment_row_none;
}
ConstSlice<UiComposerAttachmentTile> message_composer_scene_attachment_tiles(const MessageComposerScene& scene) noexcept {
    return scene.attachment_strip ? ConstSlice<UiComposerAttachmentTile>{scene.attachment_strip->tiles,
                                                                          scene.attachment_strip->count}
                                  : ConstSlice<UiComposerAttachmentTile>{};
}
UiComposerAttachmentStripView message_composer_scene_attachment_view(const MessageComposerScene& scene,
                                                                     bool focus_visible) noexcept {
    UiComposerAttachmentStripView view{};
    view.hovered = view.selected = kUiComposerAttachmentNoTile;
    if (!scene.active || !scene.attachment_strip || !scene.attachment_strip->count)
        return view;
    const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.attachments);
    if (!node || !node->visible)
        return view;
    const auto& strip = *scene.attachment_strip;
    view.tiles = {strip.tiles, strip.count};
    view.strip = ui_rect_intersection(node->content_rectangle, node->clip_rectangle);
    view.scroll = strip.scroll;
    view.hovered = strip.hovered;
    view.hovered_part = strip.hovered_part;
    view.selected = message_composer_scene_attachment_selected(scene);
    view.focus_visible = focus_visible && scene.widgets.focused.generation &&
                         scene.widgets.focused.slot == scene.attachments.slot &&
                         scene.widgets.focused.generation == scene.attachments.generation;
    view.owner = {&scene.widgets, scene.attachments};
    return view;
}
UiComposerAttachmentHit message_composer_scene_attachment_hit(const MessageComposerScene& scene, float x,
                                                              float y) noexcept {
    const auto view = message_composer_scene_attachment_view(scene, false);
    return ui_composer_attachment_strip_hit(view.strip, view.tiles, view.scroll, x, y);
}
bool message_composer_scene_attachment_hover(MessageComposerScene* scene, UiComposerAttachmentHit hit) noexcept {
    if (!scene || !scene->attachment_strip)
        return false;
    auto& strip = *scene->attachment_strip;
    if (hit.index >= strip.count)
        hit = {kUiComposerAttachmentNoTile, UiComposerAttachmentPart::none};
    if (strip.hovered == hit.index && strip.hovered_part == hit.part)
        return false;
    strip.hovered = hit.index;
    strip.hovered_part = hit.part;
    return true;
}
bool message_composer_scene_attachment_scroll(MessageComposerScene* scene, float delta) noexcept {
    if (!scene || !scene->attachment_strip || !scene->attachment_strip->count || delta != delta)
        return false;
    const auto view = message_composer_scene_attachment_view(*scene, false);
    const float next = ui_composer_attachment_strip_clamp_scroll(view.strip, scene->attachment_strip->count,
                                                                 scene->attachment_strip->scroll + delta);
    if (next == scene->attachment_strip->scroll)
        return false;
    scene->attachment_strip->scroll = next;
    return true;
}
bool message_composer_scene_attachment_select(MessageComposerScene* scene, u32 index) noexcept {
    if (!scene || !scene->attachment_strip)
        return false;
    auto& strip = *scene->attachment_strip;
    const u64 id = index < strip.count ? strip.tiles[index].id : 0;
    bool changed = strip.selected_id != id;
    strip.selected_id = id;
    if (index < strip.count) {
        const auto view = message_composer_scene_attachment_view(*scene, false);
        const float next = ui_composer_attachment_strip_reveal(view.strip, strip.count, index, strip.scroll);
        changed = changed || next != strip.scroll;
        strip.scroll = next;
    }
    return changed;
}
u32 message_composer_scene_attachment_selected(const MessageComposerScene& scene) noexcept {
    if (!scene.attachment_strip || !scene.attachment_strip->selected_id)
        return kUiComposerAttachmentNoTile;
    for (u32 i = 0; i < scene.attachment_strip->count; ++i)
        if (scene.attachment_strip->tiles[i].id == scene.attachment_strip->selected_id)
            return i;
    return kUiComposerAttachmentNoTile;
}
bool message_composer_scene_can_send(const MessageComposerScene& scene) noexcept {
    return scene.active && scene.model &&
           (!scene.reply_message_id || !scene.reply_mention_visible || scene.reply_sendable) &&
           composer_can_send(*scene.model, scene.channel_allows_send,
                             {message_composer_scene_attachment_label(scene), scene.attachments_ready,
                              scene.can_attach, scene.retained_content},
                             scene.premium_type);
}

bool message_composer_scene_set_voice(MessageComposerScene* scene, const UiComposerVoiceView* view) noexcept {
    if (!scene)
        return false;
    const UiComposerVoiceMode mode = view ? view->mode : UiComposerVoiceMode::none;
    const bool changed = scene->voice_mode != mode || scene->voice != view;
    if (ui_composer_voice_bar_visible(scene->voice_mode) != ui_composer_voice_bar_visible(mode))
        scene->geometry_dirty = true;
    scene->voice_mode = mode;
    scene->voice = view;
    return changed;
}
UiComposerVoiceMode message_composer_scene_voice_mode(const MessageComposerScene& scene) noexcept {
    return scene.voice_mode;
}

bool message_composer_scene_focus_edit(MessageComposerScene* scene) noexcept {
    return message_composer_scene_focus_control(scene, MessageComposerControl::edit);
}

bool message_composer_scene_focus_control(MessageComposerScene* scene,
                                          MessageComposerControl control) noexcept {
    return scene && scene->active && control != MessageComposerControl::none &&
           ui_widget_manager_focus(&scene->widgets, control_handle(*scene, control));
}

MessageComposerControl message_composer_scene_focused_control(const MessageComposerScene& scene) noexcept {
    if (!scene.active || !ui_widget_manager_input_enabled(scene.widgets, scene.widgets.focused))
        return MessageComposerControl::none;
    const auto* widget = ui_widget_manager_resolve(scene.widgets, scene.widgets.focused);
    if (!widget || !(widget->flags & ui_widget_focusable))
        return MessageComposerControl::none;
    const MessageComposerControl controls[] = {
        MessageComposerControl::command_close, MessageComposerControl::cancel_reply,
        MessageComposerControl::reply_mention,
        MessageComposerControl::edit,         MessageComposerControl::gif,
        MessageComposerControl::sticker,      MessageComposerControl::emoji,
        MessageComposerControl::send,         MessageComposerControl::add_attachment,
        MessageComposerControl::attachments};
    for (auto control : controls) {
        const auto handle = control_handle(scene, control);
        if (handle.slot == scene.widgets.focused.slot &&
            handle.generation == scene.widgets.focused.generation)
            return control;
    }
    return message_composer_scene_focused_plugin_slot(scene) < kMessageComposerPluginSlots
               ? MessageComposerControl::plugin_slot
               : MessageComposerControl::none;
}

u32 message_composer_scene_focused_plugin_slot(const MessageComposerScene& scene) noexcept {
    for (u32 i = 0; scene.active && i < scene.plugin_count; ++i)
        if (scene.plugin_slots[i].slot == scene.widgets.focused.slot &&
            scene.plugin_slots[i].generation == scene.widgets.focused.generation)
            return i;
    return kMessageComposerPluginSlots;
}

bool message_composer_scene_set_caret_visible(MessageComposerScene* scene, bool visible) noexcept {
    if (!scene) {
        return false;
    }
    scene->caret_visible = visible;
    return true;
}

void message_composer_scene_release(MessageComposerScene* scene) noexcept {
    if (!scene) {
        return;
    }
    if (scene->model && scene->model->atomic_spans.data == scene->atomic_spans)
        reset_value(&scene->model->atomic_spans);
    if (scene->owns_storage) {
        arena_release(&scene->owned_storage);
    }
    reset_value<MessageComposerScene>(scene);
}

Utf8View message_composer_scene_display_text(const MessageComposerScene& scene) noexcept {
    return Utf8View{scene.display, scene.display_bytes};
}

Utf8View message_composer_scene_reply_label(const MessageComposerScene& scene) noexcept {
    return scene.reply_message_id != 0 ? Utf8View{scene.reply_label, scene.reply_label_bytes} : Utf8View{};
}

Utf8View message_composer_scene_reply_mention_label(const MessageComposerScene& scene) noexcept {
    if (!scene.active || !scene.reply_message_id || !scene.reply_mention_visible)
        return {};
    return scene.mention_reply_author ? utf8_literal("ON") : utf8_literal("OFF");
}
UiRect message_composer_scene_reply_mention_icon(const MessageComposerScene& scene) noexcept {
    const auto* mention = message_composer_scene_reply_mention_label(scene).bytes
                              ? message_composer_scene_layout_node(scene, MessageComposerControl::reply_mention)
                              : nullptr;
    if (!mention)
        return {};
    const auto r = mention->rectangle;
    const float y = (r.top + r.bottom) * .5f;
    return {r.left + kMessageComposerReplyMentionPadding, y - 8, r.left + kMessageComposerReplyMentionPadding + 16,
            y + 8};
}
UiRect message_composer_scene_reply_mention_text(const MessageComposerScene& scene) noexcept {
    const auto icon = message_composer_scene_reply_mention_icon(scene);
    const auto* mention = ui_rect_has_area(icon)
                              ? message_composer_scene_layout_node(scene, MessageComposerControl::reply_mention)
                              : nullptr;
    if (!mention)
        return {};
    return {icon.right + 4, mention->rectangle.top, mention->rectangle.right - kMessageComposerReplyMentionPadding * .5f,
            mention->rectangle.bottom};
}

UiRect message_composer_scene_reply_text_rectangle(const MessageComposerScene& scene) noexcept {
    const auto* reply = ui_widget_manager_layout_node(scene.widgets, scene.reply);
    const auto* cancel = message_composer_scene_layout_node(scene, MessageComposerControl::cancel_reply);
    if (!reply || !cancel)
        return {};
    auto rectangle = reply->content_rectangle;
    rectangle.right =
        maximum(rectangle.left, cancel->rectangle.left - composer_metrics(scene.theme_styles).menu_gap);
    if (scene.reply_mention_visible) {
        const auto* mention =
            message_composer_scene_layout_node(scene, MessageComposerControl::reply_mention);
        if (mention)
            rectangle.right = maximum(rectangle.left, mention->rectangle.left -
                                                          composer_metrics(scene.theme_styles).menu_gap);
    }
    return rectangle;
}

Utf8View message_composer_scene_command_bar_name(const MessageComposerScene& scene) noexcept {
    return scene.command_bar_text ? Utf8View{scene.command_bar_text, scene.command_bar_name_bytes} : Utf8View{};
}
Utf8View message_composer_scene_command_bar_detail(const MessageComposerScene& scene) noexcept {
    return scene.command_bar_text
               ? Utf8View{scene.command_bar_text + scene.command_bar_name_bytes, scene.command_bar_detail_bytes}
               : Utf8View{};
}
UiRect message_composer_scene_command_bar_text_rectangle(const MessageComposerScene& scene) noexcept {
    const auto* bar = ui_widget_manager_layout_node(scene.widgets, scene.command_bar);
    const auto* close = message_composer_scene_layout_node(scene, MessageComposerControl::command_close);
    if (!bar || !close || !scene.command_bar_name_bytes)
        return {};
    auto rectangle = bar->content_rectangle;
    rectangle.right = maximum(rectangle.left, close->rectangle.left - composer_metrics(scene.theme_styles).menu_gap);
    return rectangle;
}

Utf8View message_composer_scene_typing_label(const MessageComposerScene& scene) noexcept {
    return Utf8View{scene.typing_label, scene.typing_label_bytes};
}

Utf8View message_composer_scene_placeholder(const MessageComposerScene& scene) noexcept {
    return Utf8View{scene.placeholder, scene.placeholder_bytes};
}

u32 message_composer_scene_display_offset_from_model(const MessageComposerScene& scene,
                                                     u32 model_byte_offset) noexcept {
    if (!scene.model || model_byte_offset > scene.model->bytes) {
        return scene.display_bytes;
    }
    return display_offset_from_source(scene, model_byte_offset);
}

u32 message_composer_scene_model_offset_from_display(const MessageComposerScene& scene,
                                                     u32 display_byte_offset) noexcept {
    if (!scene.model) {
        return 0;
    }
    const u32 display = minimum(display_byte_offset, scene.display_bytes);
    i64 adjustment = static_cast<i64>(scene.display_first);
    for (u32 index = 0; index < scene.projection_span_count; ++index) {
        const MessageComposerProjectionSpan& span = scene.projection_spans[index];
        if (display <= span.display_first) {
            break;
        }
        if (display < span.display_last) {
            if (span.kind == MessageComposerProjectionKind::regional_flag)
                return span.source_first + display - span.display_first;
            const u32 display_middle = span.display_first + (span.display_last - span.display_first) / 2;
            return display <= display_middle ? span.source_first : span.source_last;
        }
        adjustment += static_cast<i64>(span.source_last - span.source_first) -
                      static_cast<i64>(span.display_last - span.display_first);
    }
    const i64 source = static_cast<i64>(display) + adjustment;
    return source <= 0 ? 0 : minimum(scene.model->bytes, static_cast<u32>(source));
}

bool message_composer_scene_composition_range(const MessageComposerScene& scene,
                                              const TextCompositionSpan& span, u32* first,
                                              u32* last) noexcept {
    if (!scene.model || !first || !last || span.start >= span.end)
        return false;
    u32 begin = 0, end = 0;
    const auto source = message_composer_text(*scene.model);
    if (!utf8_byte_offset_for_utf16_position(source, span.start, &begin) ||
        !utf8_byte_offset_for_utf16_position(source, span.end, &end) || end <= scene.display_first ||
        begin >= scene.display_source_last)
        return false;
    begin = maximum(begin, scene.display_first);
    end = minimum(end, scene.display_source_last);
    const auto display = message_composer_scene_display_text(scene);
    return utf8_utf16_position_for_byte_offset(
               display, message_composer_scene_display_offset_from_model(scene, begin), first) &&
           utf8_utf16_position_for_byte_offset(
               display, message_composer_scene_display_offset_from_model(scene, end), last) &&
           *first < *last;
}

u32 message_composer_scene_chip_ranges(const MessageComposerScene& scene,
                                       Slice<MessageComposerChipRange> output) noexcept {
    if (!scene.model || !output.data)
        return 0;
    const auto display = message_composer_scene_display_text(scene);
    // The value of the window's last chip ends past the text only when the
    // window holds the end of the draft.
    const bool window_end = scene.display_source_last >= scene.model->bytes;
    u32 count = 0;
    for (u32 index = 0; index < scene.projection_span_count && count < output.count; ++index) {
        const auto& span = scene.projection_spans[index];
        if (span.kind != MessageComposerProjectionKind::command_option)
            continue;
        const u8 flags = static_cast<u8>(span.target);
        u32 first = 0, last = 0;
        if (!utf8_utf16_position_for_byte_offset(display, span.display_first, &first) ||
            !utf8_utf16_position_for_byte_offset(display, span.display_last, &last))
            return count;
        // A terminated value ends at its marker's end padding; one not
        // terminated yet at the next chip's head, else at the end of the text.
        u32 value_last = utf8_utf16_units(display);
        bool open_end = window_end;
        for (u32 next = index + 1; next < scene.projection_span_count; ++next) {
            const auto& following = scene.projection_spans[next];
            if (following.kind != MessageComposerProjectionKind::command_option &&
                following.kind != MessageComposerProjectionKind::command_option_end)
                continue;
            if (!utf8_utf16_position_for_byte_offset(display, following.display_first, &value_last))
                return count;
            if (following.kind == MessageComposerProjectionKind::command_option_end)
                value_last += kMessageComposerChipPadding;
            open_end = false;
            break;
        }
        output[count++] = {first + kMessageComposerChipMargin, last - kMessageComposerChipPadding, value_last, flags,
                           open_end};
    }
    return count;
}

} // namespace maicord
