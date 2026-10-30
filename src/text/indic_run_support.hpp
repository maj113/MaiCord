#pragma once
#include "maicord/core/utf.hpp"
#include "maicord/text/indic_clusters.hpp"
#include "maicord/text/shape_run.hpp"
#include "use_shape_internal.hpp"
namespace maicord {
// Splits UTF-8 into complete Indic clusters through a 32-scalar window and calls
// cluster(context,scalars,length,followed_by_text) on each in order. The scanner
// cuts every cluster at 31 scalars, so a full window always yields one; the
// unfinished last cluster waits for the next scalar or the end of the text. Invalid
// UTF-8 and scanner errors end the walk. forms is the prepared 128-byte map of the
// scripts whose scanner reads it.
SfntStatus indic_for_each_cluster_raw(SfntIndicProfile,Utf8View,ConstSlice<u8> forms,u64 work,void* context,
    SfntStatus (*cluster)(void*,const u32*,u32,bool) noexcept) noexcept;
template<class Cluster> SfntStatus indic_for_each_cluster(SfntIndicProfile profile,Utf8View text,ConstSlice<u8> forms,u64 work,Cluster cluster) noexcept {
    return indic_for_each_cluster_raw(profile,text,forms,work,&cluster,[](void* context,const u32* scalars,u32 length,bool followed) noexcept {
        return (*static_cast<Cluster*>(context))(scalars,length,followed);});
}
// Trailing spacing of a shaped Indic run: each shaping cluster of the first count
// glyphs advances its last glyph in reading order, except surviving controls.
// skip(context,index,work,&skipped) decides per glyph and may charge work. Every
// cluster costs twice its glyph count from work.
SfntStatus indic_trailing_spacing_raw(const SfntShapeStorage&,u32 count,float spacing,bool rtl,use_shape::Work&,void* context,
    SfntStatus (*skip)(void*,u32,use_shape::Work&,bool*) noexcept) noexcept;
template<class Skip> SfntStatus indic_trailing_spacing(const SfntShapeStorage& storage,u32 count,float spacing,bool rtl,
    use_shape::Work& work,Skip skip) noexcept {
    return indic_trailing_spacing_raw(storage,count,spacing,rtl,work,&skip,[](void* context,u32 index,use_shape::Work& budget,bool* skipped) noexcept {
        return (*static_cast<Skip*>(context))(index,budget,skipped);});
}
}
