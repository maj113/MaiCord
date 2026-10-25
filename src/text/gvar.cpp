#include "maicord/text/gvar.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/truetype_outline.hpp"
#include "sfnt_read.hpp"
namespace maicord {namespace {
using namespace sfnt_read;
struct Reader {ConstSlice<u8> bytes;u32 at=0;bool take(u32 n,ConstSlice<u8>* out) noexcept {if(!span(bytes,at,n))return false;*out={bytes.data+at,n};at+=n;return true;}bool byte(u8* v) noexcept {if(!span(bytes,at,1))return false;*v=bytes[at++];return true;}};
u32 location(const SfntGvar& f,u32 glyph) noexcept {return f.offset_stride==4?r32(f.offsets,glyph*4):r16(f.offsets,glyph*2)*2u;}
#pragma float_control(precise,on,push)
// Peaks, supports and the F2DOT14-rounded coordinate compare as integers; each
// ratio is one exact-integer division, bit-identical to dividing the scaled values.
SfntStatus region(ConstSlice<u8> peak,ConstSlice<u8> start,ConstSlice<u8> end,ConstSlice<double> coords,double* out,u32& work) noexcept {
    if(!spend(work,coords.count))return SfntStatus::resource_limit;double scalar=1;
    const auto unit=[](i32 v) noexcept {return v>=-16384 && v<=16384;};
    for(u32 i=0;i<coords.count;++i){const i32 p=static_cast<i16>(r16(peak,i*2));const double shifted=coords[i]*16384+.5;const i32 rounded=static_cast<i32>(shifted);
        const i32 c=rounded-(rounded>shifted?1:0);if(!unit(p))return SfntStatus::malformed;
        if(start.count){const i32 a=static_cast<i16>(r16(start,i*2)),b=static_cast<i16>(r16(end,i*2));
            if(!unit(a) || !unit(b))return SfntStatus::malformed;
            if(a>p || p>b || (a<0 && b>0 && p!=0) || p==0)continue;
            if(c<a || c>b){scalar=0;continue;}if(c==p)continue;scalar*=c<p?static_cast<double>(c-a)/(p-a):static_cast<double>(b-c)/(b-p);
        }else{if(p==0)continue;if(c==0 || (c<0)!=(p<0)){scalar=0;continue;}if((c<0?-c:c)<(p<0?-p:p))scalar*=static_cast<double>(c)/p;}
    }*out=scalar;return SfntStatus::ok;
}
struct Tuples {
    const SfntGvarInstance& instance;
    Reader headers{},serialized{};
    u32 count=0;
    bool shared=false;
    SfntStatus open(u16 glyph) noexcept {
        const auto& f=instance.font;const u32 first=location(f,glyph),end=location(f,glyph+1);
        if(first>end || end>f.table.count-f.data_offset)return SfntStatus::malformed;if(first==end)return SfntStatus::ok;
        const ConstSlice<u8> data{f.table.data+f.data_offset+first,end-first};if(!span(data,0,4))return SfntStatus::truncated;
        const u16 flags=r16(data,0),offset=r16(data,2);if(flags&0x7000||offset<4)return SfntStatus::malformed;if(offset>data.count)return SfntStatus::truncated;
        headers={{data.data,offset},4};serialized={data,offset};count=flags&0xfffu;shared=(flags&0x8000)!=0;return SfntStatus::ok;
    }
    SfntStatus next(ConstSlice<u8>* packed,u16* index,double* scale,u32& work) noexcept {
        if(!spend(work))return SfntStatus::resource_limit;const auto& f=instance.font;ConstSlice<u8> h{},peak{},start{},end{};
        if(!headers.take(4,&h))return SfntStatus::truncated;*index=r16(h,2);if(*index&0x1000)return SfntStatus::malformed;
        const u32 axis_bytes=static_cast<u32>(f.axis_count)*2;
        if(*index&0x8000){if(!headers.take(axis_bytes,&peak))return SfntStatus::truncated;}
        else{if((*index&0xfff)>=f.shared_count)return SfntStatus::malformed;peak={f.shared_tuples.data+(*index&0xfff)*axis_bytes,axis_bytes};}
        if(*index&0x4000){if(!headers.take(axis_bytes,&start)||!headers.take(axis_bytes,&end))return SfntStatus::truncated;}
        if(!serialized.take(r16(h,0),packed))return SfntStatus::truncated;
        if((*index&0xc000)==0){*scale=instance.shared_scalars[*index&0xfff];return SfntStatus::ok;}
        return region(peak,start,end,instance.coordinates,scale,work);
    }
};
struct PointList {ConstSlice<u32> indices;u32 count=0;bool all=true;};
SfntStatus point_list(Reader& r,u32 total,Slice<u32> storage,PointList* out,u32& work) noexcept {
    u8 first=0;if(!r.byte(&first))return SfntStatus::truncated;u32 count=first;
    if(first&128){u8 next=0;if(!r.byte(&next))return SfntStatus::truncated;count=((first&127u)<<8)|next;}
    if(!count){*out={{},total,true};return SfntStatus::ok;}
    if(storage.count<count || !storage.data)return SfntStatus::output_too_small;
    if(!spend(work,count))return SfntStatus::resource_limit;
    u32 at=0,point=0;while(at<count){u8 control=0;if(!r.byte(&control))return SfntStatus::truncated;const u32 n=(control&127u)+1,width=control&128?2:1;if(n>count-at)return SfntStatus::malformed;ConstSlice<u8> data{};if(!r.take(n*width,&data))return SfntStatus::truncated;
        for(u32 i=0;i<n;++i){point+=width==2?r16(data,i*2):data[i];if(point>=total)return SfntStatus::malformed;storage[at++]=point;}}
    *out={{storage.data,count},count,false};return SfntStatus::ok;
}
SfntStatus deltas(Reader& r,u32 count,Slice<i32> storage,u32& work) noexcept {
    if(storage.count<count || (count && !storage.data))return SfntStatus::output_too_small;if(!spend(work,count))return SfntStatus::resource_limit;
    u32 at=0;while(at<count){u8 control=0;if(!r.byte(&control))return SfntStatus::truncated;const u32 n=(control&63u)+1,width=control&128?0:control&64?2:1;if(n>count-at)return SfntStatus::malformed;ConstSlice<u8> data{};if(width && !r.take(n*width,&data))return SfntStatus::truncated;
        for(u32 i=0;i<n;++i)storage[at++]=!width?0:width==2?static_cast<i16>(r16(data,i*2)):static_cast<signed char>(data[i]);}return SfntStatus::ok;
}
double inferred(double target,double a,double b,double da,double db) noexcept {
    if(a==b)return da==db?da:0;if(a>b){const double c=a;a=b;b=c;const double d=da;da=db;db=d;}
    if(target<=a)return da;if(target>=b)return db;return da+(db-da)*((target-a)/(b-a));
}
SfntStatus apply(PointList list,ConstSlice<SfntOutlinePoint> points,ConstSlice<u32> ends,bool composite,SfntGvarScratch scratch,Slice<SfntGvarDelta> output,double scale,u32& work,u32 origin) noexcept {
    if(!spend(work,list.count))return SfntStatus::resource_limit;
    if(list.all){for(u32 i=0;i<list.count;++i){output[i].x+=scratch.x_deltas[i]*scale;output[i].y+=scratch.y_deltas[i]*scale;}return SfntStatus::ok;}
    // Coalesce repeated point IDs cumulatively, as required by the packed-point
    // format. The separate shared list stays reusable by later tuples.
    u32 unique=0;for(u32 i=0;i<list.count;++i){const u32 p=list.indices[i];if(unique && scratch.private_points[unique-1]==p){scratch.x_deltas[unique-1]+=scratch.x_deltas[i];scratch.y_deltas[unique-1]+=scratch.y_deltas[i];}
        else{scratch.private_points[unique]=p;scratch.x_deltas[unique]=scratch.x_deltas[i];scratch.y_deltas[unique]=scratch.y_deltas[i];++unique;}}
    const auto ids=scratch.private_points;
    for(u32 i=0;i<unique;++i){output[ids[i]].x+=scratch.x_deltas[i]*scale;output[ids[i]].y+=scratch.y_deltas[i]*scale;}
    if(composite)return SfntStatus::ok;
    u32 first=0,cursor=0;for(u32 contour=0;contour<ends.count;++contour){const u32 last=ends[contour]-origin,begin=cursor;while(cursor<unique && ids[cursor]<=last)++cursor;
        if(cursor>begin){for(u32 a=begin;a<cursor;++a){const u32 b=a+1<cursor?a+1:begin;u32 target=ids[a]==last?first:ids[a]+1;
            while(target!=ids[b]){if(!spend(work))return SfntStatus::resource_limit;const auto p=points[target],pa=points[ids[a]],pb=points[ids[b]];
                // Untouched points truncate each inferred/scaled contribution;
                // explicit points retain the fraction until final rounding.
                output[target].x+=static_cast<i64>(inferred(p.x,pa.x,pb.x,scratch.x_deltas[a],scratch.x_deltas[b])*scale);
                output[target].y+=static_cast<i64>(inferred(p.y,pa.y,pb.y,scratch.y_deltas[a],scratch.y_deltas[b])*scale);target=target==last?first:target+1;}}}
        first=last+1;
    }return SfntStatus::ok;
}
struct PhantomPoints {u32 count,first[2],end[2];};
SfntStatus phantom_points(Reader& r,u32 points,PhantomPoints* out,u32& work) noexcept {
    u8 first=0;if(!r.byte(&first))return SfntStatus::truncated;u32 count=first;
    if(first&128){u8 next=0;if(!r.byte(&next))return SfntStatus::truncated;count=((first&127u)<<8)|next;}
    if(!count){*out={points+4,{points,points+1},{points+1,points+2}};return SfntStatus::ok;}
    if(!spend(work,count))return SfntStatus::resource_limit;*out={count,{count,count},{count,count}};
    u32 at=0,point=0;
    while(at<count){u8 control=0;if(!r.byte(&control))return SfntStatus::truncated;const u32 n=(control&127u)+1,width=control&128?2:1;
        if(n>count-at)return SfntStatus::malformed;ConstSlice<u8> data{};if(!r.take(n*width,&data))return SfntStatus::truncated;
        for(u32 i=0;i<n;++i,++at){point+=width==2?r16(data,i*2):data[i];if(point>=points+4)return SfntStatus::malformed;
            if(point>=points&&point<points+2){const u32 id=point-points;if(out->first[id]==count)out->first[id]=at;out->end[id]=at+1;}}
    }return SfntStatus::ok;
}
SfntStatus phantom_deltas(Reader& r,const PhantomPoints& points,double* sums,u32& work) noexcept {
    const u32 count=maximum(points.first[0]<points.count?points.end[0]:0,points.first[1]<points.count?points.end[1]:0);
    if(!spend(work,count))return SfntStatus::resource_limit;
    for(u32 at=0;at<count;){u8 control=0;if(!r.byte(&control))return SfntStatus::truncated;const u32 n=(control&63u)+1,width=control&128?0:control&64?2:1;
        if(n>points.count-at)return SfntStatus::malformed;ConstSlice<u8> data{};if(width&&!r.take(n*width,&data))return SfntStatus::truncated;
        if(width)for(u32 id=0;id<2;++id)for(u32 i=maximum(at,points.first[id]);i<minimum(at+n,points.end[id]);++i)
            sums[id]+=width==2?static_cast<i16>(r16(data,(i-at)*2)):static_cast<signed char>(data[i-at]);
        at+=n;
    }return SfntStatus::ok;
}
SfntStatus phantoms(const SfntGvarInstance& instance,u16 glyph,u32 points,double* output,u32& work) noexcept {
    Tuples tuples{instance};auto status=tuples.open(glyph);if(status!=SfntStatus::ok)return status;
    PhantomPoints shared{points+4,{points,points+1},{points+1,points+2}};
    if(tuples.shared){status=phantom_points(tuples.serialized,points,&shared,work);if(status!=SfntStatus::ok)return status;}
    for(u32 i=0;i<tuples.count;++i){ConstSlice<u8> packed{};u16 index=0;double scale=0;
        status=tuples.next(&packed,&index,&scale,work);if(status!=SfntStatus::ok)return status;if(!scale)continue;
        Reader stream{packed};auto list=shared;
        if(index&0x2000){status=phantom_points(stream,points,&list,work);if(status!=SfntStatus::ok)return status;}
        double values[2]{};status=phantom_deltas(stream,list,values,work);if(status!=SfntStatus::ok)return status;
        for(u32 id=0;id<2;++id)output[id]+=values[id]*scale;
    }return SfntStatus::ok;
}
double rounded(double value) noexcept {const double shifted=value+.5;const i64 integer=static_cast<i64>(shifted);return static_cast<double>(integer-(integer>shifted?1:0));}
struct Advance {double x,y;};
SfntStatus glyph_advance(const SfntTrueType& font,const SfntGvarInstance& instance,u16 glyph,Slice<u16> chain,u32 depth,Advance* output,u32& work) noexcept {
    if(depth==chain.count||!spend(work,depth+1))return SfntStatus::resource_limit;
    for(u32 i=0;i<depth;++i)if(chain[i]==glyph)return SfntStatus::malformed;chain[depth]=glyph;
    SfntGlyphMetrics metrics{};auto status=sfnt_horizontal_metrics(font.metrics,glyph,&metrics);if(status!=SfntStatus::ok)return status;
    const u32 first=loca_offset(font.loca,font.loca_stride,glyph),end=loca_offset(font.loca,font.loca_stride,glyph+1u);
    if(first>end||end>font.glyf.count)return SfntStatus::malformed;u32 points=0;
    if(first<end){const ConstSlice<u8> data{font.glyf.data+first,end-first};if(!span(data,0,10))return SfntStatus::truncated;const i16 contours=static_cast<i16>(r16(data,0));
        if(contours>=0){if(!span(data,10,static_cast<u32>(contours)*2))return SfntStatus::truncated;if(!spend(work,contours))return SfntStatus::resource_limit;
            for(u32 i=0;i<static_cast<u32>(contours);++i){const u32 last=r16(data,10+i*2);if(i&&last<points)return SfntStatus::malformed;points=last+1;}}
        else{if(contours!=-1)return SfntStatus::malformed;Reader components{data,10};u16 flags=0,selected=0xffff;double matrix[4]{1,0,0,1};
            do{if(!spend(work)||points==65536)return SfntStatus::resource_limit;++points;ConstSlice<u8> record{};if(!components.take(4,&record))return SfntStatus::truncated;
                flags=r16(record,0);const u16 child=r16(record,2);if(child>=font.metrics.glyph_count)return SfntStatus::glyph_out_of_range;
                if(!components.take(flags&1?4:2,&record))return SfntStatus::truncated;const u32 count=composite_transform_words(flags);
                if(count==0xff)return SfntStatus::malformed;if(!components.take(count*2,&record))return SfntStatus::truncated;
                if(flags&0x200){selected=child;matrix[0]=matrix[3]=1;matrix[1]=matrix[2]=0;
                    if(count){matrix[0]=static_cast<i16>(r16(record,0))/16384.;matrix[3]=count==1?matrix[0]:static_cast<i16>(r16(record,(count-1)*2))/16384.;}
                    if(count==4){matrix[1]=static_cast<i16>(r16(record,2))/16384.;matrix[2]=static_cast<i16>(r16(record,4))/16384.;}}
            }while(flags&0x20);
            if(selected!=0xffff){Advance child{};status=glyph_advance(font,instance,selected,chain,depth+1,&child,work);if(status!=SfntStatus::ok)return status;
                *output={child.x*matrix[0]+child.y*matrix[2],child.x*matrix[1]+child.y*matrix[3]};return SfntStatus::ok;}
        }
    }
    double values[2]{};status=phantoms(instance,glyph,points,values,work);if(status!=SfntStatus::ok)return status;
    *output={metrics.advance+rounded(values[1])-rounded(values[0]),0};return SfntStatus::ok;
}
#pragma float_control(pop)
}
SfntStatus sfnt_gvar_open(ConstSlice<u8> table,u16 axes,u16 glyphs,SfntGvar* output,u32 work,u32* used) noexcept {
    WorkReport report{work,work,used};if(!output)return SfntStatus::invalid_input;reset_value(output);if(!span(table,0,20))return SfntStatus::truncated;
    if(r16(table,0)!=1 || r16(table,2)!=0)return SfntStatus::unsupported;if(r16(table,4)!=axes || r16(table,12)!=glyphs || (r16(table,14)&~1))return SfntStatus::malformed;
    const u16 shared=r16(table,6),stride=r16(table,14)&1?4:2;const u32 tuples=r32(table,8),data=r32(table,16),header=20+(static_cast<u32>(glyphs)+1)*stride,cells=static_cast<u32>(shared)*axes;
    if(shared>4096 || data<header || (shared && tuples<header))return SfntStatus::malformed;
    if(!span(table,0,header) || !span(table,data,0) || (shared && !span(table,tuples,cells*2)))return SfntStatus::truncated;
    SfntGvar result{table,{table.data+20,header-20},shared?ConstSlice<u8>{table.data+tuples,cells*2}:ConstSlice<u8>{},data,axes,glyphs,shared,stride};
    *output=result;return SfntStatus::ok;
}
SfntStatus sfnt_gvar_instance_prepare(const SfntGvar& font,ConstSlice<double> coordinates,Slice<double> storage,SfntGvarInstance* output,u32 work,u32* used) noexcept {
    WorkReport report{work,work,used};if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!font.table.data || coordinates.count!=font.axis_count || (coordinates.count && !coordinates.data))return SfntStatus::invalid_input;
    if(storage.count<font.shared_count || (font.shared_count && !storage.data))return SfntStatus::output_too_small;
    if(!spend(work,coordinates.count))return SfntStatus::resource_limit;for(u32 i=0;i<coordinates.count;++i)if(!(coordinates[i]>=-1 && coordinates[i]<=1))return SfntStatus::invalid_input;
    for(u32 i=0;i<font.shared_count;++i){if(!spend(work))return SfntStatus::resource_limit;const auto s=region({font.shared_tuples.data+i*font.axis_count*2,static_cast<u32>(font.axis_count)*2},{},{},coordinates,storage.data+i,work);if(s!=SfntStatus::ok)return s;}
    *output={font,coordinates,{storage.data,font.shared_count}};return SfntStatus::ok;
}
SfntStatus sfnt_gvar_design_advance(const SfntTrueType& font,const SfntGvarInstance& instance,u16 glyph,u16* output,u32 work,u32* used) noexcept {
    WorkReport report{work,work,used};if(!output)return SfntStatus::invalid_input;*output=0;const auto& g=instance.font;
    if(!g.table.data||g.glyph_count!=font.metrics.glyph_count||instance.coordinates.count!=g.axis_count||instance.shared_scalars.count!=g.shared_count||
       (g.axis_count&&!instance.coordinates.data)||(g.shared_count&&!instance.shared_scalars.data)||(font.loca_stride!=2&&font.loca_stride!=4)||
       !font.loca.data||(font.glyf.count&&!font.glyf.data)||font.loca.count/font.loca_stride<=font.metrics.glyph_count)return SfntStatus::invalid_input;
    if(glyph>=g.glyph_count)return SfntStatus::glyph_out_of_range;
    u16 chain[64];Advance advance{};const auto status=glyph_advance(font,instance,glyph,{chain,64},0,&advance,work);if(status!=SfntStatus::ok)return status;
    if(!(advance.x>=-9.e15&&advance.x<=9.e15))return SfntStatus::malformed;*output=static_cast<u16>(static_cast<i64>(rounded(advance.x)));return SfntStatus::ok;
}
SfntStatus sfnt_gvar_deltas(const SfntGvarInstance& instance,u16 glyph,ConstSlice<SfntOutlinePoint> points,ConstSlice<u32> ends,bool composite,SfntGvarScratch scratch,Slice<SfntGvarDelta> output,u32 work,u32* used,u32 origin) noexcept {
    WorkReport report{work,work,used};const auto& font=instance.font;
    if(!font.table.data || instance.coordinates.count!=font.axis_count || instance.shared_scalars.count!=font.shared_count || (instance.coordinates.count && !instance.coordinates.data) || (instance.shared_scalars.count && !instance.shared_scalars.data) || (!composite && points.count && !points.data) || (ends.count && !ends.data) || points.count>65536 || (composite && ends.count))return SfntStatus::invalid_input;
    if(glyph>=font.glyph_count)return SfntStatus::glyph_out_of_range;const u32 total=points.count+4;
    if(output.count<total || !output.data)return SfntStatus::output_too_small;
    if(!spend(work,ends.count) || !spend(work,total))return SfntStatus::resource_limit;
    u32 previous=0;for(u32 i=0;i<ends.count;++i){if(ends[i]<origin || ends[i]-origin>=points.count || (i && ends[i]<=previous))return SfntStatus::invalid_input;previous=ends[i];}
    if(!composite && (points.count?(ends.count==0 || previous-origin+1!=points.count):ends.count!=0))return SfntStatus::invalid_input;
    for(u32 i=0;i<total;++i)reset_value(&output[i]);Tuples tuples{instance};auto status=tuples.open(glyph);if(status!=SfntStatus::ok)return status;
    PointList shared{{},total,true};
    if(tuples.shared){const auto s=point_list(tuples.serialized,total,scratch.shared_points,&shared,work);if(s!=SfntStatus::ok)return s;}
    for(u32 i=0;i<tuples.count;++i){ConstSlice<u8> packed{};u16 index=0;double scale=0;
        status=tuples.next(&packed,&index,&scale,work);if(status!=SfntStatus::ok)return status;
        if(scale==0)continue;Reader stream{packed};PointList list=shared;
        if(index&0x2000){const auto s=point_list(stream,total,scratch.private_points,&list,work);if(s!=SfntStatus::ok)return s;}
        if(!list.all && (scratch.private_points.count<list.count || !scratch.private_points.data))return SfntStatus::output_too_small;
        auto s=deltas(stream,list.count,scratch.x_deltas,work);if(s!=SfntStatus::ok)return s;s=deltas(stream,list.count,scratch.y_deltas,work);if(s!=SfntStatus::ok)return s;
        s=apply(list,points,ends,composite,scratch,output,scale,work,origin);if(s!=SfntStatus::ok)return s;
    }return SfntStatus::ok;
}
} // namespace maicord
