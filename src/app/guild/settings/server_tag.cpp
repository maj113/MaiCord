#include "page_common.hpp"
#include "../../account/account_ui_runtime.hpp"
#include "maicord/model/guild/detail_store.hpp"
#include "maicord/model/user/profile.hpp"
#include "maicord/protocol/messages/application_commands.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/ui/widgets/color_picker.hpp"
#include "maicord/ui/widgets/badge.hpp"
#include "../../shell/state.hpp"

// Server Settings > Server Tag: the tag fields of PATCH /guilds/{g}/profile
// (tag, badge, badge_color_primary, badge_color_secondary; GUILD_TAGS and
// Manage Server) and PUT /users/@me/clan for adopting the tag, as the official
// page lays them out: Enable Server Tag, Choose Name, Choose Badge, Choose
// Color, a chat preview and the adoption card.
namespace maicord::app_detail::server_settings {
namespace {

// Sample chat lines of the preview, each a name and its message; the last one wears the tag. The names' role colours
// (kGuildRoleColors indexes) in the same order.
constexpr char kSampleLines[] = "Mira\0anyone down for a match?\0Juno\0count me in\0Ari\0check out my tag!\0";
constexpr u8 kSampleColors[]{6, 1, 4};
constexpr u32 kSampleCount = sizeof(kSampleColors);
static_assert(name_list_count(kSampleLines) == 2 * kSampleCount);
// What the preview shows for a tag not typed yet.
constexpr Utf8View kPlaceholderTag = utf8_literal("WUMP");

UiIcon badge_icon(u8 type) noexcept {
    switch (type) {
    case 0: return UiIcon::sword;
    case 1: return UiIcon::droplet;
    case 2: return UiIcon::skull;
    case 3: return UiIcon::toadstool;
    case 4: return UiIcon::moon;
    case 5: return UiIcon::lightning;
    case 6: return UiIcon::leaf;
    case 7: return UiIcon::heart;
    case 8: return UiIcon::flame;
    case 9: return UiIcon::compass;
    case 21: return UiIcon::rabbit;
    case 22: return UiIcon::dog;
    case 23: return UiIcon::frog;
    case 24: return UiIcon::goat;
    case 25: return UiIcon::cat;
    case 26: return UiIcon::gem;
    case 27: return UiIcon::crown;
    case 28: return UiIcon::trophy;
    case 29: return UiIcon::money_bag;
    case 30: return UiIcon::dollar;
    case 31: return UiIcon::clover;
    case 32: return UiIcon::blossom;
    case 33: return UiIcon::sprout;
    case 34: return UiIcon::maple_leaf;
    case 35: return UiIcon::wilted_flower;
    case 36: return UiIcon::butterfly;
    case 37: return UiIcon::snail;
    case 38: return UiIcon::caterpillar;
    case 39: return UiIcon::spider;
    default: return UiIcon::bee;
    }
}

// The badge in a pair of colours: a two-colour badge fills with the primary
// and outlines with the secondary; a one-colour badge outlines with a darker
// shade of its colour. The badge the server has saved, in its saved colours,
// is the server's own image of it instead (the one on a name tag chip when
// `chip`), until that picture is held the glyph shows; a tile with a corner
// mark keeps its glyph, which the picture would cover.
UiSettingsGlyphTile badge_tile(u8 type, u32 primary, u32 secondary, UiIcon mark = UiIcon::count,
                               bool chip = false) noexcept {
    const auto* badge = guild_tag_badge(type);
    const auto shade = [](u32 rgb) noexcept {
        const u32 r = (rgb >> 16 & 0xff) * 3 / 5, g = (rgb >> 8 & 0xff) * 3 / 5, b = (rgb & 0xff) * 3 / 5;
        return r << 16 | g << 8 | b;
    };
    const bool two = badge && badge->tints > 1;
    UiSettingsGlyphTile tile{two ? secondary : shade(primary), primary, badge_icon(type), mark};
    const auto& p = *owner;
    if (p.tag_images && mark == UiIcon::count && guild_tag_badge_saved(p.tag_base, type, primary, secondary)) {
        const auto& image = chip ? p.tag_chip_image : p.tag_tile_image;
        tile.media = {image.key, 0, {}, image.url_view()};
    }
    return tile;
}

u32 add_tile(const UiSettingsGlyphTile& tile) noexcept {
    auto& p = *owner;
    if (p.glyph_tile_count >= sizeof(p.glyph_tiles) / sizeof(p.glyph_tiles[0]))
        return ~0u;
    p.glyph_tiles[p.glyph_tile_count] = tile;
    return p.glyph_tile_count++;
}

// A draft's badge as the chip shows it.
UiSettingsGlyphTile draft_tile(const GuildTagDraft& d) noexcept {
    return badge_tile(d.badge, guild_tag_primary(d), guild_tag_secondary(d), UiIcon::count, true);
}

// Whether the saved badge's image is known for the draft's base: the profile read is the one the base came from (a
// save the reply of which did not decode leaves the base ahead of it) and names a badge hash. The tile and chip
// requests are the same CDN image at the size each is drawn at.
void load_saved_images(const GuildAdminProfile* profile) noexcept {
    auto& p = *owner;
    const u32 dpi = g_app.renderer.dpi;
    p.tag_images = profile && p.tag_loaded && profile->badge_hash_bytes &&
                   guild_tag_draft_equal(guild_tag_draft(*profile), p.tag_base) &&
                   ui_guild_tag_badge_media(p.guild_id, {profile->badge_hash, profile->badge_hash_bytes},
                                            static_cast<u32>(kUiSettingsGlyphImageDips), dpi, &p.tag_tile_image) &&
                   ui_guild_tag_badge_media(p.guild_id, {profile->badge_hash, profile->badge_hash_bytes},
                                            static_cast<u32>(kUiBadgeCompactIcon), dpi, &p.tag_chip_image);
}

bool eligible() noexcept {
    return (guild_features() & guild_feature_guild_tags) != 0;
}
bool read_pending() noexcept {
    return busy() && owner->request.op == Op::profile_get;
}
// The profile shows outside the server: public, or public with recruitment.
bool profile_visible(const GuildAdminProfile& profile) noexcept {
    return profile.visibility_known && (profile.visibility == 1 || profile.visibility == 3);
}
bool adopted() noexcept {
    const auto& p = *owner;
    if (p.tag_adopted)
        return true;
    const auto* self = g_app.account_ui ? user_appearance_find(g_app.account_ui->user_store.appearance, p.self_user_id)
                                        : nullptr;
    return self && (self->fields & appearance_guild_tag) && self->primary_enabled && self->primary_guild_id == p.guild_id;
}

// The colour picker (application_color_picker_open) on one of the badge's
// colours: slot 0 the primary, 1 the secondary. It previews into the draft
// while open; closing without Apply restores the colours it started from.
struct ColorPick {
    bool active, applied, primary_set, secondary_set;
    u32 primary, secondary;
} g_pick;

void set_color(u64 slot, u32 rgb) noexcept {
    auto& d = owner->tag_draft;
    (slot ? d.secondary : d.primary) = rgb;
    (slot ? d.secondary_set : d.primary_set) = true;
    repaint();
}
void color_preview(TextFormOwnerKey key, u32 rgb) noexcept {
    if (form_valid(key))
        set_color(key.item, rgb);
}
bool color_save(TextFormOwnerKey key, u32 rgb) noexcept {
    if (!form_valid(key))
        return false;
    set_color(key.item, rgb);
    g_pick.applied = true;
    return true;
}
void color_closed(TextFormOwnerKey key) noexcept {
    const auto pick = g_pick;
    reset_value(&g_pick);
    if (!pick.active || pick.applied || !form_valid(key))
        return;
    auto& d = owner->tag_draft;
    d.primary_set = pick.primary_set, d.secondary_set = pick.secondary_set;
    d.primary = pick.primary, d.secondary = pick.secondary;
    repaint();
}
void edit_color(u32 slot) noexcept {
    const auto& d = owner->tag_draft;
    g_pick = {true, false, d.primary_set, d.secondary_set, d.primary, d.secondary};
    // The presets' colours of the same kind, as quick picks.
    u32 suggested[kColorPickerSuggestedColors];
    for (u32 i = 0; i < kColorPickerSuggestedColors; ++i)
        suggested[i] = slot ? kGuildTagPalettes[i].secondary : kGuildTagPalettes[i].primary;
    ApplicationColorPickerRequest request{};
    request.title = slot ? utf8_literal("Secondary Color") : utf8_literal("Primary Color");
    request.label = request.title;
    request.owner = form_key(form_tag_color, slot);
    request.valid = form_valid;
    request.rgb = slot ? guild_tag_secondary(d) : guild_tag_primary(d);
    request.suggested = {suggested, kColorPickerSuggestedColors};
    request.preview = color_preview;
    request.save = color_save;
    request.closed = color_closed;
    if (!application_color_picker_open(request))
        reset_value(&g_pick);
}

void load() noexcept {
    auto& p = *owner;
    const auto* profile = server_profile();
    p.tag_loaded = profile != nullptr;
    p.tag_base = p.tag_draft = profile ? guild_tag_draft(*profile) : GuildTagDraft{};
    p.tag_custom_open = false;
    set_editor(editor_tag, {p.tag_draft.tag, p.tag_draft.tag_bytes});
}

bool dirty() noexcept {
    return owner->tag_loaded && !guild_tag_draft_equal(owner->tag_base, owner->tag_draft);
}

// A row of glyph tiles (one per choice) with its first tile.
u32 push_glyphs(u64 id, Text label, Text description, bool enabled, ConstSlice<Utf8View> choices, u32 selected,
                u32 first_tile) noexcept {
    return push_choices(id, label, description, UiSettingsControlKind::swatches, enabled, choices, selected, {},
                        {UiSettingsVariant::standard, 0, static_cast<u16>(first_tile), 0, 0, ui_settings_more_glyphs});
}

// A name in the preview: an initials avatar in its colour, the name and, with
// a tag, its chip led by the badge tile; the message under it.
u32 push_name(u64 id, Text name, Text message, Text tag, u32 tile, Text action = {},
              bool action_enabled = true) noexcept {
    const u32 row = push_row(id, name, message, UiSettingsControlKind::action, action_enabled, false, action,
                             {UiSettingsVariant::standard,
                              static_cast<u8>(ui_settings_presentation_avatar | ui_settings_presentation_accent),
                              static_cast<u16>(tile == ~0u ? 0 : tile), 0, 0, ui_settings_more_name_tag});
    if (row != ~0u && tile != ~0u)
        owner->controls[row].detail = text_view(tag);
    return row;
}

void project_badges(bool edit) noexcept {
    auto& p = *owner;
    auto& d = p.tag_draft;
    const u64 features = guild_features();
    u32 unlocked = 0, locked = 0, selected = kUiSettingsNoChoice;
    u32 first = p.glyph_tile_count;
    NameList name{kGuildTagBadgeNames};
    for (const auto& badge : kGuildTagBadges) {
        if (!guild_tag_badge_unlocked(badge, features)) {
            p.tag_locked[locked] = badge.type;
            p.tag_locked_names[locked++] = name.name();
        } else {
            if (badge.type == d.badge)
                selected = unlocked;
            p.tag_unlocked[unlocked] = badge.type;
            p.tag_unlocked_names[unlocked++] = name.name();
            // As the official picker: each badge in its own colours.
            add_tile(badge_tile(badge.type, badge.primary, badge.tints > 1 ? badge.secondary : badge.primary));
        }
        name.next();
    }
    push_glyphs(control_tag_badges, "Choose Badge", "Changing just the badge won't remove the Server Tag from members.",
                edit, {p.tag_unlocked_names, unlocked}, selected, first);
    if (!locked)
        return;
    push_action(control_tag_show_locked, "Badge packs", {},
                p.tag_show_locked ? "Hide locked badges" : "Show all badges", true,
                {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
    if (!p.tag_show_locked)
        return;
    first = p.glyph_tile_count;
    for (u32 i = 0; i < locked; ++i) {
        const auto* badge = guild_tag_badge(p.tag_locked[i]);
        add_tile(badge_tile(badge->type, badge->primary, badge->tints > 1 ? badge->secondary : badge->primary,
                            UiIcon::lock));
    }
    push_glyphs(control_tag_locked_badges, "Locked badges",
                "These come with badge packs (Pets, Flex, Plant and Creepy Crawlies), which a server unlocks with "
                "Boosts.",
                false, {p.tag_locked_names, locked}, kUiSettingsNoChoice, first);
}

void project_colors(bool edit) noexcept {
    auto& p = *owner;
    auto& d = p.tag_draft;
    const u32 first = p.glyph_tile_count;
    NameList name{kGuildTagPaletteNames};
    for (u32 i = 0; i < kGuildTagPaletteCount; ++i, name.next()) {
        add_tile(badge_tile(d.badge, kGuildTagPalettes[i].primary, kGuildTagPalettes[i].secondary));
        p.tag_palette_names[i] = name.name();
    }
    const auto* badge = guild_tag_badge(d.badge);
    const u32 own_primary = badge ? badge->primary : 0;
    const u32 own_secondary = badge && badge->tints > 1 ? badge->secondary : own_primary;
    add_tile(badge_tile(d.badge, own_primary, own_secondary, UiIcon::reset));
    add_tile(badge_tile(d.badge, guild_tag_primary(d), guild_tag_secondary(d), UiIcon::palette));
    p.tag_palette_names[kGuildTagPaletteDefault] = utf8_literal("Reset to default colors");
    p.tag_palette_names[kGuildTagPaletteCustom] = utf8_literal("Custom colors");
    push_glyphs(control_tag_colors, "Choose Color",
                "These preset color pairs set your badge's primary and secondary colors.", edit,
                {p.tag_palette_names, kGuildTagPaletteCount + 2}, guild_tag_palette_choice(d), first);
    if (!p.tag_custom_open && guild_tag_palette_choice(d) != kGuildTagPaletteCustom)
        return;
    // "Custom Colors": a swatch per colour the badge takes; each opens the colour picker.
    push_names(control_tag_custom, "Custom Colors", {}, UiSettingsControlKind::colors, edit, "Primary\0Accent",
               badge && badge->tints > 1 ? 2u : 1u);
}

void project(Bank& bank) noexcept {
    auto& p = *owner;
    p.glyph_tile_count = 0;
    if (!p.tag_loaded) {
        bool settled = false;
        server_profile(&settled);
        if (settled && !read_pending())
            push_error("Something went wrong", "We couldn't load the Server Tag of this server.", "Retry");
        else
            push_loading("Loading the Server Tag…");
        return;
    }
    const auto* profile = server_profile();
    load_saved_images(profile);
    const bool can = eligible(), edit = editable() && can_manage_guild() && can;
    auto& d = p.tag_draft;
    // As the official page: everything under the switch is inert while the tag is off.
    const bool fields = edit && d.enabled;
    push_row(control_tag_intro, {},
             "Create a tag that your server members can display next to their name! Anyone outside your server can "
             "view your Server Profile through the Server Tag.",
             UiSettingsControlKind::notice, true, false, page_allowed(Page::profile) ? "Server Profile" : nullptr);
    if (profile && !profile_visible(*profile))
        push_row(control_tag_private, {},
                 "Your Server Profile is private. Users will not be able to see it from a Server Tag.",
                 UiSettingsControlKind::notice, true, false, page_allowed(Page::profile) ? "Edit Setting" : nullptr);
    if (!can)
        push_row(control_tag_locked, "Unlock Server Tags",
                 "Server Tags are a Boost perk: this server needs the Guild Tags perk, which 3 Boosts unlock, before "
                 "it can have a tag.",
                 UiSettingsControlKind::notice, true, false, page_allowed(Page::boost_perks) ? "Boost Perks" : nullptr);
    else
        push_toggle(control_tag_enable, "Enable Server Tag", {}, edit, d.enabled);

    // The chat preview: two plain lines and one wearing the tag.
    push_category(control_tag_preview_category, "Preview");
    const Utf8View shown = d.tag_bytes ? bank.add({d.tag, d.tag_bytes}) : kPlaceholderTag;
    const u32 chip = add_tile(draft_tile(d));
    const char* line = kSampleLines;
    for (u32 i = 0; i < kSampleCount; ++i) {
        const char* message = line + c_string_length(line) + 1;
        push_name(control_tag_preview + i, line, message, shown, i + 1 == kSampleCount ? chip : ~0u);
        line = message + c_string_length(message) + 1;
    }

    push_category(control_tag_name_category, "Choose Name");
    push_editor(control_tag_text, {}, "You may use max 4 characters, alphabet and numbers.",
                UiSettingsControlKind::edit, fields, editor_tag, kPlaceholderTag);
    if (d.enabled && d.tag_bytes && !guild_tag_text_valid({d.tag, d.tag_bytes}))
        push_row(control_tag_text_error, "Server tags are 2–4 letters or numbers.", {}, UiSettingsControlKind::error,
                 true, false, {}, {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    if (d.enabled && profile && profile->tag_bytes && guild_tag_renamed(d, *profile))
        push_notice("Updating your Server Tag will require all of your members to manually reapply the tag to their "
                    "profile. We do this to prevent abuse.");
    project_badges(fields);
    project_colors(fields);

    // Adopting the saved tag as one's own (PUT /users/@me/clan).
    if (can && p.tag_base.enabled) {
        push_category(control_tag_adopt_category, "Your Server Tag");
        const bool mine = adopted();
        const u32 saved = add_tile(draft_tile(p.tag_base));
        const Utf8View self = user_label(p.self_user_id);
        push_name(control_tag_adopt, self.bytes ? Text(self) : Text("You"),
                  mine ? "You're currently using this Server Tag. This is how people will see you."
                       : "You don't have this Server Tag on your own profile yet. Would you like to adopt it?",
                  bank.add({p.tag_base.tag, p.tag_base.tag_bytes}), saved, "Adopt Tag", editable() && !mine);
    }
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const u64 id = activation.value.target.control_id;
    if (id == control_tag_intro || id == control_tag_private) {
        if (page_allowed(Page::profile))
            select_page(Page::profile);
        return;
    }
    if (id == control_tag_locked) {
        if (page_allowed(Page::boost_perks))
            select_page(Page::boost_perks);
        return;
    }
    if (id == control_tag_show_locked) {
        p.tag_show_locked = !p.tag_show_locked;
        repaint();
        return;
    }
    if (id == control_tag_adopt) {
        if ((activation.button || !activation.pointer) && editable() && !adopted())
            queue(Op::tag_adopt);
        repaint();
        return;
    }
    if (!p.tag_loaded || !can_manage_guild() || !eligible())
        return;
    auto& d = p.tag_draft;
    const u32 choice = activation.value.selected_choice;
    if (id == control_tag_enable) {
        d.enabled = activation.value.checked;
        animate_toggle(activation);
    } else if (!d.enabled)
        return;
    else if (id == control_tag_badges && choice < kGuildTagBadgeCount) {
        if (const auto* badge = guild_tag_badge(p.tag_unlocked[choice]);
            badge && guild_tag_badge_unlocked(*badge, guild_features()))
            d.badge = badge->type;
    } else if (id == control_tag_colors && choice <= kGuildTagPaletteCustom) {
        // The custom tile opens the picker on the primary colour, as the official popout does.
        p.tag_custom_open = choice == kGuildTagPaletteCustom;
        if (p.tag_custom_open)
            edit_color(0);
        else
            guild_tag_set_palette(&d, choice);
    } else if (id == control_tag_custom && choice < 2) {
        edit_color(choice);
        return;
    } else
        return;
    repaint();
}

bool can_save() noexcept {
    return can_manage_guild() && eligible() && dirty() && guild_tag_draft_valid(owner->tag_draft);
}

Op next_save() noexcept {
    return dirty() ? Op::tag_update : Op::none;
}

// A profile read that lands while the page is clean replaces the drafts.
bool reconcile(u64) noexcept {
    auto& p = *owner;
    const auto* profile = server_profile();
    if (!profile || (p.tag_loaded && (dirty() || guild_tag_draft_equal(guild_tag_draft(*profile), p.tag_base))))
        return false;
    load();
    return true;
}

bool ensure_data() noexcept {
    if (owner->profile_requested || !profile_fetch_due())
        return false;
    owner->profile_requested = true;
    queue(Op::profile_get);
    return true;
}

void notice_action() noexcept {
    auto& p = *owner;
    if (p.tag_loaded || read_pending() || busy() || !connected())
        return;
    // Retry reads again whatever the profile's age.
    p.profile_requested = true;
    queue(Op::profile_get);
}

const char* invalid() noexcept {
    const auto& d = owner->tag_draft;
    if (dirty() && d.enabled && !guild_tag_text_valid({d.tag, d.tag_bytes}))
        return "Server tags are 2–4 letters or numbers.";
    return nullptr;
}

} // namespace

bool tag_patch(GuildSettingsPatch* patch, GuildTagWrite* write, GuildTagColorText* colors) noexcept {
    const auto& p = *owner;
    if (!p.tag_loaded || !eligible() || !guild_tag_write(p.tag_draft, colors, write))
        return false;
    reset_value(patch);
    patch->tag = write;
    return true;
}

void tag_saved(Op op, bool ok, const Utf8View& body) noexcept {
    auto& p = *owner;
    if (!ok)
        return;
    if (op == Op::tag_adopt) {
        p.tag_adopted = true;
        return;
    }
    // The reply is the updated profile; an unreadable one still saved the draft.
    GuildAdminProfile profile{};
    if (guild_admin_decode_profile(body, {p.scratch, kScratchUnits}, &profile).ok()) {
        server_profile_loaded(&profile);
        load();
        return;
    }
    p.tag_base = p.tag_draft;
}

void tag_profile_loaded() noexcept {
    if (owner->page == Page::server_tag && (!owner->tag_loaded || !dirty()))
        load();
}

void tag_failed(const RestClientResponse& response, const Utf8View& body) noexcept {
    auto& p = *owner;
    if (response.error != RestClientError::none)
        return;
    if (response.status_code == 429) {
        p.status = utf8_literal("Too many server tag updates. Please try again later.");
        return;
    }
    // "Must be between 2 and 4 in length." and the like: the first field error, else the message.
    CommandInteractionFailure failure{};
    if (command_interaction_failure(body, {p.scratch, kScratchUnits}, {p.tag_error, sizeof(p.tag_error)}, &failure) &&
        failure.message.bytes)
        p.status = failure.message;
}

void tag_edited(const Utf8View& text) noexcept {
    guild_tag_set_text(&owner->tag_draft, text);
}

bool tag_color(u64 id, u32 choice, u32* rgb) noexcept {
    // The sample names' colours and the custom colour swatches.
    if (id >= control_tag_preview && id < control_tag_preview + kSampleCount && choice == kUiSettingsAccentChoice) {
        *rgb = kGuildRoleColors[kSampleColors[id - control_tag_preview]];
        return true;
    }
    if (id == control_tag_custom && choice < 2) {
        *rgb = choice ? guild_tag_secondary(owner->tag_draft) : guild_tag_primary(owner->tag_draft);
        return true;
    }
    return false;
}

const PageOps kServerTagPage{load,      project,   activate,    nullptr,       dirty,   can_save,
                             next_save, reconcile, ensure_data, notice_action, invalid};

} // namespace maicord::app_detail::server_settings
