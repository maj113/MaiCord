#include "maicord/ui/activity_status.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
UiActivityGlyph ui_activity_glyph(UserActivityType type) noexcept {
    switch (type) {
    case UserActivityType::playing:
    case UserActivityType::competing:
        return UiActivityGlyph::game;
    case UserActivityType::listening:
        return UiActivityGlyph::music;
    case UserActivityType::watching:
    case UserActivityType::streaming:
        return UiActivityGlyph::screen;
    default:
        return UiActivityGlyph::none;
    }
}
namespace {
// Fills the appearance part; the glyph and activity text are the caller's.
void custom_status(const UserAppearanceRecord* appearance, bool rich, UiActivityStatus* output) noexcept {
    if (!appearance || !(appearance->fields & appearance_activity))
        return;
    // A "Playing X" line stands in only while no rich activity says more.
    if (!(appearance->fields & appearance_custom_status) && rich)
        return;
    output->emoji_id = appearance->status_emoji_id;
    if (!appearance->status_emoji_id)
        output->emoji = appearance->status_emoji.view();
    output->text = appearance->activity.view();
}
bool shown(const UiActivityStatus& s) noexcept {
    return s.text.bytes || s.emoji.bytes || s.emoji_id || s.glyph != UiActivityGlyph::none;
}
} // namespace

bool ui_activity_status(const UserActivity* primary, const UserAppearanceRecord* appearance, bool bot,
                        Slice<char> storage, UiActivityStatus* output) noexcept {
    if (!output)
        return false;
    reset_value(output);
    custom_status(appearance, primary != nullptr, output);
    if (primary) {
        output->glyph = bot ? UiActivityGlyph::none : ui_activity_glyph(primary->type);
        if (!output->text.bytes && storage.data)
            output->text = {storage.data, user_activity_status_text(*primary, storage)};
    }
    return shown(*output);
}
bool ui_user_activity_status(const UserStore& users, u64 user_id, const UserAppearanceRecord* appearance,
                             Slice<char> storage, UiActivityStatus* output) noexcept {
    if (!output)
        return false;
    reset_value(output);
    UserActivityType type{};
    const bool rich = user_activity_first_type(users.activities, user_id, &type);
    custom_status(appearance, rich, output);
    if (!rich)
        return shown(*output);
    const auto* global = user_appearance_find(users.appearance, user_id);
    output->glyph = global && global->bot ? UiActivityGlyph::none : ui_activity_glyph(type);
    if (!output->text.bytes && storage.data) {
        UserActivitySet set;
        if (user_activity_find(users.activities, user_id, &set))
            output->text = {storage.data, user_activity_status_text(set.items[0], storage)};
    }
    return shown(*output);
}
UiIcon ui_activity_glyph_icon(UiActivityGlyph glyph) noexcept {
    switch (glyph) {
    case UiActivityGlyph::game:
        return UiIcon::controller;
    case UiActivityGlyph::music:
        return UiIcon::music;
    case UiActivityGlyph::screen:
        return UiIcon::monitor;
    default:
        return UiIcon::count;
    }
}
UiRect ui_activity_glyph_rectangle(UiRect line) noexcept {
    const float middle = (line.top + line.bottom) * .5f, half = kUiActivityGlyphExtent * .5f;
    return {line.left, middle - half, line.left + kUiActivityGlyphExtent, middle + half};
}
UiRect ui_activity_glyph_text(UiRect line, UiActivityGlyph glyph) noexcept {
    if (glyph != UiActivityGlyph::none)
        line.left = minimum(line.right, line.left + kUiActivityGlyphExtent + kUiActivityGlyphGap);
    return line;
}
} // namespace maicord
