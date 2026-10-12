#include "maicord/text/tt_interpreter.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
#include "generated/tt_interpreter_fixture.inc"
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
bool fail(u32 item,u32 code) noexcept {print(utf8_literal("FAIL item="));number(item);print(utf8_literal(" code="));number(code);print(utf8_literal("\n"));return false;}
struct Reader {ConstSlice<u8> bytes;u32 at=0;bool word(u32* v) noexcept {if(bytes.count-at<4)return false;*v=byte_load_le32(bytes.data+at);at+=4;return true;}bool take(u32 n,ConstSlice<u8>* v) noexcept {if(n>bytes.count-at)return false;*v={bytes.data+at,n};at+=n;return true;}};
struct Fixture {u32 upem,point_count,contours,cvt_count;ConstSlice<u8> points,ends,cvt,code[3],expected;};
i32 stack[1024],storage[128],cvt[2048];TtFunction functions[256];TtCallFrame frames[32];TtPoint points[256],twilight[64];u32 ends[32];
u64 cases=0,coordinates=0,budgets=0,guards=0,invalid=0,controls=0,push_cases=0,skip_cases=0;
TtInterpreter fresh() noexcept {
    clear_bytes(stack,sizeof(stack));clear_bytes(storage,sizeof(storage));clear_bytes(cvt,sizeof(cvt));clear_bytes(functions,sizeof(functions));clear_bytes(frames,sizeof(frames));clear_bytes(points,sizeof(points));clear_bytes(twilight,sizeof(twilight));
    TtInterpreter s{};s.stack={stack,1024};s.storage={storage,128};s.cvt={cvt,2048};s.functions={functions,256};s.frames={frames,32};s.glyph={points,256};s.twilight={twilight,64};s.units_per_em=1000;return s;
}
TtInterpreter setup(const Fixture& f) noexcept {
    auto s=fresh();s.units_per_em=f.upem;s.glyph.count=f.point_count;s.cvt.count=f.cvt_count;s.contour_ends={ends,f.contours};
    for(u32 i=0;i<f.point_count;++i){const i32 x=static_cast<i32>(byte_load_le32(f.points.data+i*8)),y=static_cast<i32>(byte_load_le32(f.points.data+i*8+4));points[i]={x,y,x,y,0,1,0};}
    for(u32 i=0;i<f.contours;++i)ends[i]=byte_load_le32(f.ends.data+i*4);
    for(u32 i=0;i<f.cvt_count;++i)cvt[i]=static_cast<i32>(byte_load_le32(f.cvt.data+i*4));return s;
}
// Fixture prep programs deliberately use only the profile settings exercised by
// these observations. General font/prep/glyph state lifetime is not integrated.
SfntStatus execute(TtInterpreter& s,const Fixture& f,u32 through,u32 work,TtExecution* result) noexcept {
    for(u32 phase=0;phase<=through;++phase){s.stack_count=0;const auto status=tt_execute(&s,f.code[phase],static_cast<TtPhase>(phase),result,phase==through?work:1000000);if(status!=SfntStatus::ok)return status;}return SfntStatus::ok;
}
#pragma float_control(precise,on,push)
bool match(const Fixture& f,const TtInterpreter& s) noexcept {
    const float scale=1.f/static_cast<float>(f.upem);
    for(u32 i=0;i<f.point_count;++i){const auto p=s.glyph[i];const float x=(p.x/64.f)*scale,y=p.y==0?0.f:-(p.y/64.f)*scale;
        if(!bytes_equal(&x,f.expected.data+i*8,4)||!bytes_equal(&y,f.expected.data+i*8+4,4))return false;coordinates+=2;}return true;
}
#pragma float_control(pop)
bool guarded(ConstSlice<u8> program,TtPhase phase) noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;if(program.count>page)return false;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;DWORD old=0;if(!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}
    u32 random=0x8265721;for(u32 n=0;n<program.count+1024;++n){const u32 size=n<program.count?n:program.count;auto* p=memory+page-size;if(size)copy_bytes(p,program.data,size);
        if(n>=program.count && size){random^=random<<13;random^=random>>17;random^=random<<5;p[random%size]^=static_cast<u8>((random>>16)|1);}
        auto s=fresh();TtExecution r{};tt_execute(&s,{p,size},phase,&r,1024);++guards;if(r.operations>1024 || s.stack_count>s.stack.count){VirtualFree(memory,0,MEM_RELEASE);return false;}
    }VirtualFree(memory,0,MEM_RELEASE);return true;
}
bool corpus() noexcept {
    Reader reader{{tt_interpreter_fixture,sizeof(tt_interpreter_fixture)},8};u32 count=0;if(!bytes_equal(reader.bytes.data,"MCTTVM1\0",8)||!reader.word(&count))return false;
    for(u32 c=0;c<count;++c){Fixture f{};if(!reader.word(&f.upem)||!reader.word(&f.point_count)||!reader.word(&f.contours)||!reader.word(&f.cvt_count)||f.point_count>256||f.contours>32||f.cvt_count>2048||!reader.take(f.point_count*8,&f.points)||!reader.take(f.contours*4,&f.ends)||!reader.take(f.cvt_count*4,&f.cvt))return fail(c,1);
        for(auto& code:f.code){u32 size=0;if(!reader.word(&size)||!reader.take(size,&code))return fail(c,2);}if(!reader.take(f.point_count*8,&f.expected))return fail(c,3);
        auto s=setup(f);TtExecution r{};if(execute(s,f,2,1000000,&r)!=SfntStatus::ok || !match(f,s))return fail(c,4);++cases;
        for(u32 phase=0;phase<3;++phase){s=setup(f);if(execute(s,f,phase,1000000,&r)!=SfntStatus::ok)return fail(c,5);const u32 full=r.operations;
            for(u32 work=0;work<=full;++work){s=setup(f);const auto status=execute(s,f,phase,work,&r);++budgets;if(r.operations>work || status!=(work<full?SfntStatus::resource_limit:SfntStatus::ok))return fail(c,6);}
            if(!guarded(f.code[phase],static_cast<TtPhase>(phase)))return fail(c,7);
        }
    }return reader.at==reader.bytes.count;
}
bool control_flow() noexcept {
    // Function 0 adds one. Function 1 invokes it twice. LOOPCALL invokes the
    // nested function three times, exercising explicit return/loop frames.
    const u8 definitions[]{0xb0,0,0x2c,0xb0,1,0x60,0x2d,0xb0,1,0x2c,0xb1,2,0,0x2a,0x2d};
    const u8 calls[]{0xb2,10,3,1,0x2a};auto s=fresh();TtExecution r{};
    if(tt_execute(&s,{definitions,sizeof(definitions)},TtPhase::font,&r)!=SfntStatus::ok || tt_execute(&s,{calls,sizeof(calls)},TtPhase::glyph,&r)!=SfntStatus::ok || s.stack_count!=1 || stack[0]!=16)return fail(0,10);++controls;
    const u8 conditional[]{0xb0,0,0x58,0xb1,0x59,0x1b,0x58,0xb0,99,0x59,0x1b,0xb0,7,0x59};
    s=fresh();if(tt_execute(&s,{conditional,sizeof(conditional)},TtPhase::glyph,&r)!=SfntStatus::ok || s.stack_count!=1 || stack[0]!=7)return fail(0,11);++controls;
    const u8 recursive[]{0xb0,0,0x2c,0xb0,0,0x2b,0x2d},call[]{0xb0,0,0x2b};
    s=fresh();if(tt_execute(&s,{recursive,sizeof(recursive)},TtPhase::font,&r)!=SfntStatus::ok || tt_execute(&s,{call,sizeof(call)},TtPhase::glyph,&r)!=SfntStatus::resource_limit || r.call_depth!=32)return fail(0,12);++controls;
    const u8 jump[]{0xb8,0xff,0xfd,0x1c};s=fresh();if(tt_execute(&s,{jump,sizeof(jump)},TtPhase::glyph,&r,64)!=SfntStatus::resource_limit || r.operations!=64)return fail(0,13);++controls;
    const u8 missing[]{0xb0,89,0x2b},underflow_call[]{0x2b},negative_call[]{0xb8,0xff,0xff,0x2b};
    s=fresh();if(tt_execute(&s,{missing,sizeof(missing)},TtPhase::prep,&r)!=SfntStatus::malformed||!r.missing_function)return fail(0,16);++controls;
    s=fresh();if(tt_execute(&s,{underflow_call,1},TtPhase::prep,&r)!=SfntStatus::malformed||r.missing_function)return fail(0,17);++controls;
    s=fresh();if(tt_execute(&s,{negative_call,sizeof(negative_call)},TtPhase::prep,&r)!=SfntStatus::malformed||r.missing_function)return fail(0,18);++controls;
    s=fresh();if(tt_execute(&s,{missing,sizeof(missing)},TtPhase::prep,&r,2)!=SfntStatus::resource_limit||r.missing_function)return fail(0,19);++controls;
    // Full-range IUP interpolation previously required a potentially overflowing
    // signed product. The middle original coordinate must resolve to zero.
    const u8 iup[]{0x31};s=fresh();s.glyph.count=3;ends[0]=2;s.contour_ends={ends,1};points[0]={2147483647,0,-2147483647,0,1,1,0};points[1]={0,0,0,0,0,1,0};points[2]={-2147483647,0,2147483647,0,1,1,0};
    // Reversed spans add a positive half before signed division toward zero;
    // the resulting midpoint is one F26Dot6 unit above mathematical zero.
    if(tt_execute(&s,{iup,1},TtPhase::glyph,&r)!=SfntStatus::ok || points[1].x!=1)return fail(0,14);++controls;
    return true;
}
bool repeated_branches() noexcept {
    const u8 definitions[]{
        0xb0,0,0x2c,0xb0,0,0x58,0x40,4,0x59,0x1b,0x58,0x2d,0xb0,0,0x58,0xb0,21,0x59,0x1b,0xb0,1,0x60,0x59,0x2d,
        0xb0,1,0x2c,0xb0,1,0x58,0xb0,1,0x60,0x1b,0x40,4,0x59,0x1b,0x58,0x2d,0xb0,0,0x58,0xb0,21,0x59,0x59,0x2d,
        0xb0,2,0x2c,0xb0,0,0x2b,0xb0,1,0x2b,0x2d};
    const u8 calls[]{0xb2,0,128,2,0x2a};TtExecution r{};
    for(u32 work=0;work<=4485;++work){auto s=fresh();
        if(tt_execute(&s,{definitions,sizeof(definitions)},TtPhase::font,&r)!=SfntStatus::ok)return fail(work,30);
        const auto status=tt_execute(&s,{calls,sizeof(calls)},TtPhase::prep,&r,work);++budgets;
        if(status!=(work<4485?SfntStatus::resource_limit:SfntStatus::ok)||r.operations>work)return fail(work,31);
        if(work==4485&&(r.operations!=4485||s.stack_count!=1||stack[0]!=256))return fail(work,32);
    }
    const u8 body[]{0xb0,0,0x58,0xb0,99,0x59},shorter[]{0xb0,0,0x2b,0xb0,1,0x2b};auto s=fresh();
    s.functions[0]={{body,sizeof(body)},true};s.functions[1]={{body,sizeof(body)-1},true};
    if(tt_execute(&s,{shorter,sizeof(shorter)},TtPhase::prep,&r)!=SfntStatus::malformed)return fail(0,33);
    controls+=2;return true;
}
bool skipped_pushes() noexcept {
    u8 code[520]{};
    for(u32 definition=0;definition<2;++definition)for(u32 mode=0;mode<4;++mode){const u32 width=(mode&1)+1,header=mode<2?2:1;
        for(u32 n=mode<2?0:1;n<=(mode<2?255u:8u);++n){
            code[0]=0xb0;code[1]=0;code[2]=definition?0x2c:0x58;
            code[3]=static_cast<u8>(mode<2?0x40+(width-1):(width==1?0xb0:0xb8)+n-1);
            if(header==2)code[4]=static_cast<u8>(n);
            for(u32 i=0;i<n*width;++i)code[3+header+i]=static_cast<u8>(i%2?0x2d:0x59);
            const u32 end=3+header+n*width;code[end]=definition?0x2d:0x59;code[end+1]=0xb0;code[end+2]=7;
            for(u32 work=0;work<=7;++work){auto s=fresh();TtExecution r{};
                const auto status=tt_execute(&s,{code,end+3},TtPhase::font,&r,work);++budgets;
                if(status!=(work<7?SfntStatus::resource_limit:SfntStatus::ok)||r.operations>work)return fail(n,50);
                if(work==7&&(s.stack_count!=1||stack[0]!=7||(definition&&(!functions[0].defined||functions[0].code.count!=header+n*width))))return fail(n,51);
            }
            for(u32 cut=0;cut<=header+n*width;++cut){auto s=fresh();TtExecution r{};
                const auto expected=cut==0||cut==header+n*width?SfntStatus::malformed:SfntStatus::truncated;
                if(tt_execute(&s,{code,3+cut},TtPhase::font,&r)!=expected)return fail(cut,52);
            }
            if((n<=1||n==8||n==255)&&!guarded({code,end+3},TtPhase::font))return fail(n,53);
            ++skip_cases;
        }
    }return true;
}
bool push_batches() noexcept {
    u8 code[512+2]{};
    for(u32 mode=0;mode<4;++mode){const u32 width=(mode&1)+1,header=mode<2?2:1;
        for(u32 n=mode<2?0:1;n<=(mode<2?255u:8u);++n){
            code[0]=static_cast<u8>(mode<2?0x40+(width-1):(width==1?0xb0:0xb8)+n-1);
            if(header==2)code[1]=static_cast<u8>(n);
            for(u32 j=0;j<n;++j){const u16 value=static_cast<u16>(0x8000u+j*257);
                if(width==1)code[header+j]=static_cast<u8>(value);
                else{code[header+j*2]=static_cast<u8>(value>>8);code[header+j*2+1]=static_cast<u8>(value);}}
            auto s=fresh();s.stack_count=3;s.stack.count=n+3;stack[n+3]=0x12345678;TtExecution r{};
            const ConstSlice<u8> program{code,header+n*width};
            if(tt_execute(&s,program,TtPhase::font,&r,n+1)!=SfntStatus::ok||r.operations!=n+1||s.stack_count!=n+3||stack[n+3]!=0x12345678)return fail(n,40);
            for(u32 j=0;j<n;++j){const u16 value=static_cast<u16>(0x8000u+j*257);
                if(stack[3+j]!=(width==1?static_cast<i32>(static_cast<u8>(value)):static_cast<i32>(static_cast<i16>(value))))return fail(n,41);}
            s.stack_count=3;if(tt_execute(&s,program,TtPhase::font,&r,n)!=SfntStatus::resource_limit||s.stack_count!=3)return fail(n,42);
            if(n){s.stack.count=n+2;
                if(tt_execute(&s,program,TtPhase::font,&r)!=SfntStatus::output_too_small||s.stack_count!=3)return fail(n,43);
                s.stack.count=n+3;if(tt_execute(&s,{code,program.count-1},TtPhase::font,&r)!=SfntStatus::truncated||s.stack_count!=3)return fail(n,44);}
            ++push_cases;
        }
    }return true;
}
bool errors() noexcept {
    struct Case {ConstSlice<u8> code;SfntStatus expected;};
    const u8 truncated[]{0x41,2,0},underflow[]{0x60},unknown[]{0x83},bad_function[]{0xb0,0,0x2b},bad_point[]{0xb8,0xff,0xff,0x2e},bad_store[]{0xb8,0xff,0xff,0x43},bad_if[]{0xb0,0,0x58},bad_definition[]{0xb0,0,0x2c},divide_zero[]{0xb1,1,0,0x62};
    const Case list[]{{{truncated,sizeof(truncated)},SfntStatus::truncated},{{underflow,1},SfntStatus::malformed},{{unknown,1},SfntStatus::unsupported},{{bad_function,sizeof(bad_function)},SfntStatus::malformed},{{bad_point,sizeof(bad_point)},SfntStatus::malformed},{{bad_store,sizeof(bad_store)},SfntStatus::malformed},{{bad_if,sizeof(bad_if)},SfntStatus::malformed},{{bad_definition,sizeof(bad_definition)},SfntStatus::malformed},{{divide_zero,sizeof(divide_zero)},SfntStatus::malformed}};
    for(const auto& c:list){auto s=fresh();TtExecution r{};if(tt_execute(&s,c.code,TtPhase::font,&r)!=c.expected)return fail(static_cast<u32>(invalid),20);++invalid;}
    auto s=fresh();TtExecution r{};const u8 push[]{0xb0,1};s.stack.count=0;if(tt_execute(&s,{push,2},TtPhase::glyph,&r)!=SfntStatus::output_too_small)return fail(0,21);++invalid;
    s=fresh();s.functions[0]={{nullptr,1},true};if(tt_execute(&s,{bad_function,sizeof(bad_function)},TtPhase::glyph,&r)!=SfntStatus::malformed)return fail(0,22);++invalid;
    s=fresh();s.stack={nullptr,1};if(tt_execute(&s,{},TtPhase::glyph,&r)!=SfntStatus::invalid_input)return fail(0,23);++invalid;
    if(tt_execute(nullptr,{},TtPhase::glyph,&r)!=SfntStatus::invalid_input || tt_execute(&s,{},TtPhase::glyph,nullptr)!=SfntStatus::invalid_input)return fail(0,24);invalid+=2;
    return true;
}
bool argument(const wchar_t** command,wchar_t* out,u32 capacity) noexcept {auto* p=*command;while(*p==' '||*p=='\t')++p;const bool quoted=*p=='"';if(quoted)++p;u32 n=0;while(*p && !(quoted?*p=='"':*p==' '||*p=='\t')){if(n+1==capacity)return false;out[n++]=*p++;}if(quoted){if(*p!='"')return false;++p;}out[n]=0;*command=p;return n!=0;}
bool programs(const SfntFace& face) noexcept {
    ConstSlice<u8> maxp{},values{},font{},prep{};SfntHorizontalMetrics metrics{};
    if(sfnt_table(face,sfnt_tag('m','a','x','p'),&maxp)!=SfntStatus::ok || maxp.count<32 || sfnt_horizontal_metrics_open(face,&metrics)!=SfntStatus::ok)return false;
    const auto value_status=sfnt_table(face,sfnt_tag('c','v','t',' '),&values),font_status=sfnt_table(face,sfnt_tag('f','p','g','m'),&font),prep_status=sfnt_table(face,sfnt_tag('p','r','e','p'),&prep);
    const SfntStatus statuses[]{value_status,font_status,prep_status};for(auto status:statuses)if(status!=SfntStatus::ok && status!=SfntStatus::missing_table)return false;
    const u32 storage_count=byte_load_be16(maxp.data+18),function_count=byte_load_be16(maxp.data+20),stack_count=byte_load_be16(maxp.data+24),twilight_count=byte_load_be16(maxp.data+16);
    if(storage_count>128 || function_count>256 || stack_count>1024 || twilight_count>64 || values.count%2 || values.count/2>2048)return false;
    auto s=fresh();s.units_per_em=metrics.units_per_em;s.storage.count=storage_count;s.functions.count=function_count;s.stack.count=stack_count;s.twilight.count=twilight_count;s.glyph.count=0;s.cvt.count=values.count/2;
    for(u32 i=0;i<s.cvt.count;++i)cvt[i]=static_cast<i16>(byte_load_be16(values.data+i*2))*64;
    const ConstSlice<u8> code[]{font,prep};for(u32 phase=0;phase<2;++phase){s.stack_count=0;s.graphics={};TtExecution r{};const auto status=tt_execute(&s,code[phase],static_cast<TtPhase>(phase),&r);
        print(utf8_literal("phase="));number(phase);print(utf8_literal(" status="));number(static_cast<u32>(status));print(utf8_literal(" operations="));number(r.operations);print(utf8_literal(" offset="));number(r.byte_offset);print(utf8_literal(" opcode="));number(r.opcode);print(utf8_literal(" stack="));number(r.stack_count);print(utf8_literal(" depth="));number(r.call_depth);print(utf8_literal("\n"));
        if(status!=SfntStatus::ok)return false;
    }return true;
}
int file(const wchar_t* path) noexcept {
    HANDLE handle=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(handle==INVALID_HANDLE_VALUE)return 2;LARGE_INTEGER size{};if(!GetFileSizeEx(handle,&size)||size.QuadPart<=0||size.QuadPart>0xffffffffll){CloseHandle(handle);return 2;}
    HANDLE mapping=CreateFileMappingW(handle,nullptr,PAGE_READONLY,0,0,nullptr);auto* data=mapping?static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0)):nullptr;SfntFace face{};const bool ok=data && sfnt_open({data,static_cast<u32>(size.QuadPart)},0,&face)==SfntStatus::ok && programs(face);
    if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);CloseHandle(handle);return ok?0:1;
}
int run() noexcept {
    const wchar_t* command=GetCommandLineW();wchar_t path[32768];if(!argument(&command,path,32768))return 2;while(*command==' '||*command=='\t')++command;if(*command){if(!argument(&command,path,32768))return 2;while(*command==' '||*command=='\t')++command;if(*command)return 2;return file(path);}
    const bool ok=corpus()&&control_flow()&&repeated_branches()&&skipped_pushes()&&push_batches()&&errors();print(utf8_literal("cases="));number(cases);print(utf8_literal("\ncoordinate_bits="));number(coordinates);print(utf8_literal("\nbudgets="));number(budgets);print(utf8_literal("\nguards="));number(guards);print(utf8_literal("\ncontrols="));number(controls);print(utf8_literal("\npush_cases="));number(push_cases);print(utf8_literal("\nskip_cases="));number(skip_cases);print(utf8_literal("\ninvalid="));number(invalid);print(ok?utf8_literal("\nPASS native TT instruction core\n"):utf8_literal("\nFAIL native TT instruction core\n"));return ok?0:1;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_tt_interpreter_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
