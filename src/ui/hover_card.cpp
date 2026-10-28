#include "maicord/ui/hover_card.hpp"

namespace maicord {
namespace {
UiMotionScope scope(const UiHoverCard& card, const UiHoverCardMotion& motion) noexcept {
    return {motion.owner, card.entrance_generation};
}
} // namespace

bool ui_hover_card_initialize(UiHoverCard& card, Utf8View name) noexcept {
    return card.owner.manager == &card.widgets ||
           ui_widget_manager_initialize_fill_root(&card.widgets, {card.widget_storage, 1}, {card.layout_storage, 1},
                                                  name, ui_layout_node_visible | ui_layout_node_allow_overlap,
                                                  &card.owner);
}

void ui_hover_card_close(UiHoverCard* card) noexcept {
    if (!card || !card->active)
        return;
    card->active = card->hovered = card->pressed = false;
    card->identity = 0;
    ++card->entrance_generation;
    ++card->revision;
}

void ui_hover_card_restart(UiHoverCard* card, const UiHoverCardMotion& motion, u64 now_ms,
                           bool reduced_motion) noexcept {
    card->hovered = card->pressed = false;
    ++card->entrance_generation;
    ui_motion_transition_reset(&card->entrance, scope(*card, motion), reduced_motion ? 1.f : 0.f);
    ui_motion_transition_tick(&card->entrance, scope(*card, motion), 1, now_ms, motion.style, reduced_motion, true);
}

void ui_hover_card_publish(UiHoverCard* card, u64 content_hash) noexcept {
    if (!card->active || content_hash != card->content_hash) {
        card->content_hash = content_hash;
        ++card->revision;
    }
    card->active = true;
}

bool ui_hover_card_advance(UiHoverCard* card, const UiHoverCardMotion& motion, u64 now_ms,
                           bool reduced_motion) noexcept {
    if (!card || !card->active || (!card->entrance.active && card->entrance.value >= 1))
        return false;
    if (!ui_motion_transition_tick(&card->entrance, scope(*card, motion), 1, now_ms, motion.style, reduced_motion,
                                   true))
        return false;
    card->progress = card->entrance.value;
    return true;
}

u64 ui_hover_card_next_wake(const UiHoverCard& card, const UiHoverCardMotion& motion, u64 now_ms,
                            bool reduced_motion) noexcept {
    return card.active
               ? ui_motion_transition_next_wake(card.entrance, scope(card, motion), now_ms, reduced_motion, true)
               : 0;
}

bool ui_hover_card_contains(const UiHoverCard& card, float x, float y) noexcept {
    return card.active && (ui_rect_contains(card.settled, x, y) || ui_rect_contains(card.panel, x, y));
}

bool ui_hover_card_set_hovered(UiHoverCard* card, bool hovered) noexcept {
    if (!card || hovered == card->hovered)
        return false;
    card->hovered = hovered;
    ++card->revision;
    return true;
}

bool ui_hover_card_set_pressed(UiHoverCard* card, bool pressed) noexcept {
    if (!card || card->pressed == pressed)
        return false;
    card->pressed = pressed;
    ++card->revision;
    return true;
}

} // namespace maicord
