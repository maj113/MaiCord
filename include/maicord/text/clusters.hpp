#pragma once
#include "maicord/text/glyph_buffer.hpp"

namespace maicord {
// Stable source indices survive glyph edits. Current cluster identity is the
// nearest surviving boundary at or before that source index.
struct SfntClusterGlyph {SfntGsubGlyph glyph;u32 source;};
struct SfntClusterMap {Slice<u64> storage;u32 text_count,levels;};
struct SfntClusterMerge {u32 first,last;u64 commit_work;};
static_assert(sizeof(SfntClusterGlyph)==8 && sizeof(SfntClusterMap)==24 && sizeof(SfntClusterMerge)==16);
u32 sfnt_cluster_map_words(u32 text_count) noexcept;
// The word count and level count a map over text_count units needs, framed on
// storage without touching its bits: for callers whose pass already wrote them.
SfntClusterMap sfnt_cluster_map_frame(u64* storage,u32 text_count) noexcept;
// Empty boundaries means one initial cluster per source unit; otherwise supply
// sorted unique starts including zero for nonempty text (the end is implicit).
// Units are caller-defined (normally UTF-16). Storage is borrowed, aligned and
// mutable. Later queries need a successful open and no direct mutation of the
// map or its storage; boundaries and output are disjoint from that storage.
SfntStatus sfnt_cluster_map_open(u32 text_count,ConstSlice<u32> boundaries,Slice<u64> storage,
                                 SfntClusterMap* output,u64 work_limit=1000000) noexcept;
SfntStatus sfnt_cluster_map_find(const SfntClusterMap& map,u32 source,u32* first,u32* end) noexcept;
// Start of the cluster holding a validated source, or 0xffffffff if none.
u32 sfnt_cluster_map_start(const SfntClusterMap& map,u32 source) noexcept;
inline bool sfnt_cluster_map_starts(const SfntClusterMap& map,u32 source) noexcept {return (map.storage.data[source>>6]>>(source&63))&1;}
// Joins the clusters containing the inclusive source range [first,last]. Plan is
// read-only and returns the work it used. Commit cannot fail when passed
// that plan and the same unchanged map. Reserve commit_work before other edits.
SfntStatus sfnt_cluster_map_plan(const SfntClusterMap& map,u32 first,u32 last,SfntClusterMerge* output,
                                 u64 work_limit=1000000,u64* work_used=nullptr) noexcept;
void sfnt_cluster_map_commit(SfntClusterMap* map,const SfntClusterMerge& plan) noexcept;
// Insert a boundary at a caller-validated source-unit boundary. This can split an
// already merged cluster; it does not recover consumed glyph source indices.
// Reserve map.levels work before a surrounding glyph edit. Insufficient work or
// an invalid source fails before mutation. An existing boundary is a valid no-op.
// No outstanding merge plan may be committed after this mutation.
SfntStatus sfnt_cluster_map_split(SfntClusterMap* map,u32 source,u64 work_limit=1000000) noexcept;
// Export first physical glyph index per source cluster, and optionally one byte
// per glyph marking that first occurrence (not DirectWrite isClusterStart, which
// is separate shaping policy). Glyph records begin with SfntClusterGlyph; extra
// metadata is ignored. Cluster membership may interleave in glyph order (e.g. a
// decomposed ligature followed by its component marks); every source cluster
// must have a glyph. Inputs, outputs and storage are disjoint; outputs are
// valid only on success.
SfntStatus sfnt_cluster_map_export(const SfntClusterMap& map,SfntGlyphView glyphs,Slice<u32> character_glyphs,
                                   Slice<u8> glyph_starts={},u64 work_limit=1000000) noexcept;
}
