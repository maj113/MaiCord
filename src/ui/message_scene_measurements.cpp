#include "message_scene_measurements.hpp"

namespace maicord::message_scene_detail {
bool initialize_measurements(MessageScene* scene, bool deferred) noexcept {
    // This fresh reservation already contains zero-filled pages. Do not touch
    // unused slots before a message needs its retained measurement.
    return scene && scene->timeline && scene->timeline->store &&
           stable_storage_initialize(&scene->owned_measurements, 2u * scene->timeline->store->capacity,
                                     deferred ? 0u : scene->timeline->store->capacity, sizeof(Measurement),
                                     alignof(Measurement));
}

Measurement* measurement(MessageScene* scene, u32 slot, bool snapshot) noexcept {
    if (!scene || !scene->timeline || slot >= scene->timeline->store->capacity) return nullptr;
    if (snapshot) slot += scene->timeline->store->capacity;
    // Newly committed pages are zero: an unmeasured slot reads as not measured.
    if (slot >= scene->owned_measurements.capacity &&
        !stable_storage_reserve(&scene->owned_measurements, slot + 1u)) return nullptr;
    return stable_storage_data<Measurement>(scene->owned_measurements) + slot;
}

void restore_measurement(MessageSceneRowWidget* row, const Measurement& value) noexcept {
    row->rich_layout = value.rich;
    row->rich_item_layout = value.items;
    row->measured_reply_height = value.reply_height;
    row->measured_content_height = value.content_height;
    row->grouped = value.grouped;
    row->text_measured = value.measured;
    row->rich_layout_measured = value.rich_measured;
    for (u32 i = 0; i < kGatewayMessageReactionMaximum; ++i) row->reaction_widths[i] = value.reactions[i];
}

void retain_measurement(MessageScene* scene, const MessageSceneRowWidget& row) noexcept {
    Measurement* value = measurement(scene, row.message.slot, row.forwarded_snapshot);
    if (!value) return;
    value->rich = row.rich_layout;
    value->items = row.rich_item_layout;
    value->reply_height = row.measured_reply_height;
    value->content_height = row.measured_content_height;
    value->grouped = row.grouped;
    value->measured = row.text_measured;
    value->rich_measured = row.rich_layout_measured;
    for (u32 i = 0; i < kGatewayMessageReactionMaximum; ++i) value->reactions[i] = row.reaction_widths[i];
}

} // namespace maicord::message_scene_detail
