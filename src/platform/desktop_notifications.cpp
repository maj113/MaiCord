#include "maicord/platform/desktop_notifications.hpp"
#include <NotificationActivationCallback.h>
#include "toast_abi.hpp"

namespace maicord {
namespace {
namespace toast = toast_abi;
using toast::Object;
using toast::StringReference;
using toast::call;
using toast::query;
using toast::release;
constexpr wchar_t kActivationPrefix[] = L"mc1:p=";
constexpr u32 kActivationUnits = 60;
DesktopNotificationResult failed(DesktopNotificationError e, HRESULT h) noexcept { return {e, static_cast<u32>(h)}; }
bool same_guid(REFIID a, REFIID b) noexcept { return bytes_equal(&a, &b, sizeof(GUID)); }
u32 bounded_length(const wchar_t* s, u32 bound) noexcept {
    if (!s) return bound + 1;
    u32 i = 0; while (i <= bound && s[i]) ++i; return i;
}
bool app_id_valid(ConstSlice<wchar_t> id) noexcept {
    if (!id.data || !id.count || id.count > 128) return false;
    for (u32 i=0; i<id.count; ++i) {
        const wchar_t c=id[i];
        if (!((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'.'||c==L'-'||c==L'_')) return false;
    }
    return true;
}
bool xml_utf8(Utf8View v) noexcept {
    if ((!v.data && v.bytes) || v.bytes >= kDesktopNotificationXmlBytes) return false;
    for (u32 i=0; i<v.bytes;) {
        u32 c=static_cast<u8>(v.data[i++]), n=0, floor=0;
        if (c>=0xC2&&c<=0xDF) {c&=31;n=1;floor=0x80;}
        else if (c>=0xE0&&c<=0xEF) {c&=15;n=2;floor=0x800;}
        else if (c>=0xF0&&c<=0xF4) {c&=7;n=3;floor=0x10000;}
        else if (c>=0x80) return false;
        if (i+n>v.bytes) return false;
        while(n--) { const u32 b=static_cast<u8>(v.data[i++]); if ((b&0xC0)!=0x80) return false; c=(c<<6)|(b&63); }
        if (c<floor || c>0x10FFFF || (c>=0xD800&&c<=0xDFFF) ||
            !((c==9)||(c==10)||(c==13)||(c>=0x20&&c<=0xD7FF)||(c>=0xE000&&c<=0xFFFD)||(c>=0x10000))) return false;
    }
    return true;
}
struct Writer {
    DesktopNotificationXml* out;
    bool append(Utf8View v) noexcept {
        if(v.bytes >= kDesktopNotificationXmlBytes-out->count) return false;
        copy_bytes(out->bytes+out->count,v.data,v.bytes); out->count+=v.bytes; out->bytes[out->count]=0; return true;
    }
    template<usize N> bool literal(const char(&v)[N]) noexcept { return append(utf8_literal(v)); }
    bool escaped(Utf8View v) noexcept {
        for(u32 i=0;i<v.bytes;++i) {
            switch(v.data[i]) {
            case '&': if(!literal("&amp;"))return false;break;
            case '<': if(!literal("&lt;"))return false;break;
            case '>': if(!literal("&gt;"))return false;break;
            case '"': if(!literal("&quot;"))return false;break;
            case '\'': if(!literal("&apos;"))return false;break;
            default:if(!append({v.data+i,1}))return false;
            }
        }
        return true;
    }
};
struct Server final : IClassFactory, INotificationActivationCallback {
    volatile LONG references{1}, server_locks{};
    SRWLOCK lock{};
    bool active{true}, apartment{}, enrolled{};
    DWORD thread{}, cookie{};
    HWND wake_window{}; u32 wake_message{};
    wchar_t app_id[129]{}; u32 app_units{};
    DesktopNotificationActivation queue[kDesktopNotificationActivationCapacity]{};
    u32 head{}, count{};
    Object* notifier{};
    Object* factory{};
    Object* history{};
    Object* xml_factory{};
    struct HistoryRequest {u64 profile{},channel{};bool refresh{};};
    HistoryRequest history_requests[8]{};
    u32 history_head{},history_count{},history_cursor{},history_size{};
    Object* history_snapshot{};
    i64 performance_frequency{};
    static void* operator new(usize, void* p) noexcept { return p; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if(!out)return E_POINTER; *out=nullptr;
        if(same_guid(id,__uuidof(IUnknown))||same_guid(id,__uuidof(IClassFactory))) *out=static_cast<IClassFactory*>(this);
        else if(same_guid(id,__uuidof(INotificationActivationCallback))) *out=static_cast<INotificationActivationCallback*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n=InterlockedDecrement(&references);
        if(!n) { this->~Server(); CoTaskMemFree(this); } return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID id, void** out) override {
        if(!out)return E_POINTER; *out=nullptr; if(outer)return CLASS_E_NOAGGREGATION;
        AcquireSRWLockShared(&lock); const bool ready=active; ReleaseSRWLockShared(&lock);
        return ready ? QueryInterface(id,out) : CO_E_SERVER_STOPPING;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL hold) override {
        if(hold){InterlockedIncrement(&server_locks);AddRef();return S_OK;}
        LONG n=InterlockedCompareExchange(&server_locks,0,0);
        while(n>0){const LONG observed=InterlockedCompareExchange(&server_locks,n-1,n);if(observed==n){Release();return S_OK;}n=observed;}
        return E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE Activate(LPCWSTR id,LPCWSTR args,const NOTIFICATION_USER_INPUT_DATA*,ULONG inputs) override {
        if(inputs || bounded_length(id,128)!=app_units || !bytes_equal(id,app_id,app_units*sizeof(wchar_t))) return E_INVALIDARG;
        DesktopNotificationActivation a{};
        const u32 units=bounded_length(args,kActivationUnits);
        if(!desktop_notification_activation_parse({args,units},&a))return E_INVALIDARG;
        AcquireSRWLockExclusive(&lock);
        HRESULT result=S_OK;
        if(!active)result=CO_E_SERVER_STOPPING;
        else if(count==kDesktopNotificationActivationCapacity)result=HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
        else {
            queue[(head+count)%kDesktopNotificationActivationCapacity]=a; ++count;
            if(wake_window && !PostMessageW(wake_window,wake_message,0,0)) {--count;result=HRESULT_FROM_WIN32(GetLastError());}
        }
        ReleaseSRWLockExclusive(&lock); return result;
    }
};
static_assert(sizeof(Server)<=2048, "early activation owner remains bounded");
Server* owner(DesktopNotifications* n) noexcept {return n?static_cast<Server*>(n->runtime):nullptr;}
bool ui_owner(Server* s) noexcept {return s && s->active && s->thread==GetCurrentThreadId();}
void drop_transport(Server* s) noexcept {release(s->notifier);release(s->factory);release(s->history);release(s->xml_factory);}
void drop_snapshot(Server* s) noexcept {release(s->history_snapshot);s->history_cursor=s->history_size=0;}
void retire_history_request(Server* s) noexcept {
    drop_snapshot(s);if(s->history_count){s->history_requests[s->history_head]={};s->history_head=(s->history_head+1)%8;--s->history_count;}
}
void cancel_profile_history(Server* s,u64 profile) noexcept {
    if(s->history_count&&s->history_requests[s->history_head].profile==profile)drop_snapshot(s);
    Server::HistoryRequest keep[8]{};u32 count=0;
    for(u32 i=0;i<s->history_count;++i){const auto r=s->history_requests[(s->history_head+i)%8];if(r.profile!=profile)keep[count++]=r;}
    copy_bytes(s->history_requests,keep,sizeof(keep));s->history_head=0;s->history_count=count;
}
bool disconnected(HRESULT h) noexcept {return h==RPC_E_DISCONNECTED||h==RPC_S_SERVER_UNAVAILABLE||h==HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE)||h==CO_E_OBJNOTCONNECTED;}
HRESULT transport(Server* s) noexcept {
    if(!s->enrolled)return E_ACCESSDENIED;
    if(s->notifier&&s->factory&&s->history)return S_OK;
    drop_transport(s);
    StringReference manager_name(toast::kToastNotificationManagerClass),toast_name(toast::kToastNotificationClass),id(s->app_id,s->app_units);
    Object* manager=nullptr;Object* second=nullptr;
    HRESULT h=RoGetActivationFactory(manager_name.get(),toast::kIidToastNotificationManagerStatics,reinterpret_cast<void**>(&manager));
    if(SUCCEEDED(h))h=call(manager,toast::kCreateNotifierWithId,id.get(),&s->notifier);
    if(SUCCEEDED(h))h=query(manager,toast::kIidToastNotificationManagerStatics2,&second);
    if(SUCCEEDED(h))h=call(second,toast::kManagerHistory,&s->history);
    if(SUCCEEDED(h))h=RoGetActivationFactory(toast_name.get(),toast::kIidToastNotificationFactory,reinterpret_cast<void**>(&s->factory));
    release(second);release(manager);if(FAILED(h))drop_transport(s);return h;
}
HRESULT document(Server* s,const DesktopNotificationXml& text,Object** out) noexcept {
    *out=nullptr;
    // One unit stays free for the NUL a string reference requires.
    wchar_t wide[kDesktopNotificationXmlBytes];
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.bytes,static_cast<int>(text.count),wide,kDesktopNotificationXmlBytes-1);
    if(!n)return E_INVALIDARG;
    wide[n]=0;
    HRESULT h=S_OK;
    if(!s->xml_factory){StringReference name(toast::kXmlDocumentClass);h=RoGetActivationFactory(name.get(),toast::kIidActivationFactory,reinterpret_cast<void**>(&s->xml_factory));}
    StringReference content(wide,static_cast<u32>(n));Object* instance=nullptr;Object* io=nullptr;
    if(SUCCEEDED(h))h=call(s->xml_factory,toast::kActivateInstance,&instance);
    if(SUCCEEDED(h))h=query(instance,toast::kIidXmlDocumentIO,&io);
    if(SUCCEEDED(h))h=call(io,toast::kXmlLoad,content.get());
    if(SUCCEEDED(h))h=query(instance,toast::kIidXmlDocument,out);
    release(io);release(instance);return h;
}
HRESULT show_once(Server* s,const DesktopNotificationPayload& p,const DesktopNotificationXml& text,bool* attempted) noexcept {
    wchar_t tag_text[17],group_text[17];desktop_notification_hex_id(p.target.message_id,{tag_text,17});desktop_notification_hex_id(p.target.profile_id,{group_text,17});
    StringReference tag(tag_text,16),group(group_text,16);Object* dom=nullptr;Object* item=nullptr;Object* identity=nullptr;
    HRESULT h=transport(s);
    if(SUCCEEDED(h))h=document(s,text,&dom);
    if(SUCCEEDED(h))h=call(s->factory,toast::kCreateToast,dom,&item);
    if(SUCCEEDED(h))h=query(item,toast::kIidToastNotification2,&identity);
    if(SUCCEEDED(h))h=call(identity,toast::kToastPutTag,tag.get());if(SUCCEEDED(h))h=call(identity,toast::kToastPutGroup,group.get());
    if(SUCCEEDED(h))h=call(identity,toast::kToastPutSuppressPopup,static_cast<u8>(p.suppress_popup));
    if(SUCCEEDED(h)){*attempted=true;h=call(s->notifier,toast::kNotifierShow,item);}
    release(identity);release(item);release(dom);return h;
}
HRESULT remove_once(Server* s,u64 profile,u64 message) noexcept {
    wchar_t g[17],t[17];desktop_notification_hex_id(profile,{g,17});
    StringReference id(s->app_id,s->app_units),group(g,16),tag(t,16);
    HRESULT h=transport(s);if(FAILED(h))return h;
    if(message){desktop_notification_hex_id(message,{t,17});return call(s->history,toast::kHistoryRemoveTag,tag.get(),group.get(),id.get());}
    return call(s->history,toast::kHistoryRemoveGroup,group.get(),id.get());
}
}
bool desktop_notification_hex_id(u64 id,Slice<wchar_t> out) noexcept {
    if(!id||!out.data||out.count<17)return false;
    constexpr wchar_t digits[]=L"0123456789abcdef";
    for(u32 i=0;i<16;++i)out[i]=digits[(id>>((15-i)*4))&15];out[16]=0;return true;
}
bool desktop_notification_activation_format(DesktopNotificationActivation a,Slice<wchar_t> out,u32* count) noexcept {
    if(count)*count=0;if(!count||!out.data||out.count<kActivationUnits+1||!a.profile_id||!a.channel_id||!a.message_id)return false;
    copy_bytes(out.data,kActivationPrefix,6*sizeof(wchar_t));
    desktop_notification_hex_id(a.profile_id,{out.data+6,17});out[22]=L';';out[23]=L'c';out[24]=L'=';
    desktop_notification_hex_id(a.channel_id,{out.data+25,17});out[41]=L';';out[42]=L'm';out[43]=L'=';
    desktop_notification_hex_id(a.message_id,{out.data+44,17});*count=kActivationUnits;return true;
}
bool desktop_notification_activation_parse(ConstSlice<wchar_t> text,DesktopNotificationActivation* out) noexcept {
    if(out)*out={};if(!out||!text.data||text.count!=kActivationUnits||!bytes_equal(text.data,kActivationPrefix,6*sizeof(wchar_t))||
        text[22]!=L';'||text[23]!=L'c'||text[24]!=L'='||text[41]!=L';'||text[42]!=L'm'||text[43]!=L'=')return false;
    u64 ids[3]{};constexpr u32 starts[]={6,25,44};
    for(u32 n=0;n<3;++n)for(u32 i=0;i<16;++i){const wchar_t c=text[starts[n]+i];u32 v;
        if(c>=L'0'&&c<=L'9')v=c-L'0';else if(c>=L'a'&&c<=L'f')v=c-L'a'+10;else return false;ids[n]=(ids[n]<<4)|v;}
    if(!ids[0]||!ids[1]||!ids[2])return false;*out={ids[0],ids[1],ids[2]};return true;
}
DesktopNotificationResult desktop_notification_xml(const DesktopNotificationPayload& p,DesktopNotificationXml* out) noexcept {
    if(!out)return failed(DesktopNotificationError::invalid_input,E_POINTER);*out={};
    wchar_t args[61];u32 units{};
    if(!desktop_notification_activation_format(p.target,{args,61},&units)||!xml_utf8(p.title)||!xml_utf8(p.body)||!xml_utf8(p.avatar_file_uri))return failed(DesktopNotificationError::invalid_input,E_INVALIDARG);
    if(p.avatar_file_uri.bytes){const auto v=p.avatar_file_uri;
        if(v.bytes<12||!bytes_equal(v.data,"file:///",8)||!((v.data[8]>='A'&&v.data[8]<='Z')||(v.data[8]>='a'&&v.data[8]<='z'))||v.data[9]!=':'||v.data[10]!='/')return failed(DesktopNotificationError::invalid_input,E_INVALIDARG);
        for(u32 i=0;i<v.bytes;++i)if(v.data[i]=='?'||v.data[i]=='#'||v.data[i]=='\\'||static_cast<u8>(v.data[i])<32)return failed(DesktopNotificationError::invalid_input,E_INVALIDARG);
    }
    char ascii[60];for(u32 i=0;i<60;++i)ascii[i]=static_cast<char>(args[i]);
    Writer w{out};bool ok=w.literal("<toast activationType=\"foreground\" launch=\"")&&w.append({ascii,60})&&w.literal("\"><visual><binding template=\"ToastGeneric\"><text>")&&w.escaped(p.title)&&w.literal("</text><text>")&&w.escaped(p.body)&&w.literal("</text>");
    if(ok&&p.avatar_file_uri.bytes)ok=w.literal("<image placement=\"appLogoOverride\" hint-crop=\"circle\" src=\"")&&w.escaped(p.avatar_file_uri)&&w.literal("\"/>");
    if(ok)ok=w.literal("</binding></visual>");if(ok&&p.silent)ok=w.literal("<audio silent=\"true\"/>");if(ok)ok=w.literal("</toast>");
    if(!ok){*out={};return failed(DesktopNotificationError::capacity,HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));}return {};
}
DesktopNotificationResult desktop_notifications_initialize(DesktopNotifications* n,const DesktopNotificationConfig& c) noexcept {
    if(!n||n->runtime||!app_id_valid(c.app_id)||same_guid(c.activator_id,GUID{})||
        (c.wake_window&&(c.wake_message<WM_APP||c.wake_message>0xBFFF)))return failed(DesktopNotificationError::invalid_input,E_INVALIDARG);
    // The flags RoInitialize(RO_INIT_SINGLETHREADED) passes for a desktop STA.
    HRESULT h=S_OK;if(c.register_activator)h=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    if(FAILED(h))return failed(DesktopNotificationError::apartment,h);
    void* allocation=CoTaskMemAlloc(sizeof(Server));if(!allocation){if(c.register_activator)CoUninitialize();return failed(DesktopNotificationError::memory,E_OUTOFMEMORY);}
    auto* s=new(allocation) Server; s->thread=GetCurrentThreadId();s->apartment=c.register_activator;s->enrolled=c.register_activator;
    LARGE_INTEGER frequency{};if(QueryPerformanceFrequency(&frequency))s->performance_frequency=frequency.QuadPart;
    s->wake_window=c.wake_window;s->wake_message=c.wake_message;s->app_units=c.app_id.count;copy_bytes(s->app_id,c.app_id.data,c.app_id.count*sizeof(wchar_t));
    if(c.register_activator)h=CoRegisterClassObject(c.activator_id,static_cast<IClassFactory*>(s),CLSCTX_LOCAL_SERVER,REGCLS_MULTIPLEUSE,&s->cookie);
    if(FAILED(h)){s->active=false;s->Release();CoUninitialize();return failed(DesktopNotificationError::identity,h);}n->runtime=s;return {};
}
DesktopNotificationShowState desktop_notification_show_state(DesktopNotificationResult result) noexcept {
    if(result.ok())return DesktopNotificationShowState::submitted;
    if(result.error==DesktopNotificationError::transport && disconnected(static_cast<HRESULT>(result.native_error)))
        return DesktopNotificationShowState::retry_required;
    return DesktopNotificationShowState::failed;
}
DesktopNotificationShowResult desktop_notifications_show_once(DesktopNotifications* n,const DesktopNotificationPayload& p) noexcept {
    auto* s=owner(n);
    if(!ui_owner(s))return {DesktopNotificationShowState::failed,failed(DesktopNotificationError::closed,CO_E_NOTINITIALIZED),false};
    DesktopNotificationXml text{};auto result=desktop_notification_xml(p,&text);
    if(!result.ok())return {DesktopNotificationShowState::failed,result,false};
    bool attempted=false;
    const HRESULT h=show_once(s,p,text,&attempted);
    if(disconnected(h)){
        // A retained cursor belongs to the disconnected broker too. Preserve
        // queued cleanup requests, which acquire a new snapshot on a later turn.
        drop_snapshot(s);drop_transport(s);
    }
    result=(FAILED(h)||disconnected(h))?failed(DesktopNotificationError::transport,h):DesktopNotificationResult{};
    return {desktop_notification_show_state(result),result,attempted};
}
DesktopNotificationResult desktop_notifications_show(DesktopNotifications* n,const DesktopNotificationPayload& p) noexcept {
    return desktop_notifications_show_once(n,p).result;
}
DesktopNotificationResult desktop_notifications_remove_message(DesktopNotifications* n,DesktopNotificationActivation a) noexcept {
    auto* s=owner(n);if(!ui_owner(s)||!a.profile_id||!a.channel_id||!a.message_id)return failed(DesktopNotificationError::invalid_input,E_INVALIDARG);
    HRESULT h=remove_once(s,a.profile_id,a.message_id);if(disconnected(h)){drop_transport(s);h=remove_once(s,a.profile_id,a.message_id);}return FAILED(h)?failed(DesktopNotificationError::transport,h):DesktopNotificationResult{};
}
DesktopNotificationResult desktop_notifications_remove_profile(DesktopNotifications* n,u64 profile) noexcept {
    auto* s=owner(n);if(!ui_owner(s)||!profile)return failed(DesktopNotificationError::invalid_input,E_INVALIDARG);
    cancel_profile_history(s,profile);
    HRESULT h=remove_once(s,profile,0);if(disconnected(h)){drop_transport(s);h=remove_once(s,profile,0);}return FAILED(h)?failed(DesktopNotificationError::transport,h):DesktopNotificationResult{};
}
DesktopNotificationResult desktop_notifications_remove_channel(DesktopNotifications* n,u64 profile,u64 channel) noexcept {
    auto* s=owner(n);if(!ui_owner(s)||!profile||!channel)return failed(DesktopNotificationError::invalid_input,E_INVALIDARG);
    for(u32 i=0;i<s->history_count;++i){auto& r=s->history_requests[(s->history_head+i)%8];if(r.profile==profile&&r.channel==channel){if(i==0&&s->history_snapshot)r.refresh=true;return {};}}
    if(s->history_count==8)return failed(DesktopNotificationError::capacity,HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA));
    const bool starting=s->history_count==0;s->history_requests[(s->history_head+s->history_count)%8]={profile,channel};++s->history_count;
    if(starting&&s->wake_window&&!PostMessageW(s->wake_window,s->wake_message,0,0)){--s->history_count;s->history_requests[s->history_head]={};return failed(DesktopNotificationError::transport,HRESULT_FROM_WIN32(GetLastError()));}
    return {};
}
bool desktop_notifications_history_pending(const DesktopNotifications* n) noexcept {
    const auto* s=n?static_cast<const Server*>(n->runtime):nullptr;return s&&s->active&&s->thread==GetCurrentThreadId()&&s->history_count!=0;
}
DesktopNotificationResult desktop_notifications_history_step(DesktopNotifications* n,bool* pending) noexcept {
    if(pending)*pending=false;auto* s=owner(n);if(!ui_owner(s)||!pending)return failed(DesktopNotificationError::invalid_input,E_INVALIDARG);
    if(!s->history_count)return {};
    const auto target=s->history_requests[s->history_head];LARGE_INTEGER start{};QueryPerformanceCounter(&start);
    HRESULT h=S_OK;
    if(!s->history_snapshot){
        h=transport(s);Object* history=nullptr;StringReference id(s->app_id,s->app_units);
        if(SUCCEEDED(h))h=query(s->history,toast::kIidToastNotificationHistory2,&history);
        if(SUCCEEDED(h))h=call(history,toast::kHistoryGetWithId,id.get(),&s->history_snapshot);
        if(SUCCEEDED(h))h=call(s->history_snapshot,toast::kViewSize,&s->history_size);release(history);
    }
    StringReference launch(L"launch");
    u32 visited=0;
    while(SUCCEEDED(h)&&s->history_cursor<s->history_size&&visited<4){
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        if(visited&&s->performance_frequency>0&&now.QuadPart-start.QuadPart>=maximum<i64>(1,s->performance_frequency/500))break;
        Object* item=nullptr;Object* dom=nullptr;Object* element=nullptr;HSTRING args=nullptr;
        h=call(s->history_snapshot,toast::kViewGetAt,s->history_cursor++,&item);++visited;
        if(SUCCEEDED(h))h=call(item,toast::kToastContent,&dom);if(SUCCEEDED(h))h=call(dom,toast::kXmlDocumentElement,&element);
        if(SUCCEEDED(h))h=call(element,toast::kXmlGetAttribute,launch.get(),&args);
        DesktopNotificationActivation a{};
        if(SUCCEEDED(h)&&desktop_notification_activation_parse(toast::string_text(args),&a)&&a.profile_id==target.profile&&a.channel_id==target.channel)h=remove_once(s,target.profile,a.message_id);
        if(args)WindowsDeleteString(args);release(element);release(dom);release(item);
    }
    if(FAILED(h))retire_history_request(s);
    else if(s->history_cursor==s->history_size){
        auto& request=s->history_requests[s->history_head];
        if(request.refresh){drop_snapshot(s);request.refresh=false;}else retire_history_request(s);
    }
    if(disconnected(h))drop_transport(s);*pending=s->history_count!=0;
    return FAILED(h)?failed(DesktopNotificationError::transport,h):DesktopNotificationResult{};
}
DesktopNotificationResult desktop_notifications_setting(DesktopNotifications* n,u32* out) noexcept {
    auto* s=owner(n);if(!ui_owner(s)||!out)return failed(DesktopNotificationError::invalid_input,E_INVALIDARG);*out=0;
    HRESULT h=transport(s);toast::NotificationSetting value{};if(SUCCEEDED(h))h=call(s->notifier,toast::kNotifierSetting,&value);
    if(disconnected(h)){drop_transport(s);h=transport(s);if(SUCCEEDED(h))h=call(s->notifier,toast::kNotifierSetting,&value);}
    // An unpackaged AUMID has no per-app record until its first toast; Windows
    // then reports Element not found, which means the default (enabled).
    if(h==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)){h=S_OK;value=toast::NotificationSetting_Enabled;}
    if(SUCCEEDED(h))*out=static_cast<u32>(value);return FAILED(h)?failed(DesktopNotificationError::transport,h):DesktopNotificationResult{};
}
DesktopNotificationResult desktop_notifications_set_wake(DesktopNotifications* n,HWND window,u32 message) noexcept {
    auto* s=owner(n);if(!ui_owner(s)||!window||message<WM_APP||message>0xBFFF)return failed(DesktopNotificationError::invalid_input,E_INVALIDARG);
    AcquireSRWLockExclusive(&s->lock);s->wake_window=window;s->wake_message=message;
    const bool posted=(!s->count&&!s->history_count)||PostMessageW(window,message,0,0)!=0;const DWORD error=posted?0:GetLastError();
    ReleaseSRWLockExclusive(&s->lock);return posted?DesktopNotificationResult{}:failed(DesktopNotificationError::transport,HRESULT_FROM_WIN32(error));
}
bool desktop_notifications_take_activation(DesktopNotifications* n,DesktopNotificationActivation* out) noexcept {
    auto* s=owner(n);if(!ui_owner(s)||!out)return false;AcquireSRWLockExclusive(&s->lock);const bool have=s->count!=0;
    if(have){*out=s->queue[s->head];s->queue[s->head]={};s->head=(s->head+1)%kDesktopNotificationActivationCapacity;--s->count;}
    ReleaseSRWLockExclusive(&s->lock);return have;
}
void desktop_notifications_discard_activations(DesktopNotifications* n) noexcept {
    auto* s=owner(n);if(!ui_owner(s))return;AcquireSRWLockExclusive(&s->lock);clear_bytes(s->queue,sizeof(s->queue));s->head=s->count=0;ReleaseSRWLockExclusive(&s->lock);
}
DesktopNotificationResult desktop_notifications_factory(DesktopNotifications* n,void** out) noexcept {
    auto* s=owner(n);if(!ui_owner(s)||!out)return failed(DesktopNotificationError::invalid_input,E_INVALIDARG);return FAILED(s->QueryInterface(__uuidof(IClassFactory),out))?failed(DesktopNotificationError::transport,E_NOINTERFACE):DesktopNotificationResult{};
}
void desktop_notifications_release(DesktopNotifications* n) noexcept {
    auto* s=owner(n);if(!s||s->thread!=GetCurrentThreadId())return;n->runtime=nullptr;
    AcquireSRWLockExclusive(&s->lock);s->active=false;s->wake_window=nullptr;clear_bytes(s->queue,sizeof(s->queue));s->head=s->count=0;ReleaseSRWLockExclusive(&s->lock);
    if(s->cookie)CoRevokeClassObject(s->cookie);drop_snapshot(s);clear_bytes(s->history_requests,sizeof(s->history_requests));s->history_count=0;
    drop_transport(s);const bool apartment=s->apartment;s->Release();if(apartment)CoUninitialize();
}
u32 desktop_notifications_owned_bytes(const DesktopNotifications* n) noexcept {return n&&n->runtime?sizeof(Server):0;}
}
