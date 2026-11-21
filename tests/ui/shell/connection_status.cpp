// The caption's connection indicator: each state transition (a short loss shows nothing, Connecting, the countdown,
// Connected and its end, Connection failed, Disconnected), its wakes (none while connected), its lines, and the caption
// that draws it: the title it replaces, the spinner and the tinted marks.
#include "maicord/ui/shell/connection_status.hpp"
#include "maicord/ui/shell/caption_bar.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/ui/widgets/spinner.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/core/asset.hpp"
#include "../../support/test_output.hpp"

namespace maicord::connection_status_test {
namespace {
u32 failures{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}

using Link = UiConnectionLink;
using Status = UiConnectionStatus;

bool says(const UiConnectionIndicator& c, Utf8View expected) noexcept {
    char text[kUiConnectionTextBytes];
    return utf8_equal(ui_connection_indicator_text(c, {text, sizeof(text)}), expected);
}

void transitions() noexcept {
    UiConnectionIndicator c{};
    check(c.shown == Status::hidden && !ui_connection_indicator_next_wake(c) && !ui_connection_indicator_tick(&c, 5000) &&
              says(c, {}),
          "connection: a live session shows nothing and never wakes");

    // A loss inside the grace shows nothing, and coming back confirms nothing.
    check(!ui_connection_indicator_set(&c, Link::down, 0, 10'000) && c.shown == Status::hidden &&
              ui_connection_indicator_next_wake(c) == 10'000 + kUiConnectionGraceMs &&
              !ui_connection_indicator_tick(&c, 10'999),
          "connection: a lost session waits for the grace before it shows");
    check(!ui_connection_indicator_set(&c, Link::up, 0, 10'500) && c.shown == Status::hidden &&
              !ui_connection_indicator_next_wake(c),
          "connection: a session back within the grace shows nothing, not even Connected");

    // Down past the grace with an attempt running: Connecting, with no wake of its own.
    ui_connection_indicator_set(&c, Link::down, 0, 20'000);
    check(ui_connection_indicator_tick(&c, 21'000) && c.shown == Status::connecting &&
              says(c, utf8_literal("Connecting\xE2\x80\xA6")) && !ui_connection_indicator_next_wake(c),
          "connection: an attempt past the grace shows Connecting and only the spinner animates");

    // The worker waits 12.5 s for the next attempt: the countdown steps at each whole second, then Connecting.
    check(ui_connection_indicator_set(&c, Link::down, 33'500, 21'000) && c.shown == Status::disconnected &&
              c.seconds == 13 && says(c, utf8_literal("Disconnected \xC2\xB7 retrying in 13s")) &&
              ui_connection_indicator_next_wake(c) == 21'500,
          "connection: a scheduled retry shows Disconnected with its countdown");
    check(!ui_connection_indicator_tick(&c, 21'499) && ui_connection_indicator_tick(&c, 21'500) && c.seconds == 12 &&
              !ui_connection_indicator_tick(&c, 21'600) && ui_connection_indicator_next_wake(c) == 22'500,
          "connection: the countdown changes once per second");
    u32 steps = 0;
    for (u64 now = 22'500; c.shown == Status::disconnected && steps < 20; now = ui_connection_indicator_next_wake(c))
        steps += ui_connection_indicator_tick(&c, now);
    check(steps == 12 && c.shown == Status::connecting && c.shown_ms == 33'500,
          "connection: twelve more steps, then Connecting at the retry time");

    // READY: Connected for the confirmation, then the title returns.
    check(ui_connection_indicator_set(&c, Link::up, 0, 34'000) && c.shown == Status::connected &&
              says(c, utf8_literal("Connected")) &&
              ui_connection_indicator_next_wake(c) == 34'000 + kUiConnectionConfirmMs,
          "connection: a session that came back shows Connected");
    check(!ui_connection_indicator_tick(&c, 35'999) && ui_connection_indicator_tick(&c, 36'000) &&
              c.shown == Status::hidden && !ui_connection_indicator_next_wake(c),
          "connection: Connected hides after the confirmation and nothing wakes again");

    // A loss during the confirmation: Connected stays until the grace ends, then Connecting.
    ui_connection_indicator_set(&c, Link::down, 0, 40'000);
    ui_connection_indicator_tick(&c, 41'000);
    ui_connection_indicator_set(&c, Link::up, 0, 41'500);
    check(!ui_connection_indicator_set(&c, Link::down, 0, 42'000) && c.shown == Status::connected &&
              ui_connection_indicator_next_wake(c) == 43'000 && ui_connection_indicator_tick(&c, 43'000) &&
              c.shown == Status::connecting,
          "connection: a loss while Connected shows waits for its grace");

    // Retries stopped: at once, no wake; the user's Reconnect connects again without a grace.
    check(ui_connection_indicator_set(&c, Link::failed, 0, 50'000) && c.shown == Status::failed &&
              says(c, utf8_literal("Connection failed")) && !ui_connection_indicator_next_wake(c),
          "connection: retries that stopped show Connection failed");
    check(ui_connection_indicator_set(&c, Link::down, 0, 50'100) && c.shown == Status::connecting,
          "connection: Reconnect after a failure shows Connecting at once");
    check(ui_connection_indicator_set(&c, Link::stopped, 0, 50'200) && c.shown == Status::stopped &&
              says(c, utf8_literal("Disconnected")) && !ui_connection_indicator_next_wake(c),
          "connection: a session that needs the user shows Disconnected without a countdown");

    // A failure straight from a live session shows at once too.
    UiConnectionIndicator live{};
    check(ui_connection_indicator_set(&live, Link::failed, 0, 1000) && live.shown == Status::failed,
          "connection: a live session that fails shows Connection failed without the grace");
}

void lines() noexcept {
    UiConnectionIndicator c{};
    ui_connection_indicator_set(&c, Link::down, 0, 1000);
    ui_connection_indicator_set(&c, Link::down, 2000 + 125'000, 2000);
    check(c.shown == Status::disconnected && c.seconds == 125 &&
              says(c, utf8_literal("Disconnected \xC2\xB7 retrying in 2:05")),
          "connection: a countdown from a minute reads m:ss");
    char small[8];
    check(utf8_equal(ui_connection_indicator_text(c, {small, sizeof(small)}), utf8_literal("Disconnected")),
          "connection: without room for the countdown the state still reads");
}

void caption() noexcept {
    const Theme& theme = kDefaultTheme;
    UiCaptionTitleInput in{};
    in.signed_in = true;
    in.guild_id = 10;
    in.guild_icon_variant = 3;
    in.guild_name = utf8_literal("Guild");
    UiCaptionTitle title = ui_caption_title(in);
    check(utf8_equal(title.text, utf8_literal("Guild")) && title.glyph == UiIcon::guild,
          "connection caption: hidden keeps the server's title");
    in.connection = Status::connecting;
    in.connection_text = utf8_literal("Connecting\xE2\x80\xA6");
    title = ui_caption_title(in);
    check(utf8_equal(title.text, in.connection_text) && title.glyph == UiIcon::sending &&
              !media_asset_key_valid(title.image),
          "connection caption: Connecting takes the title's place with the spinner's slot and no server icon");
    in.connection = Status::connected;
    check(ui_caption_title(in).glyph == UiIcon::circle_check, "connection caption: Connected has a check");
    in.connection = Status::failed;
    check(ui_caption_title(in).glyph == UiIcon::signal_zero, "connection caption: a failure has the no-signal mark");
    in.signed_in = false;
    check(utf8_equal(ui_caption_title(in).text, utf8_literal("MaiCord")),
          "connection caption: signed out, the app's name stays");

    PaintStream paint{};
    if (!paint_stream_initialize(&paint)) {
        check(false, "connection caption: paint stream");
        return;
    }
    // The bar as the renderer lays it out: the mark's box beside the shaped line.
    const auto draw = [&](Status status, UiIcon glyph, u8 phase) noexcept {
        UiCaptionBarState state{};
        state.active = true;
        state.connection = status;
        state.spinner_phase = phase;
        UiCaptionBarLayout layout = ui_caption_bar_layout(theme, 1000, state);
        layout.title_icon = ui_caption_title_place(layout, 90, true, kUiCaptionTitleGlyph).icon;
        layout.title_glyph = glyph;
        return paint_stream_reset(&paint, layout.width, layout.height) &&
               ui_caption_bar_paint(&paint, theme, layout, state, 1) && !paint.rejected;
    };
    const auto icon = [&](UiIcon glyph, Color* color) noexcept {
        u32 found = 0;
        for (u32 i = 0; i < paint.count; ++i)
            if (ui_icon_quad(paint.quads[i]) && ui_icon_quad_data(paint.quads[i]).icon == static_cast<u32>(glyph))
                ++found, *color = paint.quads[i].fill;
        return found;
    };
    const auto same = [](Color a, Color b) noexcept {
        return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
    };
    Color color{};
    check(draw(Status::hidden, UiIcon::guild, 0) && icon(UiIcon::guild, &color) == 1 &&
              same(color, ui_caption_bar_title_color(theme, {UiCaptionPart::none, UiCaptionPart::none, true})),
          "connection caption: the title's glyph keeps the title colour");
    const u32 plain = paint.count;
    u64 first = 0;
    check(draw(Status::connecting, UiIcon::sending, 0) && !icon(UiIcon::sending, &color) &&
              paint.count == plain - 1 + kUiSpinnerSegments,
          "connection caption: Connecting draws a spinner in the mark's place");
    for (u32 i = 0; i < paint.count; ++i)
        first += static_cast<u64>(paint.quads[i].left * 1000 + paint.quads[i].top);
    u64 next = 0;
    check(draw(Status::connecting, UiIcon::sending, 5), "connection caption: a later spinner step draws");
    for (u32 i = 0; i < paint.count; ++i)
        next += static_cast<u64>(paint.quads[i].left * 1000 + paint.quads[i].top);
    check(first != next, "connection caption: the spinner turns with the shared phase");
    check(draw(Status::connected, UiIcon::circle_check, 0) && icon(UiIcon::circle_check, &color) == 1 &&
              same(color, theme.tokens.text.positive),
          "connection caption: Connected's check takes the positive tint");
    check(draw(Status::disconnected, UiIcon::signal_zero, 0) && icon(UiIcon::signal_zero, &color) == 1 &&
              same(color, theme.tokens.text.warning),
          "connection caption: Disconnected's mark takes the warning tint");
    check(draw(Status::failed, UiIcon::signal_zero, 0) && icon(UiIcon::signal_zero, &color) == 1 &&
              same(color, theme.tokens.text.danger),
          "connection caption: Connection failed's mark takes the danger tint");
    paint_stream_release(&paint);
}
} // namespace
} // namespace maicord::connection_status_test

extern "C" __declspec(noreturn) void WINAPI maicord_connection_status_tests_entry() noexcept {
    using namespace maicord::connection_status_test;
    transitions();
    lines();
    caption();
    maicord::test_exit(failures, "connection_status");
}
