#include "maicord/ui/text/text.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/core/log_event.hpp"
#include "maicord/ui/text/native_text_candidate.hpp"
#include "maicord/text/fonts/session.hpp"
#include "maicord/text/fonts/run.hpp"
#include "maicord/text/layout/visibility.hpp"
#include "maicord/text/unicode/unicode.hpp"
#include "maicord/text/unicode/grapheme.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/heap.hpp"

namespace maicord {
namespace {
// The build inlines only functions marked inline (/Ob1); these sit on every
// shape and measure call.
inline bool usable_text_system(const TextSystem* system) noexcept {
    return system && system->native_text && system->native_format;
}

inline TextShapeResult shape_result(TextShapeError error, u32 utf16_units = 0) noexcept {
    return TextShapeResult{utf16_units, error};
}

inline bool layout_extent_valid(float value) noexcept {
    return value == value && value > 0.0f && value <= 1'048'576.0f;
}

struct TextHitEndpoint {
    u32 byte_offset;
    u16 hit_index;
    bool end;
};

// Bytes before the first non-ASCII one, eight at a time.
inline u32 ascii_prefix(Utf8View input) noexcept {
    u32 ascii = 0;
    for (; ascii + 8 <= input.bytes; ascii += 8) {
        if (byte_load_le64(reinterpret_cast<const u8*>(input.data) + ascii) & 0x8080808080808080ull)
            break;
    }
    while (ascii < input.bytes && static_cast<u8>(input.data[ascii]) < 0x80u)
        ++ascii;
    return ascii;
}

} // namespace

TextShapeResult validate_text_input(Utf8View input) noexcept {
    if (!input.data && input.bytes != 0) {
        return shape_result(TextShapeError::invalid_input);
    }
    // ASCII, most UI text, is valid with one unit per byte. Validation decodes
    // only from the first non-ASCII byte, a scalar boundary.
    const u32 ascii = ascii_prefix(input);
    if (ascii == input.bytes)
        return shape_result(TextShapeError::none, ascii);
    const Utf8View rest{input.data + ascii, input.bytes - ascii};
    const Utf8Validation validation = utf8_validate(rest);
    if (!validation.valid) {
        return shape_result(TextShapeError::invalid_utf8);
    }
    // In valid UTF-8 only four-byte sequences need a second UTF-16 unit.
    u32 required = ascii + validation.codepoints;
    for (u32 offset = 0; offset < rest.bytes; ++offset) {
        required += static_cast<u8>(rest.data[offset]) >= 0xF0u;
    }
    return shape_result(TextShapeError::none, required);
}

bool resolve_text_hits(Utf8View input, ConstSlice<TextHitSpan> hits, TextHitRange* resolved) noexcept {
    if (hits.count > kTextHitSpanMaximum || (!hits.data && hits.count != 0) ||
        (!resolved && hits.count != 0)) {
        return false;
    }
    if (!hits.count) return true;
    TextHitEndpoint endpoints[kTextHitSpanMaximum * 2];
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

bool shaped_text_initialize(ShapedText* text, Slice<TextGlyph> glyphs, Slice<TextRun> runs, Slice<u16> clusters,
                            Slice<TextDecoration> decorations, Slice<TextHitRegion> hit_regions) noexcept {
    if (!text || !glyphs.data || glyphs.count == 0 || glyphs.count > 0xFFFFu || !runs.data ||
        runs.count == 0 || !clusters.data || clusters.count == 0) {
        return false;
    }
    reset_value<ShapedText>(text);
    text->glyphs = glyphs.data;
    text->runs = runs.data;
    text->clusters = clusters.data;
    text->decorations = decorations.data;
    text->hit_regions = hit_regions.data;
    text->glyph_capacity = glyphs.count;
    text->run_capacity = runs.count;
    text->cluster_capacity = clusters.count;
    text->decoration_capacity = decorations.count;
    text->hit_region_capacity = hit_regions.count;
    return true;
}

void shaped_text_reset(ShapedText* text) noexcept {
    if (!text) {
        return;
    }
    for (u32 index = 0; index < text->run_count; ++index) {
        if(text->runs[index].native_font)native_font_run_release(native_font_session_runs(text->native_fonts()),text->runs[index].native_face);
        reset_value(&text->runs[index]);
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
}

TextShapeResult text_system_initialize(TextSystem* system, const wchar_t* family_name, float em_size) noexcept {
    if (!system || !family_name || !family_name[0] || !layout_extent_valid(em_size)) {
        return shape_result(TextShapeError::invalid_input);
    }
    reset_value<TextSystem>(system);
    u32 units=0;while(units<256&&family_name[units])++units;
    if(units==256||em_size>250000)
        return shape_result(TextShapeError::invalid_input);
    if(!native_font_session_create(&system->native_fonts)||
       !native_text_context_initialize(&system->native_text,system->native_fonts)||
       !native_text_format_create(system->native_text,
           {{reinterpret_cast<const u16*>(family_name),units},0,{400,5,0},em_size,em_size*4,true},&system->native_format)){
        text_system_release(system);return shape_result(TextShapeError::unsupported);
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
        const u64 peaks[]={stats.peak_files,stats.peak_faces,stats.peak_instances,stats.peak_mapped_bytes,stats.metadata_committed};
        LogEvent("native_fonts_released").fields("native_font_peak_files\0native_font_peak_faces\0native_font_peak_instances\0"
            "native_font_peak_mapped_bytes\0native_font_metadata_commit",peaks).write(LogLevel::info,"text");
        if(!native_font_session_release(&system->native_fonts))log_write(LogLevel::error,"text","native_font_session_has_live_handles");}
    system->em_size = 0.0f;
    system->typography = nullptr;
}

bool glyph_outline_initialize(GlyphOutline* outline, Slice<GlyphPathCommand> commands) noexcept {
    if (!outline || !commands.data || commands.count == 0) {
        return false;
    }
    reset_value<GlyphOutline>(outline);
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
    // ASCII has no default-ignorable scalar.
    if (!text_browser_invisibles() || !input.data || ascii_prefix(input) == input.bytes)
        return;
    Slice<char> storage{local_, sizeof(local_)};
    if (input.bytes > sizeof(local_)) {
        // Probe without output first: long text rarely hides anything, and
        // its punctuation/emoji lead bytes alone must not cost a heap copy.
        if (text_hide_default_ignorables(input, {}) != ~0u)
            return;
        heap_ = static_cast<char*>(heap_allocate(input.bytes));
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
        heap_free(heap_);
}

// The fallback stages run only after a text failed to shape whole: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
namespace {
// With browser invisibles on there is no shaper besides the native one, so a
// scalar or sequence the native items cannot express must not fail its whole
// text item. It is replaced by a same-length substitute: UTF-8 and UTF-16
// offsets are unchanged, so styles, hit spans and cluster maps stay valid. The
// bare engine (policy off) keeps rejecting such input.
bool native_fallback_scalar(u32 cp) noexcept {return cp>126||(cp<32&&cp!=9&&cp!=10&&cp!=13);}
bool native_chunk_shapes(const TextSystem& system,Utf8View text) noexcept {
    TextMeasureResult measured{};
    const bool ok=native_text_candidate_line(system,{text,{},1048576.0f,1048576.0f,TextWrapping::no_wrap},nullptr,&measured);
    (void)native_text_candidate_take_output_failure(system);
    return ok;
}
bool native_scalar_shapes(const TextSystem& system,u32 cp) noexcept {
    char text[6];u32 n=0;text[n++]='a';n+=utf8_encode(cp,text+n);text[n++]='a';
    return native_chunk_shapes(system,{text,n});
}
// Marks, format controls and emoji modifiers only shape after their base; a
// probe after 'a' says nothing about them (U+FE0F after an emoji is fine).
bool native_probe_alone(u32 cp) noexcept {
    if(cp-0x1f3fbu<5)return false;
    const auto c=unicode_properties(cp)->general_category(); // cp is a decoded scalar
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
        const bool boundary=unicode_grapheme_boundary(state,cp.codepoint);
        if(boundary)shown=false;
        if(cp.width>1){ // beyond ASCII, so always a fallback scalar
            const char* source=shown?native_invisible(cp.width):native_missing_glyph(cp.width);shown=true;
            for(u32 i=0;i<cp.width;++i)out[at+i]=source[i];changed=true;
        }else if(native_fallback_scalar(cp.codepoint)){out[at]=' ';changed=true;}
        at+=cp.width;
    }
    return changed;
}
void log_native_fallback(u32 stage,u32 bytes) noexcept {
    static u32 logged=0;
    if(logged>=32)return;
    ++logged;
    LogEvent("native_fallback").field("stage",stage).field("bytes",bytes).write(LogLevel::warning,"text");
}
} // namespace
TextNativeFallback::~TextNativeFallback() noexcept {if(heap_)heap_free(heap_);}
bool TextNativeFallback::build(const TextSystem& system,Utf8View input,u32 stage) noexcept {
    if(!text_browser_invisibles()||stage>=stages||!input.data||!input.bytes)return false;
    char* out=local_;
    if(input.bytes>sizeof(local_)){
        if(heap_bytes_<input.bytes){
            if(heap_)heap_free(heap_);
            heap_=static_cast<char*>(heap_allocate(input.bytes));heap_bytes_=heap_?input.bytes:0;
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
        // One UTF-8 scalar (at most four bytes): a vector body only adds size.
#pragma clang loop vectorize(disable) interleave(disable)
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

// Bisects a failing range at the space or line break nearest its middle, else at
// the nearest grapheme boundary, down to single clusters, which are substituted.
// Probes are bounded per build; past the bound a failing range is substituted
// whole. With `unresolved` (a failure that needs the context across the cut) a
// failing range whose halves both shape is substituted whole too.
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
            const bool boundary=unicode_grapheme_boundary(state,cp.codepoint);
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
#pragma clang attribute pop

namespace {
// Spans must be ordered, disjoint and in bounds, starting and ending on scalar
// boundaries of the already validated UTF-8 (never on a continuation byte).
inline bool styles_valid(Utf8View input, ConstSlice<TextStyleSpan> styles) noexcept {
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
struct LayoutRequest {
    TextSystem* system;
    NativeTextRequest native;
    ShapedText* output;
    TextMeasureResult* measure;
    u32 utf16_units;
};

// The output-capacity flag only matters to shaping; measurement never sets it.
__forceinline bool native_layout(const LayoutRequest& r) noexcept {
    if (r.output)
        (void)native_text_candidate_take_output_failure(*r.system);
    if (r.native.overflow == TextOverflow::ellipsis)
        return native_text_candidate_ellipsis(*r.system, r.native, r.output);
    return !r.native.styles.count && !r.native.hits.count
               ? native_text_candidate_line(*r.system, r.native, r.output, r.measure)
               : native_text_candidate_styled(*r.system, r.native, r.output, r.measure);
}

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// The fallback stages (and their 1.5 KB of substitute state) live in their own
// frame, so the first attempt, which nearly always succeeds, does not carry it.
__declspec(noinline) TextShapeResult layout_fallback(const LayoutRequest& r) noexcept {
    const Utf8View input = r.native.text;
    bool exhausted = native_text_candidate_take_output_failure(*r.system) && r.output;
    TextNativeFallback fallback;
    LayoutRequest attempt = r;
    for (u32 stage = 0; stage < TextNativeFallback::stages && !exhausted; ++stage) {
        if (!fallback.build(*r.system, input, stage))
            continue;
        if (r.output)
            shaped_text_reset(r.output);
        else
            reset_value(r.measure);
        attempt.native.text = fallback.text;
        if (native_layout(attempt)) {
            log_native_fallback(stage + 1, input.bytes);
            return shape_result(TextShapeError::none, r.utf16_units);
        }
        exhausted = native_text_candidate_take_output_failure(*r.system) && r.output;
    }
    shaped_text_reset(r.output);
    return shape_result(exhausted ? TextShapeError::output_too_small : TextShapeError::unsupported, r.utf16_units);
}
#pragma clang attribute pop

// Sized spans fold into this many inline records; longer sized lists (rare,
// and shaped at a cost far above an allocation) fold into the process heap.
constexpr u32 kInlineFoldedSpans = 32;
struct FoldedSpans {
    TextStyleSpan local[kInlineFoldedSpans];
    TextStyleSpan* heap{};
    ~FoldedSpans() noexcept {
        if (heap)
            heap_free(heap);
    }
};

// The one front end of shaping (output) and measurement (measure): both see
// the same hidden ignorables, folded styles and fallback substitutes, so
// measured and drawn text agree. Forced inline into both entries.
__forceinline TextShapeResult text_layout_utf8(TextSystem* system, const TextLayoutRequest& request,
                                               ShapedText* output, TextMeasureResult* measure) noexcept {
    const ConstSlice<TextStyleSpan> sized_styles = request.styles;
    const ConstSlice<TextHitSpan> hits = measure ? ConstSlice<TextHitSpan>{} : request.hits;
    const TextShapingView shaping(request.text);
    const Utf8View input = shaping.text;
    FoldedSpans folded;
    Slice<TextStyleSpan> fold_scratch{folded.local, kInlineFoldedSpans};
    if (sized_styles.count > kInlineFoldedSpans && sized_styles.count <= kTextStyleSpanMaximum) {
        bool sized = false;
        for (u32 i = 0; i < sized_styles.count && !sized; ++i)
            sized = sized_styles[i].size != 0;
        if (sized) {
            folded.heap = static_cast<TextStyleSpan*>(
                heap_allocate(sized_styles.count * sizeof(TextStyleSpan)));
            if (!folded.heap)
                return shape_result(TextShapeError::unsupported);
            fold_scratch = {folded.heap, sized_styles.count};
        }
    }
    const auto folding = text_fold_style_sizes(sized_styles, fold_scratch);
    const ConstSlice<TextStyleSpan> styles = folding.styles;
    if (!folding.valid || !usable_text_system(system) || !layout_extent_valid(request.maximum_width) ||
        !layout_extent_valid(request.maximum_height) || styles.count > kTextStyleSpanMaximum ||
        (!hits.data && hits.count != 0) || hits.count > kTextHitSpanMaximum ||
        (output && (!output->glyphs || !output->runs || !output->clusters))) {
        return shape_result(TextShapeError::invalid_input);
    }
    if (output)
        shaped_text_reset(output);
    const TextShapeResult validated = validate_text_input(input);
    if (!validated.ok()) {
        return validated;
    }
    const u32 utf16_units = validated.utf16_units;
    if (!styles_valid(input, styles))
        return shape_result(TextShapeError::invalid_input, utf16_units);
    if (output && utf16_units > output->cluster_capacity) {
        return shape_result(TextShapeError::output_too_small, utf16_units);
    }
    if (utf16_units == 0) {
        return shape_result(TextShapeError::none);
    }
    TextHitRange resolved_hits[kTextHitSpanMaximum];
    if (hits.count && !resolve_text_hits(input, hits, resolved_hits)) {
        shaped_text_reset(output);
        return shape_result(TextShapeError::invalid_input, utf16_units);
    }
    // Without hits the array stays unreferenced, so measurement's frame drops it.
    const LayoutRequest layout{system,
                               {input, styles, request.maximum_width, request.maximum_height, request.wrapping,
                                measure ? TextOverflow::clip : request.overflow,
                                hits.count ? ConstSlice<TextHitRange>{resolved_hits, hits.count}
                                           : ConstSlice<TextHitRange>{}},
                               output, measure, utf16_units};
    if (native_layout(layout))
        return shape_result(TextShapeError::none, utf16_units);
    return layout_fallback(layout);
}
} // namespace

TextShapeResult text_shape_utf8(TextSystem* system, const TextLayoutRequest& request, ShapedText* output) noexcept {
    if (!output)
        return shape_result(TextShapeError::invalid_input);
    return text_layout_utf8(system, request, output, nullptr);
}

TextMeasureResult text_measure_utf8(TextSystem* system, const TextLayoutRequest& request) noexcept {
    TextMeasureResult measured{};
    const TextShapeResult result = text_layout_utf8(system, request, nullptr, &measured);
    return result.ok() ? measured : TextMeasureResult{0.0f, 0.0f, 0.0f, result.utf16_units, 0, result.error};
}
} // namespace maicord
