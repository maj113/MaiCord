#include "maicord/text/components.hpp"
namespace maicord {namespace {
constexpr u32 nil=0xffffffffu;constexpr u8 alias=1,known=2,cached=4,owner=8;
bool spend(u64* work,u64 n=1) noexcept {if(n>*work)return false;*work-=n;return true;}
u32 parent(const SfntComponentNode& n) noexcept {return n.flags&owner?nil:n.parent;}
const SfntComponentGlyph& record(SfntGlyphView v,u32 i) noexcept {return *reinterpret_cast<const SfntComponentGlyph*>(&sfnt_glyph_at(v,i));}
struct Tree {
    SfntComponentForest& forest;Slice<SfntComponentUndo> journal;u32 used=0;u64 work;SfntStatus status=SfntStatus::ok;
    bool charge() noexcept {if(status!=SfntStatus::ok)return false;if(!spend(&work)){status=SfntStatus::resource_limit;return false;}return true;}
    SfntComponentNode& get(u32 i) noexcept {return forest.nodes.data[i];}
    u32 height(u32 i) noexcept {return i==nil?0:get(i).height;}
    u32 count(u32 i) noexcept {return i==nil?0:get(i).count;}
    bool save(u32 i) noexcept {if(!charge())return false;if(used==journal.count){status=SfntStatus::output_too_small;return false;}journal.data[used++]={i,get(i)};return true;}
    bool set_parent(u32 i,u32 p) noexcept {if(i==nil)return status==SfntStatus::ok;if(get(i).parent==p)return status==SfntStatus::ok;if(!save(i))return false;get(i).parent=p;return true;}
    bool set(u32 i,u32 left,u32 right) noexcept {
        if(!save(i))return false;auto& n=get(i);n.left=left;n.right=right;n.count=count(left)+1+count(right);n.height=static_cast<u8>(1+maximum(height(left),height(right)));
        return set_parent(left,i) && set_parent(right,i);
    }
    u32 balance(u32 i) noexcept {
        if(status!=SfntStatus::ok)return nil;const auto n=get(i);
        if(height(n.left)>height(n.right)+1){u32 l=n.left;
            if(height(get(l).right)>height(get(l).left)){const u32 middle=get(l).right;const auto m=get(middle);
                if(!set(l,get(l).left,m.left) || !set(i,m.right,n.right) || !set(middle,l,i))return nil;return middle;}
            const u32 middle=get(l).right;if(!set(i,middle,n.right) || !set(l,get(l).left,i))return nil;return l;
        }
        if(height(n.right)>height(n.left)+1){u32 r=n.right;
            if(height(get(r).left)>height(get(r).right)){const u32 middle=get(r).left;const auto m=get(middle);
                if(!set(i,n.left,m.left) || !set(r,m.right,get(r).right) || !set(middle,i,r))return nil;return middle;}
            const u32 middle=get(r).left;if(!set(i,n.left,middle) || !set(r,i,get(r).right))return nil;return r;
        }return i;
    }
    u32 join3(u32 left,u32 middle,u32 right) noexcept {
        if(!charge())return nil;
        if(height(left)>height(right)+1){const u32 branch=join3(get(left).right,middle,right);if(status!=SfntStatus::ok || !set(left,get(left).left,branch))return nil;return balance(left);}
        if(height(right)>height(left)+1){const u32 branch=join3(left,middle,get(right).left);if(status!=SfntStatus::ok || !set(right,branch,get(right).right))return nil;return balance(right);}
        return set(middle,left,right)?middle:nil;
    }
    u32 extract_last(u32 root,u32* last) noexcept {
        if(!charge())return nil;const auto n=get(root);if(n.right==nil){*last=root;return n.left;}
        const u32 right=extract_last(n.right,last);if(status!=SfntStatus::ok || !set(root,n.left,right))return nil;return balance(root);
    }
    u32 join(u32 left,u32 right) noexcept {
        if(left==nil)return right;if(right==nil)return left;u32 middle=nil;left=extract_last(left,&middle);if(status!=SfntStatus::ok)return nil;return join3(left,middle,right);
    }
    void split(u32 root,u32 k,u32* left,u32* right) noexcept {
        *left=*right=nil;if(!charge() || root==nil)return;
        if(!k){*right=root;return;}if(k>=count(root)){*left=root;return;}
        const auto n=get(root);const u32 before=count(n.left);
        if(k<=before){u32 remainder=nil;split(n.left,k,left,&remainder);if(status!=SfntStatus::ok)return;*right=join3(remainder,root,n.right);}
        else {u32 remainder=nil;split(n.right,k-before-1,&remainder,right);if(status!=SfntStatus::ok)return;*left=join3(n.left,root,remainder);}
    }
    bool activate(u32 root) noexcept {
        if(root==nil || !(get(root).flags&alias))return status==SfntStatus::ok;
        if(!save(root))return false;auto& n=get(root);n.left=n.right=nil;n.flags=0;return true;
    }
    bool root_flags(u32 root,bool history) noexcept {if(root==nil)return status==SfntStatus::ok;if(!set_parent(root,nil) || !save(root))return false;auto& n=get(root);n.flags=static_cast<u8>((n.flags&~known)|(history?known:0));return true;}
    u32 first_source(u32 root) noexcept {
        while(root!=nil){if(!charge())return 0;const auto& n=get(root);if((n.flags&alias) || n.left==nil)return n.source;root=n.left;}return 0;
    }
    u8 last_form(u32 root) noexcept {
        while(root!=nil){if(!charge())return 0;const auto& n=get(root);if((n.flags&alias) || n.right==nil)return static_cast<u8>(n.reserved);root=n.right;}return 0;
    }
};
}
SfntStatus sfnt_components_open(Slice<SfntComponentGlyph> glyphs,Slice<SfntComponentNode> storage,SfntComponentForest* out,u64 work,bool preserve_forms) noexcept {
    if(!out)return SfntStatus::invalid_input;*out={};if((glyphs.count && !glyphs.data) || (storage.count && !storage.data))return SfntStatus::invalid_input;
    if(storage.count<glyphs.count)return SfntStatus::output_too_small;if(static_cast<u64>(glyphs.count)*2>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<glyphs.count;++i)if(static_cast<u8>(glyphs.data[i].glyph.glyph_class)>4)return SfntStatus::invalid_input;
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs.data[i];g.component=i;g.attachment=i?i-1:nil;const bool mark=g.glyph.glyph_class==SfntGlyphClass::mark;
        storage.data[i]={mark?g.attachment:nil,nil,nil,1,g.source,1,static_cast<u8>(mark?alias:0),static_cast<u16>(preserve_forms?g.glyph.reserved:0)};}
    *out={{storage.data,glyphs.count},false,preserve_forms};return SfntStatus::ok;
}
SfntStatus sfnt_component_first_metadata(const SfntComponentForest& f,u32 node,u8* output,u64 work,u64* used) noexcept {
    if(used)*used=0;if(!output)return SfntStatus::invalid_input;*output=0;if(f.nodes.count&&!f.nodes.data)return SfntStatus::invalid_input;
    struct Report {u64 initial;u64& remaining;u64* used;~Report() noexcept {if(used)*used=initial-remaining;}} report{work,work,used};
    while(node!=nil){if(!spend(&work))return SfntStatus::resource_limit;if(node>=f.nodes.count)return SfntStatus::invalid_input;
        const auto& n=f.nodes[node];if((n.flags&alias)||n.left==nil){*output=static_cast<u8>(n.reserved>>8);return SfntStatus::ok;}node=n.left;}
    return SfntStatus::ok;
}
SfntStatus sfnt_component_location(const SfntComponentForest& f,u32 node,u32* root,u32* ordinal,u64 work,u64* used) noexcept {
    if(used)*used=0;if(!root || !ordinal)return SfntStatus::invalid_input;*root=nil;*ordinal=0;if(node==nil)return SfntStatus::ok;if(node>=f.nodes.count)return SfntStatus::invalid_input;const u64 initial=work;
    u32 at=node,rank=f.nodes.data[at].flags&alias?0:(f.nodes.data[at].left==nil?0:f.nodes.data[f.nodes.data[at].left].count);
    for(;;){if(!spend(&work)){if(used)*used=initial-work;return SfntStatus::resource_limit;}const auto& n=f.nodes.data[at];const u32 p=parent(n);if(p==nil){*root=at;*ordinal=rank;if(used)*used=initial-work;return SfntStatus::ok;}
        const auto& up=f.nodes.data[p];if(up.right==at)rank+=1+(up.left==nil?0:f.nodes.data[up.left].count);at=p;}
}
SfntStatus sfnt_component_source_bounds(const SfntComponentForest& forest,u32 node,u32* first,u32* last,u64 work,u64* used) noexcept {
    if(used)*used=0;if(!first||!last)return SfntStatus::invalid_input;*first=nil;*last=0;
    if(node==nil)return SfntStatus::ok;if(node>=forest.nodes.count||!forest.nodes.data)return SfntStatus::invalid_input;
    struct Report {u64 initial;u64& remaining;u64* used;~Report() noexcept {if(used)*used=initial-remaining;}} report{work,work,used};
    u32 pending[64],depth=0,lo=nil,hi=0;pending[depth++]=node;
    while(depth){if(!spend(&work))return SfntStatus::resource_limit;
        const u32 at=pending[--depth];if(at>=forest.nodes.count)return SfntStatus::invalid_input;const auto& n=forest.nodes[at];
        lo=minimum(lo,n.source);hi=maximum(hi,n.source);if(n.flags&alias)continue;
        if(n.left!=nil){if(depth==64)return SfntStatus::resource_limit;pending[depth++]=n.left;}
        if(n.right!=nil){if(depth==64)return SfntStatus::resource_limit;pending[depth++]=n.right;}}
    *first=lo;*last=hi;return SfntStatus::ok;
}
void sfnt_components_rollback(SfntComponentForest* forest,ConstSlice<SfntComponentUndo> journal) noexcept {
    for(u32 i=journal.count;i;){const auto& item=journal.data[--i];forest->nodes.data[item.index]=item.before;}
}
SfntStatus sfnt_components_edit(SfntComponentForest* forest,SfntGlyphView glyphs,ConstSlice<u32> consumed,bool ligature,Slice<u8> outputs,Slice<SfntComponentUndo> journal,SfntComponentEdit* out,u64 work) noexcept {
    if(!out)return SfntStatus::invalid_input;*out={};if(!forest || forest->sealed || !sfnt_glyph_view_valid(glyphs) || glyphs.record_size!=sizeof(SfntComponentGlyph) || !consumed.count || !consumed.data || !outputs.count || !outputs.data || outputs.count%sizeof(SfntComponentGlyph) || (journal.count && !journal.data))return SfntStatus::invalid_input;
    const u32 output_count=outputs.count/sizeof(SfntComponentGlyph);if((ligature && output_count!=1) || (!ligature && consumed.count!=1))return SfntStatus::invalid_input;
    Tree t{*forest,journal,0,work};u32 root=nil;bool history=false;
    for(u32 i=0;i<consumed.count;++i){if(consumed.data[i]>=sfnt_glyph_count(glyphs) || (i && consumed.data[i]<=consumed.data[i-1])){t.status=SfntStatus::invalid_input;break;}
        u32 current=nil,ordinal=0;u64 used=0;auto s=sfnt_component_location(*forest,record(glyphs,consumed.data[i]).component,&current,&ordinal,t.work,&used);t.work-=used;if(s!=SfntStatus::ok){t.status=s;break;}
        if(!ligature){root=current;history=root!=nil && (forest->nodes.data[root].flags&known);break;}
        if(current==nil)continue;if(current==root){t.status=SfntStatus::invalid_input;break;}
        if(!t.activate(current))break;root=t.join(root,current);if(t.status!=SfntStatus::ok)break;if(!t.set_parent(root,nil))break;
    }
    if(ligature && t.status==SfntStatus::ok){t.root_flags(root,true);if(t.status==SfntStatus::ok){copy_bytes(outputs.data+8,&root,4);if(forest->preserve_forms && root!=nil)outputs.data[3]=t.last_form(root);}}
    if(!ligature && t.status==SfntStatus::ok){u32 source=record(glyphs,consumed.data[0]).source;u8 form=record(glyphs,consumed.data[0]).glyph.reserved;
        for(u32 i=0;i<output_count && t.status==SfntStatus::ok;++i){u32 part=root;if(i+1<output_count){t.split(root,1,&part,&root);if(t.status!=SfntStatus::ok)break;}
            if(!t.root_flags(part,history) || !t.root_flags(i+1<output_count?root:nil,history))break;
            if(part!=nil)source=t.first_source(part);if(t.status!=SfntStatus::ok)break;
            if(forest->preserve_forms){if(part!=nil)form=t.last_form(part);if(t.status!=SfntStatus::ok)break;outputs.data[i*sizeof(SfntComponentGlyph)+3]=form;}
            copy_bytes(outputs.data+i*sizeof(SfntComponentGlyph)+4,&source,4);copy_bytes(outputs.data+i*sizeof(SfntComponentGlyph)+8,&part,4);
        }}
    out->operations=work-t.work;
    if(t.status!=SfntStatus::ok){sfnt_components_rollback(forest,{journal.data,t.used});return t.status;}
    out->undo_count=t.used;return SfntStatus::ok;
}
SfntStatus sfnt_components_export(SfntComponentForest* f,SfntGlyphView glyphs,Slice<u32> targets,Slice<SfntMarkAssociation> associations,u32* target_count,u64 work,u64* used) noexcept {
    const u64 initial=work;struct Report {u64 initial;u64& work;u64* used;~Report() noexcept {if(used)*used=initial-work;}} report{initial,work,used};
    if(!target_count)return SfntStatus::invalid_input;*target_count=0;
    if(!f || !sfnt_glyph_view_valid(glyphs) || glyphs.record_size!=sizeof(SfntComponentGlyph) || (targets.count && !targets.data) || (associations.count && !associations.data))return SfntStatus::invalid_input;
    const u32 count=sfnt_glyph_count(glyphs);if(associations.count<count)return SfntStatus::output_too_small;
    f->sealed=true;
    for(u32 i=0;i<f->nodes.count;++i){if(!spend(&work))return SfntStatus::resource_limit;auto& n=f->nodes.data[i];if(n.flags&owner){n.parent=nil;n.flags&=~owner;}}
    for(u32 i=0;i<count;++i){if(!spend(&work))return SfntStatus::resource_limit;const auto& g=record(glyphs,i);if(static_cast<u8>(g.glyph.glyph_class)>4)return SfntStatus::invalid_input;
        if(g.component!=nil && g.component>=f->nodes.count)return SfntStatus::invalid_input;if(g.attachment!=nil && g.attachment>=f->nodes.count)return SfntStatus::invalid_input;
        if(g.glyph.glyph_class==SfntGlyphClass::mark || g.component==nil)continue;u32 root=nil,rank=0;u64 charged=0;auto s=sfnt_component_location(*f,g.component,&root,&rank,work,&charged);work-=charged;if(s!=SfntStatus::ok)return s;
        auto& n=f->nodes.data[root];if(n.flags&owner)return SfntStatus::invalid_input;if(*target_count==targets.count)return SfntStatus::output_too_small;
        if(n.flags&alias){n.left=n.right=nil;n.flags=0;}n.flags|=owner;n.parent=*target_count;targets.data[(*target_count)++]=i;
    }
    for(u32 i=0;i<count;++i){if(!spend(&work))return SfntStatus::resource_limit;const auto& g=record(glyphs,i);associations.data[i]={nil,sfnt_component_unknown};if(g.glyph.glyph_class!=SfntGlyphClass::mark)continue;
        u32 node=g.attachment,leaf=nil;
        while(node!=nil){if(!spend(&work))return SfntStatus::resource_limit;const auto& n=f->nodes.data[node];if(!(n.flags&alias)){leaf=node;break;}if(n.flags&cached){leaf=n.right;break;}node=n.left;}
        node=g.attachment;while(node!=nil){auto& n=f->nodes.data[node];if(!(n.flags&alias) || (n.flags&cached))break;if(!spend(&work))return SfntStatus::resource_limit;const u32 next=n.left;n.right=leaf;n.flags|=cached;node=next;}
        if(leaf==nil)continue;u32 root=nil,rank=0;u64 charged=0;auto s=sfnt_component_location(*f,leaf,&root,&rank,work,&charged);work-=charged;if(s!=SfntStatus::ok)return s;
        const auto& n=f->nodes.data[root];if(!(n.flags&owner))continue;const u32 slot=n.parent;if(targets.data[slot]>=i)continue;
        associations.data[i]={slot,n.flags&known?rank:sfnt_component_unknown};
    }return SfntStatus::ok;
}
}
