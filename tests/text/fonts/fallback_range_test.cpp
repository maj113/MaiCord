#include "maicord/text/fonts/fallback_range.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "../../support/test_output.hpp"
namespace maicord {namespace {
u32 checks=0;
constexpr auto& print=test_write_text;constexpr auto& number=test_write_u64;
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
struct Mock {u32 fail=0;bool cover=true;};
SfntStatus select(void* context,u32 cp,NativeFallbackChoice* out) noexcept {auto& m=*static_cast<Mock*>(context);if(m.fail==1)return SfntStatus::malformed;*out={cp=='b'?1u:cp=='?'?native_fallback_missing:0u,cp=='x'?1u:0u};return SfntStatus::ok;}
SfntStatus coverage(void* context,u32,u32,bool* out) noexcept {auto& m=*static_cast<Mock*>(context);if(m.fail==2)return SfntStatus::truncated;*out=m.cover;return SfntStatus::ok;}
bool empty(const NativeFallbackRangeResult& r) noexcept {return !r.bytes&&!r.units&&r.choice.font==native_fallback_missing&&r.choice.policy==native_fallback_missing;}
struct RoutingMock {Mock font;bool fail_route=false;};
SfntStatus route_index(void* context,u32 cp,u32* out) noexcept {auto& m=*static_cast<RoutingMock*>(context);if(m.fail_route)return SfntStatus::truncated;*out=cp=='x'?1u:0u;return SfntStatus::ok;}
SfntStatus font_index(void* context,u32 cp,u32* out) noexcept {NativeFallbackChoice choice{};const auto status=select(&static_cast<RoutingMock*>(context)->font,cp,&choice);*out=choice.font;return status;}
SfntStatus routed_coverage(void* context,u32 font,u32 cp,bool* out) noexcept {return coverage(&static_cast<RoutingMock*>(context)->font,font,cp,out);}
bool routing_guards() noexcept {
    struct Case {const char* text;u32 bytes,units,font;};
    const Case cases[]{ {"a\xee\x80\x80",1,1,0},{"\xee\x80\x80" "a",3,1,0},
        {"a\xef\xb8\x8f" "a",5,3,0},{"a\xef\xb8\x8f\xef\xb8\x8f" "a",8,4,0},
        {"a\xcd\x8f" "x",4,3,0},{"a\xcd\x8f\xef\xb8\x8f" "x",7,4,0},
        {"b\xcd\x8f\xef\xb8\x8f" "a",6,3,1},{"b\xcd\x8f" "a",4,3,1},
        {"a\xcd\x8f\xef\xb8\x8f\xef\xb8\x8f" "x",9,4,0},
        {"a\xf3\xa0\x84\x80" "x",5,3,0},{"?\xef\xb8\x8f\xef\xb8\x8f" "a",1,1,native_fallback_missing},
        {"\xef\xb8\x8f\xe1\xa0\x8b",6,2,0} };
    NativeFallbackRangeResult result{};
    for(const auto& c:cases){const u32 n=c_string_length(c.text);RoutingMock mock{};
        CHECK(native_font_fallback_routed_range({c.text,n},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::ok);
        CHECK(result.bytes==c.bytes&&result.units==c.units&&result.choice.font==c.font&&!result.choice.policy);
    }
    RoutingMock mock{};CHECK(native_font_fallback_routed_range({},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::ok&&empty(result));
    mock.fail_route=true;CHECK(native_font_fallback_routed_range({"a",1},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::truncated);
    mock={};mock.font.fail=1;CHECK(native_font_fallback_routed_range({"a",1},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::malformed);
    mock={};mock.font.fail=2;CHECK(native_font_fallback_routed_range({"a\xcd\x8f",3},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::truncated);
    mock={};mock.font.cover=false;CHECK(native_font_fallback_routed_range({"b\xcd\x8f",3},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::ok&&result.bytes==1);
    const char invalid[]{'a',static_cast<char>(0xc0),static_cast<char>(0xaf)};mock={};CHECK(native_font_fallback_routed_range({invalid,3},route_index,font_index,routed_coverage,&mock,&result)==SfntStatus::invalid_input);return true;
}
#include "fallback_map_cases.inc"
bool guards() noexcept {
    CHECK(routing_guards()&&mapping_guards());
    struct Case {const char* text;u32 bytes,units,font;};
    const Case cases[]{ {"aba",1,1,0},{"axa",1,1,0},{"b\xcc\x81" "a",3,2,1},
        {"b\xe2\x80\x8d" "a",5,3,1},{"b\xe2\x80\x8c" "a",4,2,1},
        {"b\xef\xb8\x8f" "a",4,2,1},{"?\xef\xb8\x8f" "a",1,1,native_fallback_missing},
        {"b\xe2\x80\x8d\xcc\x81" "a",6,3,1},{"aaaa",4,4,0},
        {"b\xf0\x93\x90\xb0" "a",1,1,1},{"\xf0\x93\x90\xb0" "b",5,3,0} };
    NativeFallbackRangeResult result{};
    for(const auto& c:cases){const u32 n=c_string_length(c.text);Mock mock{};
        CHECK(native_font_fallback_range({c.text,n},select,coverage,&mock,&result)==SfntStatus::ok);
        CHECK(result.bytes==c.bytes&&result.units==c.units&&result.choice.font==c.font&&!result.choice.policy);
    }
    Mock mock{};CHECK(native_font_fallback_range({},select,coverage,&mock,&result)==SfntStatus::ok&&empty(result));
    mock.fail=1;CHECK(native_font_fallback_range({"a",1},select,coverage,&mock,&result)==SfntStatus::malformed);
    mock.fail=2;CHECK(native_font_fallback_range({"b\xcc\x81",3},select,coverage,&mock,&result)==SfntStatus::truncated);
    mock={0,false};CHECK(native_font_fallback_range({"b\xcc\x81",3},select,coverage,&mock,&result)==SfntStatus::ok&&result.bytes==1&&result.choice.font==1);
    const char invalid[]{'a',static_cast<char>(0xc0),static_cast<char>(0xaf)};mock={};CHECK(native_font_fallback_range({invalid,3},select,coverage,&mock,&result)==SfntStatus::invalid_input);
    Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;auto* memory=static_cast<u8*>(pages_allocate(page*4));CHECK(memory);
    struct Cleanup {u8* p;~Cleanup() noexcept{pages_release(p);}} cleanup{memory};
    char* text=reinterpret_cast<char*>(memory+page-5);copy_bytes(text,"b\xe2\x80\x8d" "a",5);auto* guarded=reinterpret_cast<NativeFallbackRangeResult*>(memory+page*3)-1;
    auto* canary=reinterpret_cast<u32*>(guarded)-1;*canary=0x197ac05d;
    CHECK(pages_protect(memory, page, PageAccess::read)&&pages_protect(memory+page, page, PageAccess::none)&&pages_protect(memory+page*3, page, PageAccess::none));
    mock={};CHECK(native_font_fallback_range({text,5},select,coverage,&mock,guarded)==SfntStatus::ok&&guarded->bytes==5&&*canary==0x197ac05d);
    RoutingMock routed{};CHECK(native_font_fallback_routed_range({text,5},route_index,font_index,routed_coverage,&routed,guarded)==SfntStatus::ok&&guarded->bytes==5&&*canary==0x197ac05d);
    MapMock mapped{};CHECK(native_font_fallback_map({text,5},map_base,map_route,map_fallback,map_cover,&mapped,guarded)==SfntStatus::ok&&guarded->bytes==4&&*canary==0x197ac05d);
    return true;
}
}}
MAICORD_TEST_ENTRY(maicord_font_fallback_range_entry) {using namespace maicord;const bool ok=guards();
    print(ok?"PASS native fallback ranges checks=":"FAIL native fallback ranges checks=");number(checks);print("\n");::maicord::test_process_exit(ok?0:1);}
