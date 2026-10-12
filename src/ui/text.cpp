#include "maicord/ui/text.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/native_text_candidate.hpp"
#include "maicord/text/font_session.hpp"
#include "maicord/text/font_run.hpp"
#include "maicord/text/visibility.hpp"
#include "maicord/text/unicode.hpp"
#include "maicord/text/grapheme.hpp"

#include <Windows.h>

namespace maicord {
namespace {
bool usable_text_system(const TextSystem* system) noexcept {
    return system && system->native_text && system->native_format;
}

TextShapeResult shape_result(TextShapeError error, HRESULT native_error = S_OK, u32 utf16_units = 0,
                             const ShapedText* output = nullptr) noexcept {
    return TextShapeResult{native_error, utf16_units, output ? output->glyph_count : 0,
                           output ? output->run_count : 0, error};
}

bool layout_extent_valid(float value) noexcept {
    return value == value && value > 0.0f && value <= 1'048'576.0f;
}

struct TextHitEndpoint {
    u32 byte_offset;
    u16 hit_index;
    bool end;
};
} // namespace

TextShapeResult validate_text_input(Utf8View input, Slice<wchar_t> output) noexcept {
    if ((!input.data && input.bytes != 0) || (!output.data && output.count != 0)) {
        return shape_result(TextShapeError::invalid_input, E_INVALIDARG);
    }
    u32 required = 0;
    u32 offset = 0;
    while (offset < input.bytes) {
        const Utf8DecodeResult decoded = utf8_decode_one(input.data + offset, input.bytes - offset);
        if (!decoded.valid || decoded.width == 0) {
            return shape_result(TextShapeError::invalid_utf8, E_INVALIDARG);
        }
        const u32 scalar_units = decoded.codepoint <= 0xFFFF ? 1u : 2u;
        if (required > ~0u - scalar_units) {
            return shape_result(TextShapeError::output_too_small,
                                HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
        }
        required += scalar_units;
        offset += decoded.width;
    }
    if (required > output.count) {
        return shape_result(TextShapeError::output_too_small, HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
                            required);
    }

    return shape_result(TextShapeError::none, S_OK, required);
}

bool resolve_text_hits(Utf8View input, ConstSlice<TextHitSpan> hits, TextHitRange* resolved) noexcept {
    if (hits.count > kTextHitSpanMaximum || (!hits.data && hits.count != 0) ||
        (!resolved && hits.count != 0)) {
        return false;
    }
    if (!hits.count) return true;
    TextHitEndpoint endpoints[kTextHitSpanMaximum * 2]{};
    for (u32 index = 0; index < hits.count; ++index) {
        const TextHitSpan& hit = hits[index];
        if (hit.byte_count == 0 || hit.byte_offset > input.bytes ||
            hit.byte_count > input.bytes - hit.byte_offset) {
            return false;
        }
        resolved[index] = TextHitRange{0, 0, hit.semantic_index, 0};
        endpoints[index * 2] = TextHitEndpoint{hit.byte_offset, static_cast<u16>(index), false};
        endpoints[index * 2 + 1] =
            TextHitEndpoint{hit.byte_offset + hit.byte_count, static_cast<u16>(index), true};
    }
    const u32 endpoint_count = hits.count * 2;
    for (u32 index = 1; index < endpoint_count; ++index) {
        const TextHitEndpoint value = endpoints[index];
        u32 insertion = index;
        while (insertion != 0 && endpoints[insertion - 1].byte_offset > value.byte_offset) {
            endpoints[insertion] = endpoints[insertion - 1];
            --insertion;
        }
        endpoints[insertion] = value;
    }
    u32 byte_offset = 0;
    u32 utf16_offset = 0;
    for (u32 endpoint = 0; endpoint < endpoint_count; ++endpoint) {
        const TextHitEndpoint& item = endpoints[endpoint];
        while (byte_offset < item.byte_offset) {
            const Utf8DecodeResult decoded =
                utf8_decode_one(input.data + byte_offset, input.bytes - byte_offset);
            if (!decoded.valid || decoded.width == 0 || decoded.width > item.byte_offset - byte_offset) {
                return false;
            }
            byte_offset += decoded.width;
            utf16_offset += decoded.codepoint <= 0xFFFF ? 1u : 2u;
        }
        if (byte_offset != item.byte_offset) {
            return false;
        }
        TextHitRange& hit = resolved[item.hit_index];
        if (!item.end) {
            hit.first_unit = utf16_offset;
        } else {
            if (utf16_offset <= hit.first_unit) {
                return false;
            }
            hit.unit_count = utf16_offset - hit.first_unit;
        }
    }
    return true;
}

bool shaped_text_initialize(ShapedText* text, Slice<TextGlyph> glyphs, Slice<TextRun> runs,
                            Slice<u16> clusters) noexcept {
    if (!text || !glyphs.data || glyphs.count == 0 || glyphs.count > 0xFFFFu || !runs.data ||
        runs.count == 0 || !clusters.data || clusters.count == 0) {
        return false;
    }
    *text = ShapedText{};
    text->glyphs = glyphs.data;
    text->runs = runs.data;
    text->clusters = clusters.data;
    text->glyph_capacity = glyphs.count;
    text->run_capacity = runs.count;
    text->cluster_capacity = clusters.count;
    return true;
}

bool shaped_text_bind_decorations(ShapedText* text, Slice<TextDecoration> decorations) noexcept {
    if (!text || !text->glyphs || !decorations.data || decorations.count == 0) {
        return false;
    }
    text->decorations = decorations.data;
    text->decoration_capacity = decorations.count;
    text->decoration_count = 0;
    return true;
}

bool shaped_text_bind_hit_regions(ShapedText* text, Slice<TextHitRegion> hit_regions) noexcept {
    if (!text || !text->glyphs || !hit_regions.data || hit_regions.count == 0) {
        return false;
    }
    text->hit_regions = hit_regions.data;
    text->hit_region_capacity = hit_regions.count;
    text->hit_region_count = 0;
    return true;
}

void shaped_text_reset(ShapedText* text) noexcept {
    if (!text) {
        return;
    }
    for (u32 index = 0; index < text->run_count; ++index) {
        if(text->runs[index].native_font)native_font_run_release(native_font_session_runs(text->native_fonts()),text->runs[index].native_face);
        text->runs[index]={};
    }
    auto* fonts=text->native_fonts();if(fonts&&!native_font_session_release(&fonts))log_write(LogLevel::error,"text","native_shaped_session_has_live_handles");text->set_native_fonts(fonts);
    text->glyph_count = 0;
    text->run_count = 0;
    text->cluster_count = 0;
    text->decoration_count = 0;
    text->hit_region_count = 0;
    text->width = 0.0f;
    text->width_including_trailing_whitespace = 0.0f;
    text->height = 0.0f;
    text->line_count = 0;
    text->set_unsupported_inline_object(false);
}

TextShapeResult text_system_initialize(TextSystem* system, const wchar_t* family_name, float em_size,
                                       const wchar_t* locale_name) noexcept {
    if (!system || !family_name || !family_name[0] || !locale_name || !layout_extent_valid(em_size)) {
        return shape_result(TextShapeError::invalid_input, E_INVALIDARG);
    }
    *system = TextSystem{};
    u32 units=0;while(units<256&&family_name[units])++units;
    // The native app policy is en-US. Reject other locales.
    constexpr wchar_t locale[]=L"en-us";u32 i=0;
    while(locale[i]&&locale_name[i]&&(locale_name[i]|32)==locale[i])++i;
    if(!units||units==256||locale[i]||locale_name[i]||em_size>250000)
        return shape_result(TextShapeError::invalid_input,E_INVALIDARG);
    if(!native_font_session_create(&system->native_fonts)||
       !native_text_context_initialize(&system->native_text,system->native_fonts)||
       !native_text_format_create(system->native_text,
           {{reinterpret_cast<const u16*>(family_name),units},0,{400,5,0},em_size,em_size*4,true},&system->native_format)){
        text_system_release(system);return shape_result(TextShapeError::unsupported,E_OUTOFMEMORY);
    }
    system->em_size=em_size;return shape_result(TextShapeError::none);
}

void text_system_release(TextSystem* system) noexcept {
    if (!system) {
        return;
    }
    native_text_context_release(&system->native_text);
    system->native_format=0;
    if(system->native_fonts){const auto stats=native_font_session_stats(system->native_fonts);
        log_u64(LogLevel::info,"text","native_font_peak_files",stats.peak_files);
        log_u64(LogLevel::info,"text","native_font_peak_faces",stats.peak_faces);
        log_u64(LogLevel::info,"text","native_font_peak_instances",stats.peak_instances);
        log_u64(LogLevel::info,"text","native_font_peak_mapped_bytes",stats.peak_mapped_bytes);
        log_u64(LogLevel::info,"text","native_font_metadata_commit",stats.metadata_committed);
        if(!native_font_session_release(&system->native_fonts))log_write(LogLevel::error,"text","native_font_session_has_live_handles");}
    system->em_size = 0.0f;
    system->typography = nullptr;
}

bool glyph_outline_initialize(GlyphOutline* outline, Slice<GlyphPathCommand> commands) noexcept {
    if (!outline || !commands.data || commands.count == 0) {
        return false;
    }
    *outline = GlyphOutline{};
    outline->commands = commands.data;
    outline->capacity = commands.count;
    return true;
}

void glyph_outline_reset(GlyphOutline* outline) noexcept {
    if (!outline) {
        return;
    }
    outline->count = 0;
    outline->rejected = 0;
    outline->minimum_x = 0.0f;
    outline->minimum_y = 0.0f;
    outline->maximum_x = 0.0f;
    outline->maximum_y = 0.0f;
    outline->fill_rule = GlyphFillRule::alternate;
    outline->has_bounds = false;
}

TextFoldedStyles text_fold_style_sizes(ConstSlice<TextStyleSpan> styles, Slice<TextStyleSpan> scratch) noexcept {
    bool sized = false;
    for (u32 i = 0; i < styles.count; ++i) {
        if (styles[i].size >= text_size_count)
            return {{}, false};
        sized = sized || styles[i].size != 0;
    }
    if (!sized)
        return {styles, true};
    if (styles.count > scratch.count || !scratch.data)
        return {{}, false};
    for (u32 i = 0; i < styles.count; ++i) {
        scratch[i] = styles[i];
        scratch[i].flags = text_style_fold_size(styles[i].flags, styles[i].size);
        scratch[i].size = 0;
    }
    return {{scratch.data, styles.count}, true};
}

TextShapingView::TextShapingView(Utf8View input) noexcept : text(input) {
    if (!text_browser_invisibles())
        return;
    Slice<char> storage{local_, sizeof(local_)};
    if (input.bytes > sizeof(local_)) {
        // Probe without output first: long text rarely hides anything, and
        // its punctuation/emoji lead bytes alone must not cost a heap copy.
        if (text_hide_default_ignorables(input, {}) != ~0u)
            return;
        heap_ = static_cast<char*>(HeapAlloc(process_heap(), 0, input.bytes));
        if (!heap_)
            return;
        storage = {heap_, input.bytes};
    }
    const u32 written = text_hide_default_ignorables(input, storage);
    if (written && written != ~0u)
        text = Utf8View{storage.data, written};
}

TextShapingView::~TextShapingView() noexcept {
    if (heap_)
        HeapFree(process_heap(), 0, heap_);
}

namespace {
// The client has no shaper besides the native one, so a scalar or sequence the
// native items cannot express must not fail its whole text item: one failing
// embed field failed the frame's content text, blanked the message list behind
// the shell fallback and re-ran the full rebuild every 1.6 s. UTF-8 and UTF-16
// offsets are unchanged, so styles, hit spans and cluster maps stay valid. The
// bare engine (policy off) keeps rejecting such input.
bool native_fallback_scalar(u32 cp) noexcept {return cp>126||(cp<32&&cp!=9&&cp!=10&&cp!=13);}
bool native_scalar_shapes(const TextSystem& system,u32 cp) noexcept {
    char text[6];u32 n=0;text[n++]='a';n+=utf8_encode(cp,text+n);text[n++]='a';
    TextMeasureResult measured{};
    const bool ok=native_text_candidate_line(system,{text,n},1048576.0f,1048576.0f,nullptr,&measured,TextWrapping::no_wrap);
    (void)native_text_candidate_take_output_failure(system);
    return ok;
}
bool native_chunk_shapes(const TextSystem& system,Utf8View text) noexcept {
    TextMeasureResult measured{};
    const bool ok=native_text_candidate_line(system,text,1048576.0f,1048576.0f,nullptr,&measured,TextWrapping::no_wrap);
    (void)native_text_candidate_take_output_failure(system);
    return ok;
}
// Marks, format controls and emoji modifiers only shape after their base; a
// probe after 'a' says nothing about them (U+FE0F after an emoji is fine).
bool native_probe_alone(u32 cp) noexcept {
    if(cp-0x1f3fbu<5)return false;
    const auto* p=unicode_properties(cp);if(!p)return false;
    const auto c=p->general_category();
    return c!=UnicodeGeneralCategory::mn&&c!=UnicodeGeneralCategory::mc&&c!=UnicodeGeneralCategory::me&&
           c!=UnicodeGeneralCategory::cf&&c!=UnicodeGeneralCategory::cs;
}
// Missing glyph of the same UTF-8 and UTF-16 length: a space for a control,
// else an unassigned scalar (U+0378, U+2B74, U+2FFFD), the glyph a browser
// draws for a character no font can show.
const char* native_missing_glyph(u32 width) noexcept {
    return width==1?" ":width==2?"\xcd\xb8":width==3?"\xe2\xad\xb4":"\xf2\xaf\xbf\xbd";
}
// Invisible of the same length (as text_hide_default_ignorables uses) for the
// rest of a cluster that already shows one missing glyph.
const char* native_invisible(u32 width) noexcept {return width==3?"\xe2\x80\x8c":"\xcd\x8f\xcd\x8f";}
// Replaces each grapheme cluster in [begin,end) that holds a scalar beyond
// printable ASCII by one missing glyph (plus invisibles); returns true when
// anything changed. ASCII clusters stay.
bool native_substitute_clusters(char* out,u32 begin,u32 end) noexcept {
    UnicodeGraphemeState state{};bool changed=false,shown=false;
    for(u32 at=begin;at<end;){
        const auto cp=utf8_decode_one(out+at,end-at);if(!cp.valid||!cp.width)return changed;
        bool boundary=true;(void)unicode_grapheme_next(&state,cp.codepoint,&boundary);
        if(boundary)shown=false;
        if(native_fallback_scalar(cp.codepoint)&&cp.width>1){
            const char* source=shown?native_invisible(cp.width):native_missing_glyph(cp.width);shown=true;
            for(u32 i=0;i<cp.width;++i)out[at+i]=source[i];changed=true;
        }else if(cp.width==1&&native_fallback_scalar(cp.codepoint)){out[at]=' ';changed=true;}
        at+=cp.width;
    }
    return changed;
}
void log_native_fallback(u32 stage,u32 bytes) noexcept {
    static u32 logged=0;
    if(logged>=32)return;
    ++logged;
    log_u64(LogLevel::warning,"text","native_fallback_stage",stage);
    log_u64(LogLevel::warning,"text","native_fallback_bytes",bytes);
}
} // namespace
TextNativeFallback::~TextNativeFallback() noexcept {if(heap_)HeapFree(process_heap(),0,heap_);}
bool TextNativeFallback::build(const TextSystem& system,Utf8View input,u32 stage) noexcept {
    if(!text_browser_invisibles()||stage>=stages||!input.data||!input.bytes)return false;
    char* out=local_;
    if(input.bytes>sizeof(local_)){
        if(heap_bytes_<input.bytes){
            if(heap_)HeapFree(process_heap(),0,heap_);
            heap_=static_cast<char*>(HeapAlloc(process_heap(),0,input.bytes));heap_bytes_=heap_?input.bytes:0;
            if(!heap_)return false;
        }
        out=heap_;
    }
    // Stages: 0 failing base scalars; 1 also bisect to failing clusters;
    // 2 also mirrored brackets; 3 also the smallest failing range whose halves
    // shape alone; 4 everything beyond printable ASCII.
    const bool everything=stage==4,brackets=stage==2;bool changed=false;
    for(u32 at=0;at<input.bytes;){
        const auto cp=utf8_decode_one(input.data+at,input.bytes-at);if(!cp.valid||!cp.width||cp.width>4)return false;
        bool replace=false;const char* bracket=nullptr;
        if(brackets&&cp.width!=2&&cp.width!=4){const auto type=unicode_bidi_bracket(cp.codepoint).type;
            // Mirrored lookalikes that do not pair: < > or U+2039 U+203A.
            if(type==UnicodeBracketType::open)bracket=cp.width==1?"<":"\xe2\x80\xb9";
            else if(type==UnicodeBracketType::close)bracket=cp.width==1?">":"\xe2\x80\xba";}
        if(!bracket&&native_fallback_scalar(cp.codepoint)){
            if(everything)replace=true;
            else if(cp.width==1||native_probe_alone(cp.codepoint)){
                // Open-addressed cache; past the probe bound a scalar is
                // assumed to shape (bisection still finds a failing cluster).
                u32 slot=(cp.codepoint*2654435761u)>>24;bool known=false;
                for(u32 n=0;n<256&&!known;++n,slot=(slot+1)&255){
                    const u32 entry=scalars_[slot];
                    if(!entry){
                        if(scalar_probes_>=160)break;
                        ++scalar_probes_;const bool shapes=native_scalar_shapes(system,cp.codepoint);
                        scalars_[slot]=cp.codepoint<<1|(shapes?1u:0u);replace=!shapes;known=true;
                    }else if(entry>>1==cp.codepoint){replace=!(entry&1u);known=true;}
                }
            }
        }
        const char* source=bracket?bracket:replace?native_missing_glyph(cp.width):input.data+at;
        for(u32 i=0;i<cp.width;++i)out[at+i]=source[i];
        // Later stages repeat the first stage's replacements; the bracket
        // stage is a new substitute only when it replaces a bracket.
        changed|=brackets?bracket!=nullptr:stage==0||everything?replace:false;at+=cp.width;
    }
    // The first stage's text failed as a whole: bisect to the clusters that fail.
    chunk_probes_=0;
    if(stage>=1&&stage<=3&&narrow(system,out,0,input.bytes,stage!=2,stage==3)&&stage!=2)changed=true;
    text={out,input.bytes};
    return changed;
}

// Bisects a failing range at the space or line break nearest its middle,
// else at the nearest grapheme boundary, down to single clusters, which are
// substituted. Probes are bounded per build; past the bound a failing range is
// substituted whole, as is a failing range whose halves both shape when
// `unresolved` is set (a failure that needs the context across the cut).
// Returns true when anything changed.
bool TextNativeFallback::narrow(const TextSystem& system,char* out,u32 begin,u32 end,bool known_failing,bool unresolved) noexcept {
    if(begin>=end)return false;
    if(!known_failing){
        if(chunk_probes_>=192)return native_substitute_clusters(out,begin,end);
        ++chunk_probes_;
        if(native_chunk_shapes(system,{out+begin,end-begin}))return false;
    }
    const u32 middle=begin+(end-begin)/2;u32 split=0,best=~0u;
    for(u32 at=begin+1;at<end;++at){
        const char c=out[at-1];
        if(c==' '||c=='\n'||c=='\t'){const u32 d=at>middle?at-middle:middle-at;if(d<best){best=d;split=at;}}
    }
    if(!split){
        UnicodeGraphemeState state{};
        for(u32 at=begin;at<end;){
            const auto cp=utf8_decode_one(out+at,end-at);if(!cp.valid||!cp.width)break;
            bool boundary=true;(void)unicode_grapheme_next(&state,cp.codepoint,&boundary);
            if(boundary&&at>begin){const u32 d=at>middle?at-middle:middle-at;if(d<best){best=d;split=at;}}
            at+=cp.width;
        }
    }
    if(!split)return native_substitute_clusters(out,begin,end);
    const bool left=narrow(system,out,begin,split,false,unresolved);
    const bool right=narrow(system,out,split,end,false,unresolved);
    if(unresolved&&!left&&!right)return native_substitute_clusters(out,begin,end);
    return left||right;
}

namespace {
// Spans must be ordered, disjoint and in bounds, starting and ending on scalar
// boundaries of the already validated UTF-8 (never on a continuation byte).
bool styles_valid(Utf8View input, ConstSlice<TextStyleSpan> styles) noexcept {
    u32 previous_end = 0;
    for (u32 i = 0; i < styles.count; ++i) {
        const auto& style = styles[i];
        if (style.byte_offset < previous_end || style.byte_offset > input.bytes ||
            style.byte_count > input.bytes - style.byte_offset)
            return false;
        previous_end = style.byte_offset + style.byte_count;
        if ((style.byte_offset < input.bytes && (static_cast<u8>(input.data[style.byte_offset]) & 0xc0) == 0x80) ||
            (previous_end < input.bytes && (static_cast<u8>(input.data[previous_end]) & 0xc0) == 0x80))
            return false;
    }
    return true;
}
TextShapeResult text_shape_utf8_internal(TextSystem* system, Utf8View source,
                                         ConstSlice<TextStyleSpan> sized_styles, ConstSlice<TextHitSpan> hits,
                                         float maximum_width, float maximum_height,
                                         Slice<wchar_t> utf16_scratch, ShapedText* output,
                                         TextWrapping wrapping, TextOverflow overflow) noexcept {
    const TextShapingView shaping(source);
    const Utf8View input = shaping.text;
    TextStyleSpan folded[kTextStyleSpanMaximum];
    const auto folding = text_fold_style_sizes(sized_styles, {folded, kTextStyleSpanMaximum});
    if (!folding.valid)
        return shape_result(TextShapeError::invalid_input, E_INVALIDARG);
    const ConstSlice<TextStyleSpan> styles = folding.styles;
    if (!usable_text_system(system) || !output || !output->glyphs || !output->runs ||
        !output->clusters || !layout_extent_valid(maximum_width) || !layout_extent_valid(maximum_height) ||
        (!styles.data && styles.count != 0) || styles.count > kTextStyleSpanMaximum ||
        (!hits.data && hits.count != 0) || hits.count > kTextHitSpanMaximum ||
        wrapping > TextWrapping::no_wrap || overflow > TextOverflow::ellipsis) {
        return shape_result(TextShapeError::invalid_input, E_INVALIDARG);
    }
    shaped_text_reset(output);
    const TextShapeResult validated = validate_text_input(input, utf16_scratch);
    if (!validated.ok()) {
        return validated;
    }
    const u32 utf16_units = validated.utf16_units;
    if (!styles_valid(input, styles))
        return shape_result(TextShapeError::invalid_input, E_INVALIDARG, utf16_units);
    if (utf16_units > output->cluster_capacity) {
        return shape_result(TextShapeError::output_too_small, HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
                            utf16_units);
    }
    output->cluster_count = utf16_units;
    for (u32 index = 0; index < utf16_units; ++index) {
        output->clusters[index] = 0xFFFF;
    }
    if (utf16_units == 0) {
        return shape_result(TextShapeError::none);
    }
    TextHitRange resolved_hits[kTextHitSpanMaximum];
    if (!resolve_text_hits(input, hits, resolved_hits)) {
        shaped_text_reset(output);
        return shape_result(TextShapeError::invalid_input, E_INVALIDARG, utf16_units);
    }
    const auto native_shape=[&](Utf8View text) noexcept {
        (void)native_text_candidate_take_output_failure(*system);
        return overflow==TextOverflow::ellipsis?
            native_text_candidate_ellipsis(*system,text,styles,maximum_width,maximum_height,output,wrapping,{resolved_hits,hits.count}):
            !styles.count&&!hits.count?
            native_text_candidate_line(*system,text,maximum_width,maximum_height,output,nullptr,wrapping,overflow):
            native_text_candidate_styled(*system,text,styles,maximum_width,maximum_height,output,nullptr,wrapping,overflow,{resolved_hits,hits.count});
    };
    if(native_shape(input))
        return shape_result(TextShapeError::none,S_OK,utf16_units,output);
    bool exhausted=native_text_candidate_take_output_failure(*system);
    TextNativeFallback fallback;
    for(u32 stage=0;stage<TextNativeFallback::stages&&!exhausted;++stage){
        if(!fallback.build(*system,input,stage))continue;
        shaped_text_reset(output);output->cluster_count=utf16_units;
        for(u32 index=0;index<utf16_units;++index)output->clusters[index]=0xFFFF;
        if(native_shape(fallback.text)){log_native_fallback(stage+1,input.bytes);return shape_result(TextShapeError::none,S_OK,utf16_units,output);}
        exhausted=native_text_candidate_take_output_failure(*system);
    }
    shaped_text_reset(output);
    return shape_result(exhausted?TextShapeError::output_too_small:TextShapeError::unsupported,exhausted?HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER):E_NOTIMPL,utf16_units);
}

TextMeasureResult measure_result(TextShapeError error, HRESULT native_error, u32 utf16_units = 0) noexcept {
    return TextMeasureResult{native_error, 0.0f, 0.0f, 0.0f, utf16_units, 0, error};
}
} // namespace

TextShapeResult text_shape_utf8(TextSystem* system, Utf8View input, float maximum_width,
                                float maximum_height, Slice<wchar_t> utf16_scratch, ShapedText* output,
                                TextWrapping wrapping, TextOverflow overflow) noexcept {
    return text_shape_utf8_internal(system, input, ConstSlice<TextStyleSpan>{}, ConstSlice<TextHitSpan>{},
                                    maximum_width, maximum_height, utf16_scratch, output, wrapping,
                                    overflow);
}

TextShapeResult text_shape_utf8_styled(TextSystem* system, Utf8View input, ConstSlice<TextStyleSpan> styles,
                                       float maximum_width, float maximum_height,
                                       Slice<wchar_t> utf16_scratch, ShapedText* output,
                                       TextWrapping wrapping, TextOverflow overflow) noexcept {
    return text_shape_utf8_internal(system, input, styles, ConstSlice<TextHitSpan>{}, maximum_width,
                                    maximum_height, utf16_scratch, output, wrapping, overflow);
}

TextShapeResult text_shape_utf8_interactive(TextSystem* system, Utf8View input,
                                            ConstSlice<TextStyleSpan> styles, ConstSlice<TextHitSpan> hits,
                                            float maximum_width, float maximum_height,
                                            Slice<wchar_t> utf16_scratch, ShapedText* output,
                                            TextWrapping wrapping, TextOverflow overflow) noexcept {
    return text_shape_utf8_internal(system, input, styles, hits, maximum_width, maximum_height,
                                    utf16_scratch, output, wrapping, overflow);
}

TextMeasureResult text_measure_utf8_styled(TextSystem* system, Utf8View source,
                                           ConstSlice<TextStyleSpan> sized_styles, float maximum_width,
                                           float maximum_height, Slice<wchar_t> utf16_scratch,
                                           TextWrapping wrapping) noexcept {
    const TextShapingView shaping(source);
    const Utf8View input = shaping.text;
    TextStyleSpan folded[kTextStyleSpanMaximum];
    const auto folding = text_fold_style_sizes(sized_styles, {folded, kTextStyleSpanMaximum});
    if (!folding.valid)
        return measure_result(TextShapeError::invalid_input, E_INVALIDARG);
    const ConstSlice<TextStyleSpan> styles = folding.styles;
    if (!usable_text_system(system) || !layout_extent_valid(maximum_width) ||
        !layout_extent_valid(maximum_height) || (!styles.data && styles.count != 0) ||
        styles.count > kTextStyleSpanMaximum || wrapping > TextWrapping::no_wrap) {
        return measure_result(TextShapeError::invalid_input, E_INVALIDARG);
    }
    const TextShapeResult validated = validate_text_input(input, utf16_scratch);
    if (!validated.ok()) {
        return measure_result(validated.error, validated.native_error, validated.utf16_units);
    }
    const u32 utf16_units = validated.utf16_units;
    if (!styles_valid(input, styles))
        return measure_result(TextShapeError::invalid_input, E_INVALIDARG, utf16_units);
    if (utf16_units == 0) {
        return measure_result(TextShapeError::none, S_OK);
    }
    TextMeasureResult native_measure{};
    const auto native_measured=[&](Utf8View text) noexcept {
        return styles.count?native_text_candidate_styled(*system,text,styles,maximum_width,maximum_height,nullptr,&native_measure,wrapping):
            native_text_candidate_line(*system,text,maximum_width,maximum_height,nullptr,&native_measure,wrapping);
    };
    if(native_measured(input))return native_measure;
    // The same substitutes shaping uses, so measured and drawn text agree.
    TextNativeFallback fallback;
    for(u32 stage=0;stage<TextNativeFallback::stages;++stage){
        if(!fallback.build(*system,input,stage))continue;
        native_measure={};
        if(native_measured(fallback.text)){log_native_fallback(stage+1,input.bytes);return native_measure;}
    }
    (void)native_text_candidate_take_output_failure(*system);
    return measure_result(TextShapeError::unsupported,E_NOTIMPL,utf16_units);
}
} // namespace maicord
