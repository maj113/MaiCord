// Theme token table: the single source for the resolved Theme layout, the
// document schema and tools/generate_theme_example.py. Append within a group;
// names are portable document keys. Defaults reference the tier below:
//   P(core)        a core palette colour (Theme::accent, Theme::base_low, ...)
//   PA(core, a)    that colour with its alpha multiplied by a
//   T(group, name) an earlier semantic token
//   RGBA(r,g,b,a)  a literal colour
// MAICORD_THEME_TOKEN(group, name, dark default, light default)
#ifndef MAICORD_THEME_GROUP
#define MAICORD_THEME_GROUP(group)
#endif
#ifndef MAICORD_THEME_GROUP_END
#define MAICORD_THEME_GROUP_END(group)
#endif
#ifndef MAICORD_THEME_TOKEN
#define MAICORD_THEME_TOKEN(group, name, dark, light)
#endif
#ifndef MAICORD_THEME_SCALE
#define MAICORD_THEME_SCALE(name, value, minimum, maximum)
#endif
#ifndef MAICORD_THEME_MOTION
#define MAICORD_THEME_MOTION(name, duration_ms, curve)
#endif

MAICORD_THEME_GROUP(background)
MAICORD_THEME_TOKEN(background, primary, P(base_low), P(base_low))
MAICORD_THEME_TOKEN(background, secondary, P(base_lower), P(base_lower))
MAICORD_THEME_TOKEN(background, floating, P(base_lower), P(base_lower))
MAICORD_THEME_TOKEN(background, input, P(surface_high), P(surface_high))
MAICORD_THEME_GROUP_END(background)

MAICORD_THEME_GROUP(accent)
MAICORD_THEME_TOKEN(accent, brand, P(accent), P(accent))
MAICORD_THEME_TOKEN(accent, success, RGBA(.14f, .65f, .38f, 1), RGBA(.14f, .65f, .38f, 1))
MAICORD_THEME_TOKEN(accent, warning, RGBA(.94f, .69f, .2f, 1), RGBA(.94f, .69f, .2f, 1))
MAICORD_THEME_TOKEN(accent, danger, P(notification), P(notification))
MAICORD_THEME_GROUP_END(accent)

MAICORD_THEME_GROUP(text)
MAICORD_THEME_TOKEN(text, normal, P(text_primary), P(text_primary))
MAICORD_THEME_TOKEN(text, muted, P(text_secondary), P(text_secondary))
MAICORD_THEME_TOKEN(text, link, P(accent), P(accent))
MAICORD_THEME_TOKEN(text, danger, P(notification), P(notification))
MAICORD_THEME_TOKEN(text, positive, RGBA(.494f, .796f, .58f, 1), RGBA(.18f, .49f, .27f, 1))
MAICORD_THEME_TOKEN(text, warning, T(accent, warning), T(accent, warning))
MAICORD_THEME_TOKEN(text, on_accent, RGBA(1, 1, 1, 1), RGBA(1, 1, 1, 1))
MAICORD_THEME_GROUP_END(text)

MAICORD_THEME_GROUP(border)
MAICORD_THEME_TOKEN(border, subtle, RGBA(1, 1, 1, .08f), RGBA(0, 0, 0, .08f))
MAICORD_THEME_TOKEN(border, strong, RGBA(1, 1, 1, .12f), RGBA(0, 0, 0, .12f))
MAICORD_THEME_TOKEN(border, focus, P(accent), P(accent))
MAICORD_THEME_GROUP_END(border)

MAICORD_THEME_GROUP(state)
MAICORD_THEME_TOKEN(state, hover, RGBA(1, 1, 1, .10f), RGBA(0, 0, 0, .06f))
MAICORD_THEME_TOKEN(state, pressed, RGBA(1, 1, 1, .16f), RGBA(0, 0, 0, .10f))
MAICORD_THEME_GROUP_END(state)

MAICORD_THEME_GROUP(fill)
MAICORD_THEME_TOKEN(fill, subtle, RGBA(1, 1, 1, .08f), RGBA(0, 0, 0, .05f))
MAICORD_THEME_TOKEN(fill, muted, RGBA(1, 1, 1, .10f), RGBA(0, 0, 0, .07f))
MAICORD_THEME_TOKEN(fill, translucent, RGBA(0, 0, 0, .25f), RGBA(0, 0, 0, .25f))
MAICORD_THEME_GROUP_END(fill)

MAICORD_THEME_GROUP(overlay)
MAICORD_THEME_TOKEN(overlay, scrim, RGBA(0, 0, 0, .56f), RGBA(0, 0, 0, .56f))
MAICORD_THEME_TOKEN(overlay, scrim_light, RGBA(0, 0, 0, .5f), RGBA(0, 0, 0, .5f))
MAICORD_THEME_TOKEN(overlay, scrim_strong, RGBA(0, 0, 0, .72f), RGBA(0, 0, 0, .72f))
MAICORD_THEME_TOKEN(overlay, dense, RGBA(0, 0, 0, .68f), RGBA(0, 0, 0, .68f))
MAICORD_THEME_TOKEN(overlay, media, RGBA(0, 0, 0, .92f), RGBA(0, 0, 0, .92f))
MAICORD_THEME_TOKEN(overlay, shadow, RGBA(0, 0, 0, .32f), RGBA(0, 0, 0, .32f))
MAICORD_THEME_TOKEN(overlay, shadow_soft, RGBA(0, 0, 0, .24f), RGBA(0, 0, 0, .24f))
MAICORD_THEME_GROUP_END(overlay)

MAICORD_THEME_GROUP(status)
MAICORD_THEME_TOKEN(status, online, T(accent, success), T(accent, success))
MAICORD_THEME_TOKEN(status, idle, T(accent, warning), T(accent, warning))
MAICORD_THEME_TOKEN(status, dnd, T(accent, danger), T(accent, danger))
MAICORD_THEME_TOKEN(status, offline, P(interactive), P(interactive))
MAICORD_THEME_GROUP_END(status)

MAICORD_THEME_GROUP(mention)
MAICORD_THEME_TOKEN(mention, background, PA(accent, .12f), PA(accent, .12f))
MAICORD_THEME_TOKEN(mention, bar, P(accent), P(accent))
MAICORD_THEME_TOKEN(mention, text, P(accent), P(accent))
MAICORD_THEME_TOKEN(mention, inline_background, PA(accent, .18f), PA(accent, .18f))
MAICORD_THEME_GROUP_END(mention)

MAICORD_THEME_GROUP(syntax)
MAICORD_THEME_TOKEN(syntax, keyword, P(syntax_keyword), P(syntax_keyword))
MAICORD_THEME_TOKEN(syntax, type, P(syntax_type), P(syntax_type))
MAICORD_THEME_TOKEN(syntax, string, P(syntax_string), P(syntax_string))
MAICORD_THEME_TOKEN(syntax, number, P(syntax_number), P(syntax_number))
MAICORD_THEME_TOKEN(syntax, comment, P(syntax_comment), P(syntax_comment))
MAICORD_THEME_TOKEN(syntax, operator_, P(syntax_operator), P(syntax_operator))
MAICORD_THEME_TOKEN(syntax, builtin, P(syntax_builtin), P(syntax_builtin))
MAICORD_THEME_TOKEN(syntax, preprocessor, P(syntax_preprocessor), P(syntax_preprocessor))
MAICORD_THEME_GROUP_END(syntax)

// Accent treatment for interactive emphasis: primary buttons, switches,
// sliders, radios, checkboxes, selection pills (fill/text), focus rings and
// selected outlines (outline) and selected rows/tabs (selected). Flat themes
// keep these defaults; a gradient theme resolves every still-default value
// from its background palette (ui_theme_resolve_emphasis) with WCAG checks.
MAICORD_THEME_GROUP(emphasis)
MAICORD_THEME_TOKEN(emphasis, fill, P(accent), P(accent))
MAICORD_THEME_TOKEN(emphasis, text, T(text, on_accent), T(text, on_accent))
MAICORD_THEME_TOKEN(emphasis, outline, P(accent), P(accent))
MAICORD_THEME_TOKEN(emphasis, selected, PA(accent, .16f), PA(accent, .14f))
MAICORD_THEME_GROUP_END(emphasis)

// ```ansi code block palette (ansi-to-html colors 0-15): SGR 30-37/40-47 use
// 0-7, 90-97/100-107 use the bright 8-15, for foreground and background alike.
MAICORD_THEME_GROUP(ansi)
MAICORD_THEME_TOKEN(ansi, black, RGBA(0, 0, 0, 1), RGBA(0, 0, 0, 1))
MAICORD_THEME_TOKEN(ansi, red, RGBA(.92549f, .388231f, .38039f, 1), RGBA(.905882f, .345098f, .345098f, 1))
MAICORD_THEME_TOKEN(ansi, green, RGBA(.270588f, .639212f, .4f, 1), RGBA(.223533f, .607847f, .364711f, 1))
MAICORD_THEME_TOKEN(ansi, yellow, RGBA(.80784f, .505883f, 0, 1), RGBA(.75294f, .462744f, 0, 1))
MAICORD_THEME_TOKEN(ansi, blue, RGBA(.270591f, .568624f, .925489f, 1), RGBA(.215694f, .537258f, .917646f, 1))
MAICORD_THEME_TOKEN(ansi, magenta, RGBA(.960783f, .286277f, .788233f, 1), RGBA(.894116f, .266664f, .733331f, 1))
MAICORD_THEME_TOKEN(ansi, cyan, RGBA(.015687f, .623534f, .666673f, 1), RGBA(0, .596083f, .63922f, 1))
MAICORD_THEME_TOKEN(ansi, white, RGBA(.713725f, .717646f, .737255f, 1), RGBA(.713725f, .717646f, .737255f, 1))
MAICORD_THEME_TOKEN(ansi, bright_black, RGBA(.43921f, .443132f, .47843f, 1), RGBA(.43921f, .443132f, .47843f, 1))
MAICORD_THEME_TOKEN(ansi, bright_red, RGBA(1, .576469f, .55686f, 1), RGBA(.792154f, .168626f, .215682f, 1))
MAICORD_THEME_TOKEN(ansi, bright_green, RGBA(.129411f, .733329f, .38823f, 1), RGBA(.062744f, .556856f, .30196f, 1))
MAICORD_THEME_TOKEN(ansi, bright_yellow, RGBA(.90588f, .580391f, .09412f, 1), RGBA(.85882f, .431371f, 0, 1))
MAICORD_THEME_TOKEN(ansi, bright_blue, RGBA(.627448f, .662742f, .980392f, 1), RGBA(.345099f, .39608f, .949021f, 1))
MAICORD_THEME_TOKEN(ansi, bright_magenta, RGBA(.992157f, .600003f, .913726f, 1), RGBA(.701961f, .149019f, .611767f, 1))
MAICORD_THEME_TOKEN(ansi, bright_cyan, RGBA(.372548f, .749017f, .784312f, 1), RGBA(0, .466662f, .5098f, 1))
MAICORD_THEME_TOKEN(ansi, bright_white, RGBA(1, 1, 1, 1), RGBA(1, 1, 1, 1))
MAICORD_THEME_GROUP_END(ansi)

// User profile cards (popout, full profile, editor preview). A Nitro-themed
// card takes the column of its own profile theme (darkness of the primary colour).
// overlay is the 60% veil over the theme gradient; surface and modal are the
// unthemed card and full-profile backgrounds; banner is the colourless
// banner; control is the circular header button over the banner.
MAICORD_THEME_GROUP(profile)
MAICORD_THEME_TOKEN(profile, surface, P(base_lower), P(base_lower))
MAICORD_THEME_TOKEN(profile, modal, P(base_lower), P(base_lower))
MAICORD_THEME_TOKEN(profile, overlay, RGBA(0, 0, 0, .6f), RGBA(1, 1, 1, .6f))
MAICORD_THEME_TOKEN(profile, text, RGBA(1, 1, 1, .96f), RGBA(0, 0, 0, .96f))
MAICORD_THEME_TOKEN(profile, text_subtle, RGBA(1, 1, 1, .88f), RGBA(0, 0, 0, .88f))
MAICORD_THEME_TOKEN(profile, text_muted, RGBA(1, 1, 1, .76f), RGBA(0, 0, 0, .76f))
MAICORD_THEME_TOKEN(profile, border, RGBA(1, 1, 1, .08f), RGBA(0, 0, 0, .08f))
MAICORD_THEME_TOKEN(profile, hover, RGBA(1, 1, 1, .04f), RGBA(0, 0, 0, .08f))
MAICORD_THEME_TOKEN(profile, card, RGBA(.594f, .594f, .626f, .16f), RGBA(1, 1, 1, .24f))
MAICORD_THEME_TOKEN(profile, card_hover, RGBA(.594f, .594f, .626f, .2f), RGBA(1, 1, 1, .36f))
MAICORD_THEME_TOKEN(profile, input, RGBA(1, 1, 1, .12f), RGBA(0, 0, 0, .08f))
MAICORD_THEME_TOKEN(profile, button, RGBA(1, 1, 1, .08f), RGBA(1, 1, 1, .72f))
MAICORD_THEME_TOKEN(profile, button_hover, RGBA(1, 1, 1, .16f), RGBA(1, 1, 1, .92f))
MAICORD_THEME_TOKEN(profile, banner, RGBA(.067f, .071f, .078f, 1), RGBA(.067f, .071f, .078f, 1))
MAICORD_THEME_TOKEN(profile, control, RGBA(0, 0, 0, .56f), RGBA(0, 0, 0, .56f))
MAICORD_THEME_TOKEN(profile, control_border, RGBA(1, 1, 1, .08f), RGBA(1, 1, 1, .08f))
MAICORD_THEME_TOKEN(profile, control_icon, RGBA(1, 1, 1, 1), RGBA(1, 1, 1, 1))
MAICORD_THEME_GROUP_END(profile)

// The window caption (ui/caption_bar.hpp): drag band, title and the window
// buttons. The band is the shell's backdrop (background is its flat base);
// hover and pressed tint over it. Inactive ink applies while another window
// is active.
MAICORD_THEME_GROUP(caption)
MAICORD_THEME_TOKEN(caption, background, P(base_lowest), P(base_lowest))
MAICORD_THEME_TOKEN(caption, text, T(text, normal), T(text, normal))
MAICORD_THEME_TOKEN(caption, text_inactive, T(text, muted), T(text, muted))
MAICORD_THEME_TOKEN(caption, icon, T(text, muted), T(text, muted))
MAICORD_THEME_TOKEN(caption, icon_inactive, PA(text_secondary, .6f), PA(text_secondary, .6f))
MAICORD_THEME_TOKEN(caption, icon_hover, T(text, normal), T(text, normal))
MAICORD_THEME_TOKEN(caption, hover, T(state, hover), T(state, hover))
MAICORD_THEME_TOKEN(caption, pressed, T(state, pressed), T(state, pressed))
MAICORD_THEME_TOKEN(caption, close_hover, T(accent, danger), T(accent, danger))
MAICORD_THEME_TOKEN(caption, close_pressed, PA(notification, .8f), PA(notification, .8f))
MAICORD_THEME_TOKEN(caption, close_icon, T(text, on_accent), T(text, on_accent))
// The update badge: download progress on its track, and the ready state.
MAICORD_THEME_TOKEN(caption, badge_track, T(fill, muted), T(fill, muted))
MAICORD_THEME_TOKEN(caption, badge_progress, T(accent, brand), T(accent, brand))
MAICORD_THEME_TOKEN(caption, badge_ready, T(accent, success), T(accent, success))
MAICORD_THEME_TOKEN(caption, badge_ready_fill, RGBA(.14f, .65f, .38f, .16f), RGBA(.14f, .65f, .38f, .14f))
MAICORD_THEME_GROUP_END(caption)

// Scales in DIPs: MAICORD_THEME_SCALE(name, default, minimum, maximum).
MAICORD_THEME_SCALE(space_1, 2, 0, 8)
MAICORD_THEME_SCALE(space_2, 4, 0, 12)
MAICORD_THEME_SCALE(space_4, 8, 0, 20)
MAICORD_THEME_SCALE(radius_xs, 2, 0, 8)
MAICORD_THEME_SCALE(radius_sm, 4, 0, 12)
MAICORD_THEME_SCALE(radius_md, 8, 0, 20)
MAICORD_THEME_SCALE(radius_lg, 12, 0, 28)
MAICORD_THEME_SCALE(radius_xl, 16, 0, 36)
MAICORD_THEME_SCALE(radius_full, 9999, 0, 9999)
// Window caption: band height, button width, window-button glyph box, app
// icon and item icon size, the inset of button fills from the band edges and
// the leading/trailing padding. window_radius
// is the top-level window's corner radius (Windows 11 rounds it by 8 DIPs);
// caption fills nest inside it.
MAICORD_THEME_SCALE(caption_height, 32, 24, 48)
MAICORD_THEME_SCALE(caption_button_width, 40, 24, 64)
MAICORD_THEME_SCALE(caption_glyph, 10, 8, 16)
MAICORD_THEME_SCALE(caption_icon, 16, 12, 24)
MAICORD_THEME_SCALE(caption_button_inset, 2, 0, 8)
MAICORD_THEME_SCALE(caption_padding, 12, 0, 32)
MAICORD_THEME_SCALE(window_radius, 8, 0, 16)

// Motion: MAICORD_THEME_MOTION(name, duration ms, UiMotionCurve). Reduced
// motion always settles immediately regardless of these durations.
MAICORD_THEME_MOTION(fast, 120, ease_out)

#undef MAICORD_THEME_GROUP
#undef MAICORD_THEME_GROUP_END
#undef MAICORD_THEME_TOKEN
#undef MAICORD_THEME_SCALE
#undef MAICORD_THEME_MOTION
