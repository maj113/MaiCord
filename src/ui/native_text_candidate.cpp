// The adapter, including the header functions it instantiates, is compiled for
// size, except retained-layout publication in native_text_styled.inc and the
// single-line entry native_text_candidate_line_once, which pop this.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

#include "maicord/core/memory.hpp"
#include "native_text_span.hpp"
#include "native_text_script.hpp"
#include "maicord/text/bidi_ltr.hpp"
#include "maicord/ui/native_text_candidate.hpp"
#include "maicord/text/nominal_sources.hpp"
#include "maicord/text/ascii_line.hpp"
#include "maicord/text/latin_line.hpp"
#include "maicord/text/latin_paragraph.hpp"
#include "maicord/text/alphabetic_run.hpp"
#include "maicord/text/script.hpp"
#include "maicord/text/emoji_paragraph.hpp"
#include "maicord/ui/emoji_presentation.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/ansi_text.hpp"
#include "maicord/text/detail/paragraph_layout.hpp"
#include "maicord/text/detail/bidi_internal.hpp"
#include "maicord/text/ascii_paragraph.hpp"
#include "maicord/text/arabic_run.hpp"
#include "maicord/text/arabic_paragraph.hpp"
#include "maicord/text/devanagari_paragraph.hpp"
#include "maicord/text/tamil_paragraph.hpp"
#include "maicord/text/cluster_paragraph.hpp"
#include "maicord/text/detail/indic_form.hpp"
#include "maicord/text/visibility.hpp"
#include "maicord/text/arabic_marks.hpp"
#include "maicord/text/unicode.hpp"
#include "maicord/text/grapheme.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/native_text_catalog.hpp"
#include "maicord/text/font_run.hpp"
#include "maicord/sys/simd.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {namespace {
constexpr u32 capacity=32;
constexpr u32 maximum_bindings=256;
constexpr u16 arabic_script=48;
constexpr u16 egyptian_script=464,runic_script=480,egyptian_missing_script=496;
// Simple scripts: native_text_script.hpp profiles, shifted by three.
constexpr u16 simple_script_first=native_simple_script_profile<<3,simple_script_end=simple_script_first+16*simple_script_count;
constexpr u16 devanagari_script=224,ethiopic_script=240,cherokee_script=256,tamil_script=272,bengali_script=288,odia_script=336,telugu_script=352,kannada_script=368,malayalam_script=384,sinhala_script=400,khmer_script=416,myanmar_script=432,tibetan_script=448;
constexpr bool form_script(u16 script) noexcept {
    return script>=bengali_script&&script<=malayalam_script&&(script-bengali_script)%16==0;
}
constexpr SfntIndicProfile form_script_profile(u16 script) noexcept {
    return static_cast<SfntIndicProfile>(static_cast<u8>(SfntIndicProfile::bengali)+(script-bengali_script)/16);
}
static_assert(form_script_profile(odia_script)==SfntIndicProfile::odia&&form_script_profile(telugu_script)==SfntIndicProfile::telugu&&form_script_profile(kannada_script)==SfntIndicProfile::kannada&&form_script_profile(malayalam_script)==SfntIndicProfile::malayalam);
// The final ID (255) is reserved for the synthetic old-Jamo profile.
constexpr u16 han_script=80,kana_script=96,hangul_script=112,hiragana_script=120,old_hangul_script=255u<<3,armenian_script=160,georgian_script=176,thai_script=192,lao_script=208;
struct NativeFormatFont {ConstSlice<u16> family;u32 scope;NativeFontProperties properties;};
// family_handle fills tail padding (sizeof stays 48). It names the first format
// owning this exact family/scope/property string.
struct NativeFormat {NativeFormatFont font;float em,tab_stop;u64 identity;bool word_wrap;u32 family_handle;};
volatile LONG64 format_identity_sequence=0;
u64 next_format_identity() noexcept {
    LONG64 previous=InterlockedCompareExchange64(&format_identity_sequence,0,0);
    for(;;){if(previous==0x7fffffffffffffffll)return 0;
        const auto observed=InterlockedCompareExchange64(&format_identity_sequence,previous+1,previous);
        if(observed==previous)return 0x8000000000000000ull|static_cast<u64>(previous+1);previous=observed;}
}
struct NativeFormatIndex {NativeFormatIndex* next;u32 offsets[64];};
static_assert(sizeof(NativeFormat)==48);
constexpr u32 emoji_font_flag=0x80000000u;
constexpr u32 binding_default=0x40000000u,binding_profile=0x80000000u,binding_face_mask=0x0fffffffu;
constexpr u32 binding_missing_family=0x10000000u;
// Profiles borrow the same retained parent plan. Only the selected profile's
// union member is active; emoji root storage belongs to the catalog arena.
struct Binding {const NativeFormat* format;NativeRunPlanHandle plan;union {NativeArabicPlanHandle arabic;NativeEmojiPlan* emoji;};NativeFontRunHandle font;u32 key,font_flags;};
constexpr usize plan_region_bytes=(sizeof(NativeRunPlanSlot)*maximum_bindings+4095u)&~usize{4095};
constexpr usize arabic_region_bytes=(sizeof(NativeArabicPlanSlot)*capacity+4095u)&~usize{4095};
// Coordinates are local to the frontend's bounded input, at most native_text_maximum_bytes.
using StyledSpan=NativeStyledSpan<Binding>;
struct AlphabeticProfile {AlphabeticProfile* next;NativeAlphabeticPlan plan;};
struct IndicProfile {IndicProfile* next;NativeIndicPlan plan;};
struct ClusterProfile {ClusterProfile* next;NativeClusterPlan plan;};
struct IndicFormProfile {IndicFormProfile* next;NativeIndicFormPlan plan;};
// Glyphs immediately follow this header in the catalog arena. The cache owns
// one native font reference and shares the immutable base-format identity.
struct NativeOmissionSign {NativeOmissionSign* next;const NativeFormat* format;NativeFontRunHandle font;float width,baseline;u32 glyphs;};
static_assert(sizeof(Binding)==40&&sizeof(StyledSpan)==24);
template<class T> Slice<T> take(Arena& arena,u32 count) noexcept {return {count?arena_allocate<T>(&arena,count):nullptr,count};}
}
struct NativeTextContext {
    NativeTextCatalogs catalogs;
    NativeFontSession* fonts;Binding* bindings;
    NativeRunPlanStore store;Arena scratch;
    NativeArabicPlanStore arabic_store;
    u32 binding_count;u64 shaped,measured,prepare_failed,execute_failed;
    // Single-line work and styled paragraph reset metadata have disjoint cache
    // lifetimes, so they share storage.
    union {NativeAsciiLineStorage last_storage;ConstSlice<u8> styled_reset;};
    NativeAsciiLineResult last_line;ConstSlice<char> last_text;
    const NativeFormat* last_format;float last_em,last_tracking;bool last_independent,last_valid,prefer_multiline;
    u64 arabic_shaped,arabic_measured;
    NativeAsciiParagraphStorage paragraph_storage;NativeParagraphShape paragraph_shape;
    Slice<NativeParagraphRun> paragraph_runs;bool last_paragraph;u32 storage_retries;
    u64 paragraphs_shaped,paragraphs_measured;
    u64 latin_shaped,latin_measured;
    ConstSlice<TextStyleSpan> last_styles;Slice<StyledSpan> styled_spans;Slice<SfntFontLineParts> styled_parts;
    float last_style_em;bool last_styled,last_fixed_weight;u8 last_mode;bool output_capacity_failed;
    Slice<NativeParagraphLine> styled_lines;
    NativeFormatIndex* formats;u32 format_count;u32 code_format;
    // Inline slot for the common first format. Its identity marks the slot;
    // overflow records never move it.
    NativeFormat first_format;u16 first_family[64];
    AlphabeticProfile* alphabetic_profiles;
    IndicProfile* indic_profiles;
    ClusterProfile* cluster_profiles[4];
    IndicFormProfile* form_profiles;
    NativeOmissionSign* omission_signs;
    Slice<u16> styled_analysis;
    // Initially self; after recovery this owns the stable binding/plan backing.
    void* backing;
};
static_assert(sizeof(ConstSlice<u8>)<=sizeof(NativeAsciiLineStorage)&&alignof(ConstSlice<u8>)<=alignof(NativeAsciiLineStorage));
namespace {
struct CatalogOperation {
    NativeTextCatalogs* owner;
    explicit CatalogOperation(NativeTextContext* context) noexcept :owner(context?&context->catalogs:nullptr) {if(owner)++owner->operation_depth;}
    ~CatalogOperation() noexcept {if(owner)--owner->operation_depth;}
};
bool output_capacity_failure(NativeTextContext& owner) noexcept {owner.output_capacity_failed=true;return false;}
constexpr usize binding_region_bytes=(sizeof(NativeTextContext)+sizeof(Binding)*maximum_bindings+4095u)&~usize{4095};
constexpr usize context_reserved_bytes=binding_region_bytes+plan_region_bytes+arabic_region_bytes;
static_assert(sizeof(NativeTextContext)%alignof(Binding)==0);
constexpr usize page_bytes(usize bytes) noexcept {return (bytes+4095u)&~usize{4095};}
static_assert(sizeof(NativeTextCatalogScope)==112&&sizeof(NativeTextContext)==2552);
static_assert(binding_region_bytes==16384&&context_reserved_bytes==40960);
static_assert(page_bytes(sizeof(NativeTextContext))==4096&&
    page_bytes(sizeof(NativeTextContext)+sizeof(Binding)*32)==4096&&
    page_bytes(sizeof(NativeTextContext)+sizeof(Binding)*64)==8192&&
    page_bytes(sizeof(NativeTextContext)+sizeof(Binding)*128)==8192);
bool tracking_valid(float value) noexcept {u32 bits=0;copy_bytes(&bits,&value,4);return (bits&0x7fffffffu)<0x7f800000u&&(!(bits&0x80000000u)||bits==0x80000000u);}
bool latin_text(Utf8View text,bool* has_controls=nullptr,bool require_letter=true,bool alphabetic=false,u8* single_script=nullptr) noexcept {
    bool letter=false;u8 scripts=0;if(single_script)*single_script=0;
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)return false;
        const auto* p=unicode_properties(cp.codepoint);const auto script=p->script();
        // ZWSP is already a source-preserving, zero-glyph paragraph control.
        // Keep otherwise Latin text on that path, including independent-letter
        // caret geometry, rather than invoking the directional editor gate.
        if(cp.codepoint==9||cp.codepoint==10||cp.codepoint==13||cp.codepoint==0x85||cp.codepoint==0x2028||cp.codepoint==0x2029||cp.codepoint==0x200b||native_text_zero_width_control(cp.codepoint)){
            if(has_controls)*has_controls=true;at+=cp.width;continue;}
        if(p->general_category()==UnicodeGeneralCategory::cc)return false;
        if(script!=UnicodeScript::latn&&script!=UnicodeScript::zyyy&&script!=UnicodeScript::zinh&&(!alphabetic||(script!=UnicodeScript::grek&&script!=UnicodeScript::cyrl)))return false;
        if(single_script){const auto value=native_script_class(cp.codepoint);if(!(value.flags&native_script_contextual))scripts|=value.script==UnicodeScript::grek?16u:value.script==UnicodeScript::cyrl?32u:1u;if(scripts&(scripts-1))return false;}
        if(cp.codepoint>127&&p->has(UnicodeFlag::emoji))return false;
        // A keycap without VS16 still receives the emoji font override.
        // U+20E3 itself has no Emoji property, so the scalar-property gate alone
        // would incorrectly shape an ASCII-base keycap in the ordinary font.
        if(cp.codepoint==0x20e3&&at){const auto previous=static_cast<u8>(text.data[at-1]);if(previous=='#'||previous=='*'||previous-'0'<10u)return false;}
        // Nonvisual script items and variation sequences require the general
        // itemizer; they are not part of one resolved Latin glyph run.
        if(p->has(UnicodeFlag::variation_selector)||(p->general_category()==UnicodeGeneralCategory::cf&&cp.codepoint!=0x200c&&cp.codepoint!=0x200d))return false;
        switch(p->bidi_class()){
            case UnicodeBidiClass::l:case UnicodeBidiClass::en:case UnicodeBidiClass::es:case UnicodeBidiClass::et:
            case UnicodeBidiClass::cs:case UnicodeBidiClass::nsm:case UnicodeBidiClass::on:case UnicodeBidiClass::ws:case UnicodeBidiClass::bn:break;
            default:return false;}
        letter|=script==UnicodeScript::latn||(alphabetic&&(script==UnicodeScript::grek||script==UnicodeScript::cyrl));at+=cp.width;
    }if(single_script&&(scripts==16||scripts==32))*single_script=scripts;return letter||!require_letter;
}
bool single_script_text(Utf8View text,UnicodeScript script) noexcept {
    bool letter=false;
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)return false;
        const auto* p=unicode_properties(cp.codepoint);const auto category=p->general_category();
        if(p->script()==script&&category>=UnicodeGeneralCategory::ll&&category<=UnicodeGeneralCategory::lu&&(script!=UnicodeScript::hani||category==UnicodeGeneralCategory::lo)&&p->bidi_class()==UnicodeBidiClass::l)letter=true;
        else if(p->script()!=UnicodeScript::zyyy||p->has(UnicodeFlag::emoji)||
            (p->bidi_class()!=UnicodeBidiClass::on&&p->bidi_class()!=UnicodeBidiClass::ws&&p->bidi_class()!=UnicodeBidiClass::cs)||
            (category!=UnicodeGeneralCategory::zs&&(category<UnicodeGeneralCategory::pc||category>UnicodeGeneralCategory::ps)))return false;
        at+=cp.width;
    }return letter;
}
bool arabic_text(Utf8View text) noexcept {
    if(!text.bytes || text.data[text.bytes-1]==' ')return false;
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)return false;
        const auto* properties=unicode_properties(cp.codepoint);
        const auto category=properties->general_category();const auto script=properties->script();
        const bool letter=(category==UnicodeGeneralCategory::lo || category==UnicodeGeneralCategory::lm) && properties->bidi_class()==UnicodeBidiClass::al && (script==UnicodeScript::arab || cp.codepoint==0x640);
        const bool mark=(script==UnicodeScript::arab || script==UnicodeScript::zinh || script==UnicodeScript::zyyy) && sfnt_arabic_combining_mark(cp.codepoint);
        if(!at && (!letter || script!=UnicodeScript::arab))return false;
        if(!letter && cp.codepoint!=32 && cp.codepoint!=0x200c && cp.codepoint!=0x200d && !mark)return false;
        at+=cp.width;
    }
    return true;
}
bool hebrew_text(Utf8View text) noexcept {
    if(!text.bytes)return false;
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)return false;const auto* p=unicode_properties(cp.codepoint);
        const bool letter=p->script()==UnicodeScript::hebr&&p->bidi_class()==UnicodeBidiClass::r;
        const bool mark=p->general_category()==UnicodeGeneralCategory::mn&&(p->script()==UnicodeScript::hebr||p->script()==UnicodeScript::zinh);
        if(!at&&!letter)return false;if(!letter&&!mark&&cp.codepoint!=32&&cp.codepoint!=0x200c&&cp.codepoint!=0x200d)return false;at+=cp.width;}
    return true;
}
bool itemized_text(Utf8View text,bool* has_emoji=nullptr) noexcept {
    bool hebrew=false,symbols=false,scanned=false,paired=true,common=true,non_ascii=false;u32 closing[61],depth=0;
    u16 previous_script=256;u8 profile=0;
    TextEmojiCursor cursor{};TextEmojiRange range{};auto next=TextEmojiNext::end;if(has_emoji)*has_emoji=false;
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)return false;
        const auto* p=unicode_properties(cp.codepoint);const auto script=p->script();
        if(!scanned&&p->has(UnicodeFlag::emoji)){next=text_emoji_next(text,&cursor,&range);scanned=true;}
        if(next==TextEmojiNext::invalid_input)return false;
        if(next==TextEmojiNext::range&&at==range.first_byte){at=range.end_byte;if(has_emoji)*has_emoji=true;next=text_emoji_next(text,&cursor,&range);continue;}
        if(static_cast<u8>(script)!=previous_script){profile=native_text_script_profile(script,false);if(!profile)return false;previous_script=static_cast<u8>(script);
            hebrew|=profile==8||profile==6;symbols|=profile>8&&profile!=16;}
        // Standalone non-ASCII punctuation has no Latin letter to enter the
        // Latin frontend. Resolve its Common script and font fallback normally.
        common=common&&(script==UnicodeScript::zyyy||script==UnicodeScript::zzzz);non_ascii|=cp.codepoint>127;
        const auto category=p->general_category();
        symbols=symbols||script==UnicodeScript::brai||script==UnicodeScript::zzzz||cp.codepoint==0x200b||native_text_bidi_format(cp.codepoint)||p->has(UnicodeFlag::variation_selector)||(cp.codepoint>127&&(p->has(UnicodeFlag::emoji)||
            (script==UnicodeScript::zyyy&&(category==UnicodeGeneralCategory::sm||category==UnicodeGeneralCategory::sc||category==UnicodeGeneralCategory::so))));
        if((p->general_category()==UnicodeGeneralCategory::cf&&cp.codepoint!=0x200b&&cp.codepoint!=0x200c&&cp.codepoint!=0x200d&&!native_text_bidi_format(cp.codepoint))||
           (p->general_category()==UnicodeGeneralCategory::cc&&cp.codepoint!=9&&cp.codepoint!=10&&cp.codepoint!=13))return false;
        if(cp.codepoint==0x20e3&&at){const auto previous=static_cast<u8>(text.data[at-1]);if(previous=='#'||previous=='*'||previous-'0'<10u)return false;}
        // Hebrew with unmatched bracket pairs is rejected here and keeps the
        // general renderer route.
        if(paired&&p->has(UnicodeFlag::bidi_mirrored)){const auto bracket=unicode_bidi_bracket(cp.codepoint);
            if(bracket.type==UnicodeBracketType::open){if(depth==61)paired=false;else closing[depth++]=bracket.paired;}
            else if(bracket.type==UnicodeBracketType::close){if(!depth||closing[depth-1]!=cp.codepoint)paired=false;else --depth;}}
        at+=cp.width;
    }return (hebrew||symbols||(common&&non_ascii))&&(!hebrew||(paired&&!depth));
}
SfntStatus arabic_line(NativeTextContext& owner,const Binding& binding,Utf8View text,bool independent,float em,NativeAsciiLineStorage& buffers,NativeAsciiLineResult* output) noexcept {
    SfntShapeRunResult run{};auto& storage=buffers.run.shaping;auto status=native_arabic_run(owner.arabic_store,binding.arabic,text,independent,true,em,storage,&run);
    if(status!=SfntStatus::ok)return status;NativeAsciiLineResult line{};
    const auto* metrics=native_run_plan_metrics(owner.store,binding.plan);if(!metrics)return SfntStatus::invalid_input;
    status=sfnt_single_font_line(*metrics,em,&line.vertical);if(status!=SfntStatus::ok)return status;
    if(!run.glyph_count || !run.source_units || storage.character_glyphs[0])return SfntStatus::unsupported_sequence;
    for(u32 i=0;i<run.glyph_count;++i)if(!storage.glyphs[i].glyph.glyph)return SfntStatus::unsupported_sequence;
    float pen=0,width=0;
    for(u32 at=0;at<run.source_units;){const u32 first=storage.character_glyphs[at];u32 end=at+1;while(end<run.source_units && storage.character_glyphs[end]==first)++end;
        const u32 last=end<run.source_units?storage.character_glyphs[end]:run.glyph_count;if(last<=first || last>run.glyph_count)return SfntStatus::unsupported_sequence;
        float cluster=0;for(u32 glyph=first;glyph<last;++glyph){const float advance=storage.placements[glyph].advance;if(!(advance>=0))return SfntStatus::unsupported_sequence;cluster+=advance;pen+=advance;}
        width+=cluster;at=end;
    }
    if(!(width>=0) || width>1000000 || !(pen>=0) || pen>1000000)return SfntStatus::resource_limit;
    line.run={run.glyph_count,run.source_units,0,1,run.lookup_operations};line.width=line.width_including_trailing=width;line.caret_end=pen;line.geometry_flags=native_line_ordered_x;
    buffers.run.character_glyphs=storage.character_glyphs;buffers.run.placements=storage.placements;*output=line;return SfntStatus::ok;
}
const NativeFormat* format_at(const NativeTextContext&,u32) noexcept;
NativeTextFontQuery font_query(const NativeTextContext& owner,const NativeFormat* format,u32 font_flags) noexcept {
    NativeTextFontQuery query{format->font.scope,format->font.family,format->font.properties};
    if(font_flags&text_style_bold)query.properties.weight=600;
    if(font_flags&text_style_italic)query.properties.style=2;
    static constexpr u16 mono[]{'C','o','n','s','o','l','a','s'};
    if(font_flags&text_style_monospace){
        const auto* code=owner.code_format?format_at(owner,owner.code_format):nullptr;
        if(code){query.scope=code->font.scope;query.family=code->font.family;}
        else query.family={mono,8};
    }
    static constexpr u16 emoji[]{'S','e','g','o','e',' ','U','I',' ','E','m','o','j','i'};
    // Emoji spans use Segoe UI Emoji at regular weight and style; the requested stretch is kept.
    if(font_flags==emoji_font_flag){query.scope=0;query.family={emoji,14};query.properties.weight=400;query.properties.style=0;}
    return query;
}
bool acquire_plan(NativeTextContext& owner,const NativeFormat* format,u32 font_flags,NativeRunPlanHandle* output,NativeFontRunHandle* font,u32* key,const NativeTextFontRange* face=nullptr) noexcept {
    NativeFontInstanceHandle instance{};NativeFontSelection source{};const auto query=font_query(owner,format,font_flags);u32 candidate=0xffffffffu,simulations=0,scope=query.scope;bool missing_family=false;
    if(face){candidate=face->match.candidate;simulations=face->match.simulations;scope=face->scope;if(!native_text_catalog_select_face(&owner.catalogs,scope,candidate,simulations,owner.fonts,&owner.scratch,&instance,&source))return false;}
    else if(!native_text_catalog_select_query(&owner.catalogs,query,owner.fonts,&owner.scratch,&instance,&source,&candidate,&simulations,&scope,&missing_family))return false;
    struct Release {NativeFontInstanceStore* store;NativeFontInstanceHandle instance;~Release() noexcept {native_font_instance_release(store,instance);}} release{owner.store.instances,instance};
    if(scope>=32||candidate>0x1fffffu||simulations>3)return false;*key=(simulations<<26)|(scope<<21)|candidate|(missing_family?binding_missing_family:0);
    const auto prepared=native_run_plan_acquire(&owner.store,instance,&owner.scratch,output,1000000,16*1024*1024,simulations,font_flags==emoji_font_flag?NativeRunPlanKind::emoji:NativeRunPlanKind::text);
    if(!prepared.ok()){if(prepared.error==NativeFontError::capacity)++owner.catalogs.retryable_failures;return false;}
    const auto recorded=native_font_session_run(owner.fonts,instance,source,simulations,font);if(recorded.ok())return true;
    if(recorded.error==NativeFontError::capacity)++owner.catalogs.retryable_failures;
    native_run_plan_release(&owner.store,*output);reset_value(output);return false;
}
bool same_font(const NativeFormatFont& a,const NativeFormatFont& b) noexcept {
    return a.scope==b.scope&&bytes_equal(&a.properties,&b.properties,sizeof(a.properties))&&a.family.count==b.family.count&&bytes_equal(a.family.data,b.family.data,a.family.count*2);
}
bool binding_capacity(NativeTextContext& owner) noexcept {
    if(owner.binding_count<owner.store.slots.count)return true;
    if(owner.binding_count==maximum_bindings)return false;
    const u32 count=minimum(owner.store.slots.count+capacity,maximum_bindings);
    // Stable addresses preserve retained span pointers and borrowed plan data.
    // Reserve once; only commit the prefix needed by actual font bindings.
    if(!VirtualAlloc(owner.store.slots.data,sizeof(NativeRunPlanSlot)*count,MEM_COMMIT,PAGE_READWRITE)||
       !VirtualAlloc(owner.bindings,sizeof(Binding)*count,MEM_COMMIT,PAGE_READWRITE))return false;
    owner.store.slots.count=count;return true;
}
Binding* binding(NativeTextContext& owner,const NativeFormat* format,u32 font_flags=0) noexcept {
    for(u32 i=0;i<owner.binding_count;++i)if((owner.bindings[i].key&binding_default)&&owner.bindings[i].font_flags==font_flags&&(owner.bindings[i].format==format||
        (font_flags==emoji_font_flag?owner.bindings[i].format->font.properties.width==format->font.properties.width:same_font(owner.bindings[i].format->font,format->font))))return owner.bindings[i].plan.slot?owner.bindings+i:nullptr;
    if(!binding_capacity(owner))return nullptr;
    owner.last_valid=false;owner.prefer_multiline=false;
    const u32 failures=owner.catalogs.retryable_failures;
    auto& slot=owner.bindings[owner.binding_count++];slot.format=format;slot.font_flags=font_flags;
    const bool ready=acquire_plan(owner,format,font_flags,&slot.plan,&slot.font,&slot.key);slot.key|=binding_default;
    if(!ready){++owner.prepare_failed;if(owner.catalogs.retryable_failures!=failures){reset_value(&slot);--owner.binding_count;}return nullptr;}
    return &slot;
}
Binding* binding_face(NativeTextContext& owner,const NativeFormat* format,u32 flags,const NativeTextFontRange& face) noexcept {
    if(face.match.candidate>0x1fffffu||face.scope>=32||face.match.simulations>3)return nullptr;
    const u32 key=(face.match.simulations<<26)|(face.scope<<21)|face.match.candidate;
    // An explicit fallback face is independent of the requesting family.
    // Default bindings retain family identity for nominal-layout policy.
    for(u32 i=0;i<owner.binding_count;++i){auto& slot=owner.bindings[i];if((slot.key&binding_face_mask)==key&&slot.font_flags==flags&&
        (!(slot.key&binding_default)||same_font(slot.format->font,format->font)))return slot.plan.slot?&slot:nullptr;}
    if(!binding_capacity(owner))return nullptr;owner.last_valid=false;owner.prefer_multiline=false;const u32 failures=owner.catalogs.retryable_failures;
    auto& slot=owner.bindings[owner.binding_count++];slot.format=format;slot.font_flags=flags;slot.key=key;
    if(!acquire_plan(owner,format,flags,&slot.plan,&slot.font,&slot.key,&face)){++owner.prepare_failed;if(owner.catalogs.retryable_failures!=failures){reset_value(&slot);--owner.binding_count;}return nullptr;}return &slot;
}
bool emoji_profile(NativeTextContext& owner,Binding& selected) noexcept {
    if(selected.font_flags!=emoji_font_flag)return false;
    if(!(selected.key&binding_profile)){selected.key|=binding_profile;selected.emoji=nullptr;owner.last_valid=false;owner.prefer_multiline=false;
        auto& arena=owner.catalogs.storage;const auto mark=arena_mark(&arena);auto* plan=arena_allocate<NativeEmojiPlan>(&arena);
        if(!plan||native_emoji_plan_prepare(owner.store,selected.plan,&arena,&owner.scratch,plan)!=SfntStatus::ok){arena_rewind(&arena,mark);++owner.prepare_failed;return false;}
        selected.emoji=plan;}
    return selected.emoji!=nullptr;
}
const NativeAlphabeticPlan* alphabetic_profile(NativeTextContext& owner,const Binding& selected,u16 script,bool independent) noexcept {
    const bool allow_missing=script==egyptian_missing_script;
    u32 tag=0;switch(script){
        case 8:tag=sfnt_tag('l','a','t','n');break;case 16:tag=sfnt_tag('g','r','e','k');break;
        case 32:tag=sfnt_tag('c','y','r','l');break;case 64:tag=sfnt_tag('h','e','b','r');break;
        case han_script:tag=sfnt_tag('h','a','n','i');break;case hiragana_script:case kana_script:tag=sfnt_tag('k','a','n','a');break;
        case hangul_script:tag=sfnt_tag('h','a','n','g');break;case old_hangul_script:tag=sfnt_tag('j','a','m','o');break;
        case armenian_script:tag=sfnt_tag('a','r','m','n');break;case georgian_script:tag=sfnt_tag('g','e','o','r');break;
        case ethiopic_script:tag=sfnt_tag('e','t','h','i');break;case cherokee_script:tag=sfnt_tag('c','h','e','r');break;
        case egyptian_script:case egyptian_missing_script:tag=sfnt_tag('e','g','y','p');break;case runic_script:tag=sfnt_tag('r','u','n','r');break;
        case thai_script:tag=sfnt_tag('t','h','a','i');break;case lao_script:tag=sfnt_tag('l','a','o',' ');break;
        case 128:break;default:if(script<simple_script_first||script>=simple_script_end||script%16)return nullptr;
            tag=simple_script_tags.values[(script-simple_script_first)/16];
    }
    for(auto* p=owner.alphabetic_profiles;p;p=p->next)if(p->plan.run.slot==selected.plan.slot&&p->plan.run.generation==selected.plan.generation&&p->plan.script_tag==tag&&p->plan.independent==independent&&p->plan.allow_missing==allow_missing)return &p->plan;
    auto& arena=owner.catalogs.storage;const auto mark=arena_mark(&arena);auto* p=arena_allocate<AlphabeticProfile>(&arena);
    if(!p||native_alphabetic_plan_prepare(owner.store,selected.plan,tag,independent,&arena,&owner.scratch,&p->plan)!=SfntStatus::ok){arena_rewind(&arena,mark);++owner.prepare_failed;return nullptr;}
    p->plan.allow_missing=allow_missing;p->next=owner.alphabetic_profiles;owner.alphabetic_profiles=p;return &p->plan;
}
NativeAsciiLineStorage storage(Arena&,u32,u32,bool,bool) noexcept;
const NativeIndicPlan* indic_profile(NativeTextContext& owner,const Binding& selected,SfntIndicProfile script) noexcept {
    for(auto* p=owner.indic_profiles;p;p=p->next)if(p->plan.run.slot==selected.plan.slot&&p->plan.run.generation==selected.plan.generation&&p->plan.profile==script)return p->plan.cmap?&p->plan:nullptr;
    auto& arena=owner.catalogs.storage;const auto mark=arena_mark(&arena);auto* p=arena_allocate<IndicProfile>(&arena);
    const auto status=p?native_indic_plan_prepare(owner.store,selected.plan,script,&arena,&owner.scratch,{},&p->plan):SfntStatus::resource_limit;
    if(status!=SfntStatus::ok){++owner.prepare_failed;
        if(p&&status==SfntStatus::unsupported_sequence){p->plan.run=selected.plan;p->plan.profile=script;p->next=owner.indic_profiles;owner.indic_profiles=p;}
        else arena_rewind(&arena,mark);return nullptr;}
    p->next=owner.indic_profiles;owner.indic_profiles=p;return &p->plan;
}
SfntStatus tamil_run(NativeTextContext& owner,const Binding& binding,Utf8View text,float em,const NativeAsciiStorage& s,NativeAsciiResult* out,float spacing,bool rtl) noexcept {
    const auto* profile=indic_profile(owner,binding,SfntIndicProfile::tamil);if(!profile)return SfntStatus::unsupported_sequence;
    return native_tamil_run(owner.store,*profile,text,em,s,out,{},spacing,rtl);
}
// Cluster-shaper plans are cached per script, font plan generation and letter
// policy. A face without the script's features is cached as unsupported, so a
// neutral fallback span returning to it does not prepare it again; capacity and
// work failures stay retryable.
const NativeClusterPlan* cluster_profile(NativeTextContext& owner,NativeClusterScript script,const Binding& selected,bool independent) noexcept {
    auto*& profiles=owner.cluster_profiles[static_cast<u32>(script)];
    for(auto* p=profiles;p;p=p->next)if(p->plan.run.slot==selected.plan.slot&&p->plan.run.generation==selected.plan.generation&&p->plan.independent_letters==independent)return p->plan.cmap?&p->plan:nullptr;
    auto& arena=owner.catalogs.storage;const auto mark=arena_mark(&arena);auto* p=arena_allocate<ClusterProfile>(&arena);
    const auto status=p?native_cluster_plan_prepare(script,owner.store,selected.plan,independent,&arena,&owner.scratch,&p->plan):SfntStatus::resource_limit;
    if(status!=SfntStatus::ok){++owner.prepare_failed;
        if(p&&status==SfntStatus::unsupported_sequence){p->plan.run=selected.plan;p->plan.independent_letters=independent;p->next=profiles;profiles=p;}
        else arena_rewind(&arena,mark);return nullptr;}
    p->next=profiles;profiles=p;return &p->plan;
}
// Sinhala, Khmer, Myanmar and Tibetan have consecutive script ids.
constexpr NativeClusterScript cluster_script(u16 script) noexcept {return static_cast<NativeClusterScript>((script-sinhala_script)/16);}
static_assert(cluster_script(sinhala_script)==NativeClusterScript::sinhala&&cluster_script(khmer_script)==NativeClusterScript::khmer&&
    cluster_script(myanmar_script)==NativeClusterScript::myanmar&&cluster_script(tibetan_script)==NativeClusterScript::tibetan);
SfntStatus cluster_run(NativeTextContext& owner,NativeClusterScript script,const Binding& binding,Utf8View text,float em,const NativeAsciiStorage& s,NativeAsciiResult* out,float spacing,bool rtl) noexcept {
    const auto* profile=cluster_profile(owner,script,binding,owner.last_independent);if(!profile)return SfntStatus::unsupported_sequence;
    return native_cluster_run(script,owner.store,*profile,text,em,s,out,{},rtl,spacing);
}
const NativeIndicFormPlan* form_profile(NativeTextContext& owner,const Binding& selected,SfntIndicProfile script) noexcept {
    for(auto* p=owner.form_profiles;p;p=p->next)if(p->plan.indic.run.slot==selected.plan.slot&&p->plan.indic.run.generation==selected.plan.generation&&p->plan.indic.profile==script)return p->plan.indic.cmap?&p->plan:nullptr;
    auto& arena=owner.catalogs.storage;const auto mark=arena_mark(&arena),temporary=arena_mark(&owner.scratch);
    auto* p=arena_allocate<IndicFormProfile>(&arena);const auto probe=storage(owner.scratch,64,128,false,false);
    const auto status=p?native_indic_plan_prepare(owner.store,selected.plan,script,&arena,&owner.scratch,probe.run.shaping,&p->plan.indic,{p->plan.forms,128}):SfntStatus::resource_limit;
    arena_rewind(&owner.scratch,temporary);
    if(status!=SfntStatus::ok){++owner.prepare_failed;
        if(p&&status==SfntStatus::unsupported_sequence){p->plan.indic.run=selected.plan;p->plan.indic.profile=script;p->next=owner.form_profiles;owner.form_profiles=p;}
        else arena_rewind(&arena,mark);return nullptr;}
    p->next=owner.form_profiles;owner.form_profiles=p;return &p->plan;
}
SfntStatus form_run(SfntIndicProfile script,NativeTextContext& owner,const Binding& binding,Utf8View text,float em,const NativeAsciiStorage& s,NativeAsciiResult* out,float spacing,bool rtl) noexcept {
    const auto* profile=form_profile(owner,binding,script);if(!profile)return SfntStatus::unsupported_sequence;
    return indic_owned_run(script,owner.store,*profile,text,em,s,out,{},rtl,spacing);
}
SfntStatus devanagari_run(NativeTextContext& owner,const Binding& binding,Utf8View text,float em,const NativeAsciiStorage& s,NativeAsciiResult* out,float spacing=0,bool rtl=false) noexcept {
    const auto* profile=form_profile(owner,binding,SfntIndicProfile::devanagari);if(!profile)return SfntStatus::unsupported_sequence;
    const auto status=native_devanagari_run(owner.store,*profile,text,em,{s.shaping,s.glyphs,s.placements,s.character_glyphs},out,{},spacing,rtl);
    if(status!=SfntStatus::ok)return status;
    for(u32 i=0;i<out->glyph_count;++i)if(!s.glyphs[i]){reset_value(out);return SfntStatus::unsupported_sequence;}return SfntStatus::ok;
}
bool prepare_script_profile(NativeTextContext& owner,Binding& selected,u16 script,bool independent) noexcept {
    if(form_script(script))return selected.font_flags!=emoji_font_flag&&form_profile(owner,selected,form_script_profile(script))!=nullptr;
    if(script>=sinhala_script&&script<=tibetan_script&&!(script%16))return selected.font_flags!=emoji_font_flag&&cluster_profile(owner,cluster_script(script),selected,independent)!=nullptr;
    if(script==tamil_script)return selected.font_flags!=emoji_font_flag&&indic_profile(owner,selected,SfntIndicProfile::tamil)!=nullptr;
    if(script==devanagari_script)return selected.font_flags!=emoji_font_flag&&form_profile(owner,selected,SfntIndicProfile::devanagari)!=nullptr;
    if(script!=arabic_script)return !script||alphabetic_profile(owner,selected,script,independent)!=nullptr;
    if(selected.font_flags==emoji_font_flag)return false;
    if(!owner.arabic_store.slots.count){
        if(!VirtualAlloc(owner.arabic_store.slots.data,arabic_region_bytes,MEM_COMMIT,PAGE_READWRITE))return false;
        owner.arabic_store.slots.count=capacity;
    }
    if(!(selected.key&binding_profile)){owner.last_valid=false;owner.prefer_multiline=false;selected.key|=binding_profile;
        if(!native_arabic_plan_acquire(&owner.arabic_store,selected.plan,&owner.scratch,&selected.arabic).ok()){++owner.prepare_failed;return false;}}
    return selected.arabic.slot!=0;
}
const NativeFormat* format_at(const NativeTextContext& owner,u32 handle) noexcept {
    if(!handle||handle>owner.format_count)return nullptr;
    if(handle==1&&owner.first_format.identity)return &owner.first_format;
    --handle;const auto* page=owner.formats;
    for(u32 n=handle/64;n;--n)page=page->next;
    return reinterpret_cast<const NativeFormat*>(owner.catalogs.storage.base+page->offsets[handle%64]);
}
u32 append_format(NativeTextContext& owner,const NativeFormat& value,NativeFormatIndex*& page,
                  const u16* shared_name,u32 shared_handle) noexcept {
    if(!owner.format_count&&value.font.family.count<=64){
        const auto identity=next_format_identity();if(!identity)return 0;
        copy_bytes(owner.first_family,value.font.family.data,value.font.family.count*2);
        owner.first_format=value;owner.first_format.identity=identity;owner.first_format.font.family.data=owner.first_family;owner.first_format.family_handle=1;
        return ++owner.format_count;
    }
    auto& arena=owner.catalogs.storage;const auto mark=arena_mark(&arena);const bool grow=!owner.formats||!(owner.format_count%64);
    auto* index=grow?arena_allocate_zero<NativeFormatIndex>(&arena):page;
    auto* saved=arena_allocate<NativeFormat>(&arena);auto* family=shared_name?nullptr:arena_allocate<u16>(&arena,value.font.family.count);
    if(!index||!saved||(!shared_name&&!family)){arena_rewind(&arena,mark);return 0;}*saved=value;
    saved->identity=next_format_identity();if(!saved->identity){arena_rewind(&arena,mark);return 0;}
    if(family)copy_bytes(family,value.font.family.data,value.font.family.count*2);saved->font.family.data=shared_name?shared_name:family;saved->family_handle=shared_name?shared_handle:owner.format_count+1;
    if(grow){if(page)page->next=index;else owner.formats=index;}page=index;
    index->offsets[owner.format_count%64]=static_cast<u32>(reinterpret_cast<u8*>(saved)-arena.base);return ++owner.format_count;
}
u32 intern_format(NativeTextContext& owner,const NativeFormat& value) noexcept {
    if(!value.font.family.data||!value.font.family.count||value.font.family.count>=256||value.font.scope>=owner.catalogs.count||
       !(value.em>0)||value.em>1000000||!(value.tab_stop>0)||value.tab_stop>1000000)return 0;
    const NativeFormat* shared=nullptr;auto* page=owner.formats;
    for(u32 i=0;i<owner.format_count;++i){if(i&&!(i%64))page=page->next;
        const auto& old=!i&&owner.first_format.identity?owner.first_format:*reinterpret_cast<const NativeFormat*>(owner.catalogs.storage.base+page->offsets[i%64]);if(same_font(old.font,value.font)){
            if(old.em==value.em&&old.tab_stop==value.tab_stop&&old.word_wrap==value.word_wrap)return i+1;shared=&old;}}
    // Canonical-family metadata is needed only for a new declaration, never
    // for a hit in the existing format table.
    return append_format(owner,value,page,shared?shared->font.family.data:nullptr,shared?shared->family_handle:0);
}
const NativeFormat* resolve_format(const TextSystem& system,TextWrapping wrapping) noexcept {
    const auto* value=format_at(*system.native_text,system.native_format);if(!value)return nullptr;
    return wrapping==TextWrapping::wrap&&!value->word_wrap?nullptr:value;
}
NativeAsciiLineStorage storage(Arena& arena,u32 source,u32 glyphs,bool arabic=false,bool full_actions=false) noexcept {
    NativeAsciiLineStorage result{};auto& out=result.run;
    if(!arabic)result.clusters=take<NativeLineCluster>(arena,source);auto& s=out.shaping;
    s.glyphs=take<SfntComponentGlyph>(arena,glyphs);s.components=take<SfntComponentNode>(arena,source);s.cluster_bits=take<u64>(arena,(source+63)/64*2);
    s.character_glyphs=take<u32>(arena,source);s.targets=take<u32>(arena,glyphs);s.associations=take<SfntMarkAssociation>(arena,glyphs);s.advances=take<double>(arena,glyphs);
    s.flags=take<u8>(arena,glyphs);s.positions=take<SfntPosition>(arena,glyphs);s.placements=take<SfntPlacement>(arena,glyphs);
    // Nested match positions and component undo are reused after each edit.
    // The first attempt caps this at 1024; the retry (full_actions) restores
    // the source-scaled capacity.
    const u32 actions=full_actions?maximum<u32>(256,source*16):minimum(maximum<u32>(256,source*16),1024u);
    s.substitutions={take<SfntGsubExecutionFrame>(arena,64),take<u32>(arena,actions),take<u32>(arena,actions),take<u32>(arena,glyphs),
        take<u8>(arena,glyphs*sizeof(SfntComponentGlyph)),take<SfntComponentUndo>(arena,actions)};
    s.positioning={take<SfntGposExecutionFrame>(arena,64),take<u32>(arena,actions),take<u64>(arena,(glyphs+63)/64),take<double>(arena,glyphs),take<SfntCursiveChainNode>(arena,glyphs)};
    if(!arabic){out.mapped={reinterpret_cast<SfntClusterGlyph*>(s.positions.data),source};out.glyphs={reinterpret_cast<u16*>(s.glyphs.data),glyphs};out.placements=s.placements;out.character_glyphs=s.character_glyphs;}
    return result;
}
bool retain_runs(NativeFontSession* fonts,NativeFontRunHandle font,u32 count,ShapedText* output) noexcept {
    if(output->native_fonts()||output->run_count||!native_font_session_retain(fonts))return false;
    auto* store=native_font_session_runs(fonts);u32 retained=0;while(retained<count&&native_font_run_retain(store,font))++retained;
    if(retained!=count){while(retained--)native_font_run_release(store,font);native_font_session_release(&fonts);return false;}
    output->set_native_fonts(fonts);return true;
}
const NativeOmissionSign* omission_sign(const NativeTextContext& owner,const NativeFormat* format) noexcept {
    for(auto* sign=owner.omission_signs;sign;sign=sign->next)
        if(sign->format==format||(sign->format->em==format->em&&same_font(sign->format->font,format->font)))return sign;
    return nullptr;
}
bool prepare_omission_sign(NativeTextContext& owner,const TextSystem& system,const NativeFormat* format) noexcept {
    // Overflow in the sign's own scratch must not leave output_capacity_failed set for the caller.
    struct RestoreFailure {bool& flag;bool previous;~RestoreFailure() noexcept {flag=previous;}} restore_failure{owner.output_capacity_failed,owner.output_capacity_failed};
    // One UTF-8 scalar fits the producer's 3*4+16 glyph retry bound.
    TextGlyph glyphs[32]{};TextRun runs[4]{};u16 clusters[1]{};ShapedText shaped{};
    shaped.glyphs=glyphs;shaped.glyph_capacity=32;shaped.runs=runs;shaped.run_capacity=4;shaped.clusters=clusters;shaped.cluster_capacity=1;
    struct Release {ShapedText& text;~Release() noexcept {
        auto* session=text.native_fonts();if(!session)return;
        for(u32 i=0;i<text.run_count;++i)native_font_run_release(native_font_session_runs(session),text.runs[i].native_face);
        native_font_session_release(&session);
    }} release{shaped};
    auto plain=system;plain.native_format=intern_format(owner,*format);
    plain.independent_letters=false;plain.tracking=0;
    if(!plain.native_format||!native_text_candidate_line(plain,utf8_literal("\xe2\x80\xa6"),1000000,1000000,&shaped,nullptr)||
        shaped.run_count!=1||!shaped.glyph_count||!shaped.runs[0].native_font||shaped.clusters[0])return false;
    const auto font=shaped.runs[0].native_face;auto* store=native_font_session_runs(owner.fonts);
    auto& arena=owner.catalogs.storage;const auto mark=arena_mark(&arena);
    auto* saved=arena_allocate<NativeOmissionSign>(&arena);auto* payload=take<TextGlyph>(arena,shaped.glyph_count).data;
    if(!saved||!payload||!native_font_run_retain(store,font)){arena_rewind(&arena,mark);return false;}
    *saved={owner.omission_signs,format,font,shaped.width,shaped.runs[0].baseline_y,shaped.glyph_count};
    copy_bytes(payload,glyphs,shaped.glyph_count*sizeof(TextGlyph));owner.omission_signs=saved;return true;
}
struct NativeTrimPlanScratch {
    Arena* arena=nullptr;ArenaMark mark{};Slice<NativeParagraphLineFit> fit{};
    ~NativeTrimPlanScratch() noexcept {if(arena)arena_rewind(arena,mark);}
    bool allocate(NativeTextContext& owner,u32 clusters) noexcept {
        arena=&owner.scratch;mark=arena_mark(arena);fit=take<NativeParagraphLineFit>(*arena,clusters+1);return fit.data!=nullptr;
    }
};
struct RtlLineOutput;
bool trim_single_line(NativeTextContext& owner,const Binding* binding,const TextSystem& system,const NativeFormat* format,
    Utf8View text,float width,float height,TextWrapping wrapping,const NativeParagraphLayout& complete,
    ShapedText* output,TextMeasureResult* measure,RtlLineOutput* directional=nullptr,ConstSlice<TextHitRange> hits={}) noexcept;
NativeLatinBoundaryStorage boundary_storage(Slice<NativeParagraphRun> runs) noexcept {
    // Runs are written after analysis. A run slot covers a scalar plus its
    // boundary byte, including the extra terminal boundary at capacity one.
    // Derive both borrowed views from the allocation, never the UTF-8 length.
    static_assert(sizeof(NativeParagraphRun)>=sizeof(u32)+2);
    if(!runs.count||!runs.data)return {};
    auto* scalars=reinterpret_cast<u32*>(runs.data);
    return {{scalars,runs.count},{reinterpret_cast<u8*>(scalars+runs.count),runs.count+1}};
}
bool paragraph(NativeTextContext& owner,const Binding& binding,const TextSystem& system,const NativeFormat* format,Utf8View text,float width,float height,
    TextWrapping wrapping,TextOverflow overflow,ShapedText* output,TextMeasureResult* measure,NativeParagraphHitStorage* hits=nullptr,bool latin=false,const NativeAlphabeticPlan* profile=nullptr,Slice<NativeParagraphLineFit> fit={},NativeParagraphLayout* planning=nullptr,NativeTrimPlanScratch* plan_storage=nullptr) noexcept {
    const float em=format->em;
    if(overflow==TextOverflow::ellipsis&&!(height>0))return false;
    if(owner.last_valid&&!owner.last_styled&&owner.last_paragraph&&owner.last_format==format&&owner.last_em==em&&owner.last_independent==system.independent_letters&&owner.last_tracking==system.tracking&&
       owner.last_text.count==text.bytes&&bytes_equal(owner.last_text.data,text.data,text.bytes)){}
    else if(owner.last_valid&&!owner.last_styled&&!owner.last_paragraph&&owner.last_format==format&&owner.last_em==em&&owner.last_independent==system.independent_letters&&owner.last_tracking==system.tracking&&owner.last_text.count==text.bytes&&bytes_equal(owner.last_text.data,text.data,text.bytes)){
        // Reuse the shaped printable line; only cluster/run descriptors are appended.
        owner.last_valid=false;owner.prefer_multiline=false;const auto& line=owner.last_line;const auto& source=owner.last_storage;
        NativeAsciiParagraphStorage s{};s.glyphs=source.run.glyphs;s.placements=source.run.placements;s.source_glyphs=source.run.character_glyphs;
        s.clusters=take<NativeParagraphCluster>(owner.scratch,line.cluster_count);auto runs=take<NativeParagraphRun>(owner.scratch,line.run.source_units);if(!s.clusters.data||!runs.data)return false;
        u32 first=0;for(u32 i=0;i<line.cluster_count;++i){const auto& c=source.clusters[i];const u32 g=source.run.character_glyphs[first],end=c.source_end==line.run.source_units?line.run.glyph_count:source.run.character_glyphs[c.source_end];
            s.clusters[i]={first,c.source_end,g,end-g,c.width,0};first=c.source_end;}
        if(!latin)native_ascii_paragraph_flags(text,{s.clusters.data,line.cluster_count});
        else if(native_latin_paragraph_boundaries(text,{s.clusters.data,line.cluster_count},boundary_storage(runs))!=SfntStatus::ok){++owner.execute_failed;return false;}
        owner.paragraph_storage=s;owner.paragraph_shape={line.run.source_units,line.run.glyph_count,line.cluster_count,line.vertical};owner.paragraph_runs=runs;owner.last_paragraph=true;owner.last_styled=false;owner.last_valid=true;
    }
    else{
        owner.last_valid=false;owner.prefer_multiline=false;u32 longest=0,segment=0,printable=0,units=latin?0:text.bytes;
        if(!latin)for(u32 i=0;i<text.bytes;++i){if(text.data[i]>=32){++segment;++printable;longest=maximum(longest,segment);}else segment=0;}
        else for(u32 i=0;i<text.bytes;){const auto cp=utf8_decode_one(text.data+i,text.bytes-i);if(!cp.valid)return false;
            if(cp.codepoint==9||cp.codepoint==10||cp.codepoint==13||cp.codepoint==0x85||cp.codepoint==0x2028||cp.codepoint==0x2029)segment=0;
            else{segment+=cp.width;printable+=cp.width;longest=maximum(longest,segment);}units+=cp.codepoint>65535?2u:1u;i+=cp.width;}
        NativeAsciiParagraphStorage s{};NativeParagraphShape shape{};Slice<char> saved{};Slice<NativeParagraphRun> runs{};SfntStatus status{};
        for(u32 attempt=0;attempt<2;++attempt){
            arena_reset(&owner.scratch);const u32 factor=attempt?4u:1u;const u32 glyphs=printable?printable*factor+16:0;
            saved=take<char>(owner.scratch,text.bytes);if(!saved.data)return false;
            reset_value(&s);if(longest)s.work=storage(owner.scratch,longest,longest*factor+16,false,attempt!=0);s.glyphs=take<u16>(owner.scratch,glyphs);s.placements=take<SfntPlacement>(owner.scratch,glyphs);
            // Source maps, clusters and runs are bounded by UTF-16 units, not UTF-8 bytes.
            s.source_glyphs=take<u32>(owner.scratch,units);s.clusters=take<NativeParagraphCluster>(owner.scratch,units);
            runs=take<NativeParagraphRun>(owner.scratch,units);if(!runs.data)return false;
            status=latin?native_latin_paragraph_shape(owner.store,binding.plan,text,system.independent_letters,em,s,boundary_storage(runs),&shape,nullptr,em*system.tracking,profile):native_ascii_paragraph_shape(owner.store,binding.plan,text,system.independent_letters,em,s,&shape,em*system.tracking);
            if(status!=SfntStatus::output_too_small||attempt)break;++owner.storage_retries;
        }
        if(status!=SfntStatus::ok){++owner.execute_failed;return false;}
        copy_bytes(saved.data,text.data,text.bytes);owner.last_text={saved.data,text.bytes};owner.paragraph_storage=s;owner.paragraph_shape=shape;owner.paragraph_runs=runs;
        owner.last_format=format;owner.last_em=em;owner.last_independent=system.independent_letters;owner.last_tracking=system.tracking;owner.last_paragraph=true;owner.last_styled=false;owner.last_valid=true;
    }
    const auto& shape=owner.paragraph_shape;const auto& s=owner.paragraph_storage;NativeParagraphLayout layout{};
    if(plan_storage){if(!plan_storage->allocate(owner,shape.cluster_count))return false;fit=plan_storage->fit;}
    if(native_prepared_paragraph_layout(shape,{s.clusters.data,shape.cluster_count},{s.placements.data,shape.glyph_count},width,format->tab_stop,wrapping==TextWrapping::wrap,owner.paragraph_runs,&layout,hits,overflow==TextOverflow::ellipsis?height:0,fit)!=SfntStatus::ok){++owner.execute_failed;return false;}
    if(planning){*planning=layout;return true;}
    // Windows trims overlong words instead of using the clip-mode emergency
    // wrap. Fitting aggregate metrics alone cannot establish no trimming.
    if(overflow==TextOverflow::ellipsis&&(layout.flags&native_paragraph_emergency_break||layout.width>width)){
        if(layout.line_count==1&&!hits&&trim_single_line(owner,&binding,system,format,text,width,height,wrapping,layout,output,measure))return true;
        return false;}
    if(output){
        const auto* final_run=layout.run_count?owner.paragraph_runs.data+layout.run_count-1:nullptr;
        const u32 visible_glyphs=(layout.flags&native_paragraph_height_trimmed)?(final_run?final_run->glyph_first+final_run->glyph_count:0):shape.glyph_count;
        const u32 visible_source=(layout.flags&native_paragraph_height_trimmed)?(final_run?static_cast<u32>(final_run->source_first)+final_run->source_count:0):shape.source_units;
        if(layout.run_count>output->run_capacity||visible_glyphs>output->glyph_capacity||shape.source_units>output->cluster_capacity||visible_glyphs>65535)return output_capacity_failure(owner);
        if(!retain_runs(owner.fonts,binding.font,layout.run_count,output))return false;
        for(u32 i=0;i<visible_glyphs;++i){const auto& p=s.placements[i];output->glyphs[i]={p.advance,p.advance_offset,p.ascender_offset,s.glyphs[i],0};}
        for(u32 i=0;i<shape.source_units;++i)output->clusters[i]=i<visible_source?static_cast<u16>(s.source_glyphs[i]):0xffffu;
        for(u32 i=0;i<layout.run_count;++i){const auto& run=owner.paragraph_runs[i];output->runs[i]={binding.font,run.x,run.baseline,em,run.glyph_first,run.glyph_count,run.source_first,run.source_count,0,0,false,true};}
        output->glyph_count=visible_glyphs;output->run_count=layout.run_count;output->cluster_count=shape.source_units;output->width=layout.width;output->width_including_trailing_whitespace=layout.extent;output->height=layout.height;output->line_count=layout.line_count;
        ++owner.shaped;++owner.paragraphs_shaped;
    }else if(measure){*measure={S_OK,layout.width,layout.extent,layout.height,shape.source_units,layout.line_count,TextShapeError::none};++owner.measured;++owner.paragraphs_measured;}
    else return false;return true;
}
}
bool native_text_context_initialize(NativeTextContext** output,NativeFontSession* fonts) noexcept {
    if(!output)return false;*output=nullptr;if(!fonts)return false;
    auto* reserved=static_cast<u8*>(VirtualAlloc(nullptr,context_reserved_bytes,MEM_RESERVE,PAGE_READWRITE));if(!reserved)return false;
    if(!VirtualAlloc(reserved,sizeof(NativeTextContext),MEM_COMMIT,PAGE_READWRITE)){VirtualFree(reserved,0,MEM_RELEASE);return false;}
    auto* owner=reinterpret_cast<NativeTextContext*>(reserved);reset_value(owner);owner->backing=owner;
    // Binding prefixes share the owner's final page; neither array ever moves.
    owner->bindings=reinterpret_cast<Binding*>(reserved+sizeof(NativeTextContext));
    owner->store={native_font_session_instances(fonts),{reinterpret_cast<NativeRunPlanSlot*>(reserved+binding_region_bytes),0}};
    owner->arabic_store={&owner->store,{reinterpret_cast<NativeArabicPlanSlot*>(reserved+binding_region_bytes+plan_region_bytes),0}};
    if(!arena_initialize(&owner->scratch,32*1024*1024,0,4096)||!native_text_catalogs_initialize(&owner->catalogs)||!native_font_session_retain(fonts)){
        native_text_catalogs_release(&owner->catalogs);arena_release(&owner->scratch);VirtualFree(owner,0,MEM_RELEASE);return false;}
    owner->fonts=fonts;*output=owner;return true;
}
bool native_text_candidate_take_output_failure(const TextSystem& system) noexcept {
    if(!system.native_text)return false;const bool failed=system.native_text->output_capacity_failed;system.native_text->output_capacity_failed=false;return failed;
}
bool native_text_format_create(NativeTextContext* owner,const NativeTextFormatDescription& d,u32* handle) noexcept {
    if(!handle)return false;*handle=0;if(!owner)return false;
    *handle=intern_format(*owner,{{d.family,d.scope,d.properties},d.em_size,d.tab_stop,0,d.word_wrap});return *handle!=0;
}
bool native_text_context_set_code_format(NativeTextContext* owner,u32 handle) noexcept {
    if(!owner||owner->binding_count||!format_at(*owner,handle))return false;
    owner->code_format=handle;return true;
}
bool native_text_format_prepare(NativeTextContext* owner,u32 handle) noexcept {
    if(!owner)return false;const auto* format=format_at(*owner,handle);
    const auto* prepared=format?binding(*owner,format,0):nullptr;
    return prepared&&!(prepared->key&binding_missing_family);
}
u64 native_text_format_identity(const TextSystem& system) noexcept {
    if(!system.native_text||!system.native_format)return 0;
    const auto* value=format_at(*system.native_text,system.native_format);return value?value->identity:0;
}
bool native_text_register_font_path(NativeTextContext* owner,const wchar_t* path,u32* scope) noexcept {
    return owner&&native_text_catalog_register_path(&owner->catalogs,path,scope);
}
bool native_text_candidate_hits_once(const TextSystem& system,Utf8View text,float width,float height,TextWrapping wrapping,NativeParagraphHitStorage hits,TextMeasureResult* measure) noexcept {
    CatalogOperation catalog_operation(system.native_text);
    if(!hits.output)return false;reset_value(&(*hits.output));
    if(!measure||!system.native_text||!tracking_valid(system.tracking)||!text.bytes||text.bytes>native_text_maximum_bytes||!text.data)return false;
    if(text.bytes>16000)return native_text_candidate_styled(system,text,{},width,height,nullptr,measure,wrapping,TextOverflow::clip,{},&hits);
    const auto& cached=*system.native_text;
    if(cached.last_valid&&cached.last_styled&&(cached.last_mode&8)&&!cached.last_styles.count&&cached.last_text.count==text.bytes&&bytes_equal(cached.last_text.data,text.data,text.bytes)&&
        native_text_candidate_styled(system,text,{},width,height,nullptr,measure,wrapping,TextOverflow::clip,{},&hits))return true;
    bool latin=false;for(u32 i=0;i<text.bytes;++i){const auto c=static_cast<u8>(text.data[i]);if((c<32&&c!=9&&c!=10&&c!=13)||c>126)latin=true;}
    if(latin&&!latin_text(text))return native_text_candidate_styled(system,text,{},width,height,nullptr,measure,wrapping,TextOverflow::clip,{},&hits);
    const auto* format=resolve_format(system,wrapping);if(!format)return false;
    auto& owner=*system.native_text;auto* selected=binding(owner,format);if(!selected)return false;
    return paragraph(owner,*selected,system,format,text,width,height,wrapping,TextOverflow::clip,nullptr,measure,&hits,latin)||
        native_text_candidate_styled(system,text,{},width,height,nullptr,measure,wrapping,TextOverflow::clip,{},&hits);
}
NativeTextCandidateCounts native_text_candidate_counts(const TextSystem& system) noexcept {
    const auto* owner=system.native_text;return owner?NativeTextCandidateCounts{owner->shaped,owner->measured,owner->arabic_shaped,owner->arabic_measured,owner->paragraphs_shaped,owner->paragraphs_measured,owner->latin_shaped,owner->latin_measured}:NativeTextCandidateCounts{};
}
NativeTextStorageCounts native_text_candidate_storage_counts(const TextSystem& system) noexcept {
    const auto* o=system.native_text;return o?NativeTextStorageCounts{o->scratch.reserved,o->scratch.committed,o->scratch.peak,o->storage_retries}:NativeTextStorageCounts{};
}
ConstSlice<NativeParagraphCluster> native_text_candidate_prepared_clusters(const TextSystem& system) noexcept {
    const auto* owner=system.native_text;
    return owner&&owner->last_valid&&owner->last_paragraph?ConstSlice<NativeParagraphCluster>{owner->paragraph_storage.clusters.data,owner->paragraph_shape.cluster_count}:ConstSlice<NativeParagraphCluster>{};
}
namespace {
void release_context_contents(NativeTextContext* o) noexcept {
    log_u64(LogLevel::info,"text","native_lines_shaped",o->shaped);log_u64(LogLevel::info,"text","native_lines_measured",o->measured);
    log_u64(LogLevel::info,"text","native_line_prepare_failures",o->prepare_failed);log_u64(LogLevel::info,"text","native_line_execute_failures",o->execute_failed);
    log_u64(LogLevel::info,"text","native_line_storage_retries",o->storage_retries);
    log_u64(LogLevel::info,"text","native_arabic_lines_shaped",o->arabic_shaped);log_u64(LogLevel::info,"text","native_arabic_lines_measured",o->arabic_measured);
    log_u64(LogLevel::info,"text","native_paragraphs_shaped",o->paragraphs_shaped);log_u64(LogLevel::info,"text","native_paragraphs_measured",o->paragraphs_measured);
    log_u64(LogLevel::info,"text","native_latin_lines_shaped",o->latin_shaped);log_u64(LogLevel::info,"text","native_latin_lines_measured",o->latin_measured);
    for(auto* sign=o->omission_signs;sign;sign=sign->next)native_font_run_release(native_font_session_runs(o->fonts),sign->font);
    for(u32 i=0;i<o->binding_count;++i){auto& slot=o->bindings[i];if(slot.font_flags!=emoji_font_flag&&slot.arabic.slot)native_arabic_plan_release(&o->arabic_store,slot.arabic);if(slot.plan.slot)native_run_plan_release(&o->store,slot.plan);if(slot.font.slot)native_font_run_release(native_font_session_runs(o->fonts),slot.font);}
    if(!native_font_session_release(&o->fonts))log_write(LogLevel::error,"text","native_line_session_has_live_handles");
    native_text_catalogs_release(&o->catalogs);
    arena_release(&o->scratch);
}
}
void native_text_context_release(NativeTextContext** pointer) noexcept {
    if(!pointer||!*pointer)return;auto* o=*pointer;auto* backing=o->backing;
    release_context_contents(o);if(backing!=o)VirtualFree(backing,0,MEM_RELEASE);
    VirtualFree(o,0,MEM_RELEASE);*pointer=nullptr;
}
#include "native_text_recovery.inc"
#include "native_text_styled.inc"
namespace {
bool trim_single_line(NativeTextContext& owner,const Binding* binding,const TextSystem& system,const NativeFormat* format,
    Utf8View text,float width,float height,TextWrapping wrapping,const NativeParagraphLayout& complete,
    ShapedText* output,TextMeasureResult* measure,RtlLineOutput* directional,ConstSlice<TextHitRange> hits) noexcept {
    const auto* sign=omission_sign(owner,format);
    if(!sign){
        // The first sign preparation can resolve a fallback font and replace
        // shape scratch. Cache it, then rebuild the original source once.
        return binding&&prepare_omission_sign(owner,system,format)&&
            native_text_candidate_line(system,text,width,height,output,measure,wrapping,TextOverflow::ellipsis);
    }
    const auto& shape=owner.paragraph_shape;const auto& s=owner.paragraph_storage;
    const float baseline=complete.run_count?owner.paragraph_runs[0].baseline:shape.vertical.baseline;
    const auto mark=arena_mark(&owner.scratch);
    struct Rewind {Arena& arena;ArenaMark mark;~Rewind() noexcept {arena_rewind(&arena,mark);}} rewind{owner.scratch,mark};
    u32 tab_count=0;for(u32 i=0;i<shape.cluster_count;++i)tab_count+=(s.clusters[i].flags&native_paragraph_tab)!=0;
    auto tabs=take<float>(owner.scratch,tab_count);if(tab_count&&!tabs.data)return false;
    float pen=0;u32 tab_index=0;
    const auto add=[](float a,float b) noexcept {return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a),_mm_set_ss(b)));};
    for(u32 i=0;i<shape.cluster_count;++i){const auto& c=s.clusters[i];
        if(c.flags&native_paragraph_tab){const float q=pen/format->tab_stop;if(!(q>=0)||q>1000000)return false;
            const float next=static_cast<float>(static_cast<u32>(q)+1)*format->tab_stop;
            tabs[tab_index++]=next-pen;pen=add(pen,tabs[tab_index-1]);}
        else pen=add(pen,c.width);
    }
    NativeLineTrimCut cut{};
    if(native_line_trim_cut({s.clusters.data,shape.cluster_count},complete.width,complete.extent,width,sign->width,false,{tabs.data,tabs.count},&cut)!=SfntStatus::ok)return false;
    NativeParagraphShape prefix=shape;prefix.cluster_count=cut.clusters;
    prefix.source_units=cut.clusters?s.clusters[cut.clusters-1].source_end:0;
    prefix.glyph_count=cut.clusters?s.clusters[cut.clusters-1].glyph_first+s.clusters[cut.clusters-1].glyph_count:0;
    bool decorations=false;if(output&&!binding)for(u32 i=0;i<owner.last_styles.count;++i)decorations|=(owner.last_styles[i].flags&native_decoration_flags)!=0;
    NativeParagraphLayout placed{};
    NativeParagraphLineSink sink{};u32* written=nullptr;
    if(directional){
        directional->written=directional->byte=directional->source=directional->span=0;
        directional->omission=cut.sign;directional->omission_width=sign->width;directional->omission_x=0;
        u32 span=0;while(span+1<owner.styled_spans.count&&owner.styled_spans[span].end_unit<=prefix.source_units)++span;
        directional->omission_level=owner.last_mode&32?static_cast<u8>(owner.styled_analysis[span]):static_cast<u8>(1);
        if(cut.sign&&(decorations||hits.count)){auto zeroes=take<NativeOmissionZero>(owner.scratch,owner.styled_spans.count-span);if(!zeroes.data)return false;u32 count=0;
            for(u32 i=span;i<owner.styled_spans.count;++i){const auto& source=owner.styled_spans[i];const u8 level=owner.last_mode&32?static_cast<u8>(owner.styled_analysis[i]):static_cast<u8>(1);
                if(count&&zeroes[count-1].level==level)zeroes[count-1].end=source.end_unit;
                else zeroes[count++]={maximum(prefix.source_units,static_cast<u32>(source.first_unit)),source.end_unit,0,level};}
            directional->zeroes={zeroes.data,count};}
        sink={directional,output?publish_rtl_line:nullptr,owner.styled_reset,split_rtl_fallback_space};if(output)written=&directional->written;
    }
    if(sink.trailing_reset.count)sink.trailing_reset.count=prefix.cluster_count;
    const auto status=binding?
        native_prepared_paragraph_layout(prefix,{s.clusters.data,prefix.cluster_count},{s.placements.data,prefix.glyph_count},width,format->tab_stop,false,owner.paragraph_runs,&placed):
        native_prepared_styled_paragraph_layout(prefix,{s.clusters.data,prefix.cluster_count},{s.placements.data,prefix.glyph_count},{owner.styled_parts.data,owner.styled_parts.count},width,format->tab_stop,false,owner.paragraph_runs,&placed,owner.styled_lines,nullptr,sink);
    if(status!=SfntStatus::ok)return false;
    const auto style_for=[&](u32 run) noexcept -> const StyledSpan* {
        if(binding)return nullptr;u32 first=0,end=owner.styled_spans.count-1;
        while(first<end){const u32 middle=first+(end-first)/2;
            if(owner.styled_spans[middle].end_unit<=owner.paragraph_runs[run].source_first)first=middle+1;else end=middle;}
        return owner.styled_spans.data+first;
    };
    const auto font_for=[&](u32 run) noexcept {return binding?binding->font:style_for(run)->binding()->font;};
    if(output){
        const u32 published_glyphs=written?*written:prefix.glyph_count;
        const u32 sign_runs=cut.sign?1u:0u,sign_glyphs=cut.sign?sign->glyphs:0u;
        if(placed.run_count+sign_runs>output->run_capacity||published_glyphs>output->glyph_capacity||sign_glyphs>output->glyph_capacity-published_glyphs||shape.source_units>output->cluster_capacity||published_glyphs+sign_glyphs>65535)return output_capacity_failure(owner);
        if(!output->glyphs||!output->clusters||!output->runs||output->run_count||output->native_fonts())return false;
        if(!written)for(u32 i=0;i<prefix.glyph_count;++i){const auto& p=s.placements[i];output->glyphs[i]={p.advance,p.advance_offset,p.ascender_offset,s.glyphs[i],0};}
        if(cut.sign)copy_bytes(output->glyphs+published_glyphs,sign+1,sign_glyphs*sizeof(TextGlyph));
        if(!written)for(u32 i=0;i<prefix.source_units;++i)output->clusters[i]=static_cast<u16>(s.source_glyphs[i]);
        for(u32 i=prefix.source_units;i<shape.source_units;++i)output->clusters[i]=0xffffu;
        const auto* last=placed.run_count?owner.paragraph_runs.data+placed.run_count-1:nullptr;
        float x=last?add(last->x,last->width):0;
        if(directional&&cut.sign)x=directional->omission_x;
        u32 decoration_count=0,hit_count=0;
        if(decorations||hits.count){
            if(!owner.styled_lines.count)return false;owner.styled_lines[0].height=complete.height;
            for(u32 i=0;i<placed.run_count;++i)owner.paragraph_runs[i].baseline=baseline;
            const NativeOmissionZero zero{prefix.source_units,shape.source_units,add(x,sign->width),0};
            const NativeStyledOmission omission{prefix.source_units,x,sign->width,complete.height,directional?directional->omission_visual:placed.run_count,
                directional?ConstSlice<NativeOmissionZero>{directional->zeroes.data,directional->zeroes.count}:ConstSlice<NativeOmissionZero>{&zero,1}};
            if(decorations&&!native_styled_decorations(owner,placed,width,format->tab_stop,{output->decorations,output->decoration_capacity},&decoration_count,written?output:nullptr,cut.sign?&omission:nullptr))return false;
            if(!native_collect_hit_regions(owner,placed,format->tab_stop,hits,true,output,&hit_count,written?output:nullptr,cut.sign?&omission:nullptr))return false;
        }
        if(!native_font_session_retain(owner.fonts))return false;
        auto* store=native_font_session_runs(owner.fonts);u32 retained=0;
        while(retained<placed.run_count&&native_font_run_retain(store,font_for(retained)))++retained;
        if(retained!=placed.run_count||(cut.sign&&!native_font_run_retain(store,sign->font))){
            while(retained){--retained;native_font_run_release(store,font_for(retained));}auto* session=owner.fonts;native_font_session_release(&session);return false;}
        output->set_native_fonts(owner.fonts);
        for(u32 i=0;i<placed.run_count;++i){const auto& r=owner.paragraph_runs[i];const auto* style=style_for(i);
            const float origin=written?output->runs[i].baseline_x:r.x;const u32 first=written?output->runs[i].first_glyph:r.glyph_first,count=written?output->runs[i].glyph_count:r.glyph_count;const u8 level=written?output->runs[i].bidi_level:0;
            output->runs[i]={font_for(i),origin,baseline,style?style->em:format->em,first,count,r.source_first,r.source_count,style?style->flags:0,level,false,true};}
        const float y=add(add(baseline,-sign->baseline),sign->baseline);
        if(cut.sign){output->runs[placed.run_count]={sign->font,x,y,format->em,published_glyphs,sign_glyphs,0,1,0,0,false,true};
            output->clusters[0]=static_cast<u16>(published_glyphs);}
        output->glyph_count=published_glyphs+sign_glyphs;output->run_count=placed.run_count+sign_runs;
        output->cluster_count=shape.source_units;output->decoration_count=decoration_count;output->hit_region_count=hit_count;output->width=cut.width;output->width_including_trailing_whitespace=cut.extent;output->height=complete.height;output->line_count=1;
        ++owner.shaped;++owner.paragraphs_shaped;
    }else if(measure){*measure={S_OK,cut.width,cut.extent,complete.height,shape.source_units,1,TextShapeError::none};++owner.measured;++owner.paragraphs_measured;}
    else return false;
    return true;
}
}

namespace {
bool prepare_trim_plan(const TextSystem& system,Utf8View text,ConstSlice<TextStyleSpan> styles,
    float width,float height,TextWrapping wrapping,Slice<NativeParagraphLineFit> fit,NativeParagraphLayout* output,NativeTrimPlanScratch* storage,bool require_styled=false) noexcept {
    CatalogOperation catalog_operation(system.native_text);
    if(!output)return false;reset_value(output);
    if(!system.native_text||!text.data||!text.bytes||text.bytes>native_text_maximum_bytes||!(height>0)||(!fit.data&&!storage)||!tracking_valid(system.tracking))return false;
    const auto* format=resolve_format(system,wrapping);if(!format)return false;
    auto& owner=*system.native_text;TextMeasureResult prepared{};NativeParagraphLayout layout{};
    if(!omission_sign(owner,format)&&!prepare_omission_sign(owner,system,format))return false;
    const auto* sign=omission_sign(owner,format);if(!sign)return false;
    bool plain=!styles.count&&!require_styled&&text.bytes<=16000;for(u32 i=0;plain&&i<text.bytes;++i){const auto c=static_cast<u8>(text.data[i]);plain=(c>=32&&c<=126)||c==9||c==10||c==13;}
    if(plain){auto* selected=binding(owner,format);if(!selected||!paragraph(owner,*selected,system,format,text,width,height,wrapping,TextOverflow::ellipsis,nullptr,&prepared,nullptr,false,nullptr,fit,&layout,storage))return false;}
    else if(!native_text_styled_plan_or_output(system,text,styles,width,height,nullptr,&prepared,wrapping,TextOverflow::ellipsis,{},nullptr,fit,&layout,storage,require_styled))return false;
    if(storage)fit=storage->fit;
    const auto& shape=owner.paragraph_shape;const auto& s=owner.paragraph_storage;
    const auto mark=arena_mark(&owner.scratch);
    struct Rewind {Arena& arena;ArenaMark mark;~Rewind() noexcept {arena_rewind(&arena,mark);}} rewind{owner.scratch,mark};
    u32 tab_capacity=0;for(u32 i=0;i<shape.cluster_count;++i)tab_capacity+=(s.clusters[i].flags&native_paragraph_tab)!=0;
    auto tabs=take<float>(owner.scratch,tab_capacity);if(tab_capacity&&!tabs.data)return false;
    layout.width=layout.extent=0;
    for(u32 i=0;i<layout.line_count;++i){auto& line=fit[i];
        if(line.flags&native_line_fit_hidden){line.visible_clusters=0;line.flags|=native_line_fit_trimmed;continue;}
        const u32 first=line.first_cluster;u32 end=first+line.cluster_count;
        while(end>first&&(s.clusters[end-1].flags&(native_paragraph_newline|native_paragraph_crlf_prefix)))--end;
        float pen=0;u32 count=0;
        for(u32 c=first;c<end;++c){const auto& cluster=s.clusters[c];
            if(cluster.flags&native_paragraph_tab){const float q=pen/format->tab_stop;if(!(q>=0)||q>1000000)return false;
                const float next=static_cast<float>(static_cast<u32>(q)+1)*format->tab_stop;tabs[count++]=decoration_sub(next,pen);pen=next;}
            else pen=decoration_add(pen,cluster.width);}
        const bool force=(line.flags&native_line_fit_force_sign)!=0;const bool trimmed=force||line.width>width;NativeLineTrimCut cut{};
        if(native_line_trim_cut({s.clusters.data+first,end-first},line.width,line.extent,width,sign->width,force,{tabs.data,count},&cut)!=SfntStatus::ok)return false;
        line.visible_clusters=static_cast<u16>(cut.clusters);line.width=cut.width;line.extent=cut.extent;
        if(cut.sign)line.flags|=native_line_fit_sign;if(trimmed)line.flags|=native_line_fit_trimmed;
        layout.width=maximum(layout.width,line.width);layout.extent=maximum(layout.extent,line.extent);
    }
    *output=layout;return true;
}

}
bool native_text_prepare_trim_plan_once(const TextSystem& system,Utf8View text,ConstSlice<TextStyleSpan> styles,
    float width,float height,TextWrapping wrapping,Slice<NativeParagraphLineFit> fit,NativeParagraphLayout* output) noexcept {
    return prepare_trim_plan(system,text,styles,width,height,wrapping,fit,output,nullptr);
}
#include "native_text_multiline_draw.inc"

// Every single-line shape and measure enters here first, including cached
// emoji and styled text: compiled for speed inside the size-optimized adapter.
#pragma clang attribute pop
bool native_text_candidate_line_once(const TextSystem& system,Utf8View text,float width,float height,ShapedText* output,TextMeasureResult* measure,TextWrapping wrapping,TextOverflow overflow) noexcept {
    CatalogOperation catalog_operation(system.native_text);
    if(!system.native_text||!tracking_valid(system.tracking)||!text.bytes||text.bytes>native_text_maximum_bytes||!text.data)return false;
    if(text.bytes>16000)return native_text_candidate_styled(system,text,{},width,height,output,measure,wrapping,overflow);
    const auto& cached=*system.native_text;
    if(cached.last_valid&&cached.last_styled&&(cached.last_mode&14)&&!cached.last_styles.count&&cached.last_text.count==text.bytes&&bytes_equal(cached.last_text.data,text.data,text.bytes)&&
        native_text_candidate_styled(system,text,{},width,height,output,measure,wrapping,overflow))return true;
    bool arabic=false,controls=false;for(u32 i=0;i<text.bytes;++i){const auto c=static_cast<u8>(text.data[i]);if(c==9||c==10||c==13)controls=true;else if(c<32||c>126)arabic=true;}
    if(arabic){const auto first=utf8_decode_one(text.data,text.bytes);
        const auto script=first.valid?unicode_properties(first.codepoint)->script():UnicodeScript::zzzz;
        u16 profile_script=0;switch(script){
            case UnicodeScript::hani:profile_script=han_script;break;case UnicodeScript::armn:profile_script=armenian_script;break;
            case UnicodeScript::geor:profile_script=georgian_script;break;case UnicodeScript::cher:profile_script=cherokee_script;break;
            case UnicodeScript::ethi:profile_script=ethiopic_script;break;case UnicodeScript::runr:profile_script=runic_script;break;default:break;}
        if(profile_script){
            const auto* format=resolve_format(system,wrapping);if(!format)return false;
            auto& owner=*system.native_text;const auto* selected=binding(owner,format);if(!selected)return false;
            const auto* instance=native_font_instance_get(*owner.store.instances,owner.store.slots[selected->plan.slot-1].instance);
            const auto* font=instance?native_font_get(*owner.store.instances->fonts,instance->face):nullptr;u16 glyph=0;
            if(font&&sfnt_cmap_glyph(font->cmap,first.codepoint,0,&glyph)==SfntStatus::ok&&glyph&&single_script_text(text,script)){
                const auto* profile=alphabetic_profile(owner,*selected,profile_script,system.independent_letters);
                if(profile&&paragraph(owner,*selected,system,format,text,width,height,wrapping,overflow,output,measure,nullptr,true,profile))return true;
            }
            return native_text_candidate_styled(system,text,{},width,height,output,measure,wrapping,overflow);
        }
    }
    bool latin=false;u8 script=0;if(arabic&&!arabic_text(text)){
        if(hebrew_text(text)||itemized_text(text))return native_text_candidate_styled(system,text,{},width,height,output,measure,wrapping,overflow);
        if(!latin_text(text,&controls)&&(!latin_text(text,&controls,true,true,&script)||!script||controls))return native_text_candidate_styled(system,text,{},width,height,output,measure,wrapping,overflow);arabic=false;latin=true;}
    if(arabic&&system.tracking)return native_text_candidate_styled(system,text,{},width,height,output,measure,wrapping,overflow);
    const auto* format=resolve_format(system,wrapping);if(!format)return false;
    auto& owner=*system.native_text;auto* selected=binding(owner,format);if(!selected)return false;
    const auto* profile=script?alphabetic_profile(owner,*selected,script,system.independent_letters):nullptr;if(script&&!profile)return false;
    if(!arabic&&(controls||(owner.last_valid&&!owner.last_styled&&owner.last_paragraph&&owner.last_format==format&&owner.last_em==format->em&&owner.last_independent==system.independent_letters&&owner.last_tracking==system.tracking&&owner.last_text.count==text.bytes&&bytes_equal(owner.last_text.data,text.data,text.bytes))))return paragraph(owner,*selected,system,format,text,width,height,wrapping,overflow,output,measure,nullptr,latin,profile)||
        native_text_candidate_styled(system,text,{},width,height,output,measure,wrapping,overflow);
    if(arabic&&!prepare_script_profile(owner,*selected,arabic_script,system.independent_letters))
        return native_text_candidate_styled(system,text,{},width,height,output,measure,wrapping,overflow);
    const float em=format->em;
    if(owner.last_valid&&!owner.last_styled&&!owner.last_paragraph&&owner.last_format==format&&owner.last_em==em&&owner.last_independent==system.independent_letters&&owner.last_tracking==system.tracking&&
       owner.last_text.count==text.bytes&&bytes_equal(owner.last_text.data,text.data,text.bytes)){}
    else{
        owner.last_valid=false;owner.prefer_multiline=false;NativeAsciiLineStorage buffers{};Slice<char> saved_text{};
        NativeAsciiLineResult line{};SfntStatus status{};
        for(u32 attempt=0;attempt<2;++attempt){
        {
            arena_reset(&owner.scratch);saved_text=take<char>(owner.scratch,text.bytes);if(!saved_text.data)return false;
            buffers=storage(owner.scratch,text.bytes,text.bytes*(attempt?4u:1u)+16,arabic,attempt!=0);
        }
        reset_value(&line);
        {
            status=arabic?arabic_line(owner,*selected,text,system.independent_letters,em,buffers,&line):latin?native_latin_line(owner.store,selected->plan,text,system.independent_letters,em,buffers,&line,{},em*system.tracking,profile):native_ascii_line(owner.store,selected->plan,text,system.independent_letters,em,buffers,&line,{},em*system.tracking);
        }
        if(status!=SfntStatus::output_too_small||attempt)break;++owner.storage_retries;
        }
        if(status!=SfntStatus::ok){++owner.execute_failed;return native_text_candidate_styled(system,text,{},width,height,output,measure,wrapping,overflow);}
        copy_bytes(saved_text.data,text.data,text.bytes);owner.last_text={saved_text.data,text.bytes};owner.last_storage=buffers;owner.last_line=line;
        owner.last_format=format;owner.last_em=em;owner.last_independent=system.independent_letters;owner.last_tracking=system.tracking;owner.last_paragraph=false;owner.last_styled=false;owner.last_valid=true;
    }
    const auto& buffers=owner.last_storage;const auto& line=owner.last_line;
    if(!(line.geometry_flags&native_line_ordered_x))return false;
    if(line.width_including_trailing>width||line.vertical.height>height){
        if(!arabic)return paragraph(owner,*selected,system,format,text,width,height,wrapping,overflow,output,measure,nullptr,latin,profile)||
            native_text_candidate_styled(system,text,{},width,height,output,measure,wrapping,overflow);
        return native_text_candidate_styled(system,text,{},width,height,output,measure,wrapping,overflow);}
    if(output){
        const bool split=line.trailing_source_units&&line.trailing_source_units<line.run.source_units;
        const u32 run_count=split?2u:1u;const u32 trim=line.run.source_units-line.trailing_source_units;
        NativeLinePosition tail{};if(split&&native_ascii_line_position({buffers.clusters.data,line.cluster_count},line,trim,false,&tail)!=SfntStatus::ok)return false;
        const u32 split_glyph=split?buffers.run.character_glyphs[trim]:line.run.glyph_count;
        if(run_count>output->run_capacity||line.run.glyph_count>output->glyph_capacity||line.run.source_units>output->cluster_capacity||line.run.glyph_count>65535)return output_capacity_failure(owner);
        if(!retain_runs(owner.fonts,selected->font,run_count,output))return false;
        for(u32 i=0;i<line.run.glyph_count;++i){const auto& p=buffers.run.placements[i];output->glyphs[i]={p.advance,p.advance_offset,p.ascender_offset,arabic?buffers.run.shaping.glyphs[i].glyph.glyph:buffers.run.glyphs[i],0};}
        for(u32 i=0;i<line.run.source_units;++i)output->clusters[i]=static_cast<u16>(buffers.run.character_glyphs[i]);
        output->runs[0]={selected->font,arabic?line.caret_end:0,line.vertical.baseline,em,0,split_glyph,0,split?trim:line.run.source_units,0,static_cast<u8>(arabic?1:0),false,true};
        if(split){output->runs[1]={selected->font,tail.left,line.vertical.baseline,em,split_glyph,line.run.glyph_count-split_glyph,trim,line.trailing_source_units,0,0,false,true};}
        output->glyph_count=line.run.glyph_count;output->run_count=run_count;output->cluster_count=line.run.source_units;output->width=line.width;output->width_including_trailing_whitespace=line.width_including_trailing;output->height=line.vertical.height;output->line_count=1;++owner.shaped;
        if(arabic)++owner.arabic_shaped;
        if(latin)++owner.latin_shaped;
    }else if(measure){*measure={S_OK,line.width,line.width_including_trailing,line.vertical.height,line.run.source_units,1,TextShapeError::none};++owner.measured;if(arabic)++owner.arabic_measured;if(latin)++owner.latin_measured;}
    else return false;
    return true;
}
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
namespace {
struct ShapeRequest {Utf8View text;ConstSlice<TextStyleSpan> styles;ConstSlice<TextHitRange> hits;float width,height;TextWrapping wrapping;};
// Share the dispatcher between the first attempt and the erased retry callback.
// Inlining it into both callers duplicates the complete alternative tree.
__declspec(noinline) bool shape_request_attempt(const TextSystem& current,ShapedText* out,void* context) noexcept {
    const auto& r=*static_cast<const ShapeRequest*>(context);
    // Each once helper already guards its nested calls against recovery.
    const bool trimmed=prefers_multiline(current,r.text,r.styles,r.wrapping);
    if(trimmed&&native_text_candidate_trimmed_once(current,r.text,r.styles,r.width,r.height,out,r.wrapping,r.hits))return true;
    if(!r.styles.count&&!r.hits.count){if(native_text_candidate_line_once(current,r.text,r.width,r.height,out,nullptr,r.wrapping,TextOverflow::ellipsis))return true;}
    else if(native_text_candidate_styled_once(current,r.text,r.styles,r.width,r.height,out,nullptr,r.wrapping,TextOverflow::ellipsis,r.hits,nullptr))return true;
    return !trimmed&&native_text_candidate_trimmed_once(current,r.text,r.styles,r.width,r.height,out,r.wrapping,r.hits);
}
}
bool native_text_candidate_ellipsis(const TextSystem& system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,float height,
    ShapedText* output,TextWrapping wrapping,ConstSlice<TextHitRange> hits) noexcept {
    ShapeRequest request{text,styles,hits,width,height,wrapping};
    return shape_request_attempt(system,output,&request)||retry_stale_erased(system,output,shape_request_attempt,&request);
}
bool native_text_candidate_line(const TextSystem& system,Utf8View text,float width,float height,ShapedText* output,TextMeasureResult* measure,TextWrapping wrapping,TextOverflow overflow) noexcept {
    if(native_text_candidate_line_once(system,text,width,height,output,measure,wrapping,overflow))return true;
    return retry_stale(system,output,[&](const TextSystem& s,ShapedText* out) noexcept {return native_text_candidate_line_once(s,text,width,height,out,measure,wrapping,overflow);});
}
bool native_text_candidate_styled(const TextSystem& system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,float height,ShapedText* output,TextMeasureResult* measure,TextWrapping wrapping,TextOverflow overflow,ConstSlice<TextHitRange> hits,NativeParagraphHitStorage* editor) noexcept {
    if(native_text_candidate_styled_once(system,text,styles,width,height,output,measure,wrapping,overflow,hits,editor))return true;
    return retry_stale(system,output,[&](const TextSystem& s,ShapedText* out) noexcept {return native_text_candidate_styled_once(s,text,styles,width,height,out,measure,wrapping,overflow,hits,editor);});
}
bool native_text_candidate_hits(const TextSystem& system,Utf8View text,float width,float height,TextWrapping wrapping,NativeParagraphHitStorage hits,TextMeasureResult* measure) noexcept {
    if(native_text_candidate_hits_once(system,text,width,height,wrapping,hits,measure))return true;
    return retry_stale(system,nullptr,[&](const TextSystem& s,ShapedText*) noexcept {return native_text_candidate_hits_once(s,text,width,height,wrapping,hits,measure);});
}
bool native_text_prepare_trim_plan(const TextSystem& system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,float height,TextWrapping wrapping,Slice<NativeParagraphLineFit> fit,NativeParagraphLayout* output) noexcept {
    if(native_text_prepare_trim_plan_once(system,text,styles,width,height,wrapping,fit,output))return true;
    return retry_stale(system,nullptr,[&](const TextSystem& s,ShapedText*) noexcept {return native_text_prepare_trim_plan_once(s,text,styles,width,height,wrapping,fit,output);});
}

}
#pragma clang attribute pop
