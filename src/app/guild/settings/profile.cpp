#include "page_common.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "../../settings/settings.hpp"
#include "../../chat/game_lookup.hpp"
#include "../../account/account_ui_runtime.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/app/account/account_profile_edit.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "../../shell/state.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/ui/text/emoji_catalog.hpp"

namespace maicord::app_detail::server_settings {
namespace {

// The profile is read again only when older than this.
constexpr u64 kProfileFreshMs = 60'000;
// Any editor size: the default crop is the centred square at zoom 1.
constexpr float kCropEditorWidth = 432, kCropEditorHeight = 350;
// The official page's grids: traits three to a line, games four.
constexpr u16 kTraitColumns = 3, kGameColumns = 4;
// Search results shown as tiles, and the sizes the game pictures are asked at.
constexpr u32 kSearchTiles = 8;
constexpr u32 kCoverDips = 128, kGameIconDips = 24;
// The emoji picker: what the trait's label finds, then each category's first emoji.
constexpr u32 kEmojiMatches = 24, kEmojiPerCategory = 56;
constexpr char kIconChoices[] = "Change Server Icon\0Remove Icon";

// Kept across openings, like the official client's profile store.
struct ProfileCache {
    u64 guild_id;
    u64 fetched_at;
    bool ok;
    GuildAdminProfile profile;
} cache;

bool has_icon() noexcept {
    const auto& d = owner->profile_draft;
    return d.icon == GuildProfileImageEdit::replace ||
           (d.icon == GuildProfileImageEdit::keep && d.profile.icon_bytes);
}
bool name_valid() noexcept {
    const auto& p = owner->profile_draft.profile;
    return !p.name_known || guild_server_name_valid({p.name, p.name_bytes});
}
bool read_pending() noexcept {
    return busy() && owner->request.op == Op::profile_get;
}
bool can_edit() noexcept {
    return editable() && can_manage_guild();
}
// A standard emoji's glyph from the name a trait stores: the official client writes Discord's emoji name (such as
// "video_game") and reads it back by name. A stored glyph reads as itself. Empty for a server emoji or an unknown
// name.
Utf8View trait_glyph(const GuildAdminProfileTrait& t) noexcept {
    const Utf8View name{t.emoji_name, t.emoji_name_bytes};
    if (t.emoji_id || !name.bytes)
        return {};
    const auto glyph = emoji_shortcode_glyph(name);
    if (glyph.bytes)
        return glyph;
    return emoji_catalog_find_glyph(name) != ~0u ? name : Utf8View{};
}
// A cell of one of the page's grids: traits (fields) or games (tiles with a cover).
UiSettingsControlPresentation cell(u16 columns, bool cover) noexcept {
    return {UiSettingsVariant::standard, static_cast<u8>(cover ? ui_settings_presentation_avatar : 0), columns, 0, 0,
            ui_settings_more_cell};
}

// The picked icon and custom banner with the data URIs made from them.
void release_icon_pick() noexcept {
    release_picked(&owner->icon);
    release_uri(&owner->icon_uri, &owner->icon_uri_bytes);
    release_picked(&owner->custom_banner);
    release_uri(&owner->custom_banner_uri, &owner->custom_banner_uri_bytes);
}
void load() noexcept {
    auto& p = *owner;
    release_icon_pick();
    const auto* profile = server_profile();
    p.profile_loaded = profile != nullptr;
    p.profile_base = p.profile_draft = profile ? guild_server_profile_draft(*profile) : GuildServerProfileDraft{};
    const auto& d = p.profile_draft.profile;
    set_editor(editor_name, {d.name, d.name_bytes});
    set_editor(editor_description, {d.description, d.description_bytes});
    for (u32 slot = 0; slot < kGuildAdminProfileTraits; ++slot)
        set_editor(static_cast<Editor>(editor_trait + slot), {d.traits[slot].label, d.traits[slot].label_bytes});
}

// The tile of game `id`: its name and cover once known; checked when it is one of the profile's games. A tile always
// has a name: "Loading…" until the game's object arrives, "Unknown Game" when the server has none.
void push_game(u64 id, bool chosen, bool enabled, u32* covers) noexcept {
    auto& p = *owner;
    game_lookup_want(id);
    const auto* record = game_lookup_find(id);
    const bool known = record && record->state == GameState::known && record->game.name_bytes;
    const Utf8View name = known ? Utf8View{record->game.name, record->game.name_bytes} : Utf8View{};
    const u32 row = push_row(id,
                             known ? Text(name)
                             : record && record->state == GameState::unknown ? Text("Unknown Game")
                                                                              : Text("Loading\xE2\x80\xA6"),
                             {}, UiSettingsControlKind::action, enabled, chosen, {}, cell(kGameColumns, true));
    if (row == ~0u || !known || *covers >= sizeof(p.covers) / sizeof(p.covers[0]))
        return;
    auto& cover = p.covers[*covers];
    if (!game_cover_media(record->game, kCoverDips, g_app.renderer.dpi, &cover))
        return;
    ++*covers;
    p.avatar_media[row].key = cover.key;
    p.avatar_media[row].url = cover.url_view();
}

void project_traits(Bank& bank, bool edit) noexcept {
    auto& p = *owner;
    push_category(control_traits_category, "Traits",
                  "Add up to 5 traits to show off your server\xE2\x80\x99s interests and personality.");
    for (u32 slot = 0; slot < kGuildAdminProfileTraits; ++slot) {
        const auto& t = p.profile_draft.profile.traits[slot];
        const bool filled = t.label_bytes || t.emoji_name_bytes || t.emoji_id;
        // "Trait N" names the field for screen readers; the grid shows no label.
        const u32 start = bank.used;
        bank.add("Trait ");
        bank.number(slot + 1);
        const u32 row = push_editor(control_trait + slot, bank.since(start), {}, UiSettingsControlKind::edit, edit,
                                    static_cast<Editor>(editor_trait + slot), {}, filled ? "Clear" : nullptr);
        if (row == ~0u)
            continue;
        // The emoji button shows a standard emoji's glyph, or a server emoji's picture from the CDN.
        p.presentation[row] = cell(kTraitColumns, t.emoji_id != 0);
        p.controls[row].detail = trait_glyph(t);
        if (t.emoji_id)
            p.avatar_media[row].key = media_asset_custom_emoji(t.emoji_id, false);
    }
}

void project_games(bool edit) noexcept {
    auto& p = *owner;
    const auto& d = p.profile_draft.profile;
    const bool room = d.game_count < kGuildAdminProfileGameMaximum;
    push_editor(control_games_search, "Games", "What games does your server play?", UiSettingsControlKind::edit,
                edit && room, editor_game_search, "Search for a game...");
    u32 covers = 0;
    for (u32 i = 0; i < d.game_count; ++i)
        push_game(d.game_application_ids[i], true, edit, &covers);
    const auto typed = editor_text(editor_game_search);
    char query[kGuildAdminGameQueryCodepoints * 4];
    if (guild_admin_game_query(typed, {query, sizeof(query)})) {
        // A search lists its games, chosen ones checked; the others take a place while there is room.
        const auto results = game_lookup_results(typed);
        if (!results.count) {
            if (results.loading)
                push_loading("Searching\xE2\x80\xA6", control_games_suggested);
            else
                push_notice("No games found.");
            return;
        }
        for (u32 i = 0, shown = 0; i < results.count && shown < kSearchTiles; ++i) {
            const u64 id = results.ids[i];
            const bool chosen = guild_server_profile_has_game(p.profile_draft, id);
            if (chosen)
                continue;
            push_game(id, false, edit && room, &covers);
            ++shown;
        }
        return;
    }
    // "Your Members Play": the server's most played games not chosen yet, most played first.
    const GuildAdminGameActivity* order[kGuildAdminProfileGameMaximum];
    u32 count = 0;
    for (u32 i = 0; i < p.top_game_count; ++i)
        if (!guild_server_profile_has_game(p.profile_draft, p.top_games[i].application_id))
            order[count++] = &p.top_games[i];
    for (u32 i = 1; i < count; ++i)
        for (u32 j = i; j && order[j]->score > order[j - 1]->score; --j) {
            const auto* swap = order[j];
            order[j] = order[j - 1];
            order[j - 1] = swap;
        }
    if (!count)
        return;
    push_category(control_games_suggested, "Your Members Play");
    for (u32 i = 0; i < count; ++i)
        push_game(order[i]->application_id, false, edit && room, &covers);
}

void project(Bank& bank) noexcept {
    auto& p = *owner;
    if (!p.profile_loaded) {
        bool settled = false;
        server_profile(&settled);
        if (settled && !read_pending())
            push_error("Something went wrong", "We couldn't load the profile for this server.", "Retry");
        else
            push_loading("Loading the server profile…");
        return;
    }
    const bool edit = can_edit();
    auto& d = p.profile_draft;
    push_editor(control_name, "Name", {}, UiSettingsControlKind::edit, edit, editor_name, "Name");

    // Icon: Change Server Icon, and Remove Icon while there is one.
    Text icon_text = "We recommend an image of at least 512x512.";
    Utf8View icon_new{};
    if (d.icon == GuildProfileImageEdit::remove)
        icon_text = "Removed when you save.";
    else if (d.icon == GuildProfileImageEdit::replace) {
        const u32 start = bank.used;
        bank.add("New: ");
        bank.number(p.icon_info.width);
        bank.add("×");
        bank.number(p.icon_info.height);
        bank.add(". Save to apply.");
        icon_new = bank.since(start);
        icon_text = icon_new;
    }
    // Remove Icon is the official critical-secondary button (red text on the secondary plate).
    push_buttons(control_icon_buttons, "Icon", icon_text, edit, kIconChoices, has_icon() ? 2u : 1u,
                 {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});

    // Banner: "Server Icon Colour" and the presets as banner tiles, and for a Discoverable server the upload tile
    // (its custom banner, shown and selected while set; a preset removes it, as on the official page).
    auto& names = p.color_names;
    names[0] = utf8_literal("Server Icon Color");
    for (u32 i = 0; i < kGuildProfileBannerColorCount; ++i)
        names[i + 1] = kGuildProfileBannerNames[i];
    // The official page reads DISCOVERABLE from the guild record.
    const bool upload = (guild_features() & guild_feature_discoverable) != 0;
    const u32 tiles = kGuildProfileBannerColorCount + 1 + (upload ? 1u : 0u);
    names[tiles - 1] = upload ? utf8_literal("Upload") : names[tiles - 1];
    const u32 banner = guild_server_profile_banner_choice(d);
    const bool custom = upload && (d.custom_banner == GuildProfileImageEdit::replace || d.profile.custom_banner_bytes);
    const u32 row = push_choices(control_banner_colors, utf8_literal("Banner"), {}, UiSettingsControlKind::swatches,
                                 edit, {names, tiles},
                                 custom ? tiles - 1 : banner == ~0u ? kUiSettingsNoChoice : banner, {},
                                 {UiSettingsVariant::standard, 0, kUiSettingsNoDefault, 0, 0,
                                  static_cast<u8>(ui_settings_more_banner_tiles |
                                                  (upload ? ui_settings_more_upload_tile : 0))});
    // The stored custom banner covers the tile; a new one shows once saved (the page has no local image preview).
    reset_value(&p.banner_tile);
    if (row != ~0u && upload && d.custom_banner == GuildProfileImageEdit::keep && d.profile.custom_banner_bytes &&
        ui_guild_card_banner_media(p.guild_id, {d.profile.custom_banner, d.profile.custom_banner_bytes}, true,
                                   g_app.renderer.dpi, &p.banner_tile)) {
        p.avatar_media[row].key = p.banner_tile.key;
        p.avatar_media[row].url = {p.banner_tile.url, p.banner_tile.url_bytes};
    }

    project_traits(bank, edit);

    push_editor(control_description, "Description", "How did your server get started? Why should people join?",
                UiSettingsControlKind::multiline, edit, editor_description, "Tell the world a bit about this server.");

    project_games(edit);

    // Discoverable servers stay public: the switch is off and a note links to Access.
    const bool discoverable = d.profile.discoverable;
    push_toggle(control_private, "Private Profile",
                "When enabled, only server members can view profile content. Non-members won\xE2\x80\x99t be able to "
                "see this content unless they have an invite.",
                edit && !discoverable, guild_server_profile_private(d));
    if (discoverable)
        push_row(control_private_access,
                 "Discoverable servers cannot have their content hidden. You can change this setting in", {},
                 UiSettingsControlKind::footnote, page_allowed(Page::access), false,
                 page_allowed(Page::access) ? "Access" : nullptr);
}

// The emoji picker of trait `slot`: its label's matches, then the categories; "No Emoji" removes one.
void open_emoji_picker(u32 slot, const UiRect& anchor) noexcept {
    auto& p = *owner;
    const auto& t = p.profile_draft.profile.traits[slot];
    Bank bank{p.popup_text, sizeof(p.popup_text), 0};
    u32 count = 0;
    const auto add = [&](Utf8View glyph, Utf8View name, u32 flags) noexcept {
        if (count >= kPopupCapacity)
            return;
        const u32 start = bank.used;
        if (glyph.bytes) {
            bank.add(glyph);
            bank.add("  ");
        }
        bank.add(name);
        p.emoji_choices[count] = glyph;
        popup_row(count, bank.since(start), count, flags);
        ++count;
    };
    if (t.emoji_name_bytes || t.emoji_id)
        add({}, utf8_literal("No Emoji"), 0);
    char key[64];
    u32 key_bytes = 0;
    if (t.label_bytes && emoji_search_key({t.label, t.label_bytes}, {key, sizeof(key)}, &key_bytes) && key_bytes) {
        struct Found {
            decltype(add)* add;
            u32 shown;
        } found{&add, 0};
        emoji_catalog_search(
            {key, key_bytes},
            [](void* context, const EmojiSearchMatch& match) noexcept {
                auto& f = *static_cast<Found*>(context);
                if (f.shown < kEmojiMatches && match.glyph.bytes) {
                    (*f.add)(match.glyph, match.name, 0);
                    ++f.shown;
                }
            },
            &found);
    }
    const auto catalog = unicode_emoji_catalog();
    for (u32 category = 0; category < kEmojiCategoryCount; ++category) {
        const auto range = emoji_catalog_category_range(static_cast<EmojiCategory>(category));
        u32 shown = 0;
        for (u32 i = range.first; i < range.one_past_last && i < catalog.count && shown < kEmojiPerCategory; ++i) {
            const auto glyph = catalog[i].glyph();
            // Skin-tone variants (U+1F3FB-1F3FF) stay with their base emoji.
            bool tone = false;
            for (u32 b = 0; b + 3 < glyph.bytes && !tone; ++b)
                tone = static_cast<u8>(glyph.data[b]) == 0xF0 && static_cast<u8>(glyph.data[b + 1]) == 0x9F &&
                       static_cast<u8>(glyph.data[b + 2]) == 0x8F && static_cast<u8>(glyph.data[b + 3]) >= 0xBB;
            if (tone)
                continue;
            add(glyph, catalog[i].name(), shown || !count ? 0u : static_cast<u32>(popup_item_separator_before));
            ++shown;
        }
    }
    p.emoji_choice_count = count;
    if (!count)
        return;
    p.emoji_slot = slot;
    open_popup(Menu::trait_emoji, p.guild_id, count, anchor, 280);
}

void clear_trait(u32 slot) noexcept {
    guild_server_profile_clear_trait(&owner->profile_draft, slot);
    set_editor(static_cast<Editor>(editor_trait + slot), {});
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    if (!can_manage_guild() || !p.profile_loaded)
        return;
    const u64 id = activation.value.target.control_id;
    const u32 choice = activation.value.selected_choice;
    auto& d = p.profile_draft;
    if (id == control_private_access) {
        // The note's "Access" link opens the Access page, as the official note's link does.
        if (page_allowed(Page::access) && !guard_dirty())
            select_page(Page::access);
        return;
    }
    if (id == control_icon_buttons) {
        if (choice == 0) {
            start_pick(Pick::icon);
            return;
        }
        release_icon_pick();
        d.icon = p.profile_base.profile.icon_bytes ? GuildProfileImageEdit::remove : GuildProfileImageEdit::keep;
    } else if (id >= control_trait && id < control_trait + kGuildAdminProfileTraits) {
        // The field's own buttons: clear (choice 0) or its emoji (choice 1).
        const u32 slot = static_cast<u32>(id - control_trait);
        if (choice == 1) {
            open_emoji_picker(slot, activation.anchor);
            return;
        }
        clear_trait(slot);
    } else if (id == control_private && !d.profile.discoverable) {
        guild_server_profile_set_private(&d, p.profile_base.profile, activation.value.checked);
        animate_toggle(activation);
    } else if (id == control_banner_colors && (guild_features() & guild_feature_discoverable) &&
               choice == kGuildProfileBannerColorCount + 1) {
        // The upload tile opens the file dialog; the picked file becomes the custom banner (Pick::custom_banner).
        start_pick(Pick::custom_banner);
        return;
    } else if (id == control_banner_colors)
        guild_server_profile_set_banner_choice(&d, choice);
    else if (id > control_trait + kGuildAdminProfileTraits && activation.value.kind == UiSettingsControlKind::action) {
        // A game tile: its control id is the game's.
        if (!guild_server_profile_toggle_game(&d, id)) {
            show_failure(utf8_literal("Servers can list up to 20 games."));
            return;
        }
    } else
        return;
    repaint();
}

void choose(Menu menu, u32 action) noexcept {
    auto& p = *owner;
    if (menu != Menu::trait_emoji || !can_manage_guild() || action < kMenuChoiceBase)
        return;
    const u32 index = action - kMenuChoiceBase;
    if (index >= p.emoji_choice_count)
        return;
    // The trait keeps the emoji's name, as the official client sends it (emoji_name "video_game", not the glyph).
    const auto glyph = p.emoji_choices[index];
    char name[kGuildAdminProfileEmojiNameBytes];
    const u32 name_bytes = glyph.bytes ? emoji_shortcode_key(glyph, {name, sizeof(name)}) : 0;
    if (name_bytes)
        guild_server_profile_set_trait_emoji(&p.profile_draft, p.emoji_slot, {name, name_bytes});
    else if (!glyph.bytes) {
        auto& t = p.profile_draft.profile.traits[p.emoji_slot];
        t.emoji_id = 0;
        t.emoji_name_bytes = 0;
        t.emoji_animated = false;
    }
    repaint();
}

// The menu key on a trait opens its emoji picker.
void context(const SettingsLayerActivation& activation) noexcept {
    const u64 id = activation.value.target.control_id;
    if (can_manage_guild() && owner->profile_loaded && id >= control_trait && id < control_trait + kGuildAdminProfileTraits)
        open_emoji_picker(static_cast<u32>(id - control_trait), activation.anchor);
}

bool dirty() noexcept {
    return owner->profile_loaded && !guild_server_profile_equal(owner->profile_base, owner->profile_draft);
}

bool can_save() noexcept {
    const auto& d = owner->profile_draft.profile;
    return can_manage_guild() && dirty() && name_valid() &&
           guild_server_description_valid({d.description, d.description_bytes});
}

Op next_save() noexcept {
    return dirty() ? Op::profile_update : Op::none;
}

// The profile first, then (for Manage Server) the server's most played games.
bool ensure_data() noexcept {
    auto& p = *owner;
    if (!p.profile_requested && profile_fetch_due()) {
        p.profile_requested = true;
        queue(Op::profile_get);
        return true;
    }
    return p.profile_loaded && can_manage_guild() && request_read(p.top_games_requested, p.top_games_failed, Op::top_games);
}

void notice_action() noexcept {
    if (owner->profile_loaded || read_pending())
        return;
    // Retry forces a read.
    cache.fetched_at = 0;
    owner->profile_requested = false;
    if (!busy() && connected())
        ensure_data();
}

const char* invalid() noexcept {
    return name_valid() ? nullptr : "Server names need 2–100 characters.";
}

// The new icon's data URI, cropped as the official client's cropper does by
// default: the centred square, a PNG for stills and a GIF for animated GIFs.
bool prepare_icon() noexcept {
    auto& p = *owner;
    if (p.icon_uri)
        return true;
    if (!p.icon.data)
        return false;
    const auto state = image_crop_begin(ImageCropUse::avatar, p.icon_info.width, p.icon_info.height,
                                        kCropEditorWidth, kCropEditorHeight);
    auto* uri = static_cast<u8*>(
        commit_pages(kAccountProfileImageUriCapacity));
    if (!uri)
        return false;
    ProfileImageRequest request{};
    request.source = {p.icon.data, p.icon.bytes};
    request.crop = image_crop_output(state);
    ProfileImageInfo info{};
    request.info = &info;
    request.data_uri = {uri, kAccountProfileImageUriCapacity};
    if (!account_module_profile_image(g_app.account.instance, &request) || !request.data_uri_bytes) {
        pages_release(uri);
        return false;
    }
    p.icon_uri = uri;
    p.icon_uri_bytes = request.data_uri_bytes;
    return true;
}

// The preview card's content: the draft as the official card shows it.
void preview_content(UiGuildCardContent* c, char (*traits)[kUiGuildCardTraitBytes]) noexcept {
    auto& p = *owner;
    const auto& d = p.profile_draft;
    const auto& profile = d.profile;
    c->guild_id = p.guild_id;
    c->phase = UiGuildCardPhase::ready;
    c->pinned = true;
    c->name = {profile.name, profile.name_bytes};
    c->description = {profile.description, profile.description_bytes};
    for (u32 slot = 0; slot < kGuildAdminProfileTraits; ++slot) {
        const auto& t = profile.traits[slot];
        if (!t.label_bytes)
            continue;
        // A chip shows its emoji before its label: a standard emoji's glyph, or the server emoji's picture (the
        // editor previews its own server, so a stored server emoji is always one of the server's).
        Bank bank{traits[c->trait_count], kUiGuildCardTraitBytes, 0};
        bank.add({t.label, t.label_bytes});
        c->trait_emoji[c->trait_count] = trait_glyph(t);
        if (t.emoji_id)
            c->trait_emoji_media[c->trait_count] = media_asset_custom_emoji(t.emoji_id, false);
        c->traits[c->trait_count++] = bank.since(0);
    }
    c->online_count = profile.online_count;
    c->member_count = profile.member_count;
    u32 color = 0;
    if (profile.brand_color_bytes && color_hex_parse_rgb24({profile.brand_color, profile.brand_color_bytes}, &color)) {
        c->banner_color = color;
        c->has_banner_color = true;
    } else if (p.has_icon_color) {
        c->banner_color = p.icon_color;
        c->has_banner_color = true;
    }
    if (d.icon == GuildProfileImageEdit::keep && profile.icon_bytes)
        c->icon = application_guild_icon_media(p.guild_id).key;
    reset_value(&p.preview_banner);
    if (profile.custom_banner_bytes &&
        ui_guild_card_banner_media(p.guild_id, {profile.custom_banner, profile.custom_banner_bytes},
                                   profile.discoverable, g_app.renderer.dpi, &p.preview_banner))
        c->banner = p.preview_banner.key;
    c->game_count = profile.game_count;
    for (u32 i = 0; i < minimum<u32>(profile.game_count, kUiGuildCardGames); ++i) {
        reset_value(&p.preview_games[i]);
        const auto* record = game_lookup_find(profile.game_application_ids[i]);
        if (record && record->state == GameState::known &&
            game_icon_media(record->game, kGameIconDips, g_app.renderer.dpi, &p.preview_games[i]))
            c->games[i] = p.preview_games[i].key;
    }
}

} // namespace

const GuildAdminProfile* server_profile(bool* settled) noexcept {
    const bool current = owner && cache.guild_id == owner->guild_id && cache.fetched_at;
    if (settled)
        *settled = current;
    return current && cache.ok ? &cache.profile : nullptr;
}

bool profile_fetch_due() noexcept {
    return !owner || cache.guild_id != owner->guild_id || !cache.fetched_at ||
           deadline_due(clock_monotonic_milliseconds(), deadline_after(cache.fetched_at, kProfileFreshMs));
}

void server_profile_loaded(const GuildAdminProfile* profile) noexcept {
    // A failed read keeps the last good profile of this guild.
    const bool keep = !profile && cache.ok && cache.guild_id == owner->guild_id;
    cache.guild_id = owner->guild_id;
    cache.fetched_at = clock_monotonic_milliseconds();
    if (profile) {
        cache.profile = *profile;
        cache.ok = true;
    } else if (!keep)
        cache.ok = false;
    auto& p = *owner;
    // A clean or unloaded page takes the new read.
    if (p.page == Page::profile && (!p.profile_loaded || !dirty()))
        load();
}

// A new custom banner is the file itself as a data URI, as the official image input reads it (no crop).
bool prepare_custom_banner() noexcept {
    auto& p = *owner;
    if (p.custom_banner_uri)
        return true;
    const auto kind = p.custom_banner.info.source_kind;
    const u32 bytes = guild_image_data_uri_bytes(kind, p.custom_banner.bytes);
    auto* uri = bytes ? static_cast<u8*>(commit_pages(bytes)) : nullptr;
    if (!uri || guild_image_write_data_uri(kind, {p.custom_banner.data, p.custom_banner.bytes}, {uri, bytes}) != bytes) {
        if (uri)
            pages_release(uri);
        return false;
    }
    p.custom_banner_uri = uri;
    p.custom_banner_uri_bytes = bytes;
    return true;
}

bool profile_patch(GuildSettingsPatch* patch, GuildServerProfileWrite* write,
                   Slice<GuildServerProfileTraitWrite> traits) noexcept {
    auto& p = *owner;
    const auto& d = p.profile_draft;
    if (!p.profile_loaded || (d.icon == GuildProfileImageEdit::replace && !prepare_icon()) ||
        (d.custom_banner == GuildProfileImageEdit::replace && !prepare_custom_banner()))
        return false;
    if (!guild_server_profile_write(d, {reinterpret_cast<const char*>(p.icon_uri), p.icon_uri_bytes},
                                    {reinterpret_cast<const char*>(p.custom_banner_uri), p.custom_banner_uri_bytes},
                                    state(), p.guild_id, traits, write))
        return false;
    reset_value(patch);
    patch->fields = guild_settings_patch_profile;
    patch->profile = write;
    return true;
}

void profile_saved(bool ok, const Utf8View& body) noexcept {
    auto& p = *owner;
    if (!ok)
        return;
    // The decoder writes its output only on success.
    if (guild_admin_decode_profile(body, {p.scratch, kScratchUnits}, &cache.profile).ok()) {
        cache.guild_id = p.guild_id;
        cache.fetched_at = clock_monotonic_milliseconds();
        cache.ok = true;
        load();
        return;
    }
    // An unreadable reply still saved the draft.
    release_icon_pick();
    p.profile_draft.icon = GuildProfileImageEdit::keep;
    p.profile_draft.custom_banner = GuildProfileImageEdit::keep;
    p.profile_base = p.profile_draft;
}

void profile_edited(Editor editor, Utf8View text) noexcept {
    if (editor == editor_game_search)
        game_lookup_search(text);
    else if (editor >= editor_trait && editor <= editor_trait_last)
        guild_server_profile_set_trait_label(&owner->profile_draft, editor - editor_trait, text);
}

bool top_games_request(GuildAdminRequest* request) noexcept {
    request->operation = GuildAdminOperation::top_games;
    return can_manage_guild();
}

void top_games_loaded(bool ok, const Utf8View& body) noexcept {
    auto& p = *owner;
    const auto decoded =
        ok ? guild_admin_decode_top_games(body, {p.scratch, kScratchUnits}, {p.top_games, kGuildAdminProfileGameMaximum})
           : GuildAdminResult{0, 0, GuildAdminError::input};
    p.top_game_count = decoded.ok() ? decoded.count : 0;
    // The suggestions are optional: a failed read shows none and is not retried this visit.
    p.top_games_failed = !decoded.ok();
}

void profile_tick(u64) noexcept {
    auto& p = *owner;
    if (p.page != Page::profile)
        return;
    bool changed = false;
    // Game names, covers and search results arrive through the shared game lookup.
    if (const u32 revision = game_lookup_revision(); revision != p.lookup_revision) {
        p.lookup_revision = revision;
        changed = true;
    }
    // The first banner tile (and an uncoloured preview banner) takes the icon's colour once it is resident.
    const auto& profile = p.profile_draft.profile;
    if (p.profile_loaded && !p.has_icon_color && profile.icon_bytes) {
        const auto icon = application_guild_icon_media(p.guild_id);
        if (media_asset_key_valid(icon.key) && renderer_media_dominant_color(&g_app.renderer, icon.key, &p.icon_color)) {
            p.has_icon_color = true;
            changed = true;
        }
    }
    if (!busy() && connected())
        ensure_page_data();
    if (changed)
        repaint();
}

void profile_page_content(UiSettingsPageContent* content) noexcept {
    auto& p = *owner;
    content->description = utf8_literal("Customize how your server appears in invite links and, if enabled, in Server "
                                        "Discovery and Announcement Channel messages");
    content->sectioned = true;
    const bool preview = p.profile_loaded;
    // The form and its preview share the wide column (the official page sets them side by side, the form ~525 wide).
    content->wide = preview;
    content->aside_width = preview ? kUiGuildCardWidth : 0;
    content->aside_height = preview ? (p.preview_height > 0 ? p.preview_height : kUiGuildCardBanner * 3) : 0;
}

bool profile_place_preview() noexcept {
    auto& p = *owner;
    UiRect heading{}, card{}, clip{};
    if (p.page != Page::profile || !p.profile_loaded || !g_app.account_ui ||
        !ui_settings_scene_aside(p.scene, &heading, &card, &clip)) {
        profile_close_preview();
        return false;
    }
    UiGuildCardContent content{};
    char traits[kUiGuildCardTraits][kUiGuildCardTraitBytes];
    preview_content(&content, traits);
    if (!ui_guild_card_show(&p.preview, p.guild_id, content, card, clip, clock_monotonic_milliseconds(),
                            application_reduced_motion(), &g_app.account_ui->text_measure_cache)) {
        profile_close_preview();
        return false;
    }
    g_app.renderer.settings_guild_preview = &p.preview;
    const float height = ui_rect_height(p.preview.settled);
    if (height == p.preview_height)
        return false;
    p.preview_height = height;
    return true;
}

void profile_close_preview() noexcept {
    if (!owner)
        return;
    if (g_app.renderer.settings_guild_preview == &owner->preview)
        g_app.renderer.settings_guild_preview = nullptr;
    if (owner->preview.active)
        ui_guild_card_close(&owner->preview);
}

u32 profile_icon_color() noexcept {
    return owner && owner->has_icon_color ? owner->icon_color : 0;
}

const PageOps kProfilePage{load,        project,       activate, choose, dirty, can_save, next_save, nullptr,
                           ensure_data, notice_action, invalid,  context};

} // namespace maicord::app_detail::server_settings

namespace maicord::app_detail {

bool application_guild_settings_preview_media(u32 index, MediaAssetKey* key, Utf8View* url) noexcept {
    using namespace server_settings;
    const auto* card = g_app.renderer.settings_guild_preview;
    if (!owner || card != &owner->preview || !card->active)
        return false;
    // The shown game icons, the custom banner (by URL), then the traits' server emoji (by key).
    for (u32 i = 0; i < card->shown_games; ++i) {
        const auto& media = owner->preview_games[i];
        if (!media.url_bytes || index--)
            continue;
        *key = media.key;
        *url = media.url_view();
        return true;
    }
    if (owner->preview_banner.url_bytes && media_asset_key_valid(card->banner_key) && !index--) {
        *key = owner->preview_banner.key;
        *url = owner->preview_banner.url_view();
        return true;
    }
    for (u32 i = 0; i < card->trait_count; ++i) {
        if (!media_asset_key_valid(card->trait_emoji_keys[i]) || index--)
            continue;
        *key = card->trait_emoji_keys[i];
        *url = {};
        return true;
    }
    return false;
}

bool application_guild_settings_preview_key_current(const MediaAssetKey& key) noexcept {
    MediaAssetKey shown{};
    Utf8View url{};
    for (u32 i = 0; application_guild_settings_preview_media(i, &shown, &url); ++i)
        if (media_asset_key_equal(shown, key))
            return true;
    return false;
}

} // namespace maicord::app_detail
