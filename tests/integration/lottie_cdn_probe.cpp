// Manual live-network probe: built only with -DMAICORD_BUILD_INTEGRATION_TESTS=ON
// (cmake/integration.cmake) and run by hand or through tools/probe_*.ps1; it is
// never part of the CTest suite because it talks to the real service.
#include "maicord/app/cdn_client.hpp"
#include "native_resource_probe.hpp"
#include "maicord/core/json_reader.hpp"

namespace maicord { namespace {
void print(const char* text) noexcept {u32 n=0;while(text[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&done,nullptr);}
void number(u64 value) noexcept {char text[32];u32 n=0;do{text[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char c=text[i];text[i]=text[n-i-1];text[n-i-1]=c;}DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&done,nullptr);}
struct Validation {CdnClientResponse response{};bool valid=false;};
bool consume(void* opaque,const CdnClientResponse& response,ConstSlice<u8> body) noexcept {
    auto& result=*static_cast<Validation*>(opaque);result.response=response;
    {
        auto* index=body.count && body.count<=1024*1024 ? static_cast<u32*>(VirtualAlloc(nullptr,
            body.count*sizeof(u32),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)) : nullptr;
        if(index){const Utf8View text{reinterpret_cast<const char*>(body.data),body.count};
            const auto scan=json_scan_structurals_avx1(text,{index,body.count});
            result.valid=response.transport_ok()&&response.request_id==1&&response.status_code==200&&
                !response.redirects&&body.count==response.body_bytes&&scan.ok()&&
                json_visit_root_object(text,{index,scan.count},64,
                    +[](void*,JsonString,JsonValue) noexcept{return true;},nullptr).ok();
            VirtualFree(index,0,MEM_RELEASE);}
    }
    // Consume even a rejected response so native cleanup is always exercised.
    return true;
}
struct Lifecycle {DWORD before=0,after=0;u32 owned_after=0;bool valid=false;};
Lifecycle lifecycle(u32 cycle) noexcept {
    print("cycle=");number(cycle);print("\n");
    bool resources_ok=resource_probe::sample(cycle==1?"cold_before":"warm_before",cycle==1?4u:0u);
    if(!resources_ok)return {};
    DWORD baseline=0;const bool baseline_ok=GetProcessHandleCount(GetCurrentProcess(),&baseline)!=FALSE;
    if(!baseline_ok)return {};
    CdnClient client{};const auto started=cdn_client_start(&client,1024*1024);
    const auto owned_resources=resource_probe::owner(client);
    resources_ok=resource_probe::sample(cycle==1?"cold_started":"warm_started",1)&&resources_ok;
    bool valid=baseline_ok&&started.ok();
    print("start_phase=");number(static_cast<u32>(started.phase));print(" start_error=");number(static_cast<u32>(started.error));
    print(" start_native=");number(started.native_error);print("\n");
    Validation validation{};
    if(started.ok()&&resources_ok){
        constexpr auto target=utf8_literal("/stickers/796140620111544330.json");
        const auto submitted=cdn_client_submit(&client,{{reinterpret_cast<const u8*>(target.data),target.bytes},{},1,
            20000,CdnHost::cdn,CdnImageType::lottie_json});
        print("submit_error=");number(static_cast<u32>(submitted.error));print("\n");
        valid=valid&&submitted.ok();
        if(submitted.ok()){
            const auto waited=cdn_client_wait_response(&client,25000);
            print("wait_phase=");number(static_cast<u32>(waited.phase));print(" wait_error=");number(static_cast<u32>(waited.error));
            print(" wait_native=");number(waited.native_error);print(" requests=");number(waited.requests);print("\n");
            valid=valid&&waited.phase==CdnClientPhase::response_ready&&waited.requests==1;
            if(waited.phase==CdnClientPhase::response_ready){
                const auto consumed=cdn_client_consume_response(&client,consume,&validation);
                valid=valid&&consumed.ok()&&validation.valid;
                print("consume_error=");number(static_cast<u32>(consumed.error));print("\n");
            }
        }
    }
    const auto& response=validation.response;
    print("http_status=");number(response.status_code);print(" bytes=");number(response.body_bytes);
    print(" redirects=");number(response.redirects);print(" client_error=");number(static_cast<u32>(response.error));
    print(" http_error=");number(static_cast<u32>(response.http_error));print(" native_error=");number(response.native_error);
    print(" elapsed_us=");number(response.elapsed_us);print("\n");
    if(resources_ok)resources_ok=resource_probe::sample(cycle==1?"cold_response":"warm_response",0);
    bool stopped=!client.runtime&&!client.worker.thread;
    if(!stopped){const auto stop=cdn_client_stop(&client,5000);stopped=stop.phase==CdnClientPhase::stopped;
        print("stop_phase=");number(static_cast<u32>(stop.phase));print(" stop_error=");number(static_cast<u32>(stop.error));print("\n");}
    // Check the exact previously owned kernel handles/address before another
    // diagnostic snapshot can create temporary handles or reserve that address.
    const bool ownership_released=resource_probe::owner_released(owned_resources);
    if(resources_ok)resources_ok=resource_probe::sample(cycle==1?"cold_stopped":"warm_stopped",cycle==1?2u:3u);
    DWORD final_handles=0;const bool final_ok=GetProcessHandleCount(GetCurrentProcess(),&final_handles)!=FALSE;
    print("handles_before=");number(baseline);print(" handles_after=");number(final_handles);print("\n");
    // Enumerate only ownership exposed by this client and worker. TLS/socket/
    // resolver storage is destroyed before runtime is cleared by stop.
    const u32 owned=(client.runtime?1u:0u)|(client.response_event?2u:0u)|(client.worker.thread?4u:0u)|
        (client.worker.ready_event?8u:0u)|(client.worker.loop.completion_port?16u:0u)|
        (client.worker.loop.winsock_started?32u:0u);
    print("owned_client_worker_mask_after=");number(owned);print("\n");
    valid=valid&&stopped&&!owned&&final_ok&&resources_ok&&ownership_released;
    return {baseline,final_handles,owned,valid};
}
u32 warm_cycles() noexcept {
    char value[2]{};SetLastError(ERROR_SUCCESS);
    const DWORD length=GetEnvironmentVariableA("MAICORD_RESOURCE_PROBE_WARM_CYCLES",value,sizeof(value));
    if(!length&&GetLastError()==ERROR_ENVVAR_NOT_FOUND)return 1;
    if(length==1&&value[0]>='1'&&value[0]<='8')return static_cast<u32>(value[0]-'0');
    print("resource_warm_cycles_invalid_expected_one_character_1_to_8\n");return 0;
}
u32 control_samples() noexcept {
    char value[3]{};SetLastError(ERROR_SUCCESS);
    const DWORD length=GetEnvironmentVariableA("MAICORD_RESOURCE_PROBE_CONTROL_SAMPLES",value,sizeof(value));
    if(!length&&GetLastError()==ERROR_ENVVAR_NOT_FOUND)return 0;
    if(length==2&&value[0]=='1'&&value[1]=='6')return 16;
    if(length==2&&value[0]=='3'&&value[1]=='0')return 30;
    print("resource_control_samples_invalid_expected_16_or_30\n");return ~0u;
}
bool control_interval(u32* milliseconds) noexcept {
    char value[5]{};SetLastError(ERROR_SUCCESS);
    const DWORD length=GetEnvironmentVariableA("MAICORD_RESOURCE_PROBE_CONTROL_INTERVAL_MS",value,sizeof(value));
    *milliseconds=0;
    if(!length&&GetLastError()==ERROR_ENVVAR_NOT_FOUND)return true;
    if(length==0||length>4)return false;
    for(u32 i=0;i<length;++i){if(value[i]<'0'||value[i]>'9')return false;*milliseconds=*milliseconds*10+static_cast<u32>(value[i]-'0');}
    return *milliseconds<=1000;
}
bool control_heap_summary(bool* enabled) noexcept {
    char value[2]{};SetLastError(ERROR_SUCCESS);
    const DWORD length=GetEnvironmentVariableA("MAICORD_RESOURCE_PROBE_CONTROL_HEAP_SUMMARY",value,sizeof(value));
    *enabled=true;
    if(!length&&GetLastError()==ERROR_ENVVAR_NOT_FOUND)return true;
    if(length!=1||(value[0]!='0'&&value[0]!='1'))return false;
    *enabled=value[0]=='1';return true;
}
bool control_arm(const char* phase,u32 count,u32 interval_ms) noexcept {
    for(u32 i=0;i<count;++i){
        print("resource_control_arm=");print(phase);print(" arm_index=");number(i);print("\n");
        if(!resource_probe::sample(phase,i&1u,false))return false;
        if(interval_ms&&i+1<count)Sleep(interval_ms);
    }
    return true;
}
bool run_control(u32 count) noexcept {
    u32 interval_ms=0;
    if(!control_interval(&interval_ms)){print("resource_control_interval_invalid_expected_0_to_1000_ms\n");return false;}
    bool summarize_heap=true;
    if(!control_heap_summary(&summarize_heap)){print("resource_control_heap_summary_invalid_expected_0_or_1\n");return false;}
    print("resource_control_samples_per_arm=");number(count);print(" interval_ms=");number(interval_ms);print("\n");
    resource_probe::initialize_control(summarize_heap);
    if(!control_arm("control_before_client",count,interval_ms)){print("resource_control_pre_arm_failed\n");return false;}
    const auto cold=lifecycle(1);
    print("resource_control_cold_valid=");number(cold.valid?1:0);print(" cold_handles_before=");number(cold.before);
    print(" cold_handles_after=");number(cold.after);print("\n");
    const bool after=control_arm("control_after_cold_stop",count,interval_ms);
    print("resource_control_summary samples_per_arm=");number(count);print(" observed_samples=");number(resource_probe::control_index);
    print(" http_lifecycles=1 cold_valid=");number(cold.valid?1:0);print(" post_arm_ok=");number(after?1:0);
    print(" callback_outstanding_requested=");number(resource_probe::callback_requested);
    print(" callback_outstanding_commit=");number(resource_probe::callback_committed);print("\n");
    print("resource_control_boundary=aggregate_observer_and_default_heap_evidence_does_not_identify_Schannel_allocations_or_prove_zero_leaks\n");
    return cold.valid&&after;
}
bool run() noexcept {
    const u32 controls=control_samples();if(controls==~0u)return false;if(controls)return run_control(controls);
    const u32 count=warm_cycles();if(!count)return false;
    print("resource_warm_cycles=");number(count);print("\n");
    if(!resource_probe::initialize()){print("resource_instrumentation_failed\n");return false;}
    const auto cold=lifecycle(1);
    print("cold_handles_before=");number(cold.before);print(" cold_handles_after=");number(cold.after);
    print(" cold_handle_delta=");if(cold.after<cold.before)print("-");
    number(cold.after>=cold.before?cold.after-cold.before:cold.before-cold.after);print("\n");
    if(!cold.valid){print("lottie_native_cdn_cold_lifecycle_failed\n");return false;}
    // Repeat fresh owners within a fixed public-request bound. Keep natural
    // count drops visible; exact equality and non-accumulation are distinct.
    DWORD previous_after=cold.after;bool all_equal=true,all_no_accumulation=true;
    for(u32 i=0;i<count;++i){
        const auto warm=lifecycle(i+2);
        const bool equal=warm.valid&&warm.before==previous_after&&warm.after==warm.before;
        const bool no_accumulation=warm.valid&&warm.before<=previous_after&&warm.after<=warm.before;
        print("warm_handles_before=");number(warm.before);print(" warm_handles_after=");number(warm.after);
        print(" warm_stable=");number(equal?1:0);print(" warm_no_handle_accumulation=");number(no_accumulation?1:0);
        print(" warm_cycle=");number(i+1);print(" previous_handles_after=");number(previous_after);print("\n");
        if(!warm.valid){print("lottie_native_cdn_warm_lifecycle_failed\n");return false;}
        all_equal=all_equal&&equal;all_no_accumulation=all_no_accumulation&&no_accumulation;
        previous_after=warm.after;
    }
    resource_probe::retained_cold();
    print("resource_warm_summary cycles=");number(count);print(" exact_equal=");number(all_equal?1:0);
    print(" no_handle_accumulation=");number(all_no_accumulation?1:0);print(" final_handles=");number(previous_after);print("\n");
    print(all_equal?"lottie_native_cdn_passed_warm_lifecycle_stable\n":
        all_no_accumulation?"lottie_native_cdn_passed_warm_lifecycle_no_handle_accumulation\n":
        "lottie_native_cdn_warm_handle_accumulation_failed\n");
    return all_no_accumulation;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_lottie_cdn_probe_entry() noexcept {ExitProcess(maicord::run()?0:1);}
