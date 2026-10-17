#include "maicord/ui/message_context_controller.hpp"
#include "maicord/ui/received_attachment_menu.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/reaction_preferences.hpp"
#include "maicord/ui/emoji_catalog.hpp"

namespace maicord {
namespace {
inline constexpr u32 kTestItems = kMessageContextMaximumPopupItems + 3;
inline constexpr u32 kTestWidgets = 2 + kTestItems * kPopupWidgetsPerItem;

struct PopupFixture {
    PopupItem items[kTestItems];
    PopupRowWidget rows[kTestItems];
    UiWidget widgets[kTestWidgets];
    UiLayoutNode layout[kTestWidgets];
    PopupScene popup;

    bool initialize(u32 capacity = kTestItems) noexcept {
        return popup_scene_initialize(&popup, {items, capacity}, {rows, capacity},
            {widgets, kTestWidgets}, {layout, kTestWidgets});
    }
};

bool reject_measure(void*, const UiLayoutMeasureRequest&, UiSize*, float*) noexcept {
    return false;
}
// The official client's attachment groups per kind, merged into the message menu.
void test_received_attachment_menu(void (*check)(bool, const char*) noexcept) noexcept {
    const auto image_url =
        utf8_literal("https://cdn.discordapp.com/attachments/1/2/Cat.PNG?ex=1&is=2&hm=3");
    check(received_attachment_link_kind(image_url) == ReceivedAttachmentLinkKind::image &&
              received_attachment_link_kind(
                  utf8_literal("https://cdn.discordapp.com/attachments/1/2/a.gif")) ==
                  ReceivedAttachmentLinkKind::media &&
              received_attachment_link_kind(
                  utf8_literal("https://media.discordapp.net/attachments/1/2/a.png")) ==
                  ReceivedAttachmentLinkKind::media &&
              received_attachment_link_kind(
                  utf8_literal("https://cdn.discordapp.com/attachments/1/2/clip.mp4")) ==
                  ReceivedAttachmentLinkKind::media &&
              received_attachment_link_kind(
                  utf8_literal("https://cdn.discordapp.com/attachments/1/2/a.zip?x=.png")) ==
                  ReceivedAttachmentLinkKind::link &&
              received_attachment_link_kind(
                  utf8_literal("https://cdn.discordapp.com/attachments/1.png/2/notes")) ==
                  ReceivedAttachmentLinkKind::link,
          "attachment menu: link labels follow official host and path-extension rules, never the query");
    struct Expected {
        ReceivedAttachmentKind kind;
        Utf8View url;
        const char* labels[4];
        u32 count;
    };
    const Expected cases[]{
        {ReceivedAttachmentKind::image,
         image_url,
         {"Copy Image", "Save Image", "Copy Image Link", "Open Image Link"},
         4},
        {ReceivedAttachmentKind::animated_image,
         utf8_literal("https://cdn.discordapp.com/attachments/1/2/a.gif"),
         {"Save Image", "Copy Media Link", "Open Media Link"},
         3},
        {ReceivedAttachmentKind::video,
         utf8_literal("https://cdn.discordapp.com/attachments/1/2/clip.mov"),
         {"Save Video", "Copy Media Link", "Open Media Link"},
         3},
        {ReceivedAttachmentKind::audio,
         utf8_literal("https://cdn.discordapp.com/attachments/1/2/song.mp3"),
         {"Save File", "Copy Link", "Open Link"},
         3},
        {ReceivedAttachmentKind::voice_message,
         utf8_literal("https://cdn.discordapp.com/attachments/1/2/voice-message.ogg"),
         {"Save File", "Copy Link", "Open Link"},
         3},
        {ReceivedAttachmentKind::file,
         utf8_literal("https://cdn.discordapp.com/attachments/1/2/notes.txt"),
         {"Save File", "Copy Link", "Open Link"},
         3},
    };
    bool labels = true, groups = true, icons = true;
    for (const auto& expected : cases) {
        PopupItem items[kReceivedAttachmentMenuItems]{};
        const u32 count = received_attachment_menu({expected.kind, expected.url, true, true}, {items, 4});
        labels = labels && count == expected.count;
        for (u32 index = 0; labels && index < count; ++index) {
            u32 bytes = 0;
            while (expected.labels[index][bytes])
                ++bytes;
            labels = utf8_equal(items[index].label, {expected.labels[index], bytes}) &&
                     (items[index].flags & popup_item_enabled) &&
                     received_attachment_menu_action(items[index].action);
        }
        // Two groups: media (copy/save) and the native link pair.
        groups = groups && count >= 3 && (items[0].flags & popup_item_separator_before) &&
                 (items[count - 2].flags & popup_item_separator_before) &&
                 !(items[count - 1].flags & popup_item_separator_before) &&
                 items[count - 2].action == kReceivedAttachmentActionCopyLink &&
                 items[count - 1].action == kReceivedAttachmentActionOpenLink;
        icons = icons && count >= 2 &&
                ui_menu_row_icon(items[count - 1].presentation) == UiIcon::external_link &&
                ui_menu_row_icon(items[count - 2].presentation) == UiIcon::link;
    }
    check(labels && groups && icons, "attachment menu: images, gifs, videos, audio, voice messages and "
                                     "files get the official rows, groups and icons");
    PopupItem items[kReceivedAttachmentMenuItems]{};
    check(received_attachment_menu({ReceivedAttachmentKind::image, image_url, false, false}, {items, 4}) ==
                  2 &&
              items[0].action == kReceivedAttachmentActionCopyLink &&
              (items[0].flags & popup_item_separator_before) &&
              !received_attachment_menu(
                  {ReceivedAttachmentKind::file, utf8_literal("http://x/a.txt"), true, true}, {items, 4}) &&
              !received_attachment_menu({ReceivedAttachmentKind::file, image_url, true, true}, {items, 3}),
          "attachment menu: unavailable copy/save rows drop out; non-https URLs and short outputs build "
          "nothing");
    const u32 sticker_rows = received_attachment_menu(
        {ReceivedAttachmentKind::sticker, utf8_literal("https://media.discordapp.net/stickers/42.png"), true, true},
        {items, 4});
    check(sticker_rows == 2 && utf8_equal(items[0].label, utf8_literal("Copy Media Link")) &&
              utf8_equal(items[1].label, utf8_literal("Open Media Link")) &&
              (items[0].flags & popup_item_separator_before),
          "attachment menu: a sticker image offers only the media link pair");
    check(static_cast<u8>(UiIcon::external_link) >= 62 &&
              ui_menu_row_presentation_valid(ui_menu_row_presentation(UiIcon::external_link)) &&
              !ui_menu_row_presentation_valid(ui_menu_row_presentation(UiIcon::count)),
          "attachment menu: icons past the old 62-entry menu limit lead rows; invalid identities still "
          "reject");

    // Merged into the message menu: media groups before the developer-mode
    // Copy message ID; plugin extras stay last.
    PopupFixture fixture{};
    MessageRecord message{};
    message.id = 10;
    message.channel_id = 20;
    message.author_id = 30;
    MessageContextController controller{};
    PopupItem media[kReceivedAttachmentMenuItems]{};
    const u32 media_count =
        received_attachment_menu({ReceivedAttachmentKind::image, image_url, true, true}, {media, 4});
    const PopupItem plugin{utf8_literal("Plugin action"), 1000, popup_item_enabled, 0};
    const UiRect anchor{40, 40, 41, 41}, viewport{0, 0, 800, 1000};
    const auto opened =
        fixture.initialize()
            ? message_context_controller_open(&controller, &fixture.popup, message, true, true, false,
                                              nullptr, anchor, viewport, true, {&plugin, 1}, false, false,
                                              false, false, false, true, {}, {media, media_count})
            : MessageContextControllerResult{MessageContextControllerError::popup, false};
    const u32 total = fixture.popup.item_count;
    bool merged = opened.ok() && media_count == 4 && total == controller.menu.count + media_count + 1;
    const u32 id_row = merged ? total - 2 : 0, first_media = merged ? id_row - media_count : 0;
    merged = merged && fixture.items[total - 1].action == 1000 &&
             fixture.items[id_row].action == static_cast<u32>(MessageContextAction::copy_id) &&
             (fixture.items[first_media].flags & popup_item_separator_before) &&
             fixture.items[first_media].action == kReceivedAttachmentActionCopyImage &&
             fixture.items[id_row - 1].action == kReceivedAttachmentActionOpenLink;
    for (u32 index = 0; merged && index < first_media; ++index)
        merged = fixture.items[index].action == controller.menu.items[index].action;
    check(merged, "attachment menu: message rows, then Copy Image..Open Image Link, then Copy message ID, "
                  "then plugins");
    const auto plain = message_context_controller_open(
        &controller, &fixture.popup, message, true, true, false, nullptr, anchor, viewport, true, {}, false,
        false, false, false, false, false, {}, {media, media_count});
    check(plain.ok() && fixture.popup.item_count &&
              fixture.items[fixture.popup.item_count - 1].action == kReceivedAttachmentActionOpenLink,
          "attachment menu: without developer mode the link group ends the menu");
    PopupItem colliding = plugin;
    colliding.action = kReceivedAttachmentActionCopyLink;
    PopupItem builtin = media[0];
    builtin.action = static_cast<u32>(MessageContextAction::copy_id);
    check(message_context_controller_open(&controller, &fixture.popup, message, true, true, false, nullptr,
                                          anchor, viewport, true, {&colliding, 1}, false, false, false,
                                          false, false, true, {}, {media, media_count})
                      .error == MessageContextControllerError::invalid_input &&
              message_context_controller_open(&controller, &fixture.popup, message, true, true, false,
                                              nullptr, anchor, viewport, true, {}, false, false, false,
                                              false, false, true, {}, {&builtin, 1})
                      .error == MessageContextControllerError::invalid_input,
          "attachment menu: media rows cannot collide with plugin rows or impersonate built-in actions");

    // A right-clicked text or embed link: Copy Link / Open Link named after
    // the URL, in the same slot before Copy message ID.
    PopupItem link[kMessageLinkMenuItems]{};
    const bool named =
        message_link_menu(utf8_literal("https://example.com/page"), {link, 2}) == 2 &&
        utf8_equal(link[0].label, utf8_literal("Copy Link")) && utf8_equal(link[1].label, utf8_literal("Open Link")) &&
        link[0].action == kMessageLinkActionCopy && link[1].action == kMessageLinkActionOpen &&
        (link[0].flags & popup_item_separator_before) && !(link[1].flags & popup_item_separator_before) &&
        message_link_menu_action(link[0].action) && message_link_menu_action(link[1].action) &&
        !received_attachment_menu_action(link[0].action) && !message_link_menu_action(kReceivedAttachmentActionCopyLink) &&
        message_link_menu(image_url, {link, 2}) == 2 && utf8_equal(link[0].label, utf8_literal("Copy Image Link")) &&
        message_link_menu(utf8_literal("https://e.test/clip.mp4?x=1"), {link, 2}) == 2 &&
        utf8_equal(link[1].label, utf8_literal("Open Media Link")) &&
        message_link_menu(utf8_literal("discord://-/channels/1"), {link, 2}) == 2 &&
        utf8_equal(link[0].label, utf8_literal("Copy Link")) && !message_link_menu({}, {link, 2}) &&
        !message_link_menu(utf8_literal("https://e.test"), {link, 1});
    check(named, "link menu: Copy/Open Link labels per URL kind; empty URLs and short outputs build nothing");
    message_link_menu(utf8_literal("https://example.com/page"), {link, 2});
    const auto linked = message_context_controller_open(&controller, &fixture.popup, message, true, true, false,
                                                        nullptr, anchor, viewport, true, {}, false, false, false,
                                                        false, false, true, {}, {link, 2});
    const u32 rows = fixture.popup.item_count;
    check(linked.ok() && rows >= 3 && fixture.items[rows - 1].action == static_cast<u32>(MessageContextAction::copy_id) &&
              fixture.items[rows - 2].action == kMessageLinkActionOpen &&
              fixture.items[rows - 3].action == kMessageLinkActionCopy &&
              (fixture.items[rows - 3].flags & popup_item_separator_before) &&
              fixture.items[0].action == controller.menu.items[0].action,
          "link menu: merged after the message sections and before Copy message ID");
}
} // namespace

void test_message_context_controller_extensions(void (*check)(bool, const char*) noexcept) noexcept {
    static_assert(kMessageContextMaximumExtraItems == 8);
    static_assert(kMessageContextMaximumPopupItems == 33);
    PopupFixture fixture{};
    check(fixture.initialize(), "message context extras: initialize bounded popup");
    MessageRecord message{};
    message.id = 10;
    message.channel_id = 20;
    message.author_id = 30;
    MessageContextController controller{};
    const UiRect anchor{40, 40, 50, 50}, viewport{0, 0, 800, 1000};
    PopupItem extras[kMessageContextMaximumExtraItems + 1]{};
    for (u32 index = 0; index < kMessageContextMaximumExtraItems + 1; ++index)
        extras[index] = {utf8_literal("Extra action"), 1000 + index,
            static_cast<u16>(index == 1 ? popup_item_separator_before : popup_item_enabled)};

    ReactionPreferenceStore preferences{};
    check(reaction_preferences_initialize(&preferences), "message context extras: initialize preferences");
    const auto catalog = unicode_emoji_catalog();
    u16 chosen = kReactionPreferenceInvalidCatalogIndex;
    for (u32 index = 0; index < catalog.count; ++index)
        if (utf8_equal(catalog[index].name(), utf8_literal("shark"))) chosen = static_cast<u16>(index);
    check(chosen != kReactionPreferenceInvalidCatalogIndex &&
        reaction_preferences_track_emoji(&preferences, ReactionPreferenceKind::reaction, reaction_emoji_unicode(chosen),
                                         1000).ok(),
        "message context extras: customize quick reaction preference");

    const auto opened = message_context_controller_open(&controller, &fixture.popup, message,
        true, true, true, &preferences, anchor, viewport, true, {extras, 8});
    check(opened.ok() && opened.changed && fixture.popup.item_count == 21 && controller.menu.count == 13,
        "message context extras: thirteen builtins plus eight extras reach twenty-one row boundary");
    bool builtin_order = true, extra_order = true;
    for (u32 index = 0; index < controller.menu.count; ++index)
        builtin_order = builtin_order && fixture.items[index].action == controller.menu.items[index].action &&
            fixture.items[index].flags == controller.menu.items[index].flags &&
            utf8_equal(fixture.items[index].label, controller.menu.items[index].label);
    for (u32 index = 0; index < 8; ++index)
        extra_order = extra_order && fixture.items[13 + index].action == extras[index].action &&
            fixture.items[13 + index].label.data == extras[index].label.data &&
            ((fixture.items[13 + index].flags & popup_item_separator_before) != 0) == (index == 0);
    check(builtin_order && extra_order && (extras[0].flags & popup_item_separator_before) == 0 &&
        (extras[1].flags & popup_item_separator_before) != 0,
        "message context extras: builtin and registration order retained with one host separator");
    check((fixture.items[14].flags & popup_item_enabled) == 0,
        "message context extras: disabled contribution remains disabled");
    MessageContextQuickReaction quick{};
    const u32 quick_action = static_cast<u32>(MessageContextAction::quick_reaction_0);
    check(message_context_controller_resolve_quick_reaction(controller, quick_action, &quick) &&
        quick.catalog_index == chosen && !message_context_controller_resolve_quick_reaction(
            controller, extras[0].action, &quick),
        "message context extras: quick reactions retain captured catalog and reject extra actions");

    const u32 old_revision = fixture.popup.revision, old_opens = controller.opens;
    const auto old_panel = fixture.popup.panel;
    const UiRect old_rectangle = fixture.layout[fixture.widgets[old_panel.slot].layout.slot].rectangle;
    message.id = 11;
    auto preserved = [&]() noexcept {
        const auto* panel = popup_scene_layout_node(fixture.popup, old_panel);
        MessageContextQuickReaction resolved{};
        return fixture.popup.active && fixture.popup.owner_id == 10 && fixture.popup.item_count == 21 &&
            fixture.popup.revision == old_revision && controller.message_id == 10 &&
            controller.opens == old_opens && controller.active && panel &&
            ui_rect_equal(panel->rectangle, old_rectangle) &&
            message_context_controller_resolve_quick_reaction(controller, quick_action, &resolved) &&
            resolved.catalog_index == chosen && fixture.items[13].action == 1000;
    };
    auto reject_extra = [&](ConstSlice<PopupItem> value, const char* name) noexcept {
        const auto result = message_context_controller_open(&controller, &fixture.popup, message,
            false, false, false, nullptr, anchor, viewport, false, value);
        check(result.error == MessageContextControllerError::invalid_input && !result.changed && preserved(), name);
    };
    reject_extra({extras, 9}, "message context extras: ninth contribution rejects atomically");
    reject_extra({nullptr, 1}, "message context extras: missing contribution storage rejects atomically");
    PopupItem invalid = extras[0];
    invalid.label = {};
    reject_extra({&invalid, 1}, "message context extras: empty label preserves prior popup and model");
    const char malformed[] = {static_cast<char>(0xC0), static_cast<char>(0xAF)};
    invalid.label = {malformed, 2};
    reject_extra({&invalid, 1}, "message context extras: malformed UTF8 preserves prior popup and model");
    invalid.label = {nullptr, 1};
    reject_extra({&invalid, 1}, "message context extras: invalid label storage preserves prior popup");
    char oversized[kMessageContextMaximumExtraLabelBytes + 1]{};
    for (u32 index = 0; index < sizeof(oversized); ++index) oversized[index] = 'a';
    invalid.label = {oversized, sizeof(oversized)};
    reject_extra({&invalid, 1}, "message context extras: one hundred twenty-nine byte label rejects atomically");
    invalid = extras[0]; invalid.action = 0;
    reject_extra({&invalid, 1}, "message context extras: zero action rejects atomically");
    invalid.action = static_cast<u32>(MessageContextAction::edit_message);
    reject_extra({&invalid, 1}, "message context extras: absent builtin action cannot be impersonated");
    invalid.action = static_cast<u32>(MessageContextAction::pin_message);
    reject_extra({&invalid, 1}, "message context extras: absent pin action cannot be impersonated");
    invalid.action = static_cast<u32>(MessageContextAction::unpin_message);
    reject_extra({&invalid, 1}, "message context extras: absent unpin action cannot be impersonated");
    invalid.action = static_cast<u32>(MessageContextAction::mark_unread);
    reject_extra({&invalid, 1}, "message context extras: absent Mark unread action cannot be impersonated");
    extras[1].action = extras[0].action;
    reject_extra({extras, 2}, "message context extras: duplicate action IDs reject atomically");
    extras[1].action = 1001;
    invalid = extras[0]; invalid.flags |= popup_item_quick_reaction;
    reject_extra({&invalid, 1}, "message context extras: contributions cannot alter quick-strip semantics");

    const auto old_item_capacity = fixture.popup.items.count;
    fixture.popup.items.count = 9;
    const auto capacity_result = message_context_controller_open(&controller, &fixture.popup, message,
        false, false, false, nullptr, anchor, viewport, false, {extras, 1});
    fixture.popup.items.count = old_item_capacity;
    check(capacity_result.error == MessageContextControllerError::popup && preserved(),
        "message context extras: combined storage exhaustion preserves old popup and quick reaction");

    check(ui_widget_manager_set_measure_callback(&fixture.popup.widgets, reject_measure, nullptr),
        "message context extras: install failing measurement callback");
    const auto failed_layout = message_context_controller_open(&controller, &fixture.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {extras, 8});
    check(failed_layout.error == MessageContextControllerError::popup && preserved(),
        "message context extras: post-reset layout failure restores prior handles and rows");
    check(ui_widget_manager_set_measure_callback(&fixture.popup.widgets, nullptr, nullptr),
        "message context extras: remove failing measurement callback");
    const auto no_extras = message_context_controller_open(&controller, &fixture.popup, message,
        false, false, false, nullptr, anchor, viewport);
    check(no_extras.ok() && fixture.popup.item_count == 9 && controller.message_id == 11,
        "message context extras: unchanged callers open builtin-only menus");
    message_context_controller_close(&controller);
    check(!message_context_controller_resolve_quick_reaction(controller, quick_action, &quick),
        "message context extras: closed controller rejects stale quick reaction actions");
    check(message_context_controller_open(nullptr, &fixture.popup, message,
        false, false, false, nullptr, anchor, viewport).error == MessageContextControllerError::invalid_input,
        "message context extras: null controller rejection does not dereference it");
    invalid = extras[0]; invalid.label = {oversized, kMessageContextMaximumExtraLabelBytes};
    check(message_context_controller_open(&controller, &fixture.popup, message,
        false, false, false, nullptr, anchor, viewport, false, {&invalid, 1}).ok() &&
        fixture.items[9].label.bytes == 128,
        "message context extras: one hundred twenty-eight byte label is admitted at exact boundary");
    message_context_controller_close(&controller);

    // A previous owner may have a larger tree than this controller ever builds.
    PopupItem prior[kTestItems]{};
    for (u32 index = 0; index < kTestItems; ++index)
        prior[index] = {utf8_literal("Previous popup owner"), 2000 + index, popup_item_enabled};
    check(popup_scene_open(&fixture.popup, PopupKind::context_menu, 99, {prior, kTestItems},
        anchor, viewport, 224).ok(), "message context extras: open prior popup larger than twenty-one rows");
    const auto old_first_label = fixture.rows[0].label, old_last_label = fixture.rows[kTestItems - 1].label;
    const auto* first_layout = popup_scene_layout_node(fixture.popup, old_first_label);
    const auto* last_layout = popup_scene_layout_node(fixture.popup, old_last_label);
    const UiRect first_rectangle = first_layout ? first_layout->rectangle : UiRect{};
    const UiRect last_rectangle = last_layout ? last_layout->rectangle : UiRect{};
    const u32 prior_revision = fixture.popup.revision;
    check(ui_widget_manager_set_measure_callback(&fixture.popup.widgets, reject_measure, nullptr),
        "message context extras: inject failure while a larger owner is retained");
    const auto larger_failure = message_context_controller_open(&controller, &fixture.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {extras, 8});
    first_layout = popup_scene_layout_node(fixture.popup, old_first_label);
    last_layout = popup_scene_layout_node(fixture.popup, old_last_label);
    check(larger_failure.error == MessageContextControllerError::popup && fixture.popup.owner_id == 99 &&
        fixture.popup.item_count == kTestItems && fixture.popup.revision == prior_revision &&
        !controller.active && first_layout && last_layout &&
        ui_rect_equal(first_layout->rectangle, first_rectangle) &&
        ui_rect_equal(last_layout->rectangle, last_rectangle) &&
        utf8_equal(fixture.items[0].label, prior[0].label) &&
        fixture.items[kTestItems - 1].action == prior[kTestItems - 1].action,
        "message context extras: rollback restores overwritten prefix and preserves larger tree tail");

    PopupFixture owned{};
    check(popup_scene_initialize_owned(&owned.popup, kMessageContextMaximumPopupItems,
        {owned.widgets, kTestWidgets}, {owned.layout, kTestWidgets}),
        "message context extras: initialize dormant owned popup backing");
    check(ui_widget_manager_set_measure_callback(&owned.popup.widgets, reject_measure, nullptr),
        "message context extras: inject failure on first owned popup materialization");
    const auto first_failure = message_context_controller_open(&controller, &owned.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {extras, 8});
    check(first_failure.error == MessageContextControllerError::popup && !owned.popup.active &&
        !owned.popup.item_count && !owned.popup.widgets.count && !controller.active &&
        owned.popup.items.data && owned.popup.rows.data && owned.popup.owned_storage.offset,
        "message context extras: failed first open retains materialized backing ownership without publication");
    check(ui_widget_manager_set_measure_callback(&owned.popup.widgets, nullptr, nullptr),
        "message context extras: allow reuse after first materialization failure");
    check(message_context_controller_open(&controller, &owned.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {extras, 8}).ok() &&
        owned.popup.item_count == 21, "message context extras: owned storage remains reusable after rollback");
    popup_scene_release(&owned.popup);
    check(!owned.popup.owned_storage.base, "message context extras: retained failed-open backing releases normally");

    PopupFixture pin_popup{};
    check(pin_popup.initialize(kMessageContextMaximumPopupItems),
        "message context pins: initialize exact combined row capacity");
    const auto pin_open = message_context_controller_open(&controller, &pin_popup.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {extras, 8}, true);
    check(pin_open.ok() && controller.menu.count == 14 &&
        pin_popup.popup.item_count == 22 &&
        pin_popup.items[9].action == static_cast<u32>(MessageContextAction::pin_message) &&
        pin_popup.items[14].action == extras[0].action &&
        (pin_popup.items[14].flags & popup_item_separator_before) != 0,
        "message context pins: fourteen builtins plus eight extensions preserve one section separator");
    message.presentation_flags |= message_presentation_pinned;
    check(message_context_controller_open(&controller, &pin_popup.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {extras, 8}, true).ok() &&
        pin_popup.items[9].action == static_cast<u32>(MessageContextAction::unpin_message) &&
        pin_popup.popup.item_count == 22,
        "message context pins: pinned message projects Unpin with unchanged extension capacity");
    const u32 pin_revision = pin_popup.popup.revision;
    pin_popup.popup.items.count = 21;
    const auto pin_capacity = message_context_controller_open(&controller, &pin_popup.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {extras, 8}, true);
    pin_popup.popup.items.count = kMessageContextMaximumPopupItems;
    check(pin_capacity.error == MessageContextControllerError::popup &&
        pin_popup.popup.revision == pin_revision &&
        pin_popup.items[9].action == static_cast<u32>(MessageContextAction::unpin_message) &&
        pin_popup.items[14].action == extras[0].action,
        "message context pins: one-short combined capacity preserves prior pin action and extensions");

    message.presentation_flags = 0;
    const auto forward_open = message_context_controller_open(&controller, &pin_popup.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {extras, 8}, true, true);
    check(forward_open.ok() && controller.menu.count == 15 &&
        pin_popup.popup.item_count == 23 &&
        pin_popup.items[9].action == static_cast<u32>(MessageContextAction::forward_message) &&
        ui_menu_row_icon(pin_popup.items[9].presentation) == UiIcon::forwarded &&
        pin_popup.items[10].action == static_cast<u32>(MessageContextAction::pin_message) &&
        pin_popup.items[15].action == extras[0].action,
        "message context forward: fifteen builtins plus eight extensions retain full capacity and native icon");
    const u32 forward_revision = pin_popup.popup.revision;
    PopupItem forward_collision = extras[0];
    forward_collision.action = static_cast<u32>(MessageContextAction::forward_message);
    const auto forward_extra = message_context_controller_open(&controller, &pin_popup.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {&forward_collision, 1});
    check(forward_extra.error == MessageContextControllerError::invalid_input &&
        pin_popup.popup.revision == forward_revision,
        "message context forward: plugin extras cannot collide with absent built-in forward action");
    static_assert(static_cast<u32>(MessageContextAction::create_thread) == 20);
    check(message_context_controller_open(&controller, &pin_popup.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {extras, 8}, true, true, true, false, true).ok() &&
        controller.menu.count == 17 && pin_popup.popup.item_count == 25,
        "message context creation: exact worst-case seventeen builtins plus eight extras fit without omitted actions");
    bool create_found = false, unread_found = false, delete_found = false;
    u32 create_index = ~0u;
    for (u32 i = 0; i < controller.menu.count; ++i) {
        const auto action = controller.menu.items[i].action;
        create_found |= action == static_cast<u32>(MessageContextAction::create_thread);
        if (action == static_cast<u32>(MessageContextAction::create_thread)) create_index = i;
        unread_found |= action == static_cast<u32>(MessageContextAction::mark_unread);
        delete_found |= action == static_cast<u32>(MessageContextAction::delete_message);
    }
    check(create_found && unread_found && delete_found && pin_popup.items[17].action == extras[0].action &&
        (pin_popup.items[17].flags & popup_item_separator_before) != 0,
        "message context creation: native creation unread deletion and plugin sections all remain present");
    bool creation_focus_changed = false;
    PopupItemLayout creation_layout{};
    check(create_index < pin_popup.popup.item_count &&
        popup_scene_select_index(&pin_popup.popup, create_index, &creation_focus_changed) &&
        popup_scene_selected_action(pin_popup.popup) == static_cast<u32>(MessageContextAction::create_thread) &&
        popup_scene_resolve_item_layout(pin_popup.popup, create_index, &creation_layout) && creation_layout.visible &&
        popup_scene_hit_test(pin_popup.popup,
            (creation_layout.visible_rectangle.left + creation_layout.visible_rectangle.right) * .5f,
            (creation_layout.visible_rectangle.top + creation_layout.visible_rectangle.bottom) * .5f).action ==
                static_cast<u32>(MessageContextAction::create_thread),
        "message context creation: actual retained keyboard focus and pointer hit resolve same Create thread action");
    const u32 creation_revision = pin_popup.popup.revision;
    pin_popup.popup.items.count = 24;
    check(message_context_controller_open(&controller, &pin_popup.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {extras, 8}, true, true, true, false, true).error ==
            MessageContextControllerError::popup && pin_popup.popup.revision == creation_revision &&
        pin_popup.popup.item_count == 25,
        "message context creation: one-short popup rejects atomically and preserves all retained actions");
    pin_popup.popup.items.count = kMessageContextMaximumPopupItems;
    PopupItem create_collision = extras[0];
    create_collision.action = static_cast<u32>(MessageContextAction::create_thread);
    check(message_context_controller_open(&controller, &pin_popup.popup, message,
        false, false, false, nullptr, anchor, viewport, false, {&create_collision, 1}).error ==
            MessageContextControllerError::invalid_input && pin_popup.popup.revision == creation_revision,
        "message context creation: plugins cannot impersonate absent native Create thread action");
    message.delivery = MessageDeliveryState::pending;
    check(message_context_controller_open(&controller, &pin_popup.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {}, true, true, true, false, true).ok() &&
        controller.menu.count == 1, "message context forward: pending outer message cannot forward");
    message.delivery = MessageDeliveryState::failed;
    check(message_context_controller_open(&controller, &pin_popup.popup, message,
        true, true, true, nullptr, anchor, viewport, true, {}, true, true, true, false, true).ok() &&
        controller.menu.count == 3, "message context forward: failed local message cannot forward");
    test_received_attachment_menu(check);
}
} // namespace maicord
