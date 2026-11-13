#pragma once
#include "maicord/text/scripts/indic_plan.hpp"
// Indic character classes, HarfBuzz-model text preparation and the syllable scanner, shared by the
// Indic run (indic_run.cpp) and the tests' boundary oracle (tests/support/indic_cluster_boundaries.hpp).
namespace maicord {
// Syllabic categories of the Indic syllable grammar (HarfBuzz hb-ot-shaper-indic-machine.rl numbering,
// with SMPst after CS), derived from Unicode IndicSyllabicCategory.
enum class IndicCategory : u8 {x,c,v,n,h,zwnj,zwj,m,sm,a,placeholder,dotted_circle,rs,mpst,repha,ra,cm,symbol,cs,smpst};
// Visual positions in a syllable from left to right, derived from IndicPositionalCategory and the
// Microsoft Indic shaping specs' matra reordering classes (HarfBuzz ot_position_t).
enum class IndicPosition : u8 {start,ra_to_become_reph,pre_m,pre_c,base_c,after_main,above_c,before_sub,below_c,after_sub,
    before_post,post_c,after_post,smvd,end};
struct IndicCharacter {IndicCategory category;IndicPosition position;};
IndicCharacter indic_character(u32 scalar) noexcept;
// Consonant-like categories: the base candidates of a syllable.
constexpr u32 indic_consonants=1u<<static_cast<u32>(IndicCategory::c)|1u<<static_cast<u32>(IndicCategory::cs)|1u<<static_cast<u32>(IndicCategory::ra)|
    1u<<static_cast<u32>(IndicCategory::cm)|1u<<static_cast<u32>(IndicCategory::v)|1u<<static_cast<u32>(IndicCategory::placeholder)|
    1u<<static_cast<u32>(IndicCategory::dotted_circle);
// One prepared character: scalar (bits 0-20), category (21-25) and position (26-29), plus its UTF-16
// source offset in the run. Inserted dotted circles take the source of the character they precede.
struct IndicPiece {
    u32 value,source;
    u32 scalar() const noexcept {return value&0x1fffffu;}
    IndicCategory category() const noexcept {return static_cast<IndicCategory>((value>>21)&31u);}
    IndicPosition position() const noexcept {return static_cast<IndicPosition>(value>>26);}
};
static_assert(sizeof(IndicPiece)==8);
inline IndicPiece indic_piece(u32 scalar,u32 source) noexcept {
    const auto c=indic_character(scalar);
    return {scalar|static_cast<u32>(c.category)<<21|static_cast<u32>(c.position)<<26,source};
}
// Outcome of the longest syllable match: an Indic (consonant, vowel or standalone) syllable, a broken
// cluster that needs a dotted circle, or a plain cluster (symbol, SMPst or any other character).
enum class IndicSyllable : u8 {none,indic,broken,plain};
// Where a reader resumes: the start of the current normalization unit and the pieces already taken.
struct IndicReaderMark {u32 byte,units,previous,before,read;};
// HarfBuzz text preparation for one run, streamed: the Indic vowel-constraint dotted circles
// (hb-ot-shaper-vowel-constraints), canonical decomposition where the font maps the parts, stable
// combining-class order and recomposition where the font maps the result (hb-ot-shape-normalize,
// mode composed-diacritics-no-short-circuit with the Indic decompose/compose exceptions). Without a cmap
// every part and result counts as mapped. Text is valid UTF-8; next() reports malformed input.
class IndicReader {
public:
    IndicReader(SfntIndicProfile profile,Utf8View text,const SfntCmap* cmap) noexcept : profile_(profile),text_(text),cmap_(cmap) {}
    // The next piece without taking it; *more is false at the end of the text.
    SfntStatus peek(IndicPiece* piece,bool* more) noexcept {
        if(read_==count_){const auto status=build();if(status!=SfntStatus::ok)return status;}
        *more=read_<count_;if(*more)*piece=unit_[read_];return SfntStatus::ok;
    }
    void take() noexcept {++read_;}
    IndicReaderMark mark() const noexcept {return {unit_byte_,unit_units_,unit_previous_,unit_before_,read_};}
    SfntStatus resume(const IndicReaderMark& mark) noexcept;
private:
    SfntStatus build() noexcept;
    SfntStatus mapped(u32 scalar,bool* present) const noexcept;
    SfntStatus decompose(u32 scalar,u32 source,u32* count) noexcept;
    SfntStatus compose() noexcept;
    bool simple() noexcept;
    SfntIndicProfile profile_;Utf8View text_;const SfntCmap* cmap_;
    u32 byte_=0,units_=0,previous_=0,before_=0;
    u32 unit_byte_=0,unit_units_=0,unit_previous_=0,unit_before_=0;
    u32 count_=0,read_=0;
    // The scalar that ended the last unit, decoded at ahead_byte_ (the next unit's first).
    u32 ahead_byte_=0xffffffffu,ahead_scalar_=0,ahead_width_=0;
    u8 simple_=2; // 2 unknown, else whether the run is simple
    // A starter (a circle and up to three pieces) and the marks after it. A combining run longer than 32
    // marks is not sorted (HB_OT_SHAPE_MAX_COMBINING_MARKS); one more mark detects that, and a full
    // unit continues in the next one.
    IndicPiece unit_[40];
};
// Scans the longest syllable at the reader. Every scanned piece, lookahead included, is stored in out;
// the first *length of them are the syllable, and the reader continues after it. Zero length means
// the end of the text. Lookahead beyond out is output_too_small.
SfntStatus indic_read_syllable(IndicReader&,Slice<IndicPiece> out,u32* length,IndicSyllable* outcome) noexcept;
}
