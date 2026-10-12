// Shared first-party emoji test storage, font ownership and diagnostics.
u32 checks=0,case_index=0,font_index=0,cases=0,runs=0,rejected_fonts=0,nominal_runs=0,proof_fallbacks=0;u64 values=0,digest=1469598103934665603ull,root_bytes=0;
void print(const char* s) noexcept {u32 length=0;while(s[length])++length;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,length,&written,nullptr);}
void number(u64 value) noexcept {char s[32];u32 count=0;do{s[count++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<count/2;++i){const auto c=s[i];s[i]=s[count-1-i];s[count-1-i]=c;}s[count]=0;print(s);}
bool check(bool value,u32 line) noexcept {++checks;if(value)return true;print("FAIL line=");number(line);print(" font=");number(font_index);print(" case=");number(case_index);print("\n");return false;}
#define CHECK(v) do{if(!check((v),__LINE__))return false;}while(false)
bool equal(u32 actual,u32 expected,u32 line) noexcept {++values;digest=(digest^actual)*1099511628211ull;if(actual==expected)return true;print("actual=");number(actual);print(" expected=");number(expected);print(" ");return check(false,line);}
#define EQUAL(a,b) do{if(!equal((a),(b),__LINE__))return false;}while(false)
struct Reader {ConstSlice<u8> data;u32 at=0;bool word(u32* value) noexcept {if(data.count-at<4)return false;*value=byte_load_le32(data.data+at);at+=4;return true;}bool take(u32 count,ConstSlice<u8>* value) noexcept {if(count>data.count-at)return false;*value={data.data+at,count};at+=count;return true;}};
NativeFontFileSlot files[1];NativeFontFaceSlot faces[1];NativeFontInstanceSlot instances[1];NativeRunPlanSlot plans[1];
NativeFontStore fonts{{files,1},{faces,1}};NativeFontInstanceStore font_instances{&fonts,{instances,1}};NativeRunPlanStore store{&font_instances,{plans,1}};
Arena scratch{},state{};wchar_t temporary[MAX_PATH]{},font_path[32768];u8* input_file=nullptr;
struct AsciiBuffers {
    SfntClusterGlyph mapped[256];u16 glyphs[1024];SfntPlacement placements[1024];u32 map[256];
    SfntComponentGlyph records[1024],replacements[1024];SfntComponentNode nodes[256];SfntComponentUndo journal[4096];
    u64 bits[8],cursive_bits[16];u32 characters[256],targets[1024],positions[4096],rebased[4096],consumed[1024],gp_positions[4096];
    SfntMarkAssociation associations[1024];double advances[1024],prefix[1024];u8 flags[1024];SfntPosition placed[1024];SfntPlacement output[1024];
    SfntGsubExecutionFrame frames[64];SfntGposExecutionFrame gp_frames[64];SfntCursiveChainNode chains[1024];
    NativeAsciiStorage storage() noexcept {return {{mapped,256},
        {{records,1024},{nodes,256},{bits,8},{characters,256},{targets,1024},{associations,1024},{advances,1024},{flags,1024},{placed,1024},{output,1024},
         {{frames,64},{positions,4096},{rebased,4096},{consumed,1024},{reinterpret_cast<u8*>(replacements),sizeof(replacements)},{journal,4096}},
         {{gp_frames,64},{gp_positions,4096},{cursive_bits,16},{prefix,1024},{chains,1024}}},
        {glyphs,1024},{placements,1024},{map,256}};}
    NativeAsciiStorage shared() noexcept {auto result=storage();result.mapped={reinterpret_cast<SfntClusterGlyph*>(result.shaping.positions.data),256};result.glyphs={reinterpret_cast<u16*>(result.shaping.glyphs.data),1024};result.placements=result.shaping.placements;result.character_glyphs=result.shaping.character_glyphs;return result;}
} ascii_buffers;

bool argument(const wchar_t** command,wchar_t* output,u32 available) noexcept {const wchar_t* p=*command;while(*p==' '||*p=='\t')++p;const bool quoted=*p=='"';if(quoted)++p;u32 n=0;
    while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){if(n+1==available)return false;output[n++]=*p++;}if(quoted){if(*p!='"')return false;++p;}output[n]=0;*command=p;return n!=0;}
bool write_font(ConstSlice<u8> data) noexcept {HANDLE file=CreateFileW(temporary,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;const bool ok=WriteFile(file,data.data,data.count,&written,nullptr)&&written==data.count;CloseHandle(file);return ok;}
bool acquire(const wchar_t* path,NativeRunPlanHandle* run,bool reject,u32 weight,NativeRunPlanKind kind=NativeRunPlanKind::text) noexcept {NativeFontHandle face{};NativeFontInstanceHandle instance{};
    const FontAxisSetting axis{sfnt_tag('w','g','h','t'),static_cast<double>(weight)};AxisSegmentMap maps[16];double coordinates[16];const NativeFontPrepareScratch prepare{{maps,16},{coordinates,16},{},{}};
    CHECK(native_font_acquire(&fonts,path,0,&face).ok());CHECK(native_font_instance_acquire(&font_instances,face,weight?ConstSlice<FontAxisSetting>{&axis,1}:ConstSlice<FontAxisSetting>{},false,prepare,&instance).ok());
    const auto result=native_run_plan_acquire(&store,instance,&scratch,run,1000000,16*1024*1024,0,kind);
    if(reject&&result.font_status!=SfntStatus::malformed){print("reject_status=");number(static_cast<u32>(result.font_status));print(" error=");number(static_cast<u32>(result.error));print(" slot=");number(run->slot);print("\n");}
    CHECK(reject?result.font_status==SfntStatus::malformed&&!run->slot:result.ok());CHECK(native_font_instance_release(&font_instances,instance));CHECK(native_font_release(&fonts,face));return true;}
