// Records and checks the committed references of the native app line driver; app_line_reference.hpp describes
// them and tools/font_reference/text_reference.py writes them. The record (reference_end) and reference
// (select_profile) layouts are read and written in both places; change them together.
#include "native_app_line_test.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/text/fonts/run.hpp"
#include "maicord/text/fonts/session.hpp"
#include "maicord/text/fonts/store.hpp"
namespace maicord::native_app_line {
namespace {
#include "../../generated/app_line_reference.inc"
constexpr u32 kGroupShift=6;
constexpr u32 kFontCapacity=64,kSessionCapacity=8,kContext=16;
constexpr u64 kSeed=0x9e3779b97f4a7c15ull;
// The layout digest: 8 bytes per step.
inline u64 mix(u64 h,u64 v) noexcept {h=(h^v)*0xff51afd7ed558ccdull;return h^(h>>29);}
u64 digest_bytes(u64 h,const void* data,u32 bytes) noexcept {
    const auto* p=static_cast<const u8*>(data);u32 at=0;
    for(;at+8<=bytes;at+=8){u64 v;copy_bytes(&v,p+at,8);h=mix(h,v);}
    u64 tail=0;copy_bytes(&tail,p+at,bytes-at);return mix(mix(h,tail),bytes);
}
template<class T> T read(const u8* p) noexcept {T value;copy_bytes(&value,p,sizeof(T));return value;}
// A font of the profile's table: table-directory digest and face, and the first directory bytes, which confirm that
// a cached mapping address still holds this font. A system font (under a Windows font folder) the reference never
// recorded is foreign: the groups that use it are skipped. Any other font is the repository's and must be recorded.
struct Font {u64 digest;u32 face,table_bytes;u16 upem,path_units;bool system,foreign;u8 table[2048];wchar_t path[512];char name[64];};
struct CachedFont {const u8* directory;u32 face,index;};
struct State {
    bool record,active,failed,skipped,group_open,group_shaped,unshaped,simulate_foreign;
    u64 profile;wchar_t profile_text[2048];u32 profile_units;
    Font fonts[kFontCapacity];u32 font_count;u64 foreign;CachedFont cached[16];u32 cached_next;
    NativeFontSession* sessions[kSessionCapacity];u32 session_count;
    const u8* reference_fonts;u32 reference_font_count;
    const u8* profile_fonts;u32 profile_font_count;const u8* profile_groups;u32 profile_group_count,profile_cases;
    const u8* runs;const u8* divergent;u32 run_count;
    u32 group_index;u64 group_hash,group_fonts;
    u64 run_mismatches,runs_missing,runs_checked,runs_divergent,runs_foreign,group_mismatches,groups_checked,groups_skipped;u32 reported;
    Arena groups,requests,keys;u64* request_keys;u32 request_count;
    wchar_t directory[1024];
} state{};
inline u32 bits(float value) noexcept {return __builtin_bit_cast(u32,value);}
void say(const char* text) noexcept {print(text);}
void wide(const wchar_t* text,u32 units) noexcept {char out[4];for(u32 i=0;i<units;++i){const u8 n=utf8_encode(text[i],out);char line[5]{};copy_bytes(line,out,n);print(line);}}
bool append(Arena& arena,const void* data,usize bytes) noexcept {
    if(!bytes)return true;auto* to=static_cast<u8*>(arena_allocate(&arena,bytes,1));if(!to)return false;copy_bytes(to,data,bytes);return true;
}
template<class T> bool append_value(Arena& arena,const T& value) noexcept {return append(arena,&value,sizeof(T));}
// The profile: the arguments after the program, each path reduced to its file name, then MAICORD_CJK_SAMPLE.
void profile_from_command_line() noexcept {
    const wchar_t* p=GetCommandLineW();bool quote=false;
    while(*p){if(*p=='"')quote=!quote;else if(!quote&&(*p==' '||*p=='\t'))break;++p;}
    u32 n=0;auto put=[&](wchar_t c) noexcept {if(n+1<2048)state.profile_text[n++]=c;};
    while(*p){while(*p==' '||*p=='\t')++p;if(!*p)break;
        wchar_t token[1024];u32 length=0;quote=false;
        while(*p&&(quote||(*p!=' '&&*p!='\t'))){if(*p=='"')quote=!quote;else if(length+1<1024)token[length++]=*p;++p;}
        u32 first=0;for(u32 i=0;i<length;++i)if(token[i]=='\\'||token[i]=='/')first=i+1;
        if(n)put(' ');for(u32 i=first;i<length;++i)put(token[i]);}
    wchar_t sample[16];const DWORD sample_units=GetEnvironmentVariableW(L"MAICORD_CJK_SAMPLE",sample,16);
    if(sample_units&&sample_units<16){const wchar_t* label=L" cjk=";while(*label)put(*label++);for(u32 i=0;i<sample_units;++i)put(sample[i]);}
    state.profile_units=n;state.profile=hash64_bytes(kHash64Seed,state.profile_text,n*2);
}
// The reference: "MCTXREF2", u32 font, profile and run counts and a reserved u32; fonts (80 bytes: u64 digest, u32
// face, u32 flags with bit 0 for a system font, char name[64]); profiles (u64 key, u32 cases, u32 fonts, u32 groups,
// u32 reserved, then u32 font indices and 16-byte groups: u32 index, u32 reserved, u64 digest, sorted; an index may
// carry one digest per recorded machine); HarfBuzz runs (12 bytes: u64 key, u32 digest, sorted by key) and one bit
// per run marking those that differed from the engine when recorded.
bool select_profile() noexcept {
    const u8* p=app_line_reference;const u8* end=p+sizeof(app_line_reference);
    if(sizeof(app_line_reference)<24||!bytes_equal(p,"MCTXREF2",8))return false;
    const u32 fonts=read<u32>(p+8),profiles=read<u32>(p+12),runs=read<u32>(p+16);p+=24;
    if(static_cast<usize>(end-p)<static_cast<usize>(fonts)*80)return false;
    state.reference_fonts=p;state.reference_font_count=fonts;p+=static_cast<usize>(fonts)*80;
    for(u32 i=0;i<profiles;++i){
        if(end-p<24)return false;
        const u64 key=read<u64>(p);const u32 cases_recorded=read<u32>(p+8),font_count=read<u32>(p+12),group_count=read<u32>(p+16);p+=24;
        if(static_cast<usize>(end-p)<static_cast<usize>(font_count)*4+static_cast<usize>(group_count)*16)return false;
        if(key==state.profile){state.profile_cases=cases_recorded;state.profile_fonts=p;state.profile_font_count=font_count;
            state.profile_groups=p+static_cast<usize>(font_count)*4;state.profile_group_count=group_count;}
        p+=static_cast<usize>(font_count)*4+static_cast<usize>(group_count)*16;
    }
    if(static_cast<usize>(end-p)!=static_cast<usize>(runs)*12+(runs+7)/8)return false;
    state.runs=p;state.divergent=p+static_cast<usize>(runs)*12;state.run_count=runs;
    return true;
}
void report_profile() noexcept {say(" profile=\"");wide(state.profile_text,state.profile_units);say("\"\n");}
u16 upem_of(const SfntFace& face) noexcept {ConstSlice<u8> head{};
    return sfnt_table(face,sfnt_tag('h','e','a','d'),&head)==SfntStatus::ok&&head.count>=54?static_cast<u16>(head.data[18]<<8|head.data[19]):0;}
// A new font: recorded for this profile, foreign (a system font of another version or never recorded), or a changed
// repository font, which fails until the reference is recorded again.
void admit_font(Font& font,u32 index) noexcept {
    font.system=false;
    for(u32 i=0;i+15<=font.path_units&&!font.system;++i){bool match=true;const wchar_t* folder=L"\\windows\\fonts\\";
        for(u32 j=0;folder[j]&&match;++j){wchar_t c=font.path[i+j];c=c>='A'&&c<='Z'?static_cast<wchar_t>(c+32):c;match=c==folder[j];}font.system=match;}
    if(state.record)return;
    bool known=false;
    for(u32 i=0;i<state.profile_font_count&&!known;++i){const u32 at=read<u32>(state.profile_fonts+i*4);if(at>=state.reference_font_count)continue;
        const u8* f=state.reference_fonts+static_cast<usize>(at)*80;known=read<u64>(f)==font.digest&&read<u32>(f+8)==font.face;}
    if(known&&!(state.simulate_foreign&&font.system))return;
    if(font.system){font.foreign=true;state.foreign|=1ull<<index;
        say("SKIP reference groups using ");say(font.name);say(" (face ");number(font.face);say("): this machine's version was never recorded (tools/font_reference/text_reference.py record --merge)");report_profile();return;}
    say("REFERENCE font ");say(font.name);say(" (face ");number(font.face);say(") is not the recorded one; record the references again");report_profile();state.failed=true;
}
// The font table index of a store face. Mappings are cached by address and confirmed by content: a released
// session's address may later hold another file.
u32 font_of(const NativeFontStore& store,NativeFontHandle handle) noexcept {
    const auto* face=native_font_get(store,handle);if(!face)return 0xfffffffeu;
    const auto& sfnt=face->geometry.face;const u32 bytes=minimum(sfnt.directory.count,2048u);
    for(const auto& c:state.cached)if(c.directory==sfnt.directory.data&&c.face==sfnt.face_index){const auto& f=state.fonts[c.index];
        if(f.table_bytes==bytes&&bytes_equal(f.table,sfnt.directory.data,bytes))return c.index;}
    const u64 digest=hash64_bytes(kHash64Seed,sfnt.directory.data,sfnt.directory.count);
    u32 index=0;while(index<state.font_count&&!(state.fonts[index].digest==digest&&state.fonts[index].face==sfnt.face_index))++index;
    if(index==state.font_count){
        if(state.font_count==kFontCapacity)return 0xfffffffdu;
        auto& font=state.fonts[state.font_count];font={};
        font.digest=digest;font.face=sfnt.face_index;font.table_bytes=bytes;copy_bytes(font.table,sfnt.directory.data,bytes);font.upem=upem_of(sfnt);
        const HANDLE file=store.files[store.faces[handle.slot-1].file_slot].file;
        const DWORD units=GetFinalPathNameByHandleW(file,font.path,512,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        font.path_units=static_cast<u16>(units&&units<512?units:0);
        u32 first=0;for(u32 i=0;i<font.path_units;++i)if(font.path[i]=='\\'||font.path[i]=='/')first=i+1;
        u32 length=0;for(u32 i=first;i<font.path_units&&length<63;++i){const wchar_t c=font.path[i];font.name[length++]=static_cast<char>(c>='A'&&c<='Z'?c+32:c<128?c:'?');}
        admit_font(font,state.font_count);
        ++state.font_count;
    }
    state.cached[state.cached_next++&15]={sfnt.directory.data,sfnt.face_index,index};
    return index;
}
u32 font_index(NativeFontSession* session,const TextRun& run,const NativeFontRun** record_out) noexcept {
    if(!session||!run.native_font)return 0xffffffffu;
    const auto* record=native_font_run_get(*native_font_session_runs(session),run.native_face);if(!record)return 0xfffffffeu;
    if(record_out)*record_out=record;
    return font_of(*native_font_session_fonts(session),record->source.face);
}
// The fonts the registered sessions shaped with so far (their live run records): what a group that observed no
// shaped runs (hits, measures, editor geometry) may depend on.
u64 session_fonts() noexcept {
    u64 mask=0;
    for(u32 s=0;s<state.session_count;++s){const auto& runs=*native_font_session_runs(state.sessions[s]);const auto& store=*native_font_session_fonts(state.sessions[s]);
        for(u32 i=0;i<runs.slots.count;++i){if(!runs.slots[i].references)continue;const u32 index=font_of(store,runs.slots[i].run.source.face);if(index<kFontCapacity)mask|=1ull<<index;}}
    return mask;
}
// A group is checked when none of its fonts is foreign: against any digest recorded for its index.
void finish_group() noexcept {
    if(!state.group_open)return;state.group_open=false;
    if(!state.group_shaped)state.group_fonts|=session_fonts();
    if(state.record){append_value(state.groups,state.group_index);append_value(state.groups,state.group_hash);append_value(state.groups,state.group_fonts);return;}
    if(state.group_fonts&state.foreign){++state.groups_skipped;return;}
    ++state.groups_checked;
    u32 low=0,high=state.profile_group_count;
    while(low<high){const u32 middle=(low+high)/2;if(read<u32>(state.profile_groups+middle*16)<state.group_index)low=middle+1;else high=middle;}
    for(;low<state.profile_group_count&&read<u32>(state.profile_groups+low*16)==state.group_index;++low)if(read<u64>(state.profile_groups+low*16+8)==state.group_hash)return;
    if(!state.group_mismatches++){say("REFERENCE layout differs from cases ");number(state.group_index<<kGroupShift);say("..");
        number(((state.group_index+1)<<kGroupShift)-1);say(" on (record with tools/font_reference/text_reference.py to see the cases)");report_profile();}
    state.failed=true;
}
void observe_raw(const void* data,u32 bytes) noexcept {
    if(!state.active)return;const u32 group=cases>>kGroupShift;
    if(!state.group_open||group!=state.group_index){finish_group();state.group_open=true;state.group_index=group;state.group_hash=mix(kSeed,group);state.group_fonts=0;state.group_shaped=false;}
    state.group_hash=digest_bytes(state.group_hash,data,bytes);
}
u16 utf16[65536+2*kContext];
u32 to_utf16(Utf8View text) noexcept {
    u32 units=0;for(u32 at=0;at<text.bytes&&units+2<=65536;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)break;
        if(cp.codepoint>0xffff){utf16[units++]=static_cast<u16>(0xd800+((cp.codepoint-0x10000)>>10));utf16[units++]=static_cast<u16>(0xdc00+((cp.codepoint-0x10000)&1023));}
        else utf16[units++]=static_cast<u16>(cp.codepoint);at+=cp.width;}
    return units;
}
bool paragraph_separator(u16 c) noexcept {return c=='\r'||c=='\n'||c==0x0b||c==0x0c||c==0x85||c==0x2028||c==0x2029;}
bool request_seen(u64 key) noexcept {
    constexpr u32 mask=(1u<<20)-1;u32 at=static_cast<u32>(key^key>>32)&mask;const u64 stored=key|1;
    while(state.request_keys[at]){if(state.request_keys[at]==stored)return true;at=(at+1)&mask;}
    if(state.request_count>=mask/2)return true;state.request_keys[at]=stored;++state.request_count;return false;
}
u32 scalar_at(u32 at,u32 limit) noexcept {
    const u16 c=utf16[at];
    if(c>=0xd800&&c<0xdc00&&at+1<limit){const u16 d=utf16[at+1];if(d>=0xdc00&&d<0xe000)return 0x10000+((c-0xd800u)<<10)+(d-0xdc00u);}
    return c;
}
bool strong_script(u32 cp) noexcept {const auto s=unicode_properties(cp)->script();return s!=UnicodeScript::zinh&&s!=UnicodeScript::zyyy&&s!=UnicodeScript::zzzz;}
bool joining(u32 cp) noexcept {const auto j=unicode_properties(cp)->joining_type();return j==UnicodeJoiningType::c||j==UnicodeJoiningType::d||j==UnicodeJoiningType::l||j==UnicodeJoiningType::r;}
// The scalar whose script the run is shaped in: its first one of a specific script, else the nearest one before it in
// the paragraph, else after it; zero when the paragraph has none (HarfBuzz then guesses from the run).
u32 script_anchor(u32 begin,u32 end,u32 units) noexcept {
    for(u32 u=begin;u<end;){const u32 cp=scalar_at(u,end);if(strong_script(cp))return cp;u+=cp>0xffff?2:1;}
    for(u32 u=begin;u>0&&!paragraph_separator(utf16[u-1]);){u32 at=u-1;if(at&&utf16[at]>=0xdc00&&utf16[at]<0xe000&&utf16[at-1]>=0xd800&&utf16[at-1]<0xdc00)--at;
        const u32 cp=scalar_at(at,u);if(strong_script(cp))return cp;u=at;}
    for(u32 u=end;u<units&&!paragraph_separator(utf16[u]);){const u32 cp=scalar_at(u,units);if(strong_script(cp))return cp;u+=cp>0xffff?2:1;}
    return 0;
}
// The canonical form both sides digest (tools/font_reference/text_reference.py writes HarfBuzz's): the run's units
// grouped by equal cluster-map value in logical order, each group as u16 first unit, unit count, glyph count and
// glyph ids (structure), and per glyph i32 advance, x and y offset in design units (positions; x along +x, so an
// RTL run's advance offset is negated). Units without a glyph form glyphless groups. The digest is the structure's,
// extended by the positions' unless tracking or a simulation moved them, folded to 32 bits.
u32 canonical_run(const ShapedText& text,const TextRun& run,u32 upem,bool measured) noexcept {
    u64 s=kHash64Seed,p=kHash64Seed;const double scale=static_cast<double>(upem)/run.em_size;const bool rtl=run.bidi_level&1;
    const auto design=[&](float value) noexcept {const double scaled=value*scale;return static_cast<i32>(scaled<0?scaled-.5:scaled+.5);};
    const u32 begin=run.text_position,end=begin+run.text_length,last=run.first_glyph+run.glyph_count;
    for(u32 u=begin;u<end;){const u16 value=text.clusters[u];u32 w=u+1;while(w<end&&text.clusters[w]==value)++w;
        u32 first=value,stop=value;
        if(value!=0xffff){stop=last;for(u32 x=w;x<end;++x)if(text.clusters[x]!=0xffff){if(text.clusters[x]>value)stop=text.clusters[x];break;}}
        const u16 group[3]{static_cast<u16>(u-begin),static_cast<u16>(w-u),static_cast<u16>(stop-first)};s=hash64_bytes(s,group,6);
        for(u32 g=first;g<stop;++g){const auto& glyph=text.glyphs[g];s=hash64_bytes(s,&glyph.index,2);
            const i32 values[3]{design(glyph.advance),rtl?-design(glyph.advance_offset):design(glyph.advance_offset),design(glyph.ascender_offset)};p=hash64_bytes(p,values,12);}
        u=w;}
    if(!measured)s=hash64_bytes(s,&p,8);
    return static_cast<u32>(s^s>>32);
}
void report_run(const char* what,u64 key,const ShapedText& text,const TextRun& run) noexcept {
    if(state.reported>=8)return;++state.reported;
    say("HARFBUZZ ");say(what);say(" key=");char hex[17];for(u32 i=0;i<16;++i)hex[i]="0123456789abcdef"[key>>(60-4*i)&15];hex[16]=0;say(hex);say(" text=\"");
    wide(reinterpret_cast<const wchar_t*>(utf16+run.text_position),run.text_length);say("\" native glyphs=");
    for(u32 g=0;g<run.glyph_count;++g){if(g)say(",");number(text.glyphs[run.first_glyph+g].index);}
    say(" clusters=");for(u32 u=0;u<run.text_length;++u){if(u)say(",");const u16 c=text.clusters[run.text_position+u];if(c==0xffff)say("-");else number(c-run.first_glyph);}
    say(" (tools/font_reference/text_reference.py explain shows HarfBuzz's)");report_profile();
}
// One run's HarfBuzz request: font, direction, features, axes, script anchor and text, with up to 16 units of
// context (cut at paragraph separators) for a run that joins. The record keeps the native glyphs and cluster map for
// the tool's report; the check compares the run's canonical digests with HarfBuzz's.
void check_run(const ShapedText& text,const TextRun& run,u32 font,const NativeFontRun& record,u32 units,bool independent,float tracking) noexcept {
    if(!run.text_length||run.text_position+run.text_length>units||run.text_position+run.text_length>text.cluster_count)return;
    const u32 begin=run.text_position,end=begin+run.text_length;
    // A shaping run maps its source onto its own glyphs; an ellipsis sign's run (source zero, length one) maps
    // nothing of its own and is not a shaping run.
    bool mapped=false,contextual=false;
    for(u32 u=begin;u<end;++u){const u32 c=text.clusters[u];if(c==0xffff)continue;if(c<run.first_glyph||c>=run.first_glyph+run.glyph_count)return;mapped=true;}
    if(!mapped)return;
    for(u32 u=begin;u<end&&!contextual;){const u32 cp=scalar_at(u,end);contextual=joining(cp);u+=cp>0xffff?2:1;}
    u32 before=0,after=0;
    while(contextual&&before<kContext&&begin-before>0&&!paragraph_separator(utf16[begin-before-1]))++before;
    while(contextual&&after<kContext&&end+after<units&&!paragraph_separator(utf16[end+after]))++after;
    const u32 anchor=script_anchor(begin,end,units);
    // Bits 4 and 8: the run begins or ends its paragraph, HarfBuzz's BOT and EOT buffer flags.
    const bool starts=!begin||paragraph_separator(utf16[begin-1]),ends=end==units||paragraph_separator(utf16[end]);
    const u8 flags=static_cast<u8>((run.bidi_level&1)|(independent?2:0)|(starts?4:0)|(ends?8:0));
    const u8 measured=static_cast<u8>((tracking!=0?4:0)|(record.simulations?8:0));
    u64 key=hash64_bytes(kHash64Seed,&state.fonts[font].digest,8);key=hash64_bytes(key,&state.fonts[font].face,4);key=hash64_bytes(key,&flags,1);
    const u8 axes=static_cast<u8>(record.source.count);key=hash64_bytes(key,&axes,1);
    for(u32 i=0;i<axes;++i){const u32 tag=record.source.settings[i].tag;const float value=static_cast<float>(record.source.settings[i].value);key=hash64_bytes(key,&tag,4);key=hash64_bytes(key,&value,4);}
    key=hash64_bytes(key,&anchor,4);key=hash64_bytes(key,&measured,1);
    const u16 lengths[3]{static_cast<u16>(before),static_cast<u16>(end-begin),static_cast<u16>(after)};key=hash64_bytes(key,lengths,6);
    key=hash64_bytes(key,utf16+begin-before,(before+end-begin+after)*2);
    ++state.runs_checked;
    if(state.record){
        // Each distinct native shape of a request is recorded: the run is conformant only if all equal HarfBuzz's.
        const u64 shape=canonical_run(text,run,state.fonts[font].upem,measured!=0);
        if(request_seen(key^(shape+1)*0x9e3779b97f4a7c15ull))return;
        auto& r=state.requests;const u16 font16=static_cast<u16>(font);
        append_value(r,key);append_value(r,font16);append_value(r,flags);append_value(r,measured);append_value(r,axes);
        for(u32 i=0;i<axes;++i){append_value(r,record.source.settings[i].tag);append_value(r,static_cast<float>(record.source.settings[i].value));}
        append_value(r,anchor);append(r,lengths,6);append(r,utf16+begin-before,(before+end-begin+after)*2);append_value(r,run.em_size);
        append_value(r,static_cast<u16>(run.glyph_count));
        for(u32 g=0;g<run.glyph_count;++g){const auto& glyph=text.glyphs[run.first_glyph+g];
            append_value(r,glyph.index);append_value(r,glyph.advance);append_value(r,glyph.advance_offset);append_value(r,glyph.ascender_offset);}
        for(u32 u=begin;u<end;++u){const u16 c=text.clusters[u];append_value(r,static_cast<u16>(c==0xffff?0xffff:c-run.first_glyph));}
        return;
    }
    u32 low=0,high=state.run_count;
    while(low<high){const u32 middle=(low+high)/2;if(read<u64>(state.runs+middle*12)<key)low=middle+1;else high=middle;}
    if(low==state.run_count||read<u64>(state.runs+low*12)!=key){++state.runs_missing;report_run("reference missing",key,text,run);state.failed=true;return;}
    // A run recorded as differing from HarfBuzz is counted, not compared; a run that matched may not regress.
    if(state.divergent[low>>3]>>(low&7)&1){++state.runs_divergent;return;}
    if(read<u32>(state.runs+low*12+8)==canonical_run(text,run,state.fonts[font].upem,measured!=0))return;
    ++state.run_mismatches;report_run("differs",key,text,run);state.failed=true;
}
} // namespace

bool reference_begin() noexcept {
    profile_from_command_line();
    const DWORD units=GetEnvironmentVariableW(L"MAICORD_TEXT_REFERENCE_RECORD",state.directory,1024);
    state.record=units&&units<1000;state.active=true;
    if(state.record){
        CHECK(arena_initialize(&state.groups,1ull<<30,0)&&arena_initialize(&state.requests,1ull<<32,0));
        CHECK(arena_initialize(&state.keys,8u<<20,8u<<20));
        state.request_keys=static_cast<u64*>(arena_allocate_zero(&state.keys,8u<<20,8));CHECK(state.request_keys);
        return true;
    }
    CHECK(select_profile());
    if(!state.profile_groups&&!state.profile_cases){say("SKIP reference: none recorded for this profile; record it with tools/font_reference/text_reference.py");report_profile();state.skipped=true;state.active=false;}
    // Treats every system font as foreign: the coverage a machine with other system fonts keeps.
    wchar_t simulate[4];state.simulate_foreign=GetEnvironmentVariableW(L"MAICORD_TEXT_REFERENCE_FOREIGN",simulate,4)==1&&simulate[0]=='1';
    return true;
}
void observe_bytes(const void* data,u32 bytes) noexcept {observe_raw(data,bytes);}
void observe_measure(const TextMeasureResult& value) noexcept {
    const u32 words[6]{bits(value.width),bits(value.width_including_trailing_whitespace),bits(value.height),value.utf16_units,value.line_count,static_cast<u32>(value.error)};
    observe_raw(words,sizeof(words));
}
void observe_shape(const ShapedText& text,Utf8View source,bool independent,float tracking) noexcept {
    if(!state.active)return;
    const u32 header[10]{text.glyph_count,text.run_count,text.cluster_count,text.decoration_count,text.hit_region_count,
        bits(text.width),bits(text.width_including_trailing_whitespace),bits(text.height),text.line_count,0};
    observe_raw(header,sizeof(header));
    observe_raw(text.glyphs,text.glyph_count*sizeof(TextGlyph));observe_raw(text.clusters,text.cluster_count*2);
    observe_raw(text.decorations,text.decoration_count*sizeof(TextDecoration));observe_raw(text.hit_regions,text.hit_region_count*sizeof(TextHitRegion));
    const u32 units=to_utf16(source);
    for(u32 i=0;i<text.run_count;++i){const auto& run=text.runs[i];const NativeFontRun* record=nullptr;
        const u32 font=font_index(text.native_fonts(),run,&record);
        const u64 identity=font<kFontCapacity?state.fonts[font].digest^state.fonts[font].face:font;
        const u32 simulations=record?record->simulations:0;
        const u32 words[13]{static_cast<u32>(identity),static_cast<u32>(identity>>32),simulations,bits(run.baseline_x),bits(run.baseline_y),bits(run.em_size),run.first_glyph,run.glyph_count,
            run.text_position,run.text_length,run.style_flags,static_cast<u32>(run.bidi_level|run.sideways<<8|run.native_font<<9),record?record->source.count:0};
        observe_raw(words,sizeof(words));state.group_shaped=true;if(font<kFontCapacity)state.group_fonts|=1ull<<font;
        if(record)for(u32 a=0;a<record->source.count;++a)observe_raw(&record->source.settings[a],sizeof(FontAxisSetting));
        if(font<kFontCapacity&&record&&run.glyph_count&&!state.unshaped){if(state.foreign>>font&1)++state.runs_foreign;else check_run(text,run,font,*record,units,independent,tracking);}
    }
}
u32 reference_end(bool ok) noexcept {
    finish_group();
    if(state.record){
        wchar_t path[1100];u32 n=0;for(u32 i=0;state.directory[i]&&n<1024;++i)path[n++]=state.directory[i];
        path[n++]='\\';for(u32 shift=64;shift;){shift-=4;path[n++]=L"0123456789abcdef"[state.profile>>shift&15];}
        const wchar_t* suffix=L".bin";while(*suffix)path[n++]=*suffix++;path[n]=0;
        const HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        bool written=file!=INVALID_HANDLE_VALUE;
        const auto put=[&](const void* data,usize bytes) noexcept {const auto* p=static_cast<const u8*>(data);
            while(written&&bytes){const DWORD chunk=static_cast<DWORD>(bytes>(1u<<30)?(1u<<30):bytes);DWORD done=0;written=WriteFile(file,p,chunk,&done,nullptr)&&done==chunk;p+=chunk;bytes-=chunk;}};
        const u32 counts[4]{state.profile_units,cases,state.font_count,static_cast<u32>(state.groups.offset/20)};
        put("MCTXREC2",8);put(&state.profile,8);put(counts,16);put(state.profile_text,state.profile_units*2);
        for(u32 i=0;i<state.font_count;++i){const auto& f=state.fonts[i];const u16 system=f.system;
            put(&f.digest,8);put(&f.face,4);put(&f.upem,2);put(&system,2);put(&f.path_units,2);put(f.path,f.path_units*2);}
        put(state.groups.base,state.groups.offset);
        const u32 requests=state.request_count;put(&requests,4);put(state.requests.base,state.requests.offset);
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        arena_release(&state.groups);arena_release(&state.requests);arena_release(&state.keys);
        say("reference_recorded fonts=");number(state.font_count);say(" runs=");number(state.runs_checked);say(" requests=");number(requests);say(written?"\n":" WRITE FAILED\n");
        return ok&&written?0:1;
    }
    if(state.skipped)return ok?77:1;
    // Without foreign fonts the run must also have covered exactly the recorded cases and groups; with them its
    // control flow may legitimately differ, and the skipped groups are reported instead.
    if(state.active&&!state.failed&&!state.foreign){
        u32 groups=0;for(u32 i=0;i<state.profile_group_count;++i)groups+=!i||read<u32>(state.profile_groups+i*16)!=read<u32>(state.profile_groups+(i-1)*16);
        if(cases!=state.profile_cases||state.groups_checked!=groups){say("REFERENCE layout differs: the run ended at ");number(cases);say(" cases and ");
            number(state.groups_checked);say(" groups, not ");number(state.profile_cases);say(" and ");number(groups);report_profile();state.failed=true;}
    }
    if(state.runs_checked||state.run_mismatches||state.runs_missing){say("harfbuzz_runs=");number(state.runs_checked);say(" mismatches=");number(state.run_mismatches);
        say(" missing=");number(state.runs_missing);say(" known_divergent=");number(state.runs_divergent);say(" foreign=");number(state.runs_foreign);say("\n");}
    say("reference_groups checked=");number(state.groups_checked);say(" skipped=");number(state.groups_skipped);say("\n");
    // MAICORD_TEXT_REFERENCE_SUMMARY names a file each process appends "<checked> <skipped>\n" to, for the suite total.
    wchar_t summary[1024];const DWORD summary_units=GetEnvironmentVariableW(L"MAICORD_TEXT_REFERENCE_SUMMARY",summary,1024);
    if(summary_units&&summary_units<1024){const HANDLE file=CreateFileW(summary,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE){char line[48];u32 n=0;const u64 values[2]{state.groups_checked,state.groups_skipped};
            for(u32 v=0;v<2;++v){char digits[24];u32 d=0;u64 x=values[v];do{digits[d++]=static_cast<char>('0'+x%10);x/=10;}while(x);while(d)line[n++]=digits[--d];line[n++]=v?'\n':' ';}
            DWORD done=0;WriteFile(file,line,n,&done,nullptr);CloseHandle(file);}}
    if(!ok||state.failed)return 1;
    return state.groups_skipped&&!state.groups_checked?77:0;
}
void reference_shaping(bool enabled) noexcept {state.unshaped=!enabled;}
void reference_session(NativeFontSession* session) noexcept {
    for(u32 i=0;i<state.session_count;++i)if(state.sessions[i]==session)return;
    if(session&&state.session_count<kSessionCapacity)state.sessions[state.session_count++]=session;
}
void reference_release_sessions() noexcept {finish_group();state.session_count=0;}
} // namespace maicord::native_app_line
