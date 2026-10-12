// QA only. Independently observe system MapCharacters; never linked into the app.
// FBP1: count, then [units,position,length,family_units,weight,style,stretch,rtl],
// UTF-16 source and family. FBS1: profile count, then
// [family_units,weight,style,stretch,prefix_scalar,suffix_scalar], UTF-16 family.
// FBR1 uses the same sweep profile but queries the whole prefix/scalar/suffix.
// FBE1: count, then [scalar,anchor,weight,style]; queries scalar + VS16 + anchor
// with no base family. This observes a directional boundary relation, not an
// equivalence relation: a joining scalar can connect otherwise separate groups.
// Each sweep emits 0x110000 records; surrogate records have E_INVALIDARG.
// Output repeats magic/count, then [HRESULT,length,font_id,scale_bits] per query,
// followed by font count and each font's metadata, axes, path and family.
#include "maicord/core/base.hpp"
#include "maicord/core/memory.hpp"
#include <dwrite_3.h>
#include "private_font_collection.hpp"
namespace maicord {namespace {
constexpr u32 no_font=0xffffffffu,capacity=1024;
u8 input_buffer[65536],output_buffer[65536];u32 input_at=0,input_end=0,output_end=0;
IDWriteFont3* fonts[capacity]{};u32 font_count=0;
template<class T> void release(T*& value) noexcept {if(value){value->Release();value=nullptr;}}
bool read(void* target,u32 bytes) noexcept {
    auto* out=static_cast<u8*>(target);
    while(bytes){if(input_at==input_end){DWORD n=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),input_buffer,sizeof(input_buffer),&n,nullptr)||!n)return false;input_at=0;input_end=n;}
        const u32 n=minimum(bytes,input_end-input_at);copy_bytes(out,input_buffer+input_at,n);out+=n;input_at+=n;bytes-=n;}return true;
}
bool flush() noexcept {DWORD n=0;const bool ok=!output_end||(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),output_buffer,output_end,&n,nullptr)&&n==output_end);output_end=0;return ok;}
bool save(const void* source,u32 bytes) noexcept {
    const auto* in=static_cast<const u8*>(source);
    while(bytes){if(output_end==sizeof(output_buffer)&&!flush())return false;const u32 n=minimum(bytes,static_cast<u32>(sizeof(output_buffer))-output_end);
        copy_bytes(output_buffer+output_end,in,n);output_end+=n;in+=n;bytes-=n;}return true;
}
bool argument(const wchar_t** command,wchar_t* out) noexcept {
    const wchar_t* p=*command;while(*p==' '||*p=='\t')++p;const bool quoted=*p=='"';if(quoted)++p;u32 n=0;
    while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){if(n==32767)return false;out[n++]=*p++;}
    if(quoted){if(*p!='"')return false;++p;}out[n]=0;*command=p;return n!=0;
}
struct Source final : IDWriteTextAnalysisSource {
    wchar_t text[32768]{};u32 units=0;DWRITE_READING_DIRECTION direction=DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID))||bytes_equal(&iid,&__uuidof(IDWriteTextAnalysisSource),sizeof(GUID)))*out=this;
        return *out?S_OK:E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;} ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 at,const WCHAR** out,UINT32* n) override {
        if(!out||!n)return E_POINTER;*out=at<units?text+at:nullptr;*n=at<units?units-at:0;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 at,const WCHAR** out,UINT32* n) override {
        if(!out||!n)return E_POINTER;if(at>units)return E_INVALIDARG;*out=at?text:nullptr;*n=at;return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {return direction;}
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 at,UINT32* n,const WCHAR** locale) override {
        if(!n||!locale)return E_POINTER;*n=at<units?units-at:0;*locale=L"en-US";return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 at,UINT32* n,IDWriteNumberSubstitution** value) override {
        if(!n||!value)return E_POINTER;*n=at<units?units-at:0;*value=nullptr;return S_OK;
    }
    void scalar(u32 cp) noexcept {
        if(cp<=0xffff)text[units++]=static_cast<wchar_t>(cp);
        else{cp-=0x10000;text[units++]=static_cast<wchar_t>(0xd800+(cp>>10));text[units++]=static_cast<wchar_t>(0xdc00+(cp&1023));}
    }
};
Source source;
bool identify(IDWriteFont* font,u32* out) noexcept {
    *out=no_font;if(!font)return true;
    for(u32 i=0;i<font_count;++i)if(fonts[i]->Equals(font)){*out=i;return true;}
    if(font_count==capacity)return false;IDWriteFont3* saved=nullptr;
    if(FAILED(font->QueryInterface(__uuidof(IDWriteFont3),reinterpret_cast<void**>(&saved))))return false;
    *out=font_count;fonts[font_count++]=saved;return true;
}
bool metadata(IDWriteFont3* font) noexcept {
    IDWriteFontFace3* base=nullptr;IDWriteFontFace5* face=nullptr;IDWriteFontFile* file=nullptr;IDWriteFontFileLoader* loader=nullptr;
    IDWriteLocalFontFileLoader* local=nullptr;IDWriteFontFamily* family=nullptr;IDWriteLocalizedStrings* names=nullptr;
    struct Cleanup {IDWriteFontFace3*& b;IDWriteFontFace5*& f;IDWriteFontFile*& file;IDWriteFontFileLoader*& l;IDWriteLocalFontFileLoader*& local;IDWriteFontFamily*& family;IDWriteLocalizedStrings*& names;
        ~Cleanup() noexcept {release(names);release(family);release(local);release(l);release(file);release(f);release(b);}} cleanup{base,face,file,loader,local,family,names};
    if(FAILED(font->CreateFontFace(&base))||FAILED(base->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&face))))return false;
    u32 count=0,key_bytes=0,path_units=0,name_units=0,language=0;const void* key=nullptr;BOOL found=FALSE;wchar_t path[32768],name[32768];
    if(FAILED(face->GetFiles(&count,nullptr))||count!=1||FAILED(face->GetFiles(&count,&file))||FAILED(file->GetReferenceKey(&key,&key_bytes))||FAILED(file->GetLoader(&loader))||
       FAILED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),reinterpret_cast<void**>(&local)))||FAILED(local->GetFilePathLengthFromKey(key,key_bytes,&path_units))||path_units>=32768||
       FAILED(local->GetFilePathFromKey(key,key_bytes,path,32768))||FAILED(font->GetFontFamily(&family))||FAILED(family->GetFamilyNames(&names))||
       FAILED(names->FindLocaleName(L"en-us",&language,&found)))return false;
    if(!found)language=0;if(FAILED(names->GetStringLength(language,&name_units))||name_units>=32768||FAILED(names->GetString(language,name,32768)))return false;
    DWRITE_FONT_AXIS_VALUE axes[64];count=face->GetFontAxisValueCount();if(count>64||FAILED(face->GetFontAxisValues(axes,count)))return false;
    const u32 row[]{face->GetIndex(),static_cast<u32>(font->GetWeight()),static_cast<u32>(font->GetStyle()),static_cast<u32>(font->GetStretch()),static_cast<u32>(font->GetSimulations()),count,path_units,name_units};
    return save(row,sizeof(row))&&save(axes,count*sizeof(*axes))&&save(path,path_units*2)&&save(name,name_units*2);
}
bool query(IDWriteFontFallback* fallback,IDWriteFontCollection* collection,const wchar_t* family,u32 position,u32 length,u32 weight,u32 style,u32 stretch) noexcept {
    IDWriteFont* font=nullptr;u32 row[]{0,0,no_font,0};float scale=0;
    const HRESULT hr=fallback->MapCharacters(&source,position,length,collection,*family?family:nullptr,static_cast<DWRITE_FONT_WEIGHT>(weight),static_cast<DWRITE_FONT_STYLE>(style),static_cast<DWRITE_FONT_STRETCH>(stretch),row+1,&font,&scale);
    row[0]=static_cast<u32>(hr);copy_bytes(row+3,&scale,4);const bool okay=identify(font,row+2);release(font);
    return okay&&save(row,sizeof(row));
}
bool run() noexcept {
    u32 header[2];if(!read(header,sizeof(header))||(header[0]!=0x31504246&&header[0]!=0x31534246&&header[0]!=0x31524246&&header[0]!=0x31454246)||!save(header,sizeof(header)))return false;
    IDWriteFactory3* factory=nullptr;IDWriteFontFallback* fallback=nullptr;IDWriteFontCollection* collection=nullptr;
    struct Cleanup {IDWriteFactory3*& f;IDWriteFontFallback*& b;IDWriteFontCollection*& c;~Cleanup() noexcept{release(c);release(b);release(f);}} cleanup{factory,fallback,collection};
    if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory3),reinterpret_cast<IUnknown**>(&factory)))||FAILED(factory->GetSystemFontFallback(&fallback)))return false;
    wchar_t path[32768],family[32768];const wchar_t* command=GetCommandLineW();if(!argument(&command,path))return false;
    const bool custom=argument(&command,path);if(custom){
        const bool all=argument(&command,family);if(all&&!font_reference::all_faces_option(family))return false;
        if(FAILED(font_reference::private_collection(factory,path,all,&collection)))return false;
    }else if(FAILED(static_cast<IDWriteFactory*>(factory)->GetSystemFontCollection(&collection,FALSE)))return false;
    // Explicit QA-only mapping policies distinguish route boundaries from font
    // identity. Require a private collection; ordinary captures leave this unset.
    wchar_t mapping_mode[3]{};const u32 mode_units=GetEnvironmentVariableW(L"MAICORD_FONT_FALLBACK_MAPPING",mapping_mode,3);
    if(mode_units){if(!custom||mode_units!=1||((mapping_mode[0]<'1'||mapping_mode[0]>'9')&&mapping_mode[0]!='A'))return false;
        IDWriteFontFallbackBuilder* builder=nullptr;if(FAILED(factory->CreateFontFallbackBuilder(&builder)))return false;
        const wchar_t* target[]{mapping_mode[0]=='6'?L"MaiCord Missing Family":L"MaiCord Range Mapping"};const DWRITE_UNICODE_RANGE ranges[]{{0,0x7f},{0x80,0x2ff},{0x300,0x36f},{0x370,0x10ffff}};
        HRESULT hr=S_OK;
        if(mapping_mode[0]=='1'){const DWRITE_UNICODE_RANGE first[]{ranges[0],ranges[2]},second[]{ranges[1],ranges[3]};
            hr=builder->AddMapping(first,2,target,1,collection,nullptr,nullptr,1);if(SUCCEEDED(hr))hr=builder->AddMapping(second,2,target,1,collection,nullptr,nullptr,1);}
        else if(mapping_mode[0]=='2'||mapping_mode[0]=='6')hr=builder->AddMapping(ranges,4,target,1,collection,nullptr,nullptr,1);
        else if(mapping_mode[0]=='3'){for(u32 i=0;i<4&&SUCCEEDED(hr);++i)hr=builder->AddMapping(ranges+i,1,target,1,collection,nullptr,nullptr,1);}
        else if(mapping_mode[0]=='4')hr=builder->AddMapping(ranges,1,target,1,collection,nullptr,nullptr,1);
        else if(mapping_mode[0]=='7'){const DWRITE_UNICODE_RANGE narrow{0x61,0x61},broad{0,0x10ffff};
            hr=builder->AddMapping(&narrow,1,target,1,collection,nullptr,nullptr,1);if(SUCCEEDED(hr))hr=builder->AddMapping(&broad,1,target,1,collection,nullptr,nullptr,1);}
        else if(mapping_mode[0]=='8'){const DWRITE_UNICODE_RANGE broad{0,0x10ffff};hr=builder->AddMapping(&broad,1,target,1,collection,nullptr,nullptr,1);}
        else if(mapping_mode[0]=='9'){} // Empty fallback policy, for routing controls.
        else if(mapping_mode[0]=='A'){const wchar_t* other[]{L"MaiCord Range Other"};const DWRITE_UNICODE_RANGE after_ascii{0x80,0x10ffff};
            hr=builder->AddMapping(ranges,1,target,1,collection,nullptr,nullptr,1);if(SUCCEEDED(hr))hr=builder->AddMapping(&after_ascii,1,other,1,collection,nullptr,nullptr,1);}
        else{const DWRITE_UNICODE_RANGE selectors[]{{0x180b,0x180d},{0x180f,0x180f},{0xfe00,0xfe0f},{0xe0100,0xe01ef}};
            hr=builder->AddMapping(selectors,4,target,1,collection,nullptr,nullptr,1);}
        IDWriteFontFallback* mapped=nullptr;if(SUCCEEDED(hr))hr=builder->CreateFontFallback(&mapped);release(builder);if(FAILED(hr))return false;release(fallback);fallback=mapped;
    }
    for(u32 i=0;i<header[1];++i){
        if(header[0]==0x31454246){u32 q[4];if(!read(q,sizeof(q))||q[0]>0x10ffff||q[1]>0x10ffff||(q[0]>=0xd800&&q[0]<=0xdfff)||(q[1]>=0xd800&&q[1]<=0xdfff))return false;
            source.units=0;source.scalar(q[0]);source.scalar(0xfe0f);source.scalar(q[1]);
            if(!query(fallback,collection,L"",0,source.units,q[2],q[3],5))return false;
        }else if(header[0]!=0x31504246){u32 q[6];if(!read(q,sizeof(q))||q[0]>=32768||q[4]>0x10ffff||q[5]>0x10ffff||!read(family,q[0]*2))return false;family[q[0]]=0;
            for(u32 cp=0;cp<0x110000;++cp){if(cp>=0xd800&&cp<=0xdfff){const u32 row[]{static_cast<u32>(E_INVALIDARG),0,no_font,0};if(!save(row,sizeof(row)))return false;continue;}
                source.units=0;if(q[4])source.scalar(q[4]);const u32 at=source.units;source.scalar(cp);const u32 length=source.units-at;if(q[5])source.scalar(q[5]);
                const bool whole=header[0]==0x31524246;
                if(!query(fallback,collection,family,whole?0:at,whole?source.units:length,q[1],q[2],q[3]))return false;}
        }else{u32 q[8];if(!read(q,sizeof(q))||q[0]>32768||q[1]>q[0]||q[2]>q[0]-q[1]||q[3]>=32768||q[7]>1||!read(source.text,q[0]*2)||!read(family,q[3]*2))return false;
            source.units=q[0];source.direction=q[7]?DWRITE_READING_DIRECTION_RIGHT_TO_LEFT:DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;family[q[3]]=0;
            if(!query(fallback,collection,family,q[1],q[2],q[4],q[5],q[6]))return false;
        }
    }
    if(!save(&font_count,4))return false;for(u32 i=0;i<font_count;++i)if(!metadata(fonts[i]))return false;return flush();
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_fallback_reference_entry() noexcept {
    using namespace maicord;const bool okay=run();for(u32 i=0;i<font_count;++i)release(fonts[i]);ExitProcess(okay?0:1);
}
