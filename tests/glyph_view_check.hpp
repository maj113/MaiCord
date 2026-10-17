// QA-only differential adapter: compare flat input with a strided, poisoned-gap
// view at every split position. Corpus expectations are still checked by callers.
#include "maicord/text/context.hpp"
struct QaGapRecord {maicord::SfntGsubGlyph glyph;maicord::u32 marker[3];};
static QaGapRecord qa_gap_records[65543];
static maicord::u32 qa_gap_positions[65536];
static maicord::u64 qa_gap_queries=0;
static inline maicord::SfntGlyphView qa_gap_view(maicord::ConstSlice<maicord::SfntGsubGlyph> input,maicord::u32 split) noexcept {
    using namespace maicord;
    for(u32 i=0;i<input.count;++i){auto& r=qa_gap_records[i+(i>=split?7:0)];r.glyph=input.data[i];r.marker[0]=i;r.marker[1]=0xfeaa21d7;r.marker[2]=~i;}
    for(u32 i=0;i<7;++i)qa_gap_records[split+i]={{65535,static_cast<SfntGlyphClass>(255),255},{0xabcdef01,0xabcdef02,0xabcdef03}};
    return {&qa_gap_records[0].glyph,&qa_gap_records[split+7].glyph,split,input.count-split,sizeof(QaGapRecord)};
}
static inline bool qa_same_gsub(const maicord::SfntGsubMatch& a,const maicord::SfntGsubMatch& b) noexcept {
    return a.matched==b.matched && a.input_count==b.input_count && a.last_input==b.last_input && a.output_count==b.output_count && a.single==b.single && a.replacements.data==b.replacements.data && a.replacements.count==b.replacements.count;
}
static inline maicord::SfntStatus qa_gsub_match(const maicord::SfntGsubSubtable& sub,const maicord::SfntGdef& gdef,const maicord::SfntLookupFilter& filter,
                                      maicord::ConstSlice<maicord::SfntGsubGlyph> glyphs,maicord::u32 start,maicord::u32 alternate,maicord::Slice<maicord::u32> positions,maicord::SfntGsubMatch* output) noexcept {
    using namespace maicord;const auto status=sfnt_gsub_subtable_match_view(sub,gdef,filter,sfnt_glyph_view(glyphs),start,alternate,positions,output);if(status!=SfntStatus::ok)return status;
    for(u32 split=0;split<=glyphs.count;++split){SfntGsubMatch other{};++qa_gap_queries;
        if(sfnt_gsub_subtable_match_view(sub,gdef,filter,qa_gap_view(glyphs,split),start,alternate,{qa_gap_positions,65536},&other)!=SfntStatus::ok || !qa_same_gsub(*output,other))return SfntStatus::malformed;
        for(u32 i=0;i<output->input_count;++i)if(qa_gap_positions[i]!=positions.data[i])return SfntStatus::malformed;}
    return status;
}
static inline maicord::SfntStatus qa_context_match(const maicord::SfntContext& c,const maicord::SfntGdef& gdef,const maicord::SfntLookupFilter& filter,
                                         maicord::ConstSlice<maicord::SfntGsubGlyph> glyphs,maicord::u32 start,maicord::u32 end,maicord::Slice<maicord::u32> positions,maicord::SfntContextMatch* output) noexcept {
    using namespace maicord;const auto status=sfnt_context_match_view(c,gdef,filter,sfnt_glyph_view(glyphs),start,end,positions,output);if(status!=SfntStatus::ok)return status;
    for(u32 split=0;split<=glyphs.count;++split){SfntContextMatch other{};++qa_gap_queries;
        if(sfnt_context_match_view(c,gdef,filter,qa_gap_view(glyphs,split),start,end,{qa_gap_positions,65536},&other)!=SfntStatus::ok || other.matched!=output->matched || other.input_count!=output->input_count || other.last_input!=output->last_input || other.actions.data!=output->actions.data || other.actions.count!=output->actions.count)return SfntStatus::malformed;
        for(u32 i=0;i<output->input_count;++i)if(qa_gap_positions[i]!=positions.data[i])return SfntStatus::malformed;}
    return status;
}
static inline maicord::SfntStatus qa_reverse_match(const maicord::SfntReverseSubstitution& r,const maicord::SfntGdef& gdef,const maicord::SfntLookupFilter& filter,
                                         maicord::ConstSlice<maicord::SfntGsubGlyph> glyphs,maicord::u32 start,maicord::SfntGsubMatch* output) noexcept {
    using namespace maicord;const auto status=sfnt_gsub_reverse_match_view(r,gdef,filter,sfnt_glyph_view(glyphs),start,output);if(status!=SfntStatus::ok)return status;
    for(u32 split=0;split<=glyphs.count;++split){SfntGsubMatch other{};++qa_gap_queries;if(sfnt_gsub_reverse_match_view(r,gdef,filter,qa_gap_view(glyphs,split),start,&other)!=SfntStatus::ok || !qa_same_gsub(*output,other))return SfntStatus::malformed;}return status;
}
