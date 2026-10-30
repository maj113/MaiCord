#include "application_expression_card.hpp"
#include "application_account_presentation.hpp"
#include "application_content_input.hpp"
#include "application_cursor.hpp"
#include "application_media_picker.hpp"
#include "application_shell_input.hpp"
#include "application_state.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/app/account_media_catalog.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/protocol/media_catalog.hpp"
#include "maicord/ui/cursor.hpp"
#include "maicord/ui/expression_card_scene.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
// GET /stickers/{id}, GET /sticker-packs, GET /sticker-packs/{id}.
enum class Lookup : u8 { none, sticker, packs, pack };
// How the popout describes a sticker once its definition is known.
enum class StickerView : u8 { loading, guild, premium_pack, retired_pack, unknown };
constexpr u32 kKnownStickers = 16;
constexpr u8 kRateRetries = 2;
constexpr u64 kLocalRateRetryMs = 1000;
constexpr u32 kPackPreviewWindow = 4; // The official popout picks from the pack's first four.

struct Bank {
    u32 count;
    MediaCatalogItem* items;
};
struct KnownSticker {
    u64 id, pack_id, guild_id;
};
struct State {
    UiExpressionCardScene scene;
    u64 scope;
    MessageExpression source;
    bool open, control_pressed;
    UiExpressionCardControl pressed_control;
    // The popout's copies: a Unicode glyph borrowed from message text and
    // the name read at opening.
    char glyph[48], name[kUiExpressionCardTitleBytes];
    u32 glyph_bytes, name_bytes;
    // Sticker lookups for the open popout; results stay for the session.
    Lookup wanted, submitted_lookup;
    u64 wanted_id, submitted_object, submitted_id, due_at;
    u8 retries;
    bool packs_failed;
    u64 failed_sticker, failed_pack;
    KnownSticker known[kKnownStickers];
    u32 known_next;
    Bank* packs;
    Bank* retired; // The last GET /sticker-packs/{id}.
    // The projection's targets.
    u64 link_guild, view_pack;
    char title[kUiExpressionCardTitleBytes], description[kUiExpressionCardTextBytes], acronym[24];
    MediaAssetKey previews[kUiExpressionCardPreviews];
    u32 preview_count;
};
State* g_card;
// The press that may become a click.
MessageExpression g_press;

bool available() noexcept {
    return g_app.account_ui && !g_app.login.visible;
}
bool open_now() noexcept {
    return g_card && g_card->open && g_card->scene.active;
}
bool reduced_motion() noexcept {
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    return !animations || g_app.preferences.reduced_motion;
}
void free_bank(Bank** bank) noexcept {
    if (*bank)
        VirtualFree(*bank, 0, MEM_RELEASE);
    *bank = nullptr;
}
bool ensure() noexcept {
    if (!g_card) {
        g_card = static_cast<State*>(VirtualAlloc(nullptr, sizeof(State), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g_card)
            return false;
        g_card->scope = g_app.plugin_account_scope_serial;
    }
    if (g_card->scope != g_app.plugin_account_scope_serial) {
        // Another account: no popout, lookup or pack of the previous one survives.
        application_expression_card_close();
        free_bank(&g_card->packs);
        free_bank(&g_card->retired);
        const auto scene = g_card->scene;
        clear_bytes(g_card, static_cast<u32>(sizeof(State)));
        g_card->scene = scene;
        g_card->scope = g_app.plugin_account_scope_serial;
    }
    return true;
}

struct Text {
    char* data;
    u32 capacity, used;
    bool ok;
    void add(Utf8View text) noexcept {
        if (!ok || text.bytes > capacity - used) {
            ok = false;
            return;
        }
        copy_bytes(data + used, text.data, text.bytes);
        used += text.bytes;
    }
};

// Sticker definitions: a joined server's list, the pack list, the last
// retired pack, or an earlier GET /stickers/{id}.
const MediaCatalogItem* bank_sticker(const Bank* bank, u64 id) noexcept {
    for (u32 i = 0; bank && i < bank->count; ++i)
        if (bank->items[i].sticker_id == id)
            return &bank->items[i];
    return nullptr;
}
const MediaCatalogItem* bank_pack(const Bank* bank, u64 pack_id) noexcept {
    for (u32 i = 0; bank && pack_id && i < bank->count; ++i)
        if (bank->items[i].pack_id == pack_id)
            return &bank->items[i];
    return nullptr;
}
bool definition(u64 id, KnownSticker* out) noexcept {
    if (const u64 guild = application_media_picker_sticker_server(id)) {
        *out = {id, 0, guild};
        return true;
    }
    const auto& c = *g_card;
    const auto* item = bank_sticker(c.packs, id);
    if (!item)
        item = bank_sticker(c.retired, id);
    if (item) {
        *out = {id, item->pack_id, item->guild_id};
        return true;
    }
    for (const auto& known : c.known)
        if (known.id == id) {
            *out = known;
            return true;
        }
    return false;
}
void want(Lookup lookup, u64 id) noexcept {
    auto& c = *g_card;
    if (c.wanted == lookup && c.wanted_id == id)
        return;
    c.wanted = lookup;
    c.wanted_id = id;
    c.retries = 0;
    c.due_at = 0;
}
// Resolves the open sticker, asking for what is missing.
StickerView sticker_view(KnownSticker* sticker) noexcept {
    auto& c = *g_card;
    const u64 id = c.source.id;
    if (!definition(id, sticker)) {
        if (c.failed_sticker == id)
            return StickerView::unknown;
        want(Lookup::sticker, id);
        return StickerView::loading;
    }
    if (sticker->guild_id)
        return StickerView::guild;
    if (!sticker->pack_id)
        return StickerView::unknown;
    if (!c.packs) {
        if (c.packs_failed)
            return StickerView::unknown;
        want(Lookup::packs, 0);
        return StickerView::loading;
    }
    // A pack the list offers is a premium pack; one it lacks has been retired.
    if (bank_pack(c.packs, sticker->pack_id))
        return StickerView::premium_pack;
    if (bank_pack(c.retired, sticker->pack_id))
        return StickerView::retired_pack;
    if (c.failed_pack == sticker->pack_id)
        return StickerView::unknown;
    want(Lookup::pack, sticker->pack_id);
    return StickerView::loading;
}

u64 current_guild() noexcept {
    const auto& ui = *g_app.account_ui;
    const auto* channel = chat_state_store_find_channel(ui.chat_state, ui.active_channel_id);
    return channel ? channel->guild_id : 0;
}
// The source server below the head: joined servers only (MaiCord fetches no
// source data for other servers). The official card reads "Invite-Only
// Server" for a joined server unless it fetched discovery data.
void guild_section(const GuildRecord& guild, Utf8View heading, UiExpressionCardContent* content) noexcept {
    auto& c = *g_card;
    const auto& state = g_app.account_ui->chat_state;
    const Utf8View name = chat_state_store_guild_name(state, guild);
    content->guild_heading = heading;
    content->guild_name = name;
    content->guild_subtitle = utf8_literal("Invite-Only Server");
    content->guild_link = true;
    c.link_guild = guild.id;
    if (chat_state_store_guild_icon_hash(guild).bytes && guild.icon_variant)
        content->guild_icon = media_asset_guild_icon(guild.id, guild.icon_variant, false);
    else {
        u32 characters = 0;
        content->guild_acronym = ui_guild_acronym(name, {c.acronym, sizeof(c.acronym)}, &characters);
    }
}

// The official sentences for a custom emoji: whether it is from this server,
// one of yours or an unknown one, and whether Nitro carries it everywhere.
Utf8View emoji_description(bool joined, bool current, bool premium) noexcept {
    if (!joined)
        return utf8_literal("This emoji is from a server that is either invite-only or unavailable.");
    if (premium)
        return current ? utf8_literal("This emoji is from this server. You can use it everywhere.")
                       : utf8_literal("This emoji is from one of your servers. Type its name in the chat bar to use it.");
    return current ? utf8_literal("This emoji is from this server. Get Nitro to use it everywhere.")
                   : utf8_literal("This emoji is from one of your servers. Get Nitro to use it everywhere.");
}
Utf8View sticker_description(bool joined, bool current, bool premium) noexcept {
    if (!joined)
        return utf8_literal("This sticker is from a server that is either invite-only or unavailable.");
    if (premium)
        return current
                   ? utf8_literal("This sticker is from this server. You can use it everywhere.")
                   : utf8_literal("This sticker is from one of your servers. Type its name in the chat bar to use it.");
    return current ? utf8_literal("This sticker is from this server. Get Nitro to use it everywhere.")
                   : utf8_literal("This sticker is from one of your servers. Get Nitro to use it everywhere.");
}

bool emoji_content(UiExpressionCardContent* content) noexcept {
    auto& c = *g_card;
    const auto& e = c.source;
    const auto& state = g_app.account_ui->chat_state;
    content->layout = UiExpressionCardLayout::head;
    content->title = {c.name, c.name_bytes};
    if (e.kind != MessageExpressionKind::custom_emoji) {
        if (e.kind == MessageExpressionKind::regional_flag)
            content->picture = media_asset_regional_flag(static_cast<u16>(e.id));
        else
            content->glyph = {c.glyph, c.glyph_bytes};
        // The official English title keeps its joke for one emoji.
        if (bytes_equal(c.name, ":pizza:", 7) && c.name_bytes == 7) {
            Text title{c.title, sizeof(c.title), 0, true};
            title.add({c.name, c.name_bytes});
            title.add(utf8_literal(" (without pineapple)"));
            if (title.ok)
                content->title = {c.title, title.used};
        }
        content->description = utf8_literal("A default emoji. You can use this emoji everywhere on Discord.");
        return true;
    }
    content->picture = media_asset_custom_emoji(e.id, e.animated);
    const auto* emoji = chat_state_store_find_emoji(state, e.id);
    const auto* guild = emoji ? chat_state_store_find_guild(state, emoji->guild_id) : nullptr;
    // Custom emoji go everywhere with any Nitro tier.
    content->description = emoji_description(guild != nullptr, guild && guild->id == current_guild(),
                                             state.self_premium_type != 0);
    if (guild)
        guild_section(*guild, utf8_literal("This emoji is from"), content);
    return true;
}

// Up to three of the pack's first four stickers, without this one.
void pack_previews(const Bank* bank, u64 pack_id, u64 sticker_id) noexcept {
    auto& c = *g_card;
    c.preview_count = 0;
    u32 seen = 0;
    for (u32 i = 0; bank && i < bank->count && seen < kPackPreviewWindow &&
                    c.preview_count < kUiExpressionCardPreviews;
         ++i) {
        const auto& item = bank->items[i];
        if (item.pack_id != pack_id)
            continue;
        ++seen;
        if (item.sticker_id != sticker_id)
            c.previews[c.preview_count++] = media_asset_sticker(item.sticker_id, item.sticker_format);
    }
}

bool sticker_content(UiExpressionCardContent* content) noexcept {
    auto& c = *g_card;
    const auto& e = c.source;
    const auto& state = g_app.account_ui->chat_state;
    KnownSticker sticker{};
    const StickerView view = sticker_view(&sticker);
    if (view != StickerView::loading)
        want(Lookup::none, 0);
    content->title = {c.name, c.name_bytes};
    switch (view) {
    case StickerView::loading:
        content->layout = UiExpressionCardLayout::loading;
        return true;
    case StickerView::premium_pack:
    case StickerView::retired_pack: {
        const Bank* bank = view == StickerView::premium_pack ? c.packs : c.retired;
        const auto* pack = bank_pack(bank, sticker.pack_id);
        content->layout = UiExpressionCardLayout::pack;
        Text text{c.description, sizeof(c.description), 0, true};
        text.add(utf8_literal("This is a "));
        content->emphasis_offset = text.used;
        text.add(pack->pack_name);
        content->emphasis_bytes = pack->pack_name.bytes;
        text.add(view == StickerView::premium_pack ? utf8_literal(" sticker.")
                                                   : utf8_literal(" sticker. They are no longer available."));
        if (!text.ok)
            return false;
        content->description = {c.description, text.used};
        pack_previews(bank, sticker.pack_id, e.id);
        for (u32 i = 0; i < c.preview_count; ++i)
            content->previews[i] = c.previews[i];
        content->preview_count = c.preview_count;
        // Only an offered pack opens in the picker.
        if (view == StickerView::premium_pack) {
            content->button = utf8_literal("View More Stickers");
            c.view_pack = sticker.pack_id;
        }
        return true;
    }
    default:
        break;
    }
    // A server sticker, or one whose definition could not be read.
    content->layout = UiExpressionCardLayout::head;
    content->picture = media_asset_sticker(e.id, e.sticker_format);
    const auto* guild = view == StickerView::guild ? chat_state_store_find_guild(state, sticker.guild_id) : nullptr;
    // Server stickers go everywhere with Nitro or Nitro Basic.
    const bool premium = state.self_premium_type == 2 || state.self_premium_type == 3;
    content->description = sticker_description(guild != nullptr, guild && guild->id == current_guild(), premium);
    if (guild)
        guild_section(*guild, utf8_literal("This sticker is from"), content);
    return true;
}

bool project() noexcept {
    auto& c = *g_card;
    if (!c.open || !available())
        return false;
    c.link_guild = c.view_pack = 0;
    c.preview_count = 0;
    UiExpressionCardContent content{};
    const bool built =
        c.source.kind == MessageExpressionKind::sticker ? sticker_content(&content) : emoji_content(&content);
    const auto& e = c.source;
    const u64 identity = hash64_bytes(hash64_bytes(kHash64Seed, &e.message_id, sizeof(e.message_id)), &e.source,
                                      sizeof(e.source));
    if (!built || !ui_expression_card_show(&c.scene, identity, content, e.anchor, application_viewport_dips(),
                                           clock_monotonic_milliseconds(), reduced_motion(),
                                           &g_app.account_ui->text_measure_cache)) {
        application_expression_card_close();
        return false;
    }
    g_app.renderer.expression_card_scene = &c.scene;
    application_request_frame();
    return true;
}

void activate(UiExpressionCardControl control) noexcept {
    auto& c = *g_card;
    if (control == UiExpressionCardControl::guild && c.link_guild) {
        const u64 guild = c.link_guild;
        application_expression_card_close();
        select_content_guild(guild);
    } else if (control == UiExpressionCardControl::button && c.view_pack) {
        const u64 pack = c.view_pack;
        application_expression_card_close();
        application_media_picker_open_sticker_pack(pack);
    }
}

Bank* decode_bank(MediaCatalogOperation operation, Utf8View body) noexcept {
    auto* scratch = static_cast<u32*>(VirtualAlloc(nullptr, kMediaCatalogStructuralCapacity * sizeof(u32),
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!scratch)
        return nullptr;
    Bank* bank = nullptr;
    const auto measured = media_catalog_measure(operation, body, {scratch, kMediaCatalogStructuralCapacity}, 32);
    if (measured.ok() && measured.item_count) {
        const u32 bytes = sizeof(Bank) + measured.item_count * sizeof(MediaCatalogItem) + measured.text_bytes;
        bank = static_cast<Bank*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (bank) {
            bank->items = reinterpret_cast<MediaCatalogItem*>(bank + 1);
            const auto decoded =
                media_catalog_decode(operation, body, {scratch, kMediaCatalogStructuralCapacity}, 32,
                                     {bank->items, measured.item_count},
                                     {reinterpret_cast<char*>(bank->items + measured.item_count), measured.text_bytes});
            bank->count = decoded.item_count;
            if (!decoded.ok() || !decoded.item_count)
                free_bank(&bank);
        }
    }
    VirtualFree(scratch, 0, MEM_RELEASE);
    return bank;
}
} // namespace

void application_expression_card_pointer_down(UiPoint point) noexcept {
    g_press = {};
    if (available())
        application_message_expression_at(point, &g_press);
}

void application_expression_card_pointer_up(UiPoint point) noexcept {
    const MessageExpression pressed = g_press;
    g_press = {};
    MessageExpression released{};
    if (pressed.kind == MessageExpressionKind::none || !available() ||
        !application_message_expression_at(point, &released) ||
        !application_message_expression_same(pressed, released))
        return;
    // A drag from the emoji selected text instead.
    if (g_app.message_text_selection.valid && g_app.message_text_selection_anchor != g_app.message_text_selection_caret)
        return;
    application_expression_card_open(released);
}

bool application_expression_card_open(const MessageExpression& expression) noexcept {
    if (!available() || expression.kind == MessageExpressionKind::none || !ensure())
        return false;
    auto& c = *g_card;
    if (open_now() && application_message_expression_same(c.source, expression)) {
        // A second click on the same source closes it, as a popout toggles.
        application_expression_card_close();
        return true;
    }
    c.source = expression;
    c.glyph_bytes = c.name_bytes = 0;
    if (expression.glyph.bytes <= sizeof(c.glyph)) {
        copy_bytes(c.glyph, expression.glyph.data, expression.glyph.bytes);
        c.glyph_bytes = expression.glyph.bytes;
    }
    c.source.glyph = {};
    c.name_bytes = application_message_expression_name(expression, {c.name, sizeof(c.name)});
    if (!c.name_bytes && expression.kind != MessageExpressionKind::sticker)
        return false;
    c.open = true;
    c.control_pressed = false;
    c.pressed_control = UiExpressionCardControl::none;
    // Failed lookups are retried when a popout opens again.
    c.failed_sticker = c.failed_pack = 0;
    c.packs_failed = false;
    want(Lookup::none, 0);
    return project();
}

void application_expression_card_close() noexcept {
    if (!g_card || !g_card->open)
        return;
    g_card->open = false;
    g_card->control_pressed = false;
    g_card->wanted = Lookup::none;
    ui_expression_card_close(&g_card->scene);
    if (g_app.renderer.expression_card_scene == &g_card->scene)
        g_app.renderer.expression_card_scene = nullptr;
    application_request_frame();
}

bool application_expression_card_shows(const MessageExpression& expression) noexcept {
    return open_now() && application_message_expression_same(g_card->source, expression);
}

bool application_expression_card_contains(UiPoint point) noexcept {
    return open_now() && ui_expression_card_contains(g_card->scene, point.x, point.y);
}

bool application_expression_card_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!open_now())
        return false;
    auto& c = *g_card;
    switch (message) {
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            application_expression_card_close();
            return true;
        }
        // The official popout focuses its button: Enter and Space press it.
        if ((wparam == VK_RETURN || wparam == VK_SPACE) && c.view_pack) {
            activate(UiExpressionCardControl::button);
            return true;
        }
        return false;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_SIZE:
    case WM_CANCELMODE:
    case WM_KILLFOCUS:
        // The official emoji popouts close when their message scrolls.
        application_expression_card_close();
        return false;
    case WM_MOUSELEAVE:
        if (ui_expression_card_set_hover(&c.scene, -1e7f, -1e7f))
            application_request_frame();
        return false;
    default:
        break;
    }
    if (message != WM_MOUSEMOVE && message != WM_LBUTTONDOWN && message != WM_LBUTTONUP &&
        message != WM_LBUTTONDBLCLK && message != WM_SETCURSOR && message != WM_RBUTTONDOWN &&
        message != WM_RBUTTONUP)
        return false;
    UiPoint point{};
    if (message == WM_SETCURSOR) {
        if (LOWORD(lparam) != HTCLIENT || !application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point))
            return false;
    } else
        point = {client_dip_x(lparam), client_dip_y(lparam)};
    const bool over = ui_expression_card_contains(c.scene, point.x, point.y);
    const auto hit = ui_expression_card_hit(c.scene, point.x, point.y);
    switch (message) {
    case WM_MOUSEMOVE:
        if (ui_expression_card_set_hover(&c.scene, point.x, point.y))
            application_request_frame();
        return over;
    case WM_SETCURSOR:
        if (!over)
            return false;
        application_set_cursor(ui_action_cursor(hit != UiExpressionCardControl::none));
        return true;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        if (over) {
            c.control_pressed = hit != UiExpressionCardControl::none;
            c.pressed_control = hit;
            if (ui_expression_card_set_pressed(&c.scene, hit == UiExpressionCardControl::button))
                application_request_frame();
            return true;
        }
        // Outside: the popout closes and the click still acts, as in the
        // official client; a click on its own source only closes it.
        MessageExpression under{};
        const bool source = application_message_expression_at(point, &under) &&
                            application_message_expression_same(under, c.source);
        application_expression_card_close();
        return source;
    }
    case WM_LBUTTONUP: {
        const bool pressed = c.control_pressed;
        const auto control = c.pressed_control;
        c.control_pressed = false;
        c.pressed_control = UiExpressionCardControl::none;
        if (ui_expression_card_set_pressed(&c.scene, false))
            application_request_frame();
        if (!over)
            return pressed;
        if (pressed && hit == control)
            activate(hit);
        return true;
    }
    case WM_RBUTTONDOWN:
        if (!over)
            application_expression_card_close();
        return over;
    default:
        return over;
    }
}

void application_expression_card_synchronize(u64 now_ms) noexcept {
    if (!open_now())
        return;
    auto& c = *g_card;
    // Its message changed, left or another channel opened: the popout goes.
    const auto* record = available() ? application_message_record(c.source.message_id) : nullptr;
    if (c.scope != g_app.plugin_account_scope_serial || !record || record->revision != c.source.message_revision ||
        record->channel_id != g_app.account_ui->active_channel_id) {
        application_expression_card_close();
        return;
    }
    const bool reduced = reduced_motion();
    if (ui_expression_card_tick(&c.scene, now_ms, reduced) || ui_expression_card_next_wake(c.scene, now_ms, reduced))
        application_request_frame();
}

bool application_expression_card_pending(u64 now) noexcept {
    return open_now() && g_card->wanted != Lookup::none && !g_card->submitted_id && now >= g_card->due_at;
}

bool application_expression_card_submit(u64 request_id, u64 now) noexcept {
    if (!request_id || !application_expression_card_pending(now) || !g_app.account.api.media_catalog)
        return false;
    auto& c = *g_card;
    const MediaCatalogOperation operation = c.wanted == Lookup::sticker ? MediaCatalogOperation::sticker
                                            : c.wanted == Lookup::pack  ? MediaCatalogOperation::sticker_pack
                                                                        : MediaCatalogOperation::sticker_packs;
    // The pack list is localized as the picker asks for it; single objects take no query.
    MediaCatalogQuery query{operation,
                            {},
                            operation == MediaCatalogOperation::sticker_packs ? utf8_literal("en-US") : Utf8View{},
                            0,
                            0u,
                            MediaCatalogFormat::tiny_webp,
                            c.wanted_id};
    const auto submitted =
        g_app.account.api.media_catalog(g_app.account.instance, AccountMediaCatalogOperation::submit, &query, request_id);
    if (!submitted.ok()) {
        if (submitted.error != RestClientSubmitError::busy) {
            // Refused outright: show what is known without it.
            if (c.wanted == Lookup::sticker)
                c.failed_sticker = c.wanted_id;
            else if (c.wanted == Lookup::pack)
                c.failed_pack = c.wanted_id;
            else
                c.packs_failed = true;
            c.wanted = Lookup::none;
            project();
        }
        return false;
    }
    c.submitted_id = request_id;
    c.submitted_lookup = c.wanted;
    c.submitted_object = c.wanted_id;
    c.wanted = Lookup::none;
    return true;
}

bool application_expression_card_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!g_card || !g_card->submitted_id || response.request_id != g_card->submitted_id)
        return false;
    auto& c = *g_card;
    const Lookup lookup = c.submitted_lookup;
    const u64 object = c.submitted_object;
    c.submitted_id = 0;
    c.submitted_lookup = Lookup::none;
    if (c.scope != g_app.plugin_account_scope_serial || !available())
        return true;
    // 429: wait out Retry-After (and an exhausted bucket's reset), as the
    // picker does; a local refusal waits its own floor.
    const bool limited = response.transport_ok() && response.rate_limited && response.status_code == 429 &&
                         response.retry_at_ms;
    const bool refused = response.error == RestClientError::rate;
    if ((limited || refused) && c.retries < kRateRetries && open_now()) {
        ++c.retries;
        const u64 now = clock_monotonic_milliseconds();
        u64 due = refused ? deadline_after(now, kLocalRateRetryMs) : response.retry_at_ms;
        if (limited && (response.rate.fields & rate_limit_field_remaining) && !response.rate.remaining &&
            response.rate.reset_after_ms)
            due = maximum(due, deadline_after(now, response.rate.reset_after_ms));
        c.wanted = lookup;
        c.wanted_id = object;
        c.due_at = maximum(due, deadline_after(now, 1));
        return true;
    }
    c.retries = 0;
    const MediaCatalogOperation operation = lookup == Lookup::sticker ? MediaCatalogOperation::sticker
                                            : lookup == Lookup::pack  ? MediaCatalogOperation::sticker_pack
                                                                      : MediaCatalogOperation::sticker_packs;
    Bank* bank = response.transport_ok() && response.status_code == 200 ? decode_bank(operation, body) : nullptr;
    if (!bank)
        log_u64(LogLevel::warning, "expression_card", "lookup_failed",
                u64{static_cast<u8>(lookup)} | u64{response.status_code} << 8);
    switch (lookup) {
    case Lookup::sticker:
        if (bank && bank->items[0].sticker_id == object) {
            c.known[c.known_next] = {object, bank->items[0].pack_id, bank->items[0].guild_id};
            c.known_next = (c.known_next + 1) % kKnownStickers;
        } else
            c.failed_sticker = object;
        free_bank(&bank);
        break;
    case Lookup::packs:
        if (bank) {
            free_bank(&c.packs);
            c.packs = bank;
        } else
            c.packs_failed = true;
        break;
    case Lookup::pack:
        if (bank) {
            free_bank(&c.retired);
            c.retired = bank;
        } else
            c.failed_pack = object;
        break;
    default:
        free_bank(&bank);
        break;
    }
    if (open_now() && c.source.kind == MessageExpressionKind::sticker)
        project();
    return true;
}

u32 application_expression_card_media(MediaAssetKey* output, u32 capacity) noexcept {
    if (!output || !open_now())
        return 0;
    const auto& s = g_card->scene;
    u32 count = 0;
    if (count < capacity && media_asset_key_valid(s.picture_key))
        output[count++] = s.picture_key;
    for (u32 i = 0; i < s.preview_count && count < capacity; ++i)
        if (media_asset_key_valid(s.preview_keys[i]))
            output[count++] = s.preview_keys[i];
    return count;
}

const GuildRecord* application_expression_card_guild() noexcept {
    if (!open_now() || !g_card->link_guild || !media_asset_key_valid(g_card->scene.guild_icon_key))
        return nullptr;
    return chat_state_store_find_guild(g_app.account_ui->chat_state, g_card->link_guild);
}

void application_expression_card_release() noexcept {
    g_press = {};
    if (!g_card)
        return;
    if (g_app.renderer.expression_card_scene == &g_card->scene)
        g_app.renderer.expression_card_scene = nullptr;
    free_bank(&g_card->packs);
    free_bank(&g_card->retired);
    VirtualFree(g_card, 0, MEM_RELEASE);
    g_card = nullptr;
}
} // namespace maicord::app_detail
