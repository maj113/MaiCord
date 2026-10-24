#include "maicord/app/update_handoff.hpp"
#include "maicord/core/byte_range.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process_launch.hpp"

namespace maicord {
namespace {
constexpr wchar_t protocol_flag[] = L"--maicord-update-child-v1";
constexpr u32 flag_characters = sizeof(protocol_flag) / sizeof(wchar_t) - 1;
// Quoted maximum fixed generation path, flag, IDs, three 64-bit handles,
// separators and terminator. Changing any protocol/path capacity fails compile.
static_assert(kUpdateHandoffCommandCharacters >= kUpdateGenerationMaximumRootCharacters + 96 +
    2 + 1 + flag_characters + 1 + 32 + 1 + 32 + 1 + 64 + 1 + 16 + 1 + 16 + 1 + 16 + 1);
constexpr LONG ack_committed = 0x3141434d;
struct AckSlot { volatile LONG committed; u32 protocol; UpdateHandoffIdentity identity; u8 reserved[56]; };
static_assert(sizeof(AckSlot) == kUpdateHandoffAcknowledgementBytes);
UpdateHandoffResult fail(UpdateHandoffError e, DWORD n = ERROR_INVALID_PARAMETER) noexcept { return {e, n, UpdateHandoffOutcome::none}; }
bool equal(const UpdateHandoffIdentity& a, const UpdateHandoffIdentity& b) noexcept {
    return bytes_equal(a.installation.bytes, b.installation.bytes, 16) &&
        bytes_equal(a.transaction.bytes, b.transaction.bytes, 16) && bytes_equal(a.document_sha256, b.document_sha256, 32);
}
bool identity_valid(const UpdateHandoffIdentity& value) noexcept {
    u8 installation = 0, digest = 0;
    for (u32 i = 0; i < 16; ++i) installation |= value.installation.bytes[i];
    for (auto b : value.document_sha256) digest |= b;
    return installation && digest;
}
bool empty(const UpdateHandoff& o) noexcept {
    return !o.process && !o.thread && !o.parent && !o.mapping && !o.event && !o.child_mapping &&
        !o.child_event && !o.validation && !o.view && !o.launched;
}
bool close(HANDLE& h) noexcept { if (!h) return true; if (!CloseHandle(h)) return false; h = nullptr; return true; }
bool handle_ok(HANDLE h) noexcept { return h && h != INVALID_HANDLE_VALUE; }
bool absolute(const wchar_t* p, u32 n) noexcept {
    if (!p || n < 4 || n > kUpdateGenerationMaximumRootCharacters + 96 ||
        !((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) || p[1] != L':' || p[2] != L'\\') return false;
    for (u32 i = 0; i < n; ++i) if (!p[i] || p[i] == L'"' || p[i] < 32 || p[i] == L'/' || (i > 1 && p[i] == L':')) return false;
    return !p[n];
}
void hex(wchar_t* out, u32& count, const u8* bytes, u32 n) noexcept {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (u32 i = 0; i < n; ++i) { out[count++] = digits[bytes[i] >> 4]; out[count++] = digits[bytes[i] & 15]; }
}
void handle_text(wchar_t* out, u32& count, HANDLE h) noexcept {
    constexpr wchar_t digits[] = L"0123456789abcdef"; const u64 value = reinterpret_cast<u64>(h);
    for (u32 i = 0; i < 16; ++i) out[count++] = digits[(value >> ((15 - i) * 4)) & 15];
}
// Reused by the test-only second compilation with a known fixture executable.
UpdateHandoffResult launch_native(const wchar_t* path, u32 path_chars,
    const UpdateHandoffIdentity& id, UpdateHandoff* owner) noexcept {
    if (!owner || !empty(*owner) || !identity_valid(id) || !absolute(path, path_chars)) return fail(UpdateHandoffError::invalid_input);
    owner->expected = id;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &owner->parent,
        SYNCHRONIZE, TRUE, 0)) return fail(UpdateHandoffError::parent, GetLastError());
    owner->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(AckSlot), nullptr);
    if (!owner->mapping) return fail(UpdateHandoffError::channel, GetLastError());
    owner->view = MapViewOfFile(owner->mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(AckSlot));
    if (!owner->view) return fail(UpdateHandoffError::channel, GetLastError());
    SecureZeroMemory(owner->view, sizeof(AckSlot));
    owner->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!owner->event || !DuplicateHandle(GetCurrentProcess(), owner->mapping, GetCurrentProcess(), &owner->child_mapping,
        FILE_MAP_READ | FILE_MAP_WRITE, TRUE, 0) ||
        !DuplicateHandle(GetCurrentProcess(), owner->event, GetCurrentProcess(), &owner->child_event, EVENT_MODIFY_STATE, TRUE, 0))
        return fail(UpdateHandoffError::channel, GetLastError());
    // Attribute lists are opaque. Query the required size; refuse if the current
    // Windows ABI needs more than this fixed stack budget. No allocator fallback.
    alignas(16) u8 attributes[512]{}; SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (!bytes || bytes > sizeof(attributes)) return fail(UpdateHandoffError::attributes, ERROR_INSUFFICIENT_BUFFER);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes);
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) return fail(UpdateHandoffError::attributes, GetLastError());
    HANDLE inherited[]{owner->parent, owner->child_mapping, owner->child_event};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        const DWORD error = GetLastError(); DeleteProcThreadAttributeList(list); return fail(UpdateHandoffError::attributes, error);
    }
    wchar_t command[kUpdateHandoffCommandCharacters]{}; u32 count = 0;
    command[count++] = L'"';
    for (u32 i = 0; i < path_chars; ++i) command[count++] = path[i];
    command[count++] = L'"'; command[count++] = L' ';
    for (u32 i = 0; i < flag_characters; ++i) command[count++] = protocol_flag[i];
    command[count++] = L' '; hex(command, count, id.installation.bytes, 16);
    command[count++] = L' '; hex(command, count, id.transaction.bytes, 16);
    command[count++] = L' '; hex(command, count, id.document_sha256, 32);
    command[count++] = L' '; handle_text(command, count, owner->parent);
    command[count++] = L' '; handle_text(command, count, owner->child_mapping);
    command[count++] = L' '; handle_text(command, count, owner->child_event); command[count] = 0;
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    // The launched application owns the visible window: forward only the
    // dispatcher's own show request (for example a minimized shortcut).
    STARTUPINFOW own{}; own.cb = sizeof(own); GetStartupInfoW(&own);
    if (own.dwFlags & STARTF_USESHOWWINDOW) { startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW; startup.StartupInfo.wShowWindow = own.wShowWindow; }
    startup.lpAttributeList = list;
    PROCESS_INFORMATION created{};
    const BOOL started = CreateProcessW(path, command, nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &created);
    const DWORD error = started ? ERROR_SUCCESS : GetLastError(); DeleteProcThreadAttributeList(list);
    // Retain even abnormal returned handles on a failed API call.
    owner->process = created.hProcess; owner->thread = created.hThread; owner->launched = started != FALSE;
    SecureZeroMemory(command, sizeof(command));
    return started ? UpdateHandoffResult{} : fail(UpdateHandoffError::launch, error);
}
bool same_file(HANDLE a, HANDLE b, u64 expected_bytes) noexcept {
    BY_HANDLE_FILE_INFORMATION x{}, y{}; LARGE_INTEGER size{};
    return GetFileInformationByHandle(a, &x) && GetFileInformationByHandle(b, &y) && GetFileSizeEx(a, &size) &&
        size.QuadPart >= 0 && static_cast<u64>(size.QuadPart) == expected_bytes &&
        !(x.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        !(y.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
}
// Fixed lowercase hex only, exact lengths, no optional switches/path arguments.
int unhex(wchar_t c) noexcept { if (c >= L'0' && c <= L'9') return c - L'0'; if (c >= L'a' && c <= L'f') return c - L'a' + 10; return -1; }
bool parse_bytes(ConstSlice<wchar_t> s, u32& at, u8* out, u32 bytes) noexcept {
    if (at + bytes * 2 > s.count) return false;
    for (u32 i = 0; i < bytes; ++i) { const int a = unhex(s.data[at++]), b = unhex(s.data[at++]); if (a < 0 || b < 0) return false; out[i] = static_cast<u8>((a << 4) | b); }
    return true;
}
bool separator(ConstSlice<wchar_t> s, u32& at) noexcept { return at < s.count && s.data[at++] == L' '; }
bool parse_handle(ConstSlice<wchar_t> s, u32& at, HANDLE& out) noexcept {
    if (at + 16 > s.count) return false; u64 value = 0;
    for (u32 i = 0; i < 16; ++i) { const int h = unhex(s.data[at++]); if (h < 0) return false; value = (value << 4) | static_cast<u64>(h); }
    out = reinterpret_cast<HANDLE>(value); return handle_ok(out);
}
bool flag_at(ConstSlice<wchar_t> line, u32* after) noexcept {
    if (!line.data || !line.count || line.count >= kUpdateHandoffCommandCharacters) return false;
    u32 at = 0;
    if (line.data[at] == L'"') { ++at; while (at < line.count && line.data[at] != L'"') ++at; if (at == line.count) return false; ++at; }
    else { while (at < line.count && line.data[at] != L' ') ++at; }
    if (!separator(line, at) || at + flag_characters > line.count) return false;
    for (u32 i = 0; i < flag_characters; ++i) if (line.data[at++] != protocol_flag[i]) return false;
    if (at < line.count && line.data[at] != L' ') return false;
    *after = at; return true;
}
bool parse(ConstSlice<wchar_t> line, UpdateHandoffChild& value) noexcept {
    u32 at = 0;
    return flag_at(line, &at) && separator(line, at) && parse_bytes(line, at, value.identity.installation.bytes, 16) &&
        separator(line, at) && parse_bytes(line, at, value.identity.transaction.bytes, 16) &&
        separator(line, at) && parse_bytes(line, at, value.identity.document_sha256, 32) &&
        separator(line, at) && parse_handle(line, at, value.parent) && separator(line, at) && parse_handle(line, at, value.mapping) &&
        separator(line, at) && parse_handle(line, at, value.event) && at == line.count &&
        value.parent != value.mapping && value.parent != value.event && value.mapping != value.event && identity_valid(value.identity);
}
} // namespace

UpdateHandoffResult update_handoff_launch(UpdateGenerationTransaction* transaction,
    UpdateGenerationAuthenticationContext* authentication, const UpdatePackage& package, bool enrolled,
    const UpdateGenerationLaunchAdmission* admission, UpdateHandoff* owner) noexcept {
    const u32 members = package.descriptor.member_count;
    if (!transaction || !authentication || authentication->retain_selected_package || !owner || !empty(*owner) ||
        !members || members > kUpdatePackageMaximumMembers) return fail(UpdateHandoffError::invalid_input);
    for (u32 i = 0; i < members; ++i) {
        if (!handle_ok(package.files[i])) return fail(UpdateHandoffError::invalid_input);
        for (u32 j = 0; j < i; ++j) if (package.files[i] == package.files[j]) return fail(UpdateHandoffError::invalid_input);
    }
    const ConstSlice<u8> destination{reinterpret_cast<const u8*>(owner), sizeof(*owner)};
    if (!byte_range_valid(destination) || byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(transaction), sizeof(*transaction)}) ||
        byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(authentication), sizeof(*authentication)}) ||
        byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(&package), sizeof(package)}) ||
        (admission && byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(admission), sizeof(*admission)})) ||
        (authentication->key && byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(authentication->key), sizeof(*authentication->key)})) ||
        (authentication->workspace && (byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(authentication->workspace), sizeof(*authentication->workspace)}) ||
            byte_ranges_overlap(destination, {authentication->workspace->storage.data, authentication->workspace->storage.capacity}))))
        return fail(UpdateHandoffError::invalid_input);
    if (!enrolled) return fail(UpdateHandoffError::enrollment, ERROR_ACCESS_DENIED);
    auto validated = update_generation_validate(transaction);
    if (!validated.ok() || !transaction->record_present || !transaction->state.active.present ||
        transaction->state.phase == UpdateGenerationPhase::rollback ||
        !update_package_descriptor_equal(package.descriptor, transaction->state.active.package))
        return fail(UpdateHandoffError::generation, validated.ok() ? ERROR_INVALID_STATE : validated.native_error);
    if (transaction->state.phase == UpdateGenerationPhase::pending && (!admission ||
        admission->sequence != transaction->state.sequence ||
        !bytes_equal(admission->transaction.bytes, transaction->state.transaction.bytes, 16) ||
        !bytes_equal(admission->document_sha256, package.descriptor.document_sha256, 32)))
        return fail(UpdateHandoffError::generation, ERROR_INVALID_STATE);
    const auto authenticator = update_generation_authenticator(authentication); DWORD native = 0;
    if (!authenticator.verify_installed(authenticator.context, transaction, package.descriptor, &native))
        return fail(UpdateHandoffError::authenticity, native);
    wchar_t path[kUpdateGenerationMaximumRootCharacters + 96]{}; u32 path_chars = 0;
    // Last the launch target, member 0, so path holds its path.
    for (u32 i = members; i-- > 0;) {
        validated = update_generation_path(*transaction, {package.descriptor.document_sha256, 32},
            update_package_member_name(package.descriptor.members[i]), {path, sizeof(path) / sizeof(wchar_t)}, &path_chars);
        if (!validated.ok()) return fail(UpdateHandoffError::target, validated.native_error);
        owner->validation = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (owner->validation == INVALID_HANDLE_VALUE) { owner->validation = nullptr; return fail(UpdateHandoffError::target, GetLastError()); }
        const bool same = same_file(owner->validation, package.files[i], package.descriptor.members[i].bytes);
        if (!close(owner->validation)) return fail(UpdateHandoffError::release, GetLastError());
        if (!same) return fail(UpdateHandoffError::target, ERROR_INVALID_DATA);
    }
    validated = update_generation_validate(transaction);
    if (!validated.ok()) return fail(UpdateHandoffError::generation, validated.native_error);
    UpdateHandoffIdentity id{transaction->installation, transaction->state.transaction, {}};
    copy_bytes(id.document_sha256, package.descriptor.document_sha256, 32);
    return launch_native(path, path_chars, id, owner);
}

UpdateHandoffResult update_handoff_wait(UpdateHandoff* owner, u32 timeout, HANDLE cancel) noexcept {
    if (!owner || !owner->launched || owner->wait_completed || !owner->process || !owner->event || !owner->view ||
        !timeout || timeout > kUpdateHandoffMaximumWaitMs || (cancel && (!handle_ok(cancel) || cancel == owner->process || cancel == owner->event)))
        return fail(UpdateHandoffError::invalid_input);
    HANDLE handles[3]{}; u32 count = 0;
    if (cancel) handles[count++] = cancel;
    const u32 process_index = count; handles[count++] = owner->process;
    const u32 ack_index = count; handles[count++] = owner->event;
    const DWORD waited = WaitForMultipleObjects(count, handles, FALSE, timeout);
    if (waited == WAIT_FAILED) return fail(UpdateHandoffError::wait, GetLastError());
    owner->wait_completed = true;
    if (waited == WAIT_TIMEOUT) return {UpdateHandoffError::none, 0, UpdateHandoffOutcome::deadline};
    if (cancel && waited == WAIT_OBJECT_0) return {UpdateHandoffError::none, 0, UpdateHandoffOutcome::cancelled};
    if (waited == WAIT_OBJECT_0 + process_index) return {UpdateHandoffError::none, 0, UpdateHandoffOutcome::process_exited};
    if (waited != WAIT_OBJECT_0 + ack_index) return fail(UpdateHandoffError::wait, ERROR_INVALID_DATA);
    auto* slot = static_cast<AckSlot*>(owner->view);
    if (InterlockedCompareExchange(&slot->committed, ack_committed, ack_committed) != ack_committed ||
        slot->protocol != kUpdateHandoffProtocol || !equal(slot->identity, owner->expected)) return fail(UpdateHandoffError::acknowledgement, ERROR_INVALID_DATA);
    for (auto byte : slot->reserved) if (byte) return fail(UpdateHandoffError::acknowledgement, ERROR_INVALID_DATA);
    return {UpdateHandoffError::none, 0, UpdateHandoffOutcome::acknowledged};
}
UpdateHandoffResult update_handoff_release(UpdateHandoff* owner) noexcept {
    if (!owner) return fail(UpdateHandoffError::invalid_input);
    if (owner->view) { if (!UnmapViewOfFile(owner->view)) return fail(UpdateHandoffError::release, GetLastError()); owner->view = nullptr; }
    HANDLE* handles[]{&owner->thread, &owner->process, &owner->child_event, &owner->child_mapping, &owner->event, &owner->mapping, &owner->parent, &owner->validation};
    for (auto* handle : handles) if (!close(*handle)) return fail(UpdateHandoffError::release, GetLastError());
    *owner = {}; return {};
}
UpdateHandoffResult update_handoff_child_open(ConstSlice<wchar_t> line, const UpdateHandoffIdentity& expected,
    bool enrolled, UpdateHandoffChild* owner) noexcept {
    if (!owner || owner->parent || owner->mapping || owner->event || owner->view || owner->acknowledged) return fail(UpdateHandoffError::invalid_input);
    if (!enrolled) return fail(UpdateHandoffError::enrollment, ERROR_ACCESS_DENIED);
    UpdateHandoffChild parsed{};
    if (!parse(line, parsed) || !equal(parsed.identity, expected)) return fail(UpdateHandoffError::acknowledgement, ERROR_INVALID_DATA);
    DWORD flags = 0;
    if (!GetHandleInformation(parsed.parent, &flags) || !(flags & HANDLE_FLAG_INHERIT) ||
        !GetHandleInformation(parsed.mapping, &flags) || !(flags & HANDLE_FLAG_INHERIT) ||
        !GetHandleInformation(parsed.event, &flags) || !(flags & HANDLE_FLAG_INHERIT)) return fail(UpdateHandoffError::channel, ERROR_INVALID_HANDLE);
    // Import only the protocol capabilities after trusted identity validation.
    *owner = parsed;
    if (WaitForSingleObject(owner->parent, 0) == WAIT_FAILED) return fail(UpdateHandoffError::parent, GetLastError());
    owner->view = MapViewOfFile(owner->mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(AckSlot));
    if (!owner->view) return fail(UpdateHandoffError::channel, GetLastError());
    return {};
}
UpdateHandoffResult update_handoff_child_acknowledge(UpdateHandoffChild* owner, bool admitted) noexcept {
    if (!owner || !owner->view || !owner->event || owner->acknowledged) return fail(UpdateHandoffError::invalid_input);
    if (!admitted) return fail(UpdateHandoffError::acknowledgement, ERROR_INVALID_STATE);
    auto* slot = static_cast<AckSlot*>(owner->view);
    const LONG marker = InterlockedCompareExchange(&slot->committed, 0, 0);
    if (marker && (marker != ack_committed || slot->protocol != kUpdateHandoffProtocol || !equal(slot->identity, owner->identity)))
        return fail(UpdateHandoffError::acknowledgement, ERROR_INVALID_STATE);
    if (!marker) {
        slot->protocol = kUpdateHandoffProtocol; slot->identity = owner->identity;
        InterlockedExchange(&slot->committed, ack_committed);
    }
    if (!SetEvent(owner->event)) return fail(UpdateHandoffError::channel, GetLastError());
    owner->acknowledged = true; return {};
}
UpdateHandoffResult update_handoff_child_release(UpdateHandoffChild* owner) noexcept {
    if (!owner) return fail(UpdateHandoffError::invalid_input);
    if (owner->view) { if (!UnmapViewOfFile(owner->view)) return fail(UpdateHandoffError::release, GetLastError()); owner->view = nullptr; }
    if (!close(owner->event) || !close(owner->mapping) || !close(owner->parent)) return fail(UpdateHandoffError::release, GetLastError());
    *owner = {}; return {};
}
} // namespace maicord
