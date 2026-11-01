#include "call_stage.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/ui/theme/surface_elevation.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/model/voice/stage_policy.hpp"

namespace maicord::call_stage {
u32 copy_text(char* destination, u32 capacity, Utf8View text) noexcept {
    return text.data ? utf8_copy_truncated(destination, capacity, text) : 0;
}
UiRect centered(float x, float y, float width, float height) noexcept {
    return {x - width * .5f, y - height * .5f, x + width * .5f, y + height * .5f};
}
namespace {
constexpr float kPad = 12, kAvatar = 40, kAvatarStep = 48, kRowAvatar = 28, kInviteWidth = 72, kButtonHeight = 32,
                kAcceptWidth = 96, kStartWidth = 104, kEndWidth = 92, kIconButton = 32;

u8 format(char* output, u32 capacity, Utf8View prefix, u32 value) noexcept {
    u32 at = copy_text(output, capacity, prefix);
    char digits[10];
    const u32 count = decimal_write(digits, value);
    for (u32 i = 0; i < count && at < capacity; ++i)
        output[at++] = digits[i];
    return static_cast<u8>(at);
}
inline UiRect control(const UiCallScene& s, UiCallControl c) noexcept {
    return s.controls[static_cast<u32>(c)];
}
// Audience slots that fit the band; the last one turns into "+K" on overflow.
inline u32 audience_slots(const UiCallScene& s) noexcept {
    if (!ui_rect_has_area(s.stage_audience))
        return 0;
    const float width = ui_rect_width(s.stage_audience) - 2 * kPad + (kAvatarStep - kAvatar);
    return width > kAvatarStep ? static_cast<u32>(width / kAvatarStep) : 0;
}
u32 audience_drawn(const UiCallScene& s) noexcept {
    const u32 slots = audience_slots(s);
    const u32 shown = minimum(s.stage_audience_shown, slots);
    return s.stage_audience_count > shown && shown == slots && shown ? shown - 1 : shown;
}
inline bool audience_more(const UiCallScene& s) noexcept {
    return audience_slots(s) && s.stage_audience_count > audience_drawn(s);
}
UiRect audience_slot(const UiCallScene& s, u32 index) noexcept {
    const auto& a = s.stage_audience;
    const float left = a.left + kPad + static_cast<float>(index) * kAvatarStep;
    return {left, a.top + 40, left + kAvatar, a.top + 40 + kAvatar};
}
inline UiRect audience_label(const UiCallScene& s) noexcept {
    const auto& a = s.stage_audience;
    return {a.left + kPad + 24, a.top + 8, a.right - kPad, a.top + 30};
}
u32 request_rows(const UiCallScene& s) noexcept {
    if (!ui_rect_has_area(s.stage_requests))
        return 0;
    const float rows = (ui_rect_height(s.stage_requests) - 40) / kUiCallStageRowHeight;
    return minimum(s.stage_request_shown, rows > 0 ? static_cast<u32>(rows) : 0u);
}
inline UiRect request_row(const UiCallScene& s, u32 index) noexcept {
    const auto& r = s.stage_requests;
    const float top = r.top + 40 + static_cast<float>(index) * kUiCallStageRowHeight;
    return {r.left, top, r.right, top + kUiCallStageRowHeight};
}
UiRect request_avatar(const UiCallScene& s, u32 index) noexcept {
    const auto row = request_row(s, index);
    const float y = (row.top + row.bottom) * .5f;
    return {row.left + kPad, y - kRowAvatar * .5f, row.left + kPad + kRowAvatar, y + kRowAvatar * .5f};
}
inline UiRect requests_label(const UiCallScene& s) noexcept {
    const auto& r = s.stage_requests;
    return {r.left + kPad + 24, r.top + 8, r.right - kPad, r.top + 32};
}
inline Utf8View person_name(const UiCallStagePerson& person) noexcept {
    return person.name.bytes ? person.name : utf8_literal("Unknown user");
}
} // namespace

void copy(UiCallScene& s, const UiCallStage& stage) noexcept {
    s.stage_active = stage.active;
    s.stage_live = stage.active && stage.live;
    s.stage_hand = stage.active && stage.hand_raised;
    s.stage_actions = stage.active ? stage.actions : 0;
    s.stage_audience_count = stage.active ? stage.audience_count : 0;
    s.stage_request_count = stage.active ? stage.request_count : 0;
    s.stage_audience_shown = stage.active ? minimum<u32>(stage.audience.count, kUiCallStageAudienceShown) : 0;
    s.stage_request_shown = stage.active ? minimum<u32>(stage.requests.count, kUiCallStageRequestsShown) : 0;
    const auto take = [&](u32 slot, const UiCallStagePerson& person) noexcept {
        s.stage_people[slot] = {person.user_id,
                                {s.stage_names[slot], copy_text(s.stage_names[slot], kUiCallNameBytes, person.name)}};
    };
    for (u32 i = 0; i < s.stage_audience_shown; ++i)
        take(i, stage.audience[i]);
    for (u32 i = 0; i < s.stage_request_shown; ++i)
        take(kUiCallStageAudienceShown + i, stage.requests[i]);
    s.stage_topic_bytes = stage.active && stage.live ? copy_text(s.stage_topic, kUiCallStageTopicBytes, stage.topic) : 0;
    s.stage_notice_bytes = stage.active ? copy_text(s.stage_notice, kUiCallStageNoticeBytes, stage.notice) : 0;
    s.stage_text_bytes[0] = format(s.stage_text[0], 32, utf8_literal("Audience \xC2\xB7 "), s.stage_audience_count);
    s.stage_text_bytes[1] = 0;
    s.stage_text_bytes[2] =
        format(s.stage_text[2], 32, utf8_literal("Requests to speak \xC2\xB7 "), s.stage_request_count);
    if (!stage.active)
        s.stage_banner = s.stage_audience = s.stage_requests = s.stage_speakers = {};
}

UiRect layout(UiCallScene& s, UiRect area, float gap) noexcept {
    s.stage_banner = s.stage_audience = s.stage_requests = {};
    if (!s.stage_active) {
        reset_value(&s.stage_speakers);
        return area;
    }
    if (s.stage_actions & stage_action_accept_invite && ui_rect_height(area) >= kUiCallStageBannerHeight + 160) {
        s.stage_banner = {area.left, area.top, area.right, area.top + kUiCallStageBannerHeight};
        area.top = s.stage_banner.bottom + gap;
    }
    if (ui_rect_height(area) >= kUiCallStageAudienceHeight + 120) {
        s.stage_audience = {area.left, area.bottom - kUiCallStageAudienceHeight, area.right, area.bottom};
        area.bottom = s.stage_audience.top - gap;
    }
    if ((s.stage_actions & stage_action_invite_others) && s.stage_request_count &&
        ui_rect_width(area) >= kUiCallStageRequestsWidth + 320 && ui_rect_height(area) >= 40 + kUiCallStageRowHeight) {
        s.stage_requests = {area.right - kUiCallStageRequestsWidth, area.top, area.right, area.bottom};
        area.right = s.stage_requests.left - gap;
    }
    // "+K" for the audience members without a slot.
    s.stage_text_bytes[1] =
        audience_more(s) ? format(s.stage_text[1], 32, utf8_literal("+"), s.stage_audience_count - audience_drawn(s))
                         : 0;
    s.stage_speakers = area;
    return area;
}

void place_controls(UiCallScene& s) noexcept {
    const UiCallControl stage_controls[]{UiCallControl::stage_start,  UiCallControl::stage_topic,
                                         UiCallControl::stage_end,    UiCallControl::stage_accept,
                                         UiCallControl::stage_decline};
    for (const auto c : stage_controls)
        reset_value(&s.controls[static_cast<u32>(c)]);
    if (!s.stage_active)
        return;
    const float middle = (s.header.top + s.header.bottom) * .5f;
    // Stage actions continue the header toolbar left of close, search and chat.
    float x = s.header.right - kPad;
    const UiRect toolbar[]{control(s, UiCallControl::close), s.search_field, control(s, UiCallControl::chat)};
    for (const UiRect& r : toolbar)
        if (ui_rect_has_area(r))
            x = r.left - 8 < x ? r.left - 8 : x;
    const auto place = [&](UiCallControl c, float width) noexcept {
        if (x - width < s.header.left + 200)
            return;
        s.controls[static_cast<u32>(c)] = {x - width, middle - kButtonHeight * .5f, x, middle + kButtonHeight * .5f};
        x -= width + 8;
    };
    if (s.stage_actions & stage_action_end)
        place(UiCallControl::stage_end, kEndWidth);
    if (s.stage_actions & stage_action_edit_topic)
        place(UiCallControl::stage_topic, kIconButton);
    if (s.stage_actions & stage_action_start)
        place(UiCallControl::stage_start, kStartWidth);
    if (ui_rect_has_area(s.stage_banner)) {
        const auto& b = s.stage_banner;
        const float y = (b.top + b.bottom) * .5f;
        const UiRect accept{b.right - kPad - kAcceptWidth, y - kButtonHeight * .5f, b.right - kPad,
                            y + kButtonHeight * .5f};
        const UiRect decline{accept.left - 8 - kAcceptWidth, accept.top, accept.left - 8, accept.bottom};
        if (decline.left > b.left + 220) {
            s.controls[static_cast<u32>(UiCallControl::stage_accept)] = accept;
            s.controls[static_cast<u32>(UiCallControl::stage_decline)] = decline;
        }
    }
}

float header_left(const UiCallScene& s) noexcept {
    float left = s.header.right;
    const UiCallControl header[]{UiCallControl::stage_start, UiCallControl::stage_topic, UiCallControl::stage_end};
    for (const auto c : header)
        if (ui_rect_has_area(control(s, c)))
            left = minimum(left, control(s, c).left);
    return left;
}

u32 bar_order(const UiCallScene& s, UiCallControl* output, u32 capacity) noexcept {
    u32 count = 0;
    const auto add = [&](UiCallControl c) noexcept {
        if (count < capacity)
            output[count++] = c;
    };
    add(UiCallControl::mute);
    add(UiCallControl::deafen);
    if (s.stage_actions & (stage_action_raise_hand | stage_action_lower_hand))
        add(UiCallControl::stage_hand);
    if (s.stage_actions & stage_action_become_speaker)
        add(UiCallControl::stage_become);
    if (s.stage_actions & stage_action_move_to_audience)
        add(UiCallControl::stage_audience);
    add(UiCallControl::disconnect);
    return count;
}

UiRect invite_button(const UiCallScene& s, u32 request) noexcept {
    if (request >= request_rows(s))
        return {};
    const auto row = request_row(s, request);
    const float y = (row.top + row.bottom) * .5f;
    return {row.right - kPad - kInviteWidth, y - 14, row.right - kPad, y + 14};
}

UiCallHit hit(const UiCallScene& s, float x, float y) noexcept {
    UiCallHit result{};
    for (u32 i = 0; i < request_rows(s); ++i)
        if (ui_rect_contains(invite_button(s, i), x, y)) {
            result.control = UiCallControl::stage_invite;
            result.user_id = s.stage_people[kUiCallStageAudienceShown + i].user_id;
            return result;
        }
    return result;
}

u64 hash(const UiCallScene& s, u64 seed) noexcept {
    u64 h = hash64_mix(seed, (u64{s.stage_active} << 0) | (u64{s.stage_live} << 1) | (u64{s.stage_hand} << 2) |
                                 (u64{s.stage_actions} << 8) | (u64{s.stage_audience_count} << 32));
    h = hash64_mix(h, (u64{s.stage_request_count} << 32) | (u64{s.stage_audience_shown} << 16) |
                          s.stage_request_shown);
    for (u32 i = 0; i < kUiCallStageAudienceShown + kUiCallStageRequestsShown; ++i) {
        const bool used = i < kUiCallStageAudienceShown ? i < s.stage_audience_shown
                                                        : i - kUiCallStageAudienceShown < s.stage_request_shown;
        if (!used)
            continue;
        h = hash64_mix(h, s.stage_people[i].user_id);
        h = hash64_counted_bytes(h, s.stage_names[i], s.stage_people[i].name.bytes);
    }
    h = hash64_counted_bytes(h, s.stage_topic, s.stage_topic_bytes);
    h = hash64_counted_bytes(h, s.stage_notice, s.stage_notice_bytes);
    const UiRect* rects[]{&s.stage_banner, &s.stage_audience, &s.stage_requests, &s.stage_speakers};
    for (const auto* r : rects)
        h = hash64_bytes(h, r, sizeof(*r));
    return h;
}

Utf8View label(const UiCallScene& s, UiCallControl c) noexcept {
    switch (c) {
    case UiCallControl::stage_hand:
        return s.stage_hand ? utf8_literal("Lower hand") : utf8_literal("Request to speak");
    case UiCallControl::stage_become:
        return utf8_literal("Become a speaker");
    case UiCallControl::stage_audience:
    case UiCallControl::stage_move:
        return utf8_literal("Move to audience");
    case UiCallControl::stage_accept:
        return utf8_literal("Accept");
    case UiCallControl::stage_decline:
        return utf8_literal("Decline");
    case UiCallControl::stage_start:
        return utf8_literal("Start stage");
    case UiCallControl::stage_topic:
        return utf8_literal("Edit topic");
    case UiCallControl::stage_end:
        return utf8_literal("End stage");
    case UiCallControl::stage_invite:
        return utf8_literal("Invite to speak");
    default:
        return {};
    }
}

bool paint(PaintStream* out, const UiCallScene& s, const Theme& theme) noexcept {
    if (!s.stage_active)
        return true;
    UiPaint p{out, theme};
    const Color fill = theme.surface;
    const UiRect* panels[]{&s.stage_banner, &s.stage_audience, &s.stage_requests};
    for (const auto* r : panels)
        if (ui_rect_has_area(*r))
            p.use<ui_surface_paint>(*r, theme, UiElevation::card, theme.scale.radius_md, &fill,
                                    &theme.tokens.border.subtle);
    // Avatar placeholders; the host draws pictures over them.
    UiCallTileMedia media{};
    for (u32 i = 0; p && avatar(s, i, &media); ++i)
        p.rounded(media.avatar, theme.avatar_placeholder, ui_rect_width(media.avatar) * .5f);
    if (audience_more(s))
        p.rounded(audience_slot(s, audience_drawn(s)), theme.surface_high, kAvatar * .5f);
    return p.end();
}

bool paint_foreground(PaintStream* out, const UiCallScene& s, const Theme& theme) noexcept {
    if (!s.stage_active)
        return true;
    UiPaint p{out, theme};
    const auto hovered = [&](UiCallControl c) noexcept { return s.hovered == c; };
    const auto pressed = [&](UiCallControl c) noexcept { return s.hovered == c && s.pressed == c; };
    const auto tint = [&](UiCallControl c, const Color& color) noexcept { return shaded(color, hovered(c), pressed(c)); };
    if (const auto r = control(s, UiCallControl::stage_start); ui_rect_has_area(r))
        p.rounded(r, tint(UiCallControl::stage_start, theme.tokens.accent.brand), theme.scale.radius_sm);
    if (const auto r = control(s, UiCallControl::stage_topic); ui_rect_has_area(r)) {
        if (hovered(UiCallControl::stage_topic))
            p.rounded(r, pressed(UiCallControl::stage_topic) ? theme.tokens.state.pressed : theme.tokens.state.hover,
                      theme.scale.radius_sm);
        p.icon(UiIcon::edit, centered((r.left + r.right) * .5f, (r.top + r.bottom) * .5f, 20, 20),
               hovered(UiCallControl::stage_topic) ? theme.text_primary : theme.text_secondary);
    }
    if (const auto r = control(s, UiCallControl::stage_end); ui_rect_has_area(r))
        p.rounded(r, tint(UiCallControl::stage_end, theme.tokens.accent.danger), theme.scale.radius_sm);
    if (ui_rect_has_area(s.stage_banner)) {
        const auto& b = s.stage_banner;
        const float y = (b.top + b.bottom) * .5f;
        p.icon(UiIcon::hand, {b.left + 16, y - 10, b.left + 36, y + 10}, ui_icon_role_color(theme, UiIconRole::accent));
        if (const auto r = control(s, UiCallControl::stage_accept); ui_rect_has_area(r))
            p.rounded(r, tint(UiCallControl::stage_accept, theme.tokens.accent.brand), theme.scale.radius_sm);
        if (const auto r = control(s, UiCallControl::stage_decline); ui_rect_has_area(r))
            p.rounded(r, hovered(UiCallControl::stage_decline) ? theme.interactive : theme.surface_high,
                      theme.scale.radius_sm);
    }
    if (ui_rect_has_area(s.stage_audience)) {
        const auto l = audience_label(s);
        const float y = (l.top + l.bottom) * .5f;
        p.icon(UiIcon::members, {l.left - 22, y - 8, l.left - 6, y + 8}, theme.text_secondary);
    }
    if (ui_rect_has_area(s.stage_requests)) {
        const auto l = requests_label(s);
        const float y = (l.top + l.bottom) * .5f;
        p.icon(UiIcon::hand, {l.left - 22, y - 8, l.left - 6, y + 8}, theme.text_secondary).clip(s.stage_requests);
        for (u32 i = 0; p && i < request_rows(s); ++i) {
            const bool hot = s.hovered == UiCallControl::stage_invite &&
                             s.stage_hovered == s.stage_people[kUiCallStageAudienceShown + i].user_id;
            p.rounded(invite_button(s, i), shaded(theme.tokens.accent.brand, hot, hot && s.pressed == UiCallControl::stage_invite),
                      theme.scale.radius_sm);
        }
        p.unclip();
    }
    return p.end();
}

void text(UiTextPen<UiTextClip::label>& pen, const UiCallScene& s, const Theme& theme) noexcept {
    if (!s.stage_active)
        return;
    const auto owner = s.owner.handle;
    const auto on_accent = theme.tokens.text.on_accent;
    const u32 button_style = text_style_subtext | text_style_bold;
    const auto button = [&](UiCallControl c, const Color& color) noexcept {
        const auto r = control(s, c);
        if (ui_rect_has_area(r))
            pen.in(r)(owner, label(s, c), r, color, button_style, ui_text_center);
    };
    button(UiCallControl::stage_start, on_accent);
    button(UiCallControl::stage_end, on_accent);
    button(UiCallControl::stage_accept, on_accent);
    button(UiCallControl::stage_decline, theme.text_primary);
    if (ui_rect_has_area(s.stage_banner)) {
        const auto& b = s.stage_banner;
        const auto decline = control(s, UiCallControl::stage_decline);
        const float right = ui_rect_has_area(decline) ? decline.left - 12 : b.right - kPad;
        pen.in(b)(owner, utf8_literal("You're invited to speak"), {b.left + 48, b.top, right, b.bottom},
                  theme.tokens.text.normal, text_style_bold);
    }
    if (ui_rect_has_area(s.stage_audience)) {
        const auto& a = s.stage_audience;
        auto line = pen.in(a);
        line(owner, {s.stage_text[0], s.stage_text_bytes[0]}, audience_label(s), theme.tokens.text.muted,
             text_style_subtext | text_style_bold);
        if (s.stage_text_bytes[1])
            line(owner, {s.stage_text[1], s.stage_text_bytes[1]}, audience_slot(s, audience_drawn(s)),
                 theme.tokens.text.normal, text_style_subtext | text_style_bold, ui_text_center);
        if (!s.stage_audience_count)
            line(owner, utf8_literal("No one is listening yet"), {a.left + kPad, a.top + 40, a.right - kPad, a.top + 80},
                 theme.tokens.text.muted, text_style_subtext);
    }
    if (ui_rect_has_area(s.stage_requests)) {
        const auto& r = s.stage_requests;
        auto line = pen.in(r);
        line(owner, {s.stage_text[2], s.stage_text_bytes[2]}, requests_label(s), theme.tokens.text.muted,
             text_style_subtext | text_style_bold);
        for (u32 i = 0; pen.ok() && i < request_rows(s); ++i) {
            const auto row = request_row(s, i);
            const auto invite = invite_button(s, i);
            line(owner, person_name(s.stage_people[kUiCallStageAudienceShown + i]),
                 {row.left + kPad + kRowAvatar + 8, row.top, invite.left - 8, row.bottom}, theme.tokens.text.normal);
            pen.in(ui_rect_intersection(r, invite))(owner, utf8_literal("Invite"), invite, on_accent, button_style,
                                                    ui_text_center);
        }
    }
    // No speakers yet: say so where the tiles would be.
    if (!s.participant_count && ui_rect_has_area(s.stage_speakers))
        pen.in(s.stage_speakers)(owner,
                                 s.stage_live ? utf8_literal("No one is speaking yet")
                                              : utf8_literal("This stage hasn't started yet"),
                                 s.stage_speakers, theme.tokens.text.muted, 0, ui_text_center);
}

bool avatar(const UiCallScene& s, u32 index, UiCallTileMedia* output) noexcept {
    if (!output || !s.active || !s.stage_active)
        return false;
    const u32 audience = audience_drawn(s);
    UiRect circle{}, clip{};
    u64 user = 0;
    if (index < audience) {
        circle = audience_slot(s, index);
        clip = s.stage_audience;
        user = s.stage_people[index].user_id;
    } else if (index - audience < request_rows(s)) {
        const u32 row = index - audience;
        circle = request_avatar(s, row);
        clip = s.stage_requests;
        user = s.stage_people[kUiCallStageAudienceShown + row].user_id;
    } else
        return false;
    *output = {user, 0, {}, clip, circle, circle, ui_rect_width(circle) * .5f, {}, false};
    return true;
}
} // namespace maicord::call_stage
