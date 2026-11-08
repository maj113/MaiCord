#pragma once
#include "maicord/text/opentype/lookup_plan.hpp"
#include "maicord/text/shaping/shape_run.hpp"
namespace maicord {
// The scripts HarfBuzz shapes with its syllabic shapers outside the Indic one: Khmer (hb-ot-shaper-khmer), Myanmar
// (hb-ot-shaper-myanmar, for fonts with a mym2 or no GSUB script) and the Universal Shaping Engine
// (hb-ot-shaper-use) for Sinhala and Tibetan. The order is NativeClusterScript's.
enum class SfntSyllabicScript : u8 {sinhala,khmer,myanmar,tibetan};
// Feature masks (SfntGsubGlyph::reserved eligibility) of the masked features. Khmer: pref on Coeng,Ro, the
// below/above/post-base forms after the first character, cfar after a Coeng,Ro. USE: rphf on the first three
// characters (one when it is a repha) and the topographical forms of each cluster.
inline constexpr u8 sfnt_khmer_pref=1,sfnt_khmer_post=2,sfnt_khmer_cfar=4;
inline constexpr u8 sfnt_use_rphf=1,sfnt_use_isol=2,sfnt_use_init=4,sfnt_use_medi=8,sfnt_use_fina=16;
// HarfBuzz's feature order for the script's shaper (collect_features and the default features it keeps), in stages
// separated by the shaper's pauses, for one table: GSUB (rvrn alone first, the per-syllable basic features, then
// the presentation and other global features) or GPOS (one stage: kern, dist, abvm, blwm, mark, mkmk, curs).
// Script records: the script's tags (mym2 then mymr for Myanmar), then DFLT, dflt and latn; language ENG, then the
// default. Lookups of features of one stage run once, in font lookup order, unmasked if any of them is; the
// language system's required feature joins the stage of its tag (the first stage for other tags), unmasked.
// Independent letters turn off liga, clig and kern (Khmer keeps clig: hb-ot-shaper-khmer enables it after the user's
// features). result->script_tag is the selected script (zero when none):
// HarfBuzz uses its default shaper for a Myanmar font whose GSUB selects DFLT, latn or mymr, and for a Sinhala or
// Tibetan font whose GSUB selects DFLT or latn. Scratch: lookup_count u16 cells. Output: four bytes per scheduled
// lookup; GSUB steps carry stage ids (sfnt_syllabic_run's stage policy) and masks. The optional per_syllable
// bitset (at least (lookups+63)/64 words) marks the steps HarfBuzz applies within each syllable. Work is shared
// across selection, counting and emission. Short output reports capacity; any failure leaves output untouched.
SfntStatus sfnt_syllabic_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,SfntSyllabicScript,bool independent,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000,Slice<u64> per_syllable={}) noexcept;
// The features of the script's shaper that match joiners manually in GSUB (HarfBuzz F_MANUAL_ZWJ and
// F_MANUAL_ZWNJ), for SfntShapePlan's substitution_manual set; GPOS keeps the default plan's mark and mkmk set.
ConstSlice<SfntJoinerFeature> sfnt_syllabic_manual_joiners(SfntSyllabicScript) noexcept;
// Hangul feature masks: ljmo, vjmo and tjmo on the jamo of a syllable the font cannot compose, calt on every other
// character (hb-ot-shaper-hangul keeps calt off jamo). Bit 0 stays free for the alphabetic frontend's synthetic bases.
inline constexpr u8 sfnt_hangul_ljmo=2,sfnt_hangul_vjmo=4,sfnt_hangul_tjmo=8,sfnt_hangul_calt=16;
// hb-ot-shaper-hangul's plan for one table: rvrn alone, then ljmo, vjmo, tjmo and calt (masked) with the other
// default features (rand, trak, abvm, blwm, ccmp, locl, mark, mkmk, rlig, clig, curs, dist, kern, liga, rclt) in one
// stage. Script records hang, DFLT, dflt, latn; language ENG, then the default. Contract otherwise as
// sfnt_syllabic_lookup_plan without per-syllable steps; GSUB steps carry stage ids 0 and 1.
SfntStatus sfnt_hangul_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,bool independent,Slice<u16> scratch,
    Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000) noexcept;
// One itemized run of the script in a font HarfBuzz shapes with the script's shaper: HarfBuzz's text preparation
// (vowel constraints, normalization with the shaper's decompositions and compositions, variation sequences),
// syllables from the shaper's grammar, dotted circles before broken clusters, the shaper's reordering and masks,
// GSUB over the run with the per-syllable steps inside each syllable, GPOS, and hidden default ignorables (a
// zero-advance space glyph where substitution left them alone). Steps come from sfnt_syllabic_lookup_plan.
// Clusters are syllables; a syllable that begins inside a grapheme (a mark, ZWJ) joins the cluster before it, as
// HarfBuzz forms clusters from graphemes. Characters are never mirrored: HarfBuzz shapes a left-to-right script
// forced right to left in its own direction. Storage follows sfnt_shape_run; until substitution starts the
// prepared characters borrow positions, the mapped glyphs the replacement scratch, their metadata the flags and
// the syllable starts the targets. No allocation; failure leaves result lengths unset.
SfntStatus sfnt_syllabic_run(SfntSyllabicScript,const SfntShapePlan&,const SfntCmap&,Utf8View,float em,const SfntShapeStorage&,
    SfntShapeRunResult*,ConstSlice<SfntLookupStep> steps,ConstSlice<u64> per_syllable,const SfntShapeLimits& limits={},bool rtl=false) noexcept;
// The cluster starts sfnt_syllabic_run forms (1 at the first scalar of each cluster, 0 elsewhere) for a run of scalars
// in a font that maps every character: full decomposition and recomposition, syllables from the shaper's grammar,
// and no cluster for a syllable that begins inside a grapheme. HarfBuzz's unsafe-to-break flags in a font without
// layout tables give the same starts. At most 256 scalars; 16 work units each.
SfntStatus sfnt_syllabic_boundaries(SfntSyllabicScript,ConstSlice<u32> scalars,Slice<u8> output,u64 work_limit=1000000) noexcept;
}
