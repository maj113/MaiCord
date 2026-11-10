#include "maicord/app/account/game_detection.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
// The official client's streamer tools (its running-game store), "obs" being the Linux one.
constexpr Utf8View kStreamerTools = utf8_literal(
    "obs/obs.exe|obs32.exe|obs64.exe|obs|xsplit.gamecaster.exe|xsplit.core.exe|gamecaster.exe|bebo.exe|"
    "streamlabs obs.exe|streamlabs desktop.exe|twitchstudio.exe");
constexpr u8 kMagic[4] = {'M', 'C', 'G', '2'};
constexpr u8 kOldMagic[4] = {'M', 'C', 'G', '1'};

struct Entry {
    Utf8View name, arguments;
    bool launcher, exact;
};
// The next non-empty '|'-separated part of *rest.
bool next_part(Utf8View* rest, Utf8View* part) noexcept {
    while (rest->bytes) {
        u32 bytes = 0;
        while (bytes < rest->bytes && rest->data[bytes] != '|')
            ++bytes;
        *part = {rest->data, bytes};
        const u32 skip = bytes < rest->bytes ? bytes + 1 : bytes;
        *rest = {rest->data + skip, rest->bytes - skip};
        if (bytes)
            return true;
    }
    return false;
}
// The next entry of a list: [*][>]name[?arguments].
bool next_entry(Utf8View* rest, Entry* entry) noexcept {
    Utf8View text;
    if (!next_part(rest, &text))
        return false;
    entry->launcher = text.data[0] == '*';
    text = {text.data + entry->launcher, text.bytes - entry->launcher};
    u32 name = 0;
    while (name < text.bytes && text.data[name] != '?')
        ++name;
    entry->arguments = name < text.bytes ? Utf8View{text.data + name + 1, text.bytes - name - 1} : Utf8View{};
    entry->exact = name && text.data[0] == '>';
    entry->name = {text.data + entry->exact, name - entry->exact};
    return true;
}
Utf8View file_of(Utf8View path) noexcept {
    u32 start = path.bytes;
    while (start && path.data[start - 1] != '/' && path.data[start - 1] != '\\')
        --start;
    return {path.data + start, path.bytes - start};
}
// `name` ends `path` and starts at a segment boundary.
bool ends_at_segment(Utf8View path, Utf8View name) noexcept {
    if (!name.bytes || name.bytes > path.bytes)
        return false;
    const u32 at = path.bytes - name.bytes;
    return (at == 0 || path.data[at - 1] == '/') && bytes_equal(path.data + at, name.data, name.bytes);
}
bool contains(Utf8View text, Utf8View needle) noexcept {
    if (!needle.bytes || needle.bytes > text.bytes)
        return false;
    for (u32 at = 0; at + needle.bytes <= text.bytes; ++at)
        if (text.data[at] == needle.data[0] && bytes_equal(text.data + at, needle.data, needle.bytes))
            return true;
    return false;
}
bool names(const Entry& entry, const GameProcess& process) noexcept {
    return (entry.exact ? utf8_equal(file_of(process.path), entry.name) : ends_at_segment(process.path, entry.name)) &&
           (!entry.arguments.bytes || contains(process.arguments, entry.arguments));
}
// The keys of the games with an executable file named like `file`, in list order (a hash collision may name
// others: users match the entries): `at` walks them while more().
struct Named {
    const DetectableKey *at, *end;
    u32 hash;
    bool more() const noexcept { return at != end && at->hash == hash; }
};
Named named_games(const DetectableList& list, Utf8View file) noexcept {
    if (!list.header || !file.bytes)
        return {};
    const u32 hash = detectable_hash(file);
    const DetectableKey *low = list.keys, *end = low + list.header->key_count;
    for (u32 count = list.header->key_count; count;) {
        const u32 half = count / 2;
        if (low[half].hash < hash) {
            low += half + 1;
            count -= half + 1;
        } else {
            count = half;
        }
    }
    return {low, end, hash};
}
// The process's game, or nothing for a launcher, a game turned off or a blocked added program.
GameDetectionMatch match_process(const RegisteredGames& custom, const DetectableList& list,
                                 const GameProcess& process) noexcept {
    for (u32 i = 0; i < custom.count; ++i) {
        const auto& game = custom.items[i];
        if (!(game.flags & registered_game_added) ||
            !ends_at_segment(process.path, {game.executable, game.executable_bytes}))
            continue;
        if ((game.flags & registered_game_detection_off) || game_detection_blocked(list, process.path))
            return {};
        return {{game.name, game.name_bytes}, 0, process.pid, i, true, true};
    }
    for (Named named = named_games(list, file_of(process.path)); named.more(); ++named.at) {
        const auto game = detectable_list_game(list, named.at->game);
        bool hidden = false; // a launcher, or a game turned off
        if (!game_executables_match(game.executables, process, &hidden))
            continue;
        for (u32 k = 0; !hidden && k < custom.count; ++k)
            hidden = custom.items[k].application_id == game.application_id &&
                     (custom.items[k].flags & registered_game_detection_off);
        return hidden ? GameDetectionMatch{}
                      : GameDetectionMatch{game.name, game.application_id, process.pid, named.at->game, false, true};
    }
    return {};
}
// Discord's exclusion patterns are regular expressions; MaiCord reads the subset of literal characters, escaped
// punctuation, `.*`, a leading `^` and a trailing `$`, matched as a glob. A piece is a literal byte (lowercased)
// or one of these.
constexpr u32 kPatternEnd = 256, kPatternWildcard = 257, kPatternAnchor = 258, kPatternBad = 259;
u32 pattern_next(Utf8View pattern, u32* at) noexcept {
    if (*at == pattern.bytes)
        return kPatternEnd;
    char c = pattern.data[(*at)++];
    const bool last = *at == pattern.bytes;
    if (c == '$' && last)
        return kPatternAnchor;
    if (c == '.' && !last && pattern.data[*at] == '*') {
        ++*at;
        return kPatternWildcard;
    }
    if (c == '\\' && !last && !ascii_is_alnum(pattern.data[*at])) {
        c = pattern.data[(*at)++];
    } else {
        for (const char* syntax = ".[]()*+?{}|^$\\"; *syntax; ++syntax)
            if (c == *syntax)
                return kPatternBad;
    }
    return static_cast<u8>(ascii_lower(c));
}
bool valid_name(Utf8View name) noexcept {
    return name.bytes && name.bytes <= kRegisteredGameNameBytes && utf8_validate(name).valid;
}
// A free entry, or the detected game played longest ago (added programs and games turned off stay).
u32 take_slot(RegisteredGames* games) noexcept {
    if (games->count < kRegisteredGameMaximum)
        return games->count++;
    u32 oldest = kRegisteredGameNone;
    for (u32 i = 0; i < games->count; ++i) {
        const auto& game = games->items[i];
        if (!(game.flags & (registered_game_added | registered_game_detection_off)) &&
            (oldest == kRegisteredGameNone || game.last_played_ms < games->items[oldest].last_played_ms))
            oldest = i;
    }
    return oldest;
}
} // namespace

u32 detectable_hash(Utf8View file) noexcept {
    u32 hash = 2166136261u;
    for (u32 i = 0; i < file.bytes; ++i)
        hash = (hash ^ static_cast<u8>(file.data[i])) * 16777619u;
    return hash;
}
bool detectable_pattern_supported(Utf8View pattern) noexcept {
    u32 at = pattern.bytes && pattern.data[0] == '^';
    bool literal = false;
    for (u32 piece; (piece = pattern_next(pattern, &at)) != kPatternEnd;) {
        if (piece == kPatternBad)
            return false;
        literal |= piece < 256;
    }
    return literal;
}
bool detectable_pattern_match(Utf8View pattern, Utf8View text) noexcept {
    if (!detectable_pattern_supported(pattern))
        return false;
    // `.*` is a star: one leads the pattern without `^`, and `$` only matters where the pattern ends in a literal.
    const bool anchored = pattern.data[0] == '^';
    u32 at = anchored, in = 0, star_at = 0, star_in = 0;
    bool star = !anchored, tail = false;
    for (;;) {
        const u32 piece = pattern_next(pattern, &at);
        if (piece == kPatternWildcard) {
            star = true;
            star_at = at;
            star_in = in;
        } else if (piece == kPatternAnchor) {
            tail = true;
        } else if (piece == kPatternEnd ? !tail || in == text.bytes
                                        : in < text.bytes && static_cast<u8>(text.data[in]) == piece) {
            if (piece == kPatternEnd)
                return true;
            ++in;
        } else if (!star || star_in == text.bytes) {
            return false;
        } else {
            at = star_at;
            in = ++star_in;
        }
    }
}

DetectableGame detectable_list_game(const DetectableList& list, u32 index) noexcept {
    const auto& row = list.games[index];
    return {{list.text + row.name, row.name_bytes}, {list.text + row.executables, row.executables_bytes},
            row.application_id};
}
bool game_executables_match(Utf8View executables, const GameProcess& process, bool* launcher) noexcept {
    bool game = false, launch = false;
    Entry entry;
    for (Utf8View rest = executables; !game && next_entry(&rest, &entry);)
        if (names(entry, process))
            (entry.launcher ? launch : game) = true;
    if (launcher)
        *launcher = !game && launch;
    return game || launch;
}
bool detectable_list_names(const DetectableList& list, const GameProcess& process) noexcept {
    for (Named named = named_games(list, file_of(process.path)); named.more(); ++named.at)
        if (game_executables_match(detectable_list_game(list, named.at->game).executables, process, nullptr))
            return true;
    return false;
}
bool game_detection_blocked(const DetectableList& list, Utf8View path) noexcept {
    if (!list.header)
        return false;
    const char* exclusions = list.text + list.header->games_text_bytes;
    Utf8View rest{exclusions, list.header->exclusions_bytes};
    Entry entry;
    while (next_entry(&rest, &entry))
        if (contains(path, entry.name))
            return true;
    // Patterns are whole '|'-separated texts (the build refuses one holding '|').
    rest = {exclusions + list.header->exclusions_bytes, list.header->patterns_bytes};
    Utf8View pattern;
    while (next_part(&rest, &pattern))
        if (detectable_pattern_match(pattern, path))
            return true;
    return false;
}
bool game_detection_streamer_tool(const GameProcess& process) noexcept {
    return game_executables_match(kStreamerTools, process, nullptr);
}
bool game_detection_normalize(Utf8View path, char* output, u8* bytes) noexcept {
    if (!path.bytes || !utf8_validate(path).valid)
        return false;
    u32 start = 0;
    if (path.bytes > kRegisteredGameExecutableBytes) {
        start = path.bytes - kRegisteredGameExecutableBytes;
        while (start < path.bytes && path.data[start - 1] != '/' && path.data[start - 1] != '\\')
            ++start;
        if (start == path.bytes)
            return false;
    }
    for (u32 i = start; i < path.bytes; ++i) {
        const char c = path.data[i];
        if (static_cast<u8>(c) < 0x20)
            return false;
        output[i - start] = c == '\\' ? '/' : ascii_lower(c);
    }
    *bytes = static_cast<u8>(path.bytes - start);
    return true;
}
u32 game_detection_display_name(Utf8View path, Slice<char> output) noexcept {
    const Utf8View file = file_of(path);
    if (!file.bytes)
        return 0;
    u32 bytes = file.bytes;
    if (bytes > 4 && file.data[bytes - 4] == '.' && ascii_lower(file.data[bytes - 3]) == 'e' &&
        ascii_lower(file.data[bytes - 2]) == 'x' && ascii_lower(file.data[bytes - 1]) == 'e')
        bytes -= 4;
    bytes = utf8_copy_truncated(output.data, output.count, {file.data, bytes});
    if (bytes && output.data[0] >= 'a' && output.data[0] <= 'z')
        output.data[0] = static_cast<char>(output.data[0] - 'a' + 'A');
    return bytes;
}
bool registered_games_add(RegisteredGames* games, Utf8View name, Utf8View path) noexcept {
    char executable[kRegisteredGameExecutableBytes];
    u8 executable_bytes = 0;
    if (!valid_name(name) || !game_detection_normalize(path, executable, &executable_bytes))
        return false;
    for (u32 i = 0; i < games->count; ++i)
        if ((games->items[i].flags & registered_game_added) &&
            utf8_equal({games->items[i].executable, games->items[i].executable_bytes}, {executable, executable_bytes}))
            return false;
    const u32 slot = take_slot(games);
    if (slot == kRegisteredGameNone)
        return false;
    auto& game = games->items[slot];
    reset_value(&game);
    copy_bytes(game.name, name.data, name.bytes);
    copy_bytes(game.executable, executable, executable_bytes);
    game.name_bytes = static_cast<u8>(name.bytes);
    game.executable_bytes = executable_bytes;
    game.flags = registered_game_added;
    return true;
}
bool registered_games_remove(RegisteredGames* games, u32 index) noexcept {
    if (index >= games->count)
        return false;
    copy_bytes(&games->items[index], &games->items[index + 1], (games->count - index - 1) * sizeof(RegisteredGame));
    reset_value(&games->items[--games->count]);
    return true;
}
u32 registered_games_seen(RegisteredGames* games, Utf8View name, u64 application_id) noexcept {
    if (!application_id || !name.bytes)
        return kRegisteredGameNone;
    for (u32 i = 0; i < games->count; ++i)
        if (games->items[i].application_id == application_id)
            return i;
    const u32 slot = take_slot(games);
    if (slot == kRegisteredGameNone)
        return slot;
    auto& game = games->items[slot];
    reset_value(&game);
    game.application_id = application_id;
    game.name_bytes = static_cast<u8>(utf8_copy_truncated(game.name, kRegisteredGameNameBytes, name));
    return slot;
}
GameDetectionMatch game_detection_select(const RegisteredGames& custom, const DetectableList& list,
                                         ConstSlice<GameProcess> processes, u32 previous_pid,
                                         u32 foreground_pid) noexcept {
    GameDetectionMatch best{};
    u32 best_rank = 0, best_order = ~0u;
    for (u32 i = 0; i < processes.count; ++i) {
        const auto& process = processes[i];
        if (!process.windowed)
            continue;
        const auto match = match_process(custom, list, process);
        if (!match.found)
            continue;
        const u32 rank = foreground_pid && process.pid == foreground_pid ? 3
                         : previous_pid && process.pid == previous_pid   ? 2
                                                                         : 1;
        const u32 order = (match.custom ? 0 : 0x10000) + match.index;
        if (rank > best_rank || (rank == best_rank && order < best_order)) {
            best = match;
            best_rank = rank;
            best_order = order;
        }
    }
    return best;
}
namespace {
// What the entries of `executables` naming `file` add to a candidate.
void note_candidate(Utf8View executables, Utf8View file, GameCandidate* candidate) noexcept {
    Entry entry;
    for (Utf8View rest = executables; next_entry(&rest, &entry);)
        if (utf8_equal(file_of(entry.name), file)) {
            candidate->named = true;
            candidate->arguments |= entry.arguments.bytes != 0;
        }
}
} // namespace

GameCandidate game_detection_candidate(const RegisteredGames& custom, const DetectableList& list,
                                       Utf8View file) noexcept {
    GameCandidate candidate{};
    for (Named named = named_games(list, file); named.more(); ++named.at)
        note_candidate(detectable_list_game(list, named.at->game).executables, file, &candidate);
    note_candidate(kStreamerTools, file, &candidate);
    for (u32 i = 0; i < custom.count && !candidate.named; ++i)
        candidate.named = (custom.items[i].flags & registered_game_added) &&
                          utf8_equal(file_of({custom.items[i].executable, custom.items[i].executable_bytes}), file);
    return candidate;
}
u32 game_detection_arguments(const DetectableList& list, Utf8View file, Utf8View command_line,
                              Slice<char> output) noexcept {
    u32 used = 0;
    Entry entry;
    for (Named named = named_games(list, file); named.more(); ++named.at)
        for (Utf8View rest = detectable_list_game(list, named.at->game).executables; next_entry(&rest, &entry);) {
            const Utf8View argument = entry.arguments;
            if (argument.bytes && argument.bytes < output.count - used && utf8_equal(file_of(entry.name), file) &&
                !contains({output.data, used}, argument) && contains(command_line, argument)) {
                copy_bytes(output.data + used, argument.data, argument.bytes);
                used += argument.bytes;
                output.data[used++] = '\n';
            }
        }
    return used;
}
u32 registered_games_encode(const RegisteredGames& games, Slice<u8> output) noexcept {
    // The header, then each entry's 19 fixed bytes, name and executable.
    u32 bytes = 5;
    for (u32 i = 0; i < games.count; ++i)
        bytes += 19 + games.items[i].name_bytes + games.items[i].executable_bytes;
    if (bytes > output.count)
        return 0;
    u8* at = output.data;
    copy_bytes(at, kMagic, 4);
    at[4] = static_cast<u8>(games.count);
    at += 5;
    // application_id and last_played_ms, adjacent in the entry, are the record's two little-endian u64s.
    static_assert(__builtin_offsetof(RegisteredGame, application_id) == 0 &&
                  __builtin_offsetof(RegisteredGame, last_played_ms) == 8);
    for (u32 i = 0; i < games.count; ++i) {
        const auto& g = games.items[i];
        *at++ = g.flags;
        copy_bytes(at, &g, 16);
        at += 16;
        *at++ = g.name_bytes;
        copy_bytes(at, g.name, g.name_bytes);
        at += g.name_bytes;
        *at++ = g.executable_bytes;
        copy_bytes(at, g.executable, g.executable_bytes);
        at += g.executable_bytes;
    }
    return bytes;
}
bool registered_games_decode(ConstSlice<u8> input, RegisteredGames* output) noexcept {
    if (input.count < 5 || input.data[4] > kRegisteredGameMaximum)
        return false;
    const bool old = bytes_equal(input.data, kOldMagic, 4);
    if (!old && !bytes_equal(input.data, kMagic, 4))
        return false;
    RegisteredGames& games = *output;
    reset_value(output);
    u32 at = 5;
    const auto text = [&](Utf8View* value) noexcept {
        if (at >= input.count || input.data[at] > input.count - at - 1)
            return false;
        *value = {reinterpret_cast<const char*>(input.data + at + 1), input.data[at]};
        at += 1 + input.data[at];
        return true;
    };
    for (u32 i = 0; i < input.data[4]; ++i) {
        u8 flags = registered_game_added;
        u64 application_id = 0, last_played_ms = 0;
        if (!old) {
            if (input.count - at < 17)
                return false;
            flags = input.data[at];
            copy_bytes(&application_id, input.data + at + 1, 8);
            copy_bytes(&last_played_ms, input.data + at + 9, 8);
            at += 17;
        }
        Utf8View name{}, executable{};
        if (!text(&name) || !text(&executable) || !valid_name(name) ||
            (flags & ~(registered_game_added | registered_game_detection_off)))
            return false;
        // Stored executables are normalized; a built-in game's entry has an application id and no executable.
        if (flags & registered_game_added) {
            if (application_id || !registered_games_add(&games, name, executable) ||
                (!old && !utf8_equal({games.items[i].executable, games.items[i].executable_bytes}, executable)))
                return false;
        } else if (executable.bytes || registered_games_seen(&games, name, application_id) != i ||
                   !utf8_equal({games.items[i].name, games.items[i].name_bytes}, name)) {
            return false;
        }
        games.items[i].flags = flags;
        games.items[i].last_played_ms = last_played_ms;
    }
    return at == input.count;
}
} // namespace maicord
