#pragma once

#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/core/visit_ref.hpp"
#include "maicord/ui/widgets/kit.hpp"
#include "maicord/ui/widgets/kit_markdown.hpp"

namespace maicord::settings_scene_detail {

enum class EntryRole : u8 {
    normal,
    nav,
    row,
    footer_status
};
struct Entry {
    UiSettingsTarget target;
    u32 source_index;
    EntryRole role;
};

// Customization geometry (profile editor): tab bar items (20 DIP
// labels over 16 DIP padding and a hairline, then a 16 DIP margin), section
// dividers 24 DIP below their section, captioned 69x50 swatches and the
// aside column (300 DIP beside the form with a 36 DIP gap).
constexpr float kTabBarHeight = 37, kTabBarMargin = 16, kTabGap = 32;
constexpr float kSectionGap = 24;
constexpr float kColorSwatchWidth = 69, kColorSwatchHeight = 50, kColorSwatchGap = 16, kColorCaption = 20;
// kAsideFormMaximum: a form beside its aside stops this wide (the official Server Profile form), the aside following
// it at kAsideGap; only a wide page has room past it.
constexpr float kAsideGap = 36, kAsideFormMinimum = 280, kAsideFormMaximum = 525, kAsideHeadingGap = 8;
// The scene's layout tokens, computed in place (a by-value return would be copied).
struct SceneTokens : UiSettingsLayoutTokens {
    explicit SceneTokens(const UiSettingsScene& scene) noexcept {
        ui_settings_fill_layout_tokens(scene.theme_styles, this);
    }
};
// How a row meets its neighbours: grouped rows share one card, solo rows (a status card)
// are a card of their own, and the rest (headings, notices, footnotes, list rows) draw their own plate or none.
enum CardEdge : u8 {
    card_none = 0,
    card_member = 1,
    card_first = 2,
    card_last = 4,
};
// What a layout needs besides the content: text measuring (null estimates), the retained markdown blocks and
// whether field rows group into cards (the modal's pages).
struct RowContext {
    const UiKitMeasure* measure;
    UiMarkdownBank** markdown;
    bool cards;
};
// One row's geometry, shared by measurement, paint, text and hit-testing. Rectangles a kind does not use
// stay empty: control is the one pointer target (a button, switch, field, the markdown link), value the
// slider's value column or a status caption, tag the chip of a disclosure or the note after a category title,
// icon a status tile's glyph, meter a status card's bar and body a markdown block.
struct RowLayout {
    UiRect row, label, description, control, lead, chips, value, tag, icon, meter, body;
    float height;
};
// Parts of a list row or card, from its row rectangle.
struct ListParts {
    UiRect content, icon, lock, column, chevron;
    UiRect actions[kUiSettingsListActions];
};
// Sidebar rows, bound navigation or a header plus every registered page; pinned rows stand in the foot.
struct NavRow {
    UiSettingsNavKind kind;
    bool destructive, pinned;
    UiIcon icon;
    Utf8View label;
    UiSettingsTarget target;
    u16 badge;
    UiPageKey subpage{};
};
// Whether a sidebar row shows as the selected one: its page, or its subpage, is shown.
bool nav_selected(const UiSettingsScene& scene, const NavRow& row) noexcept;
// A line of the sidebar note (12/16).
inline constexpr float kNavNoteLine = 16;
struct Frame {
    // nav_note: the sidebar's note lines, the last of its foot.
    UiRect nav, nav_foot, nav_rule, nav_note, body, title, description, footer, rule;
};
// The host aside: beside rows from aside_row on when the column fits both,
// else a block above that row. block includes the heading line.
struct AsideLayout {
    bool active, beside;
    float width, form, block;
};

Entry* entries(UiSettingsScene& scene) noexcept;
const Entry* entries(const UiSettingsScene& scene) noexcept;
bool editor_kind(UiSettingsControlKind kind) noexcept;
float section_trailing(const UiSettingsPageContent& content, u32 index) noexcept;
const UiSettingsControlPresentation* presentation(const UiSettingsPageContent& content, u32 index) noexcept;
bool marks(const UiSettingsPageContent& content, u32 index) noexcept;
bool link(const UiSettingsPageContent& content, u32 index) noexcept;
bool has_flag(const UiSettingsPageContent& content, u32 index, u8 flag) noexcept;
bool accent(const UiSettingsPageContent& content, u32 index) noexcept;
bool avatar_backed(const UiSettingsPageContent& content, u32 index) noexcept;
UiRect avatar_rect(const UiSettingsMetrics& m, const UiSettingsPageContent& content, u32 index, const UiRect& lead) noexcept;
const UiSettingsListItem* list_item(const UiSettingsPageContent& content, u32 index) noexcept;
bool list_style(const UiSettingsPageContent& content, u32 index, UiSettingsListStyle style) noexcept;
bool progress(const UiSettingsPageContent& content, u32 index) noexcept;
bool progress_fill(const UiSettingsPageContent& content, u32 index) noexcept;
float button_label_width(const UiKitMeasure* measure, const Utf8View& label) noexcept;
// A status card's action leads with a 16 DIP refresh glyph unless it is running (spinner) or fills as a progress
// button. With the glyph the pair is centred in the button; the label then starts after the glyph.
inline constexpr float kStatusGlyphSize = 16, kStatusGlyphGap = 8;
bool status_glyph(const UiSettingsPageContent& content, u32 index) noexcept;
UiRect status_button_label(const UiKitMeasure* measure, const Utf8View& label, const UiRect& button, UiRect* glyph) noexcept;
// A status meta line of " · "-separated parts, flowed in `box`: each part is one unbreakable label and a line breaks
// only between parts. A part that is not first on its line leads with its separator ("· Stable channel"); the
// separator at a break is dropped, so no line ends with a dot or starts with one. `out` (kStatusMetaParts entries,
// optional) receives each label and its rectangle; the result is the number of lines.
inline constexpr u32 kStatusMetaParts = 8;
struct StatusMetaPart {
    Utf8View text;
    UiRect rect;
    bool lead;
};
u32 status_meta_flow(const UiKitMeasure* measure, const Utf8View& meta, const UiRect& box, StatusMetaPart* out,
                     u32* count) noexcept;
ListParts list_parts(const UiSettingsLayoutTokens& t, const UiSettingsPageContent& content, u32 index,
                     const UiRect& row) noexcept;
// True when the row's presentation `more` byte has `flag` (UiSettingsPresentationMore).
bool more(const UiSettingsPageContent& content, u32 index, u8 flag) noexcept;
// Grid cells (ui_settings_more_cell): the run of consecutive cells a row belongs to, the line of that run it is on
// (rows [line_first, line_end)), its column and the run's column count; cell is false for any other row.
struct CellPlace {
    bool cell;
    u32 run_first, run_end, line_first, line_end, column, columns;
};
CellPlace cell_place(const UiSettingsPageContent& content, u32 index) noexcept;
// The width of one cell of a `columns`-wide line `width` wide; cells and lines are lead_gap apart.
float cell_width(const UiSettingsLayoutTokens& t, float width, u32 columns) noexcept;
// Banner tiles (ui_settings_more_banner_tiles): five a line, 3:5 tall, lead_gap apart.
inline constexpr u32 kBannerTileColumns = 5;
UiRect banner_tile_rect(const UiSettingsLayoutTokens& t, const UiRect& field, u32 index) noexcept;
float banner_tiles_height(const UiSettingsLayoutTokens& t, u32 count, float width) noexcept;
u32 banner_tile_hit(const UiSettingsLayoutTokens& t, const UiRect& field, u32 count, float x, float y) noexcept;
// Glyph tiles (ui_settings_more_glyphs): kGlyphTile squares lead_gap apart, as many to a line as fit, the icon
// kGlyphTileIcon wide in the middle.
inline constexpr float kGlyphTile = 40, kGlyphTileIcon = 28;
UiRect glyph_tile_rect(const UiSettingsLayoutTokens& t, const UiRect& field, u32 index) noexcept;
float glyph_tiles_height(const UiSettingsLayoutTokens& t, u32 count, float width) noexcept;
u32 glyph_tile_hit(const UiSettingsLayoutTokens& t, const UiRect& field, u32 count, float x, float y) noexcept;
// The glyph tile a row names at default_choice + offset, or null.
const UiSettingsGlyphTile* glyph_tile(const UiSettingsPageContent& content, u32 index, u32 offset) noexcept;
// An edit cell's leading (emoji) button and trailing clear button inside its field.
UiRect cell_lead_button(const UiSettingsLayoutTokens& t, const UiRect& field) noexcept;
UiRect cell_clear_button(const UiSettingsLayoutTokens& t, const UiRect& field) noexcept;
// The layout context of a scene: its measuring hooks and markdown bank, and whether it groups rows into cards.
RowContext row_context(const UiSettingsScene& scene) noexcept;
// CardEdge bits of a row: member of a card, and the first and last row of its card.
u8 card_edges(const UiSettingsPageContent& content, u32 index, const RowContext& context) noexcept;
RowLayout row_layout(const UiSettingsLayoutTokens& t, const UiSettingsStyle& style, const UiSettingsPageContent& content,
                     u32 index, float left, float top, float width, const RowContext& context) noexcept;
// The markdown block of a row, parsed from its source (null without memory or when the source is too long).
UiMarkdown* markdown_block(const UiSettingsPageContent& content, u32 index, const RowContext& context) noexcept;
// The status button of a row's presentation: kinds and tones of its chips.
u8 chip_tone(const UiSettingsPageContent& content, u32 index, u32 chip) noexcept;
// Chips of a status row flowed left to right inside `width`: each chip's rectangle in `out`, the flow's height.
void flow_chips(const UiSettingsControl& c, const UiKitMeasure* measure, float left, float top, float width, UiRect* out,
                float* height) noexcept;
bool nav_row(const UiSettingsScene& scene, u32 index, NavRow* row) noexcept;
float nav_extent(const UiSettingsScene& scene, const UiSettingsMetrics& m) noexcept;
float dialog_footer_height(const UiSettingsMetrics& m) noexcept;
Frame frame(const UiSettingsScene& scene, const UiSettingsMetrics& m) noexcept;
float body_width(const UiSettingsScene& scene) noexcept;
AsideLayout aside_layout(const UiSettingsScene& scene) noexcept;
float row_start(const UiSettingsScene& scene, u32 row) noexcept;
float content_extent(const UiSettingsScene& scene, const UiSettingsMetrics& m) noexcept;
float clamp_offset(const UiSettingsScene& scene, bool sidebar, float offset, const UiSettingsMetrics& m) noexcept;
bool geometry_valid(const UiEditGeometry* geometry) noexcept;
bool current(const UiSettingsScene& scene) noexcept;
bool valid_text(const Utf8View& text) noexcept;
bool valid_content(const UiSettingsPageContent& content) noexcept;
bool valid_navigation(ConstSlice<UiSettingsNavEntry> navigation) noexcept;
const UiSettingsControl* find_control(const UiSettingsScene& scene, const UiSettingsTarget& target,
                                      u32* index = nullptr) noexcept;
bool target_exists(const UiSettingsScene& scene, const UiSettingsTarget& target) noexcept;
UiWidgetHandle target_handle(const UiSettingsScene& scene, const UiSettingsTarget& target) noexcept;
bool build(UiSettingsScene* scene) noexcept;
bool row_layout_for(const UiSettingsScene& scene, const UiSettingsLayoutTokens& t, UiWidgetHandle handle, u32 source,
                    RowLayout* layout, UiRect* clip = nullptr) noexcept;
u64 layout_signature(const UiSettingsScene& scene, const UiSettingsPageContent& content, const UiSettingsStyle& style,
                     UiRect viewport, UiPageKey page, u32 registry_revision) noexcept;
void synchronize_input(UiSettingsScene& scene) noexcept;
// Keeps the focus valid after the page or its rows changed: a control that stopped being focusable (a button
// replaced by a status row) keeps the focus parked on its slot, a removed one hands it to the nearest focusable
// row, and anything else falls back to the selected page, else the search field or the close button.
void settle_focus(UiSettingsScene& scene) noexcept;
// True while the focus rests on a control that still exists but is not focusable; its row index goes to `row`.
bool focus_parked(const UiSettingsScene& scene, u32* row) noexcept;
// True when `target` comes after the parked control `row` in the panel's tab order (a later row, the footer
// buttons, the close button).
bool after_parked_focus(const UiSettingsScene& scene, const UiSettingsTarget& target, u32 row) noexcept;
UiControlState hover_option(const UiSettingsScene& scene, UiControlState state, u32 index) noexcept;
UiControlState widget_state(const UiSettingsScene& scene, UiWidgetHandle handle, const UiSettingsTarget& target,
                            bool enabled) noexcept;
// A widget's identity in the scene's hover fade (0 for none).
constexpr u64 hover_motion_id(UiWidgetHandle handle) noexcept {
    return handle.generation ? static_cast<u64>(handle.generation) << 16 | handle.slot : 0;
}
float toggle_motion(const UiSettingsScene& scene, const UiSettingsTarget& target, const UiSettingsLayoutTokens& t,
                    bool checked) noexcept;
// The plate slide of a segments row while its selection moves (at rest otherwise), and how far option `option`
// shows as the selected one (0..1): the labels cross-fade with the plate.
UiSegmentedMotion segment_motion(const UiSettingsScene& scene, const UiSettingsControl& control) noexcept;
float segment_selection(const UiSegmentedMotion& motion, u32 selected, u32 option) noexcept;
void reveal_range(UiSettingsScene* scene, bool sidebar, float first, float last, const UiSettingsMetrics& m) noexcept;
float nav_offset_of(const UiSettingsScene& scene, const UiSettingsMetrics& m, const UiSettingsTarget& target,
                    float* height) noexcept;
bool apply_scroll(UiSettingsScene* scene, bool sidebar, float previous) noexcept;
bool visit_targets(const UiSettingsScene& scene, UiSettingsFocusZone zone,
                   UiVisitRef<UiSettingsTarget> visit) noexcept;
UiSettingsFocusZone zone_of(const UiSettingsTarget& target) noexcept;
UiRect option_rect(const UiSettingsLayoutTokens& t, const UiSettingsControl& c, const UiRect& field, u32 index,
                   const UiKitMeasure* measure) noexcept;

} // namespace maicord::settings_scene_detail
