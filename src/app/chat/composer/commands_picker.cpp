#include "commands_internal.hpp"
#include "../../account/account_presentation.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/model/chat/asset_hash.hpp"
#include "maicord/ui/shell/popup_presentation.hpp"
#include "../../account/account_access.hpp"

namespace maicord::app_detail {
namespace {

u32 query_score(const Entry& entry, Utf8View query) noexcept {
    const auto& command = *entry.command;
    const Utf8View name = command.name;
    u32 score = ~0u;
    if (entry.app) {
        const Utf8View section = section_name(*entry.app);
        score = starts_with_ignore_case(section, query)  ? 5
                : ascii_contains_ignore_case(section, query) ? 6
                : fuzzy_ignore_case(query, section)          ? 11
                                                             : ~0u;
    }
    u32 first_word = query.bytes;
    for (u32 at = 0; at < query.bytes; ++at)
        if (query.data[at] == ' ') {
            first_word = at;
            break;
        }
    u32 name_word = name.bytes;
    for (u32 at = 0; at < name.bytes; ++at)
        if (name.data[at] == ' ') {
            name_word = at;
            break;
        }
    const Utf8View head{query.data, first_word};
    const Utf8View rest = first_word < query.bytes ? Utf8View{query.data + first_word + 1, query.bytes - first_word - 1}
                                                   : Utf8View{};
    const Utf8View name_rest = name_word < name.bytes ? Utf8View{name.data + name_word + 1, name.bytes - name_word - 1}
                                                      : Utf8View{};
    // Option names: prefix 3 (alone or after the command name), substring 4,
    // fuzzy 10.
    OptionMatch options{entry.index, name, query, ~0u};
    if (command.options.source.bytes)
        json_visit_indexed_array(
            entry.index->document, command.options, 64,
            [](void* opaque, u32, JsonValue option) noexcept {
                auto& m = *static_cast<OptionMatch*>(opaque);
                char storage[128];
                const Utf8View option_name = field_text(*m.index, option, "name", {storage, sizeof(storage)});
                if (!option_name.bytes)
                    return true;
                const bool after_name = m.query.bytes > m.name.bytes &&
                                        starts_with_ignore_case(m.query, m.name) && m.query.data[m.name.bytes] == ' ' &&
                                        starts_with_ignore_case(option_name, {m.query.data + m.name.bytes + 1,
                                                                              m.query.bytes - m.name.bytes - 1});
                const u32 score = starts_with_ignore_case(option_name, m.query) || after_name ? 3
                                  : ascii_contains_ignore_case(option_name, m.query)            ? 4
                                  : fuzzy_ignore_case(m.query, option_name)                     ? 10
                                                                                                : ~0u;
                m.score = minimum(m.score, score);
                return true;
            },
            &options);
    const u32 command_score =
        starts_with_ignore_case(name, query)                                                        ? 0
        : starts_with_ignore_case(name, head) && starts_with_ignore_case(name_rest, rest)           ? 1
        : ascii_contains_ignore_case(name, query)                                                  ? 2
        : options.score <= 4                                                                       ? options.score
        : ascii_contains_ignore_case(command.description, query)                                   ? 7
        : fuzzy_ignore_case(query, name)                                                           ? 9
        : options.score == 10                                                                      ? 10
        : fuzzy_ignore_case(query, command.description)                                            ? 12
                                                                                                   : ~0u;
    return minimum(score, command_score);
}
// Account frecency score (hundredths) of a command, 0 when never used.
i32 frecency_score(const ApplicationCommand& command, u64 guild) noexcept {
    const auto* entry = frecency_entry(command, guild);
    return entry ? entry->score : 0;
}

} // namespace
namespace commands_detail {

bool project(Utf8View input) noexcept {
    auto& c = *commands;
    const PickerQuery query = picker_query(input);
    arena_release(&c.projection);
    c.entries = nullptr;
    c.rail = c.body = nullptr;
    c.count = c.rail_count = c.body_count = 0;
    c.query_bytes = utf8_copy_truncated(c.query, sizeof(c.query), query.text);
    const CommandIndex* builtins = builtin_commands_index();
    const CommandIndex* plugin_index = plugin_commands_index();
    const u32 capacity = c.index[0].command_count + c.index[1].command_count +
                         (builtins ? builtins->command_count : 0) + (plugin_index ? plugin_index->command_count : 0);
    // Candidates, their ranks and the apps, then the rows and both section lists.
    const usize bytes = static_cast<usize>(capacity) * (2 * sizeof(Entry) + 2 * sizeof(u32) + sizeof(void*)) +
                        static_cast<usize>(capacity + 3) * 2 * sizeof(Section) + frequently_shown * sizeof(Entry) +
                        4096;
    if (!arena_initialize(&c.projection, bytes, 0, 4096))
        return false;
    Entry* candidates = arena_allocate<Entry>(&c.projection, maximum(1u, capacity));
    u32* ranks = arena_allocate<u32>(&c.projection, maximum(1u, capacity));
    u32* order = arena_allocate<u32>(&c.projection, maximum(1u, capacity));
    const CommandApplication** apps = arena_allocate<const CommandApplication*>(&c.projection, maximum(1u, capacity));
    c.entries = arena_allocate<Entry>(&c.projection, capacity + frequently_shown);
    c.rail = arena_allocate<Section>(&c.projection, capacity + 3);
    c.body = arena_allocate<Section>(&c.projection, capacity + 3);
    if (!candidates || !ranks || !order || !apps || !c.entries || !c.rail || !c.body)
        return false;
    u32 candidate_count = 0, app_count = 0;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, c.channel);
    const CommandIndex* const sources[] = {&c.index[0], &c.index[1], plugin_index};
    for (const CommandIndex* source_pointer : sources)
        for (u32 n = 0; source_pointer && n < source_pointer->command_count; ++n) {
            const auto& source = *source_pointer;
            const auto& command = source.commands[n];
            if ((!c.guild && !command.dm_permission) || (command.guild_id && command.guild_id != c.guild))
                continue;
            if (source_pointer != plugin_index && channel && channel->permissions_resolved &&
                !(channel->effective_permissions & (1ull << 31)))
                continue;
            bool duplicate = false;
            for (u32 j = 0; j < candidate_count && !duplicate; ++j)
                duplicate = candidates[j].command->id == command.id &&
                            utf8_equal(candidates[j].command->name, command.name);
            if (duplicate)
                continue;
            const auto* app = find_app(source, command.application_id);
            if (!app)
                continue;
            bool seen = false;
            for (u32 j = 0; j < app_count && !seen; ++j)
                seen = apps[j]->id == app->id;
            if (!seen)
                apps[app_count++] = app;
            candidates[candidate_count++] = {&command, &source, app};
        }
    // Apps by section name; Built-in comes last.
    heap_sort(apps, app_count, [](const CommandApplication* a, const CommandApplication* b) noexcept {
        const i32 order = compare_names(section_name(*a), section_name(*b));
        return order ? order < 0 : a->id < b->id;
    });
    const u32 first_builtin = candidate_count;
    for (u32 n = 0; builtins && n < builtins->command_count; ++n)
        if (builtin_command_available(builtins->commands[n], c.channel))
            candidates[candidate_count++] = {builtins->commands + n, builtins, builtins->applications};
    for (u32 n = 0; n < candidate_count; ++n) {
        order[n] = n;
        u32 rank = app_count;
        for (u32 j = 0; n < first_builtin && j < app_count; ++j)
            if (apps[j]->id == candidates[n].app->id) {
                rank = j;
                break;
            }
        ranks[n] = rank;
    }
    c.discovery = !query.text.bytes;
    if (!c.discovery) {
        // Query results: ranked by score, then frecency, then name, at most twenty.
        u32 kept = 0;
        for (u32 n = 0; n < candidate_count; ++n) {
            const auto& name = candidates[n].command->name;
            if (query.terminated &&
                !(ascii_equal_ignore_case(name, query.text) ||
                  (name.bytes > query.text.bytes && starts_with_ignore_case(name, query.text) &&
                   name.data[query.text.bytes] == ' ')))
                continue;
            const u32 score = query_score(candidates[n], query.text);
            if (score == ~0u)
                continue;
            ranks[kept] = score;
            order[kept++] = n;
        }
        struct Rank {
            const Entry* candidates;
            const u32* scores;
            const u32* order;
            u64 guild;
        } rank{candidates, ranks, order, c.guild};
        u32 slots[query_limit + 1];
        u32 slot_count = 0;
        // Keep the best twenty with an insertion pass (positions into order/ranks).
        const auto better = [&rank](u32 a, u32 b) noexcept {
            if (rank.scores[a] != rank.scores[b])
                return rank.scores[a] < rank.scores[b];
            const auto& x = rank.candidates[rank.order[a]];
            const auto& y = rank.candidates[rank.order[b]];
            const i32 fx = frecency_score(*x.command, rank.guild), fy = frecency_score(*y.command, rank.guild);
            if (fx != fy)
                return fx > fy;
            const i32 names = compare_names(x.command->name, y.command->name);
            return names ? names < 0 : rank.order[a] < rank.order[b];
        };
        for (u32 n = 0; n < kept; ++n) {
            u32 at = slot_count < query_limit ? slot_count++ : query_limit;
            while (at && better(n, slots[at - 1])) {
                if (at < query_limit)
                    slots[at] = slots[at - 1];
                --at;
            }
            if (at < query_limit)
                slots[at] = n;
        }
        for (u32 n = 0; n < slot_count; ++n)
            c.entries[c.count++] = candidates[order[slots[n]]];
    } else {
        // Discovery: each app's commands by name, then Frequently
        // Used (up to five by score among its hundred most frecent) ahead.
        heap_sort(order, candidate_count, [&](u32 a, u32 b) noexcept {
            if (ranks[a] != ranks[b])
                return ranks[a] < ranks[b];
            const i32 names = compare_names(candidates[a].command->name, candidates[b].command->name);
            return names ? names < 0 : a < b;
        });
        u32 picked[frequently_shown]{};
        i32 scores[frequently_shown]{};
        u32 frequent = 0;
        for (u32 n = 0; n < candidate_count; ++n) {
            const i32 score = frequently_score(*candidates[order[n]].command, c.guild);
            if (!score)
                continue;
            u32 at = frequent < frequently_shown ? frequent++ : frequently_shown;
            while (at && scores[at - 1] < score) {
                if (at < frequently_shown) {
                    picked[at] = picked[at - 1];
                    scores[at] = scores[at - 1];
                }
                --at;
            }
            if (at < frequently_shown) {
                picked[at] = order[n];
                scores[at] = score;
            }
        }
        if (frequent)
            c.rail[c.rail_count++] = {nullptr, 0, frequent, 0, SectionKind::frequent};
        for (u32 n = 0; n < candidate_count;) {
            const u32 rank = ranks[order[n]];
            u32 end = n;
            while (end < candidate_count && ranks[order[end]] == rank)
                ++end;
            c.rail[c.rail_count++] = {candidates[order[n]].app, frequent + n, end - n, 0,
                                      rank == app_count ? SectionKind::builtin : SectionKind::app};
            n = end;
        }
        // A rail click filters the body to that section alone.
        const Section* filtered = nullptr;
        for (u32 s = 0; s < c.rail_count && c.filter_app && !filtered; ++s)
            if (c.rail[s].app && c.rail[s].app->id == c.filter_app)
                filtered = c.rail + s;
        if (!filtered)
            c.filter_app = 0;
        if (filtered) {
            for (u32 n = 0; n < filtered->count; ++n)
                c.entries[n] = candidates[order[filtered->first - frequent + n]];
            c.count = filtered->count;
            c.body[c.body_count++] = {filtered->app, 0, filtered->count, 0, filtered->kind};
        } else {
            for (u32 n = 0; n < frequent; ++n)
                c.entries[c.count++] = candidates[picked[n]];
            for (u32 n = 0; n < candidate_count; ++n)
                c.entries[c.count++] = candidates[order[n]];
            for (u32 s = 0; s < c.rail_count; ++s)
                c.body[c.body_count++] = c.rail[s];
        }
        // While an index loads, the first section ends in placeholder rows.
        if (c.pending && c.body_count)
            c.body[0].placeholders = discovery_placeholder_rows;
    }
    c.query_placeholders = !c.discovery && c.pending ? query_placeholder_rows : 0;
    c.offset = c.rail_offset = 0;
    c.rail_focus = no_row;
    // The discovery list starts without a selection; results of a
    // typed query focus their first row.
    c.keyboard_row = c.discovery ? no_row : 0;
    ++c.generation;
    return true;
}

} // namespace commands_detail
namespace {

// Keeps a picture's URL for the media pump under its key (the URL's hash),
// fetched as a still image of format. Kind none when the URL does not fit.
MediaAssetKey url_picture(Utf8View url, MediaAttachmentFormat format) noexcept {
    auto& c = *commands;
    if (!url.bytes || url.bytes > sizeof(c.icon_urls[0].url))
        return {};
    const u64 hash = hash64_bytes(kHash64Seed, url.data, url.bytes);
    bool known = false;
    for (const auto& entry : c.icon_urls)
        known = known || (entry.hash == hash && entry.bytes);
    if (!known) {
        auto& entry = c.icon_urls[c.icon_url_next++ % (sizeof(c.icon_urls) / sizeof(c.icon_urls[0]))];
        entry.hash = hash;
        entry.bytes = static_cast<u16>(url.bytes);
        copy_bytes(entry.url, url.data, url.bytes);
    }
    return popup_media_url_key(hash, format);
}

} // namespace
namespace commands_detail {

// The app's picture for the picker and the composer's command bar alike: the
// bot's avatar in this server (its member record), its avatar, the
// application's icon (app-icons), then the bot's default avatar; all still.
// An index bot not yet known is made resident from the index, as the local
// command message does; one known without an avatar takes the index's.
MediaAssetKey app_picture(const CommandApplication* app) noexcept {
    if (!app || !g_app.account_ui)
        return {};
    const auto& c = *commands;
    auto& users = g_app.account_ui->user_store;
    char url[192];
    ProtocolWriter w{{reinterpret_cast<u8*>(url), sizeof(url)}, 0};
    if (c.guild && app->bot_id)
        if (const auto* member = user_appearance_find(users.appearance, app->bot_id, c.guild)) {
            const Utf8View hash = member->avatar.view();
            if (user_avatar_variant(hash) &&
                protocol_writer_append_c_string(&w, "https://cdn.discordapp.com/guilds/") &&
                protocol_writer_append_u64(&w, c.guild) && protocol_writer_append_c_string(&w, "/users/") &&
                protocol_writer_append_u64(&w, app->bot_id) && protocol_writer_append_c_string(&w, "/avatars/") &&
                protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(hash.data), hash.bytes) &&
                protocol_writer_append_c_string(&w, ".webp?size=64"))
                return url_picture({url, w.used}, MediaAttachmentFormat::webp);
        }
    const UserRecord* user = nullptr;
    if (app->bot_id) {
        if (!user_store_find(users, app->bot_id))
            user_store_upsert(&users, app->bot_id, section_name(*app));
        user = user_store_find(users, app->bot_id);
        if (user && !user_store_avatar_hash(*user).bytes && app->avatar.bytes &&
            user_store_set_avatar(&users, app->bot_id, app->avatar))
            user = user_store_find(users, app->bot_id);
    }
    const Utf8View hash = user ? user_store_avatar_hash(*user) : Utf8View{};
    if (const u32 variant = user ? user_avatar_variant(hash) : 0)
        return media_asset_user_avatar(user->id, variant, asset_hash_animated(hash));
    if (const u32 bytes = user_activity_application_icon_url(app->id, app->icon, {url, sizeof(url)}))
        return url_picture({url, bytes}, MediaAttachmentFormat::png);
    if (!app->bot_id)
        return {};
    // The CDN's default avatar for the bot ((id >> 22) % 6, as profiles use).
    static constexpr char prefix[] = "https://cdn.discordapp.com/embed/avatars/";
    constexpr u32 prefix_bytes = sizeof(prefix) - 1;
    copy_bytes(url, prefix, prefix_bytes);
    url[prefix_bytes] = static_cast<char>('0' + (app->bot_id >> 22) % 6);
    copy_bytes(url + prefix_bytes + 1, ".png", 4);
    return url_picture({url, prefix_bytes + 5}, MediaAttachmentFormat::png);
}

} // namespace commands_detail
namespace {

// Puts the app's picture on a picker row.
bool avatar(PopupItem& item, const CommandApplication* app) noexcept {
    const MediaAssetKey key = app_picture(app);
    if (key.kind == MediaAssetKind::none)
        return false;
    item.flags |= popup_item_avatar;
    item.media_id = key.id;
    const bool url = key.kind == MediaAssetKind::profile_image;
    item.media_variant = url ? static_cast<u32>(media_asset_embed_format(key)) : key.variant;
    item.media_flags = url ? kPopupItemMediaUrl : (key.flags & media_asset_flag_animated) ? 1 : 0;
    return true;
}

} // namespace
namespace commands_detail {

OptionList command_options() noexcept {
    OptionList list{};
    const auto& c = *commands;
    if (c.selected.command && c.selected.command->options.source.bytes)
        json_visit_indexed_array(
            c.selected.index->document, c.selected.command->options, 64,
            [](void* opaque, u32, JsonValue option) noexcept {
                auto& l = *static_cast<OptionList*>(opaque);
                if (l.count < 25)
                    l.options[l.count++] = option;
                return true;
            },
            &list);
    return list;
}
bool option_flag(JsonValue option, const char* key) noexcept {
    bool value = false;
    json_value_to_bool(command_member(commands->selected.index->document, option, {key, c_string_length(key)}),
                       &value);
    return value;
}
// Option type of one entry of a command's options array (0 when unreadable).
u64 option_type(const CommandIndex& index, JsonValue option) noexcept {
    u64 type = 0;
    json_value_to_u64(command_member(index.document, option, utf8_literal("type")), &type);
    return type;
}
// The selected command's typed options in the draft, in absolute bytes.
u32 typed_options(Utf8View input, Slice<CommandArgumentSpan> spans) noexcept {
    const auto& c = *commands;
    const u32 start = c.selected.command->name.bytes + 1;
    if (input.bytes <= start)
        return 0;
    const u32 count = command_argument_spans(*c.selected.index, *c.selected.command,
                                             {input.data + start, input.bytes - start}, spans);
    for (u32 n = 0; n < count; ++n) {
        spans[n].key_first += start;
        spans[n].value_first += start;
        spans[n].value_last += start;
        spans[n].end_last += start;
    }
    return count;
}
// The chip the caret is in (up to its end marker), or no_option: after the
// marker the caret is outside.
u32 chip_at(Utf8View, const CommandArgumentSpan* spans, u32 count, u32 caret) noexcept {
    for (u32 n = 0; n < count; ++n)
        if (caret >= spans[n].value_first && caret <= spans[n].value_last)
            return n;
    return no_option;
}
bool option_end_at(Utf8View input, u32 at) noexcept {
    return at + kCommandOptionEndBytes <= input.bytes &&
           bytes_equal(input.data + at, kCommandOptionEnd, kCommandOptionEndBytes);
}
// The word the caret ends outside every chip: back to a blank, an option's
// end or the command's name.
Utf8View caret_word(Utf8View input, u32 caret) noexcept {
    const u32 floor = commands->selected.command->name.bytes + 1;
    u32 first = minimum(caret, input.bytes);
    while (first > floor && !command_space(input.data[first - 1]) &&
           !(first >= kCommandOptionEndBytes && option_end_at(input, first - kCommandOptionEndBytes)))
        --first;
    return {input.data + first, minimum(caret, input.bytes) - first};
}
ConstSlice<CommandAcceptedChoice> accepted_choices() noexcept {
    return {commands->accepted, commands->accepted_count};
}
// Option checks after an edit: the option the caret
// left and options that failed and changed are checked again; validate_all
// checks every option (allow_empty while the command is being set up).
void check_options(const CommandArgumentSpan* spans, u32 count, u32 active_slot, bool validate_all,
                   bool allow_empty) noexcept {
    auto& c = *commands;
    const auto list = command_options();
    for (u32 slot = 0; slot < list.count; ++slot) {
        auto& state = c.option_states[slot];
        const CommandArgumentSpan* span = nullptr;
        for (u32 n = 0; n < count && !span; ++n)
            span = spans[n].slot == slot ? spans + n : nullptr;
        const Utf8View value = span ? span->value : Utf8View{};
        const u64 hash = span ? hash64_bytes(kHash64Seed, value.data, value.bytes) | 1 : 0;
        const bool check = validate_all || (slot == c.previous_slot && c.previous_slot != active_slot) ||
                           (state.error.bytes && hash != state.value_hash);
        state.value_hash = hash;
        if (check)
            state.error = command_option_check(*c.selected.index, list.options[slot], span != nullptr, value,
                                               allow_empty, {state.message, sizeof(state.message)},
                                               accepted_choices());
    }
    c.previous_slot = active_slot;
}
void reset_option_states(Commands& c) noexcept {
    for (auto& state : c.option_states) {
        reset_value(&state.error);
        state.value_hash = 0;
    }
    c.previous_slot = no_option;
    c.focused_slot = no_option;
    reset_value(&c.focused_option);
    c.accepted_count = 0;
    c.focused_revision = ~0u;
}
// The chip the caret is in becomes the active option; the option it left and
// failed options whose value changed are checked again. Once per
// composer revision: the composer's projection asks first, so the chips it
// draws show the checks of this very edit.
void update_focus(Utf8View input) noexcept {
    auto& c = *commands;
    if (!c.selected.command || c.focused_revision == g_app.message_composer.revision)
        return;
    c.focused_revision = g_app.message_composer.revision;
    CommandArgumentSpan spans[25];
    const u32 count = typed_options(input, {spans, 25});
    const u32 chip = chip_at(input, spans, count, g_app.message_composer.caret);
    reset_value(&c.focused_option);
    c.focused_slot = no_option;
    if (chip != no_option) {
        c.focused_option = spans[chip].option;
        c.focused_slot = spans[chip].slot;
        c.value_begin = spans[chip].value_first;
        c.value_end = spans[chip].value_last;
    }
    check_options(spans, count, c.focused_slot, false, false);
}
bool autocomplete_enabled() noexcept {
    const auto& c = *commands;
    return c.selected.command && c.focused_option.source.bytes && option_flag(c.focused_option, "autocomplete");
}
u64 next_nonce() noexcept {
    const u64 stamp = (clock_unix_milliseconds() - kSnowflakeEpochMilliseconds) << 22;
    auto& serial = commands->nonce_serial;
    serial = maximum(serial + 1, stamp);
    return serial;
}
// The focused option's text, used to query its choices.
Utf8View focused_query() noexcept {
    const auto& c = *commands;
    const auto input = message_composer_text(g_app.message_composer);
    if (!c.focused_option.source.bytes || c.value_begin > c.value_end || c.value_end > input.bytes)
        return {};
    u32 first = c.value_begin, last = c.value_end;
    while (first < last && command_space(input.data[first]))
        ++first;
    while (last > first && command_space(input.data[last - 1]))
        --last;
    return {input.data + first, last - first};
}

} // namespace commands_detail
namespace {

// A title row of the option lists (plain
// text, upper case): the title, or "<matching> <query>" with the query too.
void list_title(PopupItem* items, u32* count, Utf8View title, Utf8View matching, Utf8View query) noexcept {
    if (*count >= 31)
        return;
    auto& c = *commands;
    const u32 n = (*count)++;
    Utf8View label = title;
    if (query.bytes) {
        label = copy_label(c.labels[n], 512, matching, utf8_literal(" "), query);
        for (u32 at = matching.bytes; at < label.bytes; ++at)
            if (c.labels[n][at] >= 'a' && c.labels[n][at] <= 'z')
                c.labels[n][at] = static_cast<char>(c.labels[n][at] - ('a' - 'A'));
    }
    // Popup actions are unique: each title takes its own below the messages'.
    items[n] = {label, value_base - 2 - n, popup_item_command_section, 0};
}
// The focused option's choice list: BOOLEAN True/False, static choices ranked
// against the option's text, or
// the application's suggestions. Returns false when the option has no list.
bool render_choices(PopupItem* items, u32* count) noexcept {
    auto& c = *commands;
    c.choice_count = 0;
    c.choice_suggested = false;
    if (!c.focused_option.source.bytes)
        return false;
    const Utf8View query = focused_query();
    const auto& doc = c.selected.index->document;
    if (autocomplete_enabled()) {
        const auto& state = c.autocomplete;
        c.choice_suggested = true;
        if (state.phase == CommandAutocompletePhase::failed) {
            items[(*count)++] = {utf8_literal("Loading options failed"), value_base - 1, popup_item_none, 0};
            return true;
        }
        if (state.phase == CommandAutocompletePhase::ready && !state.suggestions.count) {
            items[(*count)++] = {utf8_literal("No options match your search"), value_base - 1, popup_item_none, 0};
            return true;
        }
        list_title(items, count, utf8_literal("OPTIONS"), utf8_literal("OPTIONS MATCHING"), query);
        // While the application answers, five loading rows show.
        if (state.phase != CommandAutocompletePhase::ready) {
            for (u32 seed = 0; seed < 5 && *count < 31; ++seed)
                items[(*count)++] = popup_placeholder_row(value_base - 40 - seed, seed);
            return true;
        }
        for (u32 slot = 0; slot < state.suggestions.count; ++slot) {
            const auto& choice = state.suggestions.choices[slot];
            c.choice_bytes[slot] = copy_label(c.choice_names[slot], 512, choice.name).bytes;
            ++c.choice_count;
            if (*count < 31)
                items[(*count)++] = {{c.choice_names[slot], c.choice_bytes[slot]}, value_base + slot,
                                     popup_item_enabled, 0};
        }
        return true;
    }
    // Static choices (or True/False): every name scoring above zero, best first.
    const auto choices = command_member(doc, c.focused_option, utf8_literal("choices"));
    const bool boolean = option_type(*c.selected.index, c.focused_option) == 5;
    if (!boolean && choices.kind != JsonValueKind::array)
        return false;
    struct Ranked {
        Commands* c;
        Utf8View query;
        u32 score[25];
    } ranked{&c, query, {}};
    const auto add = [](Ranked& r, Utf8View name) noexcept {
        const u32 score = command_choice_score(name, r.query);
        if (!score || r.c->choice_count == 25 || !name.bytes)
            return;
        // A stable insertion by descending score.
        u32 at = r.c->choice_count++;
        while (at && r.score[at - 1] < score) {
            r.score[at] = r.score[at - 1];
            copy_bytes(r.c->choice_names[at], r.c->choice_names[at - 1], r.c->choice_bytes[at - 1]);
            r.c->choice_bytes[at] = r.c->choice_bytes[at - 1];
            --at;
        }
        r.score[at] = score;
        r.c->choice_bytes[at] = copy_label(r.c->choice_names[at], 512, name).bytes;
    };
    if (boolean) {
        add(ranked, utf8_literal("True"));
        add(ranked, utf8_literal("False"));
    } else {
        struct Visit {
            Ranked* ranked;
            const decltype(add)* add;
        } visit{&ranked, &add};
        json_visit_indexed_array(
            doc, choices, 64,
            [](void* opaque, u32, JsonValue choice) noexcept {
                auto& v = *static_cast<Visit*>(opaque);
                char storage[512];
                (*v.add)(*v.ranked, field_text(*v.ranked->c->selected.index, choice, "name", {storage, 512}));
                return true;
            },
            &visit);
    }
    if (!c.choice_count) {
        items[(*count)++] = {utf8_literal("No options match your search"), value_base - 1, popup_item_none, 0};
        return true;
    }
    list_title(items, count, utf8_literal("OPTIONS"), utf8_literal("OPTIONS MATCHING"), query);
    for (u32 slot = 0; slot < c.choice_count && *count < 31; ++slot)
        items[(*count)++] = {{c.choice_names[slot], c.choice_bytes[slot]}, value_base + slot,
                             popup_item_enabled, 0};
    return true;
}
// The option list outside every chip: the options without
// a value whose name starts with the word at the caret, required ones first
// under their own title.
void render_options(PopupItem* items, u32* count, const CommandArgumentSpan* spans, u32 typed,
                    Utf8View query) noexcept {
    auto& c = *commands;
    const auto list = command_options();
    for (u32 pass = 0; pass < 2; ++pass) {
        bool titled = false;
        for (u32 slot = 0; slot < list.count && *count < 31; ++slot) {
            bool present = false;
            for (u32 n = 0; n < typed && !present; ++n)
                present = spans[n].slot == slot;
            char storage[128];
            const auto name = field_text(*c.selected.index, list.options[slot], "name", {storage, sizeof(storage)});
            if (present || option_flag(list.options[slot], "required") != (pass == 0) || name.bytes < query.bytes ||
                !utf8_equal({name.data, query.bytes}, query))
                continue;
            if (!titled) {
                titled = true;
                list_title(items, count,
                           pass == 0        ? utf8_literal("REQUIRED OPTIONS")
                           : c.option_count ? utf8_literal("OTHER OPTIONS")
                                            : utf8_literal("OPTIONS"),
                           pass == 0 ? utf8_literal("REQUIRED OPTIONS MATCHING") : utf8_literal("OPTIONS MATCHING"),
                           query);
            }
            const u32 n = (*count)++;
            items[n] = {copy_label(c.labels[n], 512, name), option_base + slot,
                        popup_item_enabled, 0};
            items[n].detail = field_text(*c.selected.index, list.options[slot], "description", {c.details[n], 1024});
            ++c.option_count;
        }
    }
}
// Closes the commands popup without dismissing the draft's completion (a
// list with no rows; the next one opens again).
void hide_popup() noexcept {
    auto& c = *commands;
    const u64 dismissed_document = c.dismissed_document;
    const u32 dismissed_revision = c.dismissed_revision;
    close_popup();
    c.dismissed_document = dismissed_document;
    c.dismissed_revision = dismissed_revision;
}

} // namespace
namespace commands_detail {

float list_item_height(const PopupItem& item) noexcept {
    return (item.flags & popup_item_command_section) ? kCommandTitleHeight
           : popup_item_placeholder(item)            ? kCommandGenericLoadingHeight
                                                     : kCommandGenericRowHeight;
}
// The selected command's list under the composer's option bar: the
// active option's choices, the unset options matching the word at the caret
// while it is outside every chip, or the file picker on an ATTACHMENT chip.
bool render_command(const UiLayoutNode& surface) noexcept {
    auto& c = *commands;
    PopupItem items[32]{};
    u32 count = 0;
    const auto input = message_composer_text(g_app.message_composer);
    CommandArgumentSpan spans[25];
    const u32 typed = typed_options(input, {spans, 25});
    c.option_count = 0;
    c.choices_shown = render_choices(items, &count);
    if (!c.choices_shown && !c.focused_option.source.bytes) {
        render_options(items, &count, spans, typed, caret_word(input, g_app.message_composer.caret));
    } else if (!c.choices_shown && option_type(*c.selected.index, c.focused_option) == 11) {
        // An ATTACHMENT chip offers the file picker.
        list_title(items, &count, utf8_literal("OPTIONS"), {}, {});
        items[count++] = {utf8_literal("Drag and drop or click to upload file"), option_base + c.focused_slot,
                          popup_item_enabled, 0};
        c.option_count = 1;
    }
    if (c.choices_shown)
        c.option_count = c.choice_count;
    // A lone message ("No options match your search") takes the title row.
    if (count == 1 && !(items[0].flags & popup_item_command_section))
        items[0].flags = popup_item_command_section;
    c.list_height = 0;
    for (u32 n = 0; n < count; ++n)
        c.list_height += list_item_height(items[n]) +
                         (n && (items[n].flags & popup_item_command_section) ? kCommandSectionGap : 0.f);
    c.rendered_generation = c.generation;
    // /gif and /sticker show their results above the composer for the query option.
    Utf8View query{};
    for (u32 n = 0; n < typed; ++n)
        if (utf8_equal(spans[n].name, utf8_literal("query")))
            query = spans[n].value;
    // Without rows there is no list, only the option bar.
    if (!count) {
        hide_popup();
        return builtin_command_results(c.selected.command, query, surface.rectangle);
    }
    const UiRect viewport = application_viewport_dips();
    const UiRect anchor = surface.rectangle;
    const auto result = popup_scene_open(&g_app.popup_scene, PopupKind::autocomplete, c.channel, {items, count},
                                         anchor, viewport, ui_rect_width(anchor), true, c.offset, 0);
    if (!result.ok())
        return false;
    set_popup_owner(PopupOwnerKind::commands);
    g_app.popup_scene.selected_index = ~0u;
    // The keyboard row counts the list's actionable rows from one.
    if (c.keyboard_row != no_row && c.option_count) {
        c.keyboard_row = minimum(maximum(c.keyboard_row, 1u), c.option_count);
        for (u32 n = 0, row = 0; n < count; ++n)
            if ((items[n].flags & popup_item_enabled) && ++row == c.keyboard_row) {
                g_app.popup_scene.selected_index = n;
                break;
            }
    }
    return builtin_command_results(c.selected.command, query, g_app.popup_scene.rectangle);
}

} // namespace commands_detail
namespace {

// The usage summary after a command name: its required option names
// (drawn as chips), then "+N optional" for the rest; parts are \x1E-separated.
struct OptionSummary {
    const CommandIndex* index;
    ProtocolWriter writer;
    u32 optional;
};
Utf8View option_summary(const CommandIndex& index, const ApplicationCommand& command, Slice<char> output) noexcept {
    OptionSummary summary{&index, {{reinterpret_cast<u8*>(output.data), output.count}, 0}, 0};
    if (command.options.source.bytes)
        json_visit_indexed_array(
            index.document, command.options, 64,
            [](void* opaque, u32, JsonValue option) noexcept {
                auto& s = *static_cast<OptionSummary*>(opaque);
                bool required = false;
                json_value_to_bool(command_member(s.index->document, option, utf8_literal("required")), &required);
                if (!required) {
                    ++s.optional;
                    return true;
                }
                char storage[128];
                const Utf8View name = field_text(*s.index, option, "name", {storage, sizeof(storage)});
                if (name.bytes && s.writer.used + name.bytes + 1 <= s.writer.output.count) {
                    if (s.writer.used)
                        protocol_writer_append_byte(&s.writer, 0x1E);
                    protocol_writer_append_bytes(&s.writer, {reinterpret_cast<const u8*>(name.data), name.bytes});
                }
                return true;
            },
            &summary);
    if (summary.optional && summary.writer.used + 24 <= output.count) {
        if (summary.writer.used)
            protocol_writer_append_byte(&summary.writer, 0x1E);
        protocol_writer_append_byte(&summary.writer, '+');
        protocol_writer_append_u64(&summary.writer, summary.optional);
        protocol_writer_append_literal(&summary.writer, " optional");
    }
    return {output.data, summary.writer.used};
}
// Picker geometry, virtual from the top of each column (see kCommandPicker*).
// The row before Built-in is 8 DIPs taller; the
// divider line in that gap is drawn unless Frequently Used is that row.
u32 rail_divider(const Commands& c) noexcept {
    return c.rail_count >= 2 && c.rail[c.rail_count - 1].kind == SectionKind::builtin ? c.rail_count - 1 : no_row;
}

} // namespace
namespace commands_detail {

float rail_top(const Commands& c, u32 r) noexcept {
    const u32 divider = rail_divider(c);
    return kCommandRailLead + static_cast<float>(r) * kCommandRailStride +
           (divider != no_row && r >= divider ? kCommandRailDivider : 0.f);
}
float rail_extent(const Commands& c) noexcept {
    return c.rail_count ? rail_top(c, c.rail_count - 1) + kCommandRailIcon + kCommandListInset : 0.f;
}

} // namespace commands_detail
namespace {

// A section's rows: its commands and, while indexes load, the
// placeholder rows after them.
u32 section_rows(const Section& section) noexcept {
    return section.count + section.placeholders;
}
float section_top(const Commands& c, u32 s) noexcept {
    float y = kCommandListInset;
    for (u32 n = 0; n < s && n < c.body_count; ++n)
        y += kCommandSectionHeight + static_cast<float>(section_rows(c.body[n])) * kCommandRowHeight +
             kCommandSectionGap;
    return y;
}

} // namespace
namespace commands_detail {

// Discovery without sections shows one status row; query results add their
// title and the list's bottom padding.
float body_extent(const Commands& c) noexcept {
    if (!c.discovery)
        return kCommandTitleHeight +
               static_cast<float>(maximum(c.count + c.query_placeholders, 1u)) * kCommandRowHeight +
               kCommandListInset;
    return c.body_count && !c.error.bytes ? section_top(c, c.body_count) - kCommandSectionGap
                                          : kCommandListInset + kCommandRowHeight;
}
// Virtual top of the row of entry k.
float row_top(const Commands& c, u32 k) noexcept {
    if (!c.discovery)
        return kCommandTitleHeight + static_cast<float>(k) * kCommandRowHeight;
    for (u32 s = 0; s < c.body_count; ++s) {
        const auto& section = c.body[s];
        if (k >= section.first && k < section.first + section.count)
            return section_top(c, s) + kCommandSectionHeight + static_cast<float>(k - section.first) * kCommandRowHeight;
    }
    return 0;
}
// The rail entry highlighted: the filtered section, else the one
// the list is scrolled to (its top within 20 DIPs of the view's top).
u32 active_rail(const Commands& c) noexcept {
    if (!c.discovery || !c.rail_count)
        return no_row;
    for (u32 r = 0; c.filter_app && r < c.rail_count; ++r)
        if (c.rail[r].app && c.rail[r].app->id == c.filter_app)
            return r;
    u32 spy = 0;
    for (u32 s = 1; s < c.body_count; ++s)
        if (section_top(c, s) <= c.offset + spy_lead)
            spy = s;
    return minimum(spy, c.rail_count - 1);
}

} // namespace commands_detail
namespace {

Utf8View section_label(const Section& section) noexcept {
    return section.kind == SectionKind::frequent ? utf8_literal("Frequently Used") : section_name(*section.app);
}
// The section's rail and header picture: the clock, Built-in's slash, the
// app's bot (a generic app glyph without one).
void section_picture(PopupItem& item, const Section& section) noexcept {
    if (section.kind == SectionKind::app && avatar(item, section.app))
        return;
    item.presentation = ui_menu_row_presentation(section.kind == SectionKind::frequent  ? UiIcon::recent
                                                 : section.kind == SectionKind::builtin ? UiIcon::command
                                                                                        : UiIcon::bot);
}
// The row's right column: the bot's server nickname, else the section name.
Utf8View row_source(const CommandApplication& app) noexcept {
    const auto& c = *commands;
    if (c.guild && app.bot_id)
        if (const auto* member = guild_member_store_find(g_app.account_ui->guild_member_store, c.guild, app.bot_id))
            if (const auto nick = guild_member_store_nickname(*member); nick.bytes)
                return nick;
    return section_name(app);
}
// One command row: "/name", then description, source and the usage summary
// (\x1F-separated); query results and Frequently Used show the app picture.
void command_row(PopupItem& item, u32 slot, u32 entry_index, bool picture) noexcept {
    auto& c = *commands;
    const auto& entry = c.entries[entry_index];
    item = {copy_label(c.labels[slot], 512, utf8_literal("/"), entry.command->name), choose_base + entry_index,
            popup_item_enabled | popup_item_command_row, 0};
    char summary[512];
    const Utf8View options = option_summary(*entry.index, *entry.command, {summary, sizeof(summary)});
    const u32 used = copy_label(c.details[slot], 1024, entry.command->description, utf8_literal("\x1F"),
                                row_source(*entry.app))
                         .bytes;
    item.detail = {c.details[slot],
                   used + copy_label(c.details[slot] + used, 1024 - used, utf8_literal("\x1F"), options).bytes};
    if (picture && entry.app->id != kBuiltinCommandApplication && !avatar(item, entry.app))
        item.presentation = ui_menu_row_presentation(UiIcon::bot);
    else if (picture && entry.app->id == kBuiltinCommandApplication)
        item.presentation = ui_menu_row_presentation(UiIcon::command);
}
constexpr u32 section_base = 600000, pinned_base = 700000, placeholder_base = 800000;

} // namespace
namespace commands_detail {

// The unselected picker: discovery (rail, then sections from the scroll
// offset with the current section's header pinned) or query results (title
// and every ranked row, scrolled as a whole).
bool render_picker(const UiLayoutNode& surface) noexcept {
    auto& c = *commands;
    PopupItem items[32]{};
    u32 count = 0;
    float fraction = 0, rail_fraction = 0;
    const Utf8View status = c.error.bytes ? c.error : utf8_literal("No matching commands");
    if (c.discovery) {
        const u32 active = active_rail(c), divider = rail_divider(c);
        u32 first = 0;
        while (first + 1 < c.rail_count && rail_top(c, first + 1) <= c.rail_offset)
            ++first;
        rail_fraction = c.rail_count ? c.rail_offset - (rail_top(c, first) - kCommandRailLead) : 0.f;
        for (u32 r = first; r < c.rail_count && count < 12 && rail_top(c, r) < c.rail_offset + kCommandPickerHeight;
             ++r) {
            auto& item = items[count];
            item = {section_label(c.rail[r]), rail_base + r, popup_item_enabled | popup_item_command_category, 0};
            if (r == active)
                item.flags |= popup_item_checked;
            if (r == divider)
                item.flags |= popup_item_separator_before;
            section_picture(item, c.rail[r]);
            ++count;
        }
        if (!count) {
            // Built-in is always listed; this only guards an empty catalog.
            items[count++] = {utf8_literal("Built-in"), rail_base, popup_item_command_category, 0};
            items[0].presentation = ui_menu_row_presentation(UiIcon::command);
        }
    } else {
        if (!c.count && !c.query_placeholders && !c.error.bytes) {
            // Command results that match nothing are hidden.
            close_popup();
            c.rendered_generation = c.generation;
            return true;
        }
        // The title is one plain line, the query uppercased too.
        const Utf8View title = copy_label(c.labels[count], 512, utf8_literal("COMMANDS MATCHING /"),
                                          {c.query, c.query_bytes});
        for (u32 at = 0; at < title.bytes; ++at)
            if (c.labels[count][at] >= 'a' && c.labels[count][at] <= 'z')
                c.labels[count][at] = static_cast<char>(c.labels[count][at] - ('a' - 'A'));
        items[count] = {title, section_base - 1, popup_item_command_section, 0};
        ++count;
    }
    const u32 body_begin = count;
    if (c.error.bytes || (!c.count && !c.query_placeholders && !c.discovery)) {
        items[count++] = {status, choose_base - 1, popup_item_command_row, 0};
        fraction = 0;
    } else if (!c.discovery) {
        for (u32 n = 0; n < c.count && count < 31; ++n, ++count)
            command_row(items[count], count, n, true);
        // Three loading rows follow the results.
        for (u32 n = 0; n < c.query_placeholders && count < 32; ++n, ++count)
            items[count] = popup_placeholder_row(placeholder_base + n, n, popup_item_command_row);
        fraction = minimum(c.offset, maximum(0.f, body_extent(c) - kCommandRowHeight));
    } else {
        // The section under the offset keeps its header pinned while any of
        // its rows remain in view.
        u32 pinned = no_row;
        for (u32 s = 0; s < c.body_count; ++s) {
            const float top = section_top(c, s);
            const float rows_end =
                top + kCommandSectionHeight + static_cast<float>(section_rows(c.body[s])) * kCommandRowHeight;
            if (c.offset > top && c.offset < rows_end)
                pinned = s;
        }
        bool started = false;
        const float bottom = c.offset + kCommandPickerHeight;
        for (u32 s = 0; s < c.body_count && count < 31; ++s) {
            const auto& section = c.body[s];
            const float top = section_top(c, s);
            if (top >= bottom)
                break;
            const float gap = s + 1 < c.body_count ? kCommandSectionGap : 0.f;
            // Each item with the gap after it; the first one past the offset starts the flow.
            const auto start = [&](float item_top, float item_end) noexcept {
                if (!started && item_end > c.offset) {
                    started = true;
                    fraction = c.offset - (item_top - kCommandListInset);
                }
                return started;
            };
            const u32 rows = section_rows(section);
            const float header_end = top + kCommandSectionHeight + (rows ? 0.f : gap);
            if (start(top, header_end)) {
                auto& item = items[count];
                item = {section_label(section), section_base + s, popup_item_command_section, 0};
                section_picture(item, section);
                ++count;
            }
            for (u32 k = 0; k < rows && count < 31; ++k) {
                const float row = top + kCommandSectionHeight + static_cast<float>(k) * kCommandRowHeight;
                if (row >= bottom)
                    break;
                if (!start(row, row + kCommandRowHeight + (k + 1 == rows ? gap : 0.f)))
                    continue;
                if (k >= section.count)
                    items[count] = popup_placeholder_row(placeholder_base + k, k, popup_item_command_row);
                else
                    command_row(items[count], count, section.first + k, section.kind == SectionKind::frequent);
                ++count;
            }
        }
        if (pinned != no_row && count < 32) {
            auto& item = items[count];
            item = {section_label(c.body[pinned]), pinned_base + pinned,
                    popup_item_command_section | popup_item_command_pinned, 0};
            section_picture(item, c.body[pinned]);
            ++count;
        }
    }
    const UiRect viewport = application_viewport_dips();
    const UiRect anchor = surface.rectangle;
    const auto result = popup_scene_open(&g_app.popup_scene, PopupKind::autocomplete, c.channel, {items, count},
                                         anchor, viewport, ui_rect_width(anchor), true, fraction, rail_fraction);
    if (!result.ok())
        return false;
    set_popup_owner(PopupOwnerKind::commands);
    g_app.popup_scene.selected_index = ~0u;
    // The thin scrollbar shows where the virtual list stands.
    g_app.popup_scene.picker_scroll_offset = c.offset;
    g_app.popup_scene.picker_scroll_extent = body_extent(c);
    if (c.rail_focus != no_row) {
        for (u32 n = 0; n < body_begin; ++n)
            if (items[n].action == rail_base + c.rail_focus)
                g_app.popup_scene.selected_index = n;
    } else if (c.keyboard_row != no_row && c.count) {
        c.keyboard_row = minimum(c.keyboard_row, c.count - 1);
        for (u32 n = body_begin; n < count; ++n)
            if (items[n].action == choose_base + c.keyboard_row && (items[n].flags & popup_item_enabled)) {
                g_app.popup_scene.selected_index = n;
                break;
            }
    }
    c.rendered_generation = c.generation;
    return true;
}

} // namespace commands_detail
} // namespace maicord::app_detail
