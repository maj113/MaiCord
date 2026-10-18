#include "maicord/text/emoji_run.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
#include "generated/emoji_run_fixture.inc"
#include "emoji_test_support.inc"
bool guards(NativeRunPlanHandle handle,const NativeEmojiPlan& plan) noexcept {
    NativeAsciiResult out{};const auto storage=ascii_buffers.storage();const Utf8View smile=utf8_literal("\xf0\x9f\x98\x80");
    CHECK(native_emoji_run(store,plan,{},17.5f,{},&out)==SfntStatus::ok&&!out.glyph_count&&!out.source_units);
    const u32 bad_bits[]{0,0x80000000u,0x7f800000u,0xff800000u,0x7fc00000u,0xbf800000u};
    for(u32 bits:bad_bits){float em=0;copy_bytes(&em,&bits,4);out={9,9,9,9,9};CHECK(native_emoji_run(store,plan,smile,em,storage,&out)==SfntStatus::invalid_input&&!out.glyph_count&&!out.source_units);}
    CHECK(native_emoji_run(store,plan,{nullptr,1},17.5f,storage,&out)==SfntStatus::invalid_input);
    auto invalid=plan;invalid.substitution_roots={nullptr,1};CHECK(native_emoji_run(store,invalid,smile,17.5f,storage,&out)==SfntStatus::invalid_input);invalid=plan;invalid.positioning_roots={nullptr,1};CHECK(native_emoji_run(store,invalid,smile,17.5f,storage,&out)==SfntStatus::invalid_input);
    CHECK(native_emoji_run(store,plan,utf8_literal("\xf0\x9f"),17.5f,storage,&out)==SfntStatus::invalid_input);
    CHECK(native_emoji_run(store,plan,utf8_literal("\xf0\x9f\x98\x80\xff"),17.5f,storage,&out)==SfntStatus::invalid_input);
    CHECK(native_emoji_run(store,plan,utf8_literal("\n"),17.5f,storage,&out)==SfntStatus::unsupported_sequence);
    CHECK(native_emoji_run(store,plan,utf8_literal("\xf3\xa0\x81\xa7"),17.5f,storage,&out)==SfntStatus::unsupported_sequence);
    CHECK(native_emoji_run(store,plan,utf8_literal("Q"),17.5f,storage,&out,{},true)==SfntStatus::unsupported_sequence);
    CHECK(native_emoji_run(store,plan,utf8_literal("\xe2\x80\x8c"),17.5f,storage,&out,{},true)==SfntStatus::ok);
    auto small=storage;small.mapped.count=0;CHECK(native_emoji_run(store,plan,smile,17.5f,small,&out)==SfntStatus::output_too_small&&!out.glyph_count);
    small=storage;small.shaping.targets.count=0;CHECK(native_emoji_run(store,plan,smile,17.5f,small,&out)==SfntStatus::output_too_small&&!out.source_units);
    small=storage;small.shaping.advances.count=0;CHECK(native_emoji_run(store,plan,smile,17.5f,small,&out)==SfntStatus::output_too_small);
    small=storage;small.character_glyphs.count=1;CHECK(native_emoji_run(store,plan,smile,17.5f,small,&out)==SfntStatus::output_too_small);
    SfntShapeLimits limited{};limited.stage_work=0;CHECK(native_emoji_run(store,plan,smile,17.5f,storage,&out,limited)==SfntStatus::resource_limit);
    limited={};limited.lookup_operations=0;CHECK(native_emoji_run(store,plan,smile,17.5f,storage,&out,limited)==SfntStatus::resource_limit);
    bool shaped=false;const Utf8View joined=utf8_literal("\xf0\x9f\x98\x80\xe2\x80\x8d\xf0\x9f\x98\x80");
    for(u32 budget=0;budget<128;++budget){limited={};limited.stage_work=budget;out={9,9,9,9,9};const auto status=native_emoji_run(store,plan,joined,17.5f,storage,&out,limited);
        if(status==SfntStatus::ok){CHECK(out.source_units==5&&out.glyph_count==3);shaped=true;break;}CHECK(status==SfntStatus::resource_limit&&!out.glyph_count&&!out.source_units);}
    CHECK(shaped);
    NativeEmojiPlan candidate{};const auto before=state.offset,temporary_before=scratch.offset;
    CHECK(native_emoji_plan_prepare(store,handle,&state,&state,&candidate)==SfntStatus::invalid_input&&!candidate.run.slot);
    CHECK(native_emoji_plan_prepare(store,{},&state,&scratch,&candidate)==SfntStatus::invalid_input&&!candidate.run.slot);
    Arena short_state=state;short_state.reserved=short_state.offset;CHECK(native_emoji_plan_prepare(store,handle,&short_state,&scratch,&candidate)==SfntStatus::resource_limit&&!candidate.run.slot&&short_state.offset==before&&scratch.offset==temporary_before);
    Arena short_scratch=scratch;short_scratch.reserved=short_scratch.offset;CHECK(native_emoji_plan_prepare(store,handle,&state,&short_scratch,&candidate)==SfntStatus::output_too_small&&!candidate.run.slot&&state.offset==before&&short_scratch.offset==temporary_before);
    bool reached=false;
    for(u32 work=0;work<256;++work){const auto status=native_emoji_plan_prepare(store,handle,&state,&scratch,&candidate,work);
        CHECK(scratch.offset==temporary_before&&plans[0].references==1);
        if(status==SfntStatus::ok){reached=true;CHECK(arena_rewind(&state,{before}));break;}
        CHECK(status==SfntStatus::resource_limit&&state.offset==before&&!candidate.run.slot);
    }
    CHECK(reached);return true;
}
bool corpus(ConstSlice<u8> bytes,NativeRunPlanKind kind) noexcept {
    CHECK(bytes.count>=12&&bytes_equal(bytes.data,"MCEMOJI3",8));Reader reader{bytes,8};u32 count=0;CHECK(reader.word(&count));
    for(font_index=0;font_index<count;++font_index){u32 embedded=0,size=0,case_count=0,reject=0,weight=0;ConstSlice<u8> font{};
        CHECK(reader.word(&embedded)&&reader.word(&size)&&reader.word(&case_count)&&reader.word(&reject)&&reject<=1&&reader.word(&weight)&&reader.take(size,&font));const wchar_t* path=temporary;
        if(embedded){CHECK(embedded==1&&write_font(font));}
        else{CHECK(size<32768);const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(font.data),static_cast<int>(size),font_path,32767);CHECK(n>0);font_path[n]=0;path=font_path;}
        NativeRunPlanHandle handle{};CHECK(acquire(path,&handle,reject!=0,weight,kind));if(reject){CHECK(!case_count&&!native_font_stats(fonts).files);++rejected_fonts;continue;}arena_reset(&state);CHECK(arena_allocate<u8>(&state,3));const auto mark=state.offset,scratch_mark=scratch.offset;
        NativeEmojiPlan plan{};CHECK(native_emoji_plan_prepare(store,handle,&state,&scratch,&plan)==SfntStatus::ok&&scratch.offset==scratch_mark&&plans[0].references==1);
        const u32 roots=plan.substitution_roots.count+plan.positioning_roots.count;const auto* parent=native_run_plan_get(store,handle);
        const usize root_storage=roots*2u+(roots?1u:0u);
        if(plan.positioning_program==parent->positioning){CHECK(state.offset-mark==root_storage);}
        else{CHECK(state.offset-mark>root_storage&&reinterpret_cast<const u8*>(plan.positioning_program)>=state.base+mark&&reinterpret_cast<const u8*>(plan.positioning_program)+sizeof(SfntGposProgram)<=state.base+state.offset);}
        root_bytes+=state.offset-mark;
        if(kind==NativeRunPlanKind::emoji){NativeAsciiResult rejected{};CHECK(!native_run_plan_nominal(store,handle).glyph_limit);
            CHECK(native_ascii_run(store,handle,utf8_literal("a"),false,17.5f,ascii_buffers.storage(),&rejected)==SfntStatus::invalid_input);}
        if(!font_index&&embedded)CHECK(guards(handle,plan));
        for(case_index=0;case_index<case_count;++case_index){u32 em_bits=0,length=0,n=0,units=0;ConstSlice<u8> text{},glyphs{},placements{},map{},flags{},diacritics{};
            CHECK(reader.word(&em_bits)&&reader.word(&length)&&reader.word(&n)&&reader.word(&units)&&n<=1024&&units<=256&&reader.take(length,&text)&&reader.take(n*2,&glyphs)&&reader.take(n*12,&placements)&&reader.take(units*4,&map)&&reader.take(n,&flags)&&reader.take(n,&diacritics));
            float em=0;copy_bytes(&em,&em_bits,4);
            bool short_proof=false;
            for(u32 mode=0;mode<2+(short_proof?1u:0u);++mode){const auto s=mode==1?ascii_buffers.shared():ascii_buffers.storage();NativeAsciiResult result{};SfntShapeLimits limits{};if(mode==2)limits.stage_work=32;
                EQUAL(static_cast<u32>(native_emoji_run(store,plan,{reinterpret_cast<const char*>(text.data),text.count},em,s,&result,limits)),static_cast<u32>(SfntStatus::ok));EQUAL(result.glyph_count,n);EQUAL(result.source_units,units);
                CHECK(result.nominal_ranges+result.shaped_ranges==1);nominal_runs+=result.nominal_ranges;
                if(!mode)short_proof=result.nominal_ranges&&n<=2&&units<=4&&text.count<=12&&plan.substitution_roots.count<=32&&plan.positioning_roots.count<=32;
                if(mode==2&&result.shaped_ranges)++proof_fallbacks;
                const auto* base=native_run_plan_get(store,handle);CHECK(base);
                for(u32 i=0;i<n;++i){EQUAL(s.glyphs[i],byte_load_le16(glyphs.data+i*2));EQUAL(s.shaping.flags[i],flags[i]);EQUAL(sfnt_gdef_glyph_class(base->gdef,s.glyphs[i])==SfntGlyphClass::mark?1u:0u,diacritics[i]);
                    u32 bits[3];copy_bytes(bits,s.placements.data+i,12);for(u32 k=0;k<3;++k)EQUAL(bits[k],byte_load_le32(placements.data+(i*3+k)*4));}
                for(u32 i=0;i<units;++i)EQUAL(s.character_glyphs[i],byte_load_le32(map.data+i*4));++runs;
            }++cases;
        }
        CHECK(native_run_plan_release(&store,handle)&&!native_font_stats(fonts).files);NativeAsciiResult stale{};
        CHECK(native_emoji_run(store,plan,{},17.5f,{},&stale)==SfntStatus::invalid_input&&!stale.glyph_count);NativeEmojiPlan invalid{};
        CHECK(native_emoji_plan_prepare(store,handle,&state,&scratch,&invalid)==SfntStatus::invalid_input&&!invalid.run.slot);
    }
    CHECK(reader.at==bytes.count&&nominal_runs&&nominal_runs<runs&&proof_fallbacks);return true;
}
bool run() noexcept {
    CHECK(arena_initialize(&scratch,16*1024*1024,0,4096)&&arena_initialize(&state,1024*1024,0,4096));wchar_t folder[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH,folder)&&GetTempFileNameW(folder,L"mce",0,temporary));
    const wchar_t* command=GetCommandLineW();CHECK(argument(&command,font_path,32768));
    if(!argument(&command,font_path,32768))return corpus({emoji_fixture,sizeof(emoji_fixture)},NativeRunPlanKind::text)&&corpus({emoji_fixture,sizeof(emoji_fixture)},NativeRunPlanKind::emoji);
    HANDLE file=CreateFileW(font_path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    const DWORD size=GetFileSize(file,nullptr);CHECK(size<=32*1024*1024);input_file=static_cast<u8*>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));DWORD received=0;
    const bool loaded=input_file&&ReadFile(file,input_file,size,&received,nullptr)&&received==size;CloseHandle(file);CHECK(loaded);return corpus({input_file,size},NativeRunPlanKind::text)&&corpus({input_file,size},NativeRunPlanKind::emoji);
}
}}
extern "C" void maicord_emoji_run_entry() noexcept {
    using namespace maicord;bool ok=run();while(plans[0].references)native_run_plan_release(&store,{1,plans[0].generation});
    while(instances[0].references)native_font_instance_release(&font_instances,{1,instances[0].generation});while(faces[0].references)native_font_release(&fonts,{1,faces[0].generation});
    arena_release(&scratch);arena_release(&state);if(input_file)VirtualFree(input_file,0,MEM_RELEASE);if(temporary[0]&&!DeleteFileW(temporary))ok=false;
    if(GetModuleHandleW(L"dwrite.dll")||GetModuleHandleW(L"DWriteCore.dll"))ok=false;
    print("cases=");number(cases);print("\nruns=");number(runs);print("\nchecks=");number(checks);print("\nvalues=");number(values);print("\ndigest=");number(digest);print("\nroot_bytes_sum=");number(root_bytes);print("\nrejected_fonts=");number(rejected_fonts);print("\nnominal_runs=");number(nominal_runs);print("\nproof_fallbacks=");number(proof_fallbacks);
    print(ok?"\nPASS native emoji frontend\n":"\nFAIL native emoji frontend\n");ExitProcess(ok?0u:1u);
}
