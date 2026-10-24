#include "maicord/app/account_media_worker.hpp"
#include "maicord/protocol/embed_player.hpp"
#include "maicord/core/time.hpp"
#include "maicord/sys/process_info.hpp"
#include "maicord/sys/com.hpp"
#include "maicord/sys/d3d11.hpp"
#include "test_output.hpp"
namespace maicord {
namespace {
u64 ticks(FILETIME value) noexcept { return (u64{value.dwHighDateTime}<<32)|value.dwLowDateTime; }
struct Sample { u64 process_cpu{},lane_cpu{},process_cycles{},lane_cycles{},milliseconds{},private_bytes{},working_set{}; bool valid{}; };
Sample sample(const AccountMediaWorker* worker) noexcept {
    Sample out{}; FILETIME created{},ended{},kernel{},user{};
    out.valid=GetProcessTimes(GetCurrentProcess(),&created,&ended,&kernel,&user)!=FALSE;
    out.process_cpu=ticks(kernel)+ticks(user);
    out.valid=out.valid && QueryProcessCycleTime(GetCurrentProcess(),&out.process_cycles);
    for (const auto& lane:worker->lanes) if (lane.thread) {
        if (!GetThreadTimes(lane.thread,&created,&ended,&kernel,&user)) out.valid=false;
        out.lane_cpu+=ticks(kernel)+ticks(user);
        u64 cycles{}; if (!QueryThreadCycleTime(lane.thread,&cycles)) out.valid=false; out.lane_cycles+=cycles;
    }
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory);
    out.valid=out.valid && K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
    out.private_bytes=memory.PrivateUsage; out.working_set=memory.WorkingSetSize; out.milliseconds=GetTickCount64(); return out;
}
void report(const char* name,const Sample& first,const Sample& last,u32 notifications,u32 frames) noexcept {
    test_write_line(name);
    test_write_metric("player process CPU us: ",(last.process_cpu-first.process_cpu)/10);
    test_write_metric("player decode lanes CPU us: ",(last.lane_cpu-first.lane_cpu)/10);
    test_write_metric("player process cycles: ",last.process_cycles-first.process_cycles);
    test_write_metric("player decode lanes cycles: ",last.lane_cycles-first.lane_cycles);
    test_write_metric("player observation ms: ",last.milliseconds-first.milliseconds);
    test_write_metric("player output notifications: ",notifications);
    test_write_metric("player acknowledged frames: ",frames);
    test_write_metric("player private committed bytes: ",last.private_bytes);
    test_write_metric("player working set bytes: ",last.working_set);
}
}
// Real production worker; no diagnostic fields or branches in shipping code.
// Times/memory are reported only. Assertions cover identity, commands, frames,
// quality and activity counts. A deadline only prevents a hung live probe.
bool native_player_perf_probe() noexcept {
    char channel[26]{};
    const u32 size=GetEnvironmentVariableA("MAICORD_PLAYER_PERF_PROBE",channel,sizeof(channel));
    if (!size) return true;
    if (size>=sizeof(channel) || !twitch_channel_id({channel,size})) return false;
    const auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if (FAILED(apartment)) return false;
    ID3D11Device* device{}; ID3D11DeviceContext* context{};
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0}; D3D_FEATURE_LEVEL level{};
    bool valid=SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,2,D3D11_SDK_VERSION,&device,&level,&context));
    ID3D11Multithread* guard{};
    if (context && SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread),reinterpret_cast<void**>(&guard)))) { guard->SetMultithreadProtected(TRUE); guard->Release(); }
    auto* worker=static_cast<AccountMediaWorker*>(VirtualAlloc(nullptr,sizeof(AccountMediaWorker),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    valid=valid && worker && account_media_worker_initialize(worker).ok();
    char url[64]; constexpr auto prefix=utf8_literal("https://www.twitch.tv/");
    copy_bytes(url,prefix.data,prefix.bytes); copy_bytes(url+prefix.bytes,channel,size);
    const auto key=media_asset_embed(1,1,0,MediaEmbedResource::video,MediaAttachmentFormat::mp4,true);
    const AccountMediaAttachmentRequest request{{url,prefix.bytes+size},key,1,1,1920,1080,CdnImageType::mp4,{1,{0,0}},device};
    const u64 deadline=GetTickCount64()+16000;
    AccountMediaFrame poster{}; AccountMediaRequestResult requested{};
    bool ready=false;
    while (valid && !ready && GetTickCount64()<deadline) {
        requested=account_media_worker_request_attachment(worker,request,clock_monotonic_milliseconds(),clock_unix_milliseconds()/1000);
        ready=account_media_worker_take_frame(worker,&poster);
        valid=requested.state!=AccountMediaRequestState::failed && requested.state!=AccountMediaRequestState::invalid && requested.state!=AccountMediaRequestState::unsupported;
        if (!ready) WaitForSingleObject(worker->output_event,100);
    }
    valid=valid && ready && poster.gpu_video;
    AccountVideoIdentity identity{}; u32 frames=0,notifications=0,timeouts=0,gaps=0,buffering=0; bool buffered=false;
    u64 previous_duration=0; i64 first_pts=0,previous_pts=0; u16 decoded_height=0;
    Sample warm{},done{};
    if (valid) {
        const AccountVideoSelection selection{key,{1,1,0,1,1,0,0}};
        valid=account_media_worker_video_select(worker,selection).ok();
        identity=account_media_worker_video_status(worker).playback.identity;
        valid=valid && account_media_worker_video_control(worker,{identity,1,0,1,AccountVideoCommand::mute,true}).ok() &&
            account_media_worker_video_control(worker,{identity,2,0,1,AccountVideoCommand::play,true}).ok();
    }
    while (valid && frames<420 && GetTickCount64()<deadline) {
        const DWORD waited=WaitForSingleObject(worker->output_event,100);
        if (waited==WAIT_OBJECT_0) { if (frames>=30) ++notifications; }
        else if (waited==WAIT_TIMEOUT) { if (frames>=30) ++timeouts; }
        else { valid=false; break; }
        const auto state=account_media_worker_video_status(worker);
        valid=state.selected && account_video_identity_equal(state.playback.identity,identity) && state.playback.error==AccountVideoError::none && !state.source_refresh && !state.playback.ended;
        if (frames>=30 && !buffered && state.playback.buffering) ++buffering;
        buffered=state.playback.buffering;
        AccountVideoFrame frame{};
        if (valid && account_media_worker_video_take(worker,&frame)) {
            if (frames>=30 && previous_duration && frame.presentation_time_us>previous_pts+static_cast<i64>(previous_duration+previous_duration/2)) ++gaps;
            previous_pts=frame.presentation_time_us; previous_duration=frame.duration_us; decoded_height=frame.display_height;
            valid=account_media_worker_video_acknowledge(worker,frame);
            ++frames;
            if (frames==30) { warm=sample(worker); first_pts=frame.presentation_time_us; }
        }
    }
    if (frames>=30) { done=sample(worker); valid=valid && warm.valid && done.valid; report("Native worker playing",warm,done,notifications,frames-30); }
    test_write_metric("player request state: ",static_cast<u32>(requested.state));
    test_write_metric("player decoded height: ",decoded_height);
    test_write_metric("player source elapsed us: ",previous_pts>=first_pts?previous_pts-first_pts:0);
    test_write_metric("player frame gaps: ",gaps); test_write_metric("player buffering transitions: ",buffering); test_write_metric("player observer timeouts: ",timeouts);
    valid=valid && frames==420 && decoded_height==1080;
    if (valid) {
        valid=account_media_worker_video_control(worker,{identity,3,0,1,AccountVideoCommand::pause,true}).ok();
        bool paused=false;
        while (valid && !paused && GetTickCount64()<deadline) {
            AccountVideoFrame frame{}; if (account_media_worker_video_take(worker,&frame)) valid=account_media_worker_video_acknowledge(worker,frame);
            const auto state=account_media_worker_video_status(worker);
            paused=state.playback.observed_command>=3 && state.playback.paused && !state.playback.playing;
            if (!paused) WaitForSingleObject(worker->output_event,100);
        }
        valid=valid && paused;
        if (paused) {
            while (WaitForSingleObject(worker->output_event,0)==WAIT_OBJECT_0) {}
            const auto before=sample(worker); u32 wakes=0,paused_frames=0;
            const auto queued=ReadAcquire64(&worker->counters.queued);
            for (u32 i=0;i<8;++i) {
                if (WaitForSingleObject(worker->output_event,100)==WAIT_OBJECT_0) ++wakes;
                AccountVideoFrame frame{}; if (account_media_worker_video_take(worker,&frame)) { ++paused_frames; valid=valid && account_media_worker_video_acknowledge(worker,frame); }
            }
            const auto after=sample(worker); report("Native worker paused",before,after,wakes,paused_frames);
            valid=valid && before.valid && after.valid && !paused_frames && ReadAcquire64(&worker->counters.queued)==queued;
        }
    }
    if (worker) { if (worker->initialized && !account_media_worker_stop(worker,1500)) ExitProcess(1); VirtualFree(worker,0,MEM_RELEASE); }
    if (context) context->Release(); if (device) device->Release(); CoUninitialize();
    return valid;
}
}
