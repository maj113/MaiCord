#pragma once

#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/theme/foreground_color.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/theme/background_presentation.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/core/transition.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/theme/styles.hpp"
#include "maicord/ui/core/visit_ref.hpp"

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
constexpr float kAsideGap = 36, kAsideFormMinimum = 280, kAsideHeadingGap = 8;
// The scene's layout tokens, computed in place (a by-value return would be copied).
struct SceneTokens : UiSettingsLayoutTokens {
    explicit SceneTokens(const UiSettingsScene& scene) noexcept {
        ui_settings_fill_layout_tokens(scene.theme_styles, this);
    }
};
// One row's geometry, shared by measurement, paint, text and hit-testing.
struct RowLayout {
    UiRect row, label, description, control, lead, chips, value;
    float height;
};
// Parts of a list row or card, from its row rectangle.
struct ListParts {
    UiRect content, icon, lock, column, chevron;
    UiRect actions[kUiSettingsListActions];
};
// Sidebar rows: bound navigation, or a header plus every registered page.
struct NavRow {
    UiSettingsNavKind kind;
    bool destructive;
    UiIcon icon;
    Utf8View label;
    UiSettingsTarget target;
};
struct Frame {
    UiRect nav, body, title, description, footer;
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
const UiSettingsListItem* list_item(const UiSettingsPageContent& content, u32 index) noexcept;
bool list_style(const UiSettingsPageContent& content, u32 index, UiSettingsListStyle style) noexcept;
bool progress(const UiSettingsPageContent& content, u32 index) noexcept;
float button_label_width(const Utf8View& label) noexcept;
ListParts list_parts(const UiSettingsLayoutTokens& t, const UiSettingsPageContent& content, u32 index,
                     const UiRect& row) noexcept;
RowLayout row_layout(const UiSettingsLayoutTokens& t, const UiSettingsStyle& style, const UiSettingsPageContent& content,
                     u32 index, float left, float top, float width) noexcept;
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
UiControlState hover_option(const UiSettingsScene& scene, UiControlState state, u32 index) noexcept;
UiControlState widget_state(const UiSettingsScene& scene, UiWidgetHandle handle, const UiSettingsTarget& target,
                            bool enabled) noexcept;
float toggle_motion(const UiSettingsScene& scene, const UiSettingsTarget& target, const UiSettingsLayoutTokens& t,
                    bool checked) noexcept;
void reveal_range(UiSettingsScene* scene, bool sidebar, float first, float last, const UiSettingsMetrics& m) noexcept;
float nav_offset_of(const UiSettingsScene& scene, const UiSettingsMetrics& m, const UiSettingsTarget& target,
                    float* height) noexcept;
bool apply_scroll(UiSettingsScene* scene, bool sidebar, float previous) noexcept;
bool visit_targets(const UiSettingsScene& scene, UiSettingsFocusZone zone,
                   UiVisitRef<UiSettingsTarget> visit) noexcept;
UiSettingsFocusZone zone_of(const UiSettingsTarget& target) noexcept;
UiRect option_rect(const UiSettingsLayoutTokens& t, const UiSettingsControl& c, const UiRect& field, u32 index) noexcept;

} // namespace maicord::settings_scene_detail
