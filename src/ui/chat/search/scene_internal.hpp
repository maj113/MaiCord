#pragma once

#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/ui/theme/surface_elevation.hpp"
#include "maicord/ui/chat/search/scene.hpp"
#include "maicord/ui/chat/messages/presentation.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/widgets/menu_row.hpp"
#include "maicord/ui/core/scene.hpp"
#include "maicord/ui/chat/composer/attachment_actions.hpp"
#include "maicord/ui/voice/audio_controls.hpp"
#include "maicord/model/chat/message_poll_store.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {

enum class UiSearchItemKind : u8 {
    attachment,
    embed,
    content_link
};
struct UiSearchItem {
    UiRect rectangle; // body-relative, before scrolling
    u64 message_id, channel_id;
    u32 source_index, text_offset, text_bytes;
    u16 rich_index;
    UiSearchItemKind kind;
    MediaEmbedResource resource;
    bool snapshot;
};
struct UiSearchRichProjection {
    MessageScene scene;
    UiWidgetStorageOwner widgets;
    MessageTimeline* timeline;
    u32 source_revision, height_updates, layout_passes;
    UiSearchItem items[kUiSearchItemMaximum];
    const UiSearchSource* item_source;
    u32 item_count, item_revision, visible_count;
    float item_width;
    UiSearchLayout item_layout;
    u16 visible[kUiSearchVisibleItems];
    UiWidgetHandle primary[kUiSearchVisibleItems], secondary[kUiSearchVisibleItems];
    UiRect screens[kUiSearchVisibleItems];
    char details[kUiSearchVisibleItems][16];
    u8 detail_bytes[kUiSearchVisibleItems];
};

} // namespace maicord

namespace maicord::search_scene_detail {

constexpr auto kGradient = UiBackgroundSurfaceMode::gradient;
constexpr auto kSemantic = UiBackgroundBorderTone::semantic;
using Pen = UiTextPen<>;
bool rich_enabled(const UiSearchScene& s) noexcept;
const MessageRichRecord* rich_scoped(const MessageRichStore& store, u64 message_id, bool snapshot) noexcept;
UiSearchTarget rich_target(const UiSearchScene& s, u64 channel_id, u64 message_id, u32 source_index,
                           MessageSceneRichItemKind kind, u32 index, bool snapshot) noexcept;
bool chooser_action(UiSearchAction action) noexcept;
bool sort_action(UiSearchAction action) noexcept;
UiRect rich_viewport(const UiSearchScene& s) noexcept;
void configure_rich_input(MessageScene& scene, const UiSearchSource& source) noexcept;
bool bind_rich(UiSearchScene& s) noexcept;
bool chooser_open(const UiSearchScene& s) noexcept;
bool recent_shown(const UiSearchScene& s) noexcept;
float recent_top(const UiSearchScene& s) noexcept;
bool choices_authorized(const UiSearchFilterChoiceSource* source) noexcept;
bool choices_valid(const UiSearchFilterChoiceSource* source) noexcept;
UiIcon choice_icon(const UiSearchFilterChoiceSource& source, const UiSearchFilterChoiceView& row) noexcept;
bool choices_current(const UiSearchScene& scene) noexcept;
float choice_heading_height(const UiSearchScene& scene) noexcept;
float choice_hint_height(const UiSearchScene& scene) noexcept;
float choice_row_height(const UiSearchScene& scene) noexcept;
UiRect choice_rows_rectangle(const UiSearchScene& scene) noexcept;
bool choice_menu_geometry(const UiSearchScene& scene, const UiRect& rectangle,
                          UiMenuRowGeometry* geometry) noexcept;
bool choice_person(const UiSearchFilterChoiceSource& source, const UiSearchFilterChoiceView& row) noexcept;
UiRect choice_avatar_rectangle(const UiMenuRowGeometry& geometry, const UiRect& row) noexcept;
Utf8View tab_label(u32 tab) noexcept;
Utf8View url_hostname(Utf8View url) noexcept;
bool content_link(Utf8View text, u32* offset, u32* bytes) noexcept;
MessageMediaObscure item_obscure(const UiSearchScene& s, const UiSearchItem& item) noexcept;
UiSearchLayout layout_for(const UiSearchSource* source, const UiSearchContent& content) noexcept;
MessageResultCardHeaderLayout plain_header(const UiRect& card, const UiSearchRow& row) noexcept;
bool tiled(const UiSearchScene& s) noexcept;
UiRect tile_cover(const UiSearchScene& s, const UiSearchItem& item, const UiRect& tile) noexcept;
UiSearchTarget item_target(const UiSearchScene& s, const UiSearchItem& item, bool primary) noexcept;
const UiSearchItem* target_item(const UiSearchScene& s, const UiSearchTarget& target,
                                u32* slot = nullptr) noexcept;
bool measure_rows(UiSearchScene& s) noexcept;
UiRect widget_viewport(const UiSearchScene& s) noexcept;
bool current(const UiSearchScene& s) noexcept;
Utf8View label(UiSearchAction a) noexcept;
void count_label(UiSearchScene& s) noexcept;
UiRect overlay_region(const UiSearchScene& s, const UiRect& rectangle) noexcept;
UiRect overlay_clip(const UiSearchScene& s, const UiRect& rectangle) noexcept;
bool menu_hit(const UiSearchScene& s, float x, float y) noexcept;
UiRect item_jump_rectangle(const UiRect& row) noexcept;
Utf8View item_title(const UiSearchScene& s, const UiSearchItem& item) noexcept;
Utf8View item_link(const UiSearchScene& s, const UiSearchItem& item) noexcept;
bool paint_items(PaintStream* out, const UiSearchScene& s, const Theme& theme,
                 MessageMediaCommandStream* media) noexcept;
bool paint_tabs(PaintStream* out, const UiSearchScene& s, const Theme& theme) noexcept;
bool rebuild(UiSearchScene& s) noexcept;
void text(Pen& out, const UiSearchScene& s, UiWidgetHandle owner, const Utf8View& value, const UiRect& r,
          const Color& color, u32 style, u32 mode = 0, bool chooser_text = false) noexcept;
void centered(Pen& out, const UiSearchScene& s, UiWidgetHandle owner, const Utf8View& value, const UiRect& r,
              const Color& color, u32 style = text_style_subtext) noexcept;
UiRect rich_item_clip(const UiSearchScene& s, const UiRect& rectangle, const UiRect& node_clip) noexcept;

} // namespace maicord::search_scene_detail
