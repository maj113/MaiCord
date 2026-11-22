#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/ui/theme/styles.hpp"
#include "maicord/ui/chat/composer/voice_bar.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/ui/text/regional_flag.hpp"
#include "maicord/ui/plugins/slots.hpp"
#include "maicord/ui/layout/surface_geometry.hpp"
#include "maicord/ui/core/widget_builder.hpp"

#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/chat/messages/markdown.hpp"
#include "maicord/ui/chat/messages/typing_presentation.hpp"
#include "maicord/ui/chat/messages/typing_indicator.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/model/chat/game_store.hpp"
#include "maicord/media/core/asset.hpp"

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

inline bool initialize_owned_storage(MessageComposerScene* scene) noexcept {
    if (!arena_initialize(&scene->owned_storage, kMessageComposerSceneStorageBytes, 0, 4 * 1024, ArenaTag::ui)) {
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

inline float accessory_width(const UiResolvedThemeStyles* styles) noexcept {
    return maximum(32.f, ui_theme_metrics(styles).menu_row_height);
}
inline float accessory_height(const UiResolvedThemeStyles* styles) noexcept {
    return maximum(32.f, ui_theme_metrics(styles).control_height);
}
// Plugin composer-toolbar buttons currently published; global, so the static
// width functions and the scene agree without a scene argument.
u32 plugin_slot_count() noexcept {
    UiPluginSlotNode nodes[kMessageComposerPluginSlots];
    return ui_plugin_slot_toolbar(kUiPluginSlotComposerToolbar, {nodes, kMessageComposerPluginSlots});
}
inline constexpr float kComposerReplyHeight = 28.0f;
inline constexpr float kComposerTypingHeight = 20.0f;

bool reply_label_equal(const MessageComposerScene& scene, const Utf8View& label) noexcept {
    return utf8_equal(Utf8View{scene.reply_label, scene.reply_label_bytes}, label);
}

inline bool composer_can_send(const MessageComposer& model, bool channel_allows,
                              MessageComposerAttachmentInput attachments, u8 premium_type) noexcept {
    const bool present = attachments.label.bytes != 0;
    return (!present || (attachments.ready && attachments.can_attach)) &&
           message_composer_can_send(model, channel_allows, message_composer_maximum_codepoints(premium_type),
                                     present || attachments.retained_content);
}

inline bool typing_label_equal(const MessageComposerScene& scene, Utf8View label) noexcept {
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
    style.gap = ui_theme_metrics(styles).composer_padding;
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children);
    return style;
}

UiLayoutStyle surface_style(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = root_style(styles);
    style.padding =
        UiInsets{ui_theme_metrics(styles).composer_padding, ui_theme_metrics(styles).composer_padding,
                 ui_theme_metrics(styles).composer_padding, ui_theme_metrics(styles).composer_padding};
    return style;
}

UiLayoutStyle controls_style(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.flow = UiLayoutFlow::horizontal;
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.gap = ui_theme_metrics(styles).composer_padding;
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    return style;
}

UiLayoutStyle reply_style(const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(0.0f);
    style.padding =
        UiInsets{ui_theme_metrics(styles).menu_gap, ui_theme_metrics(styles).menu_padding * .5f,
                 ui_theme_metrics(styles).menu_gap, ui_theme_metrics(styles).menu_padding * .5f};
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
    style.padding = UiInsets{ui_theme_metrics(styles).menu_gap, ui_theme_metrics(styles).menu_padding,
                             ui_theme_metrics(styles).menu_gap, ui_theme_metrics(styles).menu_padding};
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                                   ui_layout_node_text_content);
    return style;
}

// Inline text gets at least the width of the complete trailing action group.
// Below that width the four trailing actions wrap after the editor instead.
float full_controls_minimum_width(const UiResolvedThemeStyles* styles, u32 plugin_count) noexcept {
    const auto& metrics = ui_theme_metrics(styles);
    const float accessory = accessory_width(styles), gap = metrics.composer_padding;
    const float plugins = static_cast<float>(plugin_count);
    const float trailing_width = (4.f + plugins) * accessory + (3.f + plugins) * gap;
    return gap + gap + (5.f + plugins) * (accessory + gap) + trailing_width + metrics.menu_gap + metrics.menu_gap;
}

// Between the compact and the full width the GIF and sticker buttons fold
// into the emoji picker (its tabs reach both), so attach, editor, plugins,
// emoji and send keep one row with at least this much editable width.
inline constexpr float kComposerCompactEditorMinimum = 120.0f;
float compact_controls_minimum_width(const UiResolvedThemeStyles* styles, u32 plugin_count) noexcept {
    const auto& metrics = ui_theme_metrics(styles);
    const float plugins = static_cast<float>(plugin_count);
    return metrics.composer_padding + metrics.composer_padding +
           (3.f + plugins) * (accessory_width(styles) + metrics.composer_padding) + kComposerCompactEditorMinimum +
           metrics.menu_gap + metrics.menu_gap;
}
inline bool compact_controls(float composer_width, const UiResolvedThemeStyles* styles, u32 plugin_count) noexcept {
    return composer_width >= compact_controls_minimum_width(styles, plugin_count) &&
           composer_width < full_controls_minimum_width(styles, plugin_count);
}
// The editable width beside the controls. Above the full width the GIF and sticker buttons show, between the compact
// and the full width they fold away, and below the compact width the actions wrap to a toolbar.
float controls_text_width(float composer_width, const UiResolvedThemeStyles* styles, u32 plugin_count) noexcept {
    const auto& metrics = ui_theme_metrics(styles);
    const float plugins = static_cast<float>(plugin_count);
    const float actions = composer_width >= full_controls_minimum_width(styles, plugin_count)      ? 5.f + plugins
                          : composer_width >= compact_controls_minimum_width(styles, plugin_count) ? 3.f + plugins
                                                                                                    : 1.f;
    return maximum(0.f, composer_width - metrics.composer_padding - metrics.composer_padding -
                            actions * (accessory_width(styles) + metrics.composer_padding) - metrics.menu_gap -
                            metrics.menu_gap);
}
// The height of the wrapped toolbar below the compact width (0 at or above it). composer_width is positive.
float controls_toolbar_height(float composer_width, const UiResolvedThemeStyles* styles, u32 plugin_count) noexcept {
    if (composer_width >= compact_controls_minimum_width(styles, plugin_count))
        return 0.f;
    const float gap = ui_theme_metrics(styles).composer_padding;
    const float available = maximum(0.f, composer_width - gap - gap);
    const u32 trailing = 4u + plugin_count;
    const u32 columns =
        minimum(trailing, maximum(1u, static_cast<u32>((available + gap) / (accessory_width(styles) + gap))));
    const u32 rows = (trailing + columns - 1u) / columns;
    return static_cast<float>(rows) * (accessory_height(styles) + gap);
}

inline float editor_padding_height(const UiResolvedThemeStyles* styles) noexcept {
    const auto& metrics = ui_theme_metrics(styles);
    return metrics.composer_padding + metrics.composer_padding + metrics.menu_padding + metrics.menu_padding;
}

void configure_controls(MessageComposerScene* scene, const UiRect& viewport, float chrome) noexcept {
    const auto layout = [&](UiWidgetHandle handle) noexcept {
        return ui_widget_manager_layout_node(&scene->widgets, handle);
    };
    auto* controls = layout(scene->controls);
    auto* edit = layout(scene->edit);
    const auto* styles = scene->theme_styles;
    auto* root = layout(scene->root);
    auto* surface_node = layout(scene->surface);
    auto* reply = layout(scene->reply);
    auto* mention = layout(scene->reply_mention);
    auto* attachments = layout(scene->attachments);
    auto* typing = layout(scene->typing_indicator);
    root->spec.style = root_style(styles);
    surface_node->spec.style = surface_style(styles);
    reply->spec.style.padding = UiInsets{ui_theme_metrics(styles).menu_gap, 0.f, 0.f, 0.f};
    // The right margin clears Cancel and a 1 DIP divider.
    mention->spec.style.margin.right = kComposerReplyHeight + 2 * ui_theme_metrics(styles).composer_padding + 1;
    // The upload strip spans the surface; the label row keeps its inset.
    attachments->spec.style.padding =
        scene->attachment_strip && scene->attachment_strip->count ? UiInsets{} : reply_style(styles).padding;
    typing->spec.style.padding.right = ui_theme_metrics(styles).composer_padding;
    const float width = ui_rect_width(viewport);
    // The recording bar folds the media pickers and plugins away.
    const bool voice_bar = ui_composer_voice_bar_visible(scene->voice_mode);
    const bool compact = compact_controls(width, styles, scene->plugin_count) || voice_bar;
    const UiWidgetHandle accessories[]{scene->add_attachment, scene->gif, scene->sticker, scene->emoji,
                                       scene->send};
    const UiLayoutStyle accessory = fixed_control_style(accessory_width(styles), styles);
    for (u32 i = 0; i < 5; ++i) {
        auto* node = layout(accessories[i]);
        node->spec.style = accessory;
        if (compact && (i == 1 || i == 2)) // GIF and sticker
            node->spec.style.flags = static_cast<u16>(node->spec.style.flags & ~ui_layout_node_visible);
    }
    for (u32 i = 0; i < kMessageComposerPluginSlots; ++i) {
        auto* plugin = layout(scene->plugin_slots[i]);
        plugin->spec.style = accessory;
        if (i >= scene->plugin_count || voice_bar)
            plugin->spec.style.flags = static_cast<u16>(plugin->spec.style.flags & ~ui_layout_node_visible);
    }
    const float toolbar = controls_toolbar_height(width, styles, scene->plugin_count);
    controls->spec.style = controls_style(styles);
    edit->spec.style = edit_style(styles);
    scene->widgets.layout.wrap_nodes = toolbar != 0.f ? 1u : 0u;
    if (toolbar != 0.f) {
        const auto surface = surface_style(styles);
        controls->spec.style.flow = UiLayoutFlow::horizontal_wrap;
        // A fixed editor consumes the remainder of the first row; the wrap
        // engine then fits the trailing actions into the available width.
        edit->spec.style.width =
            ui_layout_fixed(controls_text_width(width, styles, scene->plugin_count) +
                            edit->spec.style.padding.left + edit->spec.style.padding.right);
        edit->spec.style.height = ui_layout_fixed(
            maximum(accessory.height.value, ui_rect_height(viewport) - chrome - toolbar -
                                                surface.padding.top - surface.padding.bottom));
        controls->measure_constraint = {maximum(0.f, width - surface.padding.left - surface.padding.right),
                                        kMaximumUiCoordinate};
    }
}

u64 projection_input_signature(const MessageComposerScene& scene, const MessageComposer& model,
                               const MessageComposerProjectionContext* context) noexcept {
    // Fields that fit one 8-byte word share one mix. The signature is only compared in memory.
    u64 signature = hash64_mix(kHash64Seed, model.document_identity);
    signature = hash64_mix(signature, model.text_revision |
                                          static_cast<u64>(context ? context->command.name_bytes : 0) << 32);
    signature = hash64_mix(signature, model.channel_id);
    // Only a game chip reads the caret (its active flag). Without one a caret move keeps the projection.
    if (scene.projection_reads_caret)
        signature = hash64_mix(signature, model.anchor | static_cast<u64>(model.caret) << 32);
    signature = hash64_mix(signature, context ? context->guild_id : 0);
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
    if (context && context->games)
        signature = hash64_mix(signature, context->games->revision);
    if (context && context->state) {
        signature = hash64_mix(signature, context->state->guild_mutations |
                                              static_cast<u64>(context->state->channel_mutations) << 32);
        signature = hash64_mix(signature, context->state->permission_mutations |
                                              static_cast<u64>(context->state->emoji_mutations) << 32);
        // Role upserts have no store-wide revision. Name allocation/counts
        // also reveal newly resolvable tokens; retained visible role records
        // provide bounded dependencies for in-place color/name changes.
        signature = hash64_mix(signature, context->state->compactions |
                                              static_cast<u64>(context->state->role_count) << 32);
        signature = hash64_mix(signature, context->state->name_used |
                                              static_cast<u64>(context->state->name_wasted) << 32);
        for (u32 index = 0; index < scene.projection_span_count; ++index) {
            const auto& span = scene.projection_spans[index];
            if (span.kind != MessageComposerProjectionKind::role)
                continue;
            const auto* role =
                chat_state_store_find_role(*context->state, context->guild_id, span.target);
            signature = hash64_mix(signature, span.target);
            signature = hash64_mix(signature, role ? role->guild_id : 0);
            signature = hash64_mix(signature, role ? role->id : 0);
            signature = hash64_mix(signature, role ? role->color | static_cast<u64>(role->name_offset) << 32 : 0);
            signature = hash64_mix(signature, role ? role->name_bytes : 0);
        }
    }
    return signature;
}

UiLayoutStyle typing_style() noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(kComposerTypingHeight);
    style.padding = UiInsets{kUiTypingLabelInset, 0.0f, ui_theme_metrics(nullptr).composer_padding, 0.0f};
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                                   ui_layout_node_text_content);
    return style;
}

// A collapsible banner row (reply, attachments): zero height unless shown.
constexpr u16 kComposerInteractiveFlags = static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable);
inline void banner_row(UiLayoutNode* node, bool shown, float height) noexcept {
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

MessageComposerSceneResult failure(MessageComposerSceneError error) noexcept {
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
    // A known game's chip: its icon cell before the name, as the chat draws it.
    bool icon_cell;
};
// The chat's game icon cell: an em, then 3 px (markdown_links.cpp).
constexpr Utf8View kGameIconCell = utf8_literal("\xE2\x80\x83\xE2\x80\x86");

// `first` is below source.bytes and holds the token's '<'.
bool projected_token_at(const Utf8View& source, u32 first, const MessageComposerProjectionContext* context,
                        ProjectedToken* token) noexcept {
    if (source.bytes - first < 4) {
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
        } else if (cursor < source.bytes && source.data[cursor] == '$' && source.data[cursor - 1] == '@') {
            kind = MessageComposerProjectionKind::game;
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
    if (!decimal_parse_padded({source.data + digits_first, cursor - digits_first}, &id) || id == 0) {
        return false;
    }
    if (kind != MessageComposerProjectionKind::emoji && kind != MessageComposerProjectionKind::game &&
        (!context || !context->state))
        return false;
    Utf8View label{};
    u32 color = 0;
    bool icon_cell = false;
    if (kind == MessageComposerProjectionKind::user) {
        const UserRecord* user =
            context->users ? user_store_find(*context->users, id) : nullptr;
        label = user ? user_store_name(*context->users, *user) : Utf8View{};
    } else if (kind == MessageComposerProjectionKind::channel) {
        const ChannelRecord* channel = chat_state_store_find_channel(*context->state, id);
        label = channel ? chat_state_store_channel_name(*context->state, *channel) : Utf8View{};
    } else if (kind == MessageComposerProjectionKind::emoji) {
        label = utf8_literal("\xEF\xBF\xBC");
    } else if (kind == MessageComposerProjectionKind::game) {
        // The official composer: a known game's icon and name in a mention
        // chip, any other "@unknown-game" as plain text.
        label = context && context->games ? game_store_name(*context->games, id) : Utf8View{};
        icon_cell = label.bytes != 0;
        if (!icon_cell)
            label = utf8_literal("unknown-game");
        else
            marker = 0;
    } else {
        const RoleRecord* role =
            chat_state_store_find_role(*context->state, context->guild_id, id);
        label = role ? chat_state_store_role_name(*context->state, *role) : Utf8View{};
        color = role ? role->color : 0;
    }
    if (label.bytes == 0 || !label.data) {
        return false;
    }
    *token = ProjectedToken{label, id, cursor + 1, color, kind, marker, animated, icon_cell};
    return true;
}

u32 display_offset_from_source(const MessageComposerScene& scene, u32 source) noexcept {
    if (source == 0) {
        return 0;
    }
    const u32 source_last = scene.display_source_last;
    if (source >= source_last) {
        return scene.display_bytes;
    }
    i64 adjustment = 0;
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
inline bool display_style_available(const MessageComposerScene& scene, u32 first, u32 flags) noexcept {
    if (!flags || scene.display_style_count < kTextStyleSpanMaximum)
        return true;
    const auto& last = scene.display_styles[scene.display_style_count - 1];
    return last.flags == flags && last.byte_offset + last.byte_count == first;
}
void project_display(MessageComposerScene* scene, const MessageComposer& model,
                     const MessageComposerProjectionContext* context) noexcept {
    update_source_styles(scene, model);
    scene->display_style_count = 0;
    // The display holds kMessageComposerMaximumBytes, the most a model can hold, and the model keeps valid UTF-8: the
    // projection reads the whole text.
    const u32 end = model.bytes;
    scene->display_bytes = 0;
    scene->projection_span_count = 0;
    scene->projection_reads_caret = false;
    const Utf8View source{model.text, model.bytes};
    u32 cursor = 0;
    u32 style_index = 0;
    u32 flag_cursor = 0;
    RegionalFlagRange flag{};
    bool have_flag = regional_flag_next(source, &flag_cursor, &flag);
    // The end mark of the game search token being drawn, and its chip flags.
    u32 game_end_at = ~0u;
    u8 game_flags = 0;
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
        // The game search token reads as a chip the same way: "@game" as its
        // head, the query as its value, active while the caret is in it.
        const bool option_chip = context && chip < context->option_chip_count;
        // Only the head's lead byte can start a token, and a token ends inside the source. game_token_at writes
        // the whole token when it finds one, and the loop reads it only then.
        GameToken game;
        const bool game_head = !option_chip && source.data[cursor] == kGameTokenHead[0] &&
                               game_token_at(source, cursor, &game);
        if (game_head) {
            game_end_at = game.query_last;
            game_flags = model.caret >= game.query_first && model.caret <= game.query_last
                             ? message_composer_chip_active
                             : 0;
            scene->projection_reads_caret = true;
        }
        if (option_chip || game_head || (!option_chip && cursor == game_end_at)) {
            const bool game_chip = !option_chip;
            if (game_chip)
                head = game_head;
            const auto& option = game_chip ? MessageComposerOptionChip{} : context->option_chips[chip];
            const bool named = game_chip || option.key_last > option.name_first;
            const Utf8View name = game_chip ? utf8_literal("@game")
                                  : named   ? Utf8View{source.data + option.name_first,
                                                     option.key_last - option.name_first - 1u}
                                            : Utf8View{};
            const u32 last = game_chip ? (head ? game.query_first : cursor + kGameTokenEndBytes)
                             : head    ? option.key_last
                                       : option.end_last;
            const u8 flags = game_chip ? game_flags : option.flags;
            const u32 spaces = head ? kMessageComposerChipMargin + 3 * kMessageComposerChipPadding
                                    : kMessageComposerChipPadding + kMessageComposerChipMargin;
            if (last <= end && named &&
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
                scene->projection_spans[scene->projection_span_count] = MessageComposerProjectionSpan{
                    cursor, last, display_first, scene->display_bytes, 0,
                    head ? MessageComposerProjectionKind::command_option
                         : MessageComposerProjectionKind::command_option_end,
                    flags, false};
                scene->atomic_spans[scene->projection_span_count++] = MessageComposerAtomicSpan{cursor, last};
                append_display_style(scene, display_first, scene->display_bytes - display_first, style);
                if (game_chip && !head)
                    game_end_at = ~0u;
                cursor = last;
                continue;
            }
        }
        // The flag assignment or projected_token_at writes every field before any read.
        ProjectedToken token;
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
                               (flag_token || (source.data[cursor] == '<' &&
                                               projected_token_at(source, cursor, context, &token))) &&
                               (!flag_token || display_style_available(*scene, scene->display_bytes,
                                                                       style | text_style_custom_emoji)) &&
                               token.source_last <= end &&
                               scene->projection_span_count < kMessageComposerProjectionSpanCapacity &&
                               token.label.bytes + kGameIconCell.bytes + 1 <=
                                   kMessageComposerDisplayBytes - scene->display_bytes;
        if (projected) {
            const u32 display_first = scene->display_bytes;
            if (token.marker)
                scene->display[scene->display_bytes++] = token.marker;
            if (token.icon_cell) {
                copy_bytes(scene->display + scene->display_bytes, kGameIconCell.data, kGameIconCell.bytes);
                scene->display_bytes += kGameIconCell.bytes;
            }
            copy_bytes(scene->display + scene->display_bytes, token.label.data, token.label.bytes);
            scene->display_bytes += token.label.bytes;
            scene->projection_spans[scene->projection_span_count++] = MessageComposerProjectionSpan{
                cursor,      token.source_last, display_first, scene->display_bytes,
                token.color, token.kind,        token.id,      token.animated};
            // Flags retain the model's existing Unicode caret/delete policy.
            // The media projection changes no source bytes or atomic boundary.
            scene->atomic_spans[scene->projection_span_count - 1] =
                token.kind == MessageComposerProjectionKind::regional_flag
                    ? MessageComposerAtomicSpan{}
                    : MessageComposerAtomicSpan{cursor, token.source_last};
            const bool media = token.kind == MessageComposerProjectionKind::emoji ||
                               token.kind == MessageComposerProjectionKind::regional_flag;
            const bool plain = token.kind == MessageComposerProjectionKind::game && !token.icon_cell;
            append_display_style(scene, display_first, scene->display_bytes - display_first,
                                 style | (media ? text_style_custom_emoji : plain ? 0u : text_style_mention));
            cursor = token.source_last;
            continue;
        }
        // A plain run: up to the next style edge, command name end, chip mark, game token end, flag, '<' or 0xE2 lead
        // byte (game token heads, the U+2063 marker and other punctuation), each character takes this path with this
        // style, so the run copies in one step. The model text is valid UTF-8 and every stop is a character boundary.
        const u8 lead = static_cast<u8>(source.data[cursor]);
        if (lead != '<' && lead != 0xE2u && !(have_flag && flag.first_byte == cursor)) {
            u32 stop = have_flag ? minimum(end, flag.first_byte) : end;
            if (context && cursor < context->command.name_bytes)
                stop = minimum<u32>(stop, context->command.name_bytes);
            if (style_index < scene->source_style_count) {
                const auto& span = scene->source_styles[style_index];
                stop = minimum(stop, span.byte_offset > cursor ? span.byte_offset : span.byte_offset + span.byte_count);
            }
            if (game_end_at > cursor)
                stop = minimum(stop, game_end_at);
            for (u32 n = 0; context && n < context->option_chip_count; ++n) {
                const auto& option = context->option_chips[n];
                if (option.head_first > cursor)
                    stop = minimum<u32>(stop, option.head_first);
                if (option.end_last > option.value_last && option.value_last > cursor)
                    stop = minimum<u32>(stop, option.value_last);
            }
            u32 last = cursor + 1;
            while (last < stop && source.data[last] != '<' && static_cast<u8>(source.data[last]) != 0xE2u)
                ++last;
            if (last - cursor <= kMessageComposerDisplayBytes - scene->display_bytes) {
                append_display_style(scene, scene->display_bytes, last - cursor, style);
                copy_bytes(scene->display + scene->display_bytes, source.data + cursor, last - cursor);
                scene->display_bytes += last - cursor;
                cursor = last;
                continue;
            }
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
        for (u32 byte = 0; byte < decoded.width; ++byte)
            scene->display[scene->display_bytes++] = marker ? "\xE2\x80\x8A"[byte] : model.text[cursor + byte];
        cursor += decoded.width;
    }
    scene->display_source_last = cursor;
    scene->display_caret = display_offset_from_source(*scene, model.caret);
    scene->display_anchor = display_offset_from_source(*scene, model.anchor);
    // The signature is only compared in memory: the display text a word at a time, then the spans.
    u64 signature = hash64_words(kHash64Seed, scene->display, scene->display_bytes);
    // Two offsets, or the colour with the kind and the animation flag, share one mix.
    for (u32 index = 0; index < scene->projection_span_count; ++index) {
        const MessageComposerProjectionSpan& span = scene->projection_spans[index];
        signature = hash64_mix(signature, span.source_first | static_cast<u64>(span.source_last) << 32);
        signature = hash64_mix(signature, span.display_first | static_cast<u64>(span.display_last) << 32);
        signature = hash64_mix(signature, span.color | static_cast<u64>(span.kind) << 32 |
                                              static_cast<u64>(span.animated) << 40);
        signature = hash64_mix(signature, span.target);
    }
    scene->display_signature =
        hash64_mix(signature, scene->projection_span_count |
                                  static_cast<u64>(context ? context->command.name_bytes : 0) << 32);
}

// Copies tile strings into scene storage; true when presentation differs.
bool store_attachment_tiles(MessageComposerScene* scene, const ConstSlice<UiComposerAttachmentTile>& tiles) noexcept {
    auto& strip = *scene->attachment_strip;
    u64 signature = hash64_mix(kHash64Seed, tiles.count);
    for (u32 i = 0; i < tiles.count; ++i) {
        const auto& source = tiles[i];
        // Draft names are strict UTF-16 conversions, so a cut at a scalar boundary is all they need.
        const u32 bytes = utf8_truncated_length(source.filename, kMessageComposerAttachmentNameBytes);
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
    strip.scroll = strip.count ? ui_composer_attachment_strip_clamp_scroll(
                                     ui_widget_manager_layout_node(scene->widgets, scene->attachments)->content_rectangle,
                                     strip.count, strip.scroll)
                               : 0.f;
}

// The layout style of a composer widget at creation, by index.
enum ComposerStyle : u8 {
    composer_style_root,
    composer_style_typing,
    composer_style_surface,
    composer_style_reply,
    composer_style_banner,
    composer_style_command_close,
    composer_style_mention,
    composer_style_cancel,
    composer_style_controls,
    composer_style_row,
    composer_style_edit,
    composer_style_hidden,
    composer_style_count
};
// One widget of the composer tree: the scene member that keeps its handle (null for the next plugin slot) and the
// member of its parent (null for the root).
struct ComposerWidgetSpec {
    UiWidgetHandle MessageComposerScene::*handle;
    UiWidgetHandle MessageComposerScene::*parent;
    const char* name;
    u8 name_bytes;
    u8 id;
    UiWidgetKind kind;
    ComposerStyle style;
    u8 flags;
};
constexpr ComposerWidgetSpec composer_widget(UiWidgetHandle MessageComposerScene::*handle,
                                             UiWidgetHandle MessageComposerScene::*parent, Utf8View name, u32 id,
                                             UiWidgetKind kind, ComposerStyle style, u16 flags = ui_widget_none) noexcept {
    return {handle, parent, name.data, static_cast<u8>(name.bytes), static_cast<u8>(id), kind, style,
            static_cast<u8>(flags)};
}
constexpr u16 kComposerControlFlags = static_cast<u16>(ui_widget_enabled | ui_widget_hit_test);
constexpr u16 kComposerFocusFlags = static_cast<u16>(kComposerControlFlags | ui_widget_focusable);
using Scene = MessageComposerScene;
// The creation order sets the hit-test and focus order. The option bar leads the stacked bars, before the reply bar.
constexpr ComposerWidgetSpec kComposerWidgets[]{
    composer_widget(&Scene::root, nullptr, utf8_literal("MessageComposer"), kComposerRootId, UiWidgetKind::container,
                    composer_style_root),
    composer_widget(&Scene::typing_indicator, &Scene::root, utf8_literal("TypingIndicator"), kComposerTypingId,
                    UiWidgetKind::text, composer_style_typing),
    composer_widget(&Scene::surface, &Scene::root, utf8_literal("ComposerSurface"), kComposerSurfaceId,
                    UiWidgetKind::container, composer_style_surface),
    composer_widget(&Scene::command_bar, &Scene::surface, utf8_literal("CommandBar"), kComposerCommandBarId,
                    UiWidgetKind::container, composer_style_banner),
    composer_widget(&Scene::command_close, &Scene::command_bar, utf8_literal("Close"), kComposerCommandCloseId,
                    UiWidgetKind::button, composer_style_command_close),
    composer_widget(&Scene::reply, &Scene::surface, utf8_literal("ReplyBanner"), kComposerReplyId,
                    UiWidgetKind::container, composer_style_banner),
    composer_widget(&Scene::reply_mention, &Scene::reply, utf8_literal("ReplyMention"), kComposerReplyMentionId,
                    UiWidgetKind::button, composer_style_mention),
    composer_widget(&Scene::reply_cancel, &Scene::reply, utf8_literal("CancelReply"), kComposerReplyCancelId,
                    UiWidgetKind::button, composer_style_cancel),
    composer_widget(&Scene::attachments, &Scene::surface, utf8_literal("Attachments"), kComposerAttachmentsId,
                    UiWidgetKind::button, composer_style_reply),
    composer_widget(&Scene::controls, &Scene::surface, utf8_literal("ComposerControls"), kComposerControlsId,
                    UiWidgetKind::container, composer_style_controls, ui_widget_enabled),
    composer_widget(&Scene::add_attachment, &Scene::controls, utf8_literal("AddAttachment"), kComposerAttachmentId,
                    UiWidgetKind::button, composer_style_row, kComposerControlFlags),
    composer_widget(&Scene::edit, &Scene::controls, utf8_literal("MessageEdit"), kComposerEditId,
                    UiWidgetKind::text_field, composer_style_edit, kComposerFocusFlags),
    composer_widget(nullptr, &Scene::controls, utf8_literal("PluginSlot"), kComposerPluginId, UiWidgetKind::button,
                    composer_style_hidden),
    composer_widget(nullptr, &Scene::controls, utf8_literal("PluginSlot"), kComposerPluginId + 1,
                    UiWidgetKind::button, composer_style_hidden),
    composer_widget(nullptr, &Scene::controls, utf8_literal("PluginSlot"), kComposerPluginId + 2,
                    UiWidgetKind::button, composer_style_hidden),
    composer_widget(nullptr, &Scene::controls, utf8_literal("PluginSlot"), kComposerPluginId + 3,
                    UiWidgetKind::button, composer_style_hidden),
    composer_widget(&Scene::gif, &Scene::controls, utf8_literal("GIFs"), kComposerGifId, UiWidgetKind::button,
                    composer_style_row, kComposerFocusFlags),
    composer_widget(&Scene::sticker, &Scene::controls, utf8_literal("Stickers"), kComposerStickerId,
                    UiWidgetKind::button, composer_style_row, kComposerFocusFlags),
    composer_widget(&Scene::emoji, &Scene::controls, utf8_literal("Emoji"), kComposerEmojiId, UiWidgetKind::button,
                    composer_style_row, kComposerFocusFlags),
    composer_widget(&Scene::send, &Scene::controls, utf8_literal("Send"), kComposerSendId, UiWidgetKind::button,
                    composer_style_row, kComposerFocusFlags),
};
static_assert(kMessageComposerPluginSlots == 4, "kComposerWidgets lists one entry for each plugin slot");
static_assert(kComposerCommandCloseId <= 0xFF);

} // namespace

bool message_composer_scene_initialize_owned(MessageComposerScene* scene) noexcept {
    if (!initialize_owned_storage(scene)) {
        message_composer_scene_release(scene);
        return false;
    }
    const Slice<UiWidget> widgets{arena_allocate<UiWidget>(&scene->owned_storage, kMessageComposerWidgetCapacity),
                                  kMessageComposerWidgetCapacity};
    const Slice<UiLayoutNode> layout_nodes{
        arena_allocate<UiLayoutNode>(&scene->owned_storage, kMessageComposerWidgetCapacity),
        kMessageComposerWidgetCapacity};
    if (!widgets.data || !layout_nodes.data ||
        !ui_widget_manager_initialize(&scene->widgets, widgets, layout_nodes)) {
        message_composer_scene_release(scene);
        return false;
    }
    UiLayoutStyle styles[composer_style_count];
    styles[composer_style_root] = root_style();
    styles[composer_style_typing] = typing_style();
    styles[composer_style_surface] = surface_style();
    styles[composer_style_reply] = reply_style();
    styles[composer_style_banner] = styles[composer_style_reply];
    styles[composer_style_banner].padding = UiInsets{kUiMenuRowHorizontalPadding, 0.f, 0.f, 0.f};
    UiLayoutStyle& command_close = styles[composer_style_command_close];
    command_close = fixed_control_style(kMessageComposerCommandBarHeight);
    command_close.height = ui_layout_fixed(kMessageComposerCommandBarHeight);
    command_close.horizontal_alignment = UiLayoutAlignment::end;
    command_close.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children);
    // The "@ ON" switch is padded 8x12: 16 DIP icon, 4 DIP gap, bold label.
    UiLayoutStyle& mention = styles[composer_style_mention];
    mention = fixed_control_style(kMessageComposerReplyMentionWidth);
    mention.height = ui_layout_fixed(kComposerReplyHeight);
    mention.horizontal_alignment = UiLayoutAlignment::end;
    mention.margin.right = kComposerReplyHeight + 2 * ui_theme_metrics(scene->theme_styles).composer_padding + 1;
    mention.flags = ui_layout_node_clip_children;
    UiLayoutStyle& cancel = styles[composer_style_cancel];
    cancel = fixed_control_style(kComposerReplyHeight);
    cancel.height = ui_layout_fixed(kComposerReplyHeight);
    cancel.horizontal_alignment = UiLayoutAlignment::end;
    cancel.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children);
    styles[composer_style_controls] = controls_style();
    styles[composer_style_row] = fixed_control_style(accessory_width(scene->theme_styles));
    styles[composer_style_edit] = edit_style();
    styles[composer_style_hidden] = styles[composer_style_row];
    styles[composer_style_hidden].flags = static_cast<u16>(styles[composer_style_row].flags & ~ui_layout_node_visible);
    UiWidgetBuilder b(scene->widgets);
    u32 plugin = 0;
    for (const ComposerWidgetSpec& spec : kComposerWidgets) {
        const UiWidgetResult result =
            b.widget(spec.parent ? scene->*spec.parent : ui_widget_invalid_handle(),
                     {spec.id, {spec.name, spec.name_bytes}}, spec.kind, spec.flags, styles[spec.style]);
        (spec.handle ? scene->*spec.handle : scene->plugin_slots[plugin++]) = result.handle;
    }
    if (!b.ok()) {
        message_composer_scene_release(scene);
        return false;
    }
    return true;
}

bool message_composer_scene_bind_theme_styles(MessageComposerScene* scene,
                                              const UiResolvedThemeStyles* styles) noexcept {
    if (styles && !styles->ready)
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

bool message_composer_scene_control_visible(const MessageComposerScene& scene,
                                            MessageComposerControl control) noexcept {
    const auto* node = message_composer_scene_layout_node(scene, control);
    return node && node->visible;
}

float message_composer_scene_toolbar_height(float composer_width,
                                            const UiResolvedThemeStyles* styles) noexcept {
    return composer_width <= 0.f ? 0.f : controls_toolbar_height(composer_width, styles, plugin_slot_count());
}

static float composer_row_height(float natural, const UiResolvedThemeStyles* styles) noexcept {
    const auto& metrics = ui_theme_metrics(styles);
    const float minimum_height =
        maximum(kMessageComposerMinimumHeight,
                maximum(32.f, metrics.control_height) + 2.f * metrics.composer_padding);
    return minimum(kMessageComposerMaximumHeight, maximum(minimum_height, natural));
}

float message_composer_scene_measured_height(const MessageComposerScene& scene,
                                             float text_height) noexcept {
    const auto* styles = scene.theme_styles;
    const float extras = message_composer_scene_chrome_height(
        scene.reply_message_id != 0, message_composer_scene_attachment_row(scene),
        styles, scene.command_bar_name_bytes != 0);
    return composer_row_height(text_height + editor_padding_height(styles), styles) +
           message_composer_scene_toolbar_height(ui_rect_width(scene.viewport), styles) + extras;
}

float message_composer_scene_cached_height(const MessageComposerScene& scene, bool reply, u8 attachments,
                                           float composer_width, bool command_bar) noexcept {
    const auto* styles = scene.theme_styles;
    const float width = composer_width > 0.f ? composer_width : ui_rect_width(scene.viewport);
    return composer_row_height(scene.measured_row_height, styles) +
           message_composer_scene_toolbar_height(width, styles) +
           message_composer_scene_chrome_height(reply, attachments, styles, command_bar);
}

MessageComposerSceneResult message_composer_scene_update(
    MessageComposerScene* scene, MessageComposer* model, const UiRect& viewport, bool channel_allows_send,
    u64 reply_message_id, const Utf8View& reply_label, const MessageComposerProjectionContext* projection,
    const Utf8View& typing_label, MessageComposerAttachmentInput attachments, bool sending_restricted,
    bool mention_reply_author, bool reply_mention_visible, bool status_is_error,
    bool reply_sendable, u8 status_tone) noexcept {
    if (!ui_rect_valid_or_empty(viewport) ||
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
        return failure(MessageComposerSceneError::invalid_input);
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
    const u32 plugin_count = plugin_slot_count();
    gif->flags = sticker->flags =
        compact_controls(ui_rect_width(viewport), scene->theme_styles, plugin_count) || voice_bar
            ? ui_widget_none
            : enabled_if(can_send);
    edit->flags = voice_bar ? static_cast<u16>(ui_widget_hit_test) : enabled_if(channel_allows_send);
    for (u32 i = 0; i < kMessageComposerPluginSlots; ++i)
        ui_widget_manager_resolve(&scene->widgets, scene->plugin_slots[i])->flags =
            i < plugin_count && !voice_bar ? static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable)
                                           : ui_widget_none;
    const bool plugins_changed = scene->plugin_count != plugin_count;
    scene->plugin_count = static_cast<u8>(plugin_count);
    ui_widget_manager_reconcile_input(&scene->widgets);
    UiWidget* reply = ui_widget_manager_resolve(&scene->widgets, scene->reply);
    UiLayoutNode* reply_layout =
        ui_widget_manager_layout_node(&scene->widgets, scene->reply);
    UiWidget* mention = ui_widget_manager_resolve(&scene->widgets, scene->reply_mention);
    UiWidget* cancel = ui_widget_manager_resolve(&scene->widgets, scene->reply_cancel);
    UiLayoutNode* mention_layout =
        ui_widget_manager_layout_node(&scene->widgets, scene->reply_mention);
    UiWidget* attachment_widget = ui_widget_manager_resolve(&scene->widgets, scene->attachments);
    UiLayoutNode* attachment_layout =
        ui_widget_manager_layout_node(&scene->widgets, scene->attachments);
    UiWidget* command_bar = ui_widget_manager_resolve(&scene->widgets, scene->command_bar);
    UiLayoutNode* command_bar_layout =
        ui_widget_manager_layout_node(&scene->widgets, scene->command_bar);
    UiWidget* command_close = ui_widget_manager_resolve(&scene->widgets, scene->command_close);
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
    ui_widget_manager_reconcile_input(&scene->widgets);
    const bool attachments_resized = previous_attachment_row != attachment_row;
    const bool relayout = scene->geometry_dirty || !scene->active || reply_changed || plugins_changed ||
                          mention_visibility_changed || attachments_resized || bar_resized ||
                          !ui_rect_equal(scene->viewport, viewport);
    bool changed = relayout || scene->model != model || scene->observed_revision != model->revision ||
                   scene->command.name_bytes != (projection ? projection->command.name_bytes : 0) ||
                   !media_asset_key_equal(scene->command.avatar,
                                          projection ? projection->command.avatar : MediaAssetKey{}) ||
                   scene->channel_allows_send != can_send || reply_changed || typing_changed || bar_changed ||
                   attachments_changed || scene->mention_reply_author != mention_reply_author ||
                   scene->status_is_error != status_is_error || scene->reply_sendable != reply_sendable ||
                   scene->status_tone != status_tone;
    changed = update_placeholder(scene, *model, projection) || changed;
    if (relayout) {
        configure_controls(scene, viewport,
                           message_composer_scene_chrome_height(reply_message_id != 0, attachment_row,
                                                                scene->theme_styles,
                                                                scene->command_bar_name_bytes != 0));
        if (!ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok())
            return failure(MessageComposerSceneError::layout);
        ++scene->layout_passes;
        scene->geometry_dirty = false;
    }
    const u64 previous_display_signature = scene->display_signature;
    const u64 input_signature = projection_input_signature(*scene, *model, projection);
    // The model's anchor and caret in one word: the selection that display_anchor and display_caret show.
    const u64 selection = model->anchor | static_cast<u64>(model->caret) << 32;
    if (!scene->active || scene->model != model || scene->projection_input_signature != input_signature) {
        project_display(scene, *model, projection);
        scene->projection_input_signature = projection_input_signature(*scene, *model, projection);
        scene->projected_selection = selection;
    } else if (scene->projected_selection != selection) {
        // The projection did not read the caret: a caret move changes the display selection only.
        scene->display_caret = display_offset_from_source(*scene, model->caret);
        scene->display_anchor = display_offset_from_source(*scene, model->anchor);
        scene->projected_selection = selection;
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
    if (!scene.active || scene.command.avatar.kind == MediaAssetKind::none)
        return {};
    const auto* attachment = message_composer_scene_layout_node(scene, MessageComposerControl::add_attachment);
    const float extent = ui_theme_metrics(scene.theme_styles).menu_icon;
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
    return Utf8View{scene.attachment_strip->label, scene.attachment_label_bytes};
}
u8 message_composer_scene_attachment_row(const MessageComposerScene& scene) noexcept {
    return scene.attachment_strip->count ? message_composer_attachment_row_strip
           : scene.attachment_label_bytes ? message_composer_attachment_row_label
                                          : message_composer_attachment_row_none;
}
ConstSlice<UiComposerAttachmentTile> message_composer_scene_attachment_tiles(const MessageComposerScene& scene) noexcept {
    return {scene.attachment_strip->tiles, scene.attachment_strip->count};
}
UiComposerAttachmentStripView message_composer_scene_attachment_view(const MessageComposerScene& scene,
                                                                     bool focus_visible) noexcept {
    UiComposerAttachmentStripView view{};
    view.hovered = view.selected = kUiComposerAttachmentNoTile;
    if (!scene.active || !scene.attachment_strip->count)
        return view;
    const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.attachments);
    if (!node->visible)
        return view;
    const auto& strip = *scene.attachment_strip;
    view.tiles = {strip.tiles, strip.count};
    view.strip = ui_rect_intersection(node->content_rectangle, node->clip_rectangle);
    view.scroll = strip.scroll;
    view.hovered = strip.hovered;
    view.hovered_part = strip.hovered_part;
    view.selected = message_composer_scene_attachment_selected(scene);
    view.focus_visible = focus_visible && ui_widget_handle_matches(scene.widgets.focused, scene.attachments);
    view.owner = {&scene.widgets, scene.attachments};
    return view;
}
UiComposerAttachmentHit message_composer_scene_attachment_hit(const MessageComposerScene& scene, float x,
                                                              float y) noexcept {
    const auto view = message_composer_scene_attachment_view(scene, false);
    return ui_composer_attachment_strip_hit(view.strip, view.tiles, view.scroll, x, y);
}
bool message_composer_scene_attachment_hover(MessageComposerScene* scene, UiComposerAttachmentHit hit) noexcept {
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
    if (!scene->attachment_strip->count || delta != delta)
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
    if (!scene.attachment_strip->selected_id)
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

void message_composer_scene_set_voice(MessageComposerScene* scene, const UiComposerVoiceView* view) noexcept {
    const UiComposerVoiceMode mode = view ? view->mode : UiComposerVoiceMode::none;
    if (ui_composer_voice_bar_visible(scene->voice_mode) != ui_composer_voice_bar_visible(mode))
        scene->geometry_dirty = true;
    scene->voice_mode = mode;
    scene->voice = view;
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
    static constexpr MessageComposerControl controls[] = {
        MessageComposerControl::command_close, MessageComposerControl::cancel_reply,
        MessageComposerControl::reply_mention,
        MessageComposerControl::edit,         MessageComposerControl::gif,
        MessageComposerControl::sticker,      MessageComposerControl::emoji,
        MessageComposerControl::send,         MessageComposerControl::add_attachment,
        MessageComposerControl::attachments};
    for (auto control : controls) {
        const auto handle = control_handle(scene, control);
        if (ui_widget_handle_equal(handle, scene.widgets.focused))
            return control;
    }
    return message_composer_scene_focused_plugin_slot(scene) < kMessageComposerPluginSlots
               ? MessageComposerControl::plugin_slot
               : MessageComposerControl::none;
}

u32 message_composer_scene_focused_plugin_slot(const MessageComposerScene& scene) noexcept {
    for (u32 i = 0; scene.active && i < scene.plugin_count; ++i)
        if (ui_widget_handle_equal(scene.plugin_slots[i], scene.widgets.focused))
            return i;
    return kMessageComposerPluginSlots;
}

void message_composer_scene_set_caret_visible(MessageComposerScene* scene, bool visible) noexcept {
    scene->caret_visible = visible;
}

void message_composer_scene_release(MessageComposerScene* scene) noexcept {
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
    auto rectangle = reply->content_rectangle;
    rectangle.right =
        maximum(rectangle.left, cancel->rectangle.left - ui_theme_metrics(scene.theme_styles).menu_gap);
    if (scene.reply_mention_visible) {
        const auto* mention = message_composer_scene_layout_node(scene, MessageComposerControl::reply_mention);
        rectangle.right =
            maximum(rectangle.left, mention->rectangle.left - ui_theme_metrics(scene.theme_styles).menu_gap);
    }
    return rectangle;
}

Utf8View message_composer_scene_command_bar_name(const MessageComposerScene& scene) noexcept {
    return Utf8View{scene.command_bar_text, scene.command_bar_name_bytes};
}
Utf8View message_composer_scene_command_bar_detail(const MessageComposerScene& scene) noexcept {
    return Utf8View{scene.command_bar_text + scene.command_bar_name_bytes, scene.command_bar_detail_bytes};
}
UiRect message_composer_scene_command_bar_text_rectangle(const MessageComposerScene& scene) noexcept {
    const auto* bar = ui_widget_manager_layout_node(scene.widgets, scene.command_bar);
    const auto* close = message_composer_scene_layout_node(scene, MessageComposerControl::command_close);
    if (!scene.command_bar_name_bytes)
        return {};
    auto rectangle = bar->content_rectangle;
    rectangle.right = maximum(rectangle.left, close->rectangle.left - ui_theme_metrics(scene.theme_styles).menu_gap);
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
    i64 adjustment = 0;
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
    if (!scene.model || span.start >= span.end)
        return false;
    u32 begin = 0, end = 0;
    const auto source = message_composer_text(*scene.model);
    if (!utf8_byte_offset_for_utf16_position(source, span.start, &begin) ||
        !utf8_byte_offset_for_utf16_position(source, span.end, &end) || end == 0 ||
        begin >= scene.display_source_last)
        return false;
    end = minimum(end, scene.display_source_last);
    const auto display = message_composer_scene_display_text(scene);
    return utf8_utf16_position_for_byte_offset(
               display, message_composer_scene_display_offset_from_model(scene, begin), first) &&
           utf8_utf16_position_for_byte_offset(
               display, message_composer_scene_display_offset_from_model(scene, end), last) &&
           *first < *last;
}

u32 message_composer_scene_chip_ranges(const MessageComposerScene& scene,
                                       const Slice<MessageComposerChipRange>& output) noexcept {
    if (!scene.model)
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

float message_composer_scene_chrome_height(
    bool reply, u8 attachments, const UiResolvedThemeStyles* styles,
    bool command_bar) noexcept {
    const float gap = ui_theme_metrics(styles).composer_padding;
    // The typing strip is always reserved: the indicator draws inside it, so the message list above never moves.
    return (command_bar ? kMessageComposerCommandBarHeight + gap : 0.f) + (reply ? kComposerReplyHeight + gap : 0.f) +
           kComposerTypingHeight + gap +
           (attachments == message_composer_attachment_row_strip ? ui_composer_attachment_strip_height() + gap
            : attachments                                         ? kComposerReplyHeight + gap
                                                                  : 0.f);
}

} // namespace maicord
