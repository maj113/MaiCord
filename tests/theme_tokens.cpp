#include "maicord/app/client_theme_storage.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/ui/edit_field.hpp"
#include "maicord/ui/theme_styles.hpp"
#include "maicord/ui/theme_presets.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
u32 scratch[8192];
char exported[kClientThemeDocumentMaximumBytes];
ClientThemeDocumentResult parse(Utf8View input, ClientThemeDocument* output,
                                ClientThemeBase base = ClientThemeBase::dark) noexcept {
    return client_theme_document_parse(input, client_theme_document_defaults(base), {scratch, 8192}, output);
}
bool same(Color a, Color b) noexcept {
    return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}
bool approx(Color a, Color b) noexcept {
    const auto close = [](float x, float y) noexcept { return x - y < .002f && y - x < .002f; };
    return close(a.red, b.red) && close(a.green, b.green) && close(a.blue, b.blue) && close(a.alpha, b.alpha);
}
bool text_equal_bytes(const char* data, u32 bytes, Utf8View text) noexcept {
    return bytes == text.bytes && bytes_equal(data, text.data, bytes);
}
bool contains_fill(const PaintStream& paint, Color color) noexcept {
    for (u32 i = 0; i < paint.count; ++i)
        if (same(paint.quads[i].fill, color) || same(paint.quads[i].border, color))
            return true;
    return false;
}
} // namespace
void test_theme_tokens(void (*check)(bool, const char*) noexcept) noexcept {
    const Theme amoled = ui_theme_amoled();
    constexpr Theme amoled_preset = ui_theme_amoled_preset();
    const Theme* shipped[]{&kDefaultTheme, &kLightTheme, &amoled};
    // The derived AMOLED theme is the constexpr preset.
    bool constexpr_matches = ui_theme_palette_equal(amoled, amoled_preset);
    for (const auto* theme : shipped) {
        Theme runtime = *theme;
        constexpr_matches = constexpr_matches && ui_theme_tokens_resolve(&runtime, nullptr).ok() &&
                            bytes_equal(&runtime.tokens, &theme->tokens, sizeof(UiThemeTokens)) &&
                            ui_theme_palette_valid(*theme);
    }
    check(constexpr_matches && ui_theme_token_descriptors().count == static_cast<u32>(UiThemeToken::count),
          "theme tokens: constexpr built-in resolution equals the runtime resolver for every shipped theme");

    PaintStream paint{};
    bool presence = paint_stream_initialize(&paint);
    const UiPresence states[]{UiPresence::online, UiPresence::idle, UiPresence::busy, UiPresence::offline};
    const Color expected[]{kDefaultTheme.tokens.status.online, kDefaultTheme.tokens.status.idle,
                           kDefaultTheme.tokens.status.dnd, kDefaultTheme.tokens.status.offline};
    for (u32 i = 0; presence && i < 4; ++i)
        presence = paint_stream_reset(&paint) &&
                   ui_presence_paint(&paint, {0, 0, 10, 10}, states[i], kDefaultTheme.base_lower, kDefaultTheme) &&
                   contains_fill(paint, expected[i]);
    Theme custom = kDefaultTheme;
    custom.tokens.status.online = {0, 1, 0, 1};
    presence = presence && paint_stream_reset(&paint) &&
               ui_presence_paint(&paint, {0, 0, 10, 10}, UiPresence::online, custom.base_lower, custom) &&
               contains_fill(paint, {0, 1, 0, 1}) &&
               !same(kDefaultTheme.tokens.status.online, kDefaultTheme.syntax_string);
    UiEditView view{};
    view.active = true;
    presence = presence && paint_stream_reset(&paint) &&
               ui_edit_field_paint(&paint, {0, 0, 100, 30}, {4, 4, 96, 26}, view, nullptr, kDefaultTheme.base_low,
                                   kDefaultTheme, 96, 8) &&
               contains_fill(paint, kDefaultTheme.tokens.border.strong);
    // The official mobile status at a 32 px avatar (status 10, ring 3): a 10x15
    // phone at (22,17), a 16x21 avatar hole at (19,14) with 4.2 corners, a
    // 7.5 px screen at (23.25,19.5) and a 1.25 px button centred at (27,29.5).
    const auto phone = ui_presence_mobile_geometry({22, 22, 32, 32}, 3);
    const auto close_to = [](float a, float b) noexcept { return a - b < .001f && b - a < .001f; };
    const bool geometry =
        close_to(phone.body.left, 22) && close_to(phone.body.top, 17) && close_to(phone.body.right, 32) &&
        close_to(phone.body.bottom, 32) && close_to(phone.body_radius, 1.875f) && close_to(phone.cutout.left, 19) &&
        close_to(phone.cutout.top, 14) && close_to(phone.cutout.right, 35) && close_to(phone.cutout.bottom, 35) &&
        close_to(phone.cutout_radius, 4.2f) && close_to(phone.screen.left, 23.25f) && close_to(phone.screen.top, 19.5f) &&
        close_to(ui_rect_width(phone.screen), 7.5f) && close_to(ui_rect_height(phone.screen), 7.5f) &&
        close_to((phone.button.left + phone.button.right) * .5f, 27) &&
        close_to((phone.button.top + phone.button.bottom) * .5f, 29.5f) && close_to(ui_rect_width(phone.button), 2.5f);
    check(geometry, "presence: the mobile status matches official phone mask geometry at a 32 px avatar");
    const u8 mobile_only = client_status_with(0, ClientPlatform::mobile, ClientPlatformStatus::online);
    const u8 both = client_status_with(mobile_only, ClientPlatform::desktop, ClientPlatformStatus::online);
    const u8 desktop_idle = client_status_with(mobile_only, ClientPlatform::desktop, ClientPlatformStatus::idle);
    const u8 web = client_status_with(mobile_only, ClientPlatform::web, ClientPlatformStatus::online);
    check(ui_presence_mobile(UiPresence::online, mobile_only) && ui_presence_mobile(UiPresence::online, desktop_idle) &&
              ui_presence_mobile(UiPresence::online, web) && !ui_presence_mobile(UiPresence::online, both) &&
              !ui_presence_mobile(UiPresence::idle, mobile_only) && !ui_presence_mobile(UiPresence::busy, mobile_only) &&
              !ui_presence_mobile(UiPresence::online, 0),
          "presence: the phone shows only when online, on mobile and not online on desktop (web ignored)");
    bool phone_paint = paint_stream_reset(&paint) &&
                       ui_presence_paint(&paint, {22, 22, 32, 32}, UiPresence::online, kDefaultTheme.base_lower,
                                         kDefaultTheme, true) &&
                       paint.count == 4;
    const auto quad = [&](u32 i) noexcept { return paint.quads[i]; };
    phone_paint = phone_paint && same(quad(0).fill, kDefaultTheme.base_lower) && close_to(quad(0).top, 15) &&
                  same(quad(1).fill, kDefaultTheme.tokens.status.online) && close_to(quad(1).top, 17) &&
                  close_to(quad(1).left, 22) && same(quad(2).fill, kDefaultTheme.base_lower) &&
                  same(quad(3).fill, kDefaultTheme.base_lower);
    const bool idle_keeps_shape = paint_stream_reset(&paint) &&
                                  ui_presence_paint(&paint, {22, 22, 32, 32}, UiPresence::idle,
                                                    kDefaultTheme.base_lower, kDefaultTheme, true) &&
                                  contains_fill(paint, kDefaultTheme.tokens.status.idle) && close_to(paint.quads[0].top, 20);
    paint_stream_release(&paint);
    check(presence, "theme tokens: presence and edit borders paint dedicated status/border tokens, not syntax colours");
    check(phone_paint && idle_keeps_shape,
          "presence: the mobile status paints a cut-out green phone with screen and button; idle keeps its dot");

    ClientThemeDocument output{};
    const auto hex = parse(utf8_literal("{\"version\":\"2.0\",\"colors\":{\"accent\":\"#5865F2\","
                                        "\"text_primary\":\"#fff\",\"notification\":\"#f2333380\"}}"),
                           &output);
    check(hex.ok() && approx(output.theme.accent, {88 / 255.f, 101 / 255.f, 242 / 255.f, 1}) &&
              same(output.theme.text_primary, {1, 1, 1, 1}) && approx(output.theme.notification, {.949f, .2f, .2f, .502f}) &&
              same(output.theme.tokens.text.link, output.theme.accent) &&
              same(output.theme.tokens.mention.background, ui_theme_color_alpha(output.theme.accent, .12f)),
          "theme document v2: #rgb/#rrggbb/#rrggbbaa colours and derived semantic tokens follow core overrides");
    const auto refs = parse(
        utf8_literal("{\"semantic\":{\"text\":{\"muted\":\"{semantic.state.hover}\",\"link\":\"{palette.sky}@0.5\"},"
                     "\"state\":{\"hover\":\"{colors.accent}@0.25\"},\"border\":{\"focus\":\"{semantic.text.link}\"}},"
                     "\"palette\":{\"sky\":\"#00a8fc\"},\"colors\":{\"accent\":\"{palette.sky}\"},\"version\":\"2.7\"}"),
        &output);
    const Color sky = output.theme.accent; // hex conversion is /fp:fast runtime arithmetic
    check(refs.ok() && approx(sky, {0, 168 / 255.f, 252 / 255.f, 1}) &&
              same(output.theme.tokens.state.hover, ui_theme_color_alpha(sky, .25f)) &&
              same(output.theme.tokens.text.muted, output.theme.tokens.state.hover) &&
              same(output.theme.tokens.text.link, ui_theme_color_alpha(sky, .5f)) &&
              same(output.theme.tokens.border.focus, output.theme.tokens.text.link),
          "theme document v2: palette/core/semantic references resolve forward, independent of member order, "
          "with alpha suffixes and any 2.x minor");
    const auto cycle = parse(utf8_literal("{\"version\":\"2\",\"semantic\":{\"text\":{\"normal\":\"{semantic.text.muted}\","
                                          "\"muted\":\"{semantic.text.normal}\"}}}"),
                             &output);
    char chain[1024];
    u32 used{};
    const auto append = [&](Utf8View text) noexcept {
        copy_bytes(chain + used, text.data, text.bytes);
        used += text.bytes;
    };
    // overlay.* tokens chain eleven references deep: beyond the depth bound.
    append(utf8_literal("{\"version\":\"2.1\",\"semantic\":{\"syntax\":{"
                        "\"keyword\":\"{semantic.syntax.type}\",\"type\":\"{semantic.syntax.string}\","
                        "\"string\":\"{semantic.syntax.number}\",\"number\":\"{semantic.syntax.comment}\","
                        "\"comment\":\"{semantic.syntax.operator}\",\"operator\":\"{semantic.syntax.builtin}\","
                        "\"builtin\":\"{semantic.syntax.preprocessor}\",\"preprocessor\":\"{semantic.status.online}\"},"
                        "\"status\":{\"online\":\"{semantic.status.idle}\",\"idle\":\"{semantic.status.dnd}\"}}}"));
    const auto deep = parse({chain, used}, &output);
    check(!cycle.ok() && cycle.error == ClientThemeDocumentError::cycle && !deep.ok() &&
              deep.error == ClientThemeDocumentError::cycle,
          "theme document v2: reference cycles and chains deeper than the bound fail atomically");
    auto before = client_theme_document_defaults();
    output = before;
    check(!parse(utf8_literal("{\"version\":\"2.0\",\"semantic\":{\"text\":{\"muted\":\"{semantic.text.nope}\"}}}"), &output).ok() &&
              !parse(utf8_literal("{\"version\":\"2.0\",\"semantic\":{\"text\":{\"muted\":\"{palette.missing}\"}}}"), &output).ok() &&
              !parse(utf8_literal("{\"version\":\"2.0\",\"colors\":{\"accent\":\"{semantic.text.muted}\"}}"), &output).ok() &&
              !parse(utf8_literal("{\"version\":\"2.0\",\"colors\":{\"accent\":\"#12345\"}}"), &output).ok() &&
              !parse(utf8_literal("{\"version\":\"2.0\",\"colors\":{\"accent\":\"{colors.accent}@2\"}}"), &output).ok() &&
              !parse(utf8_literal("{\"version\":1,\"colors\":{\"accent\":\"#123456\"}}"), &output).ok() &&
              !parse(utf8_literal("{\"version\":\"3.0\"}"), &output).ok() &&
              !parse(utf8_literal("{\"version\":\"2.x\"}"), &output).ok() &&
              bytes_equal(&output, &before, sizeof(output)),
          "theme document v2: unknown/misdirected references, bad hex, the retired v1 schema and other majors are refused "
          "without publication");
    const auto partial = parse(utf8_literal("{\"version\":\"2.3\",\"future\":1,\"semantic\":{\"future\":{},"
                                            "\"overlay\":{\"scrim\":[0,0,0,0.4],\"future\":\"#fff\"}},"
                                            "\"scale\":{\"radius_md\":10,\"future\":1},"
                                            "\"motion\":{\"fast\":{\"duration_ms\":90,\"curve\":\"spring\"}}}"),
                               &output, ClientThemeBase::light);
    check(partial.ok() && partial.unsupported_fields == 4 && output.base == ClientThemeBase::light &&
              same(output.theme.tokens.overlay.scrim, {0, 0, 0, .4f}) &&
              same(output.theme.tokens.text.normal, kLightTheme.text_primary) &&
              same(output.theme.tokens.border.subtle, kLightTheme.tokens.border.subtle) &&
              output.theme.scale.radius_md == 10 && output.theme.scale.space_4 == 8 &&
              output.theme.motion.fast.duration_ms == 90 && output.theme.motion.fast.curve == UiMotionCurve::spring,
          "theme document v2: partial themes inherit the base, count unknown keys and edit scales and motion");
    check(!parse(utf8_literal("{\"version\":\"2.0\",\"scale\":{\"radius_md\":99}}"), &output).ok() &&
              !parse(utf8_literal("{\"version\":\"2.0\",\"motion\":{\"fast\":5000}}"), &output).ok() &&
              !parse(utf8_literal("{\"version\":\"2.0\",\"motion\":{\"fast\":{\"curve\":\"bounce\"}}}"), &output).ok(),
          "theme document v2: scale and motion values are bounded");
    const auto untouched = output;
    const auto retired = parse(utf8_literal("{\"version\":1,\"semantic\":{\"text\":{\"normal\":[1,0,0,1]}},"
                                            "\"colors\":{\"accent\":[0,1,0,1]}}"),
                               &output);
    check(!retired.ok() && retired.error == ClientThemeDocumentError::unsupported_version &&
              bytes_equal(&output, &untouched, sizeof(output)),
          "theme document: the retired version 1 schema is refused as an unsupported version without publication");

    const auto components = parse(
        utf8_literal("{\"version\":\"2.0\",\"semantic\":{\"text\":{\"link\":\"#00ff00\"}},\"components\":{"
                     "\"icons\":{\"muted\":\"{semantic.text.link}@0.5\",\"weight\":1.5,\"future\":1},"
                     "\"settings\":{\"focus_ring\":\"#ff0000\",\"modal_radius\":16,\"switch_ms\":250}}}"),
        &output);
    check(components.ok() && components.unsupported_fields == 1 && output.icons.colors[1] == 0x00ff0080u &&
              output.icons.weight == 1.5f && output.settings.focus_ring == 0xff0000ffu &&
              output.settings.modal_radius == 16 && output.settings.switch_ms == 250 && !output.settings.scrim &&
              !parse(utf8_literal("{\"version\":\"2.0\",\"components\":{\"icons\":{\"weight\":9}}}"), &output).ok() &&
              !parse(utf8_literal("{\"version\":\"2.0\",\"components\":{\"settings\":{\"modal_viewport_ratio\":5}}}"),
                     &output)
                   .ok(),
          "theme document v2: component icon/settings tokens resolve references after the semantic tier and stay "
          "bounded");
    parse(utf8_literal("{\"version\":\"2.0\",\"components\":{\"icons\":{\"danger\":\"#123456\"},"
                       "\"settings\":{\"modal_radius\":20,\"scrim\":\"{semantic.overlay.scrim}\"}}}"),
          &output);
    auto authored = output;
    authored.theme.tokens.state.hover = {.2f, .3f, .4f, .5f};
    authored.theme.scale.radius_xl = 20;
    u32 bytes{};
    ClientThemeDocument roundtrip{};
    check(client_theme_document_write(authored, {exported, sizeof(exported)}, &bytes).ok() &&
              parse({exported, bytes}, &roundtrip).ok() &&
              client_theme_document_equal(authored, roundtrip) && bytes_equal(exported, "{\"version\":\"2.0\"", 15),
          "theme document v2: canonical writer emits version 2.0 with resolved semantic, scale and motion values");

#ifdef MAICORD_THEME_EXAMPLE_PATH
    {
        const HANDLE file = CreateFileW(MAICORD_THEME_EXAMPLE_PATH, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, 0, nullptr);
        DWORD read{};
        const bool loaded = file != INVALID_HANDLE_VALUE &&
                            ReadFile(file, exported, sizeof(exported), &read, nullptr) && read && read < sizeof(exported);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        ClientThemeDocument example{};
        const auto parsed = loaded ? parse({exported, read}, &example) : ClientThemeDocumentResult{};
        auto defaults = client_theme_document_defaults();
        copy_bytes(defaults.name, example.name, sizeof(defaults.name));
        defaults.name_bytes = example.name_bytes;
        check(loaded && parsed.ok() && !parsed.unsupported_fields && client_theme_document_equal(example, defaults),
              "theme example: the generated example lists every token and parses to exactly the built-in defaults");
    }
#endif
    {
        wchar_t directory[kClientThemeIoMaximumPathUnits]{};
        const DWORD temp = GetTempPathW(kClientThemeIoMaximumPathUnits - 64, directory);
        constexpr wchar_t leaf[] = L"maicord-theme-catalog";
        copy_bytes(directory + temp, leaf, sizeof(leaf));
        const u32 base = temp + sizeof(leaf) / sizeof(wchar_t) - 1;
        CreateDirectoryW(directory, nullptr);
        struct Fixture {
            const wchar_t* file;
            Utf8View text;
        };
        const Fixture files[]{
            {L"\\b.json", utf8_literal("{\"version\":\"2.0\",\"name\":\"Beta\",\"base\":\"light\"}")},
            {L"\\a.json", utf8_literal("{\"version\":\"2.0\",\"colors\":{\"accent\":[1,0,0,1]}}")},
            {L"\\corrupt.json", utf8_literal("{\"version\":\"2.0\",\"colors\":{")},
            {L"\\notes.txt", utf8_literal("{\"version\":1}")},
        };
        wchar_t path[kClientThemeIoMaximumPathUnits]{};
        const auto join = [&](const wchar_t* file) noexcept {
            copy_bytes(path, directory, base * sizeof(wchar_t));
            u32 i{};
            for (; file[i]; ++i)
                path[base + i] = file[i];
            path[base + i] = 0;
            return path;
        };
        bool written = true;
        for (const auto& fixture : files) {
            const HANDLE file = CreateFileW(join(fixture.file), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD wrote{};
            written = written && file != INVALID_HANDLE_VALUE &&
                      WriteFile(file, fixture.text.data, fixture.text.bytes, &wrote, nullptr);
            if (file != INVALID_HANDLE_VALUE)
                CloseHandle(file);
        }
        CreateDirectoryW(join(L"\\folder.json"), nullptr);
        auto* catalog = static_cast<ClientThemeCatalog*>(
            VirtualAlloc(nullptr, sizeof(ClientThemeCatalog), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        u32 units{};
        const bool scanned = catalog && client_theme_catalog_scan(directory, catalog).ok();
        const bool listed =
            scanned && catalog->count == 2 && catalog->skipped == 1 &&
            text_equal_bytes(catalog->entries[0].name, catalog->entries[0].name_bytes, utf8_literal("a")) &&
            text_equal_bytes(catalog->entries[1].name, catalog->entries[1].name_bytes, utf8_literal("Beta")) &&
            catalog->entries[1].base == ClientThemeBase::light && catalog->entries[1].identity.bytes > 0 &&
            client_theme_catalog_path(directory, catalog->entries[1], {path, kClientThemeIoMaximumPathUnits}, &units) &&
            units == base + 7 && path[units - 6] == L'b';
        for (const auto& fixture : files)
            DeleteFileW(join(fixture.file));
        RemoveDirectoryW(join(L"\\folder.json"));
        RemoveDirectoryW(directory);
        const bool empty = catalog && client_theme_catalog_scan(directory, catalog).ok() && !catalog->count;
        if (catalog)
            VirtualFree(catalog, 0, MEM_RELEASE);
        check(written && listed && empty,
              "theme catalog: installed *.json themes are parsed and sorted, corrupt files skipped, folders and "
              "other files ignored, and a missing folder is an empty catalog");
    }
    UiThemeContrastWarning warnings[4]{};
    const bool clean = ui_theme_contrast_warnings(kDefaultTheme, warnings, 4) == 0 &&
                       ui_theme_contrast_warnings(kLightTheme, warnings, 4) == 0 &&
                       ui_theme_contrast_warnings(amoled, warnings, 4) == 0;
    const auto low = parse(utf8_literal("{\"version\":\"2.0\",\"colors\":{\"text_primary\":\"#2a2a2e\"}}"), &output);
    const u32 count = ui_theme_contrast_warnings(output.theme, warnings, 1);
    check(clean && low.ok() && count >= 3 && warnings[0].foreground == UiThemeToken::text_normal &&
              warnings[0].ratio < warnings[0].minimum && ui_theme_contrast_ratio({1, 1, 1, 1}, {0, 0, 0, 1}) > 20.9f,
          "theme tokens: WCAG contrast warnings are advisory, empty for built-ins and report low-contrast pairs");
}
} // namespace maicord
