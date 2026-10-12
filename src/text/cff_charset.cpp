#include "cff_internal.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord::cff_detail {
namespace {

// StandardEncoding -> SID and predefined charset SID data (format constants
// from the CFF specification, Adobe TN #5176).
constexpr u8 standard_high[128]{
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,0,111,112,113,114,0,115,116,117,118,119,120,121,122,
    0,123,0,124,125,126,127,128,129,130,131,0,132,133,0,134,135,136,137,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,138,0,139,0,0,0,0,140,141,142,143,0,0,0,0,0,144,0,0,0,145,0,0,146,147,148,149,0,0,0,0};
constexpr u16 expert[]{
    0,1,229,230,231,232,233,234,235,236,237,238,13,14,15,99,239,240,241,242,243,244,245,246,247,248,27,28,
    249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,109,110,267,268,269,270,271,272,
    273,274,275,276,277,278,279,280,281,282,283,284,285,286,287,288,289,290,291,292,293,294,295,296,297,298,
    299,300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,158,155,163,319,320,321,
    322,323,324,325,326,150,164,169,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,
    345,346,347,348,349,350,351,352,353,354,355,356,357,358,359,360,361,362,363,364,365,366,367,368,369,370,
    371,372,373,374,375,376,377,378};
constexpr u16 expert_subset[]{
    0,1,231,232,235,236,237,238,13,14,15,99,239,240,241,242,243,244,245,246,247,248,27,28,249,250,251,253,
    254,255,256,257,258,259,260,261,262,263,264,265,266,109,110,267,268,269,270,272,300,301,302,305,314,315,
    158,155,163,320,321,322,323,324,325,326,150,164,169,327,328,329,330,331,332,333,334,335,336,337,338,339,
    340,341,342,343,344,345,346};

} // namespace

SfntStatus standard_glyph(const CffFont& font, u32 code, u16* glyph) noexcept {
    if (code > 255 || font.cid) return SfntStatus::malformed;
    const u16 sid = code >= 32 && code <= 126 ? static_cast<u16>(code-31) : code >= 128 ? standard_high[code-128] : 0;
    if (!sid) return SfntStatus::unsupported_sequence;
    if (font.charset <= 2) {
        if (!font.charset) {
            if (sid < font.glyph_count && sid < 229) { *glyph=sid; return SfntStatus::ok; }
        } else {
            const u16* ids = font.charset == 1 ? expert : expert_subset;
            const u32 count = font.charset == 1 ? static_cast<u32>(sizeof(expert)/sizeof(u16))
                                               : static_cast<u32>(sizeof(expert_subset)/sizeof(u16));
            if (font.glyph_count > count) return SfntStatus::malformed;
            for (u32 i=0;i<font.glyph_count;++i) if(ids[i]==sid) { *glyph=static_cast<u16>(i); return SfntStatus::ok; }
        }
        return SfntStatus::unsupported_sequence;
    }
    const auto table=font.table;
    if (font.charset >= table.count) return SfntStatus::truncated;
    u32 at=font.charset;
    const u8 format=table.data[at++];
    if (format > 2) return SfntStatus::unsupported;
    for (u32 i=1;i<font.glyph_count;) {
        const u32 size=format==0?2u:format==1?3u:4u;
        if (size>table.count-at) return SfntStatus::truncated;
        const u32 first=byte_load_be16(table.data+at);
        const u32 left=format==0?0u:format==1?table.data[at+2]:byte_load_be16(table.data+at+2);
        if (left >= font.glyph_count-i || left > 65535u-first) return SfntStatus::malformed;
        if (sid>=first && sid-first<=left) { *glyph=static_cast<u16>(i+sid-first); return SfntStatus::ok; }
        i+=left+1; at+=size;
    }
    return SfntStatus::unsupported_sequence;
}

} // namespace maicord::cff_detail
