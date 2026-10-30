#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/composer_attachment_strip.hpp"
#include "maicord/ui/composer_voice_bar.hpp"
#include "maicord/ui/widget_manager.hpp"
#include "maicord/ui/text_composition.hpp"
#include "maicord/ui/text_style.hpp"
#include "maicord/ui/theme_styles.hpp"

namespace maicord {
struct UserStore;

struct ChatStateStore;

// Plugin composer-toolbar buttons sit before the GIF button.
inline constexpr u32 kMessageComposerPluginSlots = 4;
inline constexpr u32 kMessageComposerWidgetCapacity = 16 + kMessageComposerPluginSlots;
inline constexpr u32 kMessageComposerDisplayBytes = kMessageComposerMaximumBytes;
inline constexpr u32 kMessageComposerSelectionRectangleCapacity = 48;
inline constexpr u32 kMessageComposerProjectionSpanCapacity = 128;
inline constexpr u32 kMessageComposerReplyLabelBytes = 256;
inline constexpr u32 kMessageComposerPlaceholderBytes = 512;
inline constexpr u32 kMessageComposerCommandBarBytes = 1536;
// The client's slash command option bar, stacked over the editor like the reply bar.
inline constexpr float kMessageComposerCommandBarHeight = 32.0f;

enum class MessageComposerProjectionKind : u8 {
    user,
    role,
    channel,
    emoji,
    regional_flag,
    // A slash command option's "name:" key (with its separating blank, if
    // any), drawn as the head of the client's option chip (target:
    // message_composer_chip_*).
    command_option,
    // The end marker of an option's value (kCommandOptionEnd): the chip's end
    // padding; text after it is outside the chip.
    command_option_end,
};

// One typed option of the selected command, in source bytes: the head's start
// (the blank before the key, or the key), the name's start, the key's end
// (after ':'), the value's end and the end of its end marker (equal while the
// value is not terminated yet).
struct MessageComposerOptionChip {
    u16 head_first, name_first, key_last, value_last, end_last;
    u8 flags;
};
inline constexpr u8 message_composer_chip_active = 1;
inline constexpr u8 message_composer_chip_error = 2;
// A chip's head shows the margin, then padding, the name and padding, and the
// value's start padding; its end marker the value's end padding and the
// margin; all in no-break spaces (one UTF-16 unit each).
inline constexpr u32 kMessageComposerChipMargin = 1;
inline constexpr u32 kMessageComposerChipPadding = 2;

struct MessageComposerCommand {
    u64 bot_id;
    u32 avatar_variant;
    u16 name_bytes; // Includes the leading slash; source bytes remain editable.
    bool animated;
};
static_assert(sizeof(MessageComposerCommand) == 16);
static_assert(__is_trivial(MessageComposerCommand) && __is_standard_layout(MessageComposerCommand));

struct MessageComposerProjectionContext {
    const UserStore* users;
    const ChatStateStore* state;
    u64 guild_id;
    // Replaces the "Message #channel" placeholder while non-empty.
    Utf8View placeholder_override{};
    MessageComposerCommand command{};
    // The selected command's typed options, in source order, drawn as the
    // client's option chips.
    u32 option_chip_count{};
    MessageComposerOptionChip option_chips[25]{};
    // The client's option bar over the editor while a command is selected:
    // the active option's name and description (its failed check instead when
    // error), else "/command" and its description. Empty name: no bar.
    Utf8View command_bar_name{}, command_bar_detail{};
    bool command_bar_error{};
};

struct MessageComposerProjectionSpan {
    u32 source_first;
    u32 source_last;
    u32 display_first;
    u32 display_last;
    u32 color;
    MessageComposerProjectionKind kind;
    u64 target;
    bool animated;
};

enum class MessageComposerControl : u8 {
    none,
    add_attachment,
    edit,
    emoji,
    send,
    cancel_reply,
    typing_indicator,
    attachments,
    gif,
    sticker,
    reply_mention,
    plugin_slot,
    // The option bar's close button: ends the selected command.
    command_close,
};

enum class MessageComposerSceneError : u8 {
    none,
    invalid_input,
    widget,
    layout,
};

struct MessageComposerSceneResult {
    MessageComposerSceneError error;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == MessageComposerSceneError::none;
    }
};

// One line's piece of a slash command option chip in edit-content DIPs: the
// option name segment (empty on a continuation line) and the value segment.
struct MessageComposerChipGeometry {
    UiRect key, value;
    u8 flags;
};
inline constexpr u32 kMessageComposerChipGeometryCapacity = 32;

struct MessageComposerPresentationInput {
    ConstSlice<UiRect> selections;
    UiRect caret;
    bool geometry_active;
    bool caret_visible;
    ConstSlice<TextCompositionMark> composition;
    ConstSlice<MessageComposerChipGeometry> chips{};
};

struct MessageComposerAttachmentInput {
    Utf8View label;
    bool ready;
    bool can_attach;
    bool retained_content{};
    // Tiles replace the one-line label with the upload strip. The label stays
    // the send-admission summary; tile strings are copied by the update.
    ConstSlice<UiComposerAttachmentTile> tiles{};
};

// Height class of the attachment row: none, the 28 DIP label, or the strip.
enum MessageComposerAttachmentRow : u8 {
    message_composer_attachment_row_none = 0,
    message_composer_attachment_row_label = 1,
    message_composer_attachment_row_strip = 2,
};
constexpr u8 message_composer_attachment_row(const MessageComposerAttachmentInput& input) noexcept {
    return input.tiles.count ? message_composer_attachment_row_strip
           : input.label.bytes ? message_composer_attachment_row_label
                               : message_composer_attachment_row_none;
}

inline constexpr u32 kMessageComposerAttachmentNameBytes = 256;
struct MessageComposerAttachmentStrip {
    UiComposerAttachmentTile tiles[kUiComposerAttachmentMaximum];
    char names[kUiComposerAttachmentMaximum][kMessageComposerAttachmentNameBytes];
    char label[kMessageComposerReplyLabelBytes];
    u64 signature;
    u64 selected_id;
    float scroll;
    u32 count;
    u32 hovered;
    UiComposerAttachmentPart hovered_part;
};

struct MessageComposerScene {
    MessageComposerCommand command;
    MessageComposerAtomicSpan* atomic_spans;
    TextStyleSpan* source_styles;
    TextStyleSpan* display_styles;
    u64 styles_document;
    u32 styles_revision;
    u32 source_style_count;
    u32 display_style_count;
    // Natural editable row including its padding; excludes auxiliary rows and
    // the width-dependent accessory toolbar.
    float measured_row_height;
    // Borrowed resolved bank; it must outlive the scene. Geometry changes
    // invalidate measurement without rebuilding the source-text view.
    const UiResolvedThemeStyles* theme_styles;
    UiWidgetManager widgets;
    Arena owned_storage;
    MessageComposer* model;
    UiWidgetHandle root;
    UiWidgetHandle surface;
    UiWidgetHandle controls;
    // The banner is static; only its ping and cancel children accept input.
    UiWidgetHandle reply;
    UiWidgetHandle reply_mention;
    UiWidgetHandle reply_cancel;
    // The selected slash command's option bar and its close button.
    UiWidgetHandle command_bar, command_close;
    UiWidgetHandle attachments;
    UiWidgetHandle add_attachment;
    UiWidgetHandle edit;
    UiWidgetHandle emoji;
    UiWidgetHandle gif, sticker;
    UiWidgetHandle send;
    UiWidgetHandle typing_indicator;
    UiWidgetHandle plugin_slots[kMessageComposerPluginSlots];
    UiRect viewport;
    char* display;
    char* reply_label;
    MessageComposerAttachmentStrip* attachment_strip;
    char* typing_label;
    char* placeholder;
    // The option bar's name then detail (its failed check when command_bar_error).
    char* command_bar_text;
    MessageComposerProjectionSpan* projection_spans;
    u32 display_bytes;
    u32 display_first;
    u32 display_source_last;
    u32 display_anchor;
    u32 display_caret;
    u32 projection_span_count;
    u32 observed_revision;
    ConstSlice<TextCompositionSpan> composition;
    u32 composition_revision;
    // Misspelling squiggles in the same UTF-16 model positions, valid while the
    // model's text_revision equals spelling_text_revision. Changes bump
    // composition_revision.
    u32 spelling_text_revision;
    ConstSlice<TextCompositionSpan> spelling;
    u32 observed_geometry_revision;
    u64 reply_message_id;
    u64 display_signature;
    u64 projection_input_signature;
    u16 reply_label_bytes;
    u16 attachment_label_bytes;
    u16 typing_label_bytes;
    u16 placeholder_bytes;
    u16 command_bar_name_bytes, command_bar_detail_bytes;
    bool command_bar_error;
    u32 layout_passes;
    u32 projection_passes;
    bool channel_allows_send;
    bool attachments_ready;
    bool can_attach;
    bool caret_visible;
    bool active;
    bool owns_storage;
    bool mention_reply_author;
    bool reply_mention_visible;
    bool retained_content;
    bool status_is_error;
    bool reply_sendable;
    // Status line tone when not an error: 0 neutral, 1 accent, 2 positive,
    // 3 warning, 4 critical (plugin notices).
    u8 status_tone;
    // The status line is someone typing, not a notice: its dots pulse at
    // typing_phase (typing_indicator.hpp), which the host's frame clock sets.
    bool typing_dots;
    u16 typing_phase;
    bool geometry_dirty;
    // Visible plugin_slots, from the composer toolbar plugin query.
    u8 plugin_count;
    // The account's premium_type, from the last projection (message length).
    u8 premium_type;
    // Voice message presentation (borrowed from the host until the next set).
    UiComposerVoiceMode voice_mode;
    const UiComposerVoiceView* voice;
};

// Bot identity replaces the attachment button for a selected command.
UiRect message_composer_scene_command_avatar(const MessageComposerScene&) noexcept;

bool message_composer_scene_initialize(MessageComposerScene* scene, Slice<UiWidget> widgets,
                                       Slice<UiLayoutNode> layout_nodes) noexcept;
bool message_composer_scene_initialize_owned(MessageComposerScene* scene) noexcept;
// Returns true when geometry must be measured again. An unready bank is rejected;
// nullptr restores the defaults. Paint-only changes do not invalidate layout.
bool message_composer_scene_bind_theme_styles(MessageComposerScene* scene,
                                              const UiResolvedThemeStyles* styles) noexcept;
// Widths are full composer viewport widths in DIPs. The editable row keeps
// its natural height. Below the inline width the GIF and sticker buttons fold
// into the emoji picker; only below the compact width do the trailing actions
// wrap into additional toolbar rows.
float message_composer_scene_text_width(float composer_width,
                                        const UiResolvedThemeStyles* styles = nullptr) noexcept;
float message_composer_scene_minimum_inline_width(const UiResolvedThemeStyles* styles = nullptr) noexcept;
float message_composer_scene_minimum_compact_width(const UiResolvedThemeStyles* styles = nullptr) noexcept;
// False for accessories folded away at the current width.
bool message_composer_scene_control_visible(const MessageComposerScene&, MessageComposerControl) noexcept;
// Keyboard-focused plugin toolbar button, or kMessageComposerPluginSlots.
u32 message_composer_scene_focused_plugin_slot(const MessageComposerScene&) noexcept;
float message_composer_scene_toolbar_height(float composer_width,
                                            const UiResolvedThemeStyles* styles = nullptr) noexcept;
// attachments is a MessageComposerAttachmentRow; true selects the label row.
float message_composer_scene_preferred_height(const MessageComposer& model, float composer_width,
                                              bool reply_visible, bool typing_visible,
                                              u8 attachments_visible = 0,
                                              const UiResolvedThemeStyles* styles = nullptr) noexcept;
float message_composer_scene_measured_height(const MessageComposerScene& scene, float text_height) noexcept;
// command_bar: the selected slash command's option bar (stacked first).
constexpr float message_composer_scene_chrome_height(
    bool reply, bool typing, u8 attachments, const UiResolvedThemeStyles* styles = nullptr,
    bool command_bar = false) noexcept {
    const float gap = styles && styles->ready ? styles->metrics.composer_padding : 4.f;
    return (command_bar ? kMessageComposerCommandBarHeight + gap : 0.f) + (reply ? 28.f + gap : 0.f) +
           (typing ? 20.f + gap : 0.f) +
           (attachments == message_composer_attachment_row_strip ? ui_composer_attachment_strip_height() + gap
            : attachments                                         ? 28.f + gap
                                                                  : 0.f);
}
float message_composer_scene_cached_height(const MessageComposerScene& scene, bool reply, bool typing,
                                           u8 attachments, float composer_width = 0.f,
                                           bool command_bar = false) noexcept;
// The option bar's name, its detail, and where both are drawn (empty without a bar).
Utf8View message_composer_scene_command_bar_name(const MessageComposerScene& scene) noexcept;
Utf8View message_composer_scene_command_bar_detail(const MessageComposerScene& scene) noexcept;
UiRect message_composer_scene_command_bar_text_rectangle(const MessageComposerScene& scene) noexcept;
MessageComposerSceneResult message_composer_scene_update(
    MessageComposerScene* scene, MessageComposer* model, UiRect viewport, bool channel_allows_send,
    u64 reply_message_id = 0, Utf8View reply_label = {},
    const MessageComposerProjectionContext* projection = nullptr, Utf8View typing_label = {},
    MessageComposerAttachmentInput attachments = {}, bool sending_restricted = false,
    // Edit banners share the reply row; their host hides the author-mention toggle.
    bool mention_reply_author = true, bool reply_mention_visible = true, bool status_is_error = false,
    bool reply_sendable = true, u8 status_tone = 0) noexcept;
bool message_composer_scene_can_send(const MessageComposerScene& scene) noexcept;
// Voice message mode. available turns Send into the microphone; the recording
// modes turn the editor into the recording bar, Add into Delete and Emoji into
// Stop or Play/Pause, and fold GIF, stickers and plugins away. True when the
// presentation changed (the next update relayouts as needed).
bool message_composer_scene_set_voice(MessageComposerScene* scene, const UiComposerVoiceView* view) noexcept;
UiComposerVoiceMode message_composer_scene_voice_mode(const MessageComposerScene& scene) noexcept;
Utf8View message_composer_scene_attachment_label(const MessageComposerScene& scene) noexcept;
u8 message_composer_scene_attachment_row(const MessageComposerScene& scene) noexcept;
ConstSlice<UiComposerAttachmentTile> message_composer_scene_attachment_tiles(const MessageComposerScene& scene) noexcept;
// Strip geometry for paint/text/thumbnails; the owner is the strip widget.
UiComposerAttachmentStripView message_composer_scene_attachment_view(const MessageComposerScene& scene,
                                                                     bool focus_visible) noexcept;
UiComposerAttachmentHit message_composer_scene_attachment_hit(const MessageComposerScene& scene, float x,
                                                              float y) noexcept;
// Each returns true when presentation changed.
bool message_composer_scene_attachment_hover(MessageComposerScene* scene, UiComposerAttachmentHit hit) noexcept;
bool message_composer_scene_attachment_scroll(MessageComposerScene* scene, float delta) noexcept;
// Keyboard selection by tile index; reveals it. Out-of-range clears.
bool message_composer_scene_attachment_select(MessageComposerScene* scene, u32 index) noexcept;
u32 message_composer_scene_attachment_selected(const MessageComposerScene& scene) noexcept;
MessageComposerControl message_composer_scene_hit_test(const MessageComposerScene& scene, float x,
                                                       float y) noexcept;
// Index of the plugin_slot under the point, or kMessageComposerPluginSlots.
u32 message_composer_scene_plugin_slot(const MessageComposerScene& scene, float x, float y) noexcept;
const UiLayoutNode* message_composer_scene_layout_node(const MessageComposerScene& scene,
                                                       MessageComposerControl control) noexcept;
bool message_composer_scene_focus_edit(MessageComposerScene* scene) noexcept;
bool message_composer_scene_focus_control(MessageComposerScene* scene,
                                          MessageComposerControl control) noexcept;
MessageComposerControl message_composer_scene_focused_control(const MessageComposerScene& scene) noexcept;
bool message_composer_scene_set_caret_visible(MessageComposerScene* scene, bool visible) noexcept;
void message_composer_scene_release(MessageComposerScene* scene) noexcept;
Utf8View message_composer_scene_display_text(const MessageComposerScene& scene) noexcept;
Utf8View message_composer_scene_reply_label(const MessageComposerScene& scene) noexcept;
Utf8View message_composer_scene_reply_mention_label(const MessageComposerScene& scene) noexcept;
// The reply mention switch has no button box: an @ icon and the label, both
// in the link colour when pinging and muted when not.
inline constexpr float kMessageComposerReplyMentionWidth = 72.0f;
inline constexpr float kMessageComposerReplyMentionPadding = 12.0f;
UiRect message_composer_scene_reply_mention_icon(const MessageComposerScene& scene) noexcept;
UiRect message_composer_scene_reply_mention_text(const MessageComposerScene& scene) noexcept;
UiRect message_composer_scene_reply_text_rectangle(const MessageComposerScene& scene) noexcept;
Utf8View message_composer_scene_typing_label(const MessageComposerScene& scene) noexcept;
Utf8View message_composer_scene_placeholder(const MessageComposerScene& scene) noexcept;
u32 message_composer_scene_display_offset_from_model(const MessageComposerScene& scene,
                                                     u32 model_byte_offset) noexcept;
u32 message_composer_scene_model_offset_from_display(const MessageComposerScene& scene,
                                                     u32 display_byte_offset) noexcept;
// Converts native UTF-16 ACP ranges through mentions and the visible window.
bool message_composer_scene_composition_range(const MessageComposerScene& scene,
                                              const TextCompositionSpan& span, u32* first,
                                              u32* last) noexcept;
// The visible option chips in UTF-16 units of the display text: the chip's
// start (after the margin), the end of its name segment and of its value.
// open_end: the value runs to the end of the text, so its end padding lies past
// the last glyph. Returns the count.
struct MessageComposerChipRange {
    u32 first, key_last, value_last;
    u8 flags;
    bool open_end;
};
u32 message_composer_scene_chip_ranges(const MessageComposerScene& scene,
                                       Slice<MessageComposerChipRange> output) noexcept;

// Size budget: 512 bytes plus the plugin slot handles and count, the spelling
// slice and its revision, the voice view, the status tone (one aligned word)
// and the widget manager's hit filter (function and context). Selected command
// identity adds exactly one 16-byte inline record, without another allocation,
// and its option bar two widget handles, its text pointer and one aligned word.
static_assert(sizeof(MessageComposerScene) < 512 + sizeof(UiWidgetHandle) * kMessageComposerPluginSlots + 8 +
                                                 sizeof(ConstSlice<TextCompositionSpan>) + 8 +
                                                 sizeof(const UiComposerVoiceView*) + 8 + 2 * sizeof(void*) +
                                                 sizeof(MessageComposerCommand) + 2 * sizeof(UiWidgetHandle) +
                                                 sizeof(char*) + 8);

} // namespace maicord
