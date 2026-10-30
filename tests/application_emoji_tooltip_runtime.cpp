#include "../src/app/application_emoji_tooltip.cpp"
#include "../src/app/application_expression_card.cpp"
#include "application_tooltip.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/regional_flag.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord {
namespace emoji_tooltip_test {
// The renderer's emoji under the pointer; the real hit test runs in the
// message link UI probe against shaped text.
MessageEmojiHit hit;
} // namespace emoji_tooltip_test
MessageEmojiHit renderer_message_emoji_hit_test(D3D11Renderer&, float x, float y) noexcept {
    const auto& hit = emoji_tooltip_test::hit;
    return ui_rect_contains(hit.rectangle, x, y) ? hit : MessageEmojiHit{};
}

namespace app_detail {
namespace expression_card_test {
u64 opened_pack, selected_guild, server_sticker, server;
u32 submissions;
MediaCatalogQuery last;
u64 last_id;
bool refuse;
RestClientSubmitResult WINAPI submit(void*, AccountMediaCatalogOperation operation, const MediaCatalogQuery* query,
                                     u64 id) noexcept {
    if (operation != AccountMediaCatalogOperation::submit || !query || refuse)
        return {RestClientSubmitError::invalid_input};
    ++submissions;
    last = *query;
    last_id = id;
    return {RestClientSubmitError::none};
}
} // namespace expression_card_test
// The popout's hosts: the composer picker, server navigation and pointer scale.
bool application_media_picker_open_sticker_pack(u64 pack_id) noexcept {
    expression_card_test::opened_pack = pack_id;
    return true;
}
u64 application_media_picker_sticker_server(u64 sticker_id) noexcept {
    return sticker_id && sticker_id == expression_card_test::server_sticker ? expression_card_test::server : 0;
}
bool select_content_guild(u64 guild) noexcept {
    expression_card_test::selected_guild = guild;
    return true;
}
float client_dip_x(LPARAM lparam) noexcept {
    return static_cast<float>(static_cast<short>(LOWORD(lparam)));
}
float client_dip_y(LPARAM lparam) noexcept {
    return static_cast<float>(static_cast<short>(HIWORD(lparam)));
}
namespace {
// The pointer rests on the hit's centre until the tooltip is due; the label
// shown then, or nothing.
Utf8View hover(UiRect at, u64* deadline, u64* started) noexcept {
    *started = clock_monotonic_milliseconds();
    application_tooltip_input(WM_MOUSEMOVE, 0,
                              MAKELPARAM(static_cast<short>((at.left + at.right) * .5f),
                                         static_cast<short>((at.top + at.bottom) * .5f)));
    *deadline = application_tooltip_next_wake();
    const auto& tip = *g_app.renderer.tooltip_scene;
    if (!*deadline || !application_tooltip_synchronize(*deadline) || !tip.visible)
        return {};
    return {tip.text, tip.text_bytes};
}
} // namespace

void test_emoji_tooltip_owner(void (*check)(bool, const char*) noexcept) noexcept {
    using emoji_tooltip_test::hit;
    auto& ui = *g_app.account_ui;
    // Every exit restores the host's state; its own checks begin with no tooltip owner.
    struct Restore {
        AccountUiRuntime& ui;
        ChatStateStore state = ui.chat_state;
        MessageStore store = g_app.message_view.store;
        u64 active = ui.active_channel_id;
        MessageRichStore rich = ui.message_rich_store;
        ~Restore() noexcept {
            application_expression_card_release();
            application_tooltip_release();
            emoji_tooltip_test::hit = {};
            g_app.renderer.message_text = nullptr;
            g_app.account.api.media_catalog = nullptr;
            ui.chat_state = state;
            ui.active_channel_id = active;
            ui.message_rich_store = rich;
            g_app.message_view.store = store;
        }
    } restore{ui};
    GuildRecord guilds[1]{};
    ChannelRecord channels[1]{};
    u16 guild_order[1]{}, channel_order[1]{};
    u8 names[256]{}, text[256]{};
    CustomEmojiRecord emojis[2]{};
    u64 emoji_roles[2]{};
    MessageRecord messages[1]{};
    u32 message_order[1]{};
    // Content and its display text: "<:blob:123>" is an unknown server's emoji,
    // "<a:spin:124>" a known animated one named "spinner" without required colons.
    constexpr Utf8View content = utf8_literal("a\xF0\x9F\x98\x82\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD<:blob:123><a:spin:124>"
                                              "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8");
    constexpr Utf8View display = utf8_literal("a\xF0\x9F\x98\x82\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD\xEF\xBF\xBC"
                                              "\xEF\xBF\xBC\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8");
    const CustomEmojiInput known{124, utf8_literal("spinner"), {}, 0, custom_emoji_animated | custom_emoji_available, 0};
    const bool seeded =
        chat_state_store_initialize(&ui.chat_state, {guilds, 1}, {guild_order, 1}, {channels, 1}, {channel_order, 1},
                                    {names, sizeof(names)}) &&
        chat_state_store_upsert_guild(&ui.chat_state, GuildInput{70, utf8_literal("Guild"), GuildMutationKind::create, 8})
            .ok() &&
        chat_state_store_upsert_channel(&ui.chat_state, {8, 70, 0, utf8_literal("general"), 0, 0, 0}).ok() &&
        chat_state_store_bind_emojis_sparse(&ui.chat_state, {emojis, 2}, {emoji_roles, 2}) &&
        chat_state_store_replace_guild_emojis(&ui.chat_state, 70, {&known, 1}) &&
        message_store_initialize(&g_app.message_view.store, {messages, 1}, {message_order, 1}, {text, sizeof(text)}) &&
        message_store_upsert(&g_app.message_view.store, {43, 8, 9, 1, 0, content, 0}).ok();
    const auto* record = application_message_record(43);
    const u16 flag = regional_flag_sequence({display.data + 19, 8});
    const MessageMarkdownSemantic semantics[]{
        {123, 13, 3, 13, 11, MessageMarkdownSemanticKind::custom_emoji, 0},
        {124, 16, 3, 24, 12, MessageMarkdownSemanticKind::custom_emoji, 1},
        {flag, 19, 8, 36, 8, MessageMarkdownSemanticKind::regional_flag, 0},
    };
    // The text's owner: a visible container in the message area.
    UiWidget widget_storage[2]{};
    UiLayoutNode node_storage[2]{};
    UiWidgetManager widgets{};
    auto fill = ui_layout_style();
    fill.width = ui_layout_fill();
    fill.height = ui_layout_fill();
    const bool owned = ui_widget_manager_initialize(&widgets, {widget_storage, 2}, {node_storage, 2});
    const auto root = owned ? ui_widget_manager_add(&widgets, ui_widget_invalid_handle(),
                                                    {1, UiWidgetKind::container, ui_widget_none,
                                                     {utf8_literal("Messages"), fill}})
                            : UiWidgetResult{};
    const auto body = root.ok() ? ui_widget_manager_add(&widgets, root.handle,
                                                        {2, UiWidgetKind::container, ui_widget_none,
                                                         {utf8_literal("Message"), fill}})
                                : UiWidgetResult{};
    MessageTextCommand command{};
    command.text = display;
    command.semantics = {semantics, 3};
    command.owner = {&widgets, body.handle};
    command.rectangle = command.clip = {80, 90, 600, 140};
    command.message_id = 43;
    command.message_revision = record ? record->revision : 0;
    command.measurement_kind = MessageTextMeasurementKind::content;
    MessageTextPresentation presentation{};
    presentation.commands = {&command, 1};
    presentation.command_count = 1;
    presentation.active = true;
    g_app.renderer.message_text = &presentation;
    ui.active_channel_id = ui.chat_state.selected_channel_id = 8;
    check(seeded && record && body.ok() && ui_widget_manager_layout_rectangle(&widgets, {0, 0, 1000, 700}).ok(),
          "emoji tooltip: message, server emoji and text owner initialize");

    const auto shows = [&](MessageEmojiHit at, Utf8View expected) noexcept {
        hit = at;
        u64 deadline = 0, started = 0;
        const Utf8View shown = hover(at.rectangle, &deadline, &started);
        return deadline >= started + 300 && deadline <= clock_monotonic_milliseconds() + 300 &&
               utf8_equal(shown, expected);
    };
    // The first eligible pointer creates the host's single tooltip owner.
    application_tooltip_input(WM_MOUSEMOVE, 0, MAKELPARAM(990, 690));
    if (!g_app.renderer.tooltip_scene) {
        check(false, "emoji tooltip: the pointer creates the tooltip owner");
        return;
    }
    const auto& tip = *g_app.renderer.tooltip_scene;
    const auto unicode = [&](u32 offset, u32 bytes, float left) noexcept {
        return MessageEmojiHit{43, {left, 100, left + 22, 122}, command.message_revision, 0, offset, bytes, ~0u,
                               MessageEmojiHitKind::unicode};
    };
    const auto semantic = [&](u32 index, float left) noexcept {
        return MessageEmojiHit{43, {left, 100, left + 22, 122}, command.message_revision, 0,
                               semantics[index].byte_offset, semantics[index].byte_count, index,
                               MessageEmojiHitKind::semantic};
    };
    const auto& scale = application_theme().scale;
    check(shows(unicode(1, 4, 420), utf8_literal(":joy:")) && tip.style_count == 1 &&
              (tip.styles[0].flags & text_style_bold) && tip.styles[0].byte_count == 5 &&
              utf8_equal({tip.emoji_glyph, tip.emoji_glyph_bytes}, {display.data + 1, 4}) &&
              !media_asset_key_valid(tip.emoji_media) && tip.placement == UiTooltipPlacement::above &&
              ui_rect_equal(tip.anchor, {420, 100, 442, 122}) && tip.rectangle.bottom <= 100 &&
              utf8_equal({tip.detail, tip.detail_bytes}, utf8_literal("Click to learn more")),
          "emoji tooltip: a Unicode emoji shows itself, its bold :name: and the grey Click to learn more above "
          "it after 300 ms");
    check(tip.style.emoji_size == 48 && tip.style.emoji_gap == scale.space_4 &&
              tip.style.padding_x == scale.space_4 + scale.space_2 && tip.style.padding_y == scale.space_4 &&
              tip.style.maximum_width == 272,
          "emoji tooltip: the rich tooltip's 48 DIP emoji column, spacing and width");
    check(application_tooltip_synchronize(clock_monotonic_milliseconds()) && tip.visible &&
              !application_tooltip_next_wake(),
          "emoji tooltip: a resting pointer keeps the tooltip without another wake");
    check(shows(unicode(5, 8, 450), utf8_literal(":thumbsup_tone3:")),
          "emoji tooltip: another emoji needs its own delay; skin tones name their variant");
    check(shows(semantic(0, 480), utf8_literal(":blob:")) &&
              media_asset_key_equal(tip.emoji_media, media_asset_custom_emoji(123, false)),
          "emoji tooltip: an unknown server's emoji shows its image and its tag's :name:");
    check(shows(semantic(1, 510), utf8_literal("spinner")) &&
              media_asset_key_equal(tip.emoji_media, media_asset_custom_emoji(124, true)),
          "emoji tooltip: a known emoji without required colons shows its current bare name, animated");
    application_tooltip_clear();
    if (auto* spinner = const_cast<CustomEmojiRecord*>(chat_state_store_find_emoji(ui.chat_state, 124)))
        spinner->flags |= custom_emoji_require_colons;
    check(shows(semantic(1, 510), utf8_literal(":spinner:")),
          "emoji tooltip: a server requiring colons names its emoji with them");
    check(shows(semantic(2, 540), utf8_literal(":flag_us:")) &&
              media_asset_key_equal(tip.emoji_media, media_asset_regional_flag(flag)),
          "emoji tooltip: a regional flag shows its flag image and :flag_xx: name");

    u64 deadline = 0, started = 0;
    hit = unicode(0, 1, 570);
    check(!hover(hit.rectangle, &deadline, &started).bytes && !deadline && !tip.pending,
          "emoji tooltip: text outside the emoji catalog has no tooltip or wake");
    hit = unicode(1, 4, 600);
    ++hit.message_revision;
    check(!hover(hit.rectangle, &deadline, &started).bytes && !deadline,
          "emoji tooltip: an emoji of an edited message's old text cannot show");
    ui.active_channel_id = 9;
    check(!shows(unicode(1, 4, 630), utf8_literal(":joy:")) && !application_tooltip_next_wake(),
          "emoji tooltip: a message outside the active channel cannot show");
    ui.active_channel_id = 8;

    check(shows(unicode(1, 4, 660), utf8_literal(":joy:")), "emoji tooltip: shown before a click");
    application_tooltip_input(WM_LBUTTONDOWN, 0, MAKELPARAM(671, 111));
    check(!tip.visible && !application_tooltip_next_wake(), "emoji tooltip: a click hides it, as the official client");
    check(shows(unicode(1, 4, 690), utf8_literal(":joy:")), "emoji tooltip: shown before the pointer leaves");
    application_tooltip_input(WM_MOUSEMOVE, 0, MAKELPARAM(701, 200));
    check(!tip.visible && !application_tooltip_next_wake(), "emoji tooltip: leaving the emoji hides it at once");

    // The popout. Reduced motion settles it at once so its controls sit where they end.
    namespace card = expression_card_test;
    const bool motion = g_app.preferences.reduced_motion;
    g_app.preferences.reduced_motion = true;
    g_app.account.api.media_catalog = card::submit;
    const auto centre = [](UiRect r) noexcept { return UiPoint{(r.left + r.right) * .5f, (r.top + r.bottom) * .5f}; };
    const auto lparam = [](UiPoint p) noexcept {
        return static_cast<LPARAM>(MAKELPARAM(static_cast<short>(p.x), static_cast<short>(p.y)));
    };
    const auto click = [&](MessageEmojiHit at) noexcept {
        hit = at;
        application_expression_card_pointer_down(centre(at.rectangle));
        application_expression_card_pointer_up(centre(at.rectangle));
    };
    const auto same = [](const char* data, u32 bytes, Utf8View expected) noexcept {
        return utf8_equal({data, bytes}, expected);
    };
    // A control's centre on the settled card.
    const auto control = [](const UiExpressionCardScene& s, UiRect r) noexcept {
        return UiPoint{s.settled.left + (r.left + r.right) * .5f, s.settled.top + (r.top + r.bottom) * .5f};
    };
    const auto press = [&](UiPoint p) noexcept {
        const bool down = application_expression_card_window_message(WM_LBUTTONDOWN, MK_LBUTTON, lparam(p));
        return down && application_expression_card_window_message(WM_LBUTTONUP, 0, lparam(p));
    };
    application_tooltip_clear();
    const auto joy = unicode(1, 4, 420);
    click(joy);
    const auto* shown = g_app.renderer.expression_card_scene;
    check(shown && shown->active && shown->layout == UiExpressionCardLayout::head &&
              same(shown->title_text, shown->title_bytes, utf8_literal(":joy:")) &&
              same(shown->description_text, shown->description_bytes,
                   utf8_literal("A default emoji. You can use this emoji everywhere on Discord.")) &&
              same(shown->glyph, shown->glyph_bytes, {display.data + 1, 4}) && !shown->guild_heading_bytes &&
              !shown->button_bytes && shown->settled.left >= joy.rectangle.right + 8 - .01f &&
              ui_rect_width(shown->settled) == kUiExpressionCardWidth,
          "expression card: a click on a Unicode emoji opens its popout to the right with :name: and the "
          "default-emoji sentence");
    if (!shown)
        return;
    const auto& popout = *shown;
    UiTooltipTarget suppressed{};
    UiTooltipStyle suppressed_style{};
    check(!application_emoji_tooltip_target(centre(joy.rectangle), 7, &suppressed, &suppressed_style) &&
              !application_emoji_tooltip_target(centre(popout.settled), 7, &suppressed, &suppressed_style),
          "expression card: no tooltip for its source or under the open popout");
    check(application_expression_card_window_message(WM_MOUSEMOVE, 0, lparam(centre(popout.settled))) &&
              !application_expression_card_window_message(WM_MOUSEMOVE, 0, lparam({5, 5})),
          "expression card: the pointer over the popout belongs to it, elsewhere to the view");
    check(application_expression_card_window_message(WM_LBUTTONDOWN, MK_LBUTTON, lparam(centre(joy.rectangle))) &&
              !popout.active && !g_app.renderer.expression_card_scene,
          "expression card: a second click on its source closes it and is consumed");
    application_expression_card_pointer_up(centre(joy.rectangle));
    check(!popout.active, "expression card: the closing click's release does not reopen it");

    // A drag that selected text from the emoji opens nothing.
    hit = joy;
    application_expression_card_pointer_down(centre(joy.rectangle));
    g_app.message_text_selection.valid = true;
    g_app.message_text_selection_anchor = 1;
    g_app.message_text_selection_caret = 5;
    application_expression_card_pointer_up(centre(joy.rectangle));
    const bool dragged = !popout.active;
    g_app.message_text_selection.valid = false;
    g_app.message_text_selection_anchor = g_app.message_text_selection_caret = 0;
    application_expression_card_pointer_down(centre(joy.rectangle));
    hit = unicode(5, 8, 450);
    application_expression_card_pointer_up(centre(hit.rectangle));
    check(dragged && !popout.active,
          "expression card: a drag selecting text, or a release over another emoji, opens nothing");

    // Escape, the wheel and a click elsewhere close it; only Escape is consumed.
    click(joy);
    const bool escape = popout.active && application_expression_card_window_message(WM_KEYDOWN, VK_ESCAPE, 0) &&
                        !popout.active;
    click(joy);
    const bool wheel = popout.active && !application_expression_card_window_message(WM_MOUSEWHEEL, 0, 0) &&
                       !popout.active;
    click(joy);
    const bool outside = popout.active &&
                         !application_expression_card_window_message(WM_LBUTTONDOWN, MK_LBUTTON, lparam({5, 5})) &&
                         !popout.active;
    check(escape && wheel && outside,
          "expression card: Escape, the wheel and an outside click (which still acts) close it");

    // Custom emoji: the server sentences and the joined source server.
    const auto spinner = semantic(1, 510);
    click(spinner);
    check(popout.active && same(popout.title_text, popout.title_bytes, utf8_literal(":spinner:")) &&
              media_asset_key_equal(popout.picture_key, media_asset_custom_emoji(124, true)) &&
              same(popout.description_text, popout.description_bytes,
                   utf8_literal("This emoji is from this server. Get Nitro to use it everywhere.")) &&
              same(popout.guild_heading_text, popout.guild_heading_bytes, utf8_literal("This emoji is from")) &&
              same(popout.guild_name_text, popout.guild_name_bytes, utf8_literal("Guild")) &&
              same(popout.guild_subtitle_text, popout.guild_subtitle_bytes, utf8_literal("Invite-Only Server")) &&
              same(popout.guild_acronym_text, popout.guild_acronym_bytes, utf8_literal("G")) && popout.guild_link,
          "expression card: this server's emoji without Nitro, and its server below");
    // The hand marks what a click acts on: the server's icon and name, not the text.
    check(ui_expression_card_hit(popout, control(popout, popout.guild_name).x,
                                 control(popout, popout.guild_name).y) == UiExpressionCardControl::guild &&
              ui_expression_card_hit(popout, control(popout, popout.guild_icon).x,
                                     control(popout, popout.guild_icon).y) == UiExpressionCardControl::guild &&
              ui_expression_card_hit(popout, control(popout, popout.description).x,
                                     control(popout, popout.description).y) == UiExpressionCardControl::none &&
              ui_action_cursor(true) == UiCursor::hand && ui_action_cursor(false) == UiCursor::arrow,
          "expression card: the cursor regions: the server is a link, the sentences are not");
    ui.chat_state.self_premium_type = 2;
    auto* channel = const_cast<ChannelRecord*>(chat_state_store_find_channel(ui.chat_state, 8));
    if (channel)
        channel->guild_id = 0;
    click(spinner);
    click(spinner);
    check(popout.active && same(popout.description_text, popout.description_bytes,
                                utf8_literal("This emoji is from one of your servers. Type its name in the chat "
                                             "bar to use it.")),
          "expression card: one of your servers' emoji with Nitro");
    if (channel)
        channel->guild_id = 70;
    ui.chat_state.self_premium_type = 0;
    check(press(control(popout, popout.guild_name)) && card::selected_guild == 70 && !popout.active,
          "expression card: the server's name opens it and closes the popout");
    click(semantic(0, 480));
    check(popout.active && same(popout.title_text, popout.title_bytes, utf8_literal(":blob:")) &&
              same(popout.description_text, popout.description_bytes,
                   utf8_literal("This emoji is from a server that is either invite-only or unavailable.")) &&
              !popout.guild_heading_bytes && !card::submissions,
          "expression card: an unknown server's emoji, without a source fetch");
    click(semantic(2, 540));
    check(popout.active && same(popout.title_text, popout.title_bytes, utf8_literal(":flag_us:")) &&
              media_asset_key_equal(popout.picture_key, media_asset_regional_flag(flag)) &&
              same(popout.description_text, popout.description_bytes,
                   utf8_literal("A default emoji. You can use this emoji everywhere on Discord.")),
          "expression card: a flag is a default emoji with its flag image");
    ui.active_channel_id = 9;
    application_expression_card_synchronize(clock_monotonic_milliseconds());
    ui.active_channel_id = 8;
    check(!popout.active, "expression card: another channel closes it");

    // Stickers: the message's three, looked up through the catalog lane.
    MessageRichRecord rich_records[1]{};
    u16 rich_order[1]{};
    u8 rich_text[512]{};
    const GatewayMessageSticker stickers[]{{1001, utf8_literal("Wave"), 1}, {1002, utf8_literal("Vibing"), 1},
                                           {1003, utf8_literal("Gone"), 1}};
    GatewayMessageEvent event{};
    event.id = 43;
    event.channel_id = 8;
    event.fields = gateway_message_field_sticker_items;
    event.stickers = {stickers, 3};
    check(message_rich_store_initialize(&ui.message_rich_store, {rich_records, 1}, {rich_order, 1},
                                        {rich_text, sizeof(rich_text)}) &&
              message_rich_store_apply(&ui.message_rich_store, 43, command.message_revision, event).ok(),
          "expression card: the message's stickers");
    const auto sticker = [&](u8 index) noexcept {
        MessageExpression e{};
        e.anchor = {100, 300, 164, 364};
        e.message_id = 43;
        e.message_revision = command.message_revision;
        e.source = kStickerSource + index;
        e.kind = MessageExpressionKind::sticker;
        e.id = stickers[index].id;
        e.sticker_format = 1;
        e.sticker_index = index;
        return e;
    };
    const auto answer = [&](u16 status, Utf8View body) noexcept {
        RestClientResponse response{};
        response.request_id = card::last_id;
        response.status_code = status;
        return application_expression_card_response(response, body);
    };
    const auto lane = [&](u64 id) noexcept {
        const u64 now = clock_monotonic_milliseconds();
        return application_expression_card_pending(now) && application_expression_card_submit(id, now);
    };
    check(application_expression_card_open(sticker(0)) && popout.layout == UiExpressionCardLayout::loading &&
              lane(500) && card::last.operation == MediaCatalogOperation::sticker && card::last.object_id == 1001 &&
              !card::last.locale.bytes && !application_expression_card_pending(clock_monotonic_milliseconds()),
          "expression card: an unknown sticker loads while GET /stickers/{id} runs");
    RestClientResponse stranger{};
    stranger.request_id = 499;
    check(!application_expression_card_response(stranger, {}), "expression card: another lane's answer is not its");
    check(answer(200, utf8_literal(R"({"id":"1001","name":"Wave","type":1,"format_type":1,"pack_id":"42"})")) &&
              popout.layout == UiExpressionCardLayout::loading && lane(501) &&
              card::last.operation == MediaCatalogOperation::sticker_packs &&
              utf8_equal(card::last.locale, utf8_literal("en-US")),
          "expression card: a standard sticker then needs the premium pack list");
    constexpr auto packs = utf8_literal(
        R"({"sticker_packs":[{"id":"42","name":"Wumpus Beyond","stickers":[)"
        R"({"id":"2001","name":"A","format_type":1},{"id":"1001","name":"Wave","format_type":1},)"
        R"({"id":"2002","name":"B","format_type":2},{"id":"2003","name":"C","format_type":1},)"
        R"({"id":"2004","name":"D","format_type":1}]}]})");
    check(answer(200, packs) && popout.layout == UiExpressionCardLayout::pack &&
              same(popout.title_text, popout.title_bytes, utf8_literal("Wave")) &&
              same(popout.description_text, popout.description_bytes,
                   utf8_literal("This is a Wumpus Beyond sticker.")) &&
              popout.emphasis_offset == 10 && popout.emphasis_bytes == 13 && popout.preview_count == 3 &&
              media_asset_key_equal(popout.preview_keys[0], media_asset_sticker(2001, 1)) &&
              media_asset_key_equal(popout.preview_keys[1], media_asset_sticker(2002, 2)) &&
              media_asset_key_equal(popout.preview_keys[2], media_asset_sticker(2003, 1)) &&
              same(popout.button_text, popout.button_bytes, utf8_literal("View More Stickers")),
          "expression card: a premium pack's sticker names its pack, three of its others and View More Stickers");
    MediaAssetKey wanted[4]{};
    check(application_expression_card_media(wanted, 4) == 3 &&
              media_asset_key_equal(wanted[0], media_asset_sticker(2001, 1)),
          "expression card: the pack's stickers are fetched by key");
    check(ui_expression_card_hit(popout, control(popout, popout.button).x, control(popout, popout.button).y) ==
                  UiExpressionCardControl::button &&
              ui_expression_card_hit(popout, control(popout, popout.previews[0]).x,
                                     control(popout, popout.previews[0]).y) == UiExpressionCardControl::none,
          "expression card: View More Stickers is the pack popout's one control");
    check(application_expression_card_window_message(WM_KEYDOWN, VK_RETURN, 0) && card::opened_pack == 42 &&
              !popout.active,
          "expression card: Enter presses View More Stickers: the picker opens at the pack");
    const u32 before = card::submissions;
    check(application_expression_card_open(sticker(0)) && popout.layout == UiExpressionCardLayout::pack &&
              !application_expression_card_pending(clock_monotonic_milliseconds()) && card::submissions == before,
          "expression card: the sticker and pack list are kept: reopening asks nothing");
    check(press(control(popout, popout.button)) && card::opened_pack == 42 && !popout.active,
          "expression card: a click on View More Stickers opens the picker at the pack");

    card::server_sticker = 1002;
    card::server = 70;
    check(application_expression_card_open(sticker(1)) && popout.layout == UiExpressionCardLayout::head &&
              media_asset_key_equal(popout.picture_key, media_asset_sticker(1002, 1)) &&
              same(popout.description_text, popout.description_bytes,
                   utf8_literal("This sticker is from this server. Get Nitro to use it everywhere.")) &&
              same(popout.guild_heading_text, popout.guild_heading_bytes, utf8_literal("This sticker is from")) &&
              card::submissions == before,
          "expression card: a joined server's sticker, from its session list without a request");

    check(application_expression_card_open(sticker(2)) && lane(502) &&
              answer(200, utf8_literal(R"({"id":"1003","name":"Gone","type":1,"format_type":1,"pack_id":"50"})")) &&
              lane(503) && card::last.operation == MediaCatalogOperation::sticker_pack && card::last.object_id == 50,
          "expression card: a pack missing from the list is read by ID");
    check(answer(200, utf8_literal(R"({"id":"50","name":"Old Pack","stickers":[{"id":"1003","name":"Gone",)"
                                   R"("format_type":1},{"id":"3001","name":"E","format_type":1}]})")) &&
              popout.layout == UiExpressionCardLayout::pack &&
              same(popout.description_text, popout.description_bytes,
                   utf8_literal("This is a Old Pack sticker. They are no longer available.")) &&
              popout.preview_count == 1 && !popout.button_bytes,
          "expression card: a retired pack's sticker says so, without a button");

    // Without its server list the server sticker is unknown again.
    card::server_sticker = 0;
    check(application_expression_card_open(sticker(1)) && lane(504), "expression card: another unknown sticker");
    RestClientResponse limited{};
    limited.request_id = 504;
    limited.status_code = 429;
    limited.rate_limited = true;
    limited.retry_at_ms = clock_monotonic_milliseconds() + 60'000;
    check(application_expression_card_response(limited, {}) && popout.layout == UiExpressionCardLayout::loading &&
              !application_expression_card_pending(clock_monotonic_milliseconds()) &&
              application_expression_card_pending(limited.retry_at_ms),
          "expression card: 429 waits out Retry-After");
    check(application_expression_card_submit(505, limited.retry_at_ms) && answer(404, utf8_literal(R"({"code":10060})")) &&
              popout.layout == UiExpressionCardLayout::head &&
              same(popout.description_text, popout.description_bytes,
                   utf8_literal("This sticker is from a server that is either invite-only or unavailable.")),
          "expression card: an unreadable sticker reads as from an invite-only or unavailable server");
    application_expression_card_close();
    g_app.preferences.reduced_motion = motion;
}
} // namespace app_detail
} // namespace maicord
