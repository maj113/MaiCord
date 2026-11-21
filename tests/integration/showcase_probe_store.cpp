// The showcase's Plugins, Themes and Effects pages: the catalog's listings (the website's catalog index, written out
// below by hand as store_source.cpp reads an index) through the store's own page builder, in the settings modal over
// the window, with the catalog's preview pictures decoded and uploaded as the host's pump does
// (MAICORD_SHOWCASE_PREVIEWS names the folder of previews/<sha256>.png). Nothing is installed: every card offers
// Install, as a new profile sees the catalog.
#include "showcase_probe_internal.hpp"
#include "settings/catalog.hpp"
#include "store/store_model.hpp"
#include "store/store_preview.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/settings/scene.hpp"

namespace maicord::showcase_probe_detail {
using namespace app_detail;
namespace {

struct CatalogEntry {
    CatalogKind kind;
    const char *slug, *name, *summary, *authors, *version, *license;
    const char* categories[3];
    u64 released;
    u32 bytes;
    u64 permissions; // plugins: MC_PERM_* bits of the newest version
    bool hosts;      // plugins: the version lists network hosts
    u32 api_level;
    u8 base, cost;
    bool has_swatches;
    u32 swatches[4];
    const char* effect;  // a theme's effect
    const char* preview; // the first preview's SHA-256, hex; empty for none
};
constexpr CatalogEntry kCatalog[]{
    {CatalogKind::plugin, "blur-nsfw", "Blur NSFW",
     "Blurs the images and videos of messages in age-restricted channels.",
     "MaiCord", "1.0.0", "MIT", {"privacy", "appearance", ""},
     1793591998u, 4824u, 0x2u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "clear-urls", "Clear URLs",
     "Removes tracking parameters from links you send.",
     "Juniper Vale", "1.0.0", "MPL-2.0", {"privacy", "chat", ""},
     1793578370u, 10888u, 0x1u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "custom-rpc", "Custom RPC",
     "Sets your own activity: a type, a name, two lines of text and an optional elapsed timer.",
     "MaiCord", "1.0.0", "MIT", {"fun", "utility", ""},
     1793591998u, 7184u, 0x80u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "member-count", "Member Count",
     "Shows how many members are online and how many there are above the member list.",
     "MaiCord", "1.0.0", "MIT", {"chat", "utility", ""},
     1793594609u, 9480u, 0x0u, false, 1u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "message-click-actions", "Message Click Actions",
     "Double-click a message to edit your own or reply to someone else's, or delete yours with a modified click.",
     "MaiCord", "1.0.0", "MIT", {"chat", "utility", ""},
     1793592523u, 7488u, 0x62u, false, 1u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "mutual-group-dms", "Mutual Group DMs",
     "Lists the group DMs you share with someone on their profile.",
     "MaiCord", "1.0.0", "MIT", {"chat", "utility", ""},
     1793594609u, 8608u, 0x2u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "no-blocked-messages", "No Blocked Messages",
     "Hides the messages of users you blocked instead of showing a blocked-messages row.",
     "MaiCord", "1.0.0", "MIT", {"chat", "moderation", ""},
     1793591998u, 4376u, 0x2u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "no-reply-mention", "No Reply Mention",
     "Turns off the ping of your replies.",
     "MaiCord", "1.0.0", "MIT", {"chat", "notifications", ""},
     1793593520u, 4296u, 0x1u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "permissions-viewer", "Permissions Viewer",
     "Shows what a member can do in a server and how a channel changes it.",
     "MaiCord", "1.0.0", "MIT", {"moderation", "utility", ""},
     1793592524u, 13792u, 0x0u, false, 1u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "preview-message", "Preview Message",
     "Shows what you've typed as a message only you see.",
     "MaiCord", "1.0.0", "MIT", {"chat", "utility", ""},
     1793593521u, 4480u, 0x20u, false, 1u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "quick-reply", "Quick Reply",
     "Walk the message you reply to up and down the channel with Ctrl+Up and Ctrl+Down.",
     "MaiCord", "1.0.0", "MIT", {"chat", "utility", ""},
     1793592523u, 5456u, 0x22u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "read-all-notifications-button", "Read All Notifications Button",
     "A button above the server list that marks every server as read.",
     "MaiCord", "1.0.0", "MIT", {"notifications", "utility", ""},
     1793592524u, 5368u, 0x40u, false, 1u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "relationship-notifier", "Relationship Notifier",
     "Tells you when a friend removes you, withdraws a request, or you lose a server or group chat.",
     "MaiCord", "1.0.0", "MIT", {"notifications", "utility", ""},
     1793591998u, 7928u, 0x8u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "send-timestamps", "Send Timestamps",
     "Inserts a Discord timestamp from a date and time you type.",
     "MaiCord", "1.0.0", "MIT", {"chat", "utility", ""},
     1793593521u, 12760u, 0x20u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "server-info", "Server Info",
     "A server's age, owner, size, boosts and your permissions from its context menu.",
     "MaiCord", "1.0.0", "MIT", {"utility", "", ""},
     1793592524u, 14632u, 0x100u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "show-me-your-name", "Show Me Your Name",
     "Shows usernames beside display names and nicknames.",
     "Juniper Vale", "1.0.0", "MPL-2.0", {"chat", "appearance", ""},
     1793578370u, 5416u, 0x0u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "silent-toggle", "Silent Toggle",
     "Sends your messages as @silent while it's on.",
     "Juniper Vale", "1.0.0", "MPL-2.0", {"chat", "notifications", ""},
     1793578370u, 6344u, 0x1u, false, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "silent-typing", "Silent Typing",
     "Doesn't tell others when you're typing.",
     "MaiCord", "1.0.0", "MIT", {"privacy", "chat", ""},
     1793593520u, 6584u, 0x1u, false, 1u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "text-replace", "Text Replace",
     "Rewrites what you send with your own find-and-replace rules.",
     "MaiCord", "1.0.0", "MIT", {"chat", "utility", ""},
     1793593521u, 17008u, 0x1u, false, 1u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "translate", "Translate",
     "Translates messages with Google's free endpoint or DeepL.",
     "Juniper Vale", "1.0.0", "MPL-2.0", {"chat", "utility", ""},
     1793578370u, 21232u, 0x33u, true, 2u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "typing-indicator", "Typing Indicator",
     "Marks the channels, DMs and servers where someone is typing.",
     "MaiCord", "1.0.0", "MIT", {"chat", "notifications", ""},
     1793594609u, 12528u, 0x2u, false, 1u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::plugin, "view-raw", "View Raw",
     "Shows a message's raw text or its details as JSON, and copies either.",
     "MaiCord", "1.0.0", "MIT", {"developer", "utility", ""},
     1793592524u, 14208u, 0x102u, false, 1u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", ""},
    {CatalogKind::theme, "deep-ocean", "Deep Ocean",
     "A deep navy dark theme with a cool teal accent.",
     "MaiCord", "1.0.0", "CC0-1.0", {"blue", "cool", ""},
     1793578370u, 1211u, 0x0u, false, 0u, 0, 0, true, {0x0b1a29, 0x10253a, 0xe4f1f8, 0x22b8cf},
     "", "429bf0b6c82a403f3909b583837cf2010e6cdd539efb8c631cb2b240d9c778d2"},
    {CatalogKind::theme, "fjord", "Fjord",
     "A muted, frosty blue-grey dark theme in the spirit of the Nord palette.",
     "MaiCord", "1.0.0", "MIT", {"muted", "cool", ""},
     1793578370u, 1313u, 0x0u, false, 0u, 0, 0, true, {0x2e3440, 0x363d4b, 0xe5e9f0, 0x88c0d0},
     "", "b5800f26df7e34131295ba2ca64a77ad8a50e8eb18a594abe90381b9fe86487f"},
    {CatalogKind::theme, "high-contrast", "High Contrast",
     "Pure black and white with a yellow accent, strong borders and heavier icons. Passes WCAG AAA for text.",
     "MaiCord", "1.0.0", "CC0-1.0", {"accessibility", "", ""},
     1793578370u, 1652u, 0x0u, false, 0u, 0, 0, true, {0x000000, 0x161616, 0xffffff, 0xffd23f},
     "", "ccb844638e2dfcea33a1a68fab9ed33004336e77516e1cfacbd8407814812c87"},
    {CatalogKind::theme, "neon-grid", "Neon Grid",
     "Deep violet-black surfaces, a hot-pink accent and a faint drift of stars.",
     "MaiCord", "1.0.0", "CC0-1.0", {"neon", "pink", "purple"},
     1793647809u, 1545u, 0x0u, false, 0u, 0, 0, true, {0x0c0414, 0x160a24, 0xfdf0fa, 0xff2e97},
     "starfield", "d82aa36f838ab35652db6be3b67049f62434be7d89407d1b042edfff094dc64e"},
    {CatalogKind::theme, "obsidian", "Obsidian",
     "True black chat and surfaces for OLED panels, with neutral greys and an emerald accent.",
     "MaiCord", "1.0.0", "CC0-1.0", {"oled", "", ""},
     1793578370u, 1209u, 0x0u, false, 0u, 0, 0, true, {0x000000, 0x101011, 0xececee, 0x34d399},
     "", "55397c10d7f75cfd960c59643ed03b7a65f425d40ff86d33822352f0920e1ff4"},
    {CatalogKind::theme, "rose-dusk", "Rose Dusk",
     "Deep plum surfaces with a soft rose accent, like the sky just after sunset.",
     "MaiCord", "1.0.0", "CC0-1.0", {"warm", "purple", ""},
     1793578370u, 1210u, 0x0u, false, 0u, 0, 0, true, {0x241540, 0x321f4d, 0xf6ecf8, 0xf08fb8},
     "", "71eb1908781d58c313e2bcaf36469a52ceb274bbf2be32e604edfa5aa8afff47"},
    {CatalogKind::theme, "silk-waves", "Silk Waves",
     "Follows the Silk Waves sky through the day and the year: near-black with the month's glow at night, soft month-tinted light by day, turning ",
     "MaiCord", "1.1.0", "MIT", {"ambient", "dynamic", ""},
     1793988624u, 15433u, 0x0u, false, 0u, 0, 0, true, {0x14120f, 0x1a1814, 0xf2ede6, 0x9c6008},
     "silk-waves", "f101831ef853b13c9edbd816a6c051e96e16d048b04f1e46fd88c29a01330bd0"},
    {CatalogKind::theme, "warm-paper", "Warm Paper",
     "A warm, cream-coloured light theme with a terracotta accent, easy on the eyes.",
     "MaiCord", "1.0.0", "CC0-1.0", {"warm", "cream", ""},
     1793578370u, 1347u, 0x0u, false, 0u, 1, 0, true, {0xf7f0e1, 0xfdf8ec, 0x2b2118, 0xb04a24},
     "", "e4a97cd56c371ecd9ab524ebf9bcb77097342a117c1b7becf45678cbcc8c0241"},
    {CatalogKind::effect, "ascii-rain", "ASCII rain",
     "Columns of glyphs falling down the window, a bright head with a fading trail.",
     "MaiCord", "1.0.0", "MPL-2.0", {"retro", "text", ""},
     1793578370u, 7730u, 0x0u, false, 0u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", "5eeca6d60e3498ed71eca4ca5ff094442b939e3198dc378052bee8e258d1deda"},
    {CatalogKind::effect, "aurora", "Aurora",
     "Soft curtains of light drifting across the top of the window, tinted by the theme's accent and positive roles over its background.",
     "MaiCord", "1.0.0", "MPL-2.0", {"ambient", "nature", ""},
     1793578370u, 10327u, 0x0u, false, 0u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", "ad957247dc213ad6aab1257c76c7bfa5b7885c5d952ed9eb4e444f30fc509978"},
    {CatalogKind::effect, "bokeh", "Soft bokeh",
     "Out-of-focus discs drifting slowly upward in three depth layers, in the theme's accent and text colours.",
     "MaiCord", "1.0.0", "MPL-2.0", {"ambient", "soft", ""},
     1793578370u, 10877u, 0x0u, false, 0u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", "6895201cf175b9f6b45c89778ef74a35ad7c7cbf6c7172049d3d4d32b28d126b"},
    {CatalogKind::effect, "film-grain", "Film grain",
     "A faint, constantly changing speckle over everything, light and dark, for a printed-paper feel.",
     "MaiCord", "1.0.0", "MPL-2.0", {"texture", "retro", ""},
     1793578370u, 3878u, 0x0u, false, 0u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", "45ab73aaef4f34dd4748b5256ea856e125bcde8c11a1ea4c646c8fdb646e5fe7"},
    {CatalogKind::effect, "plasma", "Plasma",
     "The classic sum of sines, tinted toward the theme so it never fights the UI.",
     "MaiCord", "1.0.0", "MPL-2.0", {"abstract", "retro", ""},
     1793578370u, 4841u, 0x0u, false, 0u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", "f99e9fce4589dfda30d85d553a870bd40e55e49c6e95ff0ba2a86d155b2a895b"},
    {CatalogKind::effect, "silk-waves", "Silk Waves",
     "A silky ribbon and glitter inspired by the PS3 menu background: a simulated cloth ribbon that folds and glows over a monthly day or night sk",
     "MaiCord", "1.4.0", "MIT", {"ambient", "retro", ""},
     1793988624u, 133152u, 0x0u, false, 0u, 0, 1, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", "f101831ef853b13c9edbd816a6c051e96e16d048b04f1e46fd88c29a01330bd0"},
    {CatalogKind::effect, "starfield", "Starfield",
     "Three parallax layers of twinkling stars drifting sideways, sized in DIPs so they look the same at every DPI and resolution.",
     "MaiCord", "1.0.0", "MPL-2.0", {"space", "ambient", ""},
     1793578370u, 10781u, 0x0u, false, 0u, 0, 0, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", "58e8e3d141d5400a0e54ca1e8c4c0f76cfa85f12cdfd116a05ddfbbd90718241"},
    {CatalogKind::effect, "wave-lines", "Wave lines",
     "A stack of thin horizontal lines rippling with a few overlaid sines, fading from the theme's accent to its positive colour down the window.",
     "MaiCord", "1.0.0", "MPL-2.0", {"abstract", "minimal", ""},
     1793578370u, 6299u, 0x0u, false, 0u, 0, 1, false, {0x000000, 0x000000, 0x000000, 0x000000},
     "", "676fbd531930746b1819b6d2e702f5d66307a93a28011408f7cba618f2cef129"},
};
constexpr u32 kCatalogCount = sizeof(kCatalog) / sizeof(kCatalog[0]);

Utf8View text(const char* s) noexcept {
    return {s, c_string_length(s)};
}

StoreModel g_model;
StorePageState g_page;
UiStoreScene g_scene;
UiStoreMeasure g_measure;
wchar_t g_previews[512];
u32 g_dpi = 96;
u8 g_file[512 * 1024];

u32 hex_digit(char c) noexcept {
    return c >= '0' && c <= '9' ? static_cast<u32>(c - '0') : static_cast<u32>(c - 'a' + 10);
}
// The SHA-256 of the preview of the item in `slot` (its index + 1).
bool preview_sha(u32 slot, u8 (&sha)[32], const char** hex) noexcept {
    if (!slot || slot > g_model.count)
        return false;
    const char* preview = kCatalog[g_model.items[slot - 1].item].preview;
    if (c_string_length(preview) != 64)
        return false;
    for (u32 i = 0; i < 32; ++i)
        sha[i] = static_cast<u8>(hex_digit(preview[i * 2]) << 4 | hex_digit(preview[i * 2 + 1]));
    *hex = preview;
    return true;
}
bool preview_key(const void*, u32 slot, const UiRect& rect, MediaAssetKey* key, u8* domain) noexcept {
    u8 sha[32];
    const char* hex = nullptr;
    if (!g_previews[0] || !preview_sha(slot, sha, &hex))
        return false;
    *key = store_preview_key(sha, store_preview_pixels(ui_rect_width(rect), g_dpi), false);
    *domain = 1;
    return true;
}
// What the host's pump does for the visible tiles: read the file, decode it to the tile's size and upload it.
bool upload_previews(D3D11Renderer& r) noexcept {
    UiStoreMedia media{};
    u32 cursor = 0;
    bool ok = true;
    while (ui_store_scene_next_media(g_scene, &cursor, &media)) {
        u8 sha[32];
        const char* hex = nullptr;
        if (media_atlas_peek(r.preview_media_atlas, media.key).ok() || !preview_sha(media.slot, sha, &hex))
            continue;
        wchar_t path[600];
        u32 n = 0;
        for (; g_previews[n] && n < 500; ++n)
            path[n] = g_previews[n];
        path[n++] = L'\\';
        for (u32 i = 0; i < 64; ++i)
            path[n++] = static_cast<wchar_t>(hex[i]);
        for (const wchar_t c : L".png")
            path[n++] = c;
        const HANDLE file =
            CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD read = 0;
        const bool loaded = file != INVALID_HANDLE_VALUE && ReadFile(file, g_file, sizeof(g_file), &read, nullptr) && read;
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        StorePreviewImage image{};
        if (!loaded || !store_preview_decode({g_file, read}, media.key.variant & 0x1fffffffu, &image)) {
            log_u64(LogLevel::error, "showcase", "store_preview_failed_bytes", read);
            ok = false;
            continue;
        }
        ok = renderer_upload_media_straight_rgba8(&r, media.key, static_cast<u16>(image.width),
                                                  static_cast<u16>(image.height), image.width * 4u,
                                                  {image.pixels, image.width * image.height * 4u})
                 .ok() &&
             ok;
        store_preview_free(&image);
    }
    return ok;
}

// The listings of one kind as store_model_fill writes them from the index.
void fill(CatalogKind kind) noexcept {
    auto& m = g_model;
    reset_value(&m);
    m.catalog = StoreCatalogState::ready;
    m.checked_unix = m.fetched_unix = 1'794'075'024;
    for (u32 i = 0; i < kCatalogCount && m.count < kStoreItemMaximum; ++i) {
        const CatalogEntry& e = kCatalog[i];
        if (e.kind != kind)
            continue;
        StoreItem& out = m.items[m.count++];
        reset_value(&out);
        out.kind = e.kind;
        out.item = i;
        out.slug = text(e.slug);
        out.name = text(e.name);
        out.summary = text(e.summary);
        out.authors = text(e.authors);
        out.version = text(e.version);
        out.license = text(e.license);
        for (const char* category : e.categories)
            if (*category)
                out.categories[out.category_count++] = text(category);
        out.released = e.released;
        out.bytes = e.bytes;
        out.api_level = e.api_level;
        if (kind == CatalogKind::plugin)
            out.risk = store_permission_risk(e.permissions, e.hosts);
        out.cost = e.cost;
        out.base = e.base;
        out.has_swatches = e.has_swatches;
        for (u32 s = 0; s < 4; ++s)
            out.swatches[s] = e.swatches[s];
        out.effect_slug = text(e.effect);
        out.library = out.file = ~0u;
        out.installable = true;
    }
}
} // namespace

bool showcase_store_page(D3D11Renderer& r, UiSettingsScene* settings, UiPageRegistry* registry, CatalogKind kind,
                         const UiRect& viewport) noexcept {
    const DWORD length = GetEnvironmentVariableW(L"MAICORD_SHOWCASE_PREVIEWS", g_previews, 500);
    if (!length || length >= 500)
        g_previews[0] = 0;
    g_dpi = r.dpi;
    if (!g_scene.nodes && !ui_store_scene_open(&g_scene))
        return false;
    g_measure = ui_store_cache_measure(r.message_measure_cache);
    g_scene.measure = &g_measure;
    const SettingsPanel panel = kind == CatalogKind::plugin  ? SettingsPanel::plugins
                                : kind == CatalogKind::theme ? SettingsPanel::themes
                                                             : SettingsPanel::effects;
    UiRect rect{}, clip{};
    if (!ui_page_registry_select(registry, {kSettingsNativeOwner, static_cast<u64>(panel) + 1}).ok() ||
        !ui_settings_scene_update(settings, registry, viewport) || !ui_settings_scene_host_rect(*settings, &rect, &clip))
        return false;
    fill(kind);
    store_page_reset(&g_page, kind);
    g_page.tab = StoreTab::browse;
    ui_store_scene_begin(&g_scene);
    store_page_build(&g_scene, {&g_model, &g_page, nullptr, ui_rect_width(rect), nullptr, nullptr});
    g_scene.active = true;
    if (!ui_store_scene_commit(&g_scene, clip, rect, settings->layout.modal))
        return false;
    g_scene.media_key = preview_key;
    ++settings->revision;
    r.settings_store = &g_scene;
    r.retained_presentation.valid = false;
    return upload_previews(r);
}

void showcase_store_close(D3D11Renderer& r) noexcept {
    r.settings_store = nullptr;
}
} // namespace maicord::showcase_probe_detail
