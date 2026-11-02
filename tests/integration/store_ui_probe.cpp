#include "probe_present.hpp"
#include "../support/ui_scene_fixtures.hpp"
#include "settings/catalog.hpp"
#include "store/store_model.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/render/d3d11/renderer_presentation.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/widgets/kit_markdown.hpp"

// The Plugins, Themes and Effects pages through the real settings modal, layout, paint and text paths: one
// capture per screen of store-ui.md (mockups 01-11) at 1440x900 DIP, with the content of the mockups.
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
                           utf8_literal("Chat"), utf8_literal("Privacy"));
    clear.installed = clear.enabled = clear.running = true;
    clear.risk = 1;
    clear.has_settings = true;
    StoreItem& translate = add(CatalogKind::plugin, utf8_literal("translate"), utf8_literal("Translate"),
                               utf8_literal("Translate messages with Google or DeepL from the message menu, or with /tr."),
                               utf8_literal("MaiCord"), utf8_literal("1.0.0"), utf8_literal("Chat"), utf8_literal("Utility"));
    translate.risk = 2;
    translate.sha = utf8_literal("8059942b\xE2\x80\xA6" "6e1018");
    translate.commit = utf8_literal("b76ad03");
    translate.api_level = 6;
    StoreItem& names = add(CatalogKind::plugin, utf8_literal("show-me-your-name"), utf8_literal("Show Me Your Name"),
                           utf8_literal("Shows usernames next to nicknames in chat, so you always know who's who."), utf8_literal("MaiCord"),
                           utf8_literal("1.2.0"), utf8_literal("Appearance"), utf8_literal("Chat"));
    names.installed = names.enabled = names.running = true;
    names.installed_version = utf8_literal("1.1.0");
    names.update = true;
    names.update_version = utf8_literal("1.2.0");
    names.risk = 1;
    StoreItem& reverse = add(CatalogKind::plugin, utf8_literal("reverse-image-search"), utf8_literal("Reverse Image Search"),
                             utf8_literal("Adds a Search image submenu to images: Google Lens, Bing, Yandex and TinEye."), utf8_literal("MaiCord"),
                             utf8_literal("1.0.0"), utf8_literal("Chat"), utf8_literal("Utility"));
    reverse.installed = reverse.enabled = reverse.running = true;
    reverse.has_settings = true;
    reverse.can_roll_back = true;
    reverse.previous_version = utf8_literal("0.9.2");
    reverse.auto_update = true;
    StoreItem& silent = add(CatalogKind::plugin, utf8_literal("silent-toggle"), utf8_literal("Silent Toggle"),
                            utf8_literal("A chat bar button that sends your next message silently, without pinging anyone."), utf8_literal("MaiCord"),
                            utf8_literal("1.0.0"), utf8_literal("Notifications"));
    silent.installed = true;
    silent.risk = 1;
    StoreItem& pin = add(CatalogKind::plugin, utf8_literal("pin-counter"), utf8_literal("Pin Counter"), utf8_literal("Counts pins."),
                         utf8_literal("Lumen"), utf8_literal("0.3.0"), utf8_literal("Utility"));
    pin.installed = pin.enabled = pin.running = true;
    pin.sideloaded = true;
    pin.item = ~0u;
    pin.folder = utf8_literal("D:\\dev\\pin-counter");
    pin.summary = utf8_literal("Loaded from D:\\dev\\pin-counter");
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
        Utf8View slug, name, summary, category;
        u32 sw[4];
        u8 base;
    } kThemes[]{
        {utf8_literal("deep-ocean"), utf8_literal("Deep Ocean"), utf8_literal("A deep navy dark theme with a cool teal accent."), utf8_literal("Blue"),
         {0x0c1a2a, 0x142638, 0xe6eef6, 0x3ec3d6}, 0},
        {utf8_literal("fjord"), utf8_literal("Fjord"), utf8_literal("A muted, frosty blue-grey dark theme in the spirit of the Nord palette."),
         utf8_literal("Muted"), {0x2e3440, 0x363d4b, 0xe5e9f0, 0x88c0d0}, 0},
        {utf8_literal("high-contrast"), utf8_literal("High Contrast"), utf8_literal("Pure black and white with a yellow accent, strong borders and heavier text."),
         utf8_literal("Accessibility"), {0x000000, 0x101010, 0xffffff, 0xffd400}, 0},
        {utf8_literal("oled"), utf8_literal("OLED"), utf8_literal("True black for OLED screens."), utf8_literal("OLED"),
         {0x000000, 0x0a0a0a, 0xf2f2f2, 0x7c5cff}, 0},
        {utf8_literal("dusk"), utf8_literal("Dusk"), utf8_literal("Purple evenings."), utf8_literal("Purple"), {0x1b1530, 0x261e42, 0xece8f7, 0xb28cff}, 0},
        {utf8_literal("paper"), utf8_literal("Paper"), utf8_literal("A warm light theme."), utf8_literal("Warm"), {0xf5f0e6, 0xece5d6, 0x2b2a27, 0xc8742c}, 1},
        {utf8_literal("ember"), utf8_literal("Ember"), utf8_literal("Dark with warm embers."), utf8_literal("Warm"), {0x1c1210, 0x2a1b17, 0xf3e9e4, 0xf0713a}, 0},
    };
    for (const auto& t : kThemes) {
        StoreItem& item = add(CatalogKind::theme, t.slug, t.name, t.summary, utf8_literal("MaiCord"), utf8_literal("1.0.0"), t.category);
        item.base = t.base;
        item.has_swatches = true;
        for (u32 i = 0; i < 4; ++i)
            item.swatches[i] = t.sw[i];
        item.bytes = 1331;
        item.license = utf8_literal("MIT");
    }
    m.items[0].installed = true;
    m.items[1].installed = m.items[1].active = true;
    m.items[1].sha = utf8_literal("c1a09f3e\xE2\x80\xA6" "77b2d0");
    m.items[1].contrast = utf8_literal("WCAG AA for text");
}

void effects() noexcept {
    auto& m = fixture.model;
    reset_value(&m);
    m.catalog = StoreCatalogState::ready;
    m.installable = true;
    StoreItem& aurora = add(CatalogKind::effect, utf8_literal("aurora"), utf8_literal("Aurora"), utf8_literal("Slow curtains of green light."),
                            utf8_literal("MaiCord"), utf8_literal("1.0.0"), utf8_literal("Nature"));
    aurora.installed = aurora.active = aurora.has_knobs = true;
    aurora.library = 0;
    StoreItem& grain = add(CatalogKind::effect, utf8_literal("film-grain"), utf8_literal("Film grain"),
                           utf8_literal("Soft, constantly changing speckle over everything, light and dark, for a filmic feel."), utf8_literal("MaiCord"),
                           utf8_literal("1.0.0"), utf8_literal("Texture"));
    grain.cost = 0;
    add(CatalogKind::effect, utf8_literal("starfield"), utf8_literal("Starfield"), utf8_literal("A quiet drift of stars."), utf8_literal("MaiCord"),
        utf8_literal("1.0.0"), utf8_literal("Space"));
    m.items[0].cost = 0;
    m.items[2].cost = 0;
}

void knobs() noexcept {
    auto& m = fixture.model;
    m.knob_count = 0;
    static constexpr struct {
        Utf8View label, hint;
        u8 type;
        float min, max, value, fallback;
    } kKnobs[]{
        {utf8_literal("Drift speed"), {}, 0, 0, 1, .35f, .3f},
        {utf8_literal("Brightness"), {}, 0, 0, 1, .7f, .6f},
        {utf8_literal("Reach"), utf8_literal("Share of the window the curtains cover."), 0, 0, 1, .75f, .7f},
        {utf8_literal("Curtains"), {}, 1, 1, 5, 3, 3},
    };
    for (const auto& k : kKnobs) {
        StoreKnob& out = m.knobs[m.knob_count++];
        reset_value(&out);
        out.label = k.label;
        out.hint = k.hint;
        out.type = k.type;
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

bool show(Fixture& f, D3D11Renderer& renderer, UiPageKey page, const UiRect& viewport, const wchar_t* name,
          bool (*capture)(const wchar_t*) noexcept, bool (*before)(Fixture&) noexcept = nullptr) noexcept {
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
        if (block && ui_markdown_parse(block, f.model.details_markdown, UiMarkdownOptions{true, text_size_md}, ui_markdown_bank_scratch(f.bank)))
            about = block;
    }
    StoreLayoutInput in{&f.model, &st, nullptr, ui_rect_width(rect), false, about, nullptr};
    store_page_build(&f.store, in);
    f.store.active = true;
    if (!ui_store_scene_commit(&f.store, clip, rect, f.scene.layout.modal))
        return false;
    ++f.scene.revision;
    renderer.settings_store = &f.store;
    renderer.retained_presentation.valid = false;
    // The second frame draws the glyphs the first one rasterised.
    return renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
           probe_present(&renderer, kDefaultTheme) && capture(name);
}
} // namespace

bool store_ui_probe(D3D11Renderer& renderer, PopupScene&, UiTextMeasureCache& cache, bool (*relayout)() noexcept,
                    bool (*capture)(const wchar_t*) noexcept) noexcept {
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
        ~Restore() noexcept {
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
    } restore{renderer, f, popup, old_popup, old_scene, width, height};
    u32 stage = __LINE__;
    bool valid = ui_page_registry_initialize(&f.registry, {f.pages, kSettingsPanelCount + 1});
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
    const auto next = [&](const wchar_t* name, UiPageKey page, bool (*before)(Fixture&) noexcept) noexcept {
        if (valid)
            stage = __LINE__;
        valid = valid && show(f, renderer, page, viewport, name, capture, before);
    };
    // 01 Plugins > Browse
    plugins();
    f.state[0].tab = StoreTab::browse;
    next(L"store-01-browse-plugins.bmp", kPlugins, nullptr);
    // 11 the same page once more (the flat dark theme is what the probe draws with)
    next(L"store-11-browse-plugins-dark.bmp", kPlugins, nullptr);
    // 02 Themes > Browse
    themes();
    f.state[1].tab = StoreTab::browse;
    next(L"store-02-browse-themes.bmp", kThemes, nullptr);
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
    next(L"store-03-plugin-detail.bmp", kPlugins, nullptr);
    f.state[0].detail = false;
    // 04 effect knobs
    effects();
    knobs();
    {
        StorePageState& st = f.state[2];
        st.detail = st.knobs = true;
        st.detail_from = StoreTab::installed;
        copy_bytes(st.slug, "aurora", 7);
    }
    next(L"store-04-effect-knobs.bmp", kEffects, nullptr);
    f.state[2].detail = f.state[2].knobs = false;
    // 05 Plugins > Installed with the overflow menu open
    plugins();
    f.state[0].tab = StoreTab::installed;
    next(L"store-05-installed.bmp", kPlugins, nullptr);
    if (valid) {
        stage = __LINE__;
        UiRect anchor{};
        u32 item = 0, seen = 0;
        for (u32 i = 0; i < f.store.count; ++i)
            if (store_act(f.store.nodes[i].action) == StoreAct::row_overflow && ++seen == 3 &&
                ui_store_scene_rect(f.store, f.store.nodes[i].action, &anchor))
                item = store_arg(f.store.nodes[i].action);
        const u32 count = store_menu_items(f.model, f.state[0], StoreMenu::overflow, item, f.items, 16, {f.labels, sizeof(f.labels)});
        anchor.left = anchor.right - 240.f;
        valid = count && popup_scene_open(&popup, PopupKind::settings_choice, 0x57A00001, {f.items, count}, anchor, viewport, 240, false).ok();
        if (valid) {
            bool changed = false;
            popup_scene_select_index(&popup, 1, &changed);
            valid = renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
                    capture(L"store-05-installed-menu.bmp");
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
    next(L"store-06-consent.bmp", kPlugins, nullptr);
    // 07 theme detail
    themes();
    f.state[1].tab = StoreTab::browse;
    {
        StorePageState& st = f.state[1];
        st.detail = true;
        st.detail_from = StoreTab::browse;
        copy_bytes(st.slug, "fjord", 6);
    }
    f.model.details_markdown = utf8_literal("A muted, frosty blue-grey dark theme.");
    next(L"store-07-theme-detail.bmp", kThemes, nullptr);
    f.state[1].detail = false;
    // 08 install from link (an unreviewed effect)
    effects();
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
    f.model.link.cost_text = utf8_literal("Light \xC2\xB7 0.6 ms");
    f.model.link.previewing = true;
    next(L"store-08-install-from-link.bmp", kEffects, nullptr);
    f.model.link = {};
    // 09 first load
    effects();
    f.model.catalog = StoreCatalogState::loading;
    f.model.count = 0;
    f.state[2].tab = StoreTab::browse;
    next(L"store-09-loading-skeleton.bmp", kEffects, nullptr);
    // 10 the catalog is unreachable
    plugins();
    f.model.catalog = StoreCatalogState::unreachable;
    f.model.error_text = utf8_literal("the connection timed out after 15 s");
    f.state[0].tab = StoreTab::browse;
    f.state[0].show_saved = false;
    next(L"store-10-catalog-unavailable.bmp", kPlugins, nullptr);
    if (!valid)
        log_u64(LogLevel::error, "probe", "store_ui_failure_line", stage);
    log_write(valid ? LogLevel::info : LogLevel::error, "probe", valid ? "store_ui_pages_passed" : "store_ui_failed");
    return valid;
}
} // namespace maicord
