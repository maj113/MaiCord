#include "message_scene_measurements.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::message_scene_detail {
namespace {
// The item layout's slot in the second bank (same slot space as the first).
u32 item_slot(const MessageScene& scene, u32 slot, bool snapshot) noexcept {
    return snapshot ? slot + scene.timeline->store->capacity : slot;
}

bool items_zero(const MessageSceneRichItemLayout& items) noexcept {
    const u8* bytes = reinterpret_cast<const u8*>(&items);
    u8 any = 0;
    for (u32 i = 0; i < sizeof(items); ++i) any |= bytes[i];
    return any == 0;
}
} // namespace

bool initialize_measurements(MessageScene* scene, bool deferred) noexcept {
    // This fresh reservation already contains zero-filled pages. Do not touch
    // unused slots before a message needs its retained measurement.
    if (!scene || !scene->timeline || !scene->timeline->store) return false;
    scene->measured_slots = scene->measured_snapshot_slots = 0;
    const u32 reserved = 2u * scene->timeline->store->capacity;
    if (!stable_storage_initialize(&scene->owned_measurements, reserved,
                                   deferred ? 0u : scene->timeline->store->capacity, sizeof(Measurement),
                                   alignof(Measurement)))
        return false;
    // The item bank belongs to the measurements: a fresh reservation starts it fresh too.
    stable_storage_release(&scene->owned_item_layouts);
    if (!stable_storage_initialize(&scene->owned_item_layouts, reserved, 0, sizeof(MessageSceneRichItemLayout),
                                   alignof(MessageSceneRichItemLayout))) {
        stable_storage_release(&scene->owned_measurements);
        return false;
    }
    return true;
}

Measurement* measurement(MessageScene* scene, u32 slot, bool snapshot) noexcept {
    if (!scene || !scene->timeline || slot >= scene->timeline->store->capacity) return nullptr;
    const u32 body_slot = slot;
    if (snapshot) slot += scene->timeline->store->capacity;
    // Newly committed pages are zero: an unmeasured slot reads as not measured.
    if (slot >= scene->owned_measurements.capacity &&
        !stable_storage_reserve(&scene->owned_measurements, slot + 1u)) return nullptr;
    u32& touched = snapshot ? scene->measured_snapshot_slots : scene->measured_slots;
    if (body_slot >= touched) touched = body_slot + 1u;
    return stable_storage_data<Measurement>(scene->owned_measurements) + slot;
}

void clear_measurements(MessageScene* scene) noexcept {
    // Untouched slots are still the zero pages they were committed as; zeroing
    // them would only make them resident. Item layouts need no clearing: a
    // zeroed measurement owns none.
    if (!scene->owned_measurements.arena.base) return;
    Measurement* data = stable_storage_data<Measurement>(scene->owned_measurements);
    clear_bytes(data, scene->measured_slots * sizeof(Measurement));
    if (scene->measured_snapshot_slots)
        clear_bytes(data + scene->timeline->store->capacity, scene->measured_snapshot_slots * sizeof(Measurement));
}

void restore_measurement(const MessageScene& scene, MessageSceneRowWidget* row, const Measurement& value) noexcept {
    copy_value(&row->rich_layout, value.rich);
    if (value.has_items)
        copy_value(&row->rich_item_layout,
                   stable_storage_data<MessageSceneRichItemLayout>(scene.owned_item_layouts)
                       [item_slot(scene, row->message.slot, row->forwarded_snapshot)]);
    else
        reset_value(&row->rich_item_layout);
    row->measured_reply_height = value.reply_height;
    row->measured_content_height = value.content_height;
    row->grouped = value.grouped;
    row->text_measured = value.measured;
    row->rich_layout_measured = value.rich_measured;
    for (u32 i = 0; i < kGatewayMessageReactionMaximum; ++i) row->reaction_widths[i] = value.reactions[i];
}

void store_items(MessageScene* scene, u32 slot, bool snapshot, Measurement* value,
                 const MessageSceneRichItemLayout& items) noexcept {
    value->has_items = false;
    if (items_zero(items)) return;
    const u32 at = item_slot(*scene, slot, snapshot);
    if (at >= scene->owned_item_layouts.capacity && !stable_storage_reserve(&scene->owned_item_layouts, at + 1u)) {
        // No room to retain the layout: the slot reads as unmeasured and is measured again.
        value->measured = value->rich_measured = false;
        return;
    }
    copy_value(stable_storage_data<MessageSceneRichItemLayout>(scene->owned_item_layouts) + at, items);
    value->has_items = true;
}

void retain_measurement(MessageScene* scene, const MessageSceneRowWidget& row) noexcept {
    Measurement* value = measurement(scene, row.message.slot, row.forwarded_snapshot);
    if (!value) return;
    copy_value(&value->rich, row.rich_layout);
    value->reply_height = row.measured_reply_height;
    value->content_height = row.measured_content_height;
    value->grouped = row.grouped;
    value->measured = row.text_measured;
    value->rich_measured = row.rich_layout_measured;
    for (u32 i = 0; i < kGatewayMessageReactionMaximum; ++i) value->reactions[i] = row.reaction_widths[i];
    store_items(scene, row.message.slot, row.forwarded_snapshot, value, row.rich_item_layout);
}

} // namespace maicord::message_scene_detail
