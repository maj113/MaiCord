#include "maicord/text/color_font.hpp"

namespace maicord {
namespace {
struct Walker {
    const ColrFont& font;
    Slice<u32> active;
    u32 work;
    ColrValidation result{};

    SfntStatus clip(u16 glyph) noexcept {
        ColrClip box{};const auto status=colr_clip(font,glyph,&box);
        if(status==SfntStatus::ok)result.has_variations|=box.variable;
        return status;
    }
    SfntStatus visit(u32 offset,u32 depth) noexcept {
        if(!work || depth>=active.count || depth>=64)return SfntStatus::resource_limit;
        --work;
        for(u32 i=0;i<depth;++i)if(active[i]==offset)return SfntStatus::malformed;
        active[depth]=offset;
        ColrPaint p{};auto status=colr_paint_read(font,offset,&p);
        if(status!=SfntStatus::ok)return status;
        ++result.nodes;result.formats|=1u<<(p.format-1);
        result.maximum_depth=maximum(result.maximum_depth,depth+1);
        result.has_variations|=p.variable;
        if(p.format==1) {
            for(u32 i=0;i<p.layer_count;++i) {
                u32 child=0;status=colr_layer_paint(font,p.first_layer+i,&child);if(status!=SfntStatus::ok)return status;
                status=visit(child,depth+1);if(status!=SfntStatus::ok)return status;
            }
        } else if(p.format>=4 && p.format<=9) {
            ColrColorLine line{};status=colr_color_line(font,p,&line);if(status!=SfntStatus::ok)return status;
            for(u32 i=0;i<line.count;++i) {
                if(!work)return SfntStatus::resource_limit;--work;
                ColrColorStop stop{};status=colr_color_stop(font,line,i,&stop);if(status!=SfntStatus::ok)return status;
                ++result.stops;
            }
        } else if(p.format>=10) {
            if(p.format==11) {status=clip(p.glyph);if(status!=SfntStatus::ok)return status;}
            status=visit(p.child[0],depth+1);if(status!=SfntStatus::ok)return status;
            if(p.format==32)status=visit(p.child[1],depth+1);
        }
        return status;
    }
};
}
SfntStatus colr_validate_glyph(const ColrFont& font,u16 glyph,Slice<u32> active_path,
                              u32 work_limit,ColrValidation* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!active_path.data || !active_path.count)return SfntStatus::invalid_input;
    u32 root=0;auto status=colr_base_paint(font,glyph,&root);if(status!=SfntStatus::ok)return status;
    Walker walker{font,active_path,work_limit};
    status=walker.clip(glyph);if(status!=SfntStatus::ok)return status;
    status=walker.visit(root,0);if(status==SfntStatus::ok)*output=walker.result;
    return status;
}
} // namespace maicord
