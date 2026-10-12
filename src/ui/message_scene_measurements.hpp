#pragma once

#include "maicord/ui/message_scene.hpp"

namespace maicord::message_scene_detail {

// Independent of materialized widgets. Slot identity includes message ID/revision,
// rich metadata, wrapping width and grouping; recycled store slots cannot alias.
struct Measurement {
    u64 signature;
    MessageSceneRichLayout rich;
    MessageSceneRichItemLayout items;
    float reactions[kGatewayMessageReactionMaximum];
    float reply_height;
    float content_height;
    bool grouped;
    bool measured;
    bool rich_measured;
};

bool initialize_measurements(MessageScene* scene, bool deferred = false) noexcept;
Measurement* measurement(MessageScene* scene, u32 slot, bool snapshot = false) noexcept;
void restore_measurement(MessageSceneRowWidget* row, const Measurement& value) noexcept;
void retain_measurement(MessageScene* scene, const MessageSceneRowWidget& row) noexcept;

} // namespace maicord::message_scene_detail
