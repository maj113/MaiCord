#pragma once

#include "maicord/ui/message_scene.hpp"

namespace maicord::message_scene_detail {

// Independent of materialized widgets. Slot identity includes message ID/revision,
// rich metadata, wrapping width and grouping; recycled store slots cannot alias.
struct Measurement {
    u64 signature;
    MessageSceneRichLayout rich;
    float reactions[kGatewayMessageReactionMaximum];
    float reply_height;
    float content_height;
    bool grouped;
    bool measured;
    bool rich_measured;
    // The item layout (672 bytes) lives in the slot's entry of the second bank
    // while any of it is nonzero; plain text messages never touch that bank.
    bool has_items;
};

bool initialize_measurements(MessageScene* scene, bool deferred = false) noexcept;
Measurement* measurement(MessageScene* scene, u32 slot, bool snapshot = false) noexcept;
// Zeroes every slot measurement() handed out (geometry invalidation).
void clear_measurements(MessageScene* scene) noexcept;
void restore_measurement(const MessageScene& scene, MessageSceneRowWidget* row, const Measurement& value) noexcept;
void retain_measurement(MessageScene* scene, const MessageSceneRowWidget& row) noexcept;
// Retains a freshly measured item layout for the slot (see Measurement::has_items).
void store_items(MessageScene* scene, u32 slot, bool snapshot, Measurement* value,
                 const MessageSceneRichItemLayout& items) noexcept;

} // namespace maicord::message_scene_detail
