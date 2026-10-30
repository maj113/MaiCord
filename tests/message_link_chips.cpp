// Link chips: CDN attachment URLs become a paperclip chip with the file name,
// and /channels/ URLs and <#id> references become channel, server and message
// chips. Icon cells are measured spaces that
// keep their icon on the label's line; the renderer draws the icons.
#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/received_attachment_menu.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/text/visibility.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/attachment_refresh.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

// An icon cell (U+2003), its margins (U+2005 4 px, U+2006 3 px, U+200A 2 px),
// the chevron's half cell (U+2002) and the closing glue (U+200D).
#define CELL "\xE2\x80\x83"
#define HALF "\xE2\x80\x82"
#define GAP4 "\xE2\x80\x85"
#define GAP3 "\xE2\x80\x86"
#define GAP2 "\xE2\x80\x8A"
#define GLUE "\xE2\x80\x8D"
#define ICON CELL GAP4 GLUE
#define CHEVRON GAP4 HALF GAP2 GLUE

struct ChipFixture {
    u64 own_guild;
};

constexpr u8 kGuildKnown = message_markdown_channel_guild_known;
constexpr u8 kChannelKnown = message_markdown_channel_known | message_markdown_channel_viewable |
                             message_markdown_channel_mentionable;

// Guild 10 "Factorio" (with an icon) holds #general (2), Lounge voice (3),
// the Info category (4), a hidden channel (5), the help forum (11) with its
// post (6) and a 40-character channel (9). 7 is a DM, 8 a channel of a server
// that is not in the store, 12 #bench of "Lab Tools" (30, no icon); anything
// else is unknown.
bool resolve_channel(void* context, u64 guild_id, u64 channel_id, MessageMarkdownChannelTarget* target) noexcept {
    const u64 own = static_cast<const ChipFixture*>(context)->own_guild;
    u8 flags = kChannelKnown;
    u64 guild = 10;
    target->icon = MessageMarkdownChipIcon::text_channel;
    switch (channel_id) {
    case 2: target->name = utf8_literal("general"); break;
    case 3: target->name = utf8_literal("Lounge"); target->icon = MessageMarkdownChipIcon::voice; break;
    case 4: target->name = utf8_literal("Info"); flags &= ~message_markdown_channel_mentionable; break;
    case 5: target->name = utf8_literal("staff"); flags &= ~message_markdown_channel_viewable; break;
    case 6:
        target->name = utf8_literal("post-title");
        target->icon = MessageMarkdownChipIcon::message;
        target->parent_name = utf8_literal("help");
        target->parent_icon = MessageMarkdownChipIcon::forum;
        flags |= message_markdown_channel_forum_post | message_markdown_channel_parent_known;
        break;
    case 7: target->name = utf8_literal("blueprintbot"); flags |= message_markdown_channel_private; guild = 0; break;
    case 8: target->name = utf8_literal("elsewhere"); guild = 20; break;
    case 9: target->name = utf8_literal("abcdefghijklmnopqrstuvwxyz0123456789ABCD"); break;
    case 12: target->name = utf8_literal("bench"); guild = 30; break;
    default: flags = 0; guild = guild_id; break;
    }
    if (guild == 10) {
        flags |= kGuildKnown | (own == 10 ? message_markdown_channel_same_guild : 0);
        target->guild_id = 10;
        target->guild_name = utf8_literal("Factorio");
        target->guild_icon = true;
    }
    if (guild == 30) {
        flags |= kGuildKnown;
        target->guild_id = 30;
        target->guild_name = utf8_literal("Lab Tools");
    }
    target->flags = flags;
    return flags != 0;
}

struct Parsed {
    char text[1024];
    u32 styles[1024];
    TextStyleSpan spans[128];
    MessageMarkdownSemantic semantics[32];
    MessageMarkdownResult result;
};

Parsed parsed;

const MessageMarkdownResult& parse(Utf8View input, u64 own_guild = 10, MessageMarkdownOptions options = {}) noexcept {
    static ChipFixture fixture{};
    fixture.own_guild = own_guild;
    options.context = &fixture;
    options.resolve_channel = resolve_channel;
    parsed.result = message_markdown_parse(input, {parsed.text, 1024}, {parsed.styles, 1024}, {parsed.spans, 128},
                                           options, {parsed.semantics, 32});
    return parsed.result;
}

bool text_is(Utf8View expected) noexcept {
    return parsed.result.ok() && utf8_equal(parsed.result.text, expected);
}

// The semantics as (kind, target) pairs, in order.
struct Expected {
    MessageMarkdownSemanticKind kind;
    u64 target;
};
template <u32 N> bool semantics_are(const Expected (&expected)[N]) noexcept {
    if (parsed.result.semantic_count != N)
        return false;
    for (u32 i = 0; i < N; ++i)
        if (parsed.semantics[i].kind != expected[i].kind || parsed.semantics[i].target != expected[i].target)
            return false;
    return true;
}

Utf8View source_of(Utf8View input, const MessageMarkdownSemantic& semantic) noexcept {
    return {input.data + semantic.source_offset, semantic.source_bytes};
}

using K = MessageMarkdownSemanticKind;
using I = MessageMarkdownChipIcon;
constexpr u64 icon(I value) noexcept {
    return static_cast<u64>(value);
}

void test_message_links(Check check) noexcept {
    const Utf8View same = utf8_literal("go https://discord.com/channels/10/2/3 now");
    parse(same);
    const MessageMarkdownSemantic& link = parsed.semantics[3];
    check(text_is(utf8_literal("go " ICON "general" CHEVRON ICON " now")) &&
              semantics_are({{K::chip_icon, icon(I::text_channel)},
                             {K::chip_icon, icon(I::chevron)},
                             {K::chip_icon, icon(I::message)},
                             {K::message_link, 3}}) &&
              parsed.semantics[0].byte_offset == 3 && parsed.semantics[0].byte_count == 3 &&
              parsed.semantics[1].byte_count == 3 && link.byte_offset == 3 &&
              link.byte_offset + link.byte_count == parsed.result.text.bytes - 4 &&
              utf8_equal(source_of(same, link), utf8_literal("https://discord.com/channels/10/2/3")) &&
              (parsed.styles[3] & text_style_mention) && (parsed.styles[12] & text_style_mention) &&
              !(parsed.styles[0] & text_style_mention),
          "chip: a same-server message link is #channel › message icon");
    parse(utf8_literal("https://discord.com/channels/10/2/3"), 0);
    check(text_is(utf8_literal(CELL GAP3 GLUE "Factorio" CHEVRON ICON)) &&
              semantics_are({{K::guild_icon, 10},
                             {K::chip_icon, icon(I::chevron)},
                             {K::chip_icon, icon(I::message)},
                             {K::message_link, 3}}),
          "chip: another server's message link is server icon, name › message icon");
    parse(utf8_literal("https://discord.com/channels/10/2"));
    check(text_is(utf8_literal(ICON "general")) &&
              semantics_are({{K::chip_icon, icon(I::text_channel)}, {K::channel_mention, 2}}),
          "chip: a same-server channel link is the channel alone");
    parse(utf8_literal("https://discord.com/channels/10/3"), 0);
    check(text_is(utf8_literal(CELL GAP3 GLUE "Factorio" CHEVRON ICON "Lounge")) &&
              semantics_are({{K::guild_icon, 10},
                             {K::chip_icon, icon(I::chevron)},
                             {K::chip_icon, icon(I::voice)},
                             {K::channel_mention, 3}}),
          "chip: another server's channel link names the server, then the channel with its type icon");
    parse(utf8_literal("https://discord.com/channels/10/6/9"));
    check(text_is(utf8_literal(ICON "help" CHEVRON ICON "post-title")) &&
              semantics_are({{K::chip_icon, icon(I::forum)},
                             {K::chip_icon, icon(I::chevron)},
                             {K::chip_icon, icon(I::message)},
                             {K::message_link, 9}}),
          "chip: a forum post message link is forum › post");
    parse(utf8_literal("https://discord.com/channels/10/6/9"), 0);
    check(text_is(utf8_literal(CELL GAP3 GLUE "Factorio" CHEVRON ICON "post-title")),
          "chip: another server's forum post link shows the post instead of the message icon");
    parse(utf8_literal("https://discord.com/channels/@me/7/9"), 0);
    check(text_is(utf8_literal(ICON "blueprintbot" CHEVRON ICON)) &&
              semantics_are({{K::chip_icon, icon(I::text_channel)},
                             {K::chip_icon, icon(I::chevron)},
                             {K::chip_icon, icon(I::message)},
                             {K::message_link, 9}}),
          "chip: a DM link is the conversation › message icon");
    parse(utf8_literal("https://discord.com/channels/10/5/9"));
    check(text_is(utf8_literal(ICON "No Access")) &&
              semantics_are({{K::chip_icon, icon(I::locked)}, {K::message_link, 9}}),
          "chip: an inaccessible channel is a locked No Access chip");
    parse(utf8_literal("see https://discord.com/channels/10/4/9."));
    check(text_is(utf8_literal("see #Info.")) && parsed.result.semantic_count == 0,
          "chip: a category reference is plain #name text");
    const Utf8View foreign = utf8_literal("https://discord.com/channels/20/8/9");
    parse(foreign);
    check(text_is(foreign) && parsed.result.semantic_count == 1 && parsed.semantics[0].kind == K::link,
          "chip: a channel of a server that is not loaded stays an ordinary link");
    parse(utf8_literal("https://discord.com/channels/10/99/9"), 0);
    check(text_is(utf8_literal(CELL GAP3 GLUE "Factorio" CHEVRON ICON "unknown")) &&
              (parsed.styles[parsed.result.text.bytes - 1] & text_style_italic) &&
              semantics_are({{K::guild_icon, 10},
                             {K::chip_icon, icon(I::chevron)},
                             {K::chip_icon, icon(I::text_channel)},
                             {K::message_link, 9}}),
          "chip: an unknown channel of a known other server is server › italic unknown");
    parse(utf8_literal("https://discord.com/channels/10/99/9"));
    check(text_is(utf8_literal(ICON "unknown")), "chip: an unknown channel of the own server is italic unknown");
    parse(utf8_literal("https://discord.com/channels/10/9"));
    check(text_is(utf8_literal(ICON "abcdefghijklmnopqrstuvwxyz01234\xE2\x80\xA6")),
          "chip: names longer than 32 units keep 31 characters and an ellipsis");
    parse(utf8_literal("https://discord.com/channels/30/12/9"));
    check(text_is(utf8_literal(CELL GAP3 GLUE "Lab Tools" CHEVRON ICON)) &&
              semantics_are({{K::guild_icon, 30},
                             {K::chip_icon, icon(I::chevron)},
                             {K::chip_icon, icon(I::message)},
                             {K::message_link, 9}}) &&
              (parsed.semantics[0].reserved & kMessageMarkdownChipInitials),
          "chip: a server without an icon keeps its cell for the initials plate");
    parse(utf8_literal("https://discord.com/channels/10/2/3"), 0);
    check(!(parsed.semantics[0].reserved & kMessageMarkdownChipInitials),
          "chip: a server with an icon draws the icon, not initials");
    // Thread message URLs: the thread when loaded, else its parent channel.
    const Utf8View thread = utf8_literal("https://discord.com/channels/10/11/threads/6/9 then");
    parse(thread);
    MessageLink target{};
    check(text_is(utf8_literal(ICON "help" CHEVRON ICON "post-title then")) &&
              parsed.semantics[3].kind == K::message_link && parsed.semantics[3].target == 9 &&
              message_markdown_message_link(source_of(thread, parsed.semantics[3]), &target) &&
              target.guild_id == 10 && target.channel_id == 6 && target.message_id == 9 &&
              target.parent_channel_id == 11,
          "chip: a /threads/ message URL shows its thread and navigates to it");
    parse(utf8_literal("https://discord.com/channels/10/2/threads/77/9"));
    check(text_is(utf8_literal(ICON "general" CHEVRON ICON)), "chip: a thread that is not loaded shows its parent");
    const Utf8View broken = utf8_literal("https://discord.com/channels/10/2/threads/77");
    parse(broken);
    check(text_is(broken) && parsed.semantics[0].kind == K::link,
          "chip: a /threads/ URL without a message is an ordinary link");
}

void test_channel_references(Check check) noexcept {
    parse(utf8_literal("in <#2>, <#3> and <#5>"));
    check(text_is(utf8_literal("in " ICON "general, " ICON "Lounge and " ICON "No Access")) &&
              semantics_are({{K::chip_icon, icon(I::text_channel)},
                             {K::channel_mention, 2},
                             {K::chip_icon, icon(I::voice)},
                             {K::channel_mention, 3},
                             {K::chip_icon, icon(I::locked)},
                             {K::channel_mention, 5}}) &&
              parsed.result.resolved_entities == 3,
          "chip: <#id> shows the channel type icon and the name");
    parse(utf8_literal("<#99> <#8> <#4>"));
    check(text_is(utf8_literal(ICON "unknown #unknown-channel #Info")) &&
              semantics_are({{K::chip_icon, icon(I::text_channel)}, {K::channel_mention, 99}}),
          "chip: unknown <#id> is italic unknown; an unloaded server's channel and a category are text");
    MessageMarkdownOptions plain{};
    plain.resolve_entity = +[](void*, MessageMarkdownEntityKind, u64) noexcept { return utf8_literal("general"); };
    const auto legacy = message_markdown_parse(utf8_literal("<#2> https://discord.com/channels/10/2/3 https://discord.com/channels/10/2"),
                                               {parsed.text, 1024}, {parsed.styles, 1024}, {parsed.spans, 128},
                                               plain, {parsed.semantics, 32});
    check(legacy.ok() && utf8_equal(legacy.text, utf8_literal("#general #general \xE2\x80\xBA \xF0\x9F\x92\xAC "
                                                              "https://discord.com/channels/10/2")),
          "chip: without a channel resolver (notifications) references stay text");
    MessageMarkdownOptions hovered{};
    hovered.has_hover = true;
    hovered.hovered_kind = K::message_link;
    hovered.hovered_source_offset = 0;
    parse(utf8_literal("https://discord.com/channels/10/2/3"), 10, hovered);
    bool lit = parsed.result.ok() && parsed.result.semantic_count == 4;
    for (u32 i = 0; lit && i < parsed.result.text.bytes; ++i)
        lit = (parsed.styles[i] & (text_style_mention | text_style_interactive_hover)) ==
              (text_style_mention | text_style_interactive_hover);
    for (u32 i = 0; lit && i < 3; ++i)
        lit = (parsed.semantics[i].reserved & kMessageMarkdownChipHovered) != 0;
    check(lit, "chip: a hovered chip lights its text and icons together");
    MessageMarkdownOptions title{};
    title.no_links = true;
    const Utf8View url = utf8_literal("https://discord.com/channels/10/2/3");
    parse(url, 10, title);
    check(text_is(utf8_literal(ICON "general" CHEVRON ICON)) && parsed.result.semantic_count == 4,
          "chip: embed titles keep channel chips (the client's title rules keep channelOrMessageUrl)");
    const Utf8View attachment = utf8_literal("https://cdn.discordapp.com/attachments/1/2/a.png");
    parse(attachment, 10, title);
    check(text_is(attachment) && parsed.result.semantic_count == 0,
          "chip: embed titles drop attachment chips and links");
    MessageMarkdownOptions editor{};
    editor.preserve_entities = true;
    parse(url, 10, editor);
    check(text_is(url) && (parsed.styles[0] & text_style_link) && parsed.result.semantic_count == 0,
          "chip: editors keep the URL source");
}

void test_url_grammar(Check check) noexcept {
    parse(utf8_literal("https://ptb.discordapp.com/channels/10/2/3?summaryId=5 tail"));
    check(parsed.result.ok() && parsed.result.semantic_count == 4 && parsed.semantics[3].source_bytes == 54 &&
              utf8_equal({parsed.result.text.data + parsed.result.text.bytes - 5, 5}, utf8_literal(" tail")),
          "chip: canary/ptb discordapp hosts and a ?query belong to the link");
    parse(utf8_literal("https://discord.com/channels/10/2/3abc"));
    check(text_is(utf8_literal(ICON "general" CHEVRON ICON "abc")),
          "chip: the link ends after the message id; the rest stays text");
    const Utf8View named = utf8_literal("https://discord.com/channels/10/shop");
    parse(named);
    check(text_is(named) && parsed.result.semantic_count == 1 && parsed.semantics[0].kind == K::link,
          "chip: a non-numeric channel segment is an ordinary link");
    const Utf8View guild_only = utf8_literal("https://discord.com/channels/10");
    parse(guild_only);
    check(text_is(guild_only) && parsed.semantics[0].kind == K::link, "chip: a server URL is an ordinary link");
    MessageLink link{};
    check(message_markdown_message_link(utf8_literal("https://canary.discordapp.com/channels/@me/2/3?x=1"), &link) &&
              link.guild_id == 0 && link.channel_id == 2 && link.message_id == 3 &&
              !message_markdown_message_link(utf8_literal("https://discord.com/channels/10/2"), &link) &&
              !message_markdown_message_link(utf8_literal("https://www.discord.com/channels/10/2/3"), &link) &&
              !message_markdown_message_link(utf8_literal("https://discord.com/channels/10/2/3/4"), &link),
          "chip: message link recognition covers the official hosts and whole URLs only");
}

void test_attachment_links(Check check) noexcept {
    const Utf8View screenshot = utf8_literal(
        "====> https://cdn.discordapp.com/attachments/334735604342325249/1565415049101705277/blueprint.jpg?"
        "ex=6ae4d1df&is=6ae3805f&hm=eff4daf7c0566db422e09ae10383f042447f5a0827922a441ddd8bf7a2d8a674&");
    parse(screenshot);
    check(text_is(utf8_literal("====> " CELL GAP2 GLUE "blueprint.jpg")) &&
              semantics_are({{K::chip_icon, icon(I::attachment)}, {K::link, 0}}) &&
              (parsed.semantics[1].reserved & kMessageMarkdownAttachmentChip) &&
              utf8_equal(source_of(screenshot, parsed.semantics[1]), {screenshot.data + 6, screenshot.bytes - 6}) &&
              (parsed.styles[6] & text_style_mention) && (parsed.styles[parsed.result.text.bytes - 1] & text_style_mention),
          "attachment chip: a signed CDN URL shows a paperclip and the file name");
    parse(utf8_literal("https://cdn.discordapp.com/attachments/1/2/my%20file.png"));
    check(text_is(utf8_literal(CELL GAP2 GLUE "my%20file.png")) && parsed.semantics[1].source_bytes == 45,
          "attachment chip: the name stops at the first character outside [A-Za-z0-9._-]");
    parse(utf8_literal("[x](https://cdn.discordapp.com/attachments/1/2/a.png) <https://cdn.discordapp.com/attachments/1/2/a.png>"));
    check(text_is(utf8_literal("x https://cdn.discordapp.com/attachments/1/2/a.png")) &&
              semantics_are({{K::link, 0}, {K::link, 0}}) && !parsed.semantics[0].reserved && !parsed.semantics[1].reserved,
          "attachment chip: masked and <angle> links stay links");
    Utf8View name{};
    const auto chip = [&](const char* url) noexcept {
        u32 bytes = 0;
        while (url[bytes])
            ++bytes;
        return message_markdown_attachment_link({url, bytes}, &name);
    };
    check(chip("https://media.discordapp.net/attachments/1/2/a.webp?width=10&height=20") &&
              utf8_equal(name, utf8_literal("a.webp")) && chip("https://images-1.discordapp.net/attachments/1/2/b") &&
              chip("https://cdn-eu.discordapp.com/ephemeral-attachments/1/2/c_d-e.txt") &&
              chip("https://a.b-c.cdn.discordapp.com/attachments/1/2/f.mp4?ex=1&") &&
              !chip("http://cdn.discordapp.com/attachments/1/2/a.png") &&
              !chip("https://cdn.discordapp.com/external/1/2/a.png") &&
              !chip("https://cdn.discordapp.com/attachments/1/2/") &&
              !chip("https://cdn.discordapp.com/attachments/1/2/a.png.") &&
              !chip("https://cdn.discordapp.com/attachments/1/a.png") &&
              !chip("https://images-ext-1.discordapp.net/attachments/1/2/a.png") &&
              !chip("https://media.discordapp.com/attachments/1/2/a.png") &&
              !chip("https://Cdn.discordapp.com/attachments/1/2/a.png") &&
              !chip("https://.cdn.discordapp.com/attachments/1/2/a.png") &&
              !chip("https://cdn.discordapp.com:443/attachments/1/2/a.png") &&
              !chip("https://cdn.discordapp.com/attachments/1/2/a.png#top"),
          "attachment chip: hosts, paths, names and queries follow the official pattern");
    PopupItem items[kMessageLinkMenuItems]{};
    check(attachment_link_menu({items, kMessageLinkMenuItems}) == kMessageLinkMenuItems &&
              utf8_equal(items[0].label, utf8_literal("Copy Media Link")) && items[0].action == kMessageLinkActionCopy &&
              utf8_equal(items[1].label, utf8_literal("Copy Text")) && items[1].action == kMessageLinkActionCopyText &&
              message_link_menu_action(kMessageLinkActionCopyText) && !attachment_link_menu({items, 1}),
          "attachment chip: its menu is Copy Media Link and Copy Text");
}

void test_chip_copy(Check check) noexcept {
    const Utf8View source = utf8_literal("a https://discord.com/channels/10/2/3 b "
                                         "https://cdn.discordapp.com/attachments/1/2/c.png?ex=1 <#2>");
    parse(source);
    MessageTextCommand command{};
    command.text = parsed.result.text;
    command.styles = {parsed.spans, parsed.result.span_count};
    command.semantics = {parsed.semantics, parsed.result.semantic_count};
    char output[512]{}, partial[64]{};
    const auto whole =
        message_text_project_selection_for_copy(command, source, 0, command.text.bytes, {output, sizeof(output)});
    const auto part = message_text_project_selection_for_copy(command, source, 2, 8, {partial, sizeof(partial)});
    check(whole.ok && whole.transformed && utf8_equal({output, whole.bytes}, source) && part.ok && !part.transformed &&
              part.bytes == 6,
          "chip: a fully selected chip copies its URL or <#id> source; a partial one its text");
}

void test_chip_cells(Check check) noexcept {
    // Shaped like the chat (Segoe UI 16, hidden default-ignorables): the icon
    // cell is one em wide and wraps together with its label.
    static TextSystem system;
    if (!text_system_initialize(&system, L"Segoe UI", 16).ok()) {
        check(false, "chip_cells_text_system");
        return;
    }
    const bool previous = text_browser_invisibles();
    text_set_browser_invisibles(true);
    static TextGlyph glyphs[256];
    static TextRun runs[64];
    static u16 clusters[256];
    static TextHitRegion regions[16];
    static TextDecoration decorations[16];
    static wchar_t utf16[256];
    const Utf8View text = utf8_literal("xxxxxxxx " ICON "general" CHEVRON ICON);
    const u32 cell = 9, chevron = cell + 9 + 7 + 3, message = chevron + 9;
    const TextStyleSpan styles[]{{cell, text.bytes - cell, text_style_mention}};
    const TextHitSpan hits[]{{cell, 3, 0, 0}, {chevron, 3, 1, 0}, {message, 3, 2, 0}};
    const auto shape = [&](float width) noexcept {
        ShapedText output{};
        const bool ok = shaped_text_initialize(&output, {glyphs, 256}, {runs, 64}, {clusters, 256}, {decorations, 16}, {regions, 16}) &&
                        text_shape_utf8(&system, text, {styles, 1}, width, 1000, {utf16, 256},
                                        &output, TextWrapping::wrap, TextOverflow::clip, {hits, 3})
                            .ok() &&
                        output.hit_region_count == 3;
        const u32 lines = output.line_count;
        shaped_text_reset(&output);
        return ok ? lines : 0u;
    };
    const u32 wide = shape(1000);
    const float icon_width = regions[0].right - regions[0].left, half_width = regions[1].right - regions[1].left;
    const float wide_top = regions[0].top;
    // Too narrow for the first word and the chip (about 70 + 110 DIP), wide
    // enough for the chip alone: the whole chip wraps, icon first.
    const u32 narrow = shape(130);
    check(wide == 1 && icon_width > 15.5f && icon_width < 16.5f && half_width > 7.5f && half_width < 8.5f &&
              narrow == 2 && regions[0].top > wide_top && regions[0].left < 1.f,
          "chip: icon cells are an em (half for the chevron) and never wrap away from the label");
    text_set_browser_invisibles(previous);
    text_system_release(&system);
}

bool view_contains(Utf8View text, Utf8View needle) noexcept {
    for (u32 at = 0; at + needle.bytes <= text.bytes; ++at)
        if (bytes_equal(text.data + at, needle.data, needle.bytes))
            return true;
    return false;
}

// An attachment chip whose signature expires within the hour is refreshed
// with a refresh request before it is opened or copied.
void test_refresh_request(Check check) noexcept {
    const Utf8View expired = utf8_literal(
        "https://cdn.discordapp.com/attachments/7/70/report.pdf?ex=6ae4d1df&is=6ae3805f&hm="
        "eff4daf7c0566db422e09ae10383f042447f5a0827922a441ddd8bf7a2d8a674&");
    constexpr u64 ex = 0x6ae4d1df;
    u8 body[1024]{};
    const u32 body_bytes = attachment_refresh_body(expired, {body, sizeof(body)});
    const Utf8View json{reinterpret_cast<const char*>(body), body_bytes};
    check(attachment_refresh_needed(expired, ex - 3599) && !attachment_refresh_needed(expired, ex - 3601) &&
              !attachment_refresh_needed(utf8_literal("https://example.com/attachments/7/70/report.pdf?ex=1"), ex) &&
              view_contains(json, utf8_literal("{\"attachment_urls\":[\"https://cdn.discordapp.com/attachments/7/70/"
                                               "report.pdf?ex=6ae4d1df&is=6ae3805f&hm=")),
          "attachment refresh: within an hour of ex the chip URL is refreshed; other hosts never are");
    constexpr char token[] = "fixture";
    constexpr char properties[] = "e30=";
    const RestUserRequestHeaders headers{{reinterpret_cast<const u8*>(properties), sizeof(properties) - 1},
                                         utf8_literal("en-US"), utf8_literal("Europe/Ljubljana"),
                                         utf8_literal("en-US")};
    const RestRequestHead head{{reinterpret_cast<const u8*>(kAttachmentRefreshTarget.data), kAttachmentRefreshTarget.bytes},
                               {reinterpret_cast<const u8*>(token), sizeof(token) - 1},
                               body_bytes,
                               kAttachmentRefreshRoute.method,
                               RestAuthorization::normal_user,
                               true,
                               false,
                               &headers};
    static u8 output[4096];
    const auto written = write_rest_request_head(head, {output, sizeof(output)});
    const Utf8View request{reinterpret_cast<const char*>(output), written.bytes};
    check(written.ok() && rest_route_valid(kAttachmentRefreshRoute) &&
              kAttachmentRefreshRoute.major == RestMajorParameter::none && !kAttachmentRefreshRoute.major_key &&
              utf8_starts_with(request, utf8_literal("POST /api/v10/attachments/refresh-urls HTTP/1.1\r\n")) &&
              view_contains(request, utf8_literal("\r\nAuthorization: fixture\r\n")) &&
              view_contains(request, utf8_literal("\r\nContent-Type: application/json\r\n")) &&
              view_contains(request, utf8_literal("\r\nOrigin: https://discord.com\r\n")) &&
              view_contains(request, utf8_literal("\r\nReferer: https://discord.com/channels/@me\r\n")) &&
              view_contains(request, utf8_literal("\r\nX-Super-Properties: e30=\r\n")),
          "attachment refresh: POST /attachments/refresh-urls carries the user session's client metadata");
}
#undef CELL
#undef HALF
#undef GAP4
#undef GAP3
#undef GAP2
#undef GLUE
#undef ICON
#undef CHEVRON
} // namespace

void test_message_link_chips(Check check) noexcept {
    test_refresh_request(check);
    test_message_links(check);
    test_channel_references(check);
    test_url_grammar(check);
    test_attachment_links(check);
    test_chip_copy(check);
    test_chip_cells(check);
}
} // namespace maicord
