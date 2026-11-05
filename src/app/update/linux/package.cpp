// maicord/app/update/package.hpp on Linux: the release tarball (packaging/build_tarball.py) as an update package.
// The package is read whole while its SHA-256 is taken, gunzipped (gzip_inflate checks the CRC-32 and the size) into
// anonymous pages and walked as a tar; the program, fonts/ and licenses/ of its directory are written into a new stage
// directory, and the program is read back from disk and held through a read-only descriptor. The Windows package.cpp
// does the same for a one-member zip: every check before any output, the sealed handle, a failure that leaves nothing.
#include "maicord/app/update/package.hpp"
#include "../package_admission.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/media/compression/deflate.hpp"
#include "maicord/os/file.hpp"
#include "maicord/platform/crypto/hash_stream.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "tree.hpp"

namespace maicord {
namespace {

using package_detail::file_owner;
using package_detail::refused;

constexpr u32 kTarBlock = 512;
constexpr u32 kPathCharacters = kUpdateArtifactMaximumPathCharacters + 1 + 16 + kUpdatePackageMaximumNameBytes + 1;
constexpr u32 kIoChunkBytes = 1u << 20;
constexpr u32 kDirectoryMode = 0755, kProgramMode = 0755, kDataMode = 0644;
constexpr u32 kExtensionBytes = 7; // ".tar.gz"

// Where an extracted file goes: the stage itself (the program) or one of its two directories.
enum Place : u32 { program, fonts, licenses, place_count };
constexpr Utf8View kPlaceNames[place_count]{kUpdatePackageExecutable, kUpdatePackageFontDirectory, kUpdatePackageLicenseDirectory};

struct Entry {
    Place place;
    const u8* name;
    u32 name_bytes;
    const u8* data;
    u32 bytes;
};
struct Layout {
    Entry entries[kUpdatePackageMaximumFiles];
    u32 count;
    bool has_program;
};

struct Failure {
    UpdatePackageResult result;
    bool fail(UpdatePackageError error, DWORD native = 0, i32 crypto = 0) noexcept {
        result.error = error; result.native_error = native; result.crypto_status = crypto;
        return false;
    }
};

// Zeroed pages, released when the owner leaves its scope.
struct Pages {
    u8* data = nullptr;
    usize bytes = 0;
    bool allocate(u64 count) noexcept {
        bytes = static_cast<usize>(count);
        data = static_cast<u8*>(pages_allocate(bytes));
        return data != nullptr;
    }
    void release() noexcept { if (data) pages_release(data, bytes); data = nullptr; }
    ~Pages() noexcept { release(); }
};
struct Inflater {
    DeflateWorkspace* workspace = deflate_workspace_allocate();
    ~Inflater() noexcept { deflate_workspace_free(workspace); }
};

u16 le16(const u8* p) noexcept { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 le32(const u8* p) noexcept { return le16(p) | (static_cast<u32>(le16(p + 2)) << 16); }
u64 le64(const u8* p) noexcept { return le32(p) | (static_cast<u64>(le32(p + 4)) << 32); }

// ---------------------------------------------------------------------------------------------------- the artifact

bool regular_file(HANDLE file, u64 bytes, Failure* out) noexcept {
    FileStat info{};
    if (!file_owner(file) || !file_stat(file, &info) || info.kind == FileKind::other)
        return out->fail(UpdatePackageError::file, thread_last_error());
    if (info.kind != FileKind::regular || info.links != 1 || info.bytes != bytes) return out->fail(UpdatePackageError::size);
    return true;
}

// The SHA-256 of the `bytes` of `file`, read in bounded chunks: into `into` when it is given (the package, kept for
// the gunzip), else through a small buffer.
bool read_hashed(HANDLE file, u64 bytes, u8* into, u8* digest, Failure* out) noexcept {
    HashStream hash = nullptr;
    i32 native = hash_stream_begin(&hash, HashAlgorithm::sha256);
    if (native < 0) return out->fail(UpdatePackageError::hash, 0, native);
    u8 buffer[kUpdatePackageReadBufferBytes];
    bool ok = true;
    for (u64 done = 0; ok && done < bytes; ) {
        u8* at = into ? into + done : buffer;
        const u32 chunk = static_cast<u32>(minimum<u64>(bytes - done, into ? kIoChunkBytes : sizeof(buffer)));
        u32 got = 0;
        if (!file_read_at(file, done, at, chunk, &got)) ok = out->fail(UpdatePackageError::read, thread_last_error());
        else if (got != chunk) ok = out->fail(UpdatePackageError::size);
        else if ((native = hash_stream_update(hash, at, chunk)) < 0) ok = out->fail(UpdatePackageError::hash, 0, native);
        done += chunk;
    }
    if (ok && (native = hash_stream_finish(hash, digest, 32)) < 0) ok = out->fail(UpdatePackageError::hash, 0, native);
    if (hash_stream_destroy(hash) < 0 && ok) ok = out->fail(UpdatePackageError::hash);
    return ok;
}

// ---------------------------------------------------------------------------------------------------- the tar

bool all_zero(const u8* data, u64 bytes) noexcept {
    for (u64 i = 0; i < bytes; ++i) if (data[i]) return false;
    return true;
}
// A tar number: octal digits between optional spaces and the field's NUL. The base-256 form is not the producer's.
bool octal(const u8* field, u32 count, u64* value) noexcept {
    if (field[0] & 0x80) return false;
    u32 i = 0;
    while (i < count && field[i] == ' ') ++i;
    u64 parsed = 0, digits = 0;
    for (; i < count && field[i] >= '0' && field[i] <= '7'; ++i, ++digits) {
        if (digits > 21) return false;
        parsed = parsed * 8 + (field[i] - '0');
    }
    for (; i < count; ++i) if (field[i] && field[i] != ' ') return false;
    *value = parsed;
    return digits != 0;
}
bool header_checksum(const u8* header) noexcept {
    u64 sum = 0, stored = 0;
    for (u32 i = 0; i < kTarBlock; ++i) sum += (i >= 148 && i < 156) ? ' ' : header[i];
    return octal(header + 148, 8, &stored) && stored == sum;
}
u32 text_length(const u8* text, u32 limit) noexcept {
    u32 n = 0;
    while (n < limit && text[n]) ++n;
    return n;
}
// A file name of fonts/ or licenses/: letters, digits and . _ + -, not starting with a dot.
bool flat_name(const u8* name, u32 bytes) noexcept {
    if (!bytes || bytes > kUpdatePackageMaximumNameBytes || name[0] == '.') return false;
    for (u32 i = 0; i < bytes; ++i) {
        const u8 c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-'))
            return false;
    }
    return true;
}
// A member path: relative to the archive's root directory, with no empty, "." or ".." component and no control byte.
bool member_path(const u8* name, u32 bytes) noexcept {
    if (!bytes || name[0] == '/') return false;
    u32 start = 0;
    for (u32 i = 0; i <= bytes; ++i) {
        if (i < bytes && name[i] < 0x20) return false;
        if (i < bytes && name[i] != '/') continue;
        const u32 component = i - start;
        if (!component || (component == 1 && name[start] == '.') || (component == 2 && name[start] == '.' && name[start + 1] == '.'))
            return false;
        start = i + 1;
    }
    return true;
}
bool is_name(const u8* name, u32 bytes, Utf8View expected) noexcept {
    return bytes == expected.bytes && bytes_equal(name, expected.data, bytes);
}
// Whether `name` is `directory` and a slash and more.
bool inside(const u8* name, u32 bytes, Utf8View directory) noexcept {
    return bytes > directory.bytes && name[directory.bytes] == '/' && bytes_equal(name, directory.data, directory.bytes);
}

// Where the member `relative` (below the root directory) goes. False for a name or a type the producer does not make.
bool classify(const u8* relative, u32 bytes, bool directory, u64 mode, Place* place, const u8** leaf, u32* leaf_bytes) noexcept {
    *place = place_count;
    if (is_name(relative, bytes, kPlaceNames[program])) {
        // The program: an executable file.
        if (directory || !(mode & 0100)) return false;
        *place = program; *leaf = relative; *leaf_bytes = bytes;
        return true;
    }
    if (inside(relative, bytes, kPlaceNames[program])) return false;
    for (u32 p = fonts; p < place_count; ++p) {
        if (is_name(relative, bytes, kPlaceNames[p])) return directory;
        if (!inside(relative, bytes, kPlaceNames[p])) continue;
        *leaf = relative + kPlaceNames[p].bytes + 1;
        *leaf_bytes = bytes - kPlaceNames[p].bytes - 1;
        // Flat: files only, and names a file here can have.
        if (directory || !flat_name(*leaf, *leaf_bytes)) return false;
        *place = static_cast<Place>(p);
        return true;
    }
    return true;
}

// The tar of one directory `top`: the program and the flat fonts/ and licenses/ become entries; every other member is
// checked and left. Types other than files and directories, names outside the directory, nested files, repeats and
// anything after the end marker but zeros fail the walk, as does an archive with no end marker.
bool walk(const u8* tar, u64 bytes, Utf8View top, Layout* out) noexcept {
    u64 at = 0;
    const u8* long_name = nullptr;
    u32 long_bytes = 0, headers = 0;
    while (at < bytes) {
        if (bytes - at < kTarBlock) return false;
        const u8* header = tar + at;
        if (all_zero(header, kTarBlock)) return !long_name && all_zero(header, bytes - at);
        if (++headers > kUpdatePackageMaximumEntries || !header_checksum(header) || !bytes_equal(header + 257, "ustar", 5)) return false;
        u64 size = 0, mode = 0;
        if (!octal(header + 124, 12, &size) || !octal(header + 100, 8, &mode)) return false;
        const u8 type = header[156];
        at += kTarBlock;
        const u64 padded = (size + kTarBlock - 1) / kTarBlock * kTarBlock;
        if (size > bytes - at || padded > bytes - at) return false;
        const u8* data = tar + at;
        at += padded;
        if (type == 'L') {
            // GNU long name: the next member's name, NUL terminated, in this member's data.
            if (long_name || !size || size > 1024 || data[size - 1]) return false;
            long_name = data;
            long_bytes = text_length(data, static_cast<u32>(size));
            if (long_bytes != size - 1) return false;
            continue;
        }
        const bool directory = type == '5';
        if (!directory && type != '0' && type) return false;
        // POSIX ustar splits a long path over prefix and name; GNU tar has no prefix.
        u8 joined[256];
        const u8* name = long_name;
        u32 name_bytes = long_bytes;
        if (!name) {
            name = header;
            name_bytes = text_length(header, 100);
            const u32 prefix = header[262] == 0 ? text_length(header + 345, 155) : 0;
            if (prefix) {
                copy_bytes(joined, header + 345, prefix);
                joined[prefix] = '/';
                copy_bytes(joined + prefix + 1, header, name_bytes);
                name = joined;
                name_bytes += prefix + 1;
            }
        }
        long_name = nullptr;
        long_bytes = 0;
        if (directory && name_bytes && name[name_bytes - 1] == '/') --name_bytes;
        if (!member_path(name, name_bytes) || name_bytes < top.bytes || !bytes_equal(name, top.data, top.bytes) ||
            (name_bytes > top.bytes && name[top.bytes] != '/'))
            return false;
        if (name_bytes == top.bytes) {
            // The root directory's own entry.
            if (!directory) return false;
            continue;
        }
        const u8* relative = name + top.bytes + 1;
        const u32 relative_bytes = name_bytes - top.bytes - 1;
        Place place = place_count;
        const u8* leaf = nullptr;
        u32 leaf_bytes = 0;
        if (!classify(relative, relative_bytes, directory, mode, &place, &leaf, &leaf_bytes)) return false;
        if (place == place_count) continue;
        if (size > kUpdatePackageMaximumFileBytes || (place == program && !size)) return false;
        for (u32 i = 0; i < out->count; ++i)
            if (out->entries[i].place == place && out->entries[i].name_bytes == leaf_bytes && bytes_equal(out->entries[i].name, leaf, leaf_bytes))
                return false;
        if (out->count == kUpdatePackageMaximumFiles) return false;
        out->has_program = out->has_program || place == program;
        out->entries[out->count++] = {place, leaf, leaf_bytes, data, static_cast<u32>(size)};
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------- the image

// An x86-64 ELF executable of the kind the client is (a position-independent image): program headers inside the file,
// loadable segments that fit the file and the address space without overlapping, and an entry point in an executable one.
bool elf_image(const u8* data, u64 bytes) noexcept {
    constexpr u32 kHeader = 64, kSegment = 56, kLoad = 1, kExecutable = 1;
    constexpr u64 kAddressLimit = u64{1} << 47;
    if (bytes < kHeader || data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F' || data[4] != 2 || data[5] != 1 ||
        data[6] != 1 || (data[7] != 0 && data[7] != 3) || le16(data + 16) != 3 || le16(data + 18) != 62 || le32(data + 20) != 1 ||
        le16(data + 52) != kHeader || le16(data + 54) != kSegment)
        return false;
    const u32 count = le16(data + 56);
    const u64 table = le64(data + 32), entry = le64(data + 24);
    if (!count || count > kUpdatePackageMaximumSegments || table < kHeader || table > bytes || count * kSegment > bytes - table) return false;
    bool entry_executable = false, loads = false;
    for (u32 i = 0; i < count; ++i) {
        const u8* segment = data + table + static_cast<u64>(i) * kSegment;
        if (le32(segment) != kLoad) continue;
        loads = true;
        const u64 flags = le32(segment + 4), offset = le64(segment + 8), address = le64(segment + 16), in_file = le64(segment + 32),
                  in_memory = le64(segment + 40), align = le64(segment + 48);
        if (in_file > in_memory || offset > bytes || in_file > bytes - offset || address >= kAddressLimit ||
            in_memory >= kAddressLimit || (align > 1 && ((align & (align - 1)) || offset % align != address % align)))
            return false;
        for (u32 j = 0; j < i; ++j) {
            const u8* other = data + table + static_cast<u64>(j) * kSegment;
            if (le32(other) != kLoad) continue;
            const u64 other_address = le64(other + 16), other_memory = le64(other + 40);
            if (in_memory && other_memory && address < other_address + other_memory && other_address < address + in_memory) return false;
        }
        if ((flags & kExecutable) && entry >= address && entry - address < in_file) entry_executable = true;
    }
    return loads && entry_executable;
}

// ---------------------------------------------------------------------------------------------------- the stage

struct Wide {
    wchar_t text[kPathCharacters];
    u32 count;
};
// base, a slash and the ASCII leaf, terminated.
bool join(Wide* out, ConstSlice<wchar_t> base, const u8* leaf, u32 leaf_bytes) noexcept {
    if (base.count + 1 + leaf_bytes >= kPathCharacters) return false;
    copy_bytes(out->text, base.data, base.count * sizeof(wchar_t));
    out->text[base.count] = L'/';
    for (u32 i = 0; i < leaf_bytes; ++i) out->text[base.count + 1 + i] = leaf[i];
    out->count = base.count + 1 + leaf_bytes;
    out->text[out->count] = 0;
    return true;
}
bool set_mode(const wchar_t* path, u32 mode) noexcept {
    char text[kPathCharacters * 3];
    if (!path_to_utf8(path, text, sizeof(text))) { set_thread_last_error(ERROR_BAD_PATHNAME); return false; }
    const long result = sys_chmod(text, mode);
    if (syscall_failed(result)) set_thread_last_error(win32_error_from_result(result));
    return !syscall_failed(result);
}
bool write_all(HANDLE file, const u8* data, u32 bytes) noexcept {
    for (u32 done = 0; done < bytes; ) {
        const u32 chunk = minimum<u32>(bytes - done, kIoChunkBytes);
        u32 wrote = 0;
        if (!file_write(file, data + done, chunk, &wrote) || wrote != chunk) return false;
        done += wrote;
    }
    return true;
}

// The stage being written: its directory, and the descriptors this call still owns.
struct Stage {
    Wide root, places[place_count];
    HANDLE writer, sealed;
    Failure status;
    // Closes what is open and removes the directory, which this call created and so owns; returns the failure.
    UpdatePackageResult abandon(UpdatePackageError error, DWORD native = 0) noexcept {
        status.fail(error, native);
        if (writer) file_close(writer);
        if (sealed) file_close(sealed);
        writer = sealed = nullptr;
        if (!update_tree_remove(root.text)) status.result.cleanup_error = thread_last_error();
        return status.result;
    }
};

} // namespace

UpdatePackageResult update_package_extract(UpdateArtifact* source, const UpdatePackageOffer& offer, const P256Key& key,
    KdfSha256Workspace& workspace, const UpdateManifestPolicy& policy, const UpdateAcceptanceTransaction& acceptance,
    Slice<u32> scratch, ConstSlice<wchar_t> destination, UpdatePackage* output) noexcept {
    UpdateManifest manifest{};
    const auto admitted = package_detail::admit(source, offer, key, workspace, policy, acceptance, scratch, destination, output, &manifest);
    if (!admitted.ok()) return admitted;
    Failure failure{};
    // The tarball's directory is the artifact's name without its extension.
    const Utf8View top{manifest.artifact_name, manifest.artifact_name_bytes - kExtensionBytes};
    if (manifest.artifact_bytes < 18 || manifest.artifact_bytes > kUpdatePackageMaximumBytes) {
        failure.fail(UpdatePackageError::size);
        return failure.result;
    }

    // 1. The package, whole, with the digest of exactly those bytes; then the tar, whole.
    Pages packed, tar;
    u8 package_hash[32]{};
    if (!regular_file(source->file, source->bytes, &failure)) return failure.result;
    if (!packed.allocate(source->bytes)) { failure.fail(UpdatePackageError::read, ERROR_NOT_ENOUGH_MEMORY); return failure.result; }
    if (!read_hashed(source->file, source->bytes, packed.data, package_hash, &failure)) return failure.result;
    if (!bytes_equal(package_hash, manifest.artifact_sha256, 32)) { failure.fail(UpdatePackageError::digest); return failure.result; }
    // A gzip stream ends with the CRC-32 and the length of what it holds; the package is one member, so the tar's
    // size is announced before anything is unpacked.
    const u64 tar_bytes = le32(packed.data + source->bytes - 4);
    Inflater inflater;
    if (tar_bytes < 2 * kTarBlock || tar_bytes > kUpdatePackageMaximumTarBytes || !inflater.workspace || !tar.allocate(tar_bytes)) {
        failure.fail(UpdatePackageError::archive);
        return failure.result;
    }
    const auto inflated = gzip_inflate({packed.data, static_cast<u32>(source->bytes)}, {tar.data, static_cast<u32>(tar_bytes)},
                                       inflater.workspace);
    if (!inflated.ok() || inflated.output_bytes != tar_bytes || inflated.input_bytes != source->bytes) {
        failure.fail(UpdatePackageError::archive, static_cast<DWORD>(inflated.error));
        return failure.result;
    }
    packed.release();

    // 2. The layout and the program's image, before anything is written.
    Layout layout{};
    if (!walk(tar.data, tar_bytes, top, &layout) || !layout.has_program) { failure.fail(UpdatePackageError::archive); return failure.result; }
    const Entry* program_entry = nullptr;
    for (u32 i = 0; i < layout.count; ++i) if (layout.entries[i].place == program) program_entry = &layout.entries[i];
    if (!elf_image(program_entry->data, program_entry->bytes)) { failure.fail(UpdatePackageError::elf); return failure.result; }
    UpdatePackage candidate{};
    {
        HashStream hash = nullptr;
        i32 native = hash_stream_begin(&hash, HashAlgorithm::sha256);
        if (native >= 0 && (native = hash_stream_update(hash, program_entry->data, program_entry->bytes)) >= 0)
            native = hash_stream_finish(hash, candidate.sha256, 32);
        if (hash) hash_stream_destroy(hash);
        if (native < 0) { failure.fail(UpdatePackageError::hash, 0, native); return failure.result; }
    }

    // 3. The stage directory and its files; the program is written last and kept open through the sealed descriptor.
    Stage stage{};
    copy_bytes(stage.root.text, destination.data, destination.count * sizeof(wchar_t));
    stage.root.count = destination.count;
    stage.root.text[destination.count] = 0;
    if (!dir_create(stage.root.text)) { failure.fail(UpdatePackageError::create, thread_last_error()); return failure.result; }
    for (u32 p = fonts; p < place_count; ++p) {
        bool used = false;
        for (u32 i = 0; i < layout.count; ++i) used = used || layout.entries[i].place == p;
        if (used && (!join(&stage.places[p], destination, reinterpret_cast<const u8*>(kPlaceNames[p].data), kPlaceNames[p].bytes) ||
                     !dir_create(stage.places[p].text)))
            return stage.abandon(UpdatePackageError::create, thread_last_error());
    }
    Wide path{};
    for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 i = 0; i < layout.count; ++i) {
            const Entry& entry = layout.entries[i];
            const bool is_program = entry.place == program;
            if (is_program != (pass == 1)) continue;
            const ConstSlice<wchar_t> parent = is_program ? destination : ConstSlice<wchar_t>{stage.places[entry.place].text, stage.places[entry.place].count};
            if (!join(&path, parent, entry.name, entry.name_bytes)) return stage.abandon(UpdatePackageError::create, ERROR_FILENAME_EXCED_RANGE);
            stage.writer = file_open(path.text, {is_program ? FileAccess::read_write : FileAccess::write, FileCreate::create_new, kFileShareRead,
                                                 kFileNoFollow | kFileDeletable});
            if (stage.writer == INVALID_HANDLE_VALUE) { stage.writer = nullptr; return stage.abandon(UpdatePackageError::create, thread_last_error()); }
            Failure checked{};
            if (!regular_file(stage.writer, 0, &checked)) return stage.abandon(checked.result.error, checked.result.native_error);
            if (!write_all(stage.writer, entry.data, entry.bytes)) return stage.abandon(UpdatePackageError::write, thread_last_error());
            if (!file_flush(stage.writer)) return stage.abandon(UpdatePackageError::flush, thread_last_error());
            if (!set_mode(path.text, is_program ? kProgramMode : kDataMode)) return stage.abandon(UpdatePackageError::create, thread_last_error());
            if (!is_program) {
                const bool closed = file_close(stage.writer);
                stage.writer = nullptr;
                if (!closed) return stage.abandon(UpdatePackageError::write, thread_last_error());
                continue;
            }
            // Seal: the same file through a read-only descriptor, the only one left open.
            if (!file_duplicate_sealed(stage.writer, &stage.sealed)) { stage.sealed = nullptr; return stage.abandon(UpdatePackageError::seal, thread_last_error()); }
            const bool closed = file_close(stage.writer);
            stage.writer = nullptr;
            if (!closed) return stage.abandon(UpdatePackageError::seal, thread_last_error());
            u8 digest[32]{};
            Failure read{};
            if (!regular_file(stage.sealed, entry.bytes, &read) || !read_hashed(stage.sealed, entry.bytes, nullptr, digest, &read))
                return stage.abandon(read.result.error, read.result.native_error);
            if (!bytes_equal(digest, candidate.sha256, 32)) return stage.abandon(UpdatePackageError::digest);
        }
    }
    for (u32 p = program; p < place_count; ++p)
        if ((p == program || stage.places[p].count) && !set_mode(p == program ? stage.root.text : stage.places[p].text, kDirectoryMode))
            return stage.abandon(UpdatePackageError::create, thread_last_error());
    candidate.file = stage.sealed;
    candidate.version = manifest.version;
    candidate.bytes = program_entry->bytes;
    *output = candidate;
    return {};
}

UpdatePackageResult update_package_release(UpdatePackage* package, bool discard) noexcept {
    if (!package || package->file == INVALID_HANDLE_VALUE) return refused(UpdatePackageError::invalid_input);
    UpdatePackageResult status{};
    if (package->file) {
        if ((discard && !file_mark_delete_on_close(package->file)) || !file_close(package->file)) {
            status.error = UpdatePackageError::release; status.native_error = thread_last_error(); return status;
        }
    }
    SecureZeroMemory(package, sizeof(*package));
    return status;
}

bool update_package_intact(const UpdatePackage& package, ConstSlice<wchar_t> stage) noexcept {
    Wide path{};
    if (!file_owner(package.file) || !stage.data || !stage.count ||
        !join(&path, stage, reinterpret_cast<const u8*>(kUpdatePackageExecutable.data), kUpdatePackageExecutable.bytes)) {
        set_thread_last_error(ERROR_INVALID_PARAMETER);
        return false;
    }
    FileStat held{}, named{};
    if (!file_stat(package.file, &held) || !file_path_stat(path.text, &named)) return false;
    if (held.kind != FileKind::regular || named.kind != FileKind::regular || held.links != 1 || named.links != 1 ||
        held.bytes != package.bytes || named.bytes != package.bytes || !bytes_equal(&held.id, &named.id, sizeof(held.id))) {
        set_thread_last_error(ERROR_FILE_INVALID);
        return false;
    }
    Failure failure{};
    u8 digest[32]{};
    if (!read_hashed(package.file, package.bytes, nullptr, digest, &failure)) {
        set_thread_last_error(failure.result.native_error ? failure.result.native_error : ERROR_FILE_INVALID);
        return false;
    }
    if (!bytes_equal(digest, package.sha256, 32)) { set_thread_last_error(ERROR_FILE_INVALID); return false; }
    return true;
}

} // namespace maicord
