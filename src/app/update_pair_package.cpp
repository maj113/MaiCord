#include "maicord/app/update_pair_package.hpp"
#include "maicord/core/byte_range.hpp"
#include <Windows.h>
#include <bcrypt.h>

extern "C" __declspec(dllimport) unsigned long __stdcall RtlComputeCrc32(
    unsigned long initial, const void* bytes, unsigned long count);

namespace maicord {
namespace {
struct Member { u64 local, data, bytes; u32 crc; u16 time, date; };
struct Reader {
    HANDLE file;
    u64 bytes;
    u8 buffer[kUpdatePairReadBufferBytes];
    UpdatePairResult status{};
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool fail(UpdatePairError error, DWORD native = 0, i32 crypto = 0) noexcept {
        status.error = error; status.native_error = native; status.crypto_status = crypto; return false;
    }
    bool read(u64 offset, void* output, u32 count) noexcept {
        if (offset > bytes || count > bytes - offset) return fail(UpdatePairError::size);
        LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return fail(UpdatePairError::read, GetLastError());
        u32 done = 0;
        while (done < count) {
            DWORD got = 0;
            if (!ReadFile(file, static_cast<u8*>(output) + done, count - done, &got, nullptr))
                return fail(UpdatePairError::read, GetLastError());
            if (!got || got > count - done) return fail(UpdatePairError::size);
            done += got;
        }
        return true;
    }
    bool begin_hash() noexcept {
        const NTSTATUS native = BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0);
        return native >= 0 || fail(UpdatePairError::hash, 0, native);
    }
    bool finish_hash(u8* digest) noexcept {
        NTSTATUS native = BCryptFinishHash(hash, digest, 32, 0);
        if (native < 0) return fail(UpdatePairError::hash, 0, native);
        native = BCryptDestroyHash(hash);
        if (native < 0) return fail(UpdatePairError::hash, 0, native);
        hash = nullptr; return true;
    }
    bool stream(u64 offset, u64 count, u8* digest, u32* crc, HANDLE destination = nullptr) noexcept {
        if (!begin_hash()) return false;
        u32 check = 0;
        while (count) {
            const u32 chunk = static_cast<u32>(minimum<u64>(count, sizeof(buffer)));
            if (!read(offset, buffer, chunk)) return false;
            const NTSTATUS native = BCryptHashData(hash, buffer, chunk, 0);
            if (native < 0) return fail(UpdatePairError::hash, 0, native);
            // Chain the published IEEE CRC over the same bounded read chunks.
            if (crc) check = static_cast<u32>(RtlComputeCrc32(check, buffer, chunk));
            if (destination) {
                u32 done = 0;
                while (done < chunk) {
                    DWORD wrote = 0;
                    if (!WriteFile(destination, buffer + done, chunk - done, &wrote, nullptr))
                        return fail(UpdatePairError::write, GetLastError());
                    if (!wrote || wrote > chunk - done) return fail(UpdatePairError::write);
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
bool regular(HANDLE file, u64 bytes, UpdatePairResult* status) noexcept {
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
    if (!owner(file) || GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
        !GetFileSizeEx(file, &size)) { status->error = UpdatePairError::stage; status->native_error = GetLastError(); return false; }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1 ||
        size.QuadPart < 0 || static_cast<u64>(size.QuadPart) != bytes) { status->error = UpdatePairError::size; return false; }
    return true;
}
constexpr char kNames[2][19]{"MaiCord.exe", "MaiCordAccount.dll"};
constexpr u32 kNameBytes[2]{11, 18};
bool zip(Reader& reader, Member* members) noexcept {
    if (reader.bytes < 22 || reader.bytes > kUpdatePairMaximumPackageBytes) return reader.fail(UpdatePairError::zip);
    u8 end[22]{};
    if (!reader.read(reader.bytes - 22, end, sizeof(end))) return false;
    const u64 central = dword(end + 16), central_bytes = dword(end + 12);
    if (dword(end) != 0x06054b50 || word(end + 4) || word(end + 6) || word(end + 8) != 2 ||
        word(end + 10) != 2 || word(end + 20) || central > reader.bytes - 22 ||
        central_bytes != reader.bytes - 22 - central) return reader.fail(UpdatePairError::zip);
    u64 next_local = 0, next_central = central;
    for (u32 i = 0; i < 2; ++i) {
        u8 local[30]{}, record[46]{}, name[18]{};
        if (!reader.read(next_local, local, sizeof(local)) || !reader.read(next_central, record, sizeof(record))) return false;
        const u16 version = word(local + 4);
        const u64 bytes = dword(local + 22);
        // Explicit producer subset: DOS/NTFS regular files only, version <= 2.0,
        // no flags, attributes, optional fields or ZIP64 sentinels.
        const u16 made = word(record + 4);
        const u32 attributes = dword(record + 38);
        if (dword(local) != 0x04034b50 || (version != 10 && version != 20) || word(local + 6) || word(local + 8) ||
            !bytes || bytes == 0xffffffff || dword(local + 18) != bytes || word(local + 26) != kNameBytes[i] ||
            word(local + 28) || dword(record) != 0x02014b50 || (made >> 8 != 0 && made >> 8 != 10) ||
            (made & 255) > 20 || word(record + 6) != version || word(record + 8) || word(record + 10) ||
            word(record + 12) != word(local + 10) || word(record + 14) != word(local + 12) ||
            dword(record + 16) != dword(local + 14) || dword(record + 20) != bytes || dword(record + 24) != bytes ||
            word(record + 28) != kNameBytes[i] || word(record + 30) || word(record + 32) || word(record + 34) ||
            word(record + 36) || (attributes & ~u32{FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NORMAL}) ||
            dword(record + 42) != next_local) return reader.fail(UpdatePairError::zip);
        const u64 data = next_local + 30 + kNameBytes[i];
        if (data > central || bytes > central - data || next_central > reader.bytes - 22 ||
            46 + kNameBytes[i] > reader.bytes - 22 - next_central) return reader.fail(UpdatePairError::zip);
        if (!reader.read(next_local + 30, name, kNameBytes[i]) || !bytes_equal(name, kNames[i], kNameBytes[i]) ||
            !reader.read(next_central + 46, name, kNameBytes[i]) || !bytes_equal(name, kNames[i], kNameBytes[i]))
            return reader.fail(UpdatePairError::zip);
        members[i] = {next_local, data, bytes, dword(local + 14), word(local + 10), word(local + 12)};
        next_local = data + bytes; next_central += 46 + kNameBytes[i];
    }
    if (next_local != central || next_central != reader.bytes - 22) return reader.fail(UpdatePairError::zip);
    return true;
}
struct Pe {
    Reader& reader;
    u64 base, bytes, sections;
    u32 count, headers;
    u16 ordinal; // DLL: the first exported ordinal (the account module API entry)
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
    bool verify(bool dll) noexcept {
        u8 dos[64]{}, nt[264]{};
        if (!read(0, dos, sizeof(dos)) || word(dos) != IMAGE_DOS_SIGNATURE) return false;
        const u32 pe = dword(dos + 60);
        if (pe < 64 || !read(pe, nt, sizeof(nt)) || dword(nt) != IMAGE_NT_SIGNATURE ||
            word(nt + 4) != IMAGE_FILE_MACHINE_AMD64 || word(nt + 20) != 240 || word(nt + 24) != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            !(word(nt + 22) & IMAGE_FILE_EXECUTABLE_IMAGE) || !!(word(nt + 22) & IMAGE_FILE_DLL) != dll ||
            dword(nt + 132) != 16 || !dword(nt + 40)) return false;
        count = word(nt + 6); sections = static_cast<u64>(pe) + sizeof(nt); headers = dword(nt + 84);
        if (!count || count > kUpdatePairMaximumSections || sections > bytes || count * 40 > bytes - sections ||
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
        if (!dll) return true;
        const u32 export_rva = dword(nt + 136), export_bytes = dword(nt + 140);
        u8 exports[40]{}, function[4]{};
        if (export_bytes < 40 || !map(export_rva, export_bytes, &offset) || !read(offset, exports, sizeof(exports))) return false;
        // Any account API version is accepted. The installer must not pin
        // its own API ordinal: every release that changes the account API
        // exports a new one, and an installed dispatcher never changes. The
        // package is publisher-signed, and its own EXE admits the DLL's API
        // when it confirms startup, before the update is kept.
        const u32 ordinal_base = dword(exports + 16), functions = dword(exports + 20);
        if (!functions || functions > 65536 || !ordinal_base || ordinal_base > 0xffff ||
            !map(dword(exports + 28), static_cast<u64>(functions) * 4, &offset) ||
            !read(offset, function, sizeof(function))) return false;
        ordinal = static_cast<u16>(ordinal_base);
        const u32 address = dword(function);
        return address && !(address >= export_rva && static_cast<u64>(address) - export_rva < export_bytes) &&
            map(address, 1, &offset, true);
    }
};
bool inspect(Reader& reader, const UpdateManifest& manifest, Member* members, UpdatePairDescriptor* descriptor) noexcept {
    if (manifest.artifact_bytes > kUpdatePairMaximumPackageBytes || reader.bytes != manifest.artifact_bytes)
        return reader.fail(UpdatePairError::size);
    if (!regular(reader.file, manifest.artifact_bytes, &reader.status)) return false;
    u8 package_hash[32]{};
    if (!reader.stream(0, reader.bytes, package_hash, nullptr)) return false;
    if (!bytes_equal(package_hash, manifest.artifact_sha256, 32)) return reader.fail(UpdatePairError::digest);
    if (!zip(reader, members)) return false;
    UpdatePairDescriptor derived{};
    derived.version = manifest.version; derived.channel = manifest.channel;
    derived.package_bytes = manifest.artifact_bytes;
    copy_bytes(derived.document_sha256, manifest.manifest_sha256, 32);
    copy_bytes(derived.package_sha256, package_hash, 32);
    for (u32 i = 0; i < 2; ++i) {
        u32 crc = 0; derived.members[i].bytes = members[i].bytes;
        if (!reader.stream(members[i].data, members[i].bytes, derived.members[i].sha256, &crc)) return false;
        if (crc != members[i].crc) return reader.fail(UpdatePairError::crc);
        Pe pe{reader, members[i].data, members[i].bytes, 0, 0, 0, 0};
        if (!pe.verify(i == 1)) return reader.status.error == UpdatePairError::none ? reader.fail(UpdatePairError::pe) : false;
        if (i == 1) derived.account_ordinal = pe.ordinal;
    }
    *descriptor = derived; return true;
}
bool readback(HANDLE file, const UpdatePairMember& member, bool dll, Reader& reader) noexcept {
    const HANDLE previous = reader.file; const u64 previous_bytes = reader.bytes;
    reader.file = file; reader.bytes = member.bytes;
    u8 digest[32]{};
    bool ok = regular(file, member.bytes, &reader.status) && reader.stream(0, member.bytes, digest, nullptr);
    if (ok && !bytes_equal(digest, member.sha256, 32)) ok = reader.fail(UpdatePairError::digest);
    if (ok) { Pe pe{reader, 0, member.bytes, 0, 0, 0, 0}; if (!pe.verify(dll)) ok = reader.fail(UpdatePairError::pe); }
    reader.file = previous; reader.bytes = previous_bytes; return ok;
}
UpdatePairResult finish(Reader& reader, HANDLE* files, u32 count) noexcept {
    if (reader.hash) {
        const NTSTATUS native = BCryptDestroyHash(reader.hash);
        if (native < 0) { reader.status.cleanup_hash = reader.hash; reader.status.crypto_status = native; }
        reader.hash = nullptr;
    }
    for (u32 i = 0; i < count; ++i) if (owner(files[i])) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(files[i], FileDispositionInfo, &disposition, sizeof(disposition)) || !CloseHandle(files[i])) {
            if (!reader.status.cleanup_error) reader.status.cleanup_error = GetLastError();
            reader.status.cleanup_files[i] = files[i];
        }
        files[i] = nullptr;
    }
    return reader.status;
}
UpdatePairResult invalid() noexcept { UpdatePairResult result{}; result.error = UpdatePairError::invalid_input; return result; }
bool inputs(UpdateStage* stage, const UpdatePairOffer& offer, const P256Key& key,
    KdfSha256Workspace& workspace, Slice<u32> scratch, const void* output, u32 output_bytes,
    const void* extra1 = nullptr, u32 extra1_bytes = 0, const void* extra2 = nullptr, u32 extra2_bytes = 0,
    const void* extra3 = nullptr, u32 extra3_bytes = 0) noexcept {
    if (!stage || !owner(stage->file) || !output || !offer.document.data || !offer.document.bytes ||
        offer.document.bytes > kUpdateManifestMaximumBytes || !offer.signature.data || offer.signature.count != 64 ||
        !scratch.data || !scratch.count || scratch.count > ~u32{0} / sizeof(u32)) return false;
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(stage), sizeof(*stage)}, {reinterpret_cast<const u8*>(&offer), sizeof(offer)},
        {reinterpret_cast<const u8*>(offer.document.data), offer.document.bytes}, offer.signature,
        {reinterpret_cast<const u8*>(&key), sizeof(key)}, {reinterpret_cast<const u8*>(&workspace), sizeof(workspace)},
        {workspace.storage.data, workspace.storage.capacity},
        {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))},
        {static_cast<const u8*>(output), output_bytes}, {static_cast<const u8*>(extra1), extra1_bytes},
        {static_cast<const u8*>(extra2), extra2_bytes}, {static_cast<const u8*>(extra3), extra3_bytes}};
    return byte_ranges_all_disjoint({ranges, sizeof(ranges) / sizeof(ranges[0])});
}
} // namespace

UpdatePairResult update_pair_package_extract(UpdateStage* stage, const UpdatePairOffer& offer,
    const P256Key& key, KdfSha256Workspace& workspace, const UpdateManifestPolicy& policy,
    const UpdateAcceptanceTransaction& acceptance, Slice<u32> scratch, const UpdatePairTarget& target, UpdatePair* output) noexcept {
    if (!inputs(stage, offer, key, workspace, scratch, output, sizeof(*output), &policy, sizeof(policy),
        &acceptance, sizeof(acceptance), &target, sizeof(target)) || output->files[0] || output->files[1] ||
        !target.validate || !target.create || !target.seal) return invalid();
    Reader reader{stage->file, stage->bytes, {}};
    UpdateManifestPolicy captured{}; UpdateManifest manifest{};
    if (!update_acceptance_capture_policy(acceptance, policy, &captured).ok()) {
        reader.fail(UpdatePairError::acceptance); return reader.status;
    }
    const auto verified = update_manifest_verify(offer.document, offer.signature, key, workspace, captured, scratch, &manifest);
    if (!verified.ok()) { reader.fail(UpdatePairError::offer); reader.status.manifest_error = verified.error; return reader.status; }
    const auto& admitted = acceptance.state.channels[static_cast<u32>(manifest.channel)];
    if (!acceptance.record_present || !admitted.present || admitted.highest_version != manifest.version ||
        !bytes_equal(admitted.manifest_sha256, manifest.manifest_sha256, 32)) {
        reader.fail(UpdatePairError::acceptance); return reader.status;
    }
    if (stage->bytes != manifest.artifact_bytes || stage->version != manifest.version ||
        !bytes_equal(stage->manifest_sha256, manifest.manifest_sha256, 32) ||
        !bytes_equal(stage->sha256, manifest.artifact_sha256, 32)) { reader.fail(UpdatePairError::stage); return reader.status; }
    Member members[2]{}; UpdatePair candidate{}; HANDLE owned[4]{};
    if (!inspect(reader, manifest, members, &candidate.descriptor)) return finish(reader, owned, 4);
    DWORD native = 0;
    for (u32 i = 0; i < 2; ++i) {
        if (!target.validate(target.context, &native)) { reader.fail(UpdatePairError::target, native); return finish(reader, owned, 4); }
        if (!target.create(target.context, i, &owned[i], &native) || !owner(owned[i])) {
            reader.fail(UpdatePairError::create, native); return finish(reader, owned, 4);
        }
        if (!regular(owned[i], 0, &reader.status)) return finish(reader, owned, 4);
        u8 digest[32]{};
        if (!reader.stream(members[i].data, members[i].bytes, digest, nullptr, owned[i])) return finish(reader, owned, 4);
        if (!bytes_equal(digest, candidate.descriptor.members[i].sha256, 32)) {
            reader.fail(UpdatePairError::digest); return finish(reader, owned, 4);
        }
        if (!FlushFileBuffers(owned[i])) { reader.fail(UpdatePairError::flush, GetLastError()); return finish(reader, owned, 4); }
        if (!target.seal(target.context, i, &owned[i], &owned[i + 2], &native) || owned[i] || !owner(owned[i + 2])) {
            reader.fail(UpdatePairError::seal, native); return finish(reader, owned, 4);
        }
        if (!readback(owned[i + 2], candidate.descriptor.members[i], i == 1, reader)) return finish(reader, owned, 4);
    }
    if (!target.validate(target.context, &native)) { reader.fail(UpdatePairError::target, native); return finish(reader, owned, 4); }
    candidate.files[0] = owned[2]; candidate.files[1] = owned[3]; owned[2] = owned[3] = nullptr;
    *output = candidate; return finish(reader, owned, 4);
}

UpdatePairResult update_pair_package_verify_installed(UpdateStage* stage, const UpdatePairOffer& offer,
    const P256Key& key, KdfSha256Workspace& workspace, u64 maximum_validity_seconds, Slice<u32> scratch,
    HANDLE executable, HANDLE account, UpdatePairDescriptor* output) noexcept {
    if (!inputs(stage, offer, key, workspace, scratch, output, sizeof(*output)) || !owner(executable) || !owner(account) ||
        executable == account || executable == stage->file || account == stage->file) return invalid();
    Reader reader{stage->file, stage->bytes, {}};
    UpdateManifest manifest{};
    const auto verified = update_manifest_verify_installed(offer.document, offer.signature, key, workspace,
        kUpdatePairMaximumPackageBytes, maximum_validity_seconds, scratch, &manifest);
    if (!verified.ok()) { reader.fail(UpdatePairError::offer); reader.status.manifest_error = verified.error; return reader.status; }
    if (stage->bytes != manifest.artifact_bytes) { reader.fail(UpdatePairError::size); return reader.status; }
    Member members[2]{}; UpdatePairDescriptor descriptor{};
    if (!inspect(reader, manifest, members, &descriptor) || !readback(executable, descriptor.members[0], false, reader) ||
        !readback(account, descriptor.members[1], true, reader)) return finish(reader, nullptr, 0);
    *output = descriptor; return finish(reader, nullptr, 0);
}

UpdatePairResult update_pair_release(UpdatePair* pair, bool discard) noexcept {
    if (!pair || pair->files[0] == INVALID_HANDLE_VALUE || pair->files[1] == INVALID_HANDLE_VALUE) return invalid();
    UpdatePairResult status{};
    for (auto& file : pair->files) if (file) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if ((discard && !SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) || !CloseHandle(file)) {
            status.error = UpdatePairError::release; status.native_error = GetLastError(); return status;
        }
        file = nullptr;
    }
    SecureZeroMemory(pair, sizeof(*pair)); return status;
}
UpdatePairResult update_pair_cleanup_release(UpdatePairResult* failed) noexcept {
    if (!failed) return invalid();
    UpdatePairResult status{};
    if (failed->cleanup_hash) {
        const NTSTATUS native = BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(failed->cleanup_hash));
        if (native < 0) { status.error = UpdatePairError::hash; status.crypto_status = native; return status; }
        failed->cleanup_hash = nullptr;
    }
    for (auto& file : failed->cleanup_files) if (file) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) || !CloseHandle(file)) {
            status.error = UpdatePairError::release; status.native_error = GetLastError(); failed->cleanup_error = status.native_error; return status;
        }
        file = nullptr;
    }
    failed->cleanup_error = 0; return status;
}
} // namespace maicord
