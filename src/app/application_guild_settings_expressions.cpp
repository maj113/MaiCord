#include "application_guild_settings_internal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/account_media.hpp"

namespace maicord::app_detail::server_settings {
namespace {

void project_emoji(Bank& bank) noexcept {
    auto& p = *owner;
    const bool can = editable() && connected() && page_allowed(Page::emoji);
    const auto counts = guild_emoji_counts(state(), p.guild_id);
    const u32 slots = guild_emoji_slots(premium_tier(), guild_features());
    const u32 start = bank.used;
    bank.add("Static ");
    bank.number(counts.static_count);
    bank.add(" of ");
    bank.number(slots);
    bank.add(" · Animated ");
    bank.number(counts.animated_count);
    bank.add(" of ");
    bank.number(slots);
    bank.add(". PNG, GIF, JPEG or WebP up to 256 KB; larger PNGs are resized to 128×128.");
    const bool full = counts.static_count >= slots && counts.animated_count >= slots;
    push_row(control_upload, utf8_literal("Emoji"), bank.since(start), UiSettingsControlKind::action,
             can && !full, false, utf8_literal("Upload emoji"),
             {UiSettingsVariant::primary, 0, kUiSettingsNoDefault});
    if (p.failed && p.status.bytes)
        push_error({}, p.status);
    push_editor(control_search, utf8_literal("Search emoji"), {}, UiSettingsControlKind::edit, editable(), editor_search,
                utf8_literal("Search emoji"));
    const auto query = editor_text(editor_search);
    const auto& s = state();
    p.row_count = 0;
    for (u32 i = 0; i < s.emoji_count && p.row_count < kRowCapacity; ++i) {
        const auto& emoji = s.emojis[i];
        if (emoji.guild_id != p.guild_id || !guild_settings_text_matches(query, {emoji.name, emoji.name_bytes}))
            continue;
        p.member_order[p.row_count] = i;
        p.row_ids[p.row_count++] = emoji.id;
    }
    push_count_heading(bank, utf8_literal("Emoji — "), counts.static_count + counts.animated_count);
    if (!p.row_count)
        push_notice(query.bytes ? utf8_literal("No emoji match this search.")
                                : utf8_literal("This server has no custom emoji yet."));
    for (u32 row = 0; row < p.row_count; ++row) {
        const CustomEmojiRecord* emoji = &s.emojis[p.member_order[row]];
        const u32 label = bank.used;
        bank.add(":");
        bank.add({emoji->name, emoji->name_bytes});
        bank.add(":");
        const auto name = bank.since(label);
        const u32 detail = bank.used;
        const bool managed = emoji->flags & custom_emoji_managed;
        bank.add(emoji->flags & custom_emoji_animated ? utf8_literal("Animated") : utf8_literal("Static"));
        if (managed)
            bank.add(" · Managed by an integration");
        if (!(emoji->flags & custom_emoji_available))
            bank.add(" · Unavailable");
        if (emoji->role_count)
            bank.add(" · Limited to some roles");
        const bool editable_row = p.access.manage_expressions && !managed;
        const u32 index =
            push_row(kRowBase + row, name, bank.since(detail), UiSettingsControlKind::action, can && editable_row,
                     false, editable_row ? utf8_literal("Edit") : Utf8View{},
                     {UiSettingsVariant::standard,
                      static_cast<u8>(ui_settings_presentation_compact | ui_settings_presentation_avatar),
                      kUiSettingsNoDefault});
        // The emoji image in place of the row's avatar.
        if (index < kControlCapacity)
            p.avatar_media[index].key = media_asset_custom_emoji(emoji->id, (emoji->flags & custom_emoji_animated) != 0);
    }
}

void project_stickers(Bank& bank) noexcept {
    auto& p = *owner;
    const bool can = editable() && connected() && page_allowed(Page::stickers);
    const u32 slots = guild_sticker_slots(premium_tier(), guild_features());
    const u32 start = bank.used;
    bank.number(p.sticker_count);
    bank.add(" of ");
    bank.number(slots);
    bank.add(" slots used. PNG, APNG, GIF or Lottie JSON, 320×320 and up to 512 KB.");
    push_row(control_upload, utf8_literal("Stickers"), bank.since(start), UiSettingsControlKind::action,
             can && p.stickers_loaded && p.sticker_count < slots, false, utf8_literal("Upload sticker"),
             {UiSettingsVariant::primary, 0, kUiSettingsNoDefault});
    if (p.stickers_failed)
        push_error(utf8_literal("Couldn't load stickers"), p.status, utf8_literal("Try again"));
    else if (p.failed && p.status.bytes)
        push_error({}, p.status);
    else if (!p.stickers_loaded)
        push_loading(utf8_literal("Loading stickers…"));
    push_count_heading(bank, utf8_literal("Stickers — "), p.sticker_count);
    if (p.stickers_loaded && !p.sticker_count)
        push_notice(utf8_literal("This server has no stickers yet."));
    static constexpr Utf8View formats[]{{}, utf8_literal("PNG"), utf8_literal("APNG"), utf8_literal("Lottie"),
                                 utf8_literal("GIF")};
    for (u32 i = 0; i < p.sticker_count; ++i) {
        const auto& sticker = p.stickers[i];
        const u32 detail = bank.used;
        bank.add(formats[sticker.format_type <= 4 ? sticker.format_type : 0]);
        if (sticker.tags_bytes) {
            bank.add(" · ");
            bank.add({sticker.tags, sticker.tags_bytes});
        }
        if (sticker.description_bytes) {
            bank.add(" · ");
            bank.add({sticker.description, sticker.description_bytes});
        }
        if (!sticker.available)
            bank.add(" · Unavailable");
        // Lottie stickers have no still preview.
        const bool preview = sticker.format_type != 3;
        // The row's label is appended after its detail text, which the detail range then covers.
        const Utf8View name = bank.add({sticker.name, sticker.name_bytes});
        const Utf8View description = bank.since(detail);
        const u32 index = push_row(
            kRowBase + i, name, description, UiSettingsControlKind::action, can && p.access.manage_expressions, false,
            p.access.manage_expressions ? utf8_literal("Edit") : Utf8View{},
            {UiSettingsVariant::standard,
             static_cast<u8>(ui_settings_presentation_compact | (preview ? ui_settings_presentation_avatar : 0)),
             kUiSettingsNoDefault});
        if (preview && index < kControlCapacity)
            p.avatar_media[index].key = media_asset_sticker(sticker.id, sticker.format_type);
    }
    p.row_count = p.sticker_count;
}

// Soundboard sounds live in the soundboard module (catalog, edits, notices).
void project_soundboard(Bank& bank) noexcept {
    auto& p = *owner;
    const auto view = application_soundboard_guild(p.guild_id);
    p.soundboard_revision = application_soundboard_revision();
    const bool can = editable() && connected() && page_allowed(Page::soundboard);
    const u32 slots = kSoundboardGuildSlots[minimum<u8>(premium_tier(), 3)];
    const u32 start = bank.used;
    bank.number(view.count);
    bank.add(" of ");
    bank.number(slots);
    bank.add(" slots used. MP3 or Ogg, up to 512 KB and 5.2 seconds.");
    push_row(control_upload, utf8_literal("Soundboard"), bank.since(start), UiSettingsControlKind::action,
             can && view.loaded && !view.busy && view.count < slots, false, utf8_literal("Upload sound"),
             {UiSettingsVariant::primary, 0, kUiSettingsNoDefault});
    if (view.notice.bytes)
        push_error({}, view.notice);
    else if (view.busy)
        push_notice(utf8_literal("Saving…"));
    else if (!view.loaded)
        push_notice(view.loading ? utf8_literal("Loading sounds…") : utf8_literal("Couldn't load sounds yet."));
    push_count_heading(bank, utf8_literal("Sounds — "), view.count);
    if (view.loaded && !view.count)
        push_notice(utf8_literal("This server has no sounds yet."));
    const u32 count = minimum(view.count, kRowCapacity);
    for (u32 i = 0; i < count; ++i) {
        const auto& sound = view.sounds[i];
        const u32 detail = bank.used;
        if (sound.emoji_name_bytes && !sound.emoji_id) {
            bank.add({sound.emoji_name, sound.emoji_name_bytes});
            bank.add(" · ");
        }
        bank.number((sound.volume + 5) / 10);
        bank.add("% volume");
        if (!sound.available)
            bank.add(" · Unavailable");
        p.row_ids[i] = sound.sound_id;
        const Utf8View name = bank.add({sound.name, sound.name_bytes});
        const Utf8View description = bank.since(detail);
        push_row(kRowBase + i, name, description, UiSettingsControlKind::action,
                 can && p.access.manage_expressions && !view.busy, false,
                 p.access.manage_expressions ? utf8_literal("Edit") : Utf8View{},
                 {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
    }
    p.row_count = count;
}

const GuildStickerRecord* sticker(u64 id) noexcept {
    for (u32 i = 0; i < owner->sticker_count; ++i)
        if (owner->stickers[i].id == id)
            return &owner->stickers[i];
    return nullptr;
}
const CustomEmojiRecord* emoji(u64 id) noexcept {
    const auto& s = state();
    for (u32 i = 0; i < s.emoji_count; ++i)
        if (s.emojis[i].id == id && s.emojis[i].guild_id == owner->guild_id)
            return &s.emojis[i];
    return nullptr;
}
void set_asset_text(char* storage, u16* bytes, u32 capacity, const Utf8View& text) noexcept {
    *bytes = static_cast<u16>(minimum(text.bytes, capacity));
    copy_bytes(storage, text.data, *bytes);
}
ApplicationTextFormProgress expression_progress(TextFormOwnerKey key) noexcept {
    ApplicationTextFormProgress view{};
    if (!form_valid(key)) {
        view.complete = true;
        return view;
    }
    view.revision = owner->form_revision;
    view.status = owner->form_status;
    return view;
}
const SoundboardSound* sound_of(u64 id) noexcept {
    const auto* sound = application_soundboard_find(id);
    return sound && sound->guild_id == owner->guild_id ? sound : nullptr;
}
// Edits resend every field, with one changed.
bool sound_update(u64 id, const Utf8View* name, const Utf8View* emoji, const u16* volume) noexcept {
    const auto* sound = sound_of(id);
    if (!sound)
        return false;
    const Utf8View current_emoji{sound->emoji_name, sound->emoji_name_bytes};
    return application_soundboard_update(owner->guild_id, id, name ? *name : Utf8View{sound->name, sound->name_bytes},
                                         emoji ? *emoji : current_emoji, emoji ? 0 : sound->emoji_id,
                                         volume ? *volume : sound->volume);
}
// An upload abandoned at one of its steps drops the picked file.
void expression_closed(TextFormOwnerKey key) noexcept {
    if (!owner || key.scope != owner->lifetime)
        return;
    const u64 kind = key.collection;
    if ((kind == form_sound_name && next_form != form_sound_emoji) || kind == form_sound_emoji ||
        ((kind == form_emoji_create || kind == form_sticker_name || kind == form_sticker_tags) &&
         !owner->request.queued && !owner->request_id && next_form != form_sticker_tags))
        release_picked(&owner->upload);
}
bool expression_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!form_valid(key) || busy())
        return false;
    auto& p = *owner;
    const auto reject = [&](Utf8View message) noexcept {
        p.form_status = message;
        ++p.form_revision;
        return false;
    };
    p.asset_fields = 0;
    switch (key.collection) {
    case form_emoji_create:
    case form_emoji_rename:
        if (!guild_emoji_name_valid(text))
            return reject(utf8_literal("Use 2–32 letters, numbers or underscores."));
        set_asset_text(p.asset_name, &p.asset_name_bytes, sizeof(p.asset_name), text);
        if (key.collection == form_emoji_create)
            queue(Op::emoji_create);
        else
            queue(Op::emoji_rename, key.item);
        break;
    case form_sticker_name:
    case form_sticker_rename:
        if (!guild_sticker_name_valid(text))
            return reject(utf8_literal("Sticker names need 2–30 characters."));
        set_asset_text(p.asset_name, &p.asset_name_bytes, sizeof(p.asset_name), text);
        if (key.collection == form_sticker_name) {
            // The related emoji opens next, after this form has closed.
            next_form = form_sticker_tags;
            next_form_item = 0;
        } else {
            p.asset_fields = guild_asset_field_name;
            queue(Op::sticker_update, key.item);
        }
        break;
    case form_sticker_tags:
    case form_sticker_retag:
        if (!guild_sticker_tags_valid(text))
            return reject(utf8_literal("Enter 1–200 characters, like an emoji name."));
        set_asset_text(p.asset_tags, &p.asset_tags_bytes, sizeof(p.asset_tags), text);
        if (key.collection == form_sticker_tags) {
            p.asset_description_bytes = 0;
            queue(Op::sticker_create);
        } else {
            p.asset_fields = guild_asset_field_tags;
            queue(Op::sticker_update, key.item);
        }
        break;
    case form_sticker_describe:
        if (!guild_sticker_description_valid(text))
            return reject(utf8_literal("Descriptions are empty or 2–100 characters."));
        set_asset_text(p.asset_description, &p.asset_description_bytes, sizeof(p.asset_description), text);
        p.asset_fields = guild_asset_field_description;
        queue(Op::sticker_update, key.item);
        break;
    case form_emoji_delete:
        queue(Op::emoji_delete, key.item);
        break;
    case form_sound_name:
    case form_sound_rename:
        if (!soundboard_name_valid(text))
            return reject(utf8_literal("Sound names need 2–32 characters."));
        if (key.collection == form_sound_name) {
            p.sound_name_bytes = minimum<u32>(text.bytes, sizeof(p.sound_name));
            copy_bytes(p.sound_name, text.data, p.sound_name_bytes);
            // The emoji step opens next, after this form has closed.
            next_form = form_sound_emoji;
            next_form_item = 0;
        } else if (!sound_update(key.item, &text, nullptr, nullptr))
            return reject(utf8_literal("Couldn't save that sound. Try again in a moment."));
        break;
    case form_sound_emoji:
    case form_sound_emoji_edit:
        if (!soundboard_emoji_name_valid(text) || text.bytes > 32)
            return reject(utf8_literal("Enter one emoji, or leave it empty."));
        if (key.collection == form_sound_emoji) {
            if (!p.upload.data || !application_soundboard_create(p.guild_id, {p.sound_name, p.sound_name_bytes}, text,
                                                                 kSoundboardVolumeScale,
                                                                 {p.upload.data, p.upload.bytes}))
                return reject(utf8_literal("Couldn't upload that sound. Try again in a moment."));
            release_picked(&p.upload);
        } else if (!sound_update(key.item, nullptr, &text, nullptr))
            return reject(utf8_literal("Couldn't save that sound. Try again in a moment."));
        break;
    case form_sound_volume: {
        u32 percent = 0, digits = 0;
        for (u32 i = 0; i < text.bytes; ++i) {
            const char c = text.data[i];
            if (c == '%' && i + 1 == text.bytes && digits)
                break;
            if (c < '0' || c > '9' || digits == 3)
                return reject(utf8_literal("Enter a volume from 0 to 100."));
            percent = percent * 10 + static_cast<u32>(c - '0');
            ++digits;
        }
        if (!digits || percent > 100)
            return reject(utf8_literal("Enter a volume from 0 to 100."));
        const u16 volume = static_cast<u16>(percent * 10);
        if (!sound_update(key.item, nullptr, nullptr, &volume))
            return reject(utf8_literal("Couldn't save that sound. Try again in a moment."));
        break;
    }
    case form_sound_delete:
        if (!application_soundboard_remove(p.guild_id, key.item))
            return reject(utf8_literal("Couldn't delete that sound. Try again in a moment."));
        break;
    case form_sticker_delete:
        queue(Op::sticker_delete, key.item);
        break;
    case form_delete_server: {
        const auto* guild = chat_state_store_find_guild(state(), p.guild_id);
        if (!guild || !guild_delete_confirmed(text, chat_state_store_guild_name(state(), *guild)))
            return reject(utf8_literal("Type the server name exactly as it appears."));
        queue(Op::guild_delete);
        break;
    }
    default: return false;
    }
    reset_value(&p.form_status);
    repaint();
    return true;
}

} // namespace

void open_expression_form(u64 kind, u64 item) noexcept {
    auto& p = *owner;
    static char title[200];
    static char initial[kGuildStickerTagsBytes];
    Bank bank{title, sizeof(title), 0};
    ApplicationTextFormRequest request{};
    request.owner = form_key(kind, item);
    request.valid = form_valid;
    request.save = expression_save;
    request.progress = expression_progress;
    request.closed = expression_closed;
    request.single_line = true;
    reset_value(&p.form_status);
    ++p.form_revision;
    u32 initial_bytes = 0;
    const auto copy_initial = [&](Utf8View text) noexcept {
        initial_bytes = minimum(text.bytes, static_cast<u32>(sizeof(initial)));
        copy_bytes(initial, text.data, initial_bytes);
    };
    const auto* em = emoji(item);
    const auto* st = sticker(item);
    const auto* so = kind >= form_sound_name ? sound_of(item) : nullptr;
    switch (kind) {
    case form_sound_name: {
        request.title = utf8_literal("Name your sound");
        const u32 start = bank.used;
        bank.add({p.upload_filename, p.upload_filename_bytes});
        request.context = bank.since(start);
        request.label = utf8_literal("Sound name");
        request.accept = utf8_literal("Next");
        // The file name without its extension, cut to the limit.
        u32 bytes = p.upload_filename_bytes;
        for (u32 i = bytes; i > 0; --i)
            if (p.upload_filename[i - 1] == '.') {
                bytes = i - 1;
                break;
            }
        copy_initial({p.upload_filename, bytes});
        const u32 copied = initial_bytes;
        initial_bytes = minimum<u32>(initial_bytes, kSoundboardNameMaximum);
        while (initial_bytes && initial_bytes < copied && (static_cast<u8>(initial[initial_bytes]) & 0xc0) == 0x80)
            --initial_bytes;
        request.maximum_codepoints = kSoundboardNameMaximum;
        request.maximum_bytes = kSoundboardNameBytes;
        break;
    }
    case form_sound_emoji:
    case form_sound_emoji_edit:
        if (kind == form_sound_emoji_edit && !so)
            return;
        request.title = utf8_literal("Sound emoji");
        request.context = utf8_literal("Shown next to the sound. Optional.");
        request.label = utf8_literal("Emoji");
        request.accept = kind == form_sound_emoji ? utf8_literal("Upload") : utf8_literal("Save");
        if (so && !so->emoji_id)
            copy_initial({so->emoji_name, so->emoji_name_bytes});
        request.maximum_codepoints = 8;
        request.maximum_bytes = 32;
        break;
    case form_sound_rename:
        if (!so)
            return;
        request.title = utf8_literal("Rename sound");
        request.label = utf8_literal("Sound name");
        request.accept = utf8_literal("Save");
        copy_initial({so->name, so->name_bytes});
        request.maximum_codepoints = kSoundboardNameMaximum;
        request.maximum_bytes = kSoundboardNameBytes;
        break;
    case form_sound_volume: {
        if (!so)
            return;
        request.title = utf8_literal("Sound volume");
        request.context = utf8_literal("How loud the sound plays for everyone, from 0 to 100.");
        request.label = utf8_literal("Volume (%)");
        request.accept = utf8_literal("Save");
        initial_bytes = decimal_write(initial, (so->volume + 5) / 10);
        request.maximum_codepoints = request.maximum_bytes = 4;
        break;
    }
    case form_sound_delete:
        if (!so)
            return;
        bank.add("Delete ");
        bank.add({so->name, so->name_bytes});
        bank.add("?");
        request.title = bank.since(0);
        request.context = utf8_literal("It's removed from everyone's soundboard. This can't be undone.");
        request.accept = utf8_literal("Delete sound");
        request.confirmation_only = request.danger = true;
        break;
    case form_emoji_create: {
        request.title = utf8_literal("Name your emoji");
        const u32 start = bank.used;
        image_summary(bank, p.upload.info);
        request.context = bank.since(start);
        request.label = utf8_literal("Emoji name");
        request.accept = utf8_literal("Upload");
        initial_bytes = guild_emoji_name_from_filename({p.upload_filename, p.upload_filename_bytes},
                                                       {initial, sizeof(initial)});
        request.maximum_codepoints = request.maximum_bytes = kGuildEmojiNameMaximum;
        break;
    }
    case form_emoji_rename:
        if (!em)
            return;
        request.title = utf8_literal("Rename emoji");
        request.label = utf8_literal("Emoji name");
        request.accept = utf8_literal("Save");
        copy_initial({em->name, em->name_bytes});
        request.maximum_codepoints = request.maximum_bytes = kGuildEmojiNameMaximum;
        break;
    case form_emoji_delete:
        if (!em)
            return;
        bank.add("Delete :");
        bank.add({em->name, em->name_bytes});
        bank.add(":?");
        request.title = bank.since(0);
        request.context = utf8_literal("Messages that used it show its name instead. This can't be undone.");
        request.accept = utf8_literal("Delete emoji");
        request.confirmation_only = request.danger = true;
        break;
    case form_sticker_name: {
        request.title = utf8_literal("Name your sticker");
        const u32 start = bank.used;
        image_summary(bank, p.upload.info);
        request.context = bank.since(start);
        request.label = utf8_literal("Sticker name");
        request.accept = utf8_literal("Next");
        request.maximum_codepoints = kGuildStickerNameMaximum;
        request.maximum_bytes = kGuildStickerNameBytes;
        break;
    }
    case form_sticker_tags:
    case form_sticker_retag:
        if (kind == form_sticker_retag && !st)
            return;
        request.title = utf8_literal("Related emoji");
        request.context = utf8_literal("The emoji name that suggests this sticker, like smile or thumbsup.");
        request.label = utf8_literal("Related emoji");
        request.accept = kind == form_sticker_tags ? utf8_literal("Upload") : utf8_literal("Save");
        if (st)
            copy_initial({st->tags, st->tags_bytes});
        request.maximum_codepoints = kGuildStickerTagsMaximum;
        request.maximum_bytes = kGuildStickerTagsBytes;
        break;
    case form_sticker_rename:
        if (!st)
            return;
        request.title = utf8_literal("Rename sticker");
        request.label = utf8_literal("Sticker name");
        request.accept = utf8_literal("Save");
        copy_initial({st->name, st->name_bytes});
        request.maximum_codepoints = kGuildStickerNameMaximum;
        request.maximum_bytes = kGuildStickerNameBytes;
        break;
    case form_sticker_describe:
        if (!st)
            return;
        request.title = utf8_literal("Sticker description");
        request.label = utf8_literal("Description (optional)");
        request.accept = utf8_literal("Save");
        copy_initial({st->description, st->description_bytes});
        request.maximum_codepoints = kGuildStickerDescriptionMaximum;
        request.maximum_bytes = kGuildStickerDescriptionBytes;
        break;
    case form_sticker_delete:
        if (!st)
            return;
        bank.add("Delete ");
        bank.add({st->name, st->name_bytes});
        bank.add("?");
        request.title = bank.since(0);
        request.context = utf8_literal("Members can no longer send it. This can't be undone.");
        request.accept = utf8_literal("Delete sticker");
        request.confirmation_only = request.danger = true;
        break;
    case form_delete_server: {
        const auto* guild = chat_state_store_find_guild(state(), p.guild_id);
        if (!guild || !guild_settings_can_delete(p.access))
            return;
        bank.add("Delete ");
        bank.add(chat_state_store_guild_name(state(), *guild));
        request.title = bank.since(0);
        request.context = utf8_literal("This permanently deletes the server with all of its channels and messages. "
                                       "Type the server name to confirm.");
        request.label = utf8_literal("Server name");
        request.accept = utf8_literal("Delete server");
        request.danger = true;
        request.maximum_codepoints = kGuildAdminNameCodepoints;
        request.maximum_bytes = kGuildAdminNameBytes;
        break;
    }
    default: return;
    }
    request.initial = {initial, initial_bytes};
    application_text_form_open(request);
}

namespace {

void open_expression_menu(Menu menu, u64 id, const UiRect& anchor) noexcept {
    auto& p = *owner;
    u32 count = 0;
    const auto item = [&](const Utf8View& label, u32 action) noexcept { popup_row(count++, label, action); };
    if (menu == Menu::sound)
        item(utf8_literal("Preview"), expression_preview);
    item(utf8_literal("Rename"), expression_rename);
    if (menu == Menu::sticker) {
        item(utf8_literal("Change related emoji"), expression_tags);
        item(utf8_literal("Change description"), expression_description);
    } else if (menu == Menu::sound) {
        item(utf8_literal("Change emoji"), expression_emoji);
        item(utf8_literal("Change volume"), expression_volume);
    }
    popup_row(count++, utf8_literal("Delete"), expression_delete, popup_item_danger | popup_item_separator_before);
    p.menu = menu;
    p.menu_item = id;
    settings_layer_open_choice(&p.layer, id, {p.popup_items, count}, anchor, 240);
}

// The Edit menu of an emoji or sticker row.
void open_row_menu(u64 control, const UiRect& anchor) noexcept {
    auto& p = *owner;
    const bool emoji_page = p.page == Page::emoji;
    open_expression_menu(emoji_page ? Menu::emoji : Menu::sticker,
                         emoji_page ? p.row_ids[control - kRowBase] : p.stickers[control - kRowBase].id, anchor);
}

void project(Bank& bank) noexcept {
    switch (owner->page) {
    case Page::emoji: project_emoji(bank); break;
    case Page::stickers: project_stickers(bank); break;
    case Page::soundboard: project_soundboard(bank); break;
    default: break;
    }
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const u64 id = activation.value.target.control_id;
    if (p.page == Page::emoji || p.page == Page::stickers) {
        const bool emoji_page = p.page == Page::emoji;
        if (id == control_upload && !busy())
            start_pick(emoji_page ? Pick::emoji : Pick::sticker);
        else if (id >= kRowBase && id < kRowBase + p.row_count && activation.button && !busy() &&
                 p.access.manage_expressions)
            open_row_menu(id, activation.anchor);
        return;
    }
    if (p.page == Page::soundboard) {
        if (id == control_upload)
            start_pick(Pick::sound);
        else if (id >= kRowBase && id < kRowBase + p.row_count && activation.button && p.access.manage_expressions)
            open_expression_menu(Menu::sound, p.row_ids[id - kRowBase], activation.anchor);
        return;
    }
}

void choose(Menu menu, u32 action) noexcept {
    auto& p = *owner;
    if (menu == Menu::emoji || menu == Menu::sticker) {
        if (action < kMenuChoiceBase || busy() || !p.access.manage_expressions)
            return;
        const bool emoji_menu = menu == Menu::emoji;
        const u64 kind = action - kMenuChoiceBase == expression_rename
                             ? (emoji_menu ? form_emoji_rename : form_sticker_rename)
                         : action - kMenuChoiceBase == expression_tags        ? form_sticker_retag
                         : action - kMenuChoiceBase == expression_description ? form_sticker_describe
                         : action - kMenuChoiceBase == expression_delete
                             ? (emoji_menu ? form_emoji_delete : form_sticker_delete)
                             : 0;
        // The menu closes first; the form opens on the next tick.
        next_form = kind;
        next_form_item = p.menu_item;
        application_request_frame();
        return;
    }
    if (menu == Menu::sound) {
        if (action < kMenuChoiceBase || !p.access.manage_expressions)
            return;
        const u32 choice = action - kMenuChoiceBase;
        if (choice == expression_preview) {
            if (const auto* sound = sound_of(p.menu_item))
                application_soundboard_preview(sound->sound_id, sound->volume);
            return;
        }
        next_form = choice == expression_rename   ? form_sound_rename
                    : choice == expression_emoji  ? form_sound_emoji_edit
                    : choice == expression_volume ? form_sound_volume
                    : choice == expression_delete ? form_sound_delete
                                                  : 0;
        next_form_item = p.menu_item;
        application_request_frame();
        return;
    }
}

bool ensure_data() noexcept {
    if (owner->page != Page::stickers || owner->stickers_requested)
        return false;
    owner->stickers_requested = true;
    owner->stickers_failed = false;
    queue(Op::sticker_list);
    return true;
}

void notice_action() noexcept {
    if (owner->page != Page::stickers || !owner->stickers_failed || busy() || !connected())
        return;
    owner->stickers_requested = false;
    ensure_data();
}

// Right-click opens the row's Edit menu.
void context(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const u64 id = activation.value.target.control_id;
    if ((p.page != Page::emoji && p.page != Page::stickers) || id < kRowBase || id >= kRowBase + p.row_count ||
        busy() || !p.access.manage_expressions)
        return;
    if (p.page == Page::emoji) {
        const auto* e = emoji(p.row_ids[id - kRowBase]);
        if (!e || (e->flags & custom_emoji_managed))
            return;
    }
    open_row_menu(id, activation.anchor);
}

} // namespace

const PageOps kExpressionPage{nullptr, project, activate,    choose,        nullptr, nullptr,
                              nullptr, nullptr, ensure_data, notice_action, nullptr, context};

} // namespace maicord::app_detail::server_settings
