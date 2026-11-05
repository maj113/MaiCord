// The Linux plugin loader on its own (src/app/plugins/loader.cpp, the ELF half): what it accepts of the shared
// objects the build makes, and each way it refuses a plugin that could run code at load, import the process's
// libraries, escape the pinned directory or be swapped under it. tests/app/plugins/plugin_binding.cpp runs the
// same fixtures through the whole runtime; the Windows loader's counterpart is loader_failures.cpp.
#include "maicord/app/plugins/loader.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/os/file.hpp"
#include "maicord/platform/crypto/hash_stream.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "../../support/test_output.hpp"
#include "plugin_fixture.hpp"

namespace maicord::plugin_loader_elf_tests {
using namespace plugin_fixture;

u32 failures;
void check(bool value, const char* label) noexcept {
    if (value) return;
    ++failures;
    test_write_text("FAIL plugin_loader_elf: ");
    test_write_line(label);
}

constexpr u64 kPermissions = ~0ull;
constexpr long kSysSymlink = 88, kSysMknodat = 259;

ConstSlice<wchar_t> leaf(const wchar_t* name) noexcept {
    return {name, length(name)};
}
ConstSlice<wchar_t> directory_of(const wchar_t* path) noexcept {
    return {path, length(path)};
}
bool utf8(const wchar_t* path, char (&output)[1024]) noexcept {
    return path_to_utf8(path, output, sizeof(output)) != 0;
}

// ---- Files and links made with plain system calls, so a test may break any rule the loader checks --------

bool make_directory(const wchar_t* path, u32 mode) noexcept {
    char text[1024];
    return utf8(path, text) && !syscall_failed(sys_mkdir(text, 0700)) && !syscall_failed(sys_chmod(text, mode));
}
bool set_mode(const wchar_t* path, u32 mode) noexcept {
    char text[1024];
    return utf8(path, text) && !syscall_failed(sys_chmod(text, mode));
}
bool link_to(const wchar_t* target, const wchar_t* path) noexcept {
    char to[1024], at[1024];
    return utf8(target, to) && utf8(path, at) &&
           !syscall_failed(syscall3(kSysSymlink, reinterpret_cast<long>(to), reinterpret_cast<long>(at), 0));
}
bool make_fifo(const wchar_t* path) noexcept {
    char at[1024];
    return utf8(path, at) &&
           !syscall_failed(syscall4(kSysMknodat, kAtCurrentDirectory, reinterpret_cast<long>(at), 010600, 0));
}
bool move(const wchar_t* from, const wchar_t* to) noexcept {
    char a[1024], b[1024];
    return utf8(from, a) && utf8(to, b) && !syscall_failed(sys_rename(a, b));
}
bool remove_file(const wchar_t* path) noexcept {
    char text[1024];
    return utf8(path, text) && !syscall_failed(sys_unlink(text));
}
bool write_bytes(const wchar_t* path, const u8* bytes, u64 count) noexcept {
    char text[1024];
    if (!utf8(path, text)) return false;
    const long file = sys_open(text, kOpenWriteOnly | kOpenCreate | kOpenTruncate, 0600);
    if (syscall_failed(file)) return false;
    u8 block[4096]{};
    bool ok = true;
    for (u64 done = 0; ok && done < count;) {
        const u32 now = static_cast<u32>(minimum<u64>(count - done, sizeof(block)));
        if (bytes) copy_bytes(block, bytes + done, now);
        ok = sys_write(static_cast<int>(file), block, now) == static_cast<long>(now);
        done += now;
    }
    return !syscall_failed(sys_close(static_cast<int>(file))) && ok;
}
// Reads up to count bytes from the start of the file (to its end, if that is nearer); *got is how many there were.
bool read_bytes(const wchar_t* path, u8* output, u32 count, u32* got) noexcept {
    char text[1024];
    if (!utf8(path, text)) return false;
    const long file = sys_open(text, kOpenReadOnly);
    if (syscall_failed(file)) return false;
    *got = 0;
    for (long read = 1; read > 0 && *got < count;) {
        read = sys_read(static_cast<int>(file), output + *got, count - *got);
        if (read > 0) *got += static_cast<u32>(read);
        else if (read < 0) {
            sys_close(static_cast<int>(file));
            return false;
        }
    }
    sys_close(static_cast<int>(file));
    return true;
}
bool patch(const wchar_t* path, u64 offset, const void* bytes, u32 count) noexcept {
    char text[1024];
    if (!utf8(path, text)) return false;
    const long file = sys_open(text, kOpenWriteOnly);
    if (syscall_failed(file)) return false;
    const bool ok = sys_pwrite(static_cast<int>(file), bytes, count, offset) == static_cast<long>(count);
    sys_close(static_cast<int>(file));
    return ok;
}
// The SHA-256 of a whole file, through the platform hash.
bool digest_of(const wchar_t* path, u8 (&digest)[32]) noexcept {
    char text[1024];
    if (!utf8(path, text)) return false;
    const long file = sys_open(text, kOpenReadOnly);
    if (syscall_failed(file)) return false;
    HashStream hash = nullptr;
    bool ok = hash_stream_begin(&hash, HashAlgorithm::sha256) >= 0;
    u8 block[4096];
    for (long got = 1; ok && got > 0;) {
        got = sys_read(static_cast<int>(file), block, sizeof(block));
        ok = got >= 0 && (!got || hash_stream_update(hash, block, static_cast<u32>(got)) >= 0);
    }
    ok = ok && hash_stream_finish(hash, digest, sizeof(digest)) >= 0;
    if (hash) hash_stream_destroy(hash);
    sys_close(static_cast<int>(file));
    return ok;
}

// Whether a plugin loaded from the leaf name is still mapped (the sealed copy is named "maicord-plugin:<leaf>").
bool mapped(const wchar_t* name) noexcept {
    static char maps[1 << 20];
    u32 used = 0;
    if (!read_bytes(L"/proc/self/maps", reinterpret_cast<u8*>(maps), sizeof(maps) - 1, &used)) return false;
    char needle[160] = "/memfd:maicord-plugin:";
    u32 length = 22;
    for (u32 i = 0; name[i] && length < sizeof(needle) - 1; ++i) needle[length++] = static_cast<char>(name[i]);
    for (u32 at = 0; at + length <= used; ++at)
        if (bytes_equal(maps + at, needle, length) && (at + length == used || maps[at + length] == ' ')) return true;
    return false;
}

struct Place {
    Root root;
    wchar_t outside[MAX_PATH]; // a directory next to plugins/, for what must stay out of it
    PluginDirectory pinned;
    wchar_t file[MAX_PATH];    // scratch: the last path built by path_of
    const wchar_t* path_of(const wchar_t* name) noexcept {
        join(file, root.plugins, name);
        return file;
    }
    bool pin() noexcept {
        return plugin_directory_open(&pinned, {root.plugins, length(root.plugins)}).ok();
    }
    void unpin() noexcept {
        plugin_directory_close(&pinned);
    }
    bool put(const wchar_t* built, const wchar_t* name) noexcept {
        return install(root, built, name);
    }
    PluginLoadResult load(const wchar_t* name, const u8* sha256 = nullptr, u64 approved = kPermissions) noexcept {
        PluginImage image{};
        const PluginLoadResult result = plugin_image_load(&image, pinned, leaf(name), approved, sha256);
        if (result.ok()) plugin_image_unload(&image);
        return result;
    }
};

// ---- The shared objects the build makes ----------------------------------------------------------------------

void test_good_plugins(Place& place) noexcept {
    struct Good {
        const wchar_t* built;
        const char* name;
        u64 owner;
    };
    const Good good[]{
        {L"clear-urls.dll", "Clear URLs", 0x6D63'636C'7572'6C73ull},
        {L"MaiCordPluginRuntimeFullFixture.dll", "Runtime fixture", 0x4D43525446554C4Cull},
        {L"MaiCordPluginAbiCFixture.dll", nullptr, 0x6D63635F61626943ull},
        {L"translate.dll", nullptr, 0},
        {L"show-me-your-name.dll", nullptr, 0},
        {L"reverse-image-search.dll", nullptr, 0},
        {L"silent-toggle.dll", nullptr, 0},
    };
    for (const Good& g : good) {
        check(place.put(g.built, L"good.so"), "a built plugin is installed");
        PluginImage image{};
        const PluginLoadResult result = plugin_image_load(&image, place.pinned, leaf(L"good.so"), kPermissions);
        check(result.ok() && image.module && image.descriptor && image.executable_count >= 1 && image.image_bytes > 0 &&
                  image.image_bytes % 4096 == 0 && image.owner_id == image.descriptor->owner_id,
              "a plugin built as the SDK documents loads");
        check(!g.name || same(Utf8View{image.info.name, image.info.name_bytes}, g.name), "its descriptor text is kept");
        check(!g.owner || image.owner_id == g.owner, "its owner id is the descriptor's");
        // The sealed copy is mapped under the leaf's name; no file name is opened again.
        check(mapped(L"good.so"), "the loaded plugin is mapped from its sealed copy");
        check(plugin_image_load(&image, place.pinned, leaf(L"good.so"), kPermissions).error == PluginLoadError::already_loaded,
              "an image that holds a plugin is not loaded twice");
        check(plugin_image_unload(&image).ok() && !image.module && !mapped(L"good.so"), "unload frees the plugin");
        check(plugin_image_unload(&image).error == PluginLoadError::invalid_argument, "unloading nothing is refused");
    }
    // The file on disk is not the mapped image: replacing it after the load changes nothing.
    place.put(L"clear-urls.dll", L"swap.so");
    PluginImage image{};
    check(plugin_image_load(&image, place.pinned, leaf(L"swap.so"), kPermissions).ok(), "a plugin loads for the swap");
    place.put(L"MaiCordPluginRuntimeBadFixture.dll", L"swap.so");
    check(image.module && same(Utf8View{image.info.name, image.info.name_bytes}, "Clear URLs") &&
              image.descriptor->abi == MC_ABI && mapped(L"swap.so"),
          "the mapped image outlives the replacement of its file");
    check(plugin_image_unload(&image).ok(), "the swapped plugin unloads");
}

void test_refused_fixtures(Place& place) noexcept {
    struct Case {
        const wchar_t* built;
        PluginLoadError error;
        PluginDescriptorProblem problem;
        const char* label;
    };
    constexpr PluginDescriptorProblem none = PluginDescriptorProblem::none;
    const Case cases[]{
        {L"MaiCordPluginRuntimeEntryFixture.dll", PluginLoadError::entry_point, none, "an entry point is refused"},
        {L"MaiCordPluginRuntimeInitFixture.dll", PluginLoadError::entry_point, none, "an initializer array is refused"},
        {L"MaiCordPluginRuntimeTlsFixture.dll", PluginLoadError::tls, none, "a thread-local segment is refused"},
        {L"MaiCordPluginRuntimeExtraFixture.dll", PluginLoadError::exports, none, "a second export is refused"},
        {L"MaiCordPluginRuntimeNeededFixture.dll", PluginLoadError::imports, none, "a needed library (DT_NEEDED) is refused"},
        {L"MaiCordPluginRuntimeImportFixture.dll", PluginLoadError::imports, none, "an undefined symbol is refused"},
        {L"MaiCordPluginRuntimeExecStackFixture.dll", PluginLoadError::sections, none, "an executable stack is refused"},
        {L"MaiCordPluginRuntimeBadFixture.dll", PluginLoadError::descriptor, PluginDescriptorProblem::text,
         "an invalid descriptor is refused"},
        {L"MaiCordPluginRuntimeAbiFixture.dll", PluginLoadError::descriptor, PluginDescriptorProblem::abi,
         "a descriptor of another ABI is refused"},
        {L"MaiCordPluginRuntimeLevelFixture.dll", PluginLoadError::api_level, none,
         "a plugin needing a newer api level is refused"},
    };
    for (const Case& test : cases) {
        check(place.put(test.built, L"candidate.so"), "a refused fixture is installed");
        PluginImage image{};
        const PluginLoadResult result = plugin_image_load(&image, place.pinned, leaf(L"candidate.so"), kPermissions);
        check(result.error == test.error && !image.module && !mapped(L"candidate.so"), test.label);
        check(test.problem == PluginDescriptorProblem::none || result.problem == test.problem,
              "a refused descriptor names the rule it broke");
        if (result.error != test.error) {
            test_write_metric("  error ", static_cast<u64>(result.error));
            test_write_metric("  problem ", static_cast<u64>(result.problem));
            test_write_metric("  native ", result.win32_error);
        }
        if (image.module) plugin_image_unload(&image);
    }
    // What the descriptor needs against what the user approved.
    place.put(L"MaiCordPluginRuntimeFullFixture.dll", L"full.so");
    check(place.load(L"full.so", nullptr, 0).error == PluginLoadError::permissions &&
              place.load(L"full.so", nullptr, MC_PERM_MESSAGE_SEND).ok(),
          "a hook's permission must be approved");
}

// ---- Damaged copies of a good shared object -----------------------------------------------------------

void test_damaged_files(Place& place) noexcept {
    check(place.put(L"MaiCordPluginRuntimeFullFixture.dll", L"damaged.so"), "a good copy is installed");
    const wchar_t* damaged = place.path_of(L"damaged.so");
    u8 head[1024];
    u32 got = 0;
    check(read_bytes(damaged, head, sizeof(head), &got) && got == sizeof(head), "the copy's headers are read");
    const u64 table = byte_load_le64(head + 32);
    const u32 count = byte_load_le16(head + 56);
    u32 load = 0, data = 0, stack = 0;
    for (u32 i = 0; i < count && table + 56u * (i + 1) <= sizeof(head); ++i) {
        const u32 header = static_cast<u32>(table) + 56u * i, type = byte_load_le32(head + header),
                  flags = byte_load_le32(head + header + 4);
        if (type == 1 && (flags & 1) && !load) load = header;
        if (type == 1 && (flags & 2) && !data) data = header;
        if (type == 0x6474e551) stack = header;
    }
    check(load && data && stack, "the copy has an executable and a writable load segment and a stack header");
    auto expect = [&](PluginLoadError error, const char* label) noexcept {
        check(place.load(L"damaged.so").error == error && !mapped(L"damaged.so"), label);
    };
    auto restore = [&](u64 offset, u32 bytes) noexcept { patch(damaged, offset, head + offset, bytes); };

    const u16 machine = 3;
    patch(damaged, 18, &machine, 2);
    expect(PluginLoadError::machine, "an image that is not x86-64 is refused");
    restore(18, 2);
    const u16 executable_type = 2;
    patch(damaged, 16, &executable_type, 2);
    expect(PluginLoadError::not_pe, "an executable (not a shared object) is refused");
    restore(16, 2);
    const u8 wrong_class = 1;
    patch(damaged, 4, &wrong_class, 1);
    expect(PluginLoadError::not_pe, "a 32-bit image is refused");
    restore(4, 1);
    const u8 magic = 'X';
    patch(damaged, 1, &magic, 1);
    expect(PluginLoadError::not_pe, "a file with another magic is refused");
    restore(1, 1);
    const u64 entry = 0x1000;
    patch(damaged, 24, &entry, 8);
    expect(PluginLoadError::entry_point, "a nonzero ELF entry is refused");
    restore(24, 8);
    const u32 writable_code = byte_load_le32(head + load + 4) | 2;
    patch(damaged, load + 4, &writable_code, 4);
    expect(PluginLoadError::sections, "a segment that is writable and executable is refused");
    restore(load + 4, 4);
    const u32 unreadable = byte_load_le32(head + data + 4) & ~4u;
    patch(damaged, data + 4, &unreadable, 4);
    expect(PluginLoadError::sections, "a segment that is not readable is refused");
    restore(data + 4, 4);
    const u32 executable_stack = byte_load_le32(head + stack + 4) | 1;
    patch(damaged, stack + 4, &executable_stack, 4);
    expect(PluginLoadError::sections, "an executable stack is refused");
    restore(stack + 4, 4);
    const u32 interpreter = 3;
    patch(damaged, stack, &interpreter, 4);
    expect(PluginLoadError::not_pe, "an interpreter segment is refused");
    restore(stack, 4);
    const u64 beyond = byte_load_le64(head + load + 32) + (1ull << 32);
    patch(damaged, load + 32, &beyond, 8);
    expect(PluginLoadError::sections, "a segment that reaches past its file is refused");
    restore(load + 32, 8);
    check(place.load(L"damaged.so").ok(), "the restored copy loads");

    const u8 junk[16]{0x7f, 'E', 'L', 'F'};
    check(write_bytes(place.path_of(L"truncated.so"), head, 1024) && write_bytes(place.path_of(L"junk.so"), junk, sizeof(junk)) &&
              write_bytes(place.path_of(L"empty.so"), junk, 0),
          "damaged files are written");
    check(place.load(L"truncated.so").error == PluginLoadError::sections, "a file cut off inside its segments is refused");
    check(place.load(L"junk.so").error == PluginLoadError::not_pe, "a file too short for the ELF header is refused");
    check(place.load(L"empty.so").error == PluginLoadError::open, "an empty file is refused");
    check(write_bytes(place.path_of(L"huge.so"), nullptr, kPluginDllMaximumBytes + 1ull) &&
              place.load(L"huge.so").error == PluginLoadError::not_pe,
          "a file above the size limit is refused before it is copied");
}

// ---- The digest ------------------------------------------------------------------------------------------

void test_digest(Place& place) noexcept {
    check(place.put(L"clear-urls.dll", L"hashed.so"), "a plugin is installed to be hashed");
    u8 digest[32]{};
    check(digest_of(place.path_of(L"hashed.so"), digest), "the file is hashed");
    check(place.load(L"hashed.so", digest).ok(), "the matching SHA-256 loads the plugin");
    u8 wrong[32];
    copy_bytes(wrong, digest, 32);
    wrong[31] ^= 1;
    PluginImage image{};
    check(plugin_image_load(&image, place.pinned, leaf(L"hashed.so"), kPermissions, wrong).error == PluginLoadError::digest &&
              !image.module && !mapped(L"hashed.so"),
          "another SHA-256 stops the load before anything is mapped");
}

// ---- Links, names and file modes ----------------------------------------------------------------------------

void test_file_policy(Place& place) noexcept {
    check(place.put(L"clear-urls.dll", L"ok.so"), "a plugin is installed");
    // A link that leaves the plugins directory, by a relative and by an absolute target, and a link inside it.
    wchar_t outside_copy[MAX_PATH], target[MAX_PATH];
    join(outside_copy, place.outside, L"copy.so");
    check(file_copy_new(place.path_of(L"ok.so"), outside_copy), "a plugin outside the plugins directory is made");
    check(link_to(L"../outside/copy.so", place.path_of(L"escape-relative.so")) &&
              place.load(L"escape-relative.so").error == PluginLoadError::open,
          "a link to a plugin outside the directory is not followed");
    join(target, place.outside, L"copy.so");
    check(link_to(target, place.path_of(L"escape-absolute.so")) &&
              place.load(L"escape-absolute.so").error == PluginLoadError::open,
          "a link with an absolute target is not followed");
    check(link_to(L"ok.so", place.path_of(L"alias.so")) && place.load(L"alias.so").error == PluginLoadError::open,
          "a link to a file in the same directory is not followed either");
    check(place.load(L"ok.so").ok(), "the plain file itself loads");

    // Names that are not plain names.
    check(place.load(L"..").error == PluginLoadError::open && place.load(L".").error == PluginLoadError::open &&
              place.load(L".hidden.so").error == PluginLoadError::open &&
              place.load(L"a/ok.so").error == PluginLoadError::open && place.load(L"..\\ok.so").error == PluginLoadError::open &&
              place.load(L"missing.so").error == PluginLoadError::open,
          "a name that is not an ordinary file name is refused");

    // Not a regular file: a pipe (opening it must not block) and a directory.
    check(make_fifo(place.path_of(L"pipe.so")) && place.load(L"pipe.so").error == PluginLoadError::open,
          "a named pipe is refused without blocking");
    check(make_directory(place.path_of(L"folder.so"), 0700) && place.load(L"folder.so").error == PluginLoadError::open,
          "a directory is refused");

    // Only the user may write the file.
    const wchar_t* ok = place.path_of(L"ok.so");
    check(set_mode(ok, 0660) && place.load(L"ok.so").error == PluginLoadError::open, "a file the group can write is refused");
    check(set_mode(ok, 0602) && place.load(L"ok.so").error == PluginLoadError::open, "a file everyone can write is refused");
    check(set_mode(ok, 0644) && place.load(L"ok.so").ok(), "a file others only read loads");
    check(set_mode(ok, 0600) && place.load(L"ok.so").ok(), "a private file loads");
}

// ---- The directory --------------------------------------------------------------------------------------------

void test_directory_policy(Place& place) noexcept {
    wchar_t path[MAX_PATH], child[MAX_PATH];
    PluginDirectory pinned{};
    auto open = [&](const wchar_t* text) noexcept { return plugin_directory_open(&pinned, directory_of(text)); };
    auto refused = [&](const wchar_t* text, const char* label) noexcept {
        const PluginLoadResult result = open(text);
        check(result.error == PluginLoadError::directory && !pinned.ancestor_count && !pinned.path_characters, label);
        plugin_directory_close(&pinned);
    };

    // A pinned directory holds every ancestor, once, and is not pinned twice.
    check(open(place.root.plugins).ok() && pinned.ancestor_count >= 3 && pinned.path_characters == length(place.root.plugins),
          "the plugins directory is pinned with every ancestor");
    check(open(place.root.plugins).error == PluginLoadError::already_loaded, "a pinned directory is not pinned twice");
    check(plugin_directory_close(&pinned).ok() && !pinned.ancestor_count && !pinned.path_characters,
          "closing releases every pin and clears the directory");
    check(plugin_directory_close(&pinned).ok(), "closing a closed directory is harmless");
    PluginImage image{};
    check(plugin_image_load(&image, pinned, leaf(L"ok.so"), kPermissions).error == PluginLoadError::open,
          "a closed directory cannot open files");

    // Paths that are not absolute, plain and made of ordinary names.
    refused(L"plugins", "a relative path is refused");
    refused(L"", "an empty path is refused");
    refused(L"C:\\plugins", "a Windows path is refused");
    join(path, place.root.plugins, L"");
    refused(path, "a path with a trailing separator is refused");
    join(path, place.root.base, L"plugins\\..\\plugins");
    refused(path, "a path with a backslash is refused");
    join(path, place.root.plugins, L"..");
    refused(path, "a path with a parent component is refused");
    join(path, place.root.plugins, L".");
    refused(path, "a path with a current-directory component is refused");
    join(path, place.root.base, L"missing");
    refused(path, "a missing directory is refused");
    join(path, place.root.plugins, L"ok.so");
    refused(path, "a file is not a directory");

    // A link is not followed, in the directory or in any ancestor of it.
    wchar_t real[MAX_PATH], by_link[MAX_PATH];
    join(real, place.root.base, L"real");
    join(by_link, place.root.base, L"by-link");
    check(make_directory(real, 0700) && link_to(L"real", by_link), "a directory and a link to it are made");
    refused(by_link, "a link to the directory is refused");
    join(child, by_link, L"sub");
    wchar_t sub[MAX_PATH];
    join(sub, real, L"sub");
    check(make_directory(sub, 0700), "a directory inside the real one is made");
    refused(child, "a directory below a linked ancestor is refused");

    // Who may write.
    check(set_mode(place.root.plugins, 0770), "the plugins directory is made group-writable");
    refused(place.root.plugins, "a plugins directory the group can write is refused");
    check(set_mode(place.root.plugins, 0702), "the plugins directory is made world-writable");
    refused(place.root.plugins, "a plugins directory everyone can write is refused");
    check(set_mode(place.root.plugins, 0755) && open(place.root.plugins).ok() && plugin_directory_close(&pinned).ok(),
          "a plugins directory others can only read is pinned");
    check(set_mode(place.root.plugins, 0700), "the plugins directory is private again");

    // An ancestor others can write is refused unless it is sticky (as /tmp is); the user's own group-writable one is not.
    wchar_t open_ancestor[MAX_PATH], below[MAX_PATH];
    join(open_ancestor, place.root.base, L"open");
    join(below, open_ancestor, L"plugins");
    check(make_directory(open_ancestor, 0700) && make_directory(below, 0700), "an ancestor and a plugins directory are made");
    check(set_mode(open_ancestor, 0777), "the ancestor is made world-writable");
    refused(below, "a plugins directory below an ancestor everyone can write is refused");
    check(set_mode(open_ancestor, 01777) && open(below).ok() && plugin_directory_close(&pinned).ok(),
          "a sticky world-writable ancestor, as /tmp is, is accepted");
    check(set_mode(open_ancestor, 0770) && open(below).ok() && plugin_directory_close(&pinned).ok(),
          "an ancestor of the user's that only the group can also write is accepted");
    check(set_mode(open_ancestor, 0700), "the ancestor is private again");

    // A directory that moved after it was pinned is no longer where its path says, so its files are refused.
    place.put(L"clear-urls.dll", L"ok.so");
    check(place.pin() && place.load(L"ok.so").ok(), "a plugin loads through the pin");
    wchar_t moved[MAX_PATH];
    join(moved, place.root.base, L"plugins.moved");
    check(move(place.root.plugins, moved) && link_to(L"plugins.moved", place.root.plugins),
          "the pinned directory is renamed and a link takes its name");
    check(place.load(L"ok.so").error == PluginLoadError::open, "a file of the moved directory is refused");
    remove_file(place.root.plugins);
    check(move(moved, place.root.plugins), "the directory is put back");
    check(place.load(L"ok.so").ok(), "and the same pin works again");
    place.unpin();
}

void run() noexcept {
    Place place{};
    if (!make_root(&place.root, L"mle")) {
        check(false, "temporary directory");
        return;
    }
    join(place.outside, place.root.base, L"outside");
    check(make_directory(place.outside, 0700), "the outside directory is made");
    check(place.pin(), "the plugins directory is pinned");
    test_good_plugins(place);
    test_refused_fixtures(place);
    test_damaged_files(place);
    test_digest(place);
    test_file_policy(place);
    place.unpin();
    test_directory_policy(place);
    remove_tree(place.root.base);
    if (!failures) test_write_text("plugin_loader_elf_completed\n");
}

} // namespace maicord::plugin_loader_elf_tests

MAICORD_TEST_ENTRY(maicord_plugin_loader_elf_entry) {
    maicord::plugin_loader_elf_tests::run();
    maicord::test_process_exit(maicord::plugin_loader_elf_tests::failures ? 1 : 0);
}
