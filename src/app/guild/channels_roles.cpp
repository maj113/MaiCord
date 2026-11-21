#include "channels_roles.hpp"
#include "community_internal.hpp"
#include "hub.hpp"
#include "settings/settings.hpp"
#include "../account/account_presentation.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../chat/threads/browser.hpp"
#include "../notifications/controls.hpp"
#include "../settings/settings.hpp"
#include "../shell/content_input.hpp"
#include "../shell/content_page.hpp"
#include "../voice/voice.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "maicord/model/guild/permission_compute.hpp"
#include "maicord/model/guild/settings_model.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/shell/navigation_view.hpp"

namespace maicord::app_detail {
namespace {
// Browse Channels lists this many categories and channels at most.
constexpr u32 kBrowseRows = 256;
enum : u32 {
    action_customize = 1,
    action_browse,
    action_dismiss,
    action_create,
    action_option = 0x100,
    action_channel = 0x1000, // + row: follow or unfollow
    action_view = 0x2000     // + row: View, Join or Open
};

struct Page {
    GuildOnboarding onboarding;
    // Pictures: the account's avatar and the server emoji of the answers.
    UiSettingsAvatarMedia media[1 + kGuildOnboardingOptionMaximum];
    // Browse Channels: the channel or category of each row.
    u64 rows[kBrowseRows];
    u64 guild_id, account_scope, profile_id;
    // The time the choices save (0: none waiting).
    u64 save_at;
    u32 media_count, row_count, state_seen, users_revision;
    Phase phase;
    // shown: the page is open (its going saves the choices); chosen: the answers saved when it opened.
    bool browse, browse_only, shown;
    bool chosen[kGuildOnboardingOptionMaximum];
};
Page* page;
// The servers whose page was their last view.
u64 remembered[8];
// Browse Channels' introduction was closed in this session.
bool nux_closed;

u32 add_media(const UiSettingsAvatarMedia& media) noexcept {
    auto& p = *page;
    if (!media.key.id || p.media_count == sizeof(p.media) / sizeof(p.media[0]))
        return 0;
    p.media[p.media_count] = media;
    return ++p.media_count;
}
void picture(UiStoreBuilder& b, u32 slot, float size, u16 radius) noexcept {
    auto& n = b.open(UiStoreKind::preview);
    n.variant = static_cast<u8>(UiStoreAspect::fixed);
    n.rw = n.rh = size;
    n.aux = radius;
    n.value = slot;
    b.close();
}
UiStoreNode& row(UiStoreBuilder& b, float gap) noexcept {
    auto& n = b.open(UiStoreKind::row);
    n.gap = gap;
    n.flags = ui_store_center;
    return n;
}
UiStoreNode& icon(UiStoreBuilder& b, UiIcon glyph, float size, UiStoreInk ink) noexcept {
    auto& n = b.leaf(UiStoreKind::icon);
    n.rw = n.rh = size;
    n.icon = static_cast<u8>(glyph);
    n.ink = static_cast<u8>(ink);
    return n;
}
// A badge over a card's top end corner (the official client's: 10 DIPs past it).
inline UiStoreNode& corner(UiStoreNode& n, float overhang) noexcept {
    n.anchor = static_cast<u8>(UiStoreAnchor::top_right);
    n.pad_x = n.pad_y = overhang;
    n.flags |= ui_store_foreground;
    return n;
}
void new_badge(UiStoreBuilder& b) noexcept {
    corner(b.chip("NEW", UiStoreTone::brand), 10);
}
// The window's width in DIPs: the official client lays this page out by it.
inline float window_width() noexcept {
    const auto& r = g_app.renderer;
    return r.dpi ? r.width * 96.f / r.dpi : 0;
}

// The server's categories and channels as the channel list orders them: channels without a category first, then
// each category before its channels, text before voice, then by position. A category without channels is left out.
// Returns how many of the rows are channels (not categories).
u32 list_channels() noexcept {
    auto& p = *page;
    const auto& s = state();
    const ChannelRecord* rows[kBrowseRows];
    u64 keys[kBrowseRows];
    u32 count = 0;
    for (auto* c = chat_state_store_first_guild_channel(s, p.guild_id); c && count < kBrowseRows;
         c = chat_state_store_next_guild_channel(s, *c)) {
        if (c->guild_id != p.guild_id || channel_is_thread(c->type) || !chat_state_store_channel_visible(s, *c))
            continue;
        const auto* parent = c->type == 4 ? c : chat_state_store_find_channel(s, c->parent_id);
        const u64 key = (parent ? static_cast<u64>(parent->position + 1) << 44 : 0) |
                        static_cast<u64>(c->type != 4) << 43 | static_cast<u64>(c->type == 2 || c->type == 13) << 42 |
                        static_cast<u64>(c->position & 0x3FF) << 32 | static_cast<u32>(c->id);
        u32 at = count++;
        for (; at && keys[at - 1] > key; --at) {
            keys[at] = keys[at - 1];
            rows[at] = rows[at - 1];
        }
        keys[at] = key;
        rows[at] = c;
    }
    p.row_count = 0;
    u32 channels = 0;
    for (u32 i = 0; i < count; ++i)
        if (rows[i]->type != 4 || (i + 1 < count && rows[i + 1]->parent_id == rows[i]->id)) {
            p.rows[p.row_count++] = rows[i]->id;
            channels += rows[i]->type != 4;
        }
    return channels;
}

// What the answers chosen during this visit gave, as the official client words it: plain pieces in the muted ink
// and the names in the strong one, wrapped as a flow 4 DIPs apart ("You received the @a and @b roles.").
struct Received {
    u64 ids[64];
    Utf8View names[2];
    u32 count;
};
void receive(Received& r, u64 id, Utf8View name) noexcept {
    for (u32 i = 0; i < minimum(r.count, 64u); ++i)
        if (r.ids[i] == id)
            return;
    if (r.count < 64)
        r.ids[r.count] = id;
    if (r.count < 2)
        r.names[r.count] = name;
    ++r.count;
}
void plain(UiStoreBuilder& b, Utf8View text) noexcept {
    b.text(text, UiStoreType::small, UiStoreInk::subtle);
}
void name(UiStoreBuilder& b, char sign, Utf8View text) noexcept {
    b.text(b.copy({&sign, 1}, text), UiStoreType::small, UiStoreInk::strong);
}
// "the #a channel", "the #a and #b channels" or "#a, #b, and 3 other channels", with the period when it ends the line.
void received(UiStoreBuilder& b, const Received& r, char sign, Utf8View noun, bool last) noexcept {
    const u32 n = r.count;
    if (n <= 2)
        plain(b, utf8_literal("the"));
    name(b, sign, r.names[0]);
    if (n >= 2) {
        plain(b, n == 2 ? utf8_literal("and") : utf8_literal(","));
        name(b, sign, r.names[1]);
    }
    char text[64];
    TextBank bank{text, sizeof(text), 0};
    if (n > 2) {
        bank.add(", and ");
        bank.number(n - 2);
        bank.add(" other ");
    }
    bank.add(noun);
    if (n >= 2 && n != 3)
        bank.add("s");
    if (last)
        bank.add(".");
    plain(b, b.copy(bank.since(0)));
}
void help(UiStoreBuilder& b, const GuildOnboardingPrompt& prompt) noexcept {
    const auto& p = *page;
    const auto& s = state();
    Received roles{}, channels{};
    for (u32 o = prompt.first_option; o < static_cast<u32>(prompt.first_option) + prompt.option_count; ++o) {
        const auto& option = p.onboarding.options[o];
        if (!option.selected || !option.picked)
            continue;
        for (u32 g = 0; g < option.role_count; ++g)
            if (const auto* role =
                    chat_state_store_find_role(s, p.guild_id, p.onboarding.grants[option.first_role + g]))
                receive(roles, role->id, chat_state_store_role_name(s, *role));
        for (u32 g = 0; g < option.channel_count; ++g)
            if (const auto* c = chat_state_store_find_channel(s, p.onboarding.grants[option.first_channel + g]);
                c && c->permissions_resolved && (c->effective_permissions & kPermissionViewChannel))
                receive(channels, c->id, chat_state_store_channel_name(s, *c));
    }
    if (!roles.count && !channels.count) {
        if (!prompt.single_select)
            b.text("Choose all that apply.", UiStoreType::small, UiStoreInk::subtle, ui_store_wrap);
        return;
    }
    b.open(UiStoreKind::flow).gap = 4;
    plain(b, utf8_literal("You received"));
    if (channels.count)
        received(b, channels, '#', utf8_literal("channel"), !roles.count);
    if (channels.count && roles.count)
        plain(b, utf8_literal(", and"));
    if (roles.count)
        received(b, roles, '@', utf8_literal("role"), true);
    b.close();
}

// An answer: a muted plate with its emoji and its title over its description, every answer of a question as high
// as the highest; chosen, the brand plate with a check badge on its corner; never seen, a NEW badge there.
void tile(UiStoreBuilder& b, u32 index, float height, bool can_be_new) noexcept {
    const auto& model = page->onboarding;
    const auto& option = model.options[index];
    const Utf8View description = guild_onboarding_text(model, option.description);
    auto& card = b.open(UiStoreKind::card);
    card.variant = static_cast<u8>(UiStoreCard::answer);
    card.flags = static_cast<u16>(ui_store_flex | (option.selected ? ui_store_selected : 0));
    card.action = action_option + index;
    card.pad_x = 16;
    card.pad_y = (height - (description.bytes ? 42 : 20)) * .5f;
    card.rh = height;
    row(b, 12);
    if (option.emoji_id)
        picture(b, add_media({media_asset_custom_emoji(option.emoji_id, option.emoji_animated)}), 22, 4);
    else if (option.emoji.bytes)
        b.text(guild_onboarding_text(model, option.emoji));
    auto& text = b.open(UiStoreKind::column);
    text.flags = ui_store_flex;
    text.gap = 4;
    b.text(guild_onboarding_text(model, option.title), UiStoreType::body, UiStoreInk::strong);
    if (description.bytes)
        b.text(description, UiStoreType::small, UiStoreInk::normal);
    b.close();
    b.close();
    if (option.selected) {
        auto& disc = corner(b.leaf(UiStoreKind::swatch), 10);
        disc.rw = disc.rh = 20;
        disc.variant = 2;
        // The badge names its tile: it grows in as the tile turns chosen.
        disc.value = action_option + index;
        corner(icon(b, UiIcon::check, 10, UiStoreInk::on_brand), 5).value = action_option + index;
    } else if (can_be_new && !option.seen) {
        new_badge(b);
    }
    b.close();
}

// A question's card: its title (and the red mark of a required one), its answers in a grid and what they gave. The
// title wraps to the card, as the official client's inline heading does; the mark stays beside its first line.
void question(UiStoreBuilder& b, u32 index, u32 columns) noexcept {
    const auto& model = page->onboarding;
    const auto& prompt = model.prompts[index];
    auto& card = b.open(UiStoreKind::card);
    card.pad_x = card.pad_y = card.gap = 16;
    if (!prompt.seen)
        new_badge(b);
    row(b, 4).flags = 0;
    b.text(guild_onboarding_text(model, prompt.title), UiStoreType::body_bold, UiStoreInk::strong, ui_store_wrap);
    if (prompt.required)
        b.text("*", UiStoreType::body_bold, UiStoreInk::critical);
    b.close();
    bool described = false;
    for (u32 o = 0; o < prompt.option_count; ++o)
        described |= model.options[prompt.first_option + o].description.bytes != 0;
    const float height = described ? 66 : 44;
    b.open(UiStoreKind::column).gap = 8;
    for (u32 o = 0; o < prompt.option_count; o += columns) {
        row(b, 8).flags = 0;
        for (u32 k = o; k < o + columns; ++k)
            if (k < prompt.option_count)
                tile(b, prompt.first_option + k, height, prompt.seen);
            else
                b.spacer(0, 0, true);
        b.close();
    }
    b.close();
    help(b, prompt);
    b.close();
}

// A heading counted as the official client counts it ("1 Customization Question", "2 New Options").
void heading(UiStoreBuilder& b, u32 count, Utf8View noun) noexcept {
    char text[48];
    TextBank bank{text, sizeof(text), 0};
    bank.add(noun);
    if (count != 1)
        bank.add("s");
    b.text(b.copy(bank.since(0)), UiStoreType::body_bold, UiStoreInk::strong, ui_store_wrap);
}

// My Profile: the account's avatar and name in this server, and its roles as pills in their colours, highest first.
// The roles follow the answers chosen on this page at once, as the official client shows them before the member
// update confirms them.
void profile(UiStoreBuilder& b) noexcept {
    const auto& p = *page;
    const auto& s = state();
    const auto& ui = *g_app.account_ui;
    const auto& model = p.onboarding;
    b.open(UiStoreKind::column).gap = 4;
    b.text("My Profile", UiStoreType::body_bold, UiStoreInk::strong);
    b.text("This is how you\xE2\x80\x99ll look to others!", UiStoreType::small, UiStoreInk::subtle);
    b.spacer(0, 12);
    b.open(UiStoreKind::card).pad_x = 16;
    b.node().pad_y = 16;
    const auto* self = user_store_find(ui.user_store, s.self_user_id);
    picture(b,
            add_media(application_listed_user_avatar_media(s.self_user_id,
                                                           self ? user_store_avatar_hash(*self) : Utf8View{}, 0)),
            80, 999);
    b.spacer(0, 16);
    const auto* member = guild_member_store_find(ui.guild_member_store, p.guild_id, s.self_user_id);
    Utf8View nick = member ? guild_member_store_nickname(*member) : Utf8View{};
    if (!nick.bytes && self)
        nick = user_store_name(ui.user_store, *self);
    b.text(nick.bytes ? b.copy(nick) : utf8_literal("You"), UiStoreType::heading, UiStoreInk::strong);
    u64 ids[48];
    u32 held = minimum(chat_state_store_self_role_ids(s, p.guild_id, {ids, 16}), 16u);
    const auto grants = [&](const GuildOnboardingOption& o, u64 role) noexcept {
        for (u32 g = 0; g < o.role_count; ++g)
            if (model.grants[o.first_role + g] == role)
                return true;
        return false;
    };
    for (u32 i = 0; i < model.option_count; ++i) {
        const auto& option = model.options[i];
        for (u32 g = 0; g < option.role_count; ++g) {
            const u64 role = model.grants[option.first_role + g];
            u32 at = 0;
            while (at < held && ids[at] != role)
                ++at;
            if (option.selected && option.picked && at == held && held < 48) {
                ids[held++] = role;
            } else if (!option.selected && p.chosen[i] && at < held) {
                bool kept = false;
                for (u32 o = 0; o < model.option_count && !kept; ++o)
                    kept = model.options[o].selected && grants(model.options[o], role);
                if (!kept)
                    ids[at] = ids[--held];
            }
        }
    }
    const RoleRecord* roles[48];
    u32 count = 0;
    for (u32 i = 0; i < held; ++i) {
        const auto* role = chat_state_store_find_role(s, p.guild_id, ids[i]);
        if (!role)
            continue;
        u32 at = count++;
        for (; at && roles[at - 1]->position < role->position; --at)
            roles[at] = roles[at - 1];
        roles[at] = role;
    }
    if (count) {
        b.spacer(0, 16);
        b.leaf(UiStoreKind::rule);
        b.spacer(0, 16);
        b.text("Roles", UiStoreType::small_bold, UiStoreInk::subtle);
        b.spacer(0, 8);
        b.open(UiStoreKind::flow).gap = 4;
        for (u32 i = 0; i < count; ++i) {
            auto& pill = b.open(UiStoreKind::card);
            pill.variant = static_cast<u8>(UiStoreCard::pill);
            pill.pad_x = 8;
            pill.pad_y = 4;
            row(b, 4);
            // A role without a colour shows the muted grey dot.
            auto& dot = b.leaf(UiStoreKind::swatch);
            dot.rw = dot.rh = 8;
            dot.variant = roles[i]->color ? 0 : 6;
            dot.value = roles[i]->color;
            b.text(b.copy(chat_state_store_role_name(s, *roles[i])), UiStoreType::small, UiStoreInk::strong);
            b.close();
            b.close();
        }
        b.close();
    }
    b.close();
    b.close();
}

// Without questions to show (or when they could not be read): the official client's empty page.
void empty(UiStoreBuilder& b) noexcept {
    auto& column = b.open(UiStoreKind::column);
    column.gap = 4;
    column.pad_y = 48;
    auto& disc = b.leaf(UiStoreKind::disc);
    disc.icon = static_cast<u8>(UiIcon::shield_check);
    disc.flags = ui_store_center;
    b.spacer(0, 12);
    b.text("There are no customization options yet.", UiStoreType::body_bold, UiStoreInk::strong,
           ui_store_center | ui_store_wrap);
    auto& line = row(b, 4);
    line.flags = ui_store_center | ui_store_justify;
    b.text("You can add and remove channels from", UiStoreType::label, UiStoreInk::normal, ui_store_wrap);
    b.button("Browse Channels", UiStoreButton::link, action_browse);
    b.text(".", UiStoreType::label, UiStoreInk::normal);
    b.close();
    b.close();
}

void customize(UiStoreBuilder& b) noexcept {
    auto& p = *page;
    if (p.phase == Phase::loading) {
        b.leaf(UiStoreKind::spinner);
        return;
    }
    const auto& model = p.onboarding;
    // The official client's order: new questions and those with new answers under "New Options", then the questions
    // asked after joining, then those of onboarding.
    u16 fresh[kGuildOnboardingPromptMaximum], others[kGuildOnboardingPromptMaximum];
    u32 fresh_count = 0, other_count = 0, unseen = 0;
    // One pass fills both lists: each keeps the order of the prompts, so a second walk would add nothing.
    for (u32 i = 0; i < model.prompt_count; ++i) {
        const auto& prompt = model.prompts[i];
        if (prompt.disabled || !prompt.option_count)
            continue;
        u32 answers = 0;
        for (u32 o = 0; o < prompt.option_count; ++o)
            answers += !model.options[prompt.first_option + o].seen;
        if (!prompt.seen || answers) {
            fresh[fresh_count++] = static_cast<u16>(i);
            unseen += prompt.seen ? answers : 1;
        } else {
            // Questions asked after joining lead.
            u32 at = other_count++;
            for (; at && !prompt.in_onboarding && model.prompts[others[at - 1]].in_onboarding; --at)
                others[at] = others[at - 1];
            others[at] = static_cast<u16>(i);
        }
    }
    if (p.phase != Phase::ready || model.truncated || !(fresh_count + other_count)) {
        empty(b);
        return;
    }
    // Two answers to a line, three from a 1672 DIP window, four from 2000; My Profile beside from 1344.
    const float window = window_width();
    const u32 columns = window >= 2000 ? 4 : window >= 1672 ? 3 : 2;
    const bool side = window >= 1344;
    if (side) {
        auto& split = b.open(UiStoreKind::split);
        split.gap = 32;
        split.rw = 256;
    }
    b.open(UiStoreKind::column).gap = 16;
    if (fresh_count) {
        heading(b, unseen, utf8_literal("New Option"));
        for (u32 i = 0; i < fresh_count; ++i)
            question(b, fresh[i], columns);
        b.spacer(0, 16);
        b.leaf(UiStoreKind::rule);
        b.spacer(0, 16);
    }
    if (other_count) {
        b.open(UiStoreKind::column);
        heading(b, other_count, utf8_literal("Customization Question"));
        b.text("Answer questions to get access to more channels and roles.", UiStoreType::small, UiStoreInk::subtle,
               ui_store_wrap);
        b.close();
        for (u32 i = 0; i < other_count; ++i)
            question(b, others[i], columns);
    }
    b.close();
    if (side) {
        profile(b);
        b.close();
    }
}

// "Active 5 minutes ago": the time of a channel's last message (or its creation), as the official client words it.
Utf8View active(UiStoreBuilder& b, u64 id) noexcept {
    const u64 then = (id >> 22) + 1420070400000ull, now = clock_unix_milliseconds();
    const u64 seconds = now > then ? (now - then) / 1000 : 0;
    char text[48];
    TextBank bank{text, sizeof(text), 0};
    bank.add("Active ");
    // moment's fromNow: a counted unit (unit 0: the words alone) below each bound, in seconds.
    struct Step {
        u64 below, unit;
        Utf8View words;
    };
    static constexpr Step kSteps[]{
        {45, 0, utf8_literal("a few seconds")},  {90, 0, utf8_literal("a minute")},
        {45 * 60, 60, utf8_literal(" minutes")}, {90 * 60, 0, utf8_literal("an hour")},
        {22 * 3600, 3600, utf8_literal(" hours")}, {36 * 3600, 0, utf8_literal("a day")},
        {26 * 86400, 86400, utf8_literal(" days")}, {45 * 86400, 0, utf8_literal("a month")},
        {320 * 86400, 2628000, utf8_literal(" months")}, {548 * 86400, 0, utf8_literal("a year")},
        {~0ull, 31536000, utf8_literal(" years")}};
    for (const auto& step : kSteps)
        if (seconds < step.below) {
            if (step.unit)
                bank.number((seconds + step.unit / 2) / step.unit);
            bank.add(step.words);
            break;
        }
    bank.add(" ago");
    return b.copy(bank.since(0));
}

UiIcon channel_icon(u8 type) noexcept {
    switch (type) {
    case 2: return UiIcon::speaker;
    case 5: return UiIcon::megaphone;
    case 13: return UiIcon::podcast;
    case 15: return UiIcon::message_text;
    case 16: return UiIcon::image;
    default: return UiIcon::channel;
    }
}

// Browse Channels: the introduction until closed, Create for a member who manages channels, then each category's name
// with its Follow Category box over its channels' rows. A row follows or unfollows its channel; a followed category
// follows its channels, which cannot change alone. Hovered or focused, a row offers View (Join for voice, Open for a
// forum).
void browse(UiStoreBuilder& b) noexcept {
    const auto& p = *page;
    const auto& s = state();
    b.open(UiStoreKind::column).gap = 8;
    if (!nux_closed) {
        auto& nux = b.open(UiStoreKind::card);
        nux.pad_x = nux.pad_y = 16;
        row(b, 16).flags = 0;
        auto& text = b.open(UiStoreKind::column);
        text.flags = ui_store_flex;
        text.gap = 8;
        b.text("Choose Your Channels", UiStoreType::heading, UiStoreInk::strong, ui_store_wrap);
        b.text("Customize what\xE2\x80\x99s on your channel list, so you see just the stuff you care about.",
               UiStoreType::body, UiStoreInk::normal, ui_store_wrap);
        b.open(UiStoreKind::flow).gap = 4;
        b.text("Tip:", UiStoreType::body_bold, UiStoreInk::normal);
        b.text("Channels with mentions will always appear on your channel list, even if you hide them.",
               UiStoreType::body, UiStoreInk::normal, ui_store_wrap);
        b.close();
        b.close();
        b.icon_button(UiIcon::close, "Close", action_dismiss);
        b.close();
        b.close();
    }
    const auto access = guild_settings_access(s, p.guild_id);
    if (access.resolved && permission_has(access.permissions, kPermissionManageChannels)) {
        b.open(UiStoreKind::row);
        b.spacer(0, 0, true);
        b.button("Create", UiStoreButton::secondary, action_create, UiIcon::plus);
        b.close();
    }
    bool group = false;
    for (u32 i = 0; i < p.row_count; ++i) {
        const auto* c = chat_state_store_find_channel(s, p.rows[i]);
        if (!c)
            continue;
        if (c->type == 4) {
            if (group)
                b.close();
            group = false;
            auto& header = row(b, 12);
            header.pad_y = 8;
            b.text(b.copy(chat_state_store_channel_name(s, *c)), UiStoreType::body_bold, UiStoreInk::strong).flags =
                ui_store_flex;
            auto& box = b.leaf(UiStoreKind::check);
            box.text = utf8_literal("Follow Category");
            box.action = action_channel + i;
            box.value = c->opted_in;
            b.close();
            continue;
        }
        if (!group) {
            auto& plate = b.open(UiStoreKind::card);
            plate.flags = ui_store_clip;
            group = true;
        }
        const auto* parent = chat_state_store_find_channel(s, c->parent_id);
        const bool locked = parent && parent->opted_in;
        const bool last = i + 1 == p.row_count ||
                          (chat_state_store_find_channel(s, p.rows[i + 1]) &&
                           chat_state_store_find_channel(s, p.rows[i + 1])->type == 4);
        auto& line = b.open(UiStoreKind::card);
        line.variant = static_cast<u8>(UiStoreCard::row);
        line.pad_x = 12;
        line.pad_y = 6;
        line.flags = last ? 0 : ui_store_hairline;
        line.action = action_channel + i;
        if (locked) {
            line.flags |= ui_store_disabled;
            line.text2 = utf8_literal("You must unfollow this category to add or remove specific channels.");
        }
        row(b, 8);
        icon(b, channel_icon(c->type), 20, UiStoreInk::subtle);
        auto& info = b.open(UiStoreKind::column);
        info.flags = ui_store_flex;
        info.gap = 2;
        b.text(b.copy(chat_state_store_channel_name(s, *c)), UiStoreType::body,
               locked ? UiStoreInk::subtle : UiStoreInk::strong);
        const bool voice = c->type == 2 || c->type == 13;
        if (!voice)
            b.text(active(b, c->last_message_id ? c->last_message_id : c->id), UiStoreType::small,
                   UiStoreInk::subtle);
        b.close();
        auto& view = b.button(voice ? utf8_literal("Join") : c->type == 15 || c->type == 16 ? utf8_literal("Open")
                                                                                            : utf8_literal("View"),
                              UiStoreButton::secondary, action_view + i);
        view.aux = static_cast<u16>(action_channel + i);
        auto& box = b.leaf(UiStoreKind::check);
        box.value = c->opted_in || locked;
        if (locked)
            box.flags = ui_store_disabled;
        b.close();
        b.close();
    }
    if (group)
        b.close();
    b.close();
}

// The official client lays the page out by the window's width (window_width), not by the column's.
void build(UiStoreBuilder& b, float) noexcept {
    if (!page || !g_app.account_ui)
        return;
    auto& p = *page;
    p.media_count = 0;
    p.state_seen = page_state();
    p.users_revision = g_app.account_ui->user_store.revision;
    const u32 channels = list_channels();
    b.node().gap = 32;
    if (!p.browse_only) {
        // The tabs over a line; Browse Channels counts the channels it lists.
        b.open(UiStoreKind::column);
        row(b, 24).flags = 0;
        static constexpr Utf8View kTabs[]{utf8_literal("Customize"), utf8_literal("Browse Channels")};
        for (u32 i = 0; i < 2; ++i) {
            auto& tab = b.leaf(UiStoreKind::tab);
            tab.text = kTabs[i];
            tab.action = action_customize + i;
            tab.group = 1;
            tab.variant = 2;
            tab.flags = p.browse == (i != 0) ? ui_store_selected : 0;
            if (i)
                tab.text2 = b.number(channels);
        }
        b.close();
        b.leaf(UiStoreKind::rule);
        b.close();
    }
    if (p.browse)
        browse(b);
    else
        customize(b);
}

inline void request() noexcept {
    page->phase = Phase::loading;
    enqueue(GuildCommunityOperation::onboarding_get, page->guild_id);
}

// Queues the PUT with every prompt and answer seen now: the official client sends it a second after the last change,
// when Customize goes (another tab or another page) and, through its answer, no longer shows NEW badges.
void save() noexcept {
    auto& p = *page;
    p.save_at = 0;
    if (p.phase != Phase::ready || p.onboarding.truncated || !ensure_background())
        return;
    auto& b = *background;
    const u32 bytes = guild_onboarding_write_responses(p.onboarding, clock_unix_milliseconds(),
                                                       {b.update_body, sizeof(b.update_body)}, true);
    if (!bytes)
        return;
    b.update_bytes = bytes;
    b.update_guild = p.guild_id;
    enqueue(GuildCommunityOperation::onboarding_update, p.guild_id);
    for (u32 i = 0; i < p.onboarding.prompt_count; ++i)
        p.onboarding.prompts[i].seen = true;
    for (u32 i = 0; i < p.onboarding.option_count; ++i)
        p.onboarding.options[i].seen = true;
}

// The official client keeps the only answer of a required question.
void pick(u32 option) noexcept {
    auto& p = *page;
    for (u32 i = 0; i < p.onboarding.prompt_count; ++i) {
        const auto& prompt = p.onboarding.prompts[i];
        const u32 first = prompt.first_option;
        if (option < first || option >= first + prompt.option_count)
            continue;
        u32 answers = 0;
        for (u32 o = first; o < first + prompt.option_count; ++o)
            answers += p.onboarding.options[o].selected;
        if (prompt.required && answers == 1 && p.onboarding.options[option].selected)
            return;
        guild_onboarding_toggle(&p.onboarding, i, option - first);
        p.save_at = deadline_after(clock_monotonic_milliseconds(), kSaveDelayMs);
        return;
    }
}

// A row's View: a voice channel joins, a forum opens its posts, any other channel shows.
void view(const ChannelRecord& c) noexcept {
    if (c.type == 2 || c.type == 13)
        application_voice_join(c.guild_id, c.id);
    else if (c.type == 15 || c.type == 16)
        application_thread_browser_open(c.id);
    else
        application_navigate_known_message(c.id, 0);
}

// Following a channel of a collapsed category opens the category, as the official client does.
void follow(const ChannelRecord& c) noexcept {
    auto& s = state();
    const bool on = !c.opted_in;
    if (on && c.type != 4)
        if (const auto* parent = chat_state_store_find_channel(s, c.parent_id); parent && parent->collapsed) {
            auto& category = s.channels[parent - s.channels];
            category.collapsed = false;
            saturating_increment(category.revision);
            saturating_increment(s.channel_mutations);
            synchronize_navigation_view();
        }
    application_notification_controls_follow_channel(c.guild_id, c.id, on);
}

void activate(u32 action, const UiRect&) noexcept {
    if (!page)
        return;
    auto& p = *page;
    const auto* c = action >= action_channel && (action & 0xFFF) < p.row_count
                        ? chat_state_store_find_channel(state(), p.rows[action & 0xFFF])
                        : nullptr;
    if (action == action_customize || action == action_browse) {
        if (action == action_browse && !p.browse)
            save();
        p.browse = action == action_browse;
    } else if (action == action_dismiss) {
        nux_closed = true;
    } else if (action == action_create) {
        application_guild_settings_create_channel(p.guild_id, 0, 0);
    } else if (c && action >= action_view) {
        view(*c);
    } else if (c) {
        follow(*c);
    } else if (action >= action_option && action - action_option < p.onboarding.option_count &&
               p.phase == Phase::ready && !p.onboarding.truncated) {
        pick(action - action_option);
    }
    application_content_page_invalidate();
}

const UiSettingsAvatarMedia* media(u32 slot) noexcept {
    return page && slot && slot <= page->media_count ? &page->media[slot - 1] : nullptr;
}

// Roles (GUILD_MEMBER_UPDATE), follows, channels and names change what the page shows. Another account or server
// closes it.
bool stale() noexcept {
    auto& p = *page;
    const auto* g = guild(p.guild_id);
    if (!g || g->unavailable || !application_account_available() ||
        p.account_scope != g_app.plugin_account_scope_serial || p.profile_id != g_app.account.status.profile_id ||
        state().home_selected || state().selected_guild_id != p.guild_id) {
        application_content_page_close();
        return false;
    }
    return page_state() != p.state_seen || g_app.account_ui->user_store.revision != p.users_revision;
}

// The official client's page has no maximum width and 32 DIPs of padding.
constexpr ContentPageOwner kRolesPage{kNavigationChannelsRolesRowId, build, activate, media, stale, 1e9f, 32};
} // namespace

namespace community_detail {
u64 channels_roles_save_at() noexcept {
    return page ? page->save_at : 0;
}
// A due save, and the save when the page goes while Customize shows (the official client's unmount).
void channels_roles_tick(u64 now) noexcept {
    if (!page)
        return;
    auto& p = *page;
    if (p.shown && application_content_page() != &kRolesPage) {
        p.shown = false;
        if (!p.browse || p.save_at)
            save();
    } else if (p.save_at && now >= p.save_at) {
        save();
    }
}
void channels_roles_response(u64 guild_id, bool ok, Utf8View body) noexcept {
    if (!page || page->guild_id != guild_id || page->phase != Phase::loading)
        return;
    auto& p = *page;
    p.phase = ok && guild_onboarding_decode(body, json_scratch(), &p.onboarding) ? Phase::ready : Phase::failed;
    for (u32 i = 0; i < p.onboarding.option_count; ++i)
        p.chosen[i] = p.onboarding.options[i].selected;
    application_content_page_invalidate();
}
void channels_roles_release() noexcept {
    if (application_content_page() == &kRolesPage)
        application_content_page_close();
    if (page)
        owner_release(page);
    reset_value(&remembered);
    nux_closed = false;
}
} // namespace community_detail

bool application_guild_channels_roles_open(u64 guild_id) noexcept {
    const auto* g = guild(guild_id);
    if (!g || !application_account_available() || !g_app.renderer_ready || (!page && !(page = owner_allocate<Page>())))
        return false;
    if (!application_guild_channels_roles_remembered(guild_id)) {
        static u32 next;
        remembered[next++ % 8] = guild_id;
    }
    application_guild_hub_forget(guild_id);
    // From the menu of another server: that server shows, and with it its remembered page.
    if (state().home_selected || state().selected_guild_id != guild_id)
        return select_content_guild(guild_id);
    auto& p = *page;
    if (application_content_page() == &kRolesPage && p.guild_id == guild_id)
        return true;
    if (p.save_at)
        save();
    p.guild_id = guild_id;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    p.browse = p.browse_only = !(g->upload_features & guild_record_onboarding_prompts);
    p.phase = Phase::ready;
    p.shown = true;
    if (!p.browse_only)
        request();
    return application_content_page_open(&kRolesPage);
}

bool application_guild_channels_roles_remembered(u64 guild_id) noexcept {
    for (const u64 id : remembered)
        if (id && id == guild_id)
            return true;
    return false;
}

void application_guild_channels_roles_forget(u64 guild_id) noexcept {
    for (u64& id : remembered)
        if (id == guild_id)
            id = 0;
}
} // namespace maicord::app_detail
