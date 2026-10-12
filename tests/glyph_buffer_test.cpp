#include "maicord/text/glyph_buffer.hpp"

namespace maicord {namespace {
struct Record {SfntGsubGlyph glyph;u32 source,color,flags;u64 provenance;};
static_assert(sizeof(Record)==24);
u64 edit_checks=0,record_checks=0,position_checks=0,boundary_checks=0,stream_moves=0,flat_moves=0;
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
u32 random(u32* state) noexcept {*state^=*state<<13;*state^=*state>>17;*state^=*state<<5;return *state;}
Record make_record(u32 token) noexcept {return {{static_cast<u16>(token),static_cast<SfntGlyphClass>(token%5),static_cast<u8>(token>>16)},token,token^0xa63e12b7,token*17,0xaca17e0d00000000ull|token};}
const Record& at(const SfntGlyphView& view,u32 i) noexcept {return *reinterpret_cast<const Record*>(&sfnt_glyph_at(view,i));}
bool equal(const SfntGlyphBuffer& buffer,const Record* expected,u32 count) noexcept {
    const auto view=sfnt_glyph_buffer_view(buffer);if(!sfnt_glyph_view_valid(view) || sfnt_glyph_count(view)!=count)return false;
    for(u32 i=0;i<count;++i){if(!bytes_equal(&at(view,i),expected+i,sizeof(Record)))return false;++record_checks;}return true;
}
bool exercise(u32 initial,u32 seed) noexcept {
    constexpr u32 capacity=96;Record storage[capacity+2],expected[capacity],next[capacity],replacement[8];u32 origins[capacity],positions[16];u8 before[sizeof(storage)];
    for(u32 i=0;i<sizeof(storage);++i)reinterpret_cast<u8*>(storage)[i]=0xa5;
    for(u32 i=0;i<initial;++i)storage[i+1]=expected[i]=make_record(i+1);u32 count=initial;SfntGlyphBuffer buffer{};
    if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(storage+1),capacity*sizeof(Record)+3},sizeof(Record),count,&buffer)!=SfntStatus::ok)return false;
    for(u32 step=0;step<3000;++step){
        const u32 seek_to=random(&seed)%(count+1);if(sfnt_glyph_buffer_seek(&buffer,seek_to)!=SfntStatus::ok || !equal(buffer,expected,count))return false;
        u32 start=random(&seed)%(count+1),consumed=0;
        if(start<count && (random(&seed)&3)){positions[consumed++]=start;u32 cursor=start;while(consumed<16){const u32 skip=1+random(&seed)%5;if(skip>=count-cursor)break;cursor+=skip;if(random(&seed)&1)positions[consumed++]=cursor;else break;}}
        const u32 inserted=random(&seed)%9;for(u32 i=0;i<inserted;++i)replacement[i]=make_record(0x10000+step*8+i);
        const auto previous=buffer;copy_bytes(before,storage,sizeof(storage));SfntGlyphEdit edit{};
        const auto status=sfnt_glyph_buffer_replace(&buffer,start,{positions,consumed},{reinterpret_cast<const u8*>(replacement),inserted*sizeof(Record)},&edit);
        const u32 after=count-consumed+inserted;
        if(after>capacity){if(status!=SfntStatus::output_too_small || edit.consumed.data || edit.moved_records || !bytes_equal(&buffer,&previous,sizeof(buffer)) || !bytes_equal(before,storage,sizeof(storage)))return false;continue;}
        if(status!=SfntStatus::ok || edit.old_count!=count || edit.new_count!=after || edit.start!=start || edit.replacement_count!=inserted || edit.consumed.count!=consumed)return false;
        // Build an independent flat result by walking old records and emitting
        // replacement records at the insertion point. Track surviving old slots.
        u32 n=0,p=0;for(u32 old=0;old<=count;++old){if(old==start)for(u32 i=0;i<inserted;++i){next[n]=replacement[i];origins[n++]=sfnt_removed_position;}if(old==count)break;
            if(p<consumed && positions[p]==old){++p;continue;}next[n]=expected[old];origins[n++]=old;}
        if(n!=after || !equal(buffer,next,n))return false;
        for(u32 old=0;old<count;++old){u32 found=sfnt_removed_position;for(u32 i=0;i<n;++i)if(origins[i]==old){found=i;break;}u32 actual=0;if(sfnt_glyph_edit_position(edit,old,&actual)!=SfntStatus::ok || actual!=found)return false;++position_checks;}
        for(u32 boundary=0;boundary<=count;++boundary)for(u32 affinity=0;affinity<2;++affinity){u32 found=0;for(u32 i=0;i<n;++i)if(origins[i]!=sfnt_removed_position && origins[i]<boundary)++found;
            if(start<boundary || (start==boundary && affinity))found+=inserted;u32 actual=0;if(sfnt_glyph_edit_boundary(edit,boundary,affinity!=0,&actual)!=SfntStatus::ok || actual!=found)return false;++boundary_checks;}
        for(u32 i=0;i<n;++i)expected[i]=next[i];count=n;++edit_checks;
        for(u32 i=0;i<sizeof(Record);++i)if(reinterpret_cast<u8*>(storage)[i]!=0xa5 || reinterpret_cast<u8*>(storage+capacity+1)[i]!=0xa5)return false;
    }
    return true;
}
bool failures() noexcept {
    Record storage[8],replacement[2];for(u32 i=0;i<8;++i)storage[i]=make_record(i);replacement[0]=make_record(50);replacement[1]=make_record(51);SfntGlyphBuffer buffer{};SfntGlyphEdit edit{};
    if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(storage),sizeof(storage)},sizeof(Record),6,&buffer)!=SfntStatus::ok)return false;
    u32 positions[]{1,3,3};const auto original=buffer;Record saved[8];copy_bytes(saved,storage,sizeof(storage));
    if(sfnt_glyph_buffer_replace(&buffer,1,{positions,3},{reinterpret_cast<const u8*>(replacement),sizeof(replacement)},&edit)!=SfntStatus::invalid_input)return false;
    positions[2]=7;if(sfnt_glyph_buffer_replace(&buffer,1,{positions,3},{reinterpret_cast<const u8*>(replacement),sizeof(replacement)},&edit)!=SfntStatus::invalid_input)return false;
    if(sfnt_glyph_buffer_replace(&buffer,1,{positions,1},{reinterpret_cast<const u8*>(storage),sizeof(Record)},&edit)!=SfntStatus::invalid_input)return false;
    if(sfnt_glyph_buffer_replace(&buffer,1,{positions,1},{reinterpret_cast<const u8*>(replacement),sizeof(replacement)},&edit,0)!=SfntStatus::resource_limit)return false;
    if(sfnt_glyph_buffer_seek(&buffer,0,0)!=SfntStatus::resource_limit || !bytes_equal(&buffer,&original,sizeof(buffer)) || !bytes_equal(storage,saved,sizeof(storage)))return false;
    if(sfnt_glyph_buffer_replace(&buffer,6,{}, {},&edit,0)!=SfntStatus::ok || edit.moved_records || !bytes_equal(&buffer,&original,sizeof(buffer)))return false;
    // Force many-to-many fallback with no free gap and retained intervening slots.
    if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(storage),sizeof(storage)},sizeof(Record),8,&buffer)!=SfntStatus::ok)return false;
    positions[0]=1;positions[1]=3;positions[2]=5;
    if(sfnt_glyph_buffer_replace(&buffer,1,{positions,3},{reinterpret_cast<const u8*>(replacement),sizeof(replacement)},&edit)!=SfntStatus::ok)return false;
    const Record expected[]{saved[0],replacement[0],replacement[1],saved[2],saved[4],saved[6],saved[7]};if(!equal(buffer,expected,7))return false;
    const u32 remove[]{0,1,2,3,4,5,6};if(sfnt_glyph_buffer_replace(&buffer,0,{remove,7},{},&edit)!=SfntStatus::ok || sfnt_glyph_count(sfnt_glyph_buffer_view(buffer)))return false;
    if(sfnt_glyph_buffer_replace(&buffer,0,{}, {reinterpret_cast<const u8*>(replacement),sizeof(replacement)},&edit)!=SfntStatus::ok || !equal(buffer,replacement,2))return false;
    SfntGlyphBuffer empty{};return sfnt_glyph_buffer_open({},sizeof(Record),0,&empty)==SfntStatus::ok && sfnt_glyph_view_valid(sfnt_glyph_buffer_view(empty)) &&
        sfnt_glyph_buffer_seek(&empty,0,0)==SfntStatus::ok && sfnt_glyph_buffer_open({reinterpret_cast<u8*>(storage),sizeof(storage)},3,0,&empty)==SfntStatus::invalid_input;
}
SfntGsubGlyph stream[8192];
bool streaming() noexcept {
    constexpr u32 n=4096;for(u32 i=0;i<n;++i)stream[i]={static_cast<u16>(i),SfntGlyphClass::base,0};SfntGlyphBuffer buffer{};
    if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(stream),sizeof(stream)},sizeof(SfntGsubGlyph),n,&buffer)!=SfntStatus::ok || sfnt_glyph_buffer_seek(&buffer,0)!=SfntStatus::ok)return false;
    stream_moves=n;
    for(u32 i=0;i<n;++i){const u32 consumed=i*2;const SfntGsubGlyph output[]{{static_cast<u16>(i),SfntGlyphClass::base,0},{static_cast<u16>(i+8192),SfntGlyphClass::mark,0}};SfntGlyphEdit edit{};
        if(sfnt_glyph_buffer_replace(&buffer,consumed,{&consumed,1},{reinterpret_cast<const u8*>(output),sizeof(output)},&edit)!=SfntStatus::ok)return false;stream_moves+=edit.moved_records;flat_moves+=n-i-1+2;}
    const auto view=sfnt_glyph_buffer_view(buffer);if(sfnt_glyph_count(view)!=n*2 || stream_moves!=3*n || flat_moves!=static_cast<u64>(n)*(n-1)/2+2*n)return false;
    for(u32 i=0;i<n;++i)if(sfnt_glyph_at(view,i*2).glyph!=i || sfnt_glyph_at(view,i*2+1).glyph!=i+8192 || sfnt_glyph_at(view,i*2+1).glyph_class!=SfntGlyphClass::mark)return false;return true;
}
bool guards() noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;DWORD old=0;if(!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}
    bool ok=true;Record replacement=make_record(17);
    for(u32 capacity=1;capacity<=32 && ok;++capacity){auto* b=memory+page-capacity*sizeof(Record);SfntGlyphBuffer buffer{};for(u32 i=0;i<capacity;++i)copy_bytes(b+i*sizeof(Record),&replacement,sizeof(Record));
        ok=sfnt_glyph_buffer_open({b,capacity*sizeof(Record)},sizeof(Record),capacity,&buffer)==SfntStatus::ok;
        for(u32 start=0;start<capacity && ok;++start){const u32 consumed=start;SfntGlyphEdit edit{};ok=sfnt_glyph_buffer_seek(&buffer,start)==SfntStatus::ok && sfnt_glyph_buffer_replace(&buffer,start,{&consumed,1},{reinterpret_cast<const u8*>(&replacement),sizeof(replacement)},&edit)==SfntStatus::ok;}}
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
int run() noexcept {
    if(!failures()){print(utf8_literal("FAIL glyph buffer failure checks\n"));return 1;}
    if(!exercise(0,0x941ae973) || !exercise(96,0x0519baed)){print(utf8_literal("FAIL glyph buffer model\n"));return 2;}
    if(!streaming()){print(utf8_literal("FAIL glyph buffer stream\n"));return 3;}if(!guards()){print(utf8_literal("FAIL glyph buffer guards\n"));return 4;}
    print(utf8_literal("edits="));number(edit_checks);print(utf8_literal("\nrecords="));number(record_checks);print(utf8_literal("\npositions="));number(position_checks);print(utf8_literal("\nboundaries="));number(boundary_checks);print(utf8_literal("\ngap_record_copies="));number(stream_moves);print(utf8_literal("\nflat_record_copies="));number(flat_moves);print(utf8_literal("\nPASS native glyph gap buffer\n"));return 0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_glyph_buffer_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
