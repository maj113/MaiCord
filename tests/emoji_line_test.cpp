#include "maicord/text/emoji_line.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
#include "emoji_test_support.inc"
#include "generated/emoji_line_fixture.inc"
NativeLineCluster line_clusters[256];
u32 bit_pattern(float value) noexcept {u32 bits=0;copy_bytes(&bits,&value,4);return bits;}
float real(u32 value) noexcept {float result=0;copy_bytes(&result,&value,4);return result;}
bool word_equal(Reader& reader,u32 actual) noexcept {u32 expected=0;CHECK(reader.word(&expected));EQUAL(actual,expected);return true;}
bool line_guards(const NativeEmojiPlan& plan) noexcept {
    const auto smile=utf8_literal("\xf0\x9f\x98\x80");NativeAsciiLineStorage s{ascii_buffers.storage(),{line_clusters,256}};NativeAsciiLineResult out{};
    CHECK(native_emoji_line(store,plan,{},17.5f,s,&out)==SfntStatus::ok&&!out.run.glyph_count&&!out.run.source_units&&out.vertical.height>0);
    NativeLinePosition position{};CHECK(native_ascii_line_position({},out,0,false,&position)==SfntStatus::ok&&!position.source_count&&!position.x);
    CHECK(native_emoji_line(store,plan,smile,0,s,&out)==SfntStatus::invalid_input&&!out.vertical.height);
    auto short_clusters=s;short_clusters.clusters={nullptr,1};CHECK(native_emoji_line(store,plan,smile,17.5f,short_clusters,&out)==SfntStatus::invalid_input&&!out.run.glyph_count);
    short_clusters.clusters.count=0;CHECK(native_emoji_line(store,plan,smile,17.5f,short_clusters,&out)==SfntStatus::output_too_small&&!out.run.glyph_count);
    short_clusters=s;short_clusters.clusters.count=1;CHECK(native_emoji_line(store,plan,utf8_literal("\xf0\x9f\x98\x80\xf0\x9f\x98\x80"),17.5f,short_clusters,&out)==SfntStatus::output_too_small&&!out.run.glyph_count&&!out.cluster_count);
    CHECK(native_emoji_line(store,plan,smile,17.5f,s,nullptr)==SfntStatus::invalid_input);
    CHECK(native_emoji_line(store,plan,{nullptr,1},17.5f,s,&out)==SfntStatus::invalid_input);
    CHECK(native_emoji_line(store,plan,utf8_literal("\xf0\x9f\x98\x80\xff"),17.5f,s,&out)==SfntStatus::invalid_input);
    CHECK(native_emoji_line(store,plan,utf8_literal("\xf3\xa0\x81\xa7"),17.5f,s,&out)==SfntStatus::unsupported_sequence);
    CHECK(native_emoji_line(store,plan,utf8_literal("\n"),17.5f,s,&out)==SfntStatus::unsupported_sequence);
    SfntShapeLimits limits{};limits.stage_work=0;CHECK(native_emoji_line(store,plan,smile,17.5f,s,&out,limits)==SfntStatus::resource_limit&&!out.run.glyph_count);
    const auto missing=utf8_literal("\xf4\x8f\xbf\xbf");CHECK(native_emoji_line(store,plan,missing,17.5f,s,&out)==SfntStatus::ok&&out.run.glyph_count==1&&!s.run.glyphs[0]);
    CHECK(native_emoji_line(store,plan,missing,17.5f,s,&out,{},true)==SfntStatus::unsupported_sequence&&!out.run.glyph_count);
    return true;
}
bool lines_corpus(ConstSlice<u8> data) noexcept {
    CHECK(data.count>=12&&bytes_equal(data.data,"MCEMLIN1",8));Reader reader{data,8};u32 font_count=0;CHECK(reader.word(&font_count));
    for(font_index=0;font_index<font_count;++font_index){u32 embedded=0,size=0,count=0,weight=0;ConstSlice<u8> font{};
        CHECK(reader.word(&embedded)&&reader.word(&size)&&reader.word(&count)&&reader.word(&weight)&&reader.take(size,&font));const wchar_t* path=temporary;
        if(embedded){CHECK(embedded==1&&write_font(font));}
        else{CHECK(size<32768);const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(font.data),static_cast<int>(size),font_path,32767);CHECK(n>0);font_path[n]=0;path=font_path;}
        NativeRunPlanHandle handle{};CHECK(acquire(path,&handle,false,weight));arena_reset(&state);NativeEmojiPlan plan{};
        const auto saved=scratch.offset;CHECK(native_emoji_plan_prepare(store,handle,&state,&scratch,&plan)==SfntStatus::ok&&scratch.offset==saved&&plans[0].references==1);
        root_bytes+=state.offset;if(!font_index)CHECK(line_guards(plan));
        for(case_index=0;case_index<count;++case_index){u32 em_bits=0,bytes=0,glyph_count=0,units=0,points=0,ranges=0;ConstSlice<u8> text{},glyphs{},placements{},map{};
            CHECK(reader.word(&em_bits)&&reader.word(&bytes)&&reader.word(&glyph_count)&&reader.word(&units)&&reader.word(&points)&&reader.word(&ranges));
            CHECK(glyph_count<=1024&&units<=256&&reader.take(bytes,&text)&&reader.take(glyph_count*2,&glyphs)&&reader.take(glyph_count*12,&placements)&&reader.take(units*4,&map));
            const auto geometry=reader.at;u32 next=0;
            for(u32 mode=0;mode<2;++mode){reader.at=geometry;NativeAsciiLineStorage s{mode?ascii_buffers.shared():ascii_buffers.storage(),{line_clusters,256}};NativeAsciiLineResult line{};
                EQUAL(static_cast<u32>(native_emoji_line(store,plan,{reinterpret_cast<const char*>(text.data),bytes},real(em_bits),s,&line)),static_cast<u32>(SfntStatus::ok));
                EQUAL(line.run.glyph_count,glyph_count);EQUAL(line.run.source_units,units);EQUAL(line.geometry_flags,native_line_ordered_x);
                for(u32 i=0;i<glyph_count;++i){EQUAL(s.run.glyphs[i],byte_load_le16(glyphs.data+i*2));u32 bits[3];copy_bytes(bits,s.run.placements.data+i,12);for(u32 k=0;k<3;++k)EQUAL(bits[k],byte_load_le32(placements.data+(i*3+k)*4));}
                for(u32 i=0;i<units;++i)EQUAL(s.run.character_glyphs[i],byte_load_le32(map.data+i*4));
                CHECK(word_equal(reader,bit_pattern(line.width))&&word_equal(reader,bit_pattern(line.width_including_trailing))&&word_equal(reader,bit_pattern(line.vertical.height))&&word_equal(reader,bit_pattern(line.vertical.baseline))&&word_equal(reader,line.trailing_source_units));
                CHECK(word_equal(reader,line.cluster_count));u32 first=0;
                for(u32 i=0;i<line.cluster_count;++i){const auto& cluster=line_clusters[i];CHECK(word_equal(reader,cluster.source_end-first)&&word_equal(reader,bit_pattern(cluster.width)));first=cluster.source_end;}
                for(u32 at=0;at<=units;++at)for(u32 trailing=0;trailing<2;++trailing){NativeLinePosition hit{};CHECK(native_ascii_line_position({line_clusters,line.cluster_count},line,at,trailing!=0,&hit)==SfntStatus::ok);
                    CHECK(word_equal(reader,hit.source_first)&&word_equal(reader,hit.source_count)&&word_equal(reader,bit_pattern(hit.x))&&word_equal(reader,bit_pattern(hit.left))&&word_equal(reader,bit_pattern(hit.width)));}
                // The fixture still records point and selection queries (4 and 7 words); no API answers them.
                ConstSlice<u8> recorded_queries{};CHECK(reader.take(points*16+ranges*28,&recorded_queries));
                if(!mode)next=reader.at;else EQUAL(reader.at,next);++runs;nominal_runs+=line.run.nominal_ranges;
            }++cases;
        }
        CHECK(native_run_plan_release(&store,handle)&&!native_font_stats(fonts).files);NativeAsciiLineResult stale{};
        CHECK(native_emoji_line(store,plan,{},17.5f,{},&stale)==SfntStatus::invalid_input&&!stale.run.glyph_count);
    }
    CHECK(reader.at==data.count&&cases&&runs==cases*2);return true;
}
bool run_lines() noexcept {
    CHECK(arena_initialize(&scratch,16*1024*1024,0,4096)&&arena_initialize(&state,1024*1024,0,4096));wchar_t folder[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH,folder)&&GetTempFileNameW(folder,L"mcl",0,temporary));const wchar_t* command=GetCommandLineW();CHECK(argument(&command,font_path,32768));
    if(!argument(&command,font_path,32768))return lines_corpus({emoji_line_fixture,sizeof(emoji_line_fixture)});
    HANDLE file=CreateFileW(font_path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);const DWORD size=GetFileSize(file,nullptr);CHECK(size<=32*1024*1024);
    input_file=static_cast<u8*>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));DWORD read=0;const bool loaded=input_file&&ReadFile(file,input_file,size,&read,nullptr)&&read==size;CloseHandle(file);CHECK(loaded);return lines_corpus({input_file,size});
}
}}
extern "C" void maicord_emoji_line_entry() noexcept {
    using namespace maicord;bool ok=run_lines();while(plans[0].references)native_run_plan_release(&store,{1,plans[0].generation});
    while(instances[0].references)native_font_instance_release(&font_instances,{1,instances[0].generation});while(faces[0].references)native_font_release(&fonts,{1,faces[0].generation});
    arena_release(&scratch);arena_release(&state);if(input_file)VirtualFree(input_file,0,MEM_RELEASE);if(temporary[0]&&!DeleteFileW(temporary))ok=false;
    if(GetModuleHandleW(L"dwrite.dll")||GetModuleHandleW(L"DWriteCore.dll"))ok=false;
    print("cases=");number(cases);print("\nruns=");number(runs);print("\nchecks=");number(checks);print("\nvalues=");number(values);print("\ndigest=");number(digest);print("\nroot_bytes_sum=");number(root_bytes);print("\nnominal_runs=");number(nominal_runs);
    print(ok?"\nPASS native emoji line geometry\n":"\nFAIL native emoji line geometry\n");ExitProcess(ok?0u:1u);
}
