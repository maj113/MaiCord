#pragma once
#include "maicord/render/colr_program.hpp"

namespace maicord::colr_test {
template<class T> const T* view(const ColrPaintProgram& p,u32 at,u32 count=1) noexcept {
    return p.storage.base&&at%alignof(T)==0&&at<=p.storage.offset&&count<=(p.storage.offset-at)/sizeof(T)?
        reinterpret_cast<const T*>(p.storage.base+at):nullptr;
}
inline bool bytes(const ColrPaintProgram& a,u32 x,const ColrPaintProgram& b,u32 y,u32 count) noexcept {
    const auto* left=view<u8>(a,x,count);const auto* right=view<u8>(b,y,count);
    return left&&right&&bytes_equal(left,right,count);
}
struct Edge {u32 a[2],b[2];};
struct EdgeSequence {
    const Edge* edges;u32 edge_count;const u32* indices;u32 count;
    bool valid() const noexcept {for(u32 i=0;i<count;++i)if((indices?indices[i]:i)>=edge_count)return false;return true;}
    const Edge& at(u32 i) const noexcept {return edges[indices?indices[i]:i];}
    bool contains_point(const u32* point) const noexcept {
        for(u32 i=0;i<count;++i){const auto& e=at(i);if(bytes_equal(point,e.a,8)||bytes_equal(point,e.b,8))return true;}return false;
    }
};
inline bool edge_sequence(EdgeSequence a,EdgeSequence b) noexcept {
    if(!a.valid()||!b.valid())return false;u32 i=0,j=0;
    while(i<a.count||j<b.count){
        if(i<a.count&&j<b.count&&bytes_equal(&a.at(i),&b.at(j),sizeof(Edge))){++i;++j;continue;}
        if(i<a.count&&bytes_equal(a.at(i).a,a.at(i).b,8)&&b.contains_point(a.at(i).a)){++i;continue;}
        if(j<b.count&&bytes_equal(b.at(j).a,b.at(j).b,8)&&a.contains_point(b.at(j).a)){++j;continue;}
        return false;
    }return true;
}
inline bool mask(const ColrPaintProgram& a,u32 x,const ColrPaintProgram& b,u32 y) noexcept {
    const auto* left=view<ColrMask>(a,x);const auto* right=view<ColrMask>(b,y);if(!left||!right)return false;
    auto l=*left,r=*right;l.segments=r.segments=l.rows=r.rows=l.cache_next=r.cache_next=0;
    const u32 lc=l.segment_count,rc=r.segment_count;l.segment_count=r.segment_count=0;
    if(!bytes_equal(&l,&r,sizeof(l))||l.height>0x7fffffffu||lc>0xffffffffu/16||rc>0xffffffffu/16)return false;
    const auto* le=view<Edge>(a,left->segments,lc);const auto* re=view<Edge>(b,right->segments,rc);if(!le||!re)return false;
    if(lc==rc){if(!bytes(a,left->segments,b,right->segments,lc*16))return false;}
    else if(!edge_sequence({le,lc,nullptr,lc},{re,rc,nullptr,rc}))return false;
    const auto* lr=view<u32>(a,left->rows,l.height*2);const auto* rr=view<u32>(b,right->rows,l.height*2);
    if(!lr||!rr)return false;
    for(u32 i=0;i<l.height;++i){
        const u32 ln=lr[i*2+1],rn=rr[i*2+1];if(ln>0xffffffffu/4||rn>0xffffffffu/4)return false;
        if(lc==rc){if(ln!=rn||!bytes(a,lr[i*2],b,rr[i*2],ln*4))return false;}
        else {const auto* li=view<u32>(a,lr[i*2],ln);const auto* ri=view<u32>(b,rr[i*2],rn);
            if(!li||!ri||!edge_sequence({le,lc,li,ln},{re,rc,ri,rn}))return false;}
    }
    return true;
}
inline bool equal(const ColrPaintProgram& a,const ColrPaintProgram& b) noexcept {
    if(a.nodes!=b.nodes||a.stack!=b.stack||a.peak_stack!=b.peak_stack||
       a.mask_count!=b.mask_count||a.mask_pixels!=b.mask_pixels||a.paint_types!=b.paint_types||
       a.bounds.has_bounds!=b.bounds.has_bounds||a.bounds.minimum_x!=b.bounds.minimum_x||
       a.bounds.minimum_y!=b.bounds.minimum_y||a.bounds.maximum_x!=b.bounds.maximum_x||a.bounds.maximum_y!=b.bounds.maximum_y)return false;
    u32 x=a.first,y=b.first;
    for(u32 i=0;i<a.nodes;++i){
        const auto* left=view<ColrPaintNode>(a,x);const auto* right=view<ColrPaintNode>(b,y);if(!left||!right)return false;
        auto l=*left,r=*right;if(l.kind!=r.kind||l.kind>11)return false;
        const bool shaped=l.kind>=8;const u32 kind=shaped?l.kind-8:l.kind;
        if(shaped){if(!mask(a,l.reserved,b,r.reserved))return false;l.reserved=r.reserved=0;}
        if(kind==4){if(!mask(a,l.data,b,r.data))return false;l.data=r.data=0;}
        else if(kind>=1&&kind<=3){
            if(l.count!=r.count||l.count>0xffffffffu/sizeof(ColrGradientStop)||
               !bytes(a,l.data,b,r.data,l.count*sizeof(ColrGradientStop)))return false;l.data=r.data=0;
        }
        l.next=r.next=0;if(!bytes_equal(&l,&r,sizeof(l)))return false;x=left->next;y=right->next;
    }
    return x==~0u&&y==~0u;
}
}
