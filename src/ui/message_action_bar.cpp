#include "maicord/ui/message_action_bar.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/reaction_preferences.hpp"
#include "maicord/ui/message_scene.hpp"

namespace maicord {
namespace {

MessageActionBarResult reject(MessageActionBar* bar, MessageActionBarError error) noexcept {
    saturating_increment_if(bar, bar->rejected);
    return MessageActionBarResult{error, false};
}

// Overlays above the bar's centre cover its top edge, those below its bottom
// edge. A horizontal toolbar against horizontal banners stays rectangular.
UiRect occluded_clip(const MessageActionBar& bar, UiRect rectangle, UiRect viewport) noexcept {
    UiRect clip = viewport;
    const float middle = (rectangle.top + rectangle.bottom) * .5f;
    for (const UiRect& occluder : bar.occluders) {
        if (!ui_rect_intersects(occluder, rectangle))
            continue;
        if ((occluder.top + occluder.bottom) * .5f <= middle)
            clip.top = maximum(clip.top, occluder.bottom);
        else
            clip.bottom = minimum(clip.bottom, occluder.top);
    }
    clip.bottom = maximum(clip.bottom, clip.top);
    return clip;
}

bool layout_items(MessageActionBar* bar, UiRect row, UiRect viewport, bool divider = false) noexcept {
    if (!ui_rect_has_area(row) || !ui_rect_has_area(viewport) || !bar->item_count ||
        bar->item_count > kMessageActionBarCapacity) {
        return false;
    }
    const bool sectioned = bar->section_end != 0 && bar->section_end < bar->item_count;
    const float width = kMessageActionBarPanelPadding * 2.0f +
                        kMessageActionBarCellExtent * static_cast<float>(bar->item_count) +
                        (sectioned ? kMessageActionBarSectionGap : 0.0f);
    constexpr float height = kMessageActionBarPanelPadding * 2.0f + kMessageActionBarCellExtent;
    const float maximum_right = viewport.right - kMessageActionBarViewportMargin;
    const float preferred_right = row.right - kMessageActionBarRowRightInset;
    const float right = minimum(maximum_right, preferred_right);
    const float left = maximum(viewport.left + kMessageActionBarViewportMargin, right - width);
    const float top =
        maximum(viewport.top + kMessageActionBarPanelPadding,
                row.top - (divider ? 0 : kMessageActionBarRowOverhang));
    if (right - left < width || top + height > viewport.bottom) {
        return false;
    }
    bar->rectangle = UiRect{left, top, right, top + height};
    bar->viewport = viewport;
    bar->clip = occluded_clip(*bar, bar->rectangle, viewport);
    // Entirely under an overlay: nothing to show or hit.
    if (!ui_rect_intersects(bar->rectangle, bar->clip))
        return false;
    float x = left + kMessageActionBarPanelPadding;
    const float y = top + kMessageActionBarPanelPadding;
    for (u32 index = 0; index < bar->item_count; ++index) {
        if (sectioned && index == bar->section_end) {
            x += kMessageActionBarSectionGap;
        }
        bar->items[index].rectangle =
            UiRect{x, y, x + kMessageActionBarCellExtent, y + kMessageActionBarCellExtent};
        x += kMessageActionBarCellExtent;
    }
    return true;
}

bool forwardable(const MessageRecord& message) noexcept {
    return message.content_availability == MessageContentAvailability::available &&
           (message.type == 0 || message.type == 19 || message.type == 20 || message.type == 23) &&
           !(message.flags & (1u << 6)) && !(message.presentation_flags & kMessagePresentationForwardingObstacles);
}

u16 effective_capabilities(const MessageRecord& message, u16 capabilities) noexcept {
    if (message.delivery == MessageDeliveryState::failed)
        return 0;
    if (!forwardable(message))
        capabilities = static_cast<u16>(capabilities & ~message_action_bar_forward);
    return capabilities;
}

void push(MessageActionBar* bar, MessageActionBarAction action, MessageActionBarItemKind kind) noexcept {
    bar->items[bar->item_count++] = MessageActionBarItem{{}, {}, 0, action, kind};
}

u32 plugin_count() noexcept {
    UiPluginSlotNode nodes[kMessageActionBarPluginItems]{};
    return ui_plugin_slot_toolbar(kUiPluginSlotMessageHover, {nodes, kMessageActionBarPluginItems});
}
// Plugin buttons sit just before More (Delete when expanded).
void push_plugins(MessageActionBar* bar) noexcept {
    const u32 count = plugin_count();
    for (u32 index = 0; index < count && bar->item_count < kMessageActionBarCapacity - 1; ++index)
        bar->items[bar->item_count++] = MessageActionBarItem{
            {}, {}, index, static_cast<MessageActionBarAction>(static_cast<u32>(MessageActionBarAction::plugin_0) + index),
            MessageActionBarItemKind::plugin};
}
bool plugins_current(const MessageActionBar& bar, bool failed) noexcept {
    u32 present = 0;
    for (u32 index = 0; index < bar.item_count; ++index)
        present += bar.items[index].kind == MessageActionBarItemKind::plugin ? 1u : 0u;
    return present == (failed ? 0u : plugin_count());
}

// Expanded utilities lead; quick reactions appear only when collapsed.
bool build_items(MessageActionBar* bar, const MessageRecord& message, const ReactionPreferenceStore* preferences,
                 u16 capabilities) noexcept {
    using Action = MessageActionBarAction;
    using Kind = MessageActionBarItemKind;
    bar->item_count = 0;
    bar->section_end = 0;
    if (message.delivery == MessageDeliveryState::failed) {
        push(bar, Action::retry, Kind::retry);
        push(bar, Action::delete_message, Kind::delete_message);
        return true;
    }
    const bool own = (capabilities & message_action_bar_edit) != 0;
    const bool pinned = (message.presentation_flags & message_presentation_pinned) != 0;
    if (capabilities & message_action_bar_expanded) {
        if (capabilities & message_action_bar_copy_id)
            push(bar, Action::copy_id, Kind::copy_id);
        push(bar, Action::copy_link, Kind::copy_link);
        if (capabilities & message_action_bar_mark_unread)
            push(bar, Action::mark_unread, Kind::mark_unread);
        if (capabilities & message_action_bar_pin)
            push(bar, pinned ? Action::unpin : Action::pin, pinned ? Kind::unpin : Kind::pin);
        if (own)
            push(bar, Action::reply, Kind::reply);
        push(bar, Action::add_reaction, Kind::add_reaction);
        push(bar, own ? Action::edit : Action::reply, own ? Kind::edit : Kind::reply);
        if (capabilities & message_action_bar_forward)
            push(bar, Action::forward, Kind::forward);
        push_plugins(bar);
        if (capabilities & message_action_bar_delete)
            push(bar, Action::delete_message, Kind::delete_message);
        else
            push(bar, Action::more, Kind::more);
        return true;
    }
    MessageContextMenuModel context{};
    const MessageContextMenuResult menu =
        message_context_menu_build(&context, message, false, false, false, preferences);
    if (!menu.ok() || context.count < kMessageActionBarQuickReactionCount) {
        return false;
    }
    for (u32 index = 0; index < kMessageActionBarQuickReactionCount; ++index) {
        MessageContextQuickReaction quick{};
        if (!message_context_menu_resolve_quick_reaction(
                context, static_cast<u32>(MessageContextAction::quick_reaction_0) + index, &quick)) {
            return false;
        }
        bar->items[bar->item_count++] = MessageActionBarItem{
            {}, quick.glyph, quick.catalog_index,
            static_cast<Action>(static_cast<u32>(Action::quick_reaction_0) + index), Kind::quick_reaction};
    }
    bar->section_end = static_cast<u8>(kMessageActionBarQuickReactionCount);
    push(bar, Action::add_reaction, Kind::add_reaction);
    push(bar, own ? Action::edit : Action::reply, own ? Kind::edit : Kind::reply);
    if (capabilities & message_action_bar_forward)
        push(bar, Action::forward, Kind::forward);
    push_plugins(bar);
    push(bar, Action::more, Kind::more);
    return true;
}

bool pin_state_current(const MessageActionBar& bar, const MessageRecord& message) noexcept {
    const bool pinned = (message.presentation_flags & message_presentation_pinned) != 0;
    for (u32 index = 0; index < bar.item_count; ++index) {
        if ((bar.items[index].kind == MessageActionBarItemKind::pin && pinned) ||
            (bar.items[index].kind == MessageActionBarItemKind::unpin && !pinned))
            return false;
    }
    return true;
}

const MessageSceneRowWidget* owner_row(const MessageScene& scene, const MessageActionBar& bar) noexcept {
    for (u32 index = 0; index < scene.rows.count; ++index) {
        const auto& candidate = scene.rows[index];
        if (candidate.message_id == bar.message_id && candidate.widget.slot == bar.owner.handle.slot &&
            candidate.widget.generation == bar.owner.handle.generation)
            return &candidate;
    }
    return nullptr;
}

bool supported_delivery(const MessageRecord& message) noexcept {
    return message.delivery == MessageDeliveryState::authoritative ||
           message.delivery == MessageDeliveryState::failed;
}

struct ForwardPolicyAdapter {
    MessageActionBarForwardPolicy policy;
    void* context;
};

u16 adapt_forward_policy(void* context, u64 message_id) noexcept {
    const auto& adapter = *static_cast<const ForwardPolicyAdapter*>(context);
    return adapter.policy(adapter.context, message_id) ? u16{message_action_bar_forward} : u16{0};
}

} // namespace

MessageActionBarResult message_action_bar_open(MessageActionBar* bar, const MessageScene& scene,
                                               MessageSceneHit hit, const MessageRecord& message,
                                               const ReactionPreferenceStore* preferences, bool can_forward) noexcept {
    return message_action_bar_open(bar, scene, hit, message, preferences,
                                   MessageActionBarOptions{static_cast<u16>(can_forward ? message_action_bar_forward : 0)});
}

MessageActionBarResult message_action_bar_open(MessageActionBar* bar, const MessageScene& scene,
                                               MessageSceneHit hit, const MessageRecord& message,
                                               const ReactionPreferenceStore* preferences,
                                               MessageActionBarOptions options) noexcept {
    if (!bar || !scene.active || hit.message_id == 0 || hit.message_id != message.id ||
        !supported_delivery(message)) {
        return reject(bar, hit.message_id != 0 && message.id != 0
                               ? MessageActionBarError::unsupported_message
                               : MessageActionBarError::invalid_input);
    }
    const MessageSceneRowWidget* row = message_scene_find_row(scene, hit.source_index);
    const UiLayoutNode* layout = row ? message_scene_layout_node(scene, row->widget) : nullptr;
    if (!row || row->message_id != message.id || !layout) {
        return reject(bar, MessageActionBarError::invalid_input);
    }
    const u32 preference_revision = preferences ? preferences->revision : 0;
    const u16 capabilities = effective_capabilities(message, options.capabilities);
    const bool failed = message.delivery == MessageDeliveryState::failed;
    const bool divider = ui_rect_has_area(message_scene_unread_divider_rectangle(scene, *row));
    if (bar->active && bar->message_id == message.id && bar->preference_revision == preference_revision &&
        bar->capabilities == capabilities && pin_state_current(*bar, message) && plugins_current(*bar, failed) &&
        bar->item_count &&
        (bar->items[0].kind == MessageActionBarItemKind::retry) == failed &&
        bar->owner.manager == &scene.widgets && bar->owner.handle.slot == row->widget.slot &&
        bar->owner.handle.generation == row->widget.generation) {
        const UiRect previous = bar->rectangle;
        const UiRect previous_clip = bar->clip;
        if (!layout_items(bar, message_scene_message_rectangle(scene, *row), scene.viewport, divider)) {
            message_action_bar_close(bar);
            return reject(bar, MessageActionBarError::layout);
        }
        const bool changed = !ui_rect_equal(previous, bar->rectangle) || !ui_rect_equal(previous_clip, bar->clip);
        saturating_increment_if(changed, bar->revision);
        return MessageActionBarResult{MessageActionBarError::none, changed};
    }

    MessageActionBar candidate{};
    for (u32 index = 0; index < kMessageActionBarOccluderCount; ++index)
        candidate.occluders[index] = bar->occluders[index];
    candidate.owner = UiWidgetReference{&scene.widgets, row->widget};
    candidate.message_id = message.id;
    candidate.preference_revision = preference_revision;
    candidate.capabilities = capabilities;
    candidate.active = true;
    if (!build_items(&candidate, message, preferences, capabilities)) {
        return reject(bar, MessageActionBarError::quick_reactions);
    }
    if (!layout_items(&candidate, message_scene_message_rectangle(scene, *row), scene.viewport, divider)) {
        return reject(bar, MessageActionBarError::layout);
    }
    const bool opening = !bar->active || bar->message_id != message.id;
    bool items_changed = opening || bar->item_count != candidate.item_count;
    for (u32 index = 0; !items_changed && index < candidate.item_count; ++index) {
        items_changed = bar->items[index].action != candidate.items[index].action ||
                        bar->items[index].catalog_index != candidate.items[index].catalog_index;
    }
    candidate.hovered_index = !opening && !items_changed ? bar->hovered_index : kMessageActionBarNoItem;
    const bool changed = items_changed || !ui_rect_equal(bar->rectangle, candidate.rectangle) ||
                         bar->owner.handle.slot != candidate.owner.handle.slot ||
                         bar->owner.handle.generation != candidate.owner.handle.generation;
    candidate.opens = bar->opens;
    candidate.closes = bar->closes;
    candidate.rejected = bar->rejected;
    candidate.revision = bar->revision;
    saturating_increment_if(opening, candidate.opens);
    saturating_increment_if(changed, candidate.revision);
    copy_value(bar, candidate);
    return MessageActionBarResult{MessageActionBarError::none, changed};
}

bool message_action_bar_reconfigure(MessageActionBar* bar, const MessageScene& scene,
                                    const ReactionPreferenceStore* preferences, MessageActionBarOptions options,
                                    bool* changed) noexcept {
    if (changed)
        *changed = false;
    if (!bar)
        return false;
    if (!bar->active)
        return true;
    const MessageStoreResult found = scene.timeline && scene.timeline->store
                                         ? message_store_find(*scene.timeline->store, bar->message_id)
                                         : MessageStoreResult{};
    const MessageRecord* record =
        found.ok() ? message_store_resolve(*scene.timeline->store, found.handle) : nullptr;
    const MessageSceneRowWidget* row = owner_row(scene, *bar);
    const u32 revision = bar->revision;
    if (!record || !row ||
        !message_action_bar_open(bar, scene, {bar->message_id, row->widget, row->source_index}, *record,
                                 preferences, options)
             .ok())
        return message_action_bar_close(bar, changed);
    if (changed)
        *changed = revision != bar->revision;
    return true;
}

bool message_action_bar_synchronize(MessageActionBar* bar, const MessageScene& scene,
                                    bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!bar) {
        return false;
    }
    if (!bar->active) {
        return true;
    }
    const MessageStoreResult found = scene.timeline && scene.timeline->store
                                         ? message_store_find(*scene.timeline->store, bar->message_id)
                                         : MessageStoreResult{};
    const MessageRecord* message =
        found.ok() ? message_store_resolve(*scene.timeline->store, found.handle) : nullptr;
    const MessageSceneRowWidget* row = nullptr;
    for (u32 index = 0; message && index < scene.count; ++index) {
        if (scene.rows[index].message_id == bar->message_id) {
            row = &scene.rows[index];
            break;
        }
    }
    const UiLayoutNode* layout = row ? message_scene_layout_node(scene, row->widget) : nullptr;
    if (!message || !supported_delivery(*message) || !row || !layout) {
        return message_action_bar_close(bar, changed);
    }
    const UiRect previous = bar->rectangle;
    const UiRect previous_clip = bar->clip;
    const UiWidgetHandle previous_owner = bar->owner.handle;
    if (!layout_items(bar, message_scene_message_rectangle(scene, *row), scene.viewport,
            ui_rect_has_area(message_scene_unread_divider_rectangle(scene, *row)))) {
        return message_action_bar_close(bar, changed);
    }
    bar->owner = UiWidgetReference{&scene.widgets, row->widget};
    const bool updated = !ui_rect_equal(previous, bar->rectangle) || !ui_rect_equal(previous_clip, bar->clip) ||
                         previous_owner.slot != bar->owner.handle.slot ||
                         previous_owner.generation != bar->owner.handle.generation;
    saturating_increment_if(updated, bar->revision);
    if (changed) {
        *changed = updated;
    }
    return true;
}

bool message_action_bar_close(MessageActionBar* bar, bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!bar) {
        return false;
    }
    if (!bar->active) {
        return true;
    }
    bar->active = false;
    bar->message_id = 0;
    bar->hovered_index = kMessageActionBarNoItem;
    bar->item_count = 0;
    saturating_increment(bar->closes);
    saturating_increment(bar->revision);
    if (changed) {
        *changed = true;
    }
    return true;
}

UiRect message_action_bar_visible_rectangle(const MessageActionBar& bar) noexcept {
    if (!bar.active)
        return UiRect{};
    const UiRect visible = ui_rect_intersection(bar.rectangle, bar.clip);
    return ui_rect_has_area(visible) ? visible : UiRect{};
}

bool message_action_bar_set_occluders(MessageActionBar* bar, const UiRect* occluders, u32 count,
                                      bool* changed) noexcept {
    if (changed)
        *changed = false;
    if (!bar || (count && !occluders) || count > kMessageActionBarOccluderCount)
        return false;
    for (u32 index = 0; index < kMessageActionBarOccluderCount; ++index) {
        const UiRect next = index < count && ui_rect_has_area(occluders[index]) ? occluders[index] : UiRect{};
        bar->occluders[index] = next;
    }
    if (!bar->active)
        return true;
    const UiRect clip = occluded_clip(*bar, bar->rectangle, bar->viewport);
    if (ui_rect_equal(clip, bar->clip))
        return true;
    if (!ui_rect_intersects(bar->rectangle, clip))
        return message_action_bar_close(bar, changed);
    bar->clip = clip;
    saturating_increment(bar->revision);
    if (changed)
        *changed = true;
    return true;
}

MessageActionBarHit message_action_bar_hit_test(const MessageActionBar& bar, float x, float y) noexcept {
    const UiRect visible = message_action_bar_visible_rectangle(bar);
    if (!bar.active || bar.message_id == 0 || bar.item_count == 0 ||
        bar.item_count > kMessageActionBarCapacity || !ui_rect_contains(visible, x, y)) {
        return MessageActionBarHit{};
    }
    for (u32 index = 0; index < bar.item_count; ++index) {
        const MessageActionBarItem& item = bar.items[index];
        if (ui_rect_contains(item.rectangle, x, y)) {
            return MessageActionBarHit{
                bar.message_id, item.glyph, item.rectangle, item.catalog_index, index, item.action, true};
        }
    }
    return MessageActionBarHit{
        bar.message_id, {}, visible, 0, kMessageActionBarNoItem, MessageActionBarAction::none, true};
}

bool message_hover_scroll_gate_observe(MessageHoverScrollGate* gate, u32 generation, double scroll) noexcept {
    if (!gate)
        return false;
    const bool moved = gate->scroll_known && (gate->generation != generation || gate->scroll - scroll > .5 ||
                                              scroll - gate->scroll > .5);
    gate->scroll = scroll;
    gate->generation = generation;
    gate->scroll_known = true;
    if (!moved || gate->suppressed)
        return false;
    gate->suppressed = true;
    return true;
}

bool message_hover_scroll_gate_pointer(MessageHoverScrollGate* gate, float x, float y) noexcept {
    if (!gate)
        return false;
    // Windows repeats WM_MOUSEMOVE at an unchanged position (activation,
    // capture changes); only a real move ends the scroll suppression.
    if (!gate->pointer_known || gate->pointer_x != x || gate->pointer_y != y)
        gate->suppressed = false;
    gate->pointer_x = x;
    gate->pointer_y = y;
    gate->pointer_known = true;
    return !gate->suppressed;
}

bool message_action_bar_set_hover(MessageActionBar* bar, float x, float y, bool* changed) noexcept {
    if (!bar || !changed) {
        return false;
    }
    const MessageActionBarHit hit = message_action_bar_hit_test(*bar, x, y);
    const u32 next = hit.action != MessageActionBarAction::none ? hit.item_index : kMessageActionBarNoItem;
    *changed = bar->hovered_index != next;
    if (*changed) {
        bar->hovered_index = next;
        saturating_increment(bar->revision);
    }
    return true;
}

MessageActionBarPointerResult message_action_bar_track_pointer(
    MessageActionBar* bar, MessageScene* scene, float x, float y,
    const ReactionPreferenceStore* preferences, MessageActionBarForwardPolicy forward_policy, void* policy_context) noexcept {
    ForwardPolicyAdapter adapter{forward_policy, policy_context};
    return message_action_bar_track_pointer(bar, scene, x, y, preferences,
                                            forward_policy ? adapt_forward_policy : MessageActionBarPolicy{},
                                            &adapter);
}

MessageActionBarPointerResult message_action_bar_track_pointer(
    MessageActionBar* bar, MessageScene* scene, float x, float y, const ReactionPreferenceStore* preferences,
    MessageActionBarPolicy policy, void* policy_context) noexcept {
    if (!bar || !scene || !scene->active || !scene->timeline || !scene->timeline->store) {
        return MessageActionBarPointerResult{{}, 0, MessageActionBarError::invalid_input, false};
    }
    const UiWidgetHandle prior = scene->widgets.hovered;
    const u32 prior_bar_revision = bar->revision;
    MessageActionBarHit retained = message_action_bar_hit_test(*bar, x, y);
    if (retained.inside && policy) {
        // Authority (and Shift expansion) may change while the pointer stays inside.
        const u16 capabilities = policy(policy_context, bar->message_id);
        const auto found = message_store_find(*scene->timeline->store, bar->message_id);
        const auto* record = found.ok() ? message_store_resolve(*scene->timeline->store, found.handle) : nullptr;
        if (!record || effective_capabilities(*record, capabilities) != bar->capabilities) {
            message_action_bar_reconfigure(bar, *scene, preferences, MessageActionBarOptions{capabilities});
            retained = message_action_bar_hit_test(*bar, x, y);
        }
    }
    MessageSceneHit message{};
    UiWidgetHandle next = ui_widget_invalid_handle();
    bool hover_changed = false;
    if (retained.inside) {
        next = bar->owner.handle;
        if (!message_action_bar_set_hover(bar, x, y, &hover_changed)) {
            return MessageActionBarPointerResult{retained, retained.message_id,
                                                 MessageActionBarError::scene, false};
        }
    } else {
        message = message_scene_hit_test(*scene, x, y);
        next = message.message_id != 0 ? message.widget : ui_widget_invalid_handle();
        const MessageStoreResult found =
            message.message_id != 0 ? message_store_find(*scene->timeline->store, message.message_id)
                                    : MessageStoreResult{};
        const MessageRecord* record =
            found.ok() ? message_store_resolve(*scene->timeline->store, found.handle) : nullptr;
        if (record && supported_delivery(*record)) {
            const MessageActionBarResult opened = message_action_bar_open(
                bar, *scene, message, *record, preferences,
                MessageActionBarOptions{policy ? policy(policy_context, record->id) : u16{0}});
            if (!opened.ok()) {
                message_action_bar_close(bar);
            } else if (!message_action_bar_set_hover(bar, x, y, &hover_changed)) {
                return MessageActionBarPointerResult{
                    {}, message.message_id, MessageActionBarError::scene, false};
            }
        } else {
            message_action_bar_close(bar);
        }
    }
    if (!ui_widget_manager_hover(&scene->widgets, next)) {
        return MessageActionBarPointerResult{{}, message.message_id, MessageActionBarError::scene, false};
    }
    const MessageActionBarHit action = message_action_bar_hit_test(*bar, x, y);
    return MessageActionBarPointerResult{action, retained.inside ? retained.message_id : message.message_id,
                                         MessageActionBarError::none,
                                         prior.slot != next.slot || prior.generation != next.generation ||
                                             hover_changed || prior_bar_revision != bar->revision};
}

} // namespace maicord
