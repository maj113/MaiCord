#pragma once

#include "maicord/ui/page_registry.hpp"
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/edit_field.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {

enum class UiSettingsControlKind : u8 {
    toggle,
    choice,
    action,
    edit,
    multiline,
    slider,
    palette,
    radio,
    category,
    // A notice row; `checked` marks it as loading (a spinner at its end).
    notice,
    // Joined option buttons at the row end; selected_choice is the pressed one.
    segments,
    // Compact grid of colour squares; colours come from the presentation.
    swatches,
    // A tab bar across the page: choices are the tabs, selected_choice the
    // active one; it activates like segments.
    tabs,
    // A titled group of buttons under the label (choices; the first is
    // primary). The activation's selected_choice is the pressed button.
    buttons,
    // Large captioned colour swatches (choices are the captions; colours
    // through the palette source). An action_label adds a trailing
    // secondary button, reported as choice choices.count.
    colors,
    // One line of critical text under the row before it (a field error).
    error
};
// Selection sentinel for palette and radio controls showing no current value.
inline constexpr u32 kUiSettingsNoChoice = ~0u;

// Projection only. The host owns text, selection, IME, editor scrolling and
// geometry lifetime; publish changes through the page content revision.
struct UiSettingsEditor {
    UiEditView view;
    const UiEditGeometry* geometry;
    Utf8View placeholder;
};

struct UiSettingsStyle {
    float form_width, field_padding, group_gap, single_height, multiline_height;
    float field_radius, control_radius, footer_height;
};
inline constexpr UiSettingsStyle kUiDefaultSettingsStyle{
    680, 12, 24, 40, 160, kUiEditCornerRadius, kUiControlCornerRadius, 64};
bool ui_settings_style_valid(const UiSettingsStyle&) noexcept;

struct UiSettingsFooter {
    Utf8View status, save_label, reset_label;
    bool visible, can_save, saving, failed;
};
inline constexpr u32 kUiSettingsMaterializedWidgetLimit = 512;
// The scrolling body runs from the form column to this inset from the
// panel's right edge, so its scrollbar sits in a lane right of the column.
inline constexpr float kUiSettingsBodyEdgeInset = 4;
// The column-wide footer bar pads its status text by the leading inset and
// its buttons by the inset from the top, trailing and bottom edges.
inline constexpr float kUiSettingsFooterLeadingInset = 16;
inline constexpr float kUiSettingsFooterInset = 12;

// Immutable projection between update calls. IDs are stable within their page;
// labels, choices and controls are borrowed until update/release. The host owns
// values, persistence and action dispatch, including plugin module callbacks.
// Category rows head the controls that follow; notice rows show status text
// with an optional action button.
struct UiSettingsControl {
    u64 id;
    Utf8View label, description;
    UiSettingsControlKind kind;
    bool enabled, checked;
    ConstSlice<Utf8View> choices;
    u32 selected_choice;
    Utf8View action_label;
    const UiSettingsEditor* editor;
};

enum class UiSettingsVariant : u8 {
    standard,
    primary,
    critical,
    positive
};
enum UiSettingsPresentationFlag : u8 {
    ui_settings_presentation_none = 0,
    // Sliders show every choice label as a mark above the bar.
    ui_settings_presentation_marks = 1u << 0,
    // Category rows activate as navigation links (search result groups).
    ui_settings_presentation_link = 1u << 1,
    // Action rows: a drag handle leads the row.
    ui_settings_presentation_grip = 1u << 2,
    // Segments: deny / neutral / allow glyphs instead of choice labels.
    ui_settings_presentation_tristate = 1u << 3,
    // Action rows: a circular avatar with the label's initials leads the row.
    ui_settings_presentation_avatar = 1u << 4,
    // The row is drawn as the current selection of a list.
    ui_settings_presentation_selected = 1u << 5,
    // List rows use half the vertical row padding.
    ui_settings_presentation_compact = 1u << 6,
    // Action rows: a leading colour dot (or the avatar fill) resolved through
    // the palette source with kUiSettingsAccentChoice.
    ui_settings_presentation_accent = 1u << 7,
};
// Swatches colours, chip dots and row accents resolve through the bound
// palette source: colors[0] holds 0xRRGGBB; false or zero alpha draws none.
// Chips (action-row choices) ask for their choice index; a "+" label has no dot.
inline constexpr u32 kUiSettingsAccentChoice = 0xFFFF'FFFEu;
// Optional per-control presentation parallel to UiSettingsPageContent::controls.
// default_choice marks a slider's default value; kUiSettingsNoDefault for none.
inline constexpr u16 kUiSettingsNoDefault = 0xffff;
inline constexpr u16 kUiSettingsNoMeter = 0xffff;
// Action rows' choices become chips under the text.
struct UiSettingsControlPresentation {
    UiSettingsVariant variant;
    u8 flags;
    u16 default_choice;
};

// Real image for an avatar row. An invalid key keeps the initials placeholder.
// The hash (CDN asset hash, e.g. "a_" + 32 hex) is copied so the host's
// user cache may change while the page borrows this record.
inline constexpr u32 kUiSettingsAvatarHashBytes = 34;
struct UiSettingsAvatarMedia {
    MediaAssetKey key;
    u8 hash_bytes;
    char hash[kUiSettingsAvatarHashBytes];
};

inline constexpr u32 kUiSettingsPageFormat = 0x53544731u;
// UiPage.context must point to this type for registries passed to this scene.
// Bump revision after replacing descriptors, text, choices or control values.
// Unregister the page/owner before freeing this object or any borrowed data.
struct UiSettingsPageContent {
    Utf8View title, description;
    ConstSlice<UiSettingsControl> controls;
    u32 revision;
    u32 format = kUiSettingsPageFormat;
    // Style is validated and copied as a whole. Invalid styles use the safe
    // shared default. Absence retains existing settings page geometry.
    const UiSettingsStyle* style = nullptr;
    const UiSettingsFooter* footer = nullptr;
    // Optional borrowed heading for the navigation column. Empty keeps the
    // original user-settings heading; publish replacements through revision.
    Utf8View navigation_label{};
    // Null, or one entry per control.
    const UiSettingsControlPresentation* presentation = nullptr;
    // Null, or one live level per control in slider steps: a slider with a
    // level other than kUiSettingsNoMeter shows it as a meter behind its thumb
    // (lit strongly past the thumb). Read at paint; request a frame on change.
    const u16* meters = nullptr;
    // Null, or one entry per control: avatar rows whose key (user avatar or
    // server icon) is valid draw the resident image over their initials. Bump
    // revision when an entry changes.
    const UiSettingsAvatarMedia* avatar_media = nullptr;
    // Customization sections (the profile editor): rows are
    // sections with no card, each closed by a divider 24 DIP below it; error
    // rows stay with the section above. A multiline row's action_label is a
    // counter inside its field (critical variant: over the limit).
    bool sectioned = false;
    // A host-drawn column (the profile preview card) beside the rows from
    // aside_row on. When the column is too narrow for both, the aside sits
    // above that row instead. aside_height excludes the heading line.
    float aside_width = 0, aside_height = 0;
    u32 aside_row = 0;
    Utf8View aside_label{};
};

enum class UiSettingsTargetKind : u8 {
    none,
    page,
    control,
    close,
    save,
    reset,
    search,
    search_clear,
    action
};
struct UiSettingsTarget {
    UiSettingsTargetKind kind;
    UiPageKey page;
    u64 control_id;
};
constexpr bool ui_settings_target_equal(UiSettingsTarget a, UiSettingsTarget b) noexcept {
    return a.kind == b.kind && ui_page_key_equal(a.page, b.page) && a.control_id == b.control_id;
}

struct UiSettingsActivation {
    UiSettingsTarget target;
    UiSettingsControlKind kind;
    bool checked;
    u32 selected_choice;
};

struct UiSettingsControlGeometry {
    UiSettingsTarget target;
    UiWidgetReference owner;
    UiRect control, field, text, clip;
    u32 scene_revision, content_revision;
    bool enabled, multiline;
};
struct UiSettingsSliderGeometry {
    UiSettingsControlGeometry control;
    UiRect track, thumb;
};
enum class UiSettingsSliderKey : u8 {
    decrease,
    increase,
    home,
    end
};
struct UiSettingsPaletteSource {
    void* context;
    bool (*resolve)(void*, u64 control_id, u32 choice, BackgroundTheme*) noexcept;
    u64 revision;
};
static_assert(sizeof(UiSettingsPaletteSource) == 24);

// Modal presents the sidebar and panel inside a rounded dialog over a scrim;
// layer fills the viewport (channel settings). Dialog is a single-page form
// (rules, reports, events, custom status): a compact centred dialog without
// sidebar or search, sized to its content, with a footer band for its actions.
enum class UiSettingsPresentation : u8 {
    layer,
    modal,
    dialog
};
inline constexpr float kUiSettingsDialogWidth = 600;
inline constexpr float kUiSettingsDialogPadding = 24;
// Space below the last row of a dialog's body before its footer band.
inline constexpr float kUiSettingsDialogBodyEnd = 8;
// Sidebar data. Page entries reference registered pages; action entries
// report UiSettingsTargetKind::action with their id; a contributed entry
// expands to registered pages of owners no page entry names (plugin pages).
// Labels are borrowed.
enum class UiSettingsNavKind : u8 {
    header,
    page,
    separator,
    action,
    contributed
};
struct UiSettingsNavEntry {
    UiSettingsNavKind kind;
    bool destructive;
    UiIcon icon;
    Utf8View label;
    UiPageKey page;
    u64 action;
};
inline constexpr u32 kUiSettingsNavigationMaximum = 48;

// Narrowest layer sidebar (channel and server settings at the minimum window).
inline constexpr float kUiSettingsLayerMinimumSidebar = 176;
struct UiSettingsModalLayout {
    UiRect modal, sidebar, panel, column, close;
    bool compact;
};
struct UiSettingsTokens;
UiSettingsModalLayout ui_settings_modal_layout(const UiSettingsTokens&, UiRect viewport,
                                               UiSettingsPresentation) noexcept;
// Dialogs: shrinks a non-compact layout to the content height (title through
// footer band) and re-centres it vertically; taller content keeps the full
// height and scrolls its body.
void ui_settings_dialog_fit(UiSettingsModalLayout*, UiRect viewport, float content_height) noexcept;

// Toggle animation of one control on the selected page.
struct UiSettingsAnimation {
    u64 control_id, started_ms;
    bool checked;
};
inline constexpr u32 kUiSettingsAnimationSlots = 1;

struct UiResolvedThemeStyles;
struct UiSettingsScene {
    const UiResolvedThemeStyles* theme_styles{};
    u32 theme_geometry_revision{}, projected_theme_geometry_revision{};
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    UiPageRegistry* registry;
    UiSettingsPageContent content;
    UiPageKey selected_page;
    UiSettingsTarget focus;
    UiWidgetHandle sidebar, body, title, description, close;
    UiWidgetHandle footer_status, footer_save, footer_reset;
    UiSettingsStyle style;
    UiSettingsFooter footer;
    u64 layout_signature, page_generation;
    UiRect viewport;
    float sidebar_offset, content_offset;
    u32 capacity, prefix_capacity, registry_revision, content_revision, revision;
    bool active, focus_visible;
    UiSettingsPaletteSource palette{};
    u32 palette_hovered_index = ~u32{0};
    UiSettingsPresentation presentation{};
    bool reduced_motion{};
    // Unsaved-changes bar motion in padding: shake offset in 1/16 DIP and
    // alert tint in 1/255; the drop marker sits below its row when set.
    i16 footer_shift_q{};
    u8 footer_alert_q{};
    bool drop_after{};
    UiWidgetHandle search_field{};
    ConstSlice<UiSettingsNavEntry> navigation{};
    u32 navigation_revision{}, projected_navigation_revision{};
    UiSettingsEditor search{};
    UiSettingsModalLayout layout{};
    u64 flash_control{}, clock_ms{};
    float flash{};
    UiSettingsAnimation animations[kUiSettingsAnimationSlots]{};
    // Counts activations of an inactive scene; retained across release.
    u64 opens{};
    // Paint-only changes (toggle motion, flash) that need no text or layout.
    u32 motion_revision{};
    // Drag feedback by control index + 1 (zero is none): the dragged row is
    // tinted and a line marks the drop edge.
    u16 drag_row{}, drop_row{};
    // The shared loading spinner's phase (host frame clock; paint only).
    u8 spinner_phase{};
};
// True while the page shows a loading notice, whose spinner needs the clock.
bool ui_settings_scene_spinner_visible(const UiSettingsScene&) noexcept;

// Lazy one-allocation retained scene. Only viewport rows and two spacers per
// list are materialized; inactive scenes have no widget allocation. UI thread.
bool ui_settings_scene_bind_theme_styles(UiSettingsScene*, const UiResolvedThemeStyles*) noexcept;
bool ui_settings_scene_update(UiSettingsScene* scene, UiPageRegistry* registry, UiRect viewport) noexcept;
// The page check ui_settings_scene_update applies; owners use it to tell a
// malformed projection apart from a transient (viewport) rejection.
bool ui_settings_page_content_valid(const UiSettingsPageContent& content) noexcept;
void ui_settings_scene_close(UiSettingsScene* scene) noexcept;
void ui_settings_scene_release(UiSettingsScene* scene) noexcept;
UiSettingsTarget ui_settings_scene_hit_test(const UiSettingsScene& scene, float x, float y) noexcept;
UiSettingsTarget ui_settings_scene_focused(const UiSettingsScene& scene) noexcept;
bool ui_settings_scene_focus(UiSettingsScene* scene, UiSettingsTarget target) noexcept;
bool ui_settings_scene_move_focus(UiSettingsScene* scene, bool reverse) noexcept;
// Modal keyboard zones: search, sidebar, panel and close. Tab moves between
// zones; arrows move within the sidebar or between panel rows.
enum class UiSettingsFocusZone : u8 {
    none,
    search,
    sidebar,
    panel,
    close
};
UiSettingsFocusZone ui_settings_scene_focus_zone(const UiSettingsScene&) noexcept;
bool ui_settings_scene_move_zone(UiSettingsScene* scene, bool reverse) noexcept;
bool ui_settings_scene_move_within(UiSettingsScene* scene, bool reverse) noexcept;
void ui_settings_scene_set_focus_visible(UiSettingsScene* scene, bool visible) noexcept;
bool ui_settings_scene_select(UiSettingsScene* scene, UiPageKey page) noexcept;
bool ui_settings_scene_scroll(UiSettingsScene* scene, bool sidebar, float delta) noexcept;
UiScrollbarGeometry ui_settings_scene_scrollbar(const UiSettingsScene& scene, bool sidebar) noexcept;
// Builds a semantic request; never mutates settings or invokes module code.
// Page selection and close are also returned to the host as target kinds.
bool ui_settings_scene_activation(const UiSettingsScene& scene, UiSettingsTarget target, bool reverse,
                                  UiSettingsActivation* activation) noexcept;
// Borrowed descriptor and value anchor for host-owned native controls. Reject
// stale scenes; descriptor remains immutable until its content revision changes.
const UiSettingsControl* ui_settings_scene_control(const UiSettingsScene&, UiSettingsTarget,
                                                   u32* index = nullptr) noexcept;
bool ui_settings_scene_control_anchor(const UiSettingsScene&, UiSettingsTarget, UiRect*) noexcept;
// Full field/text coordinates plus exact visible clip. A field clipped away
// by the scrolling body or sticky footer has no geometry/input target.
bool ui_settings_scene_control_geometry(const UiSettingsScene&, UiSettingsTarget,
                                        UiSettingsControlGeometry*) noexcept;
// Only inline edit/multiline fields participate, never their labels or footer.
bool ui_settings_scene_editor_hit(const UiSettingsScene&, float x, float y,
                                  UiSettingsControlGeometry*) noexcept;
bool ui_settings_scene_bind_palette(UiSettingsScene*, UiSettingsPaletteSource) noexcept;
bool ui_settings_scene_slider_geometry(const UiSettingsScene&, UiSettingsTarget,
                                       UiSettingsSliderGeometry*) noexcept;
bool ui_settings_scene_slider_hit(const UiSettingsScene&, UiSettingsTarget, float x, float y) noexcept;
bool ui_settings_scene_slider_pointer(const UiSettingsScene&, UiSettingsTarget, float x,
                                      UiSettingsActivation*) noexcept;
bool ui_settings_scene_slider_key(const UiSettingsScene&, UiSettingsTarget, UiSettingsSliderKey,
                                  UiSettingsActivation*) noexcept;
bool ui_settings_scene_palette_pointer(const UiSettingsScene&, UiSettingsTarget, float x, float y,
                                       UiSettingsActivation*) noexcept;
bool ui_settings_scene_palette_hover(UiSettingsScene*, UiSettingsTarget, float x, float y) noexcept;
// Radio rows resolve the option under the pointer.
bool ui_settings_scene_radio_pointer(const UiSettingsScene&, UiSettingsTarget, float x, float y,
                                     UiSettingsActivation*) noexcept;
// Notice and action rows activate only on their button.
bool ui_settings_scene_button_hit(const UiSettingsScene&, UiSettingsTarget, float x, float y) noexcept;
// Segments and swatches rows resolve the option under the pointer.
bool ui_settings_scene_option_pointer(const UiSettingsScene&, UiSettingsTarget, float x, float y,
                                      UiSettingsActivation*) noexcept;
// Full rectangle of one option of a radio or segments row (what the pointer
// helpers above hit-test); false for other kinds or an unmaterialized row.
bool ui_settings_scene_choice_rectangle(const UiSettingsScene&, UiSettingsTarget, u32 choice, UiRect*) noexcept;
// The leading drag handle of a grip row.
bool ui_settings_scene_grip_hit(const UiSettingsScene&, UiSettingsTarget, float x, float y) noexcept;
// Materialized row nearest to y and whether y falls in its lower half.
bool ui_settings_scene_drop_target(const UiSettingsScene&, float y, u64* control_id, bool* after) noexcept;
// Drag feedback for list reordering; zero ids clear it.
bool ui_settings_scene_set_drag(UiSettingsScene*, u64 dragged, u64 drop_control, bool after) noexcept;
// Unsaved-changes bar shake offset (DIPs) and alert tint in [0, 1].
bool ui_settings_scene_set_footer_motion(UiSettingsScene*, float shift, float alert) noexcept;

// Modal chrome and host-driven state. All return whether anything changed.
bool ui_settings_scene_set_presentation(UiSettingsScene*, UiSettingsPresentation) noexcept;
bool ui_settings_scene_bind_navigation(UiSettingsScene*, ConstSlice<UiSettingsNavEntry>, u32 revision) noexcept;
// The host owns query text and editing; geometry comes from the renderer.
bool ui_settings_scene_set_search(UiSettingsScene*, const UiSettingsEditor&) noexcept;
bool ui_settings_scene_search_geometry(const UiSettingsScene&, UiSettingsControlGeometry*) noexcept;
// Tints a control row; strength 0 removes the flash.
bool ui_settings_scene_set_flash(UiSettingsScene*, u64 control_id, float strength) noexcept;
// Starts the thumb animation of a toggled switch. Ticks advance the clock;
// tick returns whether a frame is needed.
bool ui_settings_scene_animate_toggle(UiSettingsScene*, UiSettingsTarget, bool checked, u64 now_ms) noexcept;
bool ui_settings_scene_tick(UiSettingsScene*, u64 now_ms, bool reduced_motion) noexcept;
bool ui_settings_scene_animating(const UiSettingsScene&) noexcept;
// Scrolls a control into view (with padding) without changing focus.
bool ui_settings_scene_reveal(UiSettingsScene*, UiSettingsTarget) noexcept;

struct UiSettingsPaintRanges {
    u32 page_first_quad, page_end_quad;
    // Quads drawn with the modal transform; the scrim precedes them.
    u32 modal_first_quad, modal_end_quad;
};
bool ui_settings_scene_page_owner(const UiSettingsScene&, UiWidgetReference) noexcept;
// The host-drawn aside (UiSettingsPageContent::aside_*) in page coordinates:
// its heading line, its card and the visible clip; beside reports the
// two-column layout. False when the page has none or it is out of view.
bool ui_settings_scene_aside(const UiSettingsScene&, UiRect* heading, UiRect* card, UiRect* clip,
                             bool* beside = nullptr) noexcept;
// Visible avatar rows with media, in page coordinates (before the modal layer
// transform). *cursor starts at 0 and advances; false after the last one.
struct UiSettingsAvatarGeometry {
    const UiSettingsAvatarMedia* media;
    UiWidgetReference owner;
    UiRect avatar, clip;
};
bool ui_settings_scene_next_avatar(const UiSettingsScene&, u32* cursor, UiSettingsAvatarGeometry*) noexcept;
bool ui_settings_scene_paint(const UiSettingsScene& scene, const Theme& theme, PaintStream* paint,
                             u32 dpi = 96, UiSettingsPaintRanges* ranges = nullptr) noexcept;
bool ui_settings_scene_text_output(const UiSettingsScene& scene, const Theme& theme,
                                   UiTextOutput* stream) noexcept;

} // namespace maicord

static_assert(sizeof(maicord::UiSettingsStyle) == 32);
static_assert(sizeof(maicord::UiSettingsControl) == 96);
static_assert(sizeof(maicord::UiSettingsControlPresentation) == 4);
