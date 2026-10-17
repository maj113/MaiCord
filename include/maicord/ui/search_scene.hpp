#pragma once
#include "maicord/ui/edit_field.hpp"
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/search_filter_context.hpp"

namespace maicord {
struct UiShellHeaderGeometry;
struct UiSearchRichProjection;
inline constexpr u32 kUiSearchVisibleRows = 24;
inline constexpr u32 kUiSearchWidgetCapacity =
    48 + 2 * kUiSearchVisibleRows * kGatewayMessageAttachmentMaximum;
// Media, Files and Links tabs lay out individual attachments/links rather than messages.
inline constexpr u32 kUiSearchItemMaximum = 160;
inline constexpr u32 kUiSearchVisibleItems = 64;
enum class UiSearchLayout : u8 {
    list,
    grid,
    files,
    links
};
enum class UiSearchPhase : u8 {
    empty,
    loading,
    indexing,
    ready,
    failed
};
enum class UiSearchAction : u8 {
    none,
    edit,
    submit,
    close,
    filters,
    previous,
    next,
    newest,
    oldest,
    relevance,
    result,
    filter_from,
    filter_in,
    filter_has,
    filter_mentions,
    filter_on,
    filter_before,
    filter_after,
    filter_pinned,
    filter_link_from,
    filter_file_type,
    filter_file_name,
    filter_author_type,
    page,
    sort,
    more_filters,
    rich_item,
    filter_value,
    tab,    // source_index is the MessageSearchTab
    recent, // source_index is the recent search
    clear_recent,
    scope   // the field's removable scope chip
};
// The panel's close control: a 32-DIP square icon button at the top right.
inline constexpr float kUiSearchCloseExtent = 32;
constexpr UiRect ui_search_close_rectangle(UiRect panel) noexcept {
    return {panel.right - 8 - kUiSearchCloseExtent, panel.top + 8, panel.right - 8,
            panel.top + 8 + kUiSearchCloseExtent};
}
// A host scope chip sits at the field's leading edge. A field too narrow for
// it and a useful query keeps the scope in the placeholder alone.
inline constexpr float kUiSearchScopeChipWidth = 76, kUiSearchScopeChipMinimumField = 200;
constexpr UiRect ui_search_scope_chip_rectangle(UiRect field) noexcept {
    return field.right - field.left < kUiSearchScopeChipMinimumField
               ? UiRect{}
               : UiRect{field.left + 4, field.top + 4, field.left + 4 + kUiSearchScopeChipWidth, field.bottom - 4};
}
constexpr bool ui_search_action_filter(UiSearchAction action) noexcept {
    return action >= UiSearchAction::filter_from && action <= UiSearchAction::filter_author_type;
}
struct UiSearchTarget {
    UiSearchAction action;
    u64 channel_id, message_id;
    u32 page_offset = 0;
    u32 source_revision = 0, source_index = ~u32{0};
    MessageSceneRichItemKind rich_kind = MessageSceneRichItemKind::attachment;
    u16 rich_index = 0;
    bool snapshot = false;
    MediaEmbedResource resource = MediaEmbedResource::none;
    bool save = false;
};
constexpr bool ui_search_target_equal(UiSearchTarget a, UiSearchTarget b) noexcept {
    return a.action == b.action && a.channel_id == b.channel_id && a.message_id == b.message_id &&
           a.page_offset == b.page_offset && a.source_revision == b.source_revision &&
           a.source_index == b.source_index && a.rich_kind == b.rich_kind && a.rich_index == b.rich_index &&
           a.snapshot == b.snapshot && a.resource == b.resource && a.save == b.save;
}
struct UiSearchRow {
    u64 channel_id, message_id;
    Utf8View author, channel, body, detail;
    bool hit, group_first;
    // Wrapped body area in DIPs; 0 uses the compact 48-DIP fallback.
    // Positive values must be finite and in [24, 464] (card max 512).
    float body_height = 0;
    // Borrowed from the accepted result bank, not the selected channel's
    // user cache. Old pages stop holding media when the source revision changes.
    u64 author_id = 0;
    Utf8View avatar_hash{};
    bool unavailable = false;
    bool access_check = false; // Metadata-only private membership/hydration action.
};
struct UiSearchSource {
    void* context;
    u32 count, revision;
    bool (*resolve)(void*, u32, UiSearchRow*) noexcept;
    // Optional host guard that invalidates results on profile/account/search scope changes.
    // Offline sources omit it. It must not mutate or resolve any result rows.
    bool (*current)(void*) noexcept = nullptr;
    // Optional view of the accepted result bank. The store keeps canonical lookup order;
    // rich_order maps Search display indices to bank handles. Full outer/
    // snapshot data are independent of the open conversation. The host keeps
    // this timeline/order valid under revision/current.
    MessageTimeline* rich_timeline = nullptr;
    ConstSlice<MessageHandle> rich_order{};
    const MessageScenePollBinding* polls{};
};
struct UiSearchContent {
    UiEditView edit;
    Utf8View status;
    u64 total;
    u32 offset, revision;
    UiSearchPhase phase;
    u8 sort;
    bool panel, stale, previous, next, filters;
    // Host-owned scope labels, never inferred from result channels. Strings are
    // borrowed under content revision like status/edit. Empty values use defaults.
    Utf8View heading{}, placeholder{}, filter_summary{};
    // Optional removable scope chip label inside the field, for example
    // "Threads" while the thread browser is open. Borrowed like heading.
    Utf8View scope_chip{};
    u32 page_size = 25;
    bool total_estimated = false;
    bool suggestions = false, sort_menu = false;
    const UiSearchFilterChoiceSource* filter_choices = nullptr;
    // Tab strip: tab is selected, result_tab describes the published results.
    bool tabs = false;
    u8 tab = 0, result_tab = 0, tab_known = 0;
    u64 tab_totals[5]{};
    // Recent searches shown under the filter list while the field is empty.
    // Borrowed under the content revision, newest first, at most five.
    const Utf8View* recent = nullptr;
    u8 recent_count = 0;
};
struct UiSearchScene {
    MessageMediaPolicy media_policy;
    bool media_policy_dirty;
    const UiResolvedThemeStyles* theme_styles{};
    const UiSearchFilterChoiceSource* choice_source{};
    const UiSearchFilterChoiceView* choice_rows{};
    u32 choice_source_revision{}, choice_geometry_revision{};
    u16 choice_first{}, choice_count{}, choice_total{}, choice_selected{kUiSearchFilterNoSelection};
    bool choice_details{};
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    const UiSearchSource* source;
    const UiSearchContent* content;
    UiRect viewport, field, text, panel, body, chooser, sort_chooser, scope_chip;
    UiWidgetHandle root, edit, scope, buttons[9], rows[kUiSearchVisibleRows];
    UiWidgetHandle pages[5];
    UiWidgetHandle tabs[5];
    char tab_text[5][12];
    u8 tab_bytes[5];
    UiSearchLayout layout;
    float content_height;
    UiSearchRow visible[kUiSearchVisibleRows];
    UiRect row_rectangles[kUiSearchVisibleRows];
    UiRect group_rectangles[kUiSearchVisibleRows];
    float row_offsets[126];
    u32 measured_source_revision, measured_count;
    const UiSearchSource* measured_source;
    UiSearchTarget* targets;
    u32 first, count, source_revision, content_revision, revision;
    u64 pagination_total;
    u32 pagination_offset, pagination_page_size;
    float scroll_dips;
    char count_text[64];
    char page_text[5][12];
    u8 page_bytes[5], page_count;
    u16 content_layout;
    u8 count_bytes;
    bool active, overlay;
    Arena rich_projection_storage{};
    UiSearchRichProjection* rich_projection = nullptr;
};
bool ui_search_scene_bind_theme_styles(UiSearchScene*, const UiResolvedThemeStyles*) noexcept;
bool ui_search_scene_set_media_policy(UiSearchScene*, const MessageMediaPolicy&) noexcept;
// Wide windows get room for the five tabs; the minimum window keeps its 360-DIP pane.
// The shell reserves this width beside the chat column while the pane is open.
constexpr float ui_search_panel_width(float viewport_width) noexcept {
    return minimum(viewport_width >= 900 ? 420.f : 360.f, maximum(0.f, viewport_width));
}
bool ui_search_scene_update(UiSearchScene*, const UiSearchSource*, const UiSearchContent*, UiRect viewport,
                            UiRect title, const UiShellHeaderGeometry* header = nullptr) noexcept;
void ui_search_scene_release(UiSearchScene*) noexcept;
bool ui_search_scene_scroll(UiSearchScene*, float offset) noexcept;
float ui_search_scene_scroll_maximum(const UiSearchScene&) noexcept;
bool ui_search_scene_filter_choice_geometry(const UiSearchScene&, u32 source_index, UiRect*,
                                            UiRect*) noexcept;
// A visible from:/mentions: people row's avatar circle and the row widget that
// owns it; the renderer draws the user's avatar over the placeholder.
bool ui_search_scene_filter_choice_avatar(const UiSearchScene&, u32 source_index, UiRect* rectangle,
                                          UiRect* clip, UiWidgetHandle* owner) noexcept;
UiSearchTarget ui_search_scene_hit_test(const UiSearchScene&, float x, float y) noexcept;
bool ui_search_scene_contains(const UiSearchScene&, float x, float y) noexcept;
bool ui_search_scene_current(const UiSearchScene&) noexcept;
MessageScene* ui_search_scene_message_scene(UiSearchScene*) noexcept;
const MessageScene* ui_search_scene_message_scene(const UiSearchScene&) noexcept;
// Call after the existing native text coordinator updates measured row heights.
bool ui_search_scene_refresh_rich(UiSearchScene*) noexcept;
bool ui_search_scene_target_current(const UiSearchScene&, UiSearchTarget) noexcept;
UiSearchTarget ui_search_scene_rich_widget_target(const UiSearchScene&, UiWidgetHandle) noexcept;
UiRect ui_search_scene_content_clip(const UiSearchScene&, UiRect rectangle) noexcept;
bool ui_search_scene_rich_preview_geometry(const UiSearchScene&, UiSearchTarget, UiRect* rectangle,
                                           UiRect* clip) noexcept;
bool ui_search_scene_avatar(const UiSearchScene&, u32 visible_slot, UiRect* rectangle,
                            UiRect* clip) noexcept;
// Visible media tiles of the grid layout, for media requests. The target is a
// rich_item whose preview geometry is the tile's cover rectangle.
u32 ui_search_scene_visible_tile_count(const UiSearchScene&) noexcept;
bool ui_search_scene_visible_tile(const UiSearchScene&, u32 slot, UiSearchTarget*) noexcept;
constexpr float ui_search_body_text_width(const UiSearchScene& scene) noexcept {
    return maximum(1.f, ui_rect_width(scene.body) - 92.f);
}
bool ui_search_scene_paint(const UiSearchScene&, const Theme&, PaintStream*, const UiEditGeometry*, u32 dpi,
                           MessageMediaCommandStream* rich_media = nullptr) noexcept;
bool ui_search_scene_text_output(const UiSearchScene&, const Theme&, UiTextOutput*,
                                 const UiEditGeometry*) noexcept;
} // namespace maicord
