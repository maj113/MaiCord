#pragma once
#include "maicord/ui/transition.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord {

// State shared by the hover cards (server profile, stream preview): a root
// widget the card's text hangs from, the spring entrance and the hover and
// press flags of the card's one button. Each card derives from it and adds its
// content and geometry.
struct UiHoverCard {
    UiWidgetManager widgets;
    UiWidget widget_storage[1];
    UiLayoutNode layout_storage[1];
    UiWidgetReference owner;
    UiRect anchor, viewport, settled, panel;
    bool active, hovered, pressed;
    UiMotionTransition entrance;
    u64 entrance_generation, content_hash, identity;
    float progress; // Entrance 0..1.
    u32 revision;
    u8 spinner_phase; // Host frame clock (paint only).
};

// The entrance spring of one card kind.
struct UiHoverCardMotion {
    UiMotionStyle style;
    u64 owner;
};

// Creates the root widget once; a card already initialized keeps it.
bool ui_hover_card_initialize(UiHoverCard& card, Utf8View name) noexcept;
void ui_hover_card_close(UiHoverCard* card) noexcept;
// A new identity: clears hover and press and restarts the entrance.
void ui_hover_card_restart(UiHoverCard* card, const UiHoverCardMotion& motion, u64 now_ms,
                           bool reduced_motion) noexcept;
// Records a re-projected card: the revision changes only with the content hash.
void ui_hover_card_publish(UiHoverCard* card, u64 content_hash) noexcept;
// Steps the entrance; true when the progress moved and the caller re-lays out
// (then bumps the revision).
bool ui_hover_card_advance(UiHoverCard* card, const UiHoverCardMotion& motion, u64 now_ms,
                           bool reduced_motion) noexcept;
u64 ui_hover_card_next_wake(const UiHoverCard& card, const UiHoverCardMotion& motion, u64 now_ms,
                            bool reduced_motion) noexcept;
// The settled card, so a pointer moving onto it during the slide counts.
bool ui_hover_card_contains(const UiHoverCard& card, float x, float y) noexcept;
// True when the flag changed.
bool ui_hover_card_set_hovered(UiHoverCard* card, bool hovered) noexcept;
bool ui_hover_card_set_pressed(UiHoverCard* card, bool pressed) noexcept;

} // namespace maicord
