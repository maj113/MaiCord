#include "gpos_lookup_fixture.hpp"
#include "maicord/core/byte_order.hpp"
#include "text_test_support.hpp"
namespace maicord {namespace {
#include "generated/gpos_attachment_fixture.inc"
void put(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
u64 glyph_queries=0,anchor_checks=0,null_anchors=0,device_samples=0,target_rows=0,budget_checks=0;
volatile u32 observations=0;
struct Reader {ConstSlice<u8> data;u32 at=0;
    bool word(u32* v) noexcept {if(data.count-at<4)return false;*v=byte_load_le32(data.data+at);at+=4;return true;}
    bool half(u16* v) noexcept {if(data.count-at<2)return false;*v=static_cast<u16>(data.data[at]|(static_cast<u16>(data.data[at+1])<<8));at+=2;return true;}
    bool take(u32 n,ConstSlice<u8>* v) noexcept {if(n>data.count-at)return false;*v={data.data+at,n};at+=n;return true;}
};
bool check_device(Reader& r,const SfntDevice& actual) noexcept {
    u16 format=0,start=0,end=0;u32 count=0;if(!r.half(&format))return false;if(!format)return actual.format==0;
    if(!r.half(&start)||!r.half(&end)||!r.word(&count)||format!=actual.format||start!=actual.start_or_outer||end!=actual.end_or_inner)return false;
    if(format==0x8000){u32 index=0;return !count && sfnt_device_variation_index(actual,&index) && index==((static_cast<u32>(start)<<16)|end);}
    if(format>3 || start>end || count!=static_cast<u32>(end)-start+1)return false;
    for(u32 i=0;i<count;++i){u16 expected=0;i16 delta=0;if(!r.half(&expected)||sfnt_device_pixels(actual,static_cast<u16>(start+i),&delta)!=SfntStatus::ok||delta!=static_cast<i16>(expected))return false;++device_samples;}
    return true;
}
bool check_anchor(Reader& r,ConstSlice<u8> raw,const SfntAnchor& actual) noexcept {
    u32 at=0;if(!r.word(&at))return false;++anchor_checks;
    if(at==0xffffffffu){++null_anchors;return !actual.table.data && !actual.table.count;}
    if(at>=raw.count || actual.table.data!=raw.data+at || actual.table.count!=raw.count-at)return false;
    u16 format=0,x=0,y=0,point=0;if(!r.half(&format)||!r.half(&x)||!r.half(&y)||!r.half(&point))return false;
    SfntAnchorValue value{};return sfnt_anchor_read(actual,&value)==SfntStatus::ok && value.format==format && value.x==static_cast<i16>(x) && value.y==static_cast<i16>(y) && value.point==point && check_device(r,value.devices[0]) && check_device(r,value.devices[1]);
}
SfntGposAttachment storage[1024];
bool corpus(ConstSlice<u8> bytes) noexcept {
    if(bytes.count<12 || !bytes_equal(bytes.data,"MCGAT01\0",8))return false;Reader r{bytes,8};u32 fonts=0;if(!r.word(&fonts)||fonts>10000)return false;
    for(u32 f=0;f<fonts;++f){u32 limit=0,size=0,gdsize=0,sc=0;ConstSlice<u8> raw{},gd{};
        if(!r.word(&limit)||!r.word(&size)||!r.word(&gdsize)||!r.word(&sc)||!limit||limit>65536||sc>100000||!r.take(size,&raw)||!r.take(gdsize,&gd))return false;
        SfntLayout layout{};SfntGdef gdef{};if(sfnt_layout_open(raw,SfntLayoutKind::positioning,&layout)!=SfntStatus::ok || (gdsize && sfnt_gdef_open(gd,limit,&gdef)!=SfntStatus::ok))return false;
        u32 previous=0xffffffffu;SfntGposExecutableLookup prepared{};
        for(u32 s=0;s<sc;++s){u32 li=0,si=0,offset=0,type=0,mc=0,tc=0,classes=0;
            if(!r.word(&li)||!r.word(&si)||!r.word(&offset)||!r.word(&type)||!r.word(&mc)||!r.word(&tc)||!r.word(&classes)||li>65535||offset>=size||mc>limit||tc>limit||classes>65535)return false;
            if(li!=previous){const auto status=gpos_prepare_one(layout,static_cast<u16>(li),gdef,limit,{},{storage,1024},&prepared);
                if(status!=SfntStatus::ok || prepared.type<3 || prepared.type>6){print(utf8_literal("prepare_font="));number(f);print(utf8_literal(" lookup="));number(li);print(utf8_literal(" status="));number(static_cast<u32>(status));print(utf8_literal("\n"));return false;}previous=li;}
            if(si>=prepared.tables.attachment.count)return false;const auto& sub=prepared.tables.attachment.data[si];SfntLookup directory{};ConstSlice<u8> subbytes{};
            if(sfnt_layout_lookup(layout,static_cast<u16>(li),&directory)!=SfntStatus::ok || sfnt_lookup_subtable(directory,static_cast<u16>(si),&subbytes)!=SfntStatus::ok || subbytes.data!=raw.data+offset || sub.type!=type || sub.counts[0]!=mc || sub.counts[1]!=tc || sub.class_count!=classes)return false;
            u32 consumed=0;u16 next=0,expected_class=0;if(mc && (!r.half(&next)||!r.half(&expected_class)))return false;
            for(u32 glyph=0;glyph<limit;++glyph){const bool expected=consumed<mc && glyph==next;++glyph_queries;
                if(type==3){SfntGposCursiveAnchors a{};if(sfnt_gpos_cursive_anchors(sub,static_cast<u16>(glyph),&a)!=SfntStatus::ok || a.covered!=expected)return false;
                    if(expected && (!check_anchor(r,raw,a.entry)||!check_anchor(r,raw,a.exit)))return false;
                    if(!expected && (a.entry.table.data||a.exit.table.data))return false;
                }else {SfntGposMarkAnchor a{};if(sfnt_gpos_mark_anchor(sub,static_cast<u16>(glyph),&a)!=SfntStatus::ok || a.covered!=expected)return false;
                    if(expected && (a.mark_class!=expected_class || !check_anchor(r,raw,a.anchor)))return false;
                    if(!expected && (a.anchor.table.data||a.mark_class))return false;}
                if(expected){++consumed;if(consumed<mc){const u16 last=next;if(!r.half(&next)||!r.half(&expected_class)||next<=last)return false;}}
            }
            if(consumed!=mc)return false;if(type==3)continue;consumed=0;u16 components=0;
            if(tc && (!r.half(&next)||!r.half(&components)))return false;
            for(u32 glyph=0;glyph<limit;++glyph){const bool expected=consumed<tc && glyph==next;++glyph_queries;SfntGposTargetAnchors target{};
                if(sfnt_gpos_target_anchors(sub,static_cast<u16>(glyph),&target)!=SfntStatus::ok || target.covered!=expected)return false;
                SfntAnchor a{};if(!expected){if(target.parent.data || sfnt_gpos_target_anchor(target,0,0,&a)!=SfntStatus::invalid_input)return false;continue;}
                if(target.component_count!=components || target.class_count!=classes)return false;++target_rows;
                for(u32 c=0;c<components;++c)for(u32 k=0;k<classes;++k){if(sfnt_gpos_target_anchor(target,static_cast<u16>(c),static_cast<u16>(k),&a)!=SfntStatus::ok || !check_anchor(r,raw,a))return false;}
                if(sfnt_gpos_target_anchor(target,components,0,&a)!=SfntStatus::invalid_input || a.table.data || sfnt_gpos_target_anchor(target,0,static_cast<u16>(classes),&a)!=SfntStatus::invalid_input || a.table.data)return false;
                ++consumed;if(consumed<tc){const u16 last=next;if(!r.half(&next)||!r.half(&components)||next<=last)return false;}
            }
            if(consumed!=tc)return false;
        }
    }return r.at==r.data.count;
}
bool repeated_anchors() noexcept {
    u8 bytes[436]{};put(bytes,0,1);put(bytes,2,12);put(bytes,4,18);put(bytes,6,1);put(bytes,8,28);put(bytes,10,40);
    put(bytes,12,1);put(bytes,14,1);put(bytes,16,0);
    put(bytes,18,2);put(bytes,20,1);put(bytes,22,1);put(bytes,24,130);
    put(bytes,28,1);put(bytes,32,6);put(bytes,34,1);
    put(bytes,40,130);put(bytes,302,1);put(bytes,304,11);put(bytes,430,1);put(bytes,432,22);
    for(u32 i=0;i<130;++i)put(bytes,42+i*2,i%4==3?0:i%4==1?390:262);
    SfntGposAttachment sub{};u32 used=0;
    if(sfnt_gpos_attachment_open({bytes,sizeof(bytes)},4,131,&sub,10000,&used)!=SfntStatus::ok)return false;
    for(u16 i=1;i<=130;++i){SfntGposTargetAnchors target{};SfntAnchor anchor{};SfntAnchorValue value{};
        if(sfnt_gpos_target_anchors(sub,i,&target)!=SfntStatus::ok||!target.covered||sfnt_gpos_target_anchor(target,0,0,&anchor)!=SfntStatus::ok)return false;
        if((i-1)%4==3){if(anchor.table.data)return false;}
        else if(sfnt_anchor_read(anchor,&value)!=SfntStatus::ok||value.x!=((i-1)%4==1?22:11))return false;
    }
    for(u32 budget=0;budget<=used;++budget){u32 charged=0;const auto status=sfnt_gpos_attachment_open({bytes,sizeof(bytes)},4,131,&sub,budget,&charged);
        if(charged>budget||(budget<used?status!=SfntStatus::resource_limit:status!=SfntStatus::ok))return false;++budget_checks;}
    const auto target_status=[&](u16 glyph,u32 size) noexcept {
        SfntGposTargetAnchors target{};SfntAnchor anchor{};
        if(sfnt_gpos_attachment_open({bytes,size},4,131,&sub)!=SfntStatus::ok ||
           sfnt_gpos_target_anchors(sub,glyph,&target)!=SfntStatus::ok)return SfntStatus::invalid_input;
        const auto status=sfnt_gpos_target_anchor(target,0,0,&anchor);
        if(status!=SfntStatus::ok&&(anchor.table.data||anchor.table.count))return SfntStatus::invalid_input;
        return status;
    };
    put(bytes,430,4);if(target_status(2,sizeof(bytes))!=SfntStatus::unsupported || target_status(1,sizeof(bytes))!=SfntStatus::ok)return false;
    put(bytes,430,1);if(target_status(2,sizeof(bytes)-1)!=SfntStatus::truncated)return false;
    put(bytes,42,260);if(target_status(1,sizeof(bytes))!=SfntStatus::malformed)return false;
    put(bytes,42,262);put(bytes,430,2);if(target_status(2,sizeof(bytes))!=SfntStatus::truncated)return false;
    put(bytes,430,1);put(bytes,302,3);put(bytes,308,10);put(bytes,312,16);put(bytes,314,18);put(bytes,316,3);
    if(target_status(1,sizeof(bytes))!=SfntStatus::ok)return false;
    put(bytes,316,4);if(target_status(1,sizeof(bytes))!=SfntStatus::unsupported || target_status(2,sizeof(bytes))!=SfntStatus::ok)return false;
    put(bytes,316,3);put(bytes,308,8);if(target_status(1,sizeof(bytes))!=SfntStatus::malformed)return false;
    put(bytes,308,134);if(target_status(1,sizeof(bytes))!=SfntStatus::truncated)return false;

    return true;
}
bool units_guards() noexcept {
    if(!repeated_anchors())return false;
    const u32 size=byte_load_le32(attachment_fixture+16);const u8* raw=attachment_fixture+28;SfntLayout layout{};
    if(sfnt_layout_open({raw,size},SfntLayoutKind::positioning,&layout)!=SfntStatus::ok)return false;
    ConstSlice<u8> originals[4];u16 types[4];
    for(u16 i=0;i<4;++i){SfntLookup lookup{};if(sfnt_layout_lookup(layout,i,&lookup)!=SfntStatus::ok || sfnt_lookup_subtable(lookup,0,&originals[i])!=SfntStatus::ok)return false;types[i]=lookup.type;
        SfntGposAttachment sub{};u32 used=0;if(sfnt_gpos_attachment_open(originals[i],types[i],5,&sub,10000,&used)!=SfntStatus::ok || !used)return false;
        for(u32 budget=0;budget<=used;++budget){u32 reported=0;sub.type=255;const auto status=sfnt_gpos_attachment_open(originals[i],types[i],5,&sub,budget,&reported);
            if(reported>budget || (budget<used?(status!=SfntStatus::resource_limit || sub.type!=0):status!=SfntStatus::ok))return false;++budget_checks;}
        SfntGposCursiveAnchors cursive{};SfntGposMarkAnchor mark{};SfntGposTargetAnchors target{};
        if(i==0){if(sfnt_gpos_cursive_anchors(sub,5,&cursive)!=SfntStatus::glyph_out_of_range || sfnt_gpos_mark_anchor(sub,1,&mark)!=SfntStatus::invalid_input || sfnt_gpos_target_anchors(sub,1,&target)!=SfntStatus::invalid_input)return false;}
        else if(sfnt_gpos_mark_anchor(sub,5,&mark)!=SfntStatus::glyph_out_of_range || sfnt_gpos_target_anchors(sub,5,&target)!=SfntStatus::glyph_out_of_range || sfnt_gpos_cursive_anchors(sub,1,&cursive)!=SfntStatus::invalid_input)return false;
        if(i){u8 changed[4096];if(originals[i].count>sizeof(changed))return false;const u32 mark_at=byte_load_be16(originals[i].data+8);
            for(u32 test=0;test<3;++test){copy_bytes(changed,originals[i].data,originals[i].count);
                if(test==0)put(changed,mark_at+2,3);else if(test==1)put(changed,mark_at+4,0);else put(changed,mark_at,3);
                if(sfnt_gpos_attachment_open({changed,originals[i].count},types[i],5,&sub)!=SfntStatus::malformed || sub.type)return false;}
            if(i==2){const u32 array=byte_load_be16(originals[i].data+10),lig=array+byte_load_be16(originals[i].data+array+2);copy_bytes(changed,originals[i].data,originals[i].count);put(changed,lig,65535);
                if(sfnt_gpos_attachment_open({changed,originals[i].count},5,5,&sub)!=SfntStatus::truncated || sub.type)return false;}}
    }
    SfntGposAttachment pair_storage[2];SfntGposExecutableLookup pair{};
    if(gpos_prepare_one(layout,8,{},5,{},{pair_storage,2},&pair)!=SfntStatus::ok || pair.tables.attachment.count!=2 || pair.filter.flags!=1)return false;
    if(gpos_prepare_one(layout,8,{},5,{},{pair_storage,1},&pair)!=SfntStatus::output_too_small || pair.type)return false;
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* mem=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!mem)return false;DWORD old=0;if(!VirtualProtect(mem+page,page,PAGE_NOACCESS,&old)){VirtualFree(mem,0,MEM_RELEASE);return false;}
    u32 random=0x879bad32;bool ok=true;
    for(u32 i=0;i<10000;++i){const auto original=originals[i%4];if(original.count>page){ok=false;break;}random^=random<<13;random^=random>>17;random^=random<<5;const u32 length=i<4?original.count:random%(original.count+1);u8* data=mem+page-length;copy_bytes(data,original.data,length);if(i>=4 && length)data[(random>>8)%length]^=static_cast<u8>(random>>16);
        SfntGposAttachment sub{};auto status=sfnt_gpos_attachment_open({data,length},types[i%4],5,&sub,1000);observations=observations+static_cast<u32>(status)+1;
        if(status!=SfntStatus::ok)continue;
        for(u16 glyph=0;glyph<5;++glyph){if(sub.type==3){SfntGposCursiveAnchors a{};status=sfnt_gpos_cursive_anchors(sub,glyph,&a);observations=observations+static_cast<u32>(status)+a.covered;
                for(u32 k=0;k<2;++k){const auto anchor=k?a.exit:a.entry;if(anchor.table.data){SfntAnchorValue v{};status=sfnt_anchor_read(anchor,&v);observations=observations+static_cast<u32>(status)+static_cast<u16>(v.x);}}}
            else {SfntGposMarkAnchor mark{};status=sfnt_gpos_mark_anchor(sub,glyph,&mark);observations=observations+static_cast<u32>(status)+mark.mark_class;
                if(mark.anchor.table.data){SfntAnchorValue v{};status=sfnt_anchor_read(mark.anchor,&v);observations=observations+static_cast<u32>(status)+static_cast<u16>(v.y);}
                SfntGposTargetAnchors target{};status=sfnt_gpos_target_anchors(sub,glyph,&target);observations=observations+static_cast<u32>(status)+target.component_count;
                if(target.covered)for(u32 c=0;c<target.component_count;++c)for(u32 k=0;k<target.class_count;++k){SfntAnchor a{};status=sfnt_gpos_target_anchor(target,static_cast<u16>(c),static_cast<u16>(k),&a);observations=observations+static_cast<u32>(status);
                    if(a.table.data){SfntAnchorValue v{};status=sfnt_anchor_read(a,&v);observations=observations+static_cast<u32>(status)+static_cast<u16>(v.x);}}}
        }
    }
    VirtualFree(mem,0,MEM_RELEASE);return ok;
}
bool file_corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size) || size.QuadPart<12 || size.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE map=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!map){CloseHandle(file);return false;}const auto* data=static_cast<const u8*>(MapViewOfFile(map,FILE_MAP_READ,0,0,0));const bool ok=data && corpus({data,static_cast<u32>(size.QuadPart)});if(data)UnmapViewOfFile(data);CloseHandle(map);CloseHandle(file);return ok;
}
int run() noexcept {
    if(!units_guards()){print(utf8_literal("FAIL attachment units/guards\n"));return 3;}
    const wchar_t* command=test_arguments();
    bool ok=false;if(!*command)ok=corpus({attachment_fixture,sizeof(attachment_fixture)});
    else {wchar_t path[32768];if(!test_path_argument(command,path))return 2;ok=file_corpus(path);}
    print(utf8_literal("glyph_queries="));number(glyph_queries);print(utf8_literal("\nanchor_checks="));number(anchor_checks);print(utf8_literal("\nnull_anchors="));number(null_anchors);print(utf8_literal("\ndevice_samples="));number(device_samples);print(utf8_literal("\ntarget_rows="));number(target_rows);print(utf8_literal("\nbudget_checks="));number(budget_checks);print(utf8_literal("\nguard_observations="));number(observations);print(utf8_literal("\n"));print(ok?utf8_literal("PASS native GPOS attachment data\n"):utf8_literal("FAIL native GPOS attachment data\n"));return ok?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_gpos_attachment_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
