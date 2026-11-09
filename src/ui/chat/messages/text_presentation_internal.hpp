#pragma once

#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "maicord/ui/text/flow_layout.hpp"

#include "maicord/ui/chat/messages/system_text.hpp"

namespace maicord::text_presentation_detail {

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

union FloatWord {
    float value;
    u32 word;
};

enum class MarkdownCacheMode : u8 { content = 1, reply = 2 };
// How a command is measured, aligned and shaped. The presets are the labels
// that recur; the defaults are a plain wrapping block.
struct CommandLook {
    MessageTextMeasurementKind kind;
    float minimum_height;
    float gap_after;
    bool themed;
    UiTextAlignment horizontal;
    UiTextAlignment vertical;
    TextWrapping wrapping;
    u8 slot;
    TextOverflow overflow;
    u16 source_id;
    bool interface_text;
};

inline constexpr CommandLook kLookMiddle{.vertical = UiTextAlignment::center,
                                         .wrapping = TextWrapping::no_wrap,
                                         .overflow = TextOverflow::ellipsis};
inline constexpr Color kNoRoleColor{};
inline constexpr ConstSlice<TextStyleSpan> kNoStyles{};
inline constexpr ConstSlice<MessageMarkdownSemantic> kNoSemantics{};
inline constexpr Utf8View kNoLink{};

struct RetainedReplySummary {
    Utf8View text;
    ConstSlice<TextStyleSpan> styles;
    // Icon cells of link chips in the quoted text (never hit targets).
    ConstSlice<MessageMarkdownSemantic> semantics;
};

// A quote renders as a block: a 4-DIP spine, 12/8-DIP inline padding, at most
// 90% of the markup width and 4-DIP block margins. A fenced code block is a
// framed block (kMessageCodeBlockInset on every side, full markup width in
// embeds and 90% in messages, kMessageCodeBlockMargin above). Runs of quoted,
// code and plain text become separate commands, so wrapped lines keep their
// indent; the newlines between two runs belong to neither.
constexpr float kQuoteSpine = 4.0f;
constexpr float kQuoteStartPadding = 12.0f;
constexpr u32 kQuoteSegmentMaximum = 32;

struct QuoteSegment {
    Utf8View text;
    ConstSlice<TextStyleSpan> styles;
    ConstSlice<MessageMarkdownSemantic> semantics;
    bool quote;
    bool code;
};

// The "loading" (deferred interaction) and ephemeral message flags.
constexpr u32 kMessageFlagEphemeral = 1u << 6;
constexpr u32 kMessageFlagLoading = 1u << 7;
// Semantic target selecting the pinned-messages action of a system row.
constexpr u64 kSystemPinsTarget = 1;

u32 scoped_source_offset(u16 source_id, u32 offset) noexcept;
MessageRichString rich_source_string(const MessageRichRecord& rich, u16 source_id) noexcept;
u64 command_content_signature(const MessageTextCommand& command) noexcept;
bool fail(MessageTextPresentation* presentation, MessageTextPresentationError error) noexcept;
void set_failure_context(MessageTextPresentation* presentation, MessageTextPresentationStage stage,
                         MessageTextPresentationOperation operation,
                         u16 item_index = kInvalidMessageTextMeasurement,
                         UiTextFlowError flow_error = UiTextFlowError::none) noexcept;
void set_failure_operation(MessageTextPresentation* presentation,
                           MessageTextPresentationOperation operation,
                           UiTextFlowError flow_error = UiTextFlowError::none) noexcept;
Utf8View member_nickname(const MessageTextPresentation& presentation, const ChatStateStore& state,
                         u64 channel_id, u64 user_id) noexcept;
Utf8View resolve_entity(void* opaque, MessageMarkdownEntityKind kind, u64 id) noexcept;
bool resolve_channel(void* opaque, u64 guild_id, u64 channel_id, MessageMarkdownChannelTarget* target) noexcept;
TimestampFormatResult format_timestamp(void* opaque, u64 unix_seconds, char style,
                                              Slice<char> output) noexcept;
MessageMarkdownResult cached_markdown_parse(MessageTextPresentation* presentation, BuildContext* context,
                                            MarkdownCacheMode mode, const Utf8View& source, const Slice<char>& text_output,
                                            const Slice<u32>& scratch, const Slice<TextStyleSpan>& span_output,
                                            const MessageMarkdownOptions& options,
                                            const Slice<MessageMarkdownSemantic>& semantic_output) noexcept;
bool append_command(MessageTextPresentation* presentation, const Utf8View& text,
                    const ConstSlice<TextStyleSpan>& styles, const ConstSlice<MessageMarkdownSemantic>& semantics,
                    const UiWidgetReference& owner,
                    const UiRect& rectangle, const UiRect& clip, const Color& color,
                    const MessageRecord* record, u16 measurement_index, const CommandLook& look,
                    const Color& inline_role_color = kNoRoleColor) noexcept;
bool begin_flow_item_clamped(UiTextFlowLayout* flow, float left_inset, float right_inset,
                             UiRect* rectangle) noexcept;
bool begin_item(MessageTextPresentation* presentation, UiTextFlowLayout* flow, float left_inset,
                float right_inset, UiRect* rectangle) noexcept;
bool commit_last_command(MessageTextPresentation* presentation, UiTextFlowLayout* flow) noexcept;
// Stack-heavy (the reply's span and chip records) and only for reply rows: kept out of append_row's frame,
// which every row enters.
__declspec(noinline) RetainedReplySummary
retain_reply_summary(MessageTextPresentation* presentation, BuildContext* context, Utf8View author,
                     Utf8View content, bool available, u8 content_flags, u8 message_type,
                     Utf8View poll_question, u8 quote_flags, u64 sent_unix_seconds) noexcept;
RetainedReplySummary retain_reply_placeholder(MessageTextPresentation* presentation, Utf8View text) noexcept;
RetainedReplySummary retain_reply_plain(MessageTextPresentation* presentation, const MessageSystemText& system) noexcept;
RetainedReplySummary retain_reply_media_mention(MessageTextPresentation* presentation, Utf8View author,
                                                bool at_sign, Utf8View content) noexcept;
RetainedReplySummary retain_interaction_header(MessageTextPresentation* presentation, Utf8View user,
                                               Utf8View command, bool chip) noexcept;
bool append_reply_line(MessageTextPresentation* presentation, const Utf8View& text,
                       const ConstSlice<TextStyleSpan>& styles, UiTextFlowLayout* flow, const Color& color,
                       const Color& inline_role_color, const UiWidgetReference& owner, const MessageRecord& record,
                       u16 measurement_index, TextOverflow overflow,
                       float right_inset = 0.0f, float left_inset = kMessageReplyTextInset,
                       ConstSlice<MessageMarkdownSemantic> semantics = {}, float line_height = 18.0f) noexcept;
bool initialize_flow(MessageTextPresentation* presentation, const UiLayoutNode& region, float text_left,
                     float text_width, const UiRect& clip, UiTextFlowLayout* flow) noexcept;
bool finish_flow(MessageTextPresentation* presentation, const UiTextFlowLayout& flow,
                 float* height) noexcept;
float rich_item_total(const MessageSceneRichItemLayout& layout, u32 begin, u32 count) noexcept;
bool compact_content(MessageTextPresentation* presentation, BuildContext* context, Utf8View name,
                     Utf8View source, Utf8View* text, ConstSlice<TextStyleSpan>* styles,
                     ConstSlice<MessageMarkdownSemantic>* semantics,
                     const UiPluginRenderDecoration* decoration = nullptr, u64 avatar_user = 0,
                     u32 dot_rgb = 0) noexcept;
bool append_compact_timestamp(MessageTextPresentation* presentation, const MessageScene& scene,
                              const MessageSceneRowWidget& row, const UiRect& clip, const Theme& theme,
                              const MessageRecord& record, u64 now_unix_seconds) noexcept;
// The label of an edited message: small muted type after the text, one semantic carrying the edit time
// for its tooltip. A message without text shows it on a line of its own in the group after its accessories.
inline constexpr Utf8View kEditedLabel = utf8_literal("(edited)");
// Extends the retained text (copying it behind the banks' ends when it lives elsewhere). False without
// room, leaving the arguments as they were.
bool append_edited_label(MessageTextPresentation* presentation, u64 edited_unix_seconds, Utf8View* text,
                         ConstSlice<TextStyleSpan>* styles,
                         ConstSlice<MessageMarkdownSemantic>* semantics) noexcept;
bool append_edited_tail(MessageTextPresentation* presentation, const MessageScene& scene,
                        const MessageSceneRowWidget& row, const UiRect& row_clip, const Theme& theme,
                        const MessageRecord& record, u16 measurement_index) noexcept;
bool parse_content(MessageTextPresentation* presentation, BuildContext* context, const Utf8View& source,
                   Utf8View* text, ConstSlice<TextStyleSpan>* styles,
                   ConstSlice<MessageMarkdownSemantic>* semantics, u32 base_style = 0,
                   u8 base_size = text_size_inherit) noexcept;
float quote_text_width(float width) noexcept;
u32 split_quote_segments(const Utf8View& text, const ConstSlice<TextStyleSpan>& styles,
                         const ConstSlice<MessageMarkdownSemantic>& semantics, QuoteSegment* output) noexcept;
float quote_gap_after(const QuoteSegment* segments, u32 count, u32 index, float last) noexcept;
bool append_rich_markdown_line(BuildContext* context, const Utf8View& source, UiTextFlowLayout* flow,
                               float left_inset, float right_inset, const Color& color,
                               const UiWidgetReference& owner, u16 measurement_index,
                               MessageTextMeasurementKind measurement_kind, float gap_after = 3.0f,
                               u32 base_style = 0, u8 base_size = text_size_inherit) noexcept;
float widest_framed(const MessageTextPresentation* presentation, u32 first, float inset, float widest) noexcept;
bool append_embed_plain_line(BuildContext* context, const Utf8View& text, UiTextFlowLayout* flow,
                             float left_inset, float right_inset, const Color& color,
                             const UiWidgetReference& owner, u16 measurement_index, u32 flags,
                             const Utf8View& link, u16 link_source, float minimum_height = kMessageEmbedLineMinimum,
                             u16 source_id = 0, u8 size = text_size_inherit) noexcept;
bool append_embed_block(BuildContext* context, const Utf8View& source, UiTextFlowLayout* flow, float left_inset,
                        float right_inset, const Color& color, const UiWidgetReference& owner,
                        u16 measurement_index, bool markdown, u8 rules, u32 base_style, u8 base_size,
                        const Utf8View& link = kNoLink, u16 link_source = 0) noexcept;
bool append_embed_fields(BuildContext* context, const MessageRichStore& store, const MessageRichRecord& rich,
                         const MessageRichEmbed& embed, UiTextFlowLayout* flow, const Color& name_color,
                         const Color& value_color, const UiWidgetReference& owner, u16 measurement_index,
                         float* natural_width, bool markdown) noexcept;
Utf8View poll_result_field(const MessageRichStore& store, const MessageRichRecord& rich, const Utf8View& name) noexcept;
bool append_poll_result(BuildContext* context, const MessageRichStore& store, const MessageRichRecord& rich,
                        UiTextFlowLayout* flow, const Color& text, const Color& muted,
                        const UiWidgetReference& owner, u16 measurement_index) noexcept;
bool retain_system_text(MessageTextPresentation* presentation, const ChatStateStore& state, const UserStore* users,
                        const MessageRecord& record, const MessageRichStore* rich_store,
                        const MessageRichRecord* rich, Utf8View content, u64 now_unix_seconds, Utf8View* text,
                        ConstSlice<TextStyleSpan>* styles, ConstSlice<MessageMarkdownSemantic>* semantics) noexcept;
bool append_obscure_label(MessageTextPresentation* presentation, MessageMediaObscure obscure, const UiRect& media,
                          const UiWidgetReference& owner, const UiRect& clip, const Theme& theme, const MessageRecord& record,
                          u16 measurement) noexcept;
u32 failed_component_host(const MessageScene& scene, const MessageRecord& record) noexcept;
bool append_annotations(MessageTextPresentation* presentation, const MessageScene& scene,
                        const MessageSceneRowWidget& row, const MessageRecord& record, const Theme& theme,
                        const UiRect& row_clip, u16 measurement_index, bool forwarded, float offset,
                        const UiPluginRenderDecoration* decoration = nullptr) noexcept;
bool append_invite_cards(MessageTextPresentation* presentation, const MessageScene& scene,
                         const MessageSceneRowWidget& row, const MessageRecord& record, const ChatStateStore& state,
                         const Theme& theme, const UiRect& row_clip, u16 measurement_index,
                         u64 now_unix_seconds) noexcept;
bool append_plugin_collapsed(MessageTextPresentation* presentation, const UiPluginRenderDecoration& decoration,
                             Utf8View* text, ConstSlice<TextStyleSpan>* styles,
                             ConstSlice<MessageMarkdownSemantic>* semantics) noexcept;
bool append_intro(MessageTextPresentation* presentation, const MessageScene& scene, const Theme& theme) noexcept;
bool append_rich_media_text(BuildContext& context, const MessageScene& scene,
                            const MessageSceneRowWidget& row, const Theme& theme,
                            const UiRect& row_clip, u16 measurement_index, float text_width,
                            MessageMediaPolicyResult media_policy) noexcept;
__declspec(noinline) RetainedReplySummary retain_quoted_system(MessageTextPresentation* presentation, u64 message_id,
                                                              const Utf8View& author, const Utf8View& poll_question,
                                                              decltype(MessageSystemTextInput{}.type) type) noexcept;
__declspec(noinline) bool append_poll_text(MessageTextPresentation* presentation, const MessageScene& scene,
                                           const MessageSceneRowWidget& row, const Theme& theme,
                                           const UiRect& row_clip, const MessageRecord* record) noexcept;
__declspec(noinline) bool append_content_segments(
    MessageTextPresentation* presentation, const MessageScene& scene, const MessageSceneRowWidget& row,
    const Utf8View& display_content, const ConstSlice<TextStyleSpan>& styles, const ConstSlice<MessageMarkdownSemantic>& semantics,
    bool split, float content_top, float text_left, float text_width, const UiRect& row_clip,
    const Color& content_color, const MessageRecord* record, u16 measurement_index, u16 source_id,
    const Color& role_color, u64 edited_unix_seconds = 0) noexcept;
bool append_row(MessageTextPresentation* presentation, const MessageScene& scene,
                const MessageSceneRowWidget& row, const ChatStateStore& state,
                const UserStore* users, const NavigationView& navigation, const Theme& theme,
                u64 now_unix_seconds, bool forwarded = false) noexcept;
constexpr float quote_text_inset() noexcept {
    return kQuoteSpine + kQuoteStartPadding;
}

} // namespace maicord::text_presentation_detail
