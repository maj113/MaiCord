#pragma once
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
inline constexpr u32 kUiDownloadMaximumRows = 8;
inline constexpr u32 kUiDownloadVisibleRows = 3;
inline constexpr u32 kUiDownloadWidgetCapacity = 6 + kUiDownloadVisibleRows * 2;
enum class UiDownloadPhase : u8 { queued, preparing, downloading, committing, saved, failed, cancelled };
enum class UiDownloadMode : u8 { receipts, confirmation };
enum class UiDownloadAction : u8 { none, close, previous, next, cancel, retry, reveal, dismiss, confirm, cancel_confirmation };
struct UiDownloadRow {
    u64 id{};
    Utf8View filename{}, source{}, status{};
    u64 bytes{}, total{};
    UiDownloadPhase phase{};
    bool indeterminate{}, cancel{}, retry{}, reveal{}, dismiss{};
};
struct UiDownloadSource {
    void* context{};
    u32 count{}, revision{};
    bool (*resolve)(void*, u32, UiDownloadRow*) noexcept{};
    bool (*current)(void*) noexcept{};
};
struct UiDownloadContent {
    Utf8View heading{}, status{}, filename{}, warning{};
    u64 confirmation_id{};
    u32 revision{};
    UiDownloadMode mode{};
};
struct UiDownloadTarget {
    UiDownloadAction action{};
    u64 id{};
    u32 index{}, source_revision{};
};
struct UiDownloadScene {
    static constexpr bool kZeroDefault = true; // Every member defaults to zero: reset_value clears bytes.
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    const UiDownloadSource* source{};
    const UiDownloadContent* content{};
    UiDownloadRow* accepted{};
    UiRect viewport{}, panel{}, hint{}, body{}, footer{}, warning{}, confirmation_filename{},
        confirm_rectangle{}, cancel_rectangle{}, previous_rectangle{}, next_rectangle{};
    UiWidgetHandle root{}, close{}, previous{}, next{}, confirm{}, cancel{},
        primary[kUiDownloadVisibleRows]{}, secondary[kUiDownloadVisibleRows]{};
    UiRect cards[kUiDownloadVisibleRows]{}, primary_rectangles[kUiDownloadVisibleRows]{},
        secondary_rectangles[kUiDownloadVisibleRows]{};
    char progress[kUiDownloadVisibleRows][96]{}, page_label[32]{};
    u8 progress_bytes[kUiDownloadVisibleRows]{}, page_label_bytes{};
    u32 first{}, count{}, accepted_count{}, page{}, page_size{}, source_revision{}, content_revision{}, revision{};
    u64 confirmation_id{};
    UiDownloadMode mode{};
    bool active{};
};
// The source owns stable strings under revision/current authority fences.
// Closing only hides this surface. Transfer admission, cancellation, retry,
// final filesystem commit and shell reveal belong to the host workflow.
bool ui_download_scene_update(UiDownloadScene*, const UiDownloadSource*, const UiDownloadContent*, UiRect) noexcept;
void ui_download_scene_release(UiDownloadScene*) noexcept;
bool ui_download_scene_current(const UiDownloadScene&) noexcept;
UiDownloadTarget ui_download_scene_target(const UiDownloadScene&, UiWidgetHandle) noexcept;
UiDownloadTarget ui_download_scene_hit_test(const UiDownloadScene&, float x, float y) noexcept;
bool ui_download_scene_page(UiDownloadScene*, u32 page) noexcept;
bool ui_download_scene_set_hover(UiDownloadScene*, float x, float y, bool* changed) noexcept;
bool ui_download_scene_paint(const UiDownloadScene&, const Theme&, PaintStream*) noexcept;
bool ui_download_scene_text_output(const UiDownloadScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
