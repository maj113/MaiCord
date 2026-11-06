#include "maicord/app/update/package.hpp"
#include "package_admission.hpp"
#include "maicord/platform/crypto/hash_stream.hpp"
#include "maicord/core/crc32.hpp"
#include "maicord/core/win32_image.hpp"
#include "maicord/os/file.hpp"

namespace maicord {
namespace {
// FILE_ATTRIBUTE_ARCHIVE and FILE_ATTRIBUTE_NORMAL: the DOS attributes the producer's zip may carry.
constexpr u32 kZipAttributeArchive = 0x20, kZipAttributeNormal = 0x80;
struct Member { u64 data, bytes; u32 crc; };
struct Reader {
    HANDLE file;
    u64 bytes;
    u8 buffer[kUpdatePackageReadBufferBytes];
    UpdatePackageResult status{};
    HashStream hash = nullptr;
    // File position after the last read of `file`; kUnknownCursor until a seek or after any failure.
    static constexpr u64 kUnknownCursor = ~u64{0};
    u64 cursor = kUnknownCursor;
    bool fail(UpdatePackageError error, DWORD native = 0, i32 crypto = 0) noexcept {
        status.error = error; status.native_error = native; status.crypto_status = crypto; return false;
    }
    bool read(u64 offset, void* output, u32 count) noexcept {
        if (offset > bytes || count > bytes - offset) return fail(UpdatePackageError::size);
        if (cursor != offset) {
            cursor = kUnknownCursor;
            if (!file_seek(file, static_cast<i64>(offset), FileSeek::begin)) return fail(UpdatePackageError::read, thread_last_error());
        }
        cursor = kUnknownCursor;
        u32 done = 0;
        while (done < count) {
            u32 got = 0;
            if (!file_read(file, static_cast<u8*>(output) + done, count - done, &got))
                return fail(UpdatePackageError::read, thread_last_error());
            if (!got || got > count - done) return fail(UpdatePackageError::size);
            done += got;
        }
        cursor = offset + count;
        return true;
    }
    bool begin_hash() noexcept {
        const i32 native = hash_stream_begin(&hash, HashAlgorithm::sha256);
        return native >= 0 || fail(UpdatePackageError::hash, 0, native);
    }
    bool finish_hash(u8* digest) noexcept {
        i32 native = hash_stream_finish(hash, digest, 32);
        if (native < 0) return fail(UpdatePackageError::hash, 0, native);
        native = hash_stream_destroy(hash);
        if (native < 0) return fail(UpdatePackageError::hash, 0, native);
        hash = nullptr; return true;
    }
    bool stream(u64 offset, u64 count, u8* digest, u32* crc, HANDLE destination = nullptr) noexcept {
        if (!begin_hash()) return false;
        u32 check = 0;
        while (count) {
            const u32 chunk = static_cast<u32>(minimum<u64>(count, sizeof(buffer)));
            if (!read(offset, buffer, chunk)) return false;
            const i32 native = hash_stream_update(hash, buffer, chunk);
            if (native < 0) return fail(UpdatePackageError::hash, 0, native);
            // Chain the published IEEE CRC over the same bounded read chunks.
            if (crc) check = crc32_extend(check, buffer, chunk);
            if (destination) {
                u32 done = 0;
                while (done < chunk) {
                    u32 wrote = 0;
                    if (!file_write(destination, buffer + done, chunk - done, &wrote))
                        return fail(UpdatePackageError::write, thread_last_error());
                    if (!wrote || wrote > chunk - done) return fail(UpdatePackageError::write);
                    done += wrote;
                }
            }
            count -= chunk; offset += chunk;
        }
        if (crc) *crc = check;
        return finish_hash(digest);
    }
};
u16 word(const u8* bytes) noexcept { return static_cast<u16>(bytes[0] | (static_cast<u16>(bytes[1]) << 8)); }
u32 dword(const u8* bytes) noexcept { return word(bytes) | (static_cast<u32>(word(bytes + 2)) << 16); }
bool owner(HANDLE file) noexcept { return file && file != INVALID_HANDLE_VALUE; }
bool regular(HANDLE file, u64 bytes, UpdatePackageResult* status) noexcept {
    FileStat info{};
    if (!owner(file) || !file_stat(file, &info) || info.kind == FileKind::other) {
        status->error = UpdatePackageError::file; status->native_error = thread_last_error(); return false;
    }
    if (info.kind != FileKind::regular || info.links != 1 || info.bytes != bytes) {
        status->error = UpdatePackageError::size; return false;
    }
    return true;
}
// Explicit producer subset: one DOS/NTFS regular file named exactly
// MaiCord.exe, stored, version <= 2.0, no flags, attributes, optional fields,
// comments or ZIP64 sentinels, and nothing before, between or after the records.
bool zip(Reader& reader, Member* member) noexcept {
    constexpr u32 name_bytes = kUpdatePackageExecutable.bytes;
    if (reader.bytes < 22 || reader.bytes > kUpdatePackageMaximumBytes) return reader.fail(UpdatePackageError::zip);
    u8 end[22]{};
    if (!reader.read(reader.bytes - 22, end, sizeof(end))) return false;
    const u64 central = dword(end + 16), central_bytes = dword(end + 12);
    if (dword(end) != 0x06054b50 || word(end + 4) || word(end + 6) || word(end + 8) != 1 || word(end + 10) != 1 ||
        word(end + 20) || central > reader.bytes - 22 || central_bytes != reader.bytes - 22 - central)
        return reader.fail(UpdatePackageError::zip);
    u8 local[30]{}, record[46]{}, name[name_bytes]{}, central_name[name_bytes]{};
    if (!reader.read(0, local, sizeof(local)) || !reader.read(central, record, sizeof(record))) return false;
    const u16 version = word(local + 4);
    const u64 bytes = dword(local + 22);
    const u16 made = word(record + 4);
    const u32 attributes = dword(record + 38);
    if (dword(local) != 0x04034b50 || (version != 10 && version != 20) || word(local + 6) || word(local + 8) ||
        !bytes || bytes == 0xffffffff || dword(local + 18) != bytes || word(local + 26) != name_bytes ||
        word(local + 28) || dword(record) != 0x02014b50 || (made >> 8 != 0 && made >> 8 != 10) ||
        (made & 255) > 20 || word(record + 6) != version || word(record + 8) || word(record + 10) ||
        word(record + 12) != word(local + 10) || word(record + 14) != word(local + 12) ||
        dword(record + 16) != dword(local + 14) || dword(record + 20) != bytes || dword(record + 24) != bytes ||
        word(record + 28) != name_bytes || word(record + 30) || word(record + 32) || word(record + 34) ||
        word(record + 36) || (attributes & ~u32{kZipAttributeArchive | kZipAttributeNormal}) ||
        dword(record + 42)) return reader.fail(UpdatePackageError::zip);
    const u64 data = 30 + name_bytes;
    if (data > central || bytes != central - data || 46 + name_bytes != reader.bytes - 22 - central)
        return reader.fail(UpdatePackageError::zip);
    if (!reader.read(30, name, name_bytes) || !reader.read(central + 46, central_name, name_bytes)) return false;
    if (!bytes_equal(name, kUpdatePackageExecutable.data, name_bytes) ||
        !bytes_equal(central_name, kUpdatePackageExecutable.data, name_bytes)) return reader.fail(UpdatePackageError::zip);
    *member = {data, bytes, dword(local + 14)};
    return true;
}
struct Pe {
    Reader& reader;
    u64 base, bytes, sections;
    u32 count, headers;
    bool read(u64 at, void* out, u32 size) noexcept {
        return at <= bytes && size <= bytes - at && reader.read(base + at, out, size);
    }
    bool section(u32 i, u8* out) noexcept { return read(sections + static_cast<u64>(i) * 40, out, 40); }
    // The entry point's first byte lies in an executable section's raw data.
    bool entry_executable(u32 rva) noexcept {
        for (u32 i = 0; i < count; ++i) {
            u8 sec[40]{}; if (!section(i, sec)) return false;
            const u64 start = dword(sec + 12), raw = dword(sec + 16);
            if (rva >= start && rva - start < raw && (dword(sec + 36) & IMAGE_SCN_MEM_EXECUTE)) return true;
        }
        return false;
    }
    // An AMD64 executable image (not a DLL) whose entry point lies in an
    // executable section and whose sections stay within the file and image.
    bool verify() noexcept {
        u8 dos[64]{}, nt[264]{};
        if (!read(0, dos, sizeof(dos)) || word(dos) != IMAGE_DOS_SIGNATURE) return false;
        const u32 pe = dword(dos + 60);
        if (pe < 64 || !read(pe, nt, sizeof(nt)) || dword(nt) != IMAGE_NT_SIGNATURE ||
            word(nt + 4) != IMAGE_FILE_MACHINE_AMD64 || word(nt + 20) != 240 || word(nt + 24) != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            !(word(nt + 22) & IMAGE_FILE_EXECUTABLE_IMAGE) || (word(nt + 22) & IMAGE_FILE_DLL) ||
            dword(nt + 132) != 16 || !dword(nt + 40)) return false;
        count = word(nt + 6); sections = static_cast<u64>(pe) + sizeof(nt); headers = dword(nt + 84);
        if (!count || count > kUpdatePackageMaximumSections || sections > bytes || count * 40 > bytes - sections ||
            headers < sections + count * 40 || headers > bytes || !dword(nt + 80)) return false;
        for (u32 i = 0; i < count; ++i) {
            u8 a[40]{}; if (!section(i, a)) return false;
            const u64 raw = dword(a + 16), start = dword(a + 20), va = dword(a + 12);
            const u64 virtual_bytes = maximum<u64>(dword(a + 8), raw);
            if ((raw && (start < headers || start > bytes || raw > bytes - start)) ||
                (virtual_bytes && va < headers) ||
                va + virtual_bytes > dword(nt + 80)) return false;
            for (u32 j = 0; j < i; ++j) {
                u8 b[40]{}; if (!section(j, b)) return false;
                const u64 other_raw = dword(b + 16), other_start = dword(b + 20), other_va = dword(b + 12);
                const u64 other_virtual = maximum<u64>(dword(b + 8), other_raw);
                if ((raw && other_raw && start < other_start + other_raw && other_start < start + raw) ||
                    (virtual_bytes && other_virtual && va < other_va + other_virtual && other_va < va + virtual_bytes)) return false;
            }
        }
        return entry_executable(dword(nt + 40));
    }
};
bool image(Reader& reader, u64 base, u64 bytes) noexcept {
    Pe pe{reader, base, bytes, 0, 0, 0};
    if (pe.verify()) return true;
    return reader.status.error == UpdatePackageError::none ? reader.fail(UpdatePackageError::pe) : false;
}
// The signed package bytes, then its member's bytes, CRC and image.
bool inspect(Reader& reader, const UpdateManifest& manifest, Member* member, u8* sha256) noexcept {
    if (manifest.artifact_bytes > kUpdatePackageMaximumBytes || reader.bytes != manifest.artifact_bytes)
        return reader.fail(UpdatePackageError::size);
    if (!regular(reader.file, manifest.artifact_bytes, &reader.status)) return false;
    u8 package_hash[32]{};
    if (!reader.stream(0, reader.bytes, package_hash, nullptr)) return false;
    if (!bytes_equal(package_hash, manifest.artifact_sha256, 32)) return reader.fail(UpdatePackageError::digest);
    if (!zip(reader, member)) return false;
    u32 crc = 0;
    if (!reader.stream(member->data, member->bytes, sha256, &crc)) return false;
    if (crc != member->crc) return reader.fail(UpdatePackageError::crc);
    return image(reader, member->data, member->bytes);
}
bool readback(HANDLE file, u64 bytes, const u8* sha256, Reader& reader) noexcept {
    reader.file = file; reader.bytes = bytes; reader.cursor = Reader::kUnknownCursor;
    u8 digest[32]{};
    if (!regular(file, bytes, &reader.status) || !reader.stream(0, bytes, digest, nullptr)) return false;
    if (!bytes_equal(digest, sha256, 32)) return reader.fail(UpdatePackageError::digest);
    return image(reader, 0, bytes);
}
// Deletes a partial output through the handles still owned (either one names
// the same file) and closes them.
UpdatePackageResult finish(Reader& reader, HANDLE* files, u32 count) noexcept {
    if (reader.hash) { hash_stream_destroy(reader.hash); reader.hash = nullptr; }
    bool disposed = false;
    for (u32 i = 0; i < count; ++i) if (owner(files[i])) {
        if (!disposed) disposed = file_mark_delete_on_close(files[i]);
        if (!disposed && !reader.status.cleanup_error) reader.status.cleanup_error = thread_last_error();
        if (!file_close(files[i]) && !reader.status.cleanup_error) reader.status.cleanup_error = thread_last_error();
        files[i] = nullptr;
    }
    return reader.status;
}
UpdatePackageResult invalid() noexcept { UpdatePackageResult result{}; result.error = UpdatePackageError::invalid_input; return result; }
} // namespace

UpdatePackageResult update_package_extract(UpdateArtifact* source, const UpdatePackageOffer& offer,
    const P256Key& key, KdfSha256Workspace& workspace, const UpdateManifestPolicy& policy,
    const UpdateAcceptanceTransaction& acceptance, Slice<u32> scratch, ConstSlice<wchar_t> destination,
    UpdatePackage* output) noexcept {
    UpdateManifest manifest{};
    const auto admitted = package_detail::admit(source, offer, key, workspace, policy, acceptance, scratch, destination, output, &manifest);
    if (!admitted.ok()) return admitted;
    Reader reader{source->file, source->bytes, {}};
    Member member{}; UpdatePackage candidate{};
    // owned[0] is the output while writable, owned[1] once sealed.
    HANDLE owned[2]{};
    if (!inspect(reader, manifest, &member, candidate.sha256)) return finish(reader, owned, 2);
    wchar_t path[kUpdateArtifactMaximumPathCharacters + 1];
    copy_bytes(path, destination.data, destination.count * sizeof(wchar_t)); path[destination.count] = 0;
    const HANDLE created = file_open(path, {FileAccess::read_write, FileCreate::create_new, kFileShareRead,
        kFileNoFollow | kFileDeletable});
    if (created == INVALID_HANDLE_VALUE) { reader.fail(UpdatePackageError::create, thread_last_error()); return finish(reader, owned, 2); }
    owned[0] = created;
    if (!regular(owned[0], 0, &reader.status)) return finish(reader, owned, 2);
    u8 digest[32]{};
    if (!reader.stream(member.data, member.bytes, digest, nullptr, owned[0])) return finish(reader, owned, 2);
    if (!bytes_equal(digest, candidate.sha256, 32)) { reader.fail(UpdatePackageError::digest); return finish(reader, owned, 2); }
    if (!file_flush(owned[0])) { reader.fail(UpdatePackageError::flush, thread_last_error()); return finish(reader, owned, 2); }
    // Seal: the same file object with read and DELETE access only; its
    // sharing still denies other writers and deletion.
    if (!file_duplicate_sealed(owned[0], &owned[1])) {
        owned[1] = nullptr; reader.fail(UpdatePackageError::seal, thread_last_error()); return finish(reader, owned, 2);
    }
    if (!file_close(owned[0])) { reader.fail(UpdatePackageError::seal, thread_last_error()); return finish(reader, owned, 2); }
    owned[0] = nullptr;
    if (!readback(owned[1], member.bytes, candidate.sha256, reader)) return finish(reader, owned, 2);
    candidate.file = owned[1]; owned[1] = nullptr;
    candidate.version = manifest.version; candidate.bytes = member.bytes;
    *output = candidate;
    return finish(reader, owned, 2);
}

UpdatePackageResult update_package_release(UpdatePackage* package, bool discard) noexcept {
    if (!package || package->file == INVALID_HANDLE_VALUE) return invalid();
    UpdatePackageResult status{};
    if (package->file) {
        if ((discard && !file_mark_delete_on_close(package->file)) || !file_close(package->file)) {
            status.error = UpdatePackageError::release; status.native_error = thread_last_error(); return status;
        }
    }
    SecureZeroMemory(package, sizeof(*package)); return status;
}
} // namespace maicord
