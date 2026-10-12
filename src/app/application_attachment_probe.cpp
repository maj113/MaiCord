#include "application_attachment_probe.hpp"
#include "application_attachments_state.hpp"
#include "application_state.hpp"
#include "maicord/app/attachment_media_probe.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/composer_attachment_strip.hpp"
#include <objbase.h>

namespace maicord::app_detail {
namespace {
inline constexpr u32 kProbeSlots = 12;
struct Probe {
    PTP_WORK work;
    u64 draft_id;
    u64 profile;
    FileSnapshot snapshot;
    // Clipboard file contents (no path); retained while the probe runs.
    UploadSource content;
    wchar_t path[kNativeFilePickerMaximumPathUnits + 1];
    volatile LONG done;
    u32 duration_ms;
    bool occupied;
};
struct ProbeOwner {
    Probe probes[kProbeSlots];
    u32 revision;
};
ProbeOwner* owner;

bool read_file(void* context, u64 offset, Slice<u8> output) noexcept {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD read = 0;
    return SetFilePointerEx(static_cast<HANDLE>(context), position, nullptr, FILE_BEGIN) &&
           ReadFile(static_cast<HANDLE>(context), output.data, output.count, &read, nullptr) && read == output.count;
}
bool read_content(void* context, u64 offset, Slice<u8> output) noexcept {
    const auto& source = *static_cast<const UploadSource*>(context);
    u32 done = 0;
    while (done < output.count) {
        const auto part = source.ops->read(source.context, offset + done, {output.data + done, output.count - done});
        if (!part.data || !part.count || part.count > output.count - done)
            return false;
        if (part.data != output.data + done)
            copy_bytes(output.data + done, part.data, part.count);
        done += part.count;
    }
    return true;
}
void CALLBACK run(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept {
    auto* probe = static_cast<Probe*>(context);
    u32 duration = 0;
    if (probe->content.context) {
        // Upload sources read on MTA worker threads only.
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(apartment)) {
            attachment_mp4_duration(probe->content.bytes, read_content, &probe->content, &duration);
            CoUninitialize();
        }
        probe->duration_ms = duration;
        InterlockedExchange(&probe->done, 1);
        return;
    }
    const HANDLE file = CreateFileW(probe->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        FileSnapshot found{};
        // The draft uploads this exact file identity; a replaced file is not probed.
        if (file_snapshot_read(file, &found) && found.bytes == probe->snapshot.bytes &&
            file_identity_equal(found.identity, probe->snapshot.identity))
            attachment_mp4_duration(found.bytes, read_file, file, &duration);
        CloseHandle(file);
    }
    probe->duration_ms = duration;
    InterlockedExchange(&probe->done, 1);
}
bool probeable(Utf8View filename) noexcept {
    char extension[8];
    const Utf8View ext{extension, ui_attachment_file_extension(filename, {extension, 8})};
    return utf8_equal(ext, utf8_literal("mp4")) || utf8_equal(ext, utf8_literal("m4v")) ||
           utf8_equal(ext, utf8_literal("mov"));
}
void retire(Probe& probe) noexcept {
    if (probe.work) {
        WaitForThreadpoolWorkCallbacks(probe.work, TRUE);
        CloseThreadpoolWork(probe.work);
    }
    upload_source_release(probe.content);
    SecureZeroMemory(&probe, sizeof(probe));
}
} // namespace

bool application_attachment_probe_synchronize() noexcept {
    const auto* state = g_app.attachments;
    bool changed = false;
    if (owner)
        for (auto& probe : owner->probes) {
            if (!probe.occupied)
                continue;
            const auto* record = state ? message_attachment_drafts_get(&state->drafts, probe.draft_id) : nullptr;
            const bool live = record && state->profile_id == probe.profile && !state->closing;
            if (!live) {
                retire(probe);
                continue;
            }
            if (probe.done == 1 && probe.work) {
                WaitForThreadpoolWorkCallbacks(probe.work, FALSE);
                CloseThreadpoolWork(probe.work);
                probe.work = nullptr;
                probe.done = 2;
                changed = changed || probe.duration_ms != 0;
            }
        }
    if (!state || state->closing)
        return changed;
    for (u32 i = 0; i < state->drafts.count; ++i) {
        const u32 slot = state->drafts.order[i];
        const auto& record = state->drafts.records[slot];
        const u64 id = (static_cast<u64>(record.generation) << 32) | (slot + 1);
        if (!record.occupied || record.phase == MessageAttachmentDraftPhase::queued ||
            !probeable(message_attachment_drafts_filename(&state->drafts, id)))
            continue;
        bool known = false;
        if (owner)
            for (const auto& probe : owner->probes)
                known = known || (probe.occupied && probe.draft_id == id);
        if (known)
            continue;
        if (!owner) {
            owner = static_cast<ProbeOwner*>(
                VirtualAlloc(nullptr, sizeof(ProbeOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!owner)
                return changed;
        }
        Probe* probe = nullptr;
        for (auto& candidate : owner->probes)
            if (!candidate.occupied) {
                probe = &candidate;
                break;
            }
        const auto path = record.content.context ? ConstSlice<wchar_t>{}
                                                 : message_attachment_drafts_path(&state->drafts, id);
        if (!probe || (!record.content.context && (!path.count || path.count > kNativeFilePickerMaximumPathUnits)) ||
            (record.content.context && !upload_source_valid(record.content)))
            continue;
        probe->occupied = true;
        probe->draft_id = id;
        probe->profile = state->profile_id;
        probe->snapshot = record.source;
        if (record.content.context) {
            // Pasted file contents keep their source alive until retirement.
            probe->content = record.content;
            upload_source_retain(probe->content);
        } else {
            copy_bytes(probe->path, path.data, path.count * sizeof(wchar_t));
            probe->path[path.count] = 0;
        }
        probe->work = CreateThreadpoolWork(run, probe, nullptr);
        if (probe->work)
            SubmitThreadpoolWork(probe->work);
        else
            probe->done = 2;
    }
    return changed;
}

u32 application_attachment_probe_duration(u64 draft_id) noexcept {
    if (owner && draft_id)
        for (const auto& probe : owner->probes)
            if (probe.occupied && probe.draft_id == draft_id && probe.done == 2)
                return probe.duration_ms;
    return 0;
}

void application_attachment_probe_release() noexcept {
    if (!owner)
        return;
    for (auto& probe : owner->probes)
        if (probe.occupied)
            retire(probe);
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
}
} // namespace maicord::app_detail
