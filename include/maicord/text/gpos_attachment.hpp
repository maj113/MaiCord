#pragma once
#include "maicord/text/gpos.hpp"

namespace maicord {
// Immutable borrowed views for GPOS types 3..6. Mark classes here are local
// attachment classes, not GDEF glyph classes or mark-attachment filter classes.
struct SfntGposAttachment {
    SfntCoverage coverage[2];ConstSlice<u8> arrays[2];u32 glyph_limit;
    u16 counts[2],class_count;u8 type,reserved;
};
struct SfntGposCursiveAnchors {SfntAnchor entry,exit;bool covered;};
struct SfntGposMarkAnchor {SfntAnchor anchor;u16 mark_class;bool covered;};
struct SfntGposTargetAnchors {ConstSlice<u8> parent;u32 offset;u16 component_count,class_count;bool covered;};
struct SfntGposAttachmentLookup {ConstSlice<SfntGposAttachment> subtables;SfntLookupFilter filter;};
static_assert(sizeof(SfntGposAttachment)==80 && sizeof(SfntGposCursiveAnchors)==40 && sizeof(SfntGposMarkAnchor)==24 && sizeof(SfntGposTargetAnchors)==32);
// Validates coverage, marks, cursive anchors and matrix dimensions under one
// work budget. Target anchor payloads are checked only when queried.
SfntStatus sfnt_gpos_attachment_open(ConstSlice<u8> bytes,u16 type,u32 glyph_limit,SfntGposAttachment* output,
                                     u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Queries require successfully prepared, unchanged views. Target queries validate
// the selected anchor and device offsets before exposing a view. Uncovered glyphs yield
// an empty result with covered=false. A covered cursive glyph can have either or
// both anchors absent. Target anchor offsets may also be null (empty Anchor).
SfntStatus sfnt_gpos_cursive_anchors(const SfntGposAttachment& sub,u16 glyph,SfntGposCursiveAnchors* output) noexcept;
SfntStatus sfnt_gpos_mark_anchor(const SfntGposAttachment& sub,u16 glyph,SfntGposMarkAnchor* output) noexcept;
SfntStatus sfnt_gpos_target_anchors(const SfntGposAttachment& sub,u16 glyph,SfntGposTargetAnchors* output) noexcept;
// Component is zero for base/mark targets. Ligature component identity must come
// from shaping provenance, not from glyph ID, mark class or a guessed last slot.
// Out-of-range components/classes are invalid_input; no clamping or substitution.
SfntStatus sfnt_gpos_target_anchor(const SfntGposTargetAnchors& target,u16 component,u16 mark_class,SfntAnchor* output) noexcept;
}
