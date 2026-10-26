#include "maicord/app/update_package.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/sys/crypto.hpp"
#include "maicord/core/win32_image.hpp"

extern "C" __declspec(dllimport) unsigned long __stdcall RtlComputeCrc32(
    unsigned long initial, const void* bytes, unsigned long count);

namespace maicord {
namespace {
struct Member { u64 data, bytes; u32 crc; };
struct Reader {
    HANDLE file;
    u64 bytes;
    u8 buffer[kUpdatePackageReadBufferBytes];
    UpdatePackageResult status{};
    BCRYPT_HASH_HANDLE hash = nullptr;
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
            LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
            if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return fail(UpdatePackageError::read, GetLastError());
        }
        cursor = kUnknownCursor;
        u32 done = 0;
        while (done < count) {
            DWORD got = 0;
            if (!ReadFile(file, static_cast<u8*>(output) + done, count - done, &got, nullptr))
                return fail(UpdatePackageError::read, GetLastError());
            if (!got || got > count - done) return fail(UpdatePackageError::size);
            done += got;
        }
        cursor = offset + count;
        return true;
    }
    bool begin_hash() noexcept {
        const NTSTATUS native = BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0);
        return native >= 0 || fail(UpdatePackageError::hash, 0, native);
    }
    bool finish_hash(u8* digest) noexcept {
        NTSTATUS native = BCryptFinishHash(hash, digest, 32, 0);
        if (native < 0) return fail(UpdatePackageError::hash, 0, native);
        native = BCryptDestroyHash(hash);
        if (native < 0) return fail(UpdatePackageError::hash, 0, native);
        hash = nullptr; return true;
    }
    bool stream(u64 offset, u64 count, u8* digest, u32* crc, HANDLE destination = nullptr) noexcept {
        if (!begin_hash()) return false;
        u32 check = 0;
        while (count) {
            const u32 chunk = static_cast<u32>(minimum<u64>(count, sizeof(buffer)));
            if (!read(offset, buffer, chunk)) return false;
            const NTSTATUS native = BCryptHashData(hash, buffer, chunk, 0);
            if (native < 0) return fail(UpdatePackageError::hash, 0, native);
            // Chain the published IEEE CRC over the same bounded read chunks.
            if (crc) check = static_cast<u32>(RtlComputeCrc32(check, buffer, chunk));
            if (destination) {
                u32 done = 0;
                while (done < chunk) {
                    DWORD wrote = 0;
                    if (!WriteFile(destination, buffer + done, chunk - done, &wrote, nullptr))
                        return fail(UpdatePackageError::write, GetLastError());
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
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
    if (!owner(file) || GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
        !GetFileSizeEx(file, &size)) { status->error = UpdatePackageError::file; status->native_error = GetLastError(); return false; }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1 ||
        size.QuadPart < 0 || static_cast<u64>(size.QuadPart) != bytes) { status->error = UpdatePackageError::size; return false; }
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
        word(record + 36) || (attributes & ~u32{FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NORMAL}) ||
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
    bool map(u32 rva, u64 size, u64* offset, bool executable = false) noexcept {
        for (u32 i = 0; i < count; ++i) {
            u8 sec[40]{}; if (!section(i, sec)) return false;
            const u64 start = dword(sec + 12), raw = dword(sec + 16);
            if (rva >= start && rva - start < raw && size <= raw - (rva - start) &&
                (!executable || (dword(sec + 36) & IMAGE_SCN_MEM_EXECUTE))) {
                *offset = dword(sec + 20) + (rva - start); return true;
            }
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
        u64 offset = 0;
        if (!map(dword(nt + 40), 1, &offset, true)) return false;
        return true;
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
    if (reader.hash) { BCryptDestroyHash(reader.hash); reader.hash = nullptr; }
    bool disposed = false;
    for (u32 i = 0; i < count; ++i) if (owner(files[i])) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!disposed) disposed = SetFileInformationByHandle(files[i], FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
        if (!disposed && !reader.status.cleanup_error) reader.status.cleanup_error = GetLastError();
        if (!CloseHandle(files[i]) && !reader.status.cleanup_error) reader.status.cleanup_error = GetLastError();
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
    if (!source || !owner(source->file) || !output || output->file || !offer.document.data || !offer.document.bytes ||
        offer.document.bytes > kUpdateManifestMaximumBytes || !offer.signature.data || offer.signature.count != 64 ||
        !scratch.data || !scratch.count || scratch.count > ~u32{0} / sizeof(u32) ||
        !destination.data || !destination.count || destination.count > kUpdateArtifactMaximumPathCharacters) return invalid();
    for (u32 i = 0; i < destination.count; ++i) if (!destination[i]) return invalid();
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(source), sizeof(*source)}, {reinterpret_cast<const u8*>(&offer), sizeof(offer)},
        {reinterpret_cast<const u8*>(offer.document.data), offer.document.bytes}, offer.signature,
        {reinterpret_cast<const u8*>(&key), sizeof(key)}, {reinterpret_cast<const u8*>(&workspace), sizeof(workspace)},
        {workspace.storage.data, workspace.storage.capacity},
        {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))},
        {reinterpret_cast<const u8*>(output), sizeof(*output)}, {reinterpret_cast<const u8*>(&policy), sizeof(policy)},
        {reinterpret_cast<const u8*>(&acceptance), sizeof(acceptance)},
        {reinterpret_cast<const u8*>(destination.data), destination.count * static_cast<u32>(sizeof(wchar_t))}};
    if (!byte_ranges_all_disjoint({ranges, sizeof(ranges) / sizeof(ranges[0])})) return invalid();
    Reader reader{source->file, source->bytes, {}};
    UpdateManifestPolicy captured{}; UpdateManifest manifest{};
    if (!update_acceptance_capture_policy(acceptance, policy, &captured).ok()) {
        reader.fail(UpdatePackageError::acceptance); return reader.status;
    }
    const auto verified = update_manifest_verify(offer.document, offer.signature, key, workspace, captured, scratch, &manifest);
    if (!verified.ok()) { reader.fail(UpdatePackageError::offer); reader.status.manifest_error = verified.error; return reader.status; }
    const auto& admitted = acceptance.state.channels[static_cast<u32>(manifest.channel)];
    if (!acceptance.record_present || !admitted.present || admitted.highest_version != manifest.version ||
        !bytes_equal(admitted.manifest_sha256, manifest.manifest_sha256, 32)) {
        reader.fail(UpdatePackageError::acceptance); return reader.status;
    }
    if (source->bytes != manifest.artifact_bytes || source->version != manifest.version ||
        !bytes_equal(source->manifest_sha256, manifest.manifest_sha256, 32) ||
        !bytes_equal(source->sha256, manifest.artifact_sha256, 32)) { reader.fail(UpdatePackageError::source); return reader.status; }
    Member member{}; UpdatePackage candidate{};
    // owned[0] is the output while writable, owned[1] once sealed.
    HANDLE owned[2]{};
    if (!inspect(reader, manifest, &member, candidate.sha256)) return finish(reader, owned, 2);
    wchar_t path[kUpdateArtifactMaximumPathCharacters + 1];
    copy_bytes(path, destination.data, destination.count * sizeof(wchar_t)); path[destination.count] = 0;
    const HANDLE created = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (created == INVALID_HANDLE_VALUE) { reader.fail(UpdatePackageError::create, GetLastError()); return finish(reader, owned, 2); }
    owned[0] = created;
    if (!regular(owned[0], 0, &reader.status)) return finish(reader, owned, 2);
    u8 digest[32]{};
    if (!reader.stream(member.data, member.bytes, digest, nullptr, owned[0])) return finish(reader, owned, 2);
    if (!bytes_equal(digest, candidate.sha256, 32)) { reader.fail(UpdatePackageError::digest); return finish(reader, owned, 2); }
    if (!FlushFileBuffers(owned[0])) { reader.fail(UpdatePackageError::flush, GetLastError()); return finish(reader, owned, 2); }
    // Seal: the same file object with read and DELETE access only; its
    // sharing still denies other writers and deletion.
    if (!DuplicateHandle(GetCurrentProcess(), owned[0], GetCurrentProcess(), &owned[1], GENERIC_READ | DELETE, FALSE, 0)) {
        owned[1] = nullptr; reader.fail(UpdatePackageError::seal, GetLastError()); return finish(reader, owned, 2);
    }
    if (!CloseHandle(owned[0])) { reader.fail(UpdatePackageError::seal, GetLastError()); return finish(reader, owned, 2); }
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
        FILE_DISPOSITION_INFO disposition{TRUE};
        if ((discard && !SetFileInformationByHandle(package->file, FileDispositionInfo, &disposition, sizeof(disposition))) ||
            !CloseHandle(package->file)) {
            status.error = UpdatePackageError::release; status.native_error = GetLastError(); return status;
        }
    }
    SecureZeroMemory(package, sizeof(*package)); return status;
}
} // namespace maicord
