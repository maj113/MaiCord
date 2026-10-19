#pragma once

#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/surface_geometry.hpp"

namespace maicord {
inline constexpr u32 kUiPollMaximumAnswers = 10;
inline constexpr u32 kUiPollQuestionMaximumBytes = 1200;
inline constexpr u32 kUiPollAnswerMaximumBytes = 220;
inline constexpr u32 kUiPollEmojiMaximumBytes = 128;

struct UiPollAnswer {
    u32 id{};
    Utf8View text{}, emoji_name{};
    u64 emoji_id{};
    u32 count{};
    bool emoji_animated{}, count_known{}, me_voted{}, me_known{}, selected{};
};
struct UiPollState {
    // Publication identity guards against stale input. Content-only identity keys wrapping
    // measurements, so count/selection/pending updates reuse the same layouts.
    u64 owner{}, generation{}, revision{};
    u32 content_revision{}, result_revision{};
    Utf8View question{};
    Utf8View status{};
    ConstSlice<UiPollAnswer> answers{};
    u64 now_ms{}, expiry_ms{}, total_votes{};
    bool expiry_known{}, finalized{}, finalized_known{}, multiselect{}, results{}, results_known{};
    bool total_votes_known{}, supported{}, loading{}, error{}, can_vote{}, can_remove_vote{};
    bool can_view_results{}, can_view_voters{}, can_retry{};
};
// Finalized, or past a known expiry.
bool ui_poll_closed(const UiPollState& state) noexcept;
// Results view (vote counts shown): voted, closed or "Show results".
bool ui_poll_counts_visible(const UiPollState& state) noexcept;
// Default poll layout (DIP): a 16-padded card at most 472 wide; 50-high answer
// rows (8/16 padding, 8 radius, 8 gap) with the selection icon on the right; a
// 38-high footer of vote details and actions. Theme metrics override the lengths.
struct UiPollStyle {
    float padding{16}, gap{8}, answer_padding{16}, answer_gap{8};
    float minimum_width{128}, maximum_width{472}, target{38}, marker{24}, emoji_extent{24};
    float question_minimum_height{20}, answer_minimum_height{50}, label_minimum_height{16};
    float radius{8};
    u32 question_style{text_style_bold};
    u32 answer_style{text_style_fold_size(text_style_bold, text_size_sm)};
    u32 detail_style{text_style_fold_size(text_style_none, text_size_sm)};
    u32 count_style{text_style_fold_size(text_style_bold, text_size_xs)};
    u32 percent_style{text_style_bold};
    u32 button_style{text_style_fold_size(text_style_bold, text_size_sm)};
    Utf8View question_fallback{utf8_literal("Poll")}, answer_fallback{utf8_literal("Answer")};
    Utf8View vote{utf8_literal("Vote")}, remove_vote{utf8_literal("Remove Vote")};
    Utf8View view_results{utf8_literal("Show results")}, back{utf8_literal("Go back to vote")};
    Utf8View view_voters{utf8_literal("View voters")}, retry{utf8_literal("Retry")};
    Utf8View single{utf8_literal("Select one answer")}, multi{utf8_literal("Select one or more answers")};
    Utf8View closed{utf8_literal("Poll closed")}, unavailable{utf8_literal("Poll unavailable")};
    Utf8View unknown_results{utf8_literal("Results unavailable")};
    Utf8View loading{utf8_literal("Updating poll\xE2\x80\xA6")},
        failed{utf8_literal("Seems like something's gone wrong. Sorry about that.")};
};
inline constexpr UiPollStyle kDefaultUiPollStyle{};
enum class UiPollControl : u8 {
    none,
    answer,
    vote,
    remove_vote,
    results,
    retry
};
struct UiPollTarget {
    UiPollControl control{};
    u32 answer_id{};
};
constexpr bool ui_poll_target_equal(UiPollTarget a, UiPollTarget b) noexcept {
    return a.control == b.control && a.answer_id == b.answer_id;
}
// marker: the right-hand radio/checkbox (or own-vote check); result: the
// "N votes" count and percent: "NN%" before it, both shown with results.
struct UiPollAnswerGeometry {
    UiRect bounds{}, marker{}, emoji{}, text{}, result{}, percent{};
};
struct UiPollGeometry {
    UiRect bounds{}, question{}, instruction{}, status{}, summary{}, expiry{};
    UiPollAnswerGeometry answers[kUiPollMaximumAnswers]{};
    UiRect vote{}, remove_vote{}, results{}, retry{};
    u8 footer_rows{};
    bool compact{};
};
struct UiPollProjection {
    UiPollState state{};
    UiPollGeometry geometry{};
    Color surface{}, foreground{}, secondary{}, accent{}, primary_foreground{}, border{}, selected{},
        error{};
    Color answer_fill{}, outline_hover{}, bar{}, bar_voted{}, bar_victor{}, victor{};
    char count_text[kUiPollMaximumAnswers][24]{};
    u8 count_bytes[kUiPollMaximumAnswers]{};
    char percent_text[kUiPollMaximumAnswers][8]{};
    u8 percent_bytes[kUiPollMaximumAnswers]{};
    u32 percentages[kUiPollMaximumAnswers]{};
    bool victor_answers[kUiPollMaximumAnswers]{};
    // "N votes ∙ 3d left" (summary) and its time part (expiry).
    char summary_text[64]{}, expiry_text[32]{};
    u8 summary_bytes{}, expiry_bytes{};
    bool vote_enabled{}, remove_enabled{}, results_enabled{}, retry_enabled{}, answers_enabled{};
    // The results control shows as the "Show results" text action, or as the
    // "Go back to vote" button in the primary slot.
    bool results_primary{};
};
struct UiPollLayout {
    UiPollGeometry geometry{};
    u64 content_signature{};
    u32 content_revision{};
    float width{}, height{};
    // Footer action label widths, for state-dependent placement.
    float vote_width{}, remove_width{}, back_width{}, show_width{}, retry_width{};
};
bool ui_poll_measure_layout(const UiPollState&, float width, const UiPollStyle&, UiTextMeasureCache*,
                            u16 backend_revision, UiPollLayout*) noexcept;
bool ui_poll_controls_project_layout(const UiPollState&, UiPoint origin, const UiPollLayout&,
                                     const UiPollStyle&, const Theme&, UiPollProjection*) noexcept;
// Width is full card width. The returned bounds may be taller than the supplied
// bounds: callers use its measured height in their normal scroll layout.
// Failure is atomic. Generated labels never keep pointers into this object.
bool ui_poll_controls_project(const UiPollState&, UiRect bounds, const UiPollStyle&, const Theme&,
                              UiTextMeasureCache*, u16 backend_revision, UiPollProjection*) noexcept;
Utf8View ui_poll_count_label(const UiPollProjection&, u32 answer_index) noexcept;
Utf8View ui_poll_summary_label(const UiPollProjection&) noexcept;
UiPollTarget ui_poll_controls_hit(const UiPollProjection&, UiPoint) noexcept;
UiRect ui_poll_target_rectangle(const UiPollProjection&, UiPollTarget) noexcept;
bool ui_poll_target_enabled(const UiPollProjection&, UiPollTarget) noexcept;
bool ui_poll_target_visible(const UiPollProjection&, UiPollTarget) noexcept;
enum class UiPollActionKind : u8 {
    none,
    toggle_answer,
    vote,
    remove_vote,
    results,
    voters,
    retry
};
struct UiPollAction {
    u64 owner{}, generation{}, revision{};
    UiPollActionKind kind{};
    u32 answer_id{};
};
bool ui_poll_controls_action(const UiPollProjection&, UiPollTarget, UiPollAction*) noexcept;
bool ui_poll_action_current(const UiPollState&, const UiPollAction&) noexcept;
enum class UiPollKey : u8 {
    activate,
    previous,
    next,
    first,
    last
};
enum class UiPollPointerEvent : u8 {
    down,
    move,
    up,
    leave,
    cancel
};
enum class UiPollSourceSurface : u8 {
    timeline,
    search,
    pins,
    forward
};
struct UiPollScene {
    UiPollProjection projection{};
    UiPollStyle style{};
    UiWidgetReference owner{};
    UiRect clip{};
    UiPollTarget focus{}, hover{}, capture{};
    UiPollSourceSurface surface{};
    bool keyboard_focus{}, active{};
};
struct UiPollSceneResult {
    UiPollAction action{};
    bool handled{}, request_capture{}, release_capture{}, action_requested{};
};
bool ui_poll_scene_project(UiPollScene*, const UiPollState&, UiRect bounds, UiRect clip, UiWidgetReference,
                           UiPollSourceSurface, const UiPollStyle&, const Theme&, UiTextMeasureCache*,
                           u16 backend_revision) noexcept;
UiPollTarget ui_poll_scene_hit(const UiPollScene&, UiPoint) noexcept;
void ui_poll_scene_blur(UiPollScene*) noexcept;
UiPollSceneResult ui_poll_scene_pointer(UiPollScene*, UiPollPointerEvent, UiPoint) noexcept;
UiPollSceneResult ui_poll_scene_key(UiPollScene*, UiPollKey) noexcept;

inline constexpr u32 kUiPollVoterPageMaximum = 100;
inline constexpr u32 kUiPollVisibleVotersMaximum = 25;
struct UiPollVoter {
    u64 id{};
    Utf8View name{};
    u32 avatar_variant{};
    bool avatar_animated{};
    // Borrowed with the page; requests the shared user-avatar identity.
    Utf8View avatar_hash{};
};
struct UiPollVotersState {
    u64 owner{}, generation{}, revision{};
    u32 content_revision{}, answer_id{}, page_index{};
    Utf8View question{}, status{};
    ConstSlice<UiPollAnswer> answers{};
    ConstSlice<UiPollVoter> voters{};
    bool loading{}, error{}, has_previous{}, has_next{}, can_retry{};
};
struct UiPollVotersStyle {
    float preferred_width{480}, preferred_height{640}, margin{16}, padding{16}, gap{8};
    float header_height{48}, target{32}, row_height{44}, avatar_extent{32};
    float radius{kUiControlCornerRadius};
    float backdrop_opacity{.6f};
    Utf8View title{utf8_literal("Voters")}, previous{utf8_literal("Previous")};
    Utf8View next{utf8_literal("Next")}, retry{utf8_literal("Retry")}, close{utf8_literal("Close")};
    Utf8View loading{utf8_literal("Loading voters…")}, failed{utf8_literal("Couldn't load voters")};
    Utf8View empty{utf8_literal("No voters to show")};
};
inline constexpr UiPollVotersStyle kDefaultUiPollVotersStyle{};
enum class UiPollVotersControl : u8 {
    none,
    close,
    answer,
    user,
    previous,
    next,
    retry
};
struct UiPollVotersTarget {
    UiPollVotersControl control{};
    u64 id{};
};
constexpr bool ui_poll_voters_target_equal(UiPollVotersTarget a, UiPollVotersTarget b) noexcept {
    return a.control == b.control && a.id == b.id;
}
// Wide panels list the answers in a left column (text and vote count) beside
// the selected answer's voters; narrow panels keep numbered answer chips.
struct UiPollVotersGeometry {
    UiRect viewport{}, panel{}, title{}, close{}, question{}, answer{}, status{}, body{};
    UiRect answers[kUiPollMaximumAnswers]{}, previous{}, next{}, retry{}, page{};
    bool columns{};
};
struct UiPollVotersScene {
    UiPollVotersState state{};
    UiPollVotersStyle style{};
    UiPollVotersGeometry geometry{};
    UiWidgetReference owner{};
    UiPollVotersTarget focus{}, hover{}, capture{};
    Color surface{}, foreground{}, secondary{}, accent{}, accent_foreground{}, error{};
    char page_text[24]{};
    u8 page_bytes{};
    float scroll{};
    u32 first{}, visible{};
    bool keyboard_focus{}, active{};
};
struct UiPollVotersAction {
    u64 owner{}, generation{}, revision{};
    UiPollVotersControl control{};
    u64 id{};
};
struct UiPollVotersSceneResult {
    UiPollVotersAction action{};
    bool handled{}, request_capture{}, release_capture{}, action_requested{};
};
bool ui_poll_voters_scene_project(UiPollVotersScene*, const UiPollVotersState&, UiRect viewport,
                                  UiWidgetReference, const UiPollVotersStyle&, const Theme&,
                                  UiTextMeasureCache*, u16 backend_revision,
                                  float requested_scroll = 0) noexcept;
bool ui_poll_voters_scene_scroll(UiPollVotersScene*, float delta) noexcept;
UiRect ui_poll_voters_row_rectangle(const UiPollVotersScene&, u32 index) noexcept;
// Avatar of a visible voter row (scene.first <= index < first + visible) and the
// body clip; false when culled.
bool ui_poll_voters_avatar(const UiPollVotersScene&, u32 index, UiRect* rectangle, UiRect* clip) noexcept;
UiRect ui_poll_voters_target_rectangle(const UiPollVotersScene&, UiPollVotersTarget) noexcept;
bool ui_poll_voters_target_enabled(const UiPollVotersScene&, UiPollVotersTarget) noexcept;
UiPollVotersTarget ui_poll_voters_scene_hit(const UiPollVotersScene&, UiPoint) noexcept;
bool ui_poll_voters_scene_focus(UiPollVotersScene*, UiPollVotersTarget, bool keyboard) noexcept;
UiPollVotersSceneResult ui_poll_voters_scene_pointer(UiPollVotersScene*, UiPollPointerEvent,
                                                     UiPoint) noexcept;
UiPollVotersSceneResult ui_poll_voters_scene_key(UiPollVotersScene*, UiPollKey) noexcept;
} // namespace maicord
