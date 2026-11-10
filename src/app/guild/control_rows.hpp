#pragma once
#include "maicord/core/memory.hpp"
#include "maicord/ui/settings/scene.hpp"

namespace maicord::app_detail {
// The parallel arrays of a modal's projected controls and how many rows they hold.
struct ControlRows {
    UiSettingsControl* controls;
    UiSettingsControlPresentation* presentation;
    UiSettingsAvatarMedia* avatar_media; // null for a page without server tiles
    u32* count;
    u32 capacity;
};
// Appends one row and returns it for its optional fields (choices, editor, action label). Past capacity the row
// lands in a scratch sink the page never reads.
__declspec(noinline) inline UiSettingsControl& control_row_add(const ControlRows& rows, u64 id, const Utf8View& label,
                                                               const Utf8View& description, UiSettingsControlKind kind,
                                                               bool enabled, bool checked,
                                                               const UiSettingsControlPresentation& shown) noexcept {
    static UiSettingsControl overflow;
    if (*rows.count >= rows.capacity)
        return overflow;
    const u32 index = (*rows.count)++;
    rows.presentation[index] = shown;
    if (rows.avatar_media)
        reset_value(&rows.avatar_media[index]);
    rows.controls[index] = {id, label, description, kind, enabled, checked, {}, 0, {}, nullptr};
    return rows.controls[index];
}
} // namespace maicord::app_detail
