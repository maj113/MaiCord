#include "maicord/app/update_package.hpp"
#include "maicord/app/acceptance_record.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/sys/crypto.hpp"
#include "maicord/core/win32_image.hpp"

extern "C" __declspec(dllimport) unsigned long __stdcall RtlComputeCrc32(
    unsigned long initial, const void* bytes, unsigned long count);

namespace maicord {
namespace {
struct Member { u64 local, data, bytes; u32 crc; u16 time, date; };
struct Reader {
    HANDLE file;
    u64 bytes;
    u8 buffer[kUpdatePackageReadBufferBytes];
    UpdatePackageResult status{};
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool fail(UpdatePackageError error, DWORD native = 0, i32 crypto = 0) noexcept {
        status.error = error; status.native_error = native; status.crypto_status = crypto; return false;
    }
    bool read(u64 offset, void* output, u32 count) noexcept {
        if (offset > bytes || count > bytes - offset) return fail(UpdatePackageError::size);
        LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return fail(UpdatePackageError::read, GetLastError());
        u32 done = 0;
        while (done < count) {
            DWORD got = 0;
            if (!ReadFile(file, static_cast<u8*>(output) + done, count - done, &got, nullptr))
                return fail(UpdatePackageError::read, GetLastError());
            if (!got || got > count - done) return fail(UpdatePackageError::size);
            done += got;
        }
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
        !GetFileSizeEx(file, &size)) { status->error = UpdatePackageError::stage; status->native_error = GetLastError(); return false; }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1 ||
        size.QuadPart < 0 || static_cast<u64>(size.QuadPart) != bytes) { status->error = UpdatePackageError::size; return false; }
    return true;
}
bool lower_equal(const char* a, const char* b, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes; ++i) if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    return true;
}
bool suffix(Utf8View name, const char* tail) noexcept {
    return name.bytes > 4 && lower_equal(name.data + name.bytes - 4, tail, 4);
}
// Members in package order: their names, local headers, data and CRCs.
struct Members {
    Member entries[kUpdatePackageMaximumMembers];
    char names[kUpdatePackageMaximumMembers][kUpdatePackageMaximumNameBytes];
    u8 name_bytes[kUpdatePackageMaximumMembers];
    u32 count;
};
bool zip(Reader& reader, Members* members) noexcept {
    if (reader.bytes < 22 || reader.bytes > kUpdatePackageMaximumBytes) return reader.fail(UpdatePackageError::zip);
    u8 end[22]{};
    if (!reader.read(reader.bytes - 22, end, sizeof(end))) return false;
    const u64 central = dword(end + 16), central_bytes = dword(end + 12);
    const u32 count = word(end + 8);
    if (dword(end) != 0x06054b50 || word(end + 4) || word(end + 6) || !count || count > kUpdatePackageMaximumMembers ||
        word(end + 10) != count || word(end + 20) || central > reader.bytes - 22 ||
        central_bytes != reader.bytes - 22 - central) return reader.fail(UpdatePackageError::zip);
    u64 next_local = 0, next_central = central;
    for (u32 i = 0; i < count; ++i) {
        u8 local[30]{}, record[46]{}, name[kUpdatePackageMaximumNameBytes]{}, central_name[kUpdatePackageMaximumNameBytes]{};
        if (!reader.read(next_local, local, sizeof(local)) || !reader.read(next_central, record, sizeof(record))) return false;
        const u16 version = word(local + 4);
        const u64 bytes = dword(local + 22);
        const u32 name_bytes = word(local + 26);
        // Explicit producer subset: DOS/NTFS regular files only, version <= 2.0,
        // no flags, attributes, optional fields or ZIP64 sentinels.
        const u16 made = word(record + 4);
        const u32 attributes = dword(record + 38);
        if (dword(local) != 0x04034b50 || (version != 10 && version != 20) || word(local + 6) || word(local + 8) ||
            !bytes || bytes == 0xffffffff || dword(local + 18) != bytes || !name_bytes ||
            name_bytes > kUpdatePackageMaximumNameBytes ||
            word(local + 28) || dword(record) != 0x02014b50 || (made >> 8 != 0 && made >> 8 != 10) ||
            (made & 255) > 20 || word(record + 6) != version || word(record + 8) || word(record + 10) ||
            word(record + 12) != word(local + 10) || word(record + 14) != word(local + 12) ||
            dword(record + 16) != dword(local + 14) || dword(record + 20) != bytes || dword(record + 24) != bytes ||
            word(record + 28) != name_bytes || word(record + 30) || word(record + 32) || word(record + 34) ||
            word(record + 36) || (attributes & ~u32{FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NORMAL}) ||
            dword(record + 42) != next_local) return reader.fail(UpdatePackageError::zip);
        const u64 data = next_local + 30 + name_bytes;
        if (data > central || bytes > central - data || next_central > reader.bytes - 22 ||
            46 + name_bytes > reader.bytes - 22 - next_central) return reader.fail(UpdatePackageError::zip);
        if (!reader.read(next_local + 30, name, name_bytes) || !reader.read(next_central + 46, central_name, name_bytes) ||
            !bytes_equal(name, central_name, name_bytes)) return reader.fail(UpdatePackageError::zip);
        const Utf8View view{reinterpret_cast<const char*>(name), name_bytes};
        // The first member is the launch target.
        if (!update_package_member_name_valid(view) || (i == 0 && (name_bytes != kUpdatePackageExecutable.bytes ||
            !bytes_equal(name, kUpdatePackageExecutable.data, name_bytes)))) return reader.fail(UpdatePackageError::zip);
        for (u32 j = 0; j < i; ++j)
            if (members->name_bytes[j] == name_bytes && lower_equal(members->names[j], view.data, name_bytes))
                return reader.fail(UpdatePackageError::zip);
        members->entries[i] = {next_local, data, bytes, dword(local + 14), word(local + 10), word(local + 12)};
        copy_bytes(members->names[i], name, name_bytes); members->name_bytes[i] = static_cast<u8>(name_bytes);
        next_local = data + bytes; next_central += 46 + name_bytes;
    }
    if (next_local != central || next_central != reader.bytes - 22) return reader.fail(UpdatePackageError::zip);
    members->count = count;
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
    bool verify(bool dll) noexcept {
        u8 dos[64]{}, nt[264]{};
        if (!read(0, dos, sizeof(dos)) || word(dos) != IMAGE_DOS_SIGNATURE) return false;
        const u32 pe = dword(dos + 60);
        if (pe < 64 || !read(pe, nt, sizeof(nt)) || dword(nt) != IMAGE_NT_SIGNATURE ||
            word(nt + 4) != IMAGE_FILE_MACHINE_AMD64 || word(nt + 20) != 240 || word(nt + 24) != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            !(word(nt + 22) & IMAGE_FILE_EXECUTABLE_IMAGE) || !!(word(nt + 22) & IMAGE_FILE_DLL) != dll ||
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
// An .exe or .dll member must be an AMD64 image of that kind.
bool image(Reader& reader, u64 base, u64 bytes, Utf8View name) noexcept {
    const bool exe = suffix(name, ".exe"), dll = suffix(name, ".dll");
    if (!exe && !dll) return true;
    Pe pe{reader, base, bytes, 0, 0, 0};
    return pe.verify(dll);
}
bool inspect(Reader& reader, const UpdateManifest& manifest, Members* members, UpdatePackageDescriptor* descriptor) noexcept {
    if (manifest.artifact_bytes > kUpdatePackageMaximumBytes || reader.bytes != manifest.artifact_bytes)
        return reader.fail(UpdatePackageError::size);
    if (!regular(reader.file, manifest.artifact_bytes, &reader.status)) return false;
    u8 package_hash[32]{};
    if (!reader.stream(0, reader.bytes, package_hash, nullptr)) return false;
    if (!bytes_equal(package_hash, manifest.artifact_sha256, 32)) return reader.fail(UpdatePackageError::digest);
    if (!zip(reader, members)) return false;
    UpdatePackageDescriptor derived{};
    derived.version = manifest.version; derived.channel = manifest.channel;
    derived.package_bytes = manifest.artifact_bytes; derived.member_count = members->count;
    copy_bytes(derived.document_sha256, manifest.manifest_sha256, 32);
    copy_bytes(derived.package_sha256, package_hash, 32);
    for (u32 i = 0; i < members->count; ++i) {
        const Member& m = members->entries[i];
        auto& out = derived.members[i];
        u32 crc = 0; out.bytes = m.bytes;
        copy_bytes(out.name, members->names[i], members->name_bytes[i]); out.name_bytes = members->name_bytes[i];
        if (!reader.stream(m.data, m.bytes, out.sha256, &crc)) return false;
        if (crc != m.crc) return reader.fail(UpdatePackageError::crc);
        if (!image(reader, m.data, m.bytes, update_package_member_name(out)))
            return reader.status.error == UpdatePackageError::none ? reader.fail(UpdatePackageError::pe) : false;
    }
    *descriptor = derived; return true;
}
bool readback(HANDLE file, const UpdatePackageMember& member, Reader& reader) noexcept {
    const HANDLE previous = reader.file; const u64 previous_bytes = reader.bytes;
    reader.file = file; reader.bytes = member.bytes;
    u8 digest[32]{};
    bool ok = regular(file, member.bytes, &reader.status) && reader.stream(0, member.bytes, digest, nullptr);
    if (ok && !bytes_equal(digest, member.sha256, 32)) ok = reader.fail(UpdatePackageError::digest);
    if (ok && !image(reader, 0, member.bytes, update_package_member_name(member))) ok = reader.fail(UpdatePackageError::pe);
    reader.file = previous; reader.bytes = previous_bytes; return ok;
}
UpdatePackageResult finish(Reader& reader, HANDLE* files, u32 count) noexcept {
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
UpdatePackageResult invalid() noexcept { UpdatePackageResult result{}; result.error = UpdatePackageError::invalid_input; return result; }
bool inputs(UpdateStage* stage, const UpdatePackageOffer& offer, const P256Key& key,
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

bool update_package_member_name_valid(Utf8View name) noexcept {
    if (!name.data || !name.bytes || name.bytes > kUpdatePackageMaximumNameBytes || name.data[name.bytes - 1] == '.') return false;
    wchar_t wide[kUpdatePackageMaximumNameBytes]{};
    for (u32 i = 0; i < name.bytes; ++i) {
        const char c = name.data[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '.' && c != '-' && c != '_'))) return false;
        wide[i] = static_cast<wchar_t>(c);
    }
    // The generation directory keeps the signed offer and package beside the members.
    static constexpr Utf8View provenance[]{utf8_literal("offer.json"), utf8_literal("offer.sig"), utf8_literal("package.zip")};
    for (const auto reserved : provenance)
        if (name.bytes == reserved.bytes && lower_equal(name.data, reserved.data, name.bytes)) return false;
    return !acceptance_record_reserved_name(wide, name.bytes);
}
Utf8View update_package_member_name(const UpdatePackageMember& member) noexcept {
    return {member.name, member.name_bytes <= kUpdatePackageMaximumNameBytes ? member.name_bytes : 0u};
}
bool update_package_descriptor_equal(const UpdatePackageDescriptor& a, const UpdatePackageDescriptor& b) noexcept {
    if (a.version != b.version || a.package_bytes != b.package_bytes || a.channel != b.channel || a.member_count != b.member_count ||
        a.member_count > kUpdatePackageMaximumMembers || !bytes_equal(a.document_sha256, b.document_sha256, 32) ||
        !bytes_equal(a.package_sha256, b.package_sha256, 32)) return false;
    for (u32 i = 0; i < a.member_count; ++i) {
        const auto &x = a.members[i], &y = b.members[i];
        if (x.bytes != y.bytes || x.name_bytes != y.name_bytes || x.name_bytes > kUpdatePackageMaximumNameBytes ||
            !bytes_equal(x.sha256, y.sha256, 32) || !bytes_equal(x.name, y.name, x.name_bytes)) return false;
    }
    return true;
}
bool update_package_descriptor_valid(const UpdatePackageDescriptor& p) noexcept {
    if (!p.version || !p.package_bytes || p.package_bytes > kUpdatePackageMaximumBytes || p.channel > UpdateChannel::preview ||
        !p.member_count || p.member_count > kUpdatePackageMaximumMembers ||
        bytes_zero(p.document_sha256, 32) || bytes_zero(p.package_sha256, 32)) return false;
    u64 total = 0;
    for (u32 i = 0; i < kUpdatePackageMaximumMembers; ++i) {
        const auto& m = p.members[i];
        if (i >= p.member_count) {
            if (m.bytes || m.name_bytes || !bytes_zero(m.sha256, 32) || !bytes_zero(reinterpret_cast<const u8*>(m.name), sizeof(m.name)))
                return false;
            continue;
        }
        const Utf8View name = update_package_member_name(m);
        if (!m.bytes || m.bytes > p.package_bytes - total || bytes_zero(m.sha256, 32) || !update_package_member_name_valid(name) ||
            !bytes_zero(reinterpret_cast<const u8*>(m.name) + m.name_bytes, sizeof(m.name) - m.name_bytes) ||
            (i == 0 && (name.bytes != kUpdatePackageExecutable.bytes || !bytes_equal(name.data, kUpdatePackageExecutable.data, name.bytes))))
            return false;
        total += m.bytes;
        for (u32 j = 0; j < i; ++j)
            if (p.members[j].name_bytes == m.name_bytes && lower_equal(p.members[j].name, m.name, m.name_bytes)) return false;
    }
    return true;
}

UpdatePackageResult update_package_extract(UpdateStage* stage, const UpdatePackageOffer& offer,
    const P256Key& key, KdfSha256Workspace& workspace, const UpdateManifestPolicy& policy,
    const UpdateAcceptanceTransaction& acceptance, Slice<u32> scratch, const UpdatePackageTarget& target, UpdatePackage* output) noexcept {
    if (!inputs(stage, offer, key, workspace, scratch, output, sizeof(*output), &policy, sizeof(policy),
        &acceptance, sizeof(acceptance), &target, sizeof(target)) || !target.validate || !target.create || !target.seal) return invalid();
    for (auto file : output->files) if (file) return invalid();
    Reader reader{stage->file, stage->bytes, {}};
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
    if (stage->bytes != manifest.artifact_bytes || stage->version != manifest.version ||
        !bytes_equal(stage->manifest_sha256, manifest.manifest_sha256, 32) ||
        !bytes_equal(stage->sha256, manifest.artifact_sha256, 32)) { reader.fail(UpdatePackageError::stage); return reader.status; }
    constexpr u32 owned_count = kUpdatePackageMaximumMembers * 2;
    Members members{}; UpdatePackage candidate{}; HANDLE owned[owned_count]{};
    if (!inspect(reader, manifest, &members, &candidate.descriptor)) return finish(reader, owned, owned_count);
    DWORD native = 0;
    // owned[i] is member i while writable, owned[max + i] once sealed.
    for (u32 i = 0; i < members.count; ++i) {
        HANDLE& writable = owned[i];
        HANDLE& sealed = owned[kUpdatePackageMaximumMembers + i];
        const auto& member = candidate.descriptor.members[i];
        if (!target.validate(target.context, &native)) { reader.fail(UpdatePackageError::target, native); return finish(reader, owned, owned_count); }
        if (!target.create(target.context, update_package_member_name(member), &writable, &native) || !owner(writable)) {
            reader.fail(UpdatePackageError::create, native); return finish(reader, owned, owned_count);
        }
        if (!regular(writable, 0, &reader.status)) return finish(reader, owned, owned_count);
        u8 digest[32]{};
        if (!reader.stream(members.entries[i].data, members.entries[i].bytes, digest, nullptr, writable)) return finish(reader, owned, owned_count);
        if (!bytes_equal(digest, member.sha256, 32)) { reader.fail(UpdatePackageError::digest); return finish(reader, owned, owned_count); }
        if (!FlushFileBuffers(writable)) { reader.fail(UpdatePackageError::flush, GetLastError()); return finish(reader, owned, owned_count); }
        if (!target.seal(target.context, &writable, &sealed, &native) || writable || !owner(sealed)) {
            reader.fail(UpdatePackageError::seal, native); return finish(reader, owned, owned_count);
        }
        if (!readback(sealed, member, reader)) return finish(reader, owned, owned_count);
    }
    if (!target.validate(target.context, &native)) { reader.fail(UpdatePackageError::target, native); return finish(reader, owned, owned_count); }
    for (u32 i = 0; i < members.count; ++i) {
        candidate.files[i] = owned[kUpdatePackageMaximumMembers + i]; owned[kUpdatePackageMaximumMembers + i] = nullptr;
    }
    *output = candidate; return finish(reader, owned, owned_count);
}

UpdatePackageResult update_package_verify_installed(UpdateStage* stage, const UpdatePackageOffer& offer,
    const P256Key& key, KdfSha256Workspace& workspace, u64 maximum_validity_seconds, Slice<u32> scratch,
    ConstSlice<HANDLE> files, UpdatePackageDescriptor* output) noexcept {
    if (!inputs(stage, offer, key, workspace, scratch, output, sizeof(*output),
        files.data, files.count * static_cast<u32>(sizeof(HANDLE))) || !files.data || !files.count ||
        files.count > kUpdatePackageMaximumMembers) return invalid();
    for (u32 i = 0; i < files.count; ++i) {
        if (!owner(files[i]) || files[i] == stage->file) return invalid();
        for (u32 j = 0; j < i; ++j) if (files[i] == files[j]) return invalid();
    }
    Reader reader{stage->file, stage->bytes, {}};
    UpdateManifest manifest{};
    const auto verified = update_manifest_verify_installed(offer.document, offer.signature, key, workspace,
        kUpdatePackageMaximumBytes, maximum_validity_seconds, scratch, &manifest);
    if (!verified.ok()) { reader.fail(UpdatePackageError::offer); reader.status.manifest_error = verified.error; return reader.status; }
    if (stage->bytes != manifest.artifact_bytes) { reader.fail(UpdatePackageError::size); return reader.status; }
    Members members{}; UpdatePackageDescriptor descriptor{};
    if (!inspect(reader, manifest, &members, &descriptor)) return finish(reader, nullptr, 0);
    if (descriptor.member_count != files.count) { reader.fail(UpdatePackageError::size); return finish(reader, nullptr, 0); }
    for (u32 i = 0; i < files.count; ++i)
        if (!readback(files[i], descriptor.members[i], reader)) return finish(reader, nullptr, 0);
    *output = descriptor; return finish(reader, nullptr, 0);
}

UpdatePackageResult update_package_release(UpdatePackage* package, bool discard) noexcept {
    if (!package) return invalid();
    for (auto file : package->files) if (file == INVALID_HANDLE_VALUE) return invalid();
    UpdatePackageResult status{};
    for (auto& file : package->files) if (file) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if ((discard && !SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) || !CloseHandle(file)) {
            status.error = UpdatePackageError::release; status.native_error = GetLastError(); return status;
        }
        file = nullptr;
    }
    SecureZeroMemory(package, sizeof(*package)); return status;
}
UpdatePackageResult update_package_cleanup_release(UpdatePackageResult* failed) noexcept {
    if (!failed) return invalid();
    UpdatePackageResult status{};
    if (failed->cleanup_hash) {
        const NTSTATUS native = BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(failed->cleanup_hash));
        if (native < 0) { status.error = UpdatePackageError::hash; status.crypto_status = native; return status; }
        failed->cleanup_hash = nullptr;
    }
    for (auto& file : failed->cleanup_files) if (file) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) || !CloseHandle(file)) {
            status.error = UpdatePackageError::release; status.native_error = GetLastError(); failed->cleanup_error = status.native_error; return status;
        }
        file = nullptr;
    }
    failed->cleanup_error = 0; return status;
}
} // namespace maicord
