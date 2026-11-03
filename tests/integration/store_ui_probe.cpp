#include "maicord/core/win32_types.hpp"
#include "maicord_effect_aurora.hpp"
#include "probe_present.hpp"
#include "../support/ui_scene_fixtures.hpp"
#include "settings/catalog.hpp"
#include "store/store_model.hpp"
#include "store/store_preview.hpp"
#include "maicord/app/settings/client_theme.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/render/d3d11/layer_presentation.hpp"
#include "maicord/render/d3d11/renderer_presentation.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/widgets/kit_markdown.hpp"

// The Plugins, Themes and Effects pages through the real settings modal, layout, paint and text paths: one
// capture per screen of store-ui.md (mockups 01-11) at 1440x900 DIP, with the content of the mockups, on the
// Sunset theme like the mockups (11 is the flat dark one). MAICORD_STORE_PREVIEW_DIR names a folder of the
// catalog fixture's `previews/<sha>.png`: the tiles then show the real pictures, decoded and uploaded the way the
// host does, and 04 and 08 run the Aurora sample as the window background so their live tiles have an effect.
namespace maicord {
namespace {
using namespace app_detail;

constexpr UiPageKey kPlugins{kSettingsNativeOwner, static_cast<u64>(SettingsPanel::plugins) + 1};
constexpr UiPageKey kThemes{kSettingsNativeOwner, static_cast<u64>(SettingsPanel::themes) + 1};
constexpr UiPageKey kEffects{kSettingsNativeOwner, static_cast<u64>(SettingsPanel::effects) + 1};

struct Fixture {
    UiPage pages[kSettingsPanelCount + 1];
    UiSettingsPageContent contents[kSettingsPanelCount + 1];
    UiPageRegistry registry;
    UiSettingsScene scene;
    UiStoreScene store;
    StoreModel model;
    StorePageState state[3];
    PopupItem items[16];
    char labels[48];
    // An isolated run has no shared popup bank; this probe owns one.
    UiMarkdownBank* bank;
    PopupScene menu;
    PopupItem menu_items[16];
    PopupRowWidget menu_rows[16];
    UiWidget menu_widgets[2 + 16 * kPopupWidgetsPerItem];
    UiLayoutNode menu_nodes[2 + 16 * kPopupWidgetsPerItem];
};
Fixture fixture;

// The catalog fixture's pictures: the first preview of each themed or effect item, by slug.
struct PreviewFile {
    Utf8View slug;
    const char* sha;
};
constexpr PreviewFile kPreviews[]{
    {utf8_literal("deep-ocean"), "429bf0b6c82a403f3909b583837cf2010e6cdd539efb8c631cb2b240d9c778d2"},
    {utf8_literal("warm-paper"), "e4a97cd56c371ecd9ab524ebf9bcb77097342a117c1b7becf45678cbcc8c0241"},
    {utf8_literal("high-contrast"), "ccb844638e2dfcea33a1a68fab9ed33004336e77516e1cfacbd8407814812c87"},
    {utf8_literal("fjord"), "b5800f26df7e34131295ba2ca64a77ad8a50e8eb18a594abe90381b9fe86487f"},
    {utf8_literal("rose-dusk"), "71eb1908781d58c313e2bcaf36469a52ceb274bbf2be32e604edfa5aa8afff47"},
    {utf8_literal("obsidian"), "55397c10d7f75cfd960c59643ed03b7a65f425d40ff86d33822352f0920e1ff4"},
    {utf8_literal("sample-neon-grid"), "8f1c021d00000000000000000000000000000000000000000000000000000000"},
    {utf8_literal("ascii-rain"), "b62e84a1272683816d8bf7af0ba66247df69488a646f4e2c67ff1939dd6745a5"},
    {utf8_literal("aurora"), "c98761d5636b1a6f01862883d52db8d3e9e5bc34615daaab4ca14ac1ce702d00"},
    {utf8_literal("bokeh"), "8c23df884adb03f1d017ddc4b858f44ea1af4dd008192ac3636fbf2864cc6569"},
    {utf8_literal("film-grain"), "1bd5739a7db8d9cc7f7cc13046f26ddabed3dc91c025778ba9092050e7d62f2d"},
    {utf8_literal("plasma"), "38bae74cf751a84b496301a591dfef98a9ad7902619dcd186fabf9fb7c45752f"},
    {utf8_literal("starfield"), "080b0653749c9dee3d47b455523e7118e3442201689138e8fcf701e1769918a9"},
    {utf8_literal("wave-lines"), "fb9de290619af0f20b5ea512e7558f7c2a3bb29ec4647f755fd64a4dac326979"},
};

// The probe window's media atlases are 256 px square; tiles of a real page need room for their pictures.
constexpr u32 kAtlasEntries = 64, kAtlasRows = 64;
struct AtlasStorage {
    MediaAtlas atlas;
    MediaAtlasEntry entries[kAtlasEntries];
    u32 buckets[kAtlasEntries * 2];
    GlyphAtlasEntry allocations[kAtlasEntries];
    GlyphAtlasRow rows[kAtlasRows];
};
AtlasStorage g_primary_atlas, g_preview_atlas;

bool bind_page_atlases(D3D11Renderer& renderer) noexcept {
    renderer_unbind_media_atlas(&renderer);
    AtlasStorage* const storage[]{&g_primary_atlas, &g_preview_atlas};
    for (AtlasStorage* s : storage)
        if (!media_atlas_initialize(&s->atlas, 2048, 2048, 1, {s->entries, kAtlasEntries}, {s->buckets, kAtlasEntries * 2},
                                    {s->allocations, kAtlasEntries}, {s->rows, kAtlasRows}))
            return false;
    return renderer_bind_media_atlas(&renderer, &g_primary_atlas.atlas) &&
           renderer_bind_preview_media_atlas(&renderer, &g_preview_atlas.atlas);
}

wchar_t g_preview_directory[512];
u32 g_preview_dpi = 96;
u8 g_preview_file[512 * 1024];

u32 hex_digit(char c) noexcept {
    return c >= '0' && c <= '9' ? static_cast<u32>(c - '0') : static_cast<u32>(c - 'a' + 10);
}

bool preview_sha(u32 slot, u8 (&sha)[32], const char** hex) noexcept {
    if (!g_preview_directory[0] || !slot || slot > fixture.model.count)
        return false;
    const StoreItem& item = fixture.model.items[slot - 1];
    for (const auto& file : kPreviews)
        if (file.slug.bytes == item.slug.bytes && bytes_equal(file.slug.data, item.slug.data, item.slug.bytes)) {
            for (u32 i = 0; i < 32; ++i)
                sha[i] = static_cast<u8>(hex_digit(file.sha[i * 2]) << 4 | hex_digit(file.sha[i * 2 + 1]));
            *hex = file.sha;
            return true;
        }
    return false;
}

bool preview_media_key(const void*, u32 slot, const UiRect& rect, MediaAssetKey* key, u8* domain) noexcept {
    u8 sha[32];
    const char* hex = nullptr;
    if (!preview_sha(slot, sha, &hex))
        return false;
    *key = store_preview_key(sha, store_preview_pixels(ui_rect_width(rect), g_preview_dpi), false);
    *domain = 1;
    return true;
}

// What the host's pump does for the visible tiles: read the verified bytes, decode to the tile's size, upload.
bool upload_previews(D3D11Renderer& renderer) noexcept {
    UiStoreMedia media{};
    u32 cursor = 0;
    while (ui_store_scene_next_media(fixture.store, &cursor, &media)) {
        if (media_atlas_peek(renderer.preview_media_atlas, media.key).ok())
            continue;
        u8 sha[32];
        const char* hex = nullptr;
        if (!preview_sha(media.slot, sha, &hex))
            continue;
        wchar_t path[600];
        u32 n = 0;
        for (; g_preview_directory[n]; ++n)
            path[n] = g_preview_directory[n];
        path[n++] = L'\\';
        for (u32 i = 0; i < 64; ++i)
            path[n++] = static_cast<wchar_t>(hex[i]);
        for (const wchar_t c : L".png")
            path[n++] = c;
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            continue;
        DWORD read = 0;
        const bool loaded = ReadFile(file, g_preview_file, sizeof(g_preview_file), &read, nullptr) && read;
        CloseHandle(file);
        StorePreviewImage image{};
        if (!loaded || !store_preview_decode({g_preview_file, read}, media.key.variant & 0x1fffffffu, &image)) {
            log_u64(LogLevel::error, "probe", loaded ? "store_preview_decode_failed_bytes" : "store_preview_read_failed", read);
            return false;
        }
        const bool uploaded = renderer_upload_media_straight_rgba8(&renderer, media.key, static_cast<u16>(image.width),
                                                                   static_cast<u16>(image.height), image.width * 4u,
                                                                   {image.pixels, image.width * image.height * 4u})
                                  .ok();
        store_preview_free(&image);
        if (!uploaded) {
            log_u64(LogLevel::error, "probe", "store_preview_upload_failed", media.key.variant);
            return false;
        }
    }
    return true;
}

StoreItem& add(CatalogKind kind, Utf8View slug, Utf8View name, Utf8View summary, Utf8View authors, Utf8View version,
               Utf8View c0, Utf8View c1 = {}) noexcept {
    auto& m = fixture.model;
    StoreItem& item = m.items[m.count];
    reset_value(&item);
    item.kind = kind;
    item.slug = slug;
    item.name = name;
    item.summary = summary;
    item.authors = authors;
    item.version = item.installed_version = version;
    item.categories[0] = c0;
    item.category_count = 1;
    if (c1.bytes)
        item.categories[item.category_count++] = c1;
    item.item = m.count++;
    item.library = ~0u;
    item.installable = true;
    item.license = utf8_literal("MPL-2.0");
    item.released = 1793000000;
    item.bytes = 9216;
    item.base = 0;
    return item;
}

void plugins() noexcept {
    auto& m = fixture.model;
    reset_value(&m);
    m.catalog = StoreCatalogState::ready;
    m.installable = true;
    m.checked_unix = 1793000000;
    m.fetched_unix = 1792700000;
    m.protocol_state = 0;
    StoreItem& clear = add(CatalogKind::plugin, utf8_literal("clear-urls"), utf8_literal("Clear URLs"),
                           utf8_literal("Removes tracking parameters from links you send."), utf8_literal("Maj Sokoli\xC4\x8D"), utf8_literal("1.0.0"),
                           utf8_literal("Privacy"), utf8_literal("Chat"));
    clear.installed = clear.enabled = clear.running = true;
    clear.risk = 1;
    clear.has_settings = true;
    clear.order = 0;
    StoreItem& translate = add(CatalogKind::plugin, utf8_literal("translate"), utf8_literal("Translate"),
                               utf8_literal("Translate messages with Google or DeepL from the message menu, or with /tr."),
                               utf8_literal("MaiCord"), utf8_literal("1.0.0"), utf8_literal("Chat"), utf8_literal("Utility"));
    translate.risk = 2;
    translate.sha = utf8_literal("8059942b\xE2\x80\xA6" "6e1018");
    translate.commit = utf8_literal("b76ad03");
    translate.api_level = 6;
    translate.source = utf8_literal("https://github.com/maj113/MaiCord/tree/master/samples/plugins/translate");
    StoreItem& names = add(CatalogKind::plugin, utf8_literal("show-me-your-name"), utf8_literal("Show Me Your Name"),
                           utf8_literal("Shows usernames next to nicknames in chat, so you always know who's who."), utf8_literal("MaiCord"),
                           utf8_literal("1.2.0"), utf8_literal("Appearance"), utf8_literal("Chat"));
    names.installed = names.enabled = names.running = true;
    names.installed_version = utf8_literal("1.1.0");
    names.update = true;
    names.update_version = utf8_literal("1.2.0");
    names.risk = 1;
    names.order = 1;
    StoreItem& reverse = add(CatalogKind::plugin, utf8_literal("reverse-image-search"), utf8_literal("Reverse Image Search"),
                             utf8_literal("Adds a Search image submenu to images: Google Lens, Bing, Yandex and TinEye."), utf8_literal("MaiCord"),
                             utf8_literal("1.0.0"), utf8_literal("Chat"), utf8_literal("Utility"));
    reverse.installed = reverse.enabled = reverse.running = true;
    reverse.has_settings = true;
    reverse.can_roll_back = true;
    reverse.previous_version = utf8_literal("0.9.2");
    reverse.auto_update = true;
    reverse.order = 3;
    StoreItem& silent = add(CatalogKind::plugin, utf8_literal("silent-toggle"), utf8_literal("Silent Toggle"),
                            utf8_literal("A chat bar button that sends your next message silently, without pinging anyone."), utf8_literal("MaiCord"),
                            utf8_literal("1.0.0"), utf8_literal("Notifications"));
    silent.installed = true;
    silent.risk = 1;
    silent.order = 2;
    StoreItem& pin = add(CatalogKind::plugin, utf8_literal("pin-counter"), utf8_literal("Pin Counter"), utf8_literal("Counts pins."),
                         utf8_literal("Lumen"), utf8_literal("0.3.0"), utf8_literal("Utility"));
    pin.installed = pin.enabled = pin.running = true;
    pin.sideloaded = true;
    pin.item = ~0u;
    pin.folder = utf8_literal("D:\\dev\\pin-counter");
    pin.summary = utf8_literal("Loaded from D:\\dev\\pin-counter");
    pin.order = 4;
    names.has_settings = false;
}

void permissions(StoreConsent* k) noexcept {
    reset_value(k);
    store_permission_text(4, &k->permissions[k->permission_count++]);
    store_permission_text(13 - 13 + 0, &k->permissions[k->permission_count++]);
    k->permissions[1].sentence = utf8_literal("Changes how messages look");
    k->permissions[1].name = utf8_literal("message_render");
    k->permissions[1].glyph = UiIcon::eye;
    k->permissions[1].risk = 1;
    store_permission_text(0, &k->permissions[k->permission_count++]);
    k->permissions[2].risk = 1;
    k->permissions[k->permission_count] = {utf8_literal("Adds badges, buttons and local slash commands"), utf8_literal("ui_slots \xC2\xB7 commands"), {},
                                           UiIcon::command, 0, false, false};
    ++k->permission_count;
    k->hosts[0] = utf8_literal("translate.googleapis.com");
    k->hosts[1] = utf8_literal("api-free.deepl.com");
    k->hosts[2] = utf8_literal("api.deepl.com");
    k->host_count = 3;
    k->permissions[0].note = utf8_literal("It sends the text of messages you choose to translate.");
}

void themes() noexcept {
    auto& m = fixture.model;
    reset_value(&m);
    m.catalog = StoreCatalogState::ready;
    m.installable = true;
    static constexpr struct {
        Utf8View slug, name, summary, c0, c1;
        u32 sw[4];
        u8 base;
    } kThemes[]{
        {utf8_literal("deep-ocean"), utf8_literal("Deep Ocean"), utf8_literal("A deep navy dark theme with a cool teal accent."), utf8_literal("Blue"),
         utf8_literal("Cool"), {0x0b1a29, 0x10253a, 0xe4f1f8, 0x22b8cf}, 0},
        {utf8_literal("fjord"), utf8_literal("Fjord"), utf8_literal("A muted, frosty blue-grey dark theme in the spirit of the Nord palette."),
         utf8_literal("Muted"), utf8_literal("Cool"), {0x2e3440, 0x363d4b, 0xe5e9f0, 0x88c0d0}, 0},
        {utf8_literal("high-contrast"), utf8_literal("High Contrast"),
         utf8_literal("Pure black and white with a yellow accent, strong borders and heavier icons. Passes WCAG AAA for text."),
         utf8_literal("Accessibility"), {}, {0x000000, 0x161616, 0xffffff, 0xffd23f}, 0},
        {utf8_literal("obsidian"), utf8_literal("Obsidian"), utf8_literal("True black chat and surfaces for OLED panels, with neutral greys and an emerald accent."),
         utf8_literal("OLED"), {}, {0x000000, 0x101011, 0xececee, 0x34d399}, 0},
        {utf8_literal("rose-dusk"), utf8_literal("Rose Dusk"), utf8_literal("Deep plum surfaces with a soft rose accent, like the sky just after sunset."),
         utf8_literal("Warm"), utf8_literal("Purple"), {0x241540, 0x321f4d, 0xf6ecf8, 0xf08fb8}, 0},
        {utf8_literal("sample-neon-grid"), utf8_literal("Sample Neon Grid"),
         utf8_literal("Synthwave purples and hot pink, with drifting stars behind the chat. This is a sample entry that exists only to lay out the page."),
         utf8_literal("Colorful"), utf8_literal("Animated"), {0x0b0614, 0x140b24, 0xf3e9ff, 0xff3ea5}, 0},
        {utf8_literal("warm-paper"), utf8_literal("Warm Paper"), utf8_literal("A warm, cream-coloured light theme with a terracotta accent, easy on the eyes."),
         utf8_literal("Warm"), utf8_literal("Cream"), {0xf7f0e1, 0xfdf8ec, 0x2b2118, 0xb04a24}, 1},
    };
    for (const auto& t : kThemes) {
        StoreItem& item = add(CatalogKind::theme, t.slug, t.name, t.summary, utf8_literal("MaiCord"), utf8_literal("1.0.0"), t.c0, t.c1);
        item.base = t.base;
        item.has_swatches = true;
        for (u32 i = 0; i < 4; ++i)
            item.swatches[i] = t.sw[i];
        item.bytes = 1331;
        item.license = utf8_literal("MIT");
        item.released = 1793534400;
        item.source = utf8_literal("https://github.com/maj113/MaiCord-plugins/tree/b76ad03b224c41d6b9338e769370aded6a3a5d01/themes");
        item.contrast = utf8_literal("WCAG AA for text");
    }
    m.items[0].installed = true;
    m.items[0].order = 0;
    m.items[1].installed = m.items[1].active = true;
    m.items[1].order = 1;
}

void effects() noexcept {
    auto& m = fixture.model;
    reset_value(&m);
    m.catalog = StoreCatalogState::ready;
    m.installable = true;
    static constexpr struct {
        Utf8View slug, name, summary, c0, c1;
    } kEffects[]{
        {utf8_literal("ascii-rain"), utf8_literal("ASCII rain"), utf8_literal("Columns of glyphs falling down the window, a bright head with a fading trail."),
         utf8_literal("Retro"), utf8_literal("Text")},
        {utf8_literal("aurora"), utf8_literal("Aurora"),
         utf8_literal("Soft curtains of light drifting across the top of the window, tinted by the theme's accent and positive roles over its background."),
         utf8_literal("Ambient"), utf8_literal("Nature")},
        {utf8_literal("bokeh"), utf8_literal("Soft bokeh"), utf8_literal("Out-of-focus discs drifting slowly upward in three depth layers, in the theme's accent and text colours."),
         utf8_literal("Ambient"), utf8_literal("Soft")},
        {utf8_literal("film-grain"), utf8_literal("Film grain"),
         utf8_literal("A faint, constantly changing speckle over everything, light and dark, for a printed-paper feel."), utf8_literal("Texture"),
         utf8_literal("Retro")},
        {utf8_literal("plasma"), utf8_literal("Plasma"), utf8_literal("The classic sum of sines, tinted toward the theme so it never fights the UI."),
         utf8_literal("Abstract"), utf8_literal("Retro")},
        {utf8_literal("starfield"), utf8_literal("Starfield"),
         utf8_literal("Three parallax layers of twinkling stars drifting sideways, sized in DIPs so they look the same at every DPI and resolution."),
         utf8_literal("Space"), utf8_literal("Ambient")},
        {utf8_literal("wave-lines"), utf8_literal("Wave lines"),
         utf8_literal("A stack of thin horizontal lines rippling with a few overlaid sines, fading from the theme's accent to its positive colour down the window."),
         utf8_literal("Abstract"), utf8_literal("Minimal")},
    };
    for (const auto& e : kEffects) {
        StoreItem& item = add(CatalogKind::effect, e.slug, e.name, e.summary, utf8_literal("MaiCord"), utf8_literal("1.0.0"), e.c0, e.c1);
        item.cost = 0;
        item.released = 1793534400;
        item.source = utf8_literal("https://github.com/maj113/MaiCord/tree/master/samples/effects");
    }
    StoreItem& aurora = m.items[1];
    aurora.installed = aurora.active = aurora.has_knobs = true;
    aurora.library = 0;
    aurora.order = 0;
}

void knobs() noexcept {
    auto& m = fixture.model;
    m.knob_count = 0;
    static constexpr struct {
        Utf8View label, hint;
        u8 type;
        bool percent;
        float min, max, value, fallback;
    } kKnobs[]{
        {utf8_literal("Drift speed"), {}, 0, false, 0, 2, .35f, .35f},
        {utf8_literal("Brightness"), {}, 0, false, .2f, 1, .7f, .7f},
        {utf8_literal("Reach"), utf8_literal("Share of the window the curtains cover."), 0, true, .3f, 1, .75f, .75f},
        {utf8_literal("Curtains"), {}, 1, false, 1, 4, 3, 3},
    };
    for (const auto& k : kKnobs) {
        StoreKnob& out = m.knobs[m.knob_count++];
        reset_value(&out);
        out.label = k.label;
        out.hint = k.hint;
        out.type = k.type;
        out.percent = k.percent;
        out.minimum = k.min;
        out.maximum = k.max;
        out.value = k.value;
        out.fallback = k.fallback;
    }
    StoreUniversal& u = m.universal;
    u = {};
    u.resolution = 0;
    u.frame_rate = 4; // 30 fps (override)
    u.speed = 20;
    u.opacity = 100;
    u.global_resolution = 3;
    u.global_frame_rate = 3;
    u.requested_frame_rate = 0;
    m.effect_cost_state = 1;
    m.effect_cost_text = utf8_literal("Light here \xC2\xB7 0.4 ms");
    m.global_universal = u;
}

// The Aurora sample as the window background: what a live tile shows.
bool run_aurora(D3D11Renderer& renderer) noexcept {
    BackgroundEffectSpec spec{};
    spec.bytecode = {g_maicord_effect_aurora, sizeof(g_maicord_effect_aurora)};
    spec.identity = hash64_bytes(kHash64Seed, g_maicord_effect_aurora, sizeof(g_maicord_effect_aurora)) | 1;
    spec.uses = effect_use_blue_noise;
    spec.blend = EffectBlend::normal;
    spec.requested_resolution = spec.requested_frame_rate = 3;
    const float values[]{.35f, .7f, .75f, 3};
    for (u32 i = 0; i < 4; ++i)
        spec.knobs[0][i] = values[i];
    const EffectSettings settings{1.f, 1.f, EffectBlend::normal, 2, 2, EffectBudget::high, false};
    const auto selected = renderer_select_effect(&renderer, spec, settings);
    if (selected.state == EffectSelect::rejected || FAILED(selected.native_error))
        return false;
    // The first redraw is the clock's zero: start the picture a few seconds in, where the curtains have formed.
    renderer.effect.time = 6.f;
    return renderer_set_background_phase(&renderer, 6.f);
}

bool show(Fixture& f, D3D11Renderer& renderer, UiPageKey page, const UiRect& viewport, const wchar_t* name,
          bool (*capture)(const wchar_t*, const Theme&) noexcept, const Theme& theme,
          bool (*before)(Fixture&) noexcept = nullptr) noexcept {
    const CatalogKind kind = page.page_id == kPlugins.page_id ? CatalogKind::plugin : page.page_id == kThemes.page_id ? CatalogKind::theme : CatalogKind::effect;
    if (!ui_page_registry_select(&f.registry, page).ok() || !ui_settings_scene_update(&f.scene, &f.registry, viewport))
        return false;
    UiRect rect{}, clip{};
    if (!ui_settings_scene_host_rect(f.scene, &rect, &clip))
        return false;
    StorePageState& st = f.state[static_cast<u32>(kind)];
    st.kind = kind;
    if (before && !before(f))
        return false;
    ui_store_scene_begin(&f.store);
    const void* about = nullptr;
    if (st.detail && f.model.details_markdown.bytes) {
        UiMarkdown* block = ui_markdown_bank_block(&f.bank, 1);
        if (block && ui_markdown_parse(block, f.model.details_markdown, text_size_md, ui_markdown_bank_scratch(f.bank)))
            about = block;
    }
    StoreLayoutInput in{&f.model, &st, nullptr, ui_rect_width(rect), false, about, nullptr};
    store_page_build(&f.store, in);
    f.store.active = true;
    if (!ui_store_scene_commit(&f.store, clip, rect, f.scene.layout.modal))
        return false;
    f.store.media_key = preview_media_key;
    ++f.scene.revision;
    renderer.settings_store = &f.store;
    renderer.retained_presentation.valid = false;
    // The pictures of the visible tiles, as the host's pump delivers them.
    if (!upload_previews(renderer))
        return false;
    // The second frame draws the glyphs the first one rasterised.
    return renderer_rebuild_content_text(&renderer, theme) && probe_present(&renderer, theme) &&
           probe_present(&renderer, theme) && capture(name, theme);
}
} // namespace

bool store_ui_probe(D3D11Renderer& renderer, PopupScene&, UiTextMeasureCache& cache, bool (*relayout)() noexcept,
                    bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    if (!capture || !relayout || renderer.login_visible || !renderer.dpi)
        return false;
    auto& f = fixture;
    PopupScene& popup = f.menu;
    if (!popup.items.data &&
        !popup_scene_initialize(&popup, {f.menu_items, 16}, {f.menu_rows, 16}, {f.menu_widgets, 2 + 16 * kPopupWidgetsPerItem},
                                {f.menu_nodes, 2 + 16 * kPopupWidgetsPerItem}))
        return false;
    const PopupScene* old_popup = renderer.popup_scene;
    if (old_popup != &popup && !renderer_bind_popup_scene(&renderer, &popup))
        return false;
    const UiSettingsScene* old_scene = renderer.settings_scene;
    const u32 width = renderer.width, height = renderer.height;
    struct Restore {
        D3D11Renderer& renderer;
        Fixture& f;
        PopupScene& popup;
        const PopupScene* old_popup;
        const UiSettingsScene* old_scene;
        u32 width, height;
        MediaAtlas* primary;
        MediaAtlas* preview;
        ~Restore() noexcept {
            renderer_clear_effect(&renderer);
            renderer_unbind_media_atlas(&renderer);
            renderer_bind_media_atlas(&renderer, primary);
            renderer_bind_preview_media_atlas(&renderer, preview);
            renderer.settings_store = nullptr;
            renderer_bind_settings_scene(&renderer, old_scene);
            ui_markdown_bank_release(&f.bank);
            popup_scene_close(&popup);
            renderer_bind_popup_scene(&renderer, old_popup);
            ui_store_scene_release(&f.store);
            ui_settings_scene_release(&f.scene);
            ui_page_registry_clear(&f.registry);
            renderer_resize(&renderer, width, height);
        }
    } restore{renderer, f, popup, old_popup, old_scene, width, height, renderer.media_atlas, renderer.preview_media_atlas};
    u32 stage = __LINE__;
    // Sunset, like the mockups; 11 is the flat dark theme.
    static Theme sunset;
    {
        ClientPreferences preferences = client_preferences_defaults();
        preferences.theme = ClientTheme::sunset;
        sunset = client_theme_projection(preferences);
    }
    g_preview_dpi = renderer.dpi;
    g_preview_directory[0] = 0;
    {
        const DWORD n = GetEnvironmentVariableW(L"MAICORD_STORE_PREVIEW_DIR", g_preview_directory, 500);
        if (!n || n >= 500)
            g_preview_directory[0] = 0;
    }
    bool valid = bind_page_atlases(renderer) && ui_page_registry_initialize(&f.registry, {f.pages, kSettingsPanelCount + 1});
    for (u32 panel = 0; valid && panel < kSettingsPanelCount; ++panel) {
        const auto& source = settings_catalog_panel(static_cast<SettingsPanel>(panel));
        f.contents[panel] = {source.title, {}, {}, 1};
        const auto owner = static_cast<SettingsPanel>(panel);
        f.contents[panel].host = f.contents[panel].wide =
            owner == SettingsPanel::plugins || owner == SettingsPanel::themes || owner == SettingsPanel::effects;
        valid = ui_page_registry_register(&f.registry, {{kSettingsNativeOwner, panel + 1u}, source.title, &f.contents[panel]}).ok();
    }
    for (u32 i = 0; i < 3; ++i)
        store_page_reset(&f.state[i], static_cast<CatalogKind>(i));
    valid = valid && ui_store_scene_open(&f.store);
    static UiStoreMeasure measure;
    measure = ui_store_cache_measure(&cache);
    f.store.measure = &measure;
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    // The shell scene follows the window size: text batches share its scissor.
    valid = valid && renderer_resize(&renderer, static_cast<u32>(1440 * scale + .5f), static_cast<u32>(900 * scale + .5f)) &&
            relayout();
    {   // A typography owner supplies the code face (the default policy has none in a bare probe window).
        RendererTypographyCandidate typography{};
        UiThemeTypography value{};
        value.mono_ratio = .94f;
        valid = valid && renderer_prepare_typography(&renderer, value, &typography) &&
                renderer_commit_typography(&renderer, &typography);
        renderer_discard_typography(&typography);
    }
    const UiRect viewport{0, 32, 1440, 900};
    valid = valid && ui_settings_scene_set_presentation(&f.scene, UiSettingsPresentation::modal) &&
            ui_settings_scene_bind_navigation(&f.scene, settings_catalog_navigation(true), 1);
    if (valid)
        renderer_bind_settings_scene(&renderer, &f.scene);
    const auto next = [&](const wchar_t* name, UiPageKey page, bool (*before)(Fixture&) noexcept, const Theme& theme) noexcept {
        if (valid)
            stage = __LINE__;
        valid = valid && show(f, renderer, page, viewport, name, capture, theme, before);
    };
    // 01 Plugins > Browse
    plugins();
    f.model.items[3].installed = f.model.items[3].enabled = f.model.items[3].running = false; // Reverse Image Search: not installed yet
    f.state[0].tab = StoreTab::browse;
    next(L"store-01-browse-plugins.bmp", kPlugins, nullptr, sunset);
    // 11 the same page in the flat dark theme
    next(L"store-11-browse-plugins-dark.bmp", kPlugins, nullptr, kDefaultTheme);
    // 02 Themes > Browse
    themes();
    f.state[1].tab = StoreTab::browse;
    next(L"store-02-browse-themes.bmp", kThemes, nullptr, sunset);
    // 12 the first card with the keyboard focus ring: the ring sits past the column's start edge and stays whole.
    if (valid) {
        stage = __LINE__;
        u32 card = 0;
        for (u32 i = 0; !card && i < f.store.count; ++i)
            if (store_act(f.store.nodes[i].action) == StoreAct::card)
                card = f.store.nodes[i].action;
        valid = card && ui_store_scene_focus(&f.store, card, true);
        if (valid) {
            ++f.scene.revision;
            renderer.retained_presentation.valid = false;
            valid = renderer_rebuild_content_text(&renderer, sunset) && probe_present(&renderer, sunset) &&
                    capture(L"store-12-focus-ring.bmp", sunset);
        }
    }
    // The same page mid-entrance: the host page's cards, focus ring, pictures and glyphs move and fade with the
    // modal; only the scrim before it draws apart.
    if (valid) {
        stage = __LINE__;
        renderer_set_settings_modal_progress(&renderer, .5f);
        valid = probe_present(&renderer, sunset) && renderer.content_text &&
                renderer.settings_modal_paint_first_batch < renderer.paint_stream.batch_count &&
                renderer.settings_preview_paint_first_batch < renderer.settings_preview_paint_end_batch;
        if (valid) {
            const u32 scrim = renderer.paint_stream.batches[renderer.settings_modal_paint_first_batch].first_quad;
            const u32 glyphs = renderer.content_text->quad_count;
            log_u64(LogLevel::info, "probe", "store_mid_open_modal_instances", renderer.modal_layer_instances_last_frame);
            log_u64(LogLevel::info, "probe", "store_mid_open_other_instances", renderer.other_layer_instances_last_frame);
            valid = capture(L"store-mid-open.bmp", sunset) && scrim && glyphs &&
                    renderer.other_layer_instances_last_frame == scrim &&
                    renderer.modal_layer_instances_last_frame >= renderer.paint_stream.count - scrim + glyphs;
        }
        renderer_set_settings_modal_progress(&renderer, 1);
    }
    // 03 plugin detail
    plugins();
    permissions(&f.model.consent);
    f.model.details_markdown = utf8_literal("Adds a **Translate** button to the message menu. One click sends that message's text to the service you choose and shows the result under the message.\n\n- Pick a target language and a service in its settings.\n- Type `/tr text` to translate and insert your own text.\n- DeepL needs your API key; MaiCord stores it encrypted for this plugin only.");
    {
        StorePageState& st = f.state[0];
        st.detail = true;
        st.detail_from = StoreTab::browse;
        copy_bytes(st.slug, "translate", 10);
    }
    next(L"store-03-plugin-detail.bmp", kPlugins, nullptr, sunset);
    f.state[0].detail = false;
    // 04 effect knobs: Aurora is the window background, so its live tile has the effect to show
    effects();
    knobs();
    if (valid) {
        stage = __LINE__;
        valid = run_aurora(renderer);
    }
    {
        StorePageState& st = f.state[2];
        st.detail = st.knobs = true;
        st.detail_from = StoreTab::installed;
        copy_bytes(st.slug, "aurora", 7);
    }
    next(L"store-04-effect-knobs.bmp", kEffects, nullptr, sunset);
    f.state[2].detail = f.state[2].knobs = false;
    // 05 Plugins > Installed with the overflow menu open
    renderer_clear_effect(&renderer);
    plugins();
    f.state[0].tab = StoreTab::installed;
    next(L"store-05-installed.bmp", kPlugins, nullptr, sunset);
    if (valid) {
        stage = __LINE__;
        UiRect anchor{};
        u32 item = 0, seen = 0;
        for (u32 i = 0; i < f.store.count; ++i)
            if (store_act(f.store.nodes[i].action) == StoreAct::row_overflow && ++seen == 4 &&
                ui_store_scene_rect(f.store, f.store.nodes[i].action, &anchor))
                item = store_arg(f.store.nodes[i].action);
        const u32 count = store_menu_items(f.model, f.state[0], StoreMenu::overflow, item, f.items, 16, {f.labels, sizeof(f.labels)});
        anchor.left = anchor.right - 240.f;
        valid = count && popup_scene_open(&popup, PopupKind::settings_choice, 0x57A00001, {f.items, count}, anchor, viewport, 240, false).ok();
        if (valid) {
            bool changed = false;
            popup_scene_select_index(&popup, 1, &changed);
            valid = renderer_rebuild_content_text(&renderer, sunset) && probe_present(&renderer, sunset) &&
                    capture(L"store-05-installed-menu.bmp", sunset);
            popup_scene_close(&popup);
        }
    }
    // 06 consent over Browse
    plugins();
    permissions(&f.model.consent);
    f.state[0].tab = StoreTab::browse;
    f.model.consent.open = true;
    f.model.consent.reviewed = true;
    f.model.consent.name = utf8_literal("Translate");
    f.model.consent.version = utf8_literal("1.0.0");
    f.model.consent.author = utf8_literal("MaiCord");
    f.model.consent.bytes = 9216;
    f.state[0].dialog_check = true;
    next(L"store-06-consent.bmp", kPlugins, nullptr, sunset);
    // 07 theme detail: installed, not the applied one, so it offers Apply theme and Try it
    themes();
    f.model.items[1].active = false;
    f.state[1].tab = StoreTab::browse;
    {
        StorePageState& st = f.state[1];
        st.detail = true;
        st.detail_from = StoreTab::browse;
        copy_bytes(st.slug, "fjord", 6);
    }
    next(L"store-07-theme-detail.bmp", kThemes, nullptr, sunset);
    f.state[1].detail = false;
    // 13 Themes > Installed: a catalog theme to Apply, and an imported file (an edited copy) in use, read as Applied.
    themes();
    f.model.items[1].active = false;
    {
        StoreItem& local = add(CatalogKind::theme, utf8_literal("~5f3a9c0d7e21b846"), utf8_literal("Sample Neon Grid (edited)"),
                               utf8_literal("A theme file in your MaiCord themes folder."), {}, {}, {}, {});
        local.item = ~0u;
        local.local = local.installed = local.active = true;
        local.order = 2;
        local.has_swatches = true;
        const u32 sw[4]{0x0b0614, 0x140b24, 0xf3e9ff, 0xff3ea5};
        for (u32 i = 0; i < 4; ++i)
            local.swatches[i] = sw[i];
    }
    f.state[1].tab = StoreTab::installed;
    next(L"store-13-themes-installed.bmp", kThemes, nullptr, sunset);
    f.state[1].tab = StoreTab::browse;
    // 08 install from link (an unreviewed effect), previewing live at the Low budget
    effects();
    f.model.items[1].active = false;
    f.state[2].tab = StoreTab::browse;
    f.model.link = {};
    f.model.link.open = true;
    f.model.link.phase = 2;
    f.model.link.kind = CatalogKind::effect;
    f.model.link.host = utf8_literal("gist.githubusercontent.com");
    f.model.link.name = utf8_literal("Plasma Drift");
    f.model.link.authors = utf8_literal("lumen");
    f.model.link.license = utf8_literal("CC-BY-NC-SA-4.0");
    f.model.link.based_on = utf8_literal("Plasma by MaiCord");
    f.model.link.size_text = utf8_literal("18.4 KB");
    f.model.link.cost_known = true;
    f.model.link.cost_level = 0;
    f.model.link.cost_text = utf8_literal("Light \xC2\xB7 0.6 ms");
    f.model.link.previewing = true;
    if (valid) {
        stage = __LINE__;
        valid = run_aurora(renderer);
    }
    next(L"store-08-install-from-link.bmp", kEffects, nullptr, sunset);
    renderer_clear_effect(&renderer);
    f.model.link = {};
    // 09 first load
    effects();
    f.model.catalog = StoreCatalogState::loading;
    f.model.count = 0;
    f.state[2].tab = StoreTab::browse;
    next(L"store-09-loading-skeleton.bmp", kEffects, nullptr, sunset);
    // 10 the catalog is unreachable
    plugins();
    f.model.catalog = StoreCatalogState::unreachable;
    f.model.error_text = utf8_literal("the connection timed out after 15 s");
    f.state[0].tab = StoreTab::browse;
    f.state[0].show_saved = false;
    next(L"store-10-catalog-unavailable.bmp", kPlugins, nullptr, sunset);
    if (!valid)
        log_u64(LogLevel::error, "probe", "store_ui_failure_line", stage);
    log_write(valid ? LogLevel::info : LogLevel::error, "probe", valid ? "store_ui_pages_passed" : "store_ui_failed");
    return valid;
}
} // namespace maicord
