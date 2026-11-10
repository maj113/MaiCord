// The spell checker on Linux (include/maicord/platform/desktop/spell_checker.hpp): Hunspell (libhunspell-1.7.so.0) or,
// without it, Enchant 2 (libenchant-2.so.2), both opened with dlopen so a missing library is "unavailable", never a
// loader failure. MAICORD_SPELLCHECK_BACKEND (hunspell | enchant) forces one; MAICORD_LIBRARY_PATH adds the directories
// that hold the libraries on an unusual install.
//
// Hunspell reads the dictionaries itself named <language>_<TERRITORY>.dic and .aff from $DICPATH, the XDG data
// directories ($XDG_DATA_HOME, $XDG_DATA_DIRS: hunspell and myspell below each), /usr/share/hunspell, /usr/share/myspell
// and the NixOS profile directories; only UTF-8 dictionaries are used. Enchant finds its own. The language is the
// locale of LC_ALL, LC_MESSAGES or LANG (de_AT.UTF-8 is "de-AT": that dictionary, else the base language's, else
// en-US), the choice the Windows checker makes with spell_checker_choose_language.
//
// "Add" is Enchant's personal word list, or for Hunspell one word per line in <data directory>/spellcheck/<tag>.txt,
// read when the dictionary opens; "Ignore" lasts for the checker.
#include "maicord/platform/desktop/spell_checker.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/heap.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/os/file.hpp"
#include "maicord/core/test_hook.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {

constexpr u32 kPathBytes = 512;
constexpr u32 kNameBytes = 32;       // "de_DE"
constexpr u32 kDirectoryLimit = 40;
constexpr u32 kPersonalBytes = 64 * 1024;

// Hunspell's C interface (hunspell/hunspell.h, version 1.7).
struct HunspellApi {
    void* (*create)(const char* affix_path, const char* dictionary_path) noexcept;
    void (*destroy)(void* handle) noexcept;
    int (*spell)(void* handle, const char* word) noexcept;
    int (*suggest)(void* handle, char*** list, const char* word) noexcept;
    int (*add)(void* handle, const char* word) noexcept;
    void (*free_list)(void* handle, char*** list, int count) noexcept;
    char* (*encoding)(void* handle) noexcept;
};

// Enchant 2's (enchant.h).
struct EnchantApi {
    void* (*broker_init)() noexcept;
    void (*broker_free)(void* broker) noexcept;
    void* (*request_dict)(void* broker, const char* tag) noexcept;
    void (*free_dict)(void* broker, void* dict) noexcept;
    int (*dict_exists)(void* broker, const char* tag) noexcept;
    int (*check)(void* dict, const char* word, long length) noexcept;
    char** (*suggest)(void* dict, const char* word, long length, usize* count) noexcept;
    void (*free_list)(void* dict, char** list) noexcept;
    void (*add)(void* dict, const char* word, long length) noexcept;
    void (*add_to_session)(void* dict, const char* word, long length) noexcept;
};

enum class Kind : u8 { hunspell, enchant };

struct Backend {
    SharedLibrary library;
    Kind kind;
    HunspellApi hunspell;
    EnchantApi enchant;
    void* broker;                // Enchant
    char personal[kPathBytes];   // Hunspell: the file "Add" appends to; empty when the data directory is unavailable
};

bool equal_ignore_case(const char* a, const char* b) noexcept {
    return ascii_equal_ignore_case({a, c_string_length(a)}, {b, c_string_length(b)});
}

int compare_bytes(const char* a, const char* b, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (a[i] != b[i])
            return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[i]) ? -1 : 1;
    return 0;
}

bool open_hunspell(Backend* backend) noexcept {
    // Only the tests name another soname (the missing-library path).
    const char* soname = test_hook("MAICORD_HUNSPELL_LIBRARY");
    if (!soname || !*soname)
        soname = "libhunspell-1.7.so.0";
    if (!shared_library_open(&backend->library, soname))
        return false;
    // The functions of HunspellApi, in member order.
    static constexpr char kNames[] =
        "Hunspell_create\0Hunspell_destroy\0Hunspell_spell\0Hunspell_suggest\0Hunspell_add\0Hunspell_free_list\0"
        "Hunspell_get_dic_encoding\0";
    static_assert(shared_library_name_count(kNames) * sizeof(void*) == sizeof(HunspellApi));
    if (shared_library_resolve(backend->library, &backend->hunspell, sizeof(HunspellApi), kNames)) {
        shared_library_close(&backend->library);
        return false;
    }
    backend->kind = Kind::hunspell;
    return true;
}

bool open_enchant(Backend* backend) noexcept {
    const char* soname = test_hook("MAICORD_ENCHANT_LIBRARY");
    if (!soname || !*soname)
        soname = "libenchant-2.so.2";
    if (!shared_library_open(&backend->library, soname))
        return false;
    // The functions of EnchantApi, in member order.
    static constexpr char kNames[] =
        "enchant_broker_init\0enchant_broker_free\0enchant_broker_request_dict\0enchant_broker_free_dict\0"
        "enchant_broker_dict_exists\0enchant_dict_check\0enchant_dict_suggest\0enchant_dict_free_string_list\0"
        "enchant_dict_add\0enchant_dict_add_to_session\0";
    static_assert(shared_library_name_count(kNames) * sizeof(void*) == sizeof(EnchantApi));
    void* broker = nullptr;
    if (!shared_library_resolve(backend->library, &backend->enchant, sizeof(EnchantApi), kNames))
        broker = backend->enchant.broker_init();
    if (!broker) {
        shared_library_close(&backend->library);
        return false;
    }
    backend->broker = broker;
    backend->kind = Kind::enchant;
    return true;
}

// The directories that may hold Hunspell dictionaries, in the order searched.
struct Directories {
    char path[kDirectoryLimit][kPathBytes];
    u32 count;

    void add(const char* directory, u32 length, const char* suffix = "") noexcept {
        while (length > 1 && directory[length - 1] == '/')
            --length;
        const u32 suffix_length = c_string_length(suffix);
        if (count >= kDirectoryLimit || length == 0 || length + suffix_length >= kPathBytes)
            return;
        for (u32 i = 0; i < count; ++i) {
            // Skip a directory already listed.
            if (c_string_length(path[i]) == length + suffix_length && bytes_equal(path[i], directory, length) &&
                bytes_equal(path[i] + length, suffix, suffix_length))
                return;
        }
        copy_bytes(path[count], directory, length);
        copy_bytes(path[count] + length, suffix, suffix_length + 1);
        ++count;
    }
    void add(const char* directory, const char* suffix = "") noexcept {
        add(directory, c_string_length(directory), suffix);
    }
};

void list_directories(Directories* list) noexcept {
    list->count = 0;
    if (const char* dicpath = environment_get("DICPATH")) {
        for (u32 start = 0; dicpath[start];) {
            u32 end = start;
            while (dicpath[end] && dicpath[end] != ':')
                ++end;
            list->add(dicpath + start, end - start);
            start = dicpath[end] ? end + 1 : end;
        }
    }
    const char* home = environment_get("HOME");
    const char* data_home = environment_get("XDG_DATA_HOME");
    if (data_home && data_home[0] == '/') {
        list->add(data_home, "/hunspell");
    } else if (home && home[0] == '/') {
        list->add(home, "/.local/share/hunspell");
    }
    const char* data_dirs = environment_get("XDG_DATA_DIRS");
    if (!data_dirs || !*data_dirs)
        data_dirs = "/usr/local/share:/usr/share";
    for (u32 start = 0; data_dirs[start];) {
        u32 end = start;
        while (data_dirs[end] && data_dirs[end] != ':')
            ++end;
        if (data_dirs[start] == '/') {
            list->add(data_dirs + start, end - start, "/hunspell");
            list->add(data_dirs + start, end - start, "/myspell");
        }
        start = data_dirs[end] ? end + 1 : end;
    }
    static constexpr const char* fixed[] = {"/usr/share/hunspell",
                                            "/usr/share/myspell",
                                            "/usr/share/myspell/dicts",
                                            "/usr/local/share/hunspell",
                                            "/run/current-system/sw/share/hunspell",
                                            "/run/current-system/sw/share/myspell",
                                            "/nix/var/nix/profiles/default/share/hunspell"};
    for (const char* directory : fixed)
        list->add(directory);
    if (home && home[0] == '/')
        list->add(home, "/.nix-profile/share/hunspell");
    if (const char* user = environment_get("USER")) {
        char profile[kPathBytes];
        const u32 length = c_string_length(user);
        static constexpr char prefix[] = "/etc/profiles/per-user/";
        static constexpr char suffix[] = "/share/hunspell";
        if (length != 0 && sizeof(prefix) + length + sizeof(suffix) < kPathBytes) {
            copy_bytes(profile, prefix, sizeof(prefix) - 1);
            copy_bytes(profile + sizeof(prefix) - 1, user, length);
            copy_bytes(profile + sizeof(prefix) - 1 + length, suffix, sizeof(suffix));
            list->add(profile);
        }
    }
}

bool readable(const char* path) noexcept {
    const long file = sys_open(path, kOpenReadOnly);
    if (syscall_failed(file))
        return false;
    sys_close(static_cast<int>(file));
    return true;
}

// "<directory>/<name>.<extension>" into `path`.
bool join(char (&path)[kPathBytes], const char* directory, const char* name, u32 name_length,
          const char* extension) noexcept {
    const u32 d = c_string_length(directory), e = c_string_length(extension);
    if (d + 1 + name_length + e >= kPathBytes)
        return false;
    copy_bytes(path, directory, d);
    path[d] = '/';
    copy_bytes(path + d + 1, name, name_length);
    copy_bytes(path + d + 1 + name_length, extension, e + 1);
    return true;
}

// The dictionary files of `name` ("de_DE") in one directory.
bool find_in_directory(const char* directory, const char* name, u32 name_length, char (&affix)[kPathBytes],
                       char (&dictionary)[kPathBytes]) noexcept {
    return join(dictionary, directory, name, name_length, ".dic") && join(affix, directory, name, name_length, ".aff") &&
           readable(dictionary) && readable(affix);
}

// A language without a territory ("de"): de_DE when it exists, else the first de_*.dic of the directory.
bool find_base_in_directory(const char* directory, const char* name, u32 name_length, char (&affix)[kPathBytes],
                            char (&dictionary)[kPathBytes]) noexcept {
    char same[kNameBytes];
    if (name_length * 2 + 1 < kNameBytes) {
        copy_bytes(same, name, name_length);
        same[name_length] = '_';
        for (u32 i = 0; i < name_length; ++i)
            same[name_length + 1 + i] = static_cast<char>(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
        same[name_length * 2 + 1] = 0;
        if (find_in_directory(directory, same, name_length * 2 + 1, affix, dictionary))
            return true;
    }
    wchar_t wide[kPathBytes];
    DirIter iterator{};
    if (!path_from_utf8(directory, wide, kPathBytes) || !dir_open(&iterator, wide))
        return false;
    char best[kNameBytes] = {};
    u32 best_length = 0;
    DirEntry entry{};
    while (dir_next(&iterator, &entry)) {
        char narrow[kNameBytes];
        u32 length = 0;
        while (entry.name[length] && entry.name[length] < 128 && length < kNameBytes - 1) {
            narrow[length] = static_cast<char>(entry.name[length]);
            ++length;
        }
        narrow[length] = 0;
        // <name>_<territory>.dic
        if (entry.name[length] || length < name_length + 6 || !bytes_equal(narrow, name, name_length) ||
            narrow[name_length] != '_' || !bytes_equal(narrow + length - 4, ".dic", 4))
            continue;
        const u32 stem = length - 4;
        if (best_length == 0 || stem < best_length || (stem == best_length && compare_bytes(narrow, best, stem) < 0)) {
            copy_bytes(best, narrow, stem);
            best[stem] = 0;
            best_length = stem;
        }
    }
    dir_close(&iterator);
    return best_length != 0 && find_in_directory(directory, best, best_length, affix, dictionary);
}

// The dictionary a name ("de_DE", or "de" for any German one) maps to, searching every directory.
bool find_dictionary(const char* name, char (&affix)[kPathBytes], char (&dictionary)[kPathBytes]) noexcept {
    const u32 length = c_string_length(name);
    if (length == 0 || length >= kNameBytes)
        return false;
    bool base = true;
    for (u32 i = 0; i < length; ++i)
        base = base && name[i] != '_';
    Directories directories;
    list_directories(&directories);
    for (u32 i = 0; i < directories.count; ++i) {
        if (find_in_directory(directories.path[i], name, length, affix, dictionary))
            return true;
        if (base && find_base_in_directory(directories.path[i], name, length, affix, dictionary))
            return true;
    }
    return false;
}

// A language tag ("de-AT") as a dictionary name ("de_AT"); false when it holds anything but letters, digits, '-' and
// '_' or is too long.
bool tag_to_name(const wchar_t* tag, char (&name)[kNameBytes]) noexcept {
    u32 length = 0;
    for (; tag[length]; ++length) {
        const wchar_t c = tag[length];
        if (length + 1 >= kNameBytes)
            return false;
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))
            name[length] = static_cast<char>(c);
        else if (c == L'-' || c == L'_')
            name[length] = '_';
        else
            return false;
    }
    name[length] = 0;
    return length != 0;
}

bool backend_supports(void* context, const wchar_t* tag) noexcept {
    auto* backend = static_cast<Backend*>(context);
    char name[kNameBytes];
    if (!tag_to_name(tag, name))
        return false;
    if (backend->kind == Kind::enchant)
        return backend->enchant.dict_exists(backend->broker, name) != 0;
    char affix[kPathBytes], dictionary[kPathBytes];
    return find_dictionary(name, affix, dictionary);
}

// The locale of LC_ALL, LC_MESSAGES or LANG as a language tag ("de_AT.UTF-8" is "de-AT"); empty for none, "C" and
// "POSIX".
u32 environment_locale(wchar_t (&tag)[kSpellCheckerLanguageChars]) noexcept {
    static constexpr const char* variables[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    const char* value = nullptr;
    for (const char* variable : variables) {
        value = environment_get(variable);
        if (value && *value)
            break;
        value = nullptr;
    }
    tag[0] = 0;
    if (!value)
        return 0;
    u32 length = 0;
    while (value[length] && value[length] != '.' && value[length] != '@' && length < kSpellCheckerLanguageChars - 1) {
        const char c = value[length];
        tag[length] = c == '_' ? L'-' : static_cast<wchar_t>(static_cast<unsigned char>(c));
        ++length;
    }
    tag[length] = 0;
    // "C" and "POSIX" name no language.
    const bool c_locale = (length == 1 && tag[0] == L'C') ||
                          (length == 5 && tag[0] == L'P' && tag[1] == L'O' && tag[2] == L'S' && tag[3] == L'I' &&
                           tag[4] == L'X');
    if (c_locale)
        length = tag[0] = 0;
    return length;
}

bool valid_word(Utf8View word, char (&output)[kSpellCheckerWordBytes + 1]) noexcept {
    if (!word.data || !word.bytes || word.bytes > kSpellCheckerWordBytes)
        return false;
    for (u32 i = 0; i < word.bytes; ++i)
        if (word.data[i] == 0)
            return false;
    if (!utf8_validate(word).valid)
        return false;
    copy_bytes(output, word.data, word.bytes);
    output[word.bytes] = 0;
    return true;
}

// The words of the personal list into the open Hunspell dictionary.
void load_personal_words(Backend* backend, void* handle) noexcept {
    if (!backend->personal[0])
        return;
    const long file = sys_open(backend->personal, kOpenReadOnly);
    if (syscall_failed(file))
        return;
    auto* text = static_cast<char*>(heap_allocate(kPersonalBytes + 1));
    u32 size = 0;
    if (text) {
        for (;;) {
            const long got = sys_read(static_cast<int>(file), text + size, kPersonalBytes - size);
            if (syscall_failed(got) || got == 0)
                break;
            size += static_cast<u32>(got);
            if (size == kPersonalBytes)
                break;
        }
        text[size] = 0;
    }
    sys_close(static_cast<int>(file));
    for (u32 start = 0; text && start < size;) {
        u32 end = start;
        while (end < size && text[end] != '\n')
            ++end;
        char word[kSpellCheckerWordBytes + 1];
        if (valid_word({text + start, end - start}, word))
            backend->hunspell.add(handle, word);
        start = end + 1;
    }
    if (text)
        heap_free(text);
}

// <data directory>/spellcheck/<tag>.txt, relative to the data directory, for the language of `spell`.
void personal_relative(const SpellChecker& spell, wchar_t (&relative)[kSpellCheckerLanguageChars + 24]) noexcept {
    static constexpr char prefix[] = "spellcheck/";
    static constexpr char suffix[] = ".txt";
    u32 length = 0;
    for (u32 i = 0; i < sizeof(prefix) - 1; ++i)
        relative[length++] = static_cast<wchar_t>(prefix[i]);
    for (u32 i = 0; spell.language[i]; ++i)
        relative[length++] = spell.language[i];
    for (u32 i = 0; i < sizeof(suffix); ++i)
        relative[length++] = static_cast<wchar_t>(suffix[i]);
}

// The personal list of a language, read when the dictionary opens and created beside the data by the first "Add".
void personal_path(Backend* backend, const SpellChecker& spell) noexcept {
    backend->personal[0] = 0;
    wchar_t relative[kSpellCheckerLanguageChars + 24];
    personal_relative(spell, relative);
    wchar_t absolute[kPathBytes];
    if (data_path(absolute, kPathBytes, relative, false).ok())
        path_to_utf8(absolute, backend->personal, kPathBytes);
}

bool append_personal_word(const Backend& backend, const SpellChecker& spell, const char* word) noexcept {
    if (!backend.personal[0])
        return false;
    wchar_t relative[kSpellCheckerLanguageChars + 24];
    personal_relative(spell, relative);
    wchar_t absolute[kPathBytes];
    // The data and spellcheck directories are created here, with the first word.
    if (!data_path(absolute, kPathBytes, relative, true).ok())
        return false;
    const long file = sys_open(backend.personal, kOpenWriteOnly | kOpenCreate | kOpenAppend, 0600);
    if (syscall_failed(file))
        return false;
    const u32 bytes = c_string_length(word);
    char line[kSpellCheckerWordBytes + 2];
    copy_bytes(line, word, bytes);
    line[bytes] = '\n';
    const long written = sys_write(static_cast<int>(file), line, bytes + 1);
    sys_close(static_cast<int>(file));
    return !syscall_failed(written) && written == static_cast<long>(bytes + 1);
}

void release_backend(SpellChecker* spell) noexcept {
    auto* backend = static_cast<Backend*>(spell->factory);
    if (!backend)
        return;
    if (spell->checker) {
        if (backend->kind == Kind::hunspell)
            backend->hunspell.destroy(spell->checker);
        else
            backend->enchant.free_dict(backend->broker, spell->checker);
    }
    if (backend->kind == Kind::enchant && backend->broker)
        backend->enchant.broker_free(backend->broker);
    shared_library_close(&backend->library);
    heap_free(backend);
    spell->factory = nullptr;
    spell->checker = nullptr;
}

// Opens the dictionary of spell->language on the loaded library.
bool open_dictionary(SpellChecker* spell, Backend* backend) noexcept {
    char name[kNameBytes];
    if (!tag_to_name(spell->language, name))
        return false;
    if (backend->kind == Kind::enchant) {
        spell->checker = backend->enchant.request_dict(backend->broker, name);
        return spell->checker != nullptr;
    }
    char affix[kPathBytes], dictionary[kPathBytes];
    if (!find_dictionary(name, affix, dictionary))
        return false;
    void* handle = backend->hunspell.create(affix, dictionary);
    if (!handle)
        return false;
    // The words cross the interface as UTF-8; a dictionary in another encoding would need converting.
    const char* encoding = backend->hunspell.encoding(handle);
    if (!encoding || !equal_ignore_case(encoding, "UTF-8")) {
        backend->hunspell.destroy(handle);
        return false;
    }
    spell->checker = handle;
    personal_path(backend, *spell);
    load_personal_words(backend, handle);
    return true;
}

} // namespace

bool spell_checker_open(SpellChecker* spell) noexcept {
    if (!spell)
        return false;
    if (spell->checker)
        return true;
    if (spell->attempted)
        return false;
    spell->attempted = true;
    const char* forced = environment_get("MAICORD_SPELLCHECK_BACKEND");
    const bool only_hunspell = forced && equal_ignore_case(forced, "hunspell");
    const bool only_enchant = forced && equal_ignore_case(forced, "enchant");
    const bool try_hunspell = !only_enchant;
    const bool try_enchant = !only_hunspell;
    wchar_t locale[kSpellCheckerLanguageChars];
    environment_locale(locale);
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        const Kind kind = attempt == 0 ? Kind::hunspell : Kind::enchant;
        if (kind == Kind::hunspell ? !try_hunspell : !try_enchant)
            continue;
        auto* backend = static_cast<Backend*>(heap_allocate(sizeof(Backend), true));
        if (!backend)
            return false;
        if (!(kind == Kind::hunspell ? open_hunspell(backend) : open_enchant(backend))) {
            heap_free(backend);
            continue;
        }
        spell->factory = backend;
        if (spell_checker_choose_language(locale, backend_supports, backend,
                                          {spell->language, kSpellCheckerLanguageChars}) &&
            open_dictionary(spell, backend))
            return true;
        release_backend(spell);
        spell->language[0] = 0;
    }
    return false;
}

SpellCheckerVerdict spell_checker_check(SpellChecker* spell, Utf8View word) noexcept {
    char text[kSpellCheckerWordBytes + 1];
    if (!valid_word(word, text) || !spell_checker_open(spell))
        return SpellCheckerVerdict::unavailable;
    auto* backend = static_cast<Backend*>(spell->factory);
    if (backend->kind == Kind::hunspell)
        return backend->hunspell.spell(spell->checker, text) != 0 ? SpellCheckerVerdict::correct
                                                                  : SpellCheckerVerdict::misspelled;
    const int result = backend->enchant.check(spell->checker, text, -1);
    if (result < 0)
        return SpellCheckerVerdict::unavailable;
    return result == 0 ? SpellCheckerVerdict::correct : SpellCheckerVerdict::misspelled;
}

u32 spell_checker_suggest(SpellChecker* spell, Utf8View word, Slice<char> storage, Slice<Utf8View> output) noexcept {
    char text[kSpellCheckerWordBytes + 1];
    if (!storage.data || !output.data || !output.count || !valid_word(word, text) || !spell_checker_open(spell))
        return 0;
    auto* backend = static_cast<Backend*>(spell->factory);
    char** list = nullptr;
    usize available = 0;
    if (backend->kind == Kind::hunspell) {
        const int count = backend->hunspell.suggest(spell->checker, &list, text);
        available = count > 0 && list ? static_cast<usize>(count) : 0;
    } else {
        list = backend->enchant.suggest(spell->checker, text, -1, &available);
        if (!list)
            available = 0;
    }
    u32 count = 0, used = 0;
    for (usize i = 0; i < available && count < output.count; ++i) {
        const u32 bytes = c_string_length(list[i]);
        // Empty, not UTF-8 or too long for the remaining storage: skipped, as the Windows checker does.
        if (bytes == 0 || bytes > storage.count - used || !utf8_validate({list[i], bytes}).valid)
            continue;
        copy_bytes(storage.data + used, list[i], bytes);
        output[count++] = {storage.data + used, bytes};
        used += bytes;
    }
    if (backend->kind == Kind::hunspell) {
        if (list)
            backend->hunspell.free_list(spell->checker, &list, static_cast<int>(available));
    } else if (list) {
        backend->enchant.free_list(spell->checker, list);
    }
    return count;
}

bool spell_checker_add(SpellChecker* spell, Utf8View word) noexcept {
    char text[kSpellCheckerWordBytes + 1];
    if (!valid_word(word, text) || !spell_checker_open(spell))
        return false;
    auto* backend = static_cast<Backend*>(spell->factory);
    if (backend->kind == Kind::enchant) {
        backend->enchant.add(spell->checker, text, -1);
        return true;
    }
    // Known words are not listed twice.
    const bool known = backend->hunspell.spell(spell->checker, text) != 0;
    backend->hunspell.add(spell->checker, text);
    return known || append_personal_word(*backend, *spell, text);
}

bool spell_checker_ignore(SpellChecker* spell, Utf8View word) noexcept {
    char text[kSpellCheckerWordBytes + 1];
    if (!valid_word(word, text) || !spell_checker_open(spell))
        return false;
    auto* backend = static_cast<Backend*>(spell->factory);
    if (backend->kind == Kind::enchant)
        backend->enchant.add_to_session(spell->checker, text, -1);
    else
        backend->hunspell.add(spell->checker, text);
    return true;
}

void spell_checker_release(SpellChecker* spell) noexcept {
    if (!spell)
        return;
    release_backend(spell);
    reset_value(spell);
}

} // namespace maicord
