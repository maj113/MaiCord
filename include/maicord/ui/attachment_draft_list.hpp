#pragma once
#include "maicord/ui/widget_builder.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/media/asset_key.hpp"

namespace maicord {
inline constexpr u32 kUiAttachmentDraftMaximum = 10;
inline constexpr u32 kUiAttachmentDraftParts = 11;
enum class UiAttachmentDraftAction : u8 {
    none,
    remove,
    retry,
    spoiler,
    describe
};
enum class UiAttachmentDraftPhase : u8 {
    preparing,
    waiting,
    uploading,
    ready,
    failed
};
struct UiAttachmentDraftView {
    u64 id;
    Utf8View filename, status, description;
    u64 bytes, uploaded_bytes;
    MediaAssetKey preview;
    UiAttachmentDraftPhase phase;
    bool spoiler, can_remove, can_retry, can_describe, preview_ready;
};
constexpr bool ui_attachment_draft_progress_visible(const UiAttachmentDraftView& view) noexcept {
    return view.phase == UiAttachmentDraftPhase::uploading || view.phase == UiAttachmentDraftPhase::waiting;
}
struct UiAttachmentDraftStyle {
    float minimum_card_width = 240, preview_extent = 64, gap = 8, padding = 10;
    float control_height = 32, control_padding = 8, radius = 6, progress_height = 3;
};
struct UiAttachmentDraftTarget {
    u64 id;
    UiAttachmentDraftAction action;
    UiWidgetHandle widget;
};
struct UiAttachmentDraftList {
    UiWidgetHandle parts[kUiAttachmentDraftMaximum][kUiAttachmentDraftParts];
    ConstSlice<UiAttachmentDraftView> rows;
    UiAttachmentDraftStyle style;
    UiWidgetManager* manager;
    u32 key_base;
};
bool ui_attachment_draft_list_build(UiAttachmentDraftList*, UiWidgetManager*, UiWidgetHandle parent,
                                    ConstSlice<UiAttachmentDraftView>, u32 key_base,
                                    const UiAttachmentDraftStyle&) noexcept;
Utf8View ui_attachment_draft_list_label(const UiAttachmentDraftList&, u32 key) noexcept;
bool ui_attachment_draft_list_single_line(const UiAttachmentDraftList&, u32 key) noexcept;
bool ui_attachment_draft_list_layout(UiAttachmentDraftList*, float available_width, bool editable) noexcept;
UiAttachmentDraftTarget ui_attachment_draft_list_target(const UiAttachmentDraftList&,
                                                        UiWidgetHandle) noexcept;
UiWidgetHandle ui_attachment_draft_list_handle(const UiAttachmentDraftList&, u64 id,
                                               UiAttachmentDraftAction) noexcept;
UiRect ui_attachment_draft_list_preview(const UiAttachmentDraftList&, u32 row,
                                        UiRect* clip = nullptr) noexcept;
UiRect ui_attachment_draft_list_progress(const UiAttachmentDraftList&, u32 row) noexcept;
bool ui_attachment_draft_list_paint(const UiAttachmentDraftList&, const Theme&, PaintStream*,
                                    bool focus_visible) noexcept;
bool ui_attachment_draft_list_text(const UiAttachmentDraftList&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
