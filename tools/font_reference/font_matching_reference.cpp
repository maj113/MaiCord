// QA only: capture WWS family candidates and Windows matching order.
#include "maicord/core/slice.hpp"
#include <dwrite_3.h>
#include "private_font_collection.hpp"
namespace maicord {namespace {
char buffer[65536];u32 used=0;bool ok=true;IDWriteFontFace5* faces[256];u32 face_count=0;u32 face_properties[256][4]{};
void flush() noexcept {DWORD n=0;if(used&&(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),buffer,used,&n,nullptr)||n!=used))ok=false;used=0;}
void ch(char c) noexcept {if(used==sizeof(buffer))flush();buffer[used++]=c;}
void text(const char* s) noexcept {while(*s)ch(*s++);}
void number(u32 v) noexcept {char s[16];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);while(n)ch(s[--n]);}
void wide(const wchar_t* s) noexcept {const char hex[]="0123456789abcdef";ch('"');while(*s){text("\\u");for(int shift=12;shift>=0;shift-=4)ch(hex[(*s>>shift)&15]);++s;}ch('"');}
#define CHECK(x) do{if(!(x)){text("FAIL line=");number(__LINE__);text("\n");return false;}}while(0)
template<class T> void release(T*& p) noexcept {if(p){p->Release();p=nullptr;}}
bool argument(const wchar_t** command,wchar_t* out) noexcept {const wchar_t* p=*command;while(*p==' '||*p=='\t')++p;bool quoted=*p=='"';if(quoted)++p;u32 n=0;
    while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){if(n==32767)return false;out[n++]=*p++;}if(quoted){if(*p!='"')return false;++p;}out[n]=0;*command=p;return n!=0;}
bool identify(IDWriteFont* font,u32* id) noexcept {
    IDWriteFontFace* base=nullptr;CHECK(SUCCEEDED(font->CreateFontFace(&base)));IDWriteFontFace5* face=nullptr;
    const HRESULT hr=base->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&face));release(base);CHECK(SUCCEEDED(hr));
    const u32 properties[]{static_cast<u32>(font->GetWeight()),static_cast<u32>(font->GetStretch()),static_cast<u32>(font->GetStyle()),static_cast<u32>(font->GetSimulations())};
    for(u32 i=0;i<face_count;++i)if(faces[i]->Equals(face)){*id=i;release(face);CHECK(bytes_equal(face_properties[i],properties,sizeof(properties)));return true;}
    CHECK(face_count<256);*id=face_count;copy_bytes(face_properties[face_count],properties,sizeof(properties));faces[face_count++]=face;
    IDWriteFontFile* file=nullptr;IDWriteFontFileLoader* loader=nullptr;IDWriteLocalFontFileLoader* local=nullptr;
    struct Cleanup{IDWriteFontFile*& f;IDWriteFontFileLoader*& l;IDWriteLocalFontFileLoader*& p;~Cleanup() noexcept{release(p);release(l);release(f);}} cleanup{file,loader,local};
    u32 count=0;CHECK(SUCCEEDED(face->GetFiles(&count,nullptr))&&count==1);CHECK(SUCCEEDED(face->GetFiles(&count,&file)));
    const void* key=nullptr;u32 size=0,length=0;CHECK(SUCCEEDED(file->GetReferenceKey(&key,&size))&&SUCCEEDED(file->GetLoader(&loader)));
    CHECK(SUCCEEDED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),reinterpret_cast<void**>(&local))));
    CHECK(SUCCEEDED(local->GetFilePathLengthFromKey(key,size,&length))&&length<32768);wchar_t path[32768];CHECK(SUCCEEDED(local->GetFilePathFromKey(key,size,path,32768)));
    text("{\"kind\":\"face\",\"id\":");number(*id);text(",\"path\":");wide(path);text(",\"index\":");number(face->GetIndex());text(",\"properties\":[");number(font->GetWeight());ch(',');number(font->GetStretch());ch(',');number(font->GetStyle());text("],\"simulations\":");number(font->GetSimulations());text(",\"axes\":[");
    DWRITE_FONT_AXIS_VALUE axes[64];count=face->GetFontAxisValueCount();CHECK(count<=64&&SUCCEEDED(face->GetFontAxisValues(axes,count)));
    for(u32 i=0;i<count;++i){if(i)ch(',');ch('[');number(axes[i].axisTag);ch(',');u32 bits=0;copy_bytes(&bits,&axes[i].value,4);number(bits);ch(']');}text("]}\n");return true;
}
bool locked_metadata(IDWriteFactory3* factory,const wchar_t* path) noexcept {
    IDWriteFontCollection* collection=nullptr;CHECK(SUCCEEDED(font_reference::private_collection(factory,path,false,&collection)));
    struct CollectionRelease {IDWriteFontCollection*& p;~CollectionRelease() noexcept {release(p);}} collection_release{collection};
    HANDLE held=CreateFileW(path,GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);const u32 lock_error=held==INVALID_HANDLE_VALUE?GetLastError():0;
    struct Close {HANDLE h;~Close() noexcept {if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}} close{held};
    IDWriteFontFamily* family=nullptr;IDWriteLocalizedStrings* names=nullptr;IDWriteFont* font=nullptr;IDWriteFontFace* face=nullptr;
    struct Release {IDWriteFontFamily*& family;IDWriteLocalizedStrings*& names;IDWriteFont*& font;IDWriteFontFace*& face;
        ~Release() noexcept {release(face);release(font);release(names);release(family);}} cleanup{family,names,font,face};
    const auto family_hr=collection->GetFontFamily(0,&family);
    const auto names_hr=SUCCEEDED(family_hr)?family->GetFamilyNames(&names):family_hr;
    const auto font_hr=SUCCEEDED(family_hr)?family->GetFont(0,&font):family_hr;
    const auto face_hr=SUCCEEDED(font_hr)?font->CreateFontFace(&face):font_hr;
    u16 glyph=0;u32 cp='A';const auto glyph_hr=SUCCEEDED(face_hr)?face->GetGlyphIndices(&cp,1,&glyph):face_hr;
    text("{\"kind\":\"locked_metadata\",\"lock_error\":");number(lock_error);text(",\"family_hr\":");number(family_hr);text(",\"names_hr\":");number(names_hr);
    text(",\"font_hr\":");number(font_hr);text(",\"face_hr\":");number(face_hr);text(",\"glyph_hr\":");number(glyph_hr);text(",\"glyph\":");number(glyph);text("}\n");return true;
}
#include "collection_lifetime.inc"
bool run() noexcept {
    const wchar_t* command=GetCommandLineW();wchar_t name[32768],path[32768];CHECK(argument(&command,path)&&argument(&command,name));const bool custom=argument(&command,path);
    IDWriteFactory3* factory=nullptr;IDWriteFontCollection* collection=nullptr;IDWriteFontFamily* family=nullptr;
    struct Cleanup{IDWriteFactory3*& f;IDWriteFontCollection*& c;IDWriteFontFamily*& g;~Cleanup() noexcept{release(g);release(c);release(f);}} cleanup{factory,collection,family};
    CHECK(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory3),reinterpret_cast<IUnknown**>(&factory))));
    if(custom&&bytes_equal(name,L"--collection-lifetime",44)){wchar_t second[32768];CHECK(argument(&command,second));return collection_lifetime(factory,path,second);}
    if(custom&&bytes_equal(name,L"--locked-metadata",36))return locked_metadata(factory,path);
    if(custom){wchar_t option[32768];const bool all=argument(&command,option);CHECK(!all||font_reference::all_faces_option(option));
        CHECK(SUCCEEDED(font_reference::private_collection(factory,path,all,&collection)));
    }else CHECK(SUCCEEDED(static_cast<IDWriteFactory*>(factory)->GetSystemFontCollection(&collection,FALSE)));
    if(bytes_equal(name,L"--families",22)){
        for(u32 i=0;i<collection->GetFontFamilyCount();++i){IDWriteFontFamily* group=nullptr;IDWriteLocalizedStrings* names=nullptr;CHECK(SUCCEEDED(collection->GetFontFamily(i,&group))&&SUCCEEDED(group->GetFamilyNames(&names)));
            u32 language=0,length=0;BOOL found=FALSE;CHECK(SUCCEEDED(names->FindLocaleName(L"en-us",&language,&found)));if(!found)language=0;
            CHECK(SUCCEEDED(names->GetStringLength(language,&length))&&length<32768&&SUCCEEDED(names->GetString(language,name,32768)));release(names);
            u32 ids[256];const u32 count=group->GetFontCount();CHECK(count<=256);for(u32 j=0;j<count;++j){IDWriteFont* font=nullptr;CHECK(SUCCEEDED(group->GetFont(j,&font)));const bool good=identify(font,ids+j);release(font);CHECK(good);}release(group);
            text("{\"kind\":\"collection_family\",\"name\":");wide(name);text(",\"candidates\":[");for(u32 j=0;j<count;++j){if(j)ch(',');number(ids[j]);}text("]}\n");
        }return true;
    }
    u32 index=0;BOOL exists=FALSE;CHECK(SUCCEEDED(collection->FindFamilyName(name,&index,&exists)));
    if(!exists){text("{\"kind\":\"missing_family\",\"name\":");wide(name);text("}\n");return true;}
    CHECK(SUCCEEDED(collection->GetFontFamily(index,&family)));
    u32 ids[256];const u32 count=family->GetFontCount();CHECK(count<=256);
    for(u32 i=0;i<count;++i){IDWriteFont* font=nullptr;CHECK(SUCCEEDED(family->GetFont(i,&font)));const bool good=identify(font,ids+i);release(font);CHECK(good);}
    text("{\"kind\":\"family\",\"name\":");wide(name);text(",\"candidates\":[");for(u32 i=0;i<count;++i){if(i)ch(',');number(ids[i]);}text("]}\n");
    // Exploratory font-set order requires every reference to have an identity in
    // GetFont's table; some controlled simulation families do not satisfy this.
    wchar_t order_mode[2];if(GetEnvironmentVariableW(L"MAICORD_FONT_REFERENCE_ORDER",order_mode,2)){
        IDWriteFontFamily2* family2=nullptr;IDWriteFontSet1* set=nullptr;CHECK(SUCCEEDED(family->QueryInterface(__uuidof(IDWriteFontFamily2),reinterpret_cast<void**>(&family2))));
        const HRESULT set_hr=family2->GetFontSet(&set);release(family2);CHECK(SUCCEEDED(set_hr));
        text("{\"kind\":\"fontset\",\"candidates\":[");
        for(u32 i=0;i<set->GetFontCount();++i){IDWriteFontFaceReference* ref=nullptr;IDWriteFontFace3* face=nullptr;CHECK(SUCCEEDED(set->GetFontFaceReference(i,&ref)));
            const HRESULT hr=ref->CreateFontFace(&face);release(ref);CHECK(SUCCEEDED(hr));u32 id=0;while(id<face_count&&!faces[id]->Equals(face))++id;release(face);CHECK(id<face_count);if(i)ch(',');number(id);}
        text("]}\n");release(set);
    }
    // Optional QA single-query capture checks whether earlier queries affect order.
    wchar_t single[32]{};const u32 single_units=GetEnvironmentVariableW(L"MAICORD_FONT_REFERENCE_QUERY",single,32);u32 requested[3]{};
    if(single_units){CHECK(single_units<32);u32 at=0;for(u32 i=0;i<3;++i){CHECK(single[at]>='0'&&single[at]<='9');while(single[at]>='0'&&single[at]<='9'){CHECK(requested[i]<=1000);requested[i]=requested[i]*10+single[at++]-'0';}CHECK(single[at]==(i<2?',':0));if(i<2)++at;}}
    const u32 weights[]{100,200,300,350,400,450,500,550,600,650,700,800,900,950};
    if(single_units){bool supported=false;for(u32 weight:weights)if(weight==requested[0])supported=true;CHECK(supported&&requested[1]>=1&&requested[1]<=9&&requested[2]<=2);}
    for(u32 weight:weights)for(u32 stretch=1;stretch<=9;++stretch)for(u32 style=0;style<3;++style){IDWriteFontList* list=nullptr;
        if(single_units&&(weight!=requested[0]||stretch!=requested[1]||style!=requested[2]))continue;
        CHECK(SUCCEEDED(family->GetMatchingFonts(static_cast<DWRITE_FONT_WEIGHT>(weight),static_cast<DWRITE_FONT_STRETCH>(stretch),static_cast<DWRITE_FONT_STYLE>(style),&list)));
        const u32 n=list->GetFontCount();CHECK(n<=256);for(u32 i=0;i<n;++i){IDWriteFont* font=nullptr;CHECK(SUCCEEDED(list->GetFont(i,&font)));const bool good=identify(font,ids+i);release(font);CHECK(good);}release(list);
        IDWriteFont* first=nullptr;CHECK(SUCCEEDED(family->GetFirstMatchingFont(static_cast<DWRITE_FONT_WEIGHT>(weight),static_cast<DWRITE_FONT_STRETCH>(stretch),static_cast<DWRITE_FONT_STYLE>(style),&first)));
        u32 first_id=0;const bool first_ok=identify(first,&first_id);release(first);CHECK(first_ok&&n);
        text("{\"kind\":\"query\",\"request\":[");number(weight);ch(',');number(stretch);ch(',');number(style);text("],\"matches\":[");for(u32 i=0;i<n;++i){if(i)ch(',');number(ids[i]);}ch(']');
        // Record disagreement instead of assuming the two APIs share tie policy.
        if(first_id!=ids[0]){text(",\"first\":");number(first_id);}text("}\n");
    }
    return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_matching_reference_entry() noexcept {
    using namespace maicord;const bool success=run();for(u32 i=0;i<face_count;++i)release(faces[i]);flush();ExitProcess(success&&ok?0:1);
}
