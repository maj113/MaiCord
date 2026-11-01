#pragma once
// Caller-storage initializer for the message media controller. The shipping client builds it from owned
// stable storage (message_media_controller_initialize_owned); the fixed-capacity variant is for tests.
#include "maicord/app/message_media_controller.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
inline bool message_media_controller_initialize(MessageMediaController* controller,
                                                Slice<MessageMediaVisibleRow> rows,
                                                Slice<MessageMediaAssetMark> asset_marks) noexcept {
    const u32 marks = asset_marks.count;
    if (!controller || !rows.data || rows.count == 0 || !asset_marks.data || marks == 0 ||
        (marks & (marks - 1u)) != 0 || marks < 4)
        return false;
    reset_value<MessageMediaController>(controller);
    controller->rows = rows.data;
    controller->asset_marks = asset_marks.data;
    controller->row_capacity = rows.count;
    controller->asset_mark_capacity = marks;
    controller->asset_mark_epoch = 1;
    for (u32 index = 0; index < marks; ++index)
        reset_value<MessageMediaAssetMark>(&asset_marks[index]);
    controller->initialized = true;
    return true;
}
} // namespace maicord
