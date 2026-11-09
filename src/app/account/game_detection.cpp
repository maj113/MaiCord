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
Entry parse(Utf8View text) noexcept {
    Entry entry{};
    if (text.bytes && text.data[0] == '*') {
        entry.launcher = true;
        text = {text.data + 1, text.bytes - 1};
    }
    u32 name = 0;
    while (name < text.bytes && text.data[name] != '?')
        ++name;
    if (name < text.bytes)
        entry.arguments = {text.data + name + 1, text.bytes - name - 1};
    entry.name = {text.data, name};
    if (entry.name.bytes && entry.name.data[0] == '>') {
        entry.exact = true;
        entry.name = {entry.name.data + 1, entry.name.bytes - 1};
    }
    return entry;
}
// Calls visit(entry) for each '|'-separated entry until it returns false.
template <typename Visit> void each_entry(Utf8View list, Visit visit) noexcept {
    u32 begin = 0;
    for (u32 i = 0; i <= list.bytes; ++i)
        if (i == list.bytes || list.data[i] == '|') {
            if (i > begin && !visit(parse({list.data + begin, i - begin})))
                return;
            begin = i + 1;
        }
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
u32 find(Utf8View text, Utf8View needle) noexcept {
    if (!needle.bytes || needle.bytes > text.bytes)
        return ~0u;
    for (u32 at = 0; at + needle.bytes <= text.bytes; ++at)
        if (text.data[at] == needle.data[0] && bytes_equal(text.data + at, needle.data, needle.bytes))
            return at;
    return ~0u;
}
bool contains(Utf8View text, Utf8View needle) noexcept {
    return find(text, needle) != ~0u;
}
bool names(const Entry& entry, const GameProcess& process) noexcept {
    return (entry.exact ? utf8_equal(file_of(process.path), entry.name) : ends_at_segment(process.path, entry.name)) &&
           (!entry.arguments.bytes || contains(process.arguments, entry.arguments));
}
// Calls visit(game index) for each game of the list with an executable file named like `file`, in list order
// (a hash collision may name others: visitors match the entries), until it returns false.
template <typename Visit> void each_named(const DetectableList& list, Utf8View file, Visit visit) noexcept {
    if (!list.header || !file.bytes)
        return;
    const u32 hash = detectable_hash(file);
    u32 low = 0, high = list.header->key_count;
    while (low < high) {
        const u32 middle = (low + high) / 2;
        if (list.keys[middle].hash < hash)
            low = middle + 1;
        else
            high = middle;
    }
    for (; low < list.header->key_count && list.keys[low].hash == hash; ++low)
        if (!visit(list.keys[low].game))
            return;
}
// The process's game, or nothing for a launcher, a game turned off or a blocked added program.
GameDetectionMatch match_process(const RegisteredGames& custom, const DetectableList& list,
                                 const GameProcess& process) noexcept {
    for (u32 i = 0; i < custom.count && i < kRegisteredGameMaximum; ++i) {
        const auto& game = custom.items[i];
        if (!(game.flags & registered_game_added) ||
            !ends_at_segment(process.path, {game.executable, game.executable_bytes}))
            continue;
        if ((game.flags & registered_game_detection_off) || game_detection_blocked(list, process.path))
            return {};
        return {{game.name, game.name_bytes}, 0, process.pid, i, true, true};
    }
    GameDetectionMatch match{};
    each_named(list, file_of(process.path), [&](u32 index) noexcept {
        const auto game = detectable_list_game(list, index);
        bool launcher = false;
        if (!game_executables_match(game.executables, process, &launcher))
            return true;
        match.found = !launcher;
        for (u32 k = 0; k < custom.count && k < kRegisteredGameMaximum && match.found; ++k)
            match.found = custom.items[k].application_id != game.application_id ||
                          !(custom.items[k].flags & registered_game_detection_off);
        if (match.found)
            match = {game.name, game.application_id, process.pid, index, false, true};
        return false;
    });
    return match;
}
// One piece of a pattern: a literal run, decoded into the caller's buffer.
struct PatternParse {
    char literal[256];
    u32 starts[16], ends[16], count;
    bool anchored_start, anchored_end, valid;
};
// Discord's exclusion patterns are regular expressions; this reads the subset of literal characters, escaped
// punctuation, `.*`, a leading `^` and a trailing `$`.
PatternParse parse_pattern(Utf8View pattern) noexcept {
    PatternParse p{};
    u32 at = 0, used = 0;
    bool open = false; // a literal run is being read
    if (at < pattern.bytes && pattern.data[at] == '^')
        p.anchored_start = ++at != 0;
    bool leading = true;
    for (; at < pattern.bytes; ++at) {
        char c = pattern.data[at];
        if (c == '$' && at + 1 == pattern.bytes) {
            p.anchored_end = true;
            break;
        }
        if (c == '.' && at + 1 < pattern.bytes && pattern.data[at + 1] == '*') {
            ++at;
            if (leading)
                p.anchored_start = false;
            open = false;
            p.anchored_end = false;
            continue;
        }
        if (c == '\\' && at + 1 < pattern.bytes && !ascii_is_alnum(pattern.data[at + 1]))
            c = pattern.data[++at];
        else if (c == '.' || c == '[' || c == ']' || c == '(' || c == ')' || c == '*' || c == '+' || c == '?' ||
                 c == '{' || c == '}' || c == '|' || c == '^' || c == '$' || c == '\\')
            return {};
        if (!open) {
            if (p.count == 16)
                return {};
            p.starts[p.count] = p.ends[p.count] = used;
            ++p.count;
            open = true;
        }
        if (used == sizeof(p.literal))
            return {};
        p.literal[used++] = ascii_lower(c);
        p.ends[p.count - 1] = used;
        leading = false;
    }
    // A wildcard after the last literal unanchors the end.
    if (p.anchored_end && !open)
        p.anchored_end = false;
    p.valid = p.count != 0;
    return p;
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
    return parse_pattern(pattern).valid;
}
bool detectable_pattern_match(Utf8View pattern, Utf8View text) noexcept {
    const PatternParse p = parse_pattern(pattern);
    if (!p.valid)
        return false;
    u32 at = 0;
    for (u32 i = 0; i < p.count; ++i) {
        const Utf8View piece{p.literal + p.starts[i], p.ends[i] - p.starts[i]};
        const bool first = i == 0 && p.anchored_start, last = i + 1 == p.count && p.anchored_end;
        if (first) {
            if (piece.bytes > text.bytes || !bytes_equal(text.data, piece.data, piece.bytes) ||
                (last && piece.bytes != text.bytes))
                return false;
            at = piece.bytes;
        } else if (last) {
            if (piece.bytes > text.bytes - at ||
                !bytes_equal(text.data + text.bytes - piece.bytes, piece.data, piece.bytes))
                return false;
        } else {
            const u32 found = find({text.data + at, text.bytes - at}, piece);
            if (found == ~0u)
                return false;
            at += found + piece.bytes;
        }
    }
    return true;
}
namespace {
void put_u64(u8* output, u64 value) noexcept {
    for (u32 i = 0; i < 8; ++i)
        output[i] = static_cast<u8>(value >> (8 * i));
}
u64 get_u64(const u8* input) noexcept {
    u64 value = 0;
    for (u32 i = 0; i < 8; ++i)
        value |= static_cast<u64>(input[i]) << (8 * i);
    return value;
}
} // namespace

DetectableGame detectable_list_game(const DetectableList& list, u32 index) noexcept {
    const auto& row = list.games[index];
    return {{list.text + row.name, row.name_bytes}, {list.text + row.executables, row.executables_bytes},
            row.application_id};
}
bool game_executables_match(Utf8View executables, const GameProcess& process, bool* launcher) noexcept {
    bool game = false, launch = false;
    each_entry(executables, [&](const Entry& entry) noexcept {
        if (names(entry, process))
            (entry.launcher ? launch : game) = true;
        return !game;
    });
    if (launcher)
        *launcher = !game && launch;
    return game || launch;
}
bool detectable_list_names(const DetectableList& list, const GameProcess& process) noexcept {
    bool named = false;
    each_named(list, file_of(process.path), [&](u32 index) noexcept {
        named = game_executables_match(detectable_list_game(list, index).executables, process, nullptr);
        return !named;
    });
    return named;
}
bool game_detection_blocked(const DetectableList& list, Utf8View path) noexcept {
    if (!list.header)
        return false;
    const char* exclusions = list.text + list.header->games_text_bytes;
    bool blocked = false;
    each_entry({exclusions, list.header->exclusions_bytes}, [&](const Entry& entry) noexcept {
        blocked = contains(path, entry.name);
        return !blocked;
    });
    // Patterns are whole '|'-separated texts (the build refuses one holding '|').
    const Utf8View patterns{exclusions + list.header->exclusions_bytes, list.header->patterns_bytes};
    for (u32 begin = 0, i = 0; !blocked && i <= patterns.bytes; ++i)
        if (i == patterns.bytes || patterns.data[i] == '|') {
            blocked = i > begin && detectable_pattern_match({patterns.data + begin, i - begin}, path);
            begin = i + 1;
        }
    return blocked;
}
bool game_detection_streamer_tool(const GameProcess& process) noexcept {
    return game_executables_match(kStreamerTools, process, nullptr);
}
bool game_detection_normalize(Utf8View path, char* output, u8* bytes) noexcept {
    if (!output || !bytes || !path.data || !path.bytes || !utf8_validate(path).valid)
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
    if (!output.data || !file.bytes)
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
    RegisteredGame game{};
    if (!games || !valid_name(name) || !game_detection_normalize(path, game.executable, &game.executable_bytes))
        return false;
    for (u32 i = 0; i < games->count; ++i)
        if ((games->items[i].flags & registered_game_added) &&
            utf8_equal({games->items[i].executable, games->items[i].executable_bytes},
                       {game.executable, game.executable_bytes}))
            return false;
    const u32 slot = take_slot(games);
    if (slot == kRegisteredGameNone)
        return false;
    copy_bytes(game.name, name.data, name.bytes);
    game.name_bytes = static_cast<u8>(name.bytes);
    game.flags = registered_game_added;
    games->items[slot] = game;
    return true;
}
bool registered_games_remove(RegisteredGames* games, u32 index) noexcept {
    if (!games || index >= games->count)
        return false;
    for (u32 i = index; i + 1 < games->count; ++i)
        games->items[i] = games->items[i + 1];
    reset_value(&games->items[--games->count]);
    return true;
}
u32 registered_games_seen(RegisteredGames* games, Utf8View name, u64 application_id) noexcept {
    if (!games || !application_id || !name.bytes)
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
GameCandidate game_detection_candidate(const RegisteredGames& custom, const DetectableList& list,
                                       Utf8View file) noexcept {
    GameCandidate candidate{};
    const auto visit = [&](const Entry& entry) noexcept {
        if (utf8_equal(file_of(entry.name), file)) {
            candidate.named = true;
            candidate.arguments |= entry.arguments.bytes != 0;
        }
        return true;
    };
    each_named(list, file, [&](u32 index) noexcept {
        each_entry(detectable_list_game(list, index).executables, visit);
        return true;
    });
    each_entry(kStreamerTools, visit);
    for (u32 i = 0; i < custom.count && i < kRegisteredGameMaximum && !candidate.named; ++i)
        candidate.named = (custom.items[i].flags & registered_game_added) &&
                          utf8_equal(file_of({custom.items[i].executable, custom.items[i].executable_bytes}), file);
    return candidate;
}
u32 game_detection_arguments(const DetectableList& list, Utf8View file, Utf8View command_line,
                              Slice<char> output) noexcept {
    u32 used = 0;
    if (!output.data)
        return 0;
    each_named(list, file, [&](u32 index) noexcept {
        each_entry(detectable_list_game(list, index).executables, [&](const Entry& entry) noexcept {
            const Utf8View argument = entry.arguments;
            if (argument.bytes && argument.bytes < output.count - used && utf8_equal(file_of(entry.name), file) &&
                !contains({output.data, used}, argument) && contains(command_line, argument)) {
                copy_bytes(output.data + used, argument.data, argument.bytes);
                used += argument.bytes;
                output.data[used++] = '\n';
            }
            return true;
        });
        return true;
    });
    return used;
}
u32 registered_games_encode(const RegisteredGames& games, Slice<u8> output) noexcept {
    if (!output.data || games.count > kRegisteredGameMaximum)
        return 0;
    u32 at = 0;
    const auto put = [&](const void* data, u32 bytes) noexcept {
        if (bytes > output.count - at)
            return false;
        copy_bytes(output.data + at, data, bytes);
        at += bytes;
        return true;
    };
    const u8 count = static_cast<u8>(games.count);
    if (!put(kMagic, 4) || !put(&count, 1))
        return 0;
    for (u32 i = 0; i < games.count; ++i) {
        const auto& g = games.items[i];
        u8 numbers[17];
        numbers[0] = g.flags;
        put_u64(numbers + 1, g.application_id);
        put_u64(numbers + 9, g.last_played_ms);
        if (!put(numbers, sizeof(numbers)) || !put(&g.name_bytes, 1) || !put(g.name, g.name_bytes) ||
            !put(&g.executable_bytes, 1) || !put(g.executable, g.executable_bytes))
            return 0;
    }
    return at;
}
bool registered_games_decode(ConstSlice<u8> input, RegisteredGames* output) noexcept {
    if (!output || !input.data || input.count < 5 || input.data[4] > kRegisteredGameMaximum)
        return false;
    const bool old = bytes_equal(input.data, kOldMagic, 4);
    if (!old && !bytes_equal(input.data, kMagic, 4))
        return false;
    RegisteredGames games{};
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
            application_id = get_u64(input.data + at + 1);
            last_played_ms = get_u64(input.data + at + 9);
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
    if (at != input.count)
        return false;
    *output = games;
    return true;
}
} // namespace maicord
