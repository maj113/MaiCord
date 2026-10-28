#include "maicord/platform/notification_identity.hpp"
#include "maicord/sys/registry.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord {
namespace {
constexpr u32 kPathUnits=1024, kKeyUnits=512, kClassesUnits=200;
constexpr wchar_t kDefaultClasses[]=L"Software\\Classes";
constexpr wchar_t kOwner[]=L"MaiCordOwner";
struct Paths {
    wchar_t exe[kPathUnits]{}, icon[kPathUnits]{}, command[kPathUnits+24]{};
    wchar_t aumid_key[kKeyUnits]{}, clsid_key[kKeyUnits]{}, server_key[kKeyUnits]{};
    wchar_t id[129]{}, name[129]{}, nonce[65]{}, activator[40]{};
};
NotificationIdentityResult failure(NotificationIdentityError e,u32 code) noexcept {return {e,code};}
bool token(ConstSlice<wchar_t> s,u32 maximum,bool punctuation) noexcept {
    if(!s.data||!s.count||s.count>maximum)return false;
    for(u32 i=0;i<s.count;++i){const wchar_t c=s[i];if(!((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||(punctuation&&(c==L'.'||c==L'-'||c==L'_'))))return false;}return true;
}
bool absolute_path(ConstSlice<wchar_t> s) noexcept {
    if(!s.data||s.count<4||s.count>=kPathUnits||!((s[0]>=L'A'&&s[0]<=L'Z')||(s[0]>=L'a'&&s[0]<=L'z'))||s[1]!=L':'||s[2]!=L'\\')return false;
    u32 segment=3;
    for(u32 i=3;i<=s.count;++i){
        if(i==s.count||s[i]==L'\\') {
            if(i==segment||(i-segment==1&&s[segment]==L'.')||(i-segment==2&&s[segment]==L'.'&&s[segment+1]==L'.')||s[i-1]==L' '||s[i-1]==L'.')return false;segment=i+1;
        } else if(s[i]<32||s[i]==L'"'||s[i]==L'/'||s[i]==L':'||s[i]==L'*'||s[i]==L'?'||s[i]==L'<'||s[i]==L'>'||s[i]==L'|')return false;
    }return true;
}
// Relative HKCU path: alphanumeric, '.', '-', '_' segments joined by single backslashes.
bool registry_path(ConstSlice<wchar_t> s) noexcept {
    if(!s.data||!s.count||s.count>kClassesUnits||s[0]==L'\\'||s[s.count-1]==L'\\')return false;
    for(u32 i=0;i<s.count;++i){
        const wchar_t c=s[i];
        if(c==L'\\'){if(s[i-1]==L'\\')return false;continue;}
        if(!token({&s[i],1},1,true))return false;
    }return true;
}
bool extension(ConstSlice<wchar_t> s,const wchar_t (&e)[5]) noexcept {
    if(s.count<5)return false;
    for(u32 i=0;i<4;++i){wchar_t c=s[s.count-4+i];if(c>=L'A'&&c<=L'Z')c=static_cast<wchar_t>(c-L'A'+L'a');if(c!=e[i])return false;}return true;
}
u32 units(const wchar_t* s) noexcept {u32 n=0;while(s[n])++n;return n;}
struct Text {
    wchar_t* out; u32 capacity, count{}; bool ok{true};
    Text& add(ConstSlice<wchar_t> s) noexcept {
        if(!ok||s.count>=capacity-count){ok=false;return *this;}
        copy_bytes(out+count,s.data,s.count*sizeof(wchar_t));count+=s.count;out[count]=0;return *this;
    }
    Text& add(const wchar_t* s) noexcept {return add(ConstSlice<wchar_t>{s,units(s)});}
};
bool paths(const NotificationIdentityConfig& c,Paths* p) noexcept {
    if(!notification_identity_valid(c)||StringFromGUID2(c.activator_id,p->activator,40)!=39)return false;
    const ConstSlice<wchar_t> classes=c.classes_key.count?c.classes_key:ConstSlice<wchar_t>{kDefaultClasses,sizeof(kDefaultClasses)/sizeof(wchar_t)-1};
    Text exe{p->exe,kPathUnits},icon{p->icon,kPathUnits},id{p->id,129},name{p->name,129},nonce{p->nonce,65};
    Text command{p->command,kPathUnits+24},aumid{p->aumid_key,kKeyUnits},clsid{p->clsid_key,kKeyUnits},server{p->server_key,kKeyUnits};
    exe.add(c.executable_path);id.add(c.app_id);name.add(c.display_name);nonce.add(c.ownership_nonce);
    if(c.icon_path.count)icon.add(c.icon_path);
    command.add(L"\"").add(c.executable_path).add(L"\" --toast-activated");
    aumid.add(classes).add(L"\\AppUserModelId\\").add(c.app_id);
    clsid.add(classes).add(L"\\CLSID\\").add(p->activator);
    server.add(p->clsid_key).add(L"\\LocalServer32");
    return exe.ok&&icon.ok&&id.ok&&name.ok&&nonce.ok&&command.ok&&aumid.ok&&clsid.ok&&server.ok;
}
LSTATUS set_string(HKEY key,const wchar_t* name,const wchar_t* text) noexcept {
    return RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(text),(units(text)+1)*sizeof(wchar_t));
}
LSTATUS clear_value(HKEY key,const wchar_t* name) noexcept {
    const LSTATUS r=RegDeleteValueW(key,name);return r==ERROR_FILE_NOT_FOUND?ERROR_SUCCESS:r;
}
bool string_equals(HKEY key,const wchar_t* name,const wchar_t* expected) noexcept {
    wchar_t value[kPathUnits+32]{};DWORD bytes=sizeof(value)-sizeof(wchar_t),type=0;
    const LSTATUS r=RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(value),&bytes);
    const DWORD want=(units(expected)+1)*sizeof(wchar_t);
    return r==ERROR_SUCCESS&&type==REG_SZ&&bytes==want&&bytes_equal(value,expected,want);
}
bool value_absent(HKEY key,const wchar_t* name) noexcept {return RegQueryValueExW(key,name,nullptr,nullptr,nullptr,nullptr)==ERROR_FILE_NOT_FOUND;}
enum class Key : u8 { absent, owned, foreign, error };
Key open_key(const wchar_t* path,const wchar_t* nonce,HKEY* out,LSTATUS* status) noexcept {
    *out=nullptr;const LSTATUS r=RegOpenKeyExW(HKEY_CURRENT_USER,path,0,KEY_QUERY_VALUE,out);*status=r;
    if(r==ERROR_FILE_NOT_FOUND||r==ERROR_PATH_NOT_FOUND){*out=nullptr;return Key::absent;}
    if(r!=ERROR_SUCCESS){*out=nullptr;return Key::error;}
    if(string_equals(*out,kOwner,nonce))return Key::owned;
    RegCloseKey(*out);*out=nullptr;return Key::foreign;
}
bool aumid_current(HKEY key,const Paths& p) noexcept {
    return string_equals(key,L"DisplayName",p.name)&&string_equals(key,L"CustomActivator",p.activator)&&
        (p.icon[0]?string_equals(key,L"IconUri",p.icon):value_absent(key,L"IconUri"));
}
bool clsid_current(HKEY key,const Paths& p) noexcept {
    HKEY server=nullptr;
    if(!string_equals(key,L"MaiCordAumid",p.id)||RegOpenKeyExW(key,L"LocalServer32",0,KEY_QUERY_VALUE,&server)!=ERROR_SUCCESS)return false;
    const bool ok=string_equals(server,nullptr,p.command)&&string_equals(server,L"ServerExecutable",p.exe);
    RegCloseKey(server);return ok;
}
// Opens or creates one key. An existing key must already carry the nonce;
// created reports a new key even when stamping it failed, for rollback.
LSTATUS claim(const wchar_t* path,const wchar_t* nonce,HKEY* out,bool* created) noexcept {
    *out=nullptr;*created=false;DWORD disposition{};
    LSTATUS r=RegCreateKeyExW(HKEY_CURRENT_USER,path,0,nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE|KEY_CREATE_SUB_KEY,nullptr,out,&disposition);
    if(r!=ERROR_SUCCESS){*out=nullptr;return r;}
    *created=disposition==REG_CREATED_NEW_KEY;
    if(*created)r=set_string(*out,kOwner,nonce);
    else if(!string_equals(*out,kOwner,nonce))r=ERROR_ALREADY_EXISTS;
    if(r!=ERROR_SUCCESS){RegCloseKey(*out);*out=nullptr;}
    return r;
}
}
bool notification_identity_valid(const NotificationIdentityConfig& c) noexcept {
    if(!token(c.app_id,128,true)||!token(c.ownership_nonce,64,false)||c.ownership_nonce.count<16||
        c.activator_id==GUID{}||!absolute_path(c.executable_path)||
        !c.display_name.data||!c.display_name.count||c.display_name.count>128)return false;
    for(u32 i=0;i<c.display_name.count;++i)if(c.display_name[i]<32||(c.display_name[i]>=0xD800&&c.display_name[i]<=0xDFFF))return false;
    if(c.icon_path.count&&(!absolute_path(c.icon_path)||!(extension(c.icon_path,L".png")||extension(c.icon_path,L".ico"))))return false;
    return !c.classes_key.count||registry_path(c.classes_key);
}
NotificationIdentityResult notification_identity_set_process(ConstSlice<wchar_t> id) noexcept {
    if(!token(id,128,true))return failure(NotificationIdentityError::invalid_input,ERROR_INVALID_PARAMETER);
    wchar_t text[129]{};copy_bytes(text,id.data,id.count*sizeof(wchar_t));const HRESULT h=SetCurrentProcessExplicitAppUserModelID(text);
    return FAILED(h)?failure(NotificationIdentityError::process,static_cast<u32>(h)):NotificationIdentityResult{};
}
NotificationIdentityResult notification_identity_check(const NotificationIdentityConfig& c) noexcept {
    Paths p{};if(!paths(c,&p))return failure(NotificationIdentityError::invalid_input,ERROR_INVALID_PARAMETER);
    HKEY aumid=nullptr,clsid=nullptr;LSTATUS aumid_status{},clsid_status{};
    const Key a=open_key(p.aumid_key,p.nonce,&aumid,&aumid_status),k=open_key(p.clsid_key,p.nonce,&clsid,&clsid_status);
    NotificationIdentityResult result{};
    if(a==Key::error||k==Key::error)result=failure(NotificationIdentityError::registry,static_cast<u32>(a==Key::error?aumid_status:clsid_status));
    else if(a==Key::foreign||k==Key::foreign)result=failure(NotificationIdentityError::foreign_resource,ERROR_ACCESS_DENIED);
    else if(a==Key::absent&&k==Key::absent)result=failure(NotificationIdentityError::absent,ERROR_FILE_NOT_FOUND);
    else if(a==Key::absent||k==Key::absent||!aumid_current(aumid,p)||!clsid_current(clsid,p))result=failure(NotificationIdentityError::stale,ERROR_INVALID_DATA);
    if(aumid)RegCloseKey(aumid);if(clsid)RegCloseKey(clsid);return result;
}
NotificationIdentityResult notification_identity_create(const NotificationIdentityConfig& c) noexcept {
    Paths p{};if(!paths(c,&p))return failure(NotificationIdentityError::invalid_input,ERROR_INVALID_PARAMETER);
    const DWORD attributes=GetFileAttributesW(p.exe);
    if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_DIRECTORY))return failure(NotificationIdentityError::invalid_input,ERROR_FILE_NOT_FOUND);
    HKEY clsid=nullptr,server=nullptr,aumid=nullptr;bool clsid_created=false,aumid_created=false;
    // The COM server first, so CustomActivator never names an unregistered class.
    LSTATUS r=claim(p.clsid_key,p.nonce,&clsid,&clsid_created);
    if(r==ERROR_SUCCESS)r=set_string(clsid,L"MaiCordAumid",p.id);
    if(r==ERROR_SUCCESS)r=RegCreateKeyExW(clsid,L"LocalServer32",0,nullptr,0,KEY_SET_VALUE,nullptr,&server,nullptr);
    if(r==ERROR_SUCCESS)r=set_string(server,nullptr,p.command);
    if(r==ERROR_SUCCESS)r=set_string(server,L"ServerExecutable",p.exe);
    if(r==ERROR_SUCCESS)r=claim(p.aumid_key,p.nonce,&aumid,&aumid_created);
    if(r==ERROR_SUCCESS)r=set_string(aumid,L"DisplayName",p.name);
    if(r==ERROR_SUCCESS)r=set_string(aumid,L"CustomActivator",p.activator);
    if(r==ERROR_SUCCESS)r=p.icon[0]?set_string(aumid,L"IconUri",p.icon):clear_value(aumid,L"IconUri");
    if(server)RegCloseKey(server);if(clsid)RegCloseKey(clsid);if(aumid)RegCloseKey(aumid);
    if(r==ERROR_SUCCESS){NotificationIdentityResult done{};done.registry_created=clsid_created||aumid_created;return done;}
    // Only keys exclusively created by this invocation are eligible for rollback.
    bool removed=true;
    if(aumid_created)removed=RegDeleteKeyW(HKEY_CURRENT_USER,p.aumid_key)==ERROR_SUCCESS;
    if(clsid_created){
        const LSTATUS s=RegDeleteKeyW(HKEY_CURRENT_USER,p.server_key);
        removed=(s==ERROR_SUCCESS||s==ERROR_FILE_NOT_FOUND)&&RegDeleteKeyW(HKEY_CURRENT_USER,p.clsid_key)==ERROR_SUCCESS&&removed;
    }
    auto result=failure(!removed?NotificationIdentityError::rollback:r==ERROR_ALREADY_EXISTS?NotificationIdentityError::foreign_resource:
        NotificationIdentityError::registry,static_cast<u32>(r));
    result.registry_created=!removed;return result;
}
NotificationIdentityResult notification_identity_remove_owned(const NotificationIdentityConfig& c) noexcept {
    Paths p{};if(!paths(c,&p))return failure(NotificationIdentityError::invalid_input,ERROR_INVALID_PARAMETER);
    HKEY aumid=nullptr,clsid=nullptr;LSTATUS aumid_status{},clsid_status{};
    const Key a=open_key(p.aumid_key,p.nonce,&aumid,&aumid_status),k=open_key(p.clsid_key,p.nonce,&clsid,&clsid_status);
    if(aumid)RegCloseKey(aumid);if(clsid)RegCloseKey(clsid);
    if(a==Key::error||k==Key::error)return failure(NotificationIdentityError::registry,static_cast<u32>(a==Key::error?aumid_status:clsid_status));
    if(a==Key::foreign||k==Key::foreign)return failure(NotificationIdentityError::not_owned,ERROR_ACCESS_DENIED);
    if(a==Key::absent&&k==Key::absent)return failure(NotificationIdentityError::absent,ERROR_FILE_NOT_FOUND);
    // RegDeleteKeyW is not recursive: a key holding foreign subkeys survives.
    LSTATUS r=a==Key::owned?RegDeleteKeyW(HKEY_CURRENT_USER,p.aumid_key):ERROR_SUCCESS;
    if(k==Key::owned){
        const LSTATUS s=RegDeleteKeyW(HKEY_CURRENT_USER,p.server_key);
        const LSTATUS t=s==ERROR_SUCCESS||s==ERROR_FILE_NOT_FOUND?RegDeleteKeyW(HKEY_CURRENT_USER,p.clsid_key):s;
        if(r==ERROR_SUCCESS)r=t;
    }
    if(r!=ERROR_SUCCESS){auto result=failure(NotificationIdentityError::rollback,static_cast<u32>(r));result.registry_created=true;return result;}
    return {};
}
}
