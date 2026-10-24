#include "maicord/app/account_video_session.hpp"
#include "maicord/media/mp4_fragment_audio.hpp"
#include "maicord/media/mp4_segment_store.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
// Bounded init/first/last capture only; no signed URLs or credentials. The
// source keeps its real track clock and endpoint, including encoder priming.
bool run(ConstSlice<u8> bytes) noexcept {
    if (bytes.count < 32 || !bytes_equal(bytes.data, "MCAAC001", 8)) return false;
    const u32 sizes[]{byte_load_le32(bytes.data + 8), byte_load_le32(bytes.data + 12), byte_load_le32(bytes.data + 16)};
    const u32 count = byte_load_le32(bytes.data + 20);
    const u64 segment_us = byte_load_le64(bytes.data + 24);
    if (u64{sizes[0]} + sizes[1] + sizes[2] != bytes.count - 32 || !sizes[0] || !sizes[1] || !sizes[2] || count < 2)
        return false;
    const ConstSlice<u8> init{bytes.data + 32, sizes[0]}, first{init.data + sizes[0], sizes[1]}, last{first.data + sizes[1], sizes[2]};
    Mp4SegmentStore store{}; Mp4FragmentSource source{}; Mp4FragmentAudio audio{};
    u32 requests = 0;
    const auto feed = [&]() noexcept {
        if (!store.pending) return true;
        if (store.demand && store.demand != count - 1) return false;
        ++requests;
        return mp4_segment_store_accept(&store, store.demand, store.request_serial,
            store.demand ? last : first) == Mp4DemuxError::none;
    };
    bool valid = mp4_segment_store_initialize(&store, init, true, count, segment_us, 1000000) == Mp4DemuxError::none;
    Mp4DemuxError error = Mp4DemuxError::data_pending;
    for (u32 calls = 0; valid && calls < 8 && error == Mp4DemuxError::data_pending; ++calls) {
        error = mp4_fragment_source_initialize_segments(&store, &source);
        valid = error == Mp4DemuxError::none || (error == Mp4DemuxError::data_pending && feed());
    }
    valid = valid && error == Mp4DemuxError::none;
    if (valid) error = mp4_fragment_audio_initialize(&source, &audio);
    valid = valid && error == Mp4DemuxError::none;
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    AccountVideoSession session{};
    const AccountVideoIdentity identity{1, 2, 3, 4, 0, 1, 0};
    valid = valid && SUCCEEDED(apartment) && event &&
        account_video_session_initialize_fragment_audio(&session, &audio, identity, event).ok() &&
        !session.video && !session.workspace && !session.status.live && session.status.duration_100ns > 10000000 &&
        account_video_session_control(&session, identity, 1, AccountVideoCommand::mute, 0, 1, true).ok() &&
        account_video_session_control(&session, identity, 2, AccountVideoCommand::play).ok();
    u64 initial_consumed = 0, sought_epoch = 0;
    u32 picture_frames = 0;
    const u64 deadline = GetTickCount64() + 5000;
    while (valid && GetTickCount64() < deadline && !session.status.ended) {
        valid = feed();
        if (!valid) break;
        const auto work = account_video_session_service(&session);
        picture_frames += work.frame_ready;
        valid = session.status.error == AccountVideoError::none && !picture_frames;
        const auto progress = account_video_session_audio_progress(&session);
        if (valid && !sought_epoch && progress.consumed_frames >= 4096) {
            initial_consumed = progress.consumed_frames;
            valid = account_video_session_control(&session, identity, 3, AccountVideoCommand::seek,
                session.status.duration_100ns - 5000000).ok();
            sought_epoch = session.playback_epoch;
        }
        if (!work.needs_service) WaitForSingleObject(event, minimum(10u, work.next_service_delay_ms));
    }
    const auto progress = account_video_session_audio_progress(&session);
    test_write_metric("Fragment audio demux error: ", static_cast<u32>(error));
    test_write_metric("Fragment audio session error: ", static_cast<u32>(session.status.error));
    test_write_metric("Fragment audio native error: ", session.status.native_error);
    test_write_metric("Fragment audio duration ms: ", session.status.duration_100ns > 0 ? session.status.duration_100ns / 10000 : 0);
    test_write_metric("Fragment audio initial consumed PCM: ", initial_consumed);
    test_write_metric("Fragment audio sought consumed PCM: ", progress.consumed_frames);
    test_write_metric("Fragment audio ended: ", session.status.ended);
    test_write_metric("Fragment audio media requests: ", requests);
    test_write_metric("Fragment audio picture frames: ", picture_frames);
    test_write_metric("Fragment audio workspace bytes: ", account_video_session_owned_bytes(&session));
    const u32 committed = store.initialization.committed + store.slots[0].storage.committed + store.slots[1].storage.committed;
    test_write_metric("Fragment audio encoded committed bytes: ", committed);
    valid = valid && initial_consumed >= 4096 && sought_epoch > 1 && session.playback_epoch == sought_epoch &&
        session.status.ended && progress.consumed_frames == audio.timeline.sample_rate / 2 &&
        !session.status.frame_pending && !session.video && requests == 2 && committed <= 3 * 1024 * 1024;
    if (!account_video_session_release(&session, 1000).ok()) ExitProcess(1);
    mp4_fragment_source_release(&source); mp4_segment_store_release(&store);
    if (event) CloseHandle(event);
    if (SUCCEEDED(apartment)) CoUninitialize();
    return valid;
}
}
bool fragment_audio_playback_probe() noexcept {
    wchar_t path[1024]{};
    const u32 length = GetEnvironmentVariableW(L"MAICORD_FRAGMENT_AUDIO_PROBE", path, 1024);
    if (!length) return true;
    if (length >= 1024) return false;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    Win32LargeInteger size{};
    const bool sized = GetFileSizeEx(file, size.native()) && size.QuadPart >= 32 && size.QuadPart <= 2 * 1024 * 1024;
    auto* data = sized ? static_cast<u8*>(VirtualAlloc(nullptr, static_cast<usize>(size.QuadPart), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) : nullptr;
    DWORD read{};
    const bool loaded = data && ReadFile(file, data, static_cast<DWORD>(size.QuadPart), &read, nullptr) && read == size.QuadPart;
    CloseHandle(file);
    const bool valid = loaded && run({data, read});
    if (data) VirtualFree(data, 0, MEM_RELEASE);
    return valid;
}
} // namespace maicord
