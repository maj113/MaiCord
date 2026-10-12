#pragma once
#include "maicord/text/font_catalog.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>

namespace maicord::font_metadata {
// Projection for family/candidate discovery: the table directory plus only OS/2,
// STAT, fvar and name, after validating every original directory span. Not a
// font image; never pass it to outline/shaping preparation. Returns
// output_too_small for more than 64 tables or when the copied tables exceed
// storage, so callers can use the general mapped-file path. The borrowed handle
// must be synchronous and opened excluding writers/deletion; the cached file
// prefix is reused across collection faces while that handle stays open.
class Reader {
    HANDLE file;u32 bytes;u8 prefix[1024];bool ready=false;
public:
    Reader(HANDLE source,u32 length) noexcept:file(source),bytes(length) {}
    NativeFontResult read(u32 index,Slice<u8> storage,SfntFace* output) noexcept {
        *output={};const auto error=[](SfntStatus status) noexcept {return NativeFontResult{NativeFontError::font,status,0};};
        const u32 prefix_bytes=minimum<u32>(bytes,sizeof(prefix));
        const auto read_at=[&](u32 offset,void* target,u32 count) noexcept -> NativeFontResult {
            if(offset>bytes||count>bytes-offset)return error(SfntStatus::truncated);
            if(!count)return {NativeFontError::none,SfntStatus::ok,0};
            OVERLAPPED at{};at.Offset=offset;DWORD got=0;
            if(!ReadFile(file,target,count,&got,&at))return {NativeFontError::io,SfntStatus::ok,GetLastError()};
            return got==count?NativeFontResult{NativeFontError::none,SfntStatus::ok,0}:error(SfntStatus::truncated);
        };
        if(bytes<12)return error(SfntStatus::truncated);
        NativeFontResult status{};
        if(!ready){status=read_at(0,prefix,prefix_bytes);if(!status.ok())return status;ready=true;}
        const auto fetch=[&](u32 offset,void* target,u32 count) noexcept -> NativeFontResult {
            if(offset<=prefix_bytes&&count<=prefix_bytes-offset){copy_bytes(target,prefix+offset,count);return {NativeFontError::none,SfntStatus::ok,0};}
            return read_at(offset,target,count);
        };
        u32 offset=0,faces=1;
        if(byte_load_be32(prefix)==sfnt_tag('t','t','c','f')){
            const u32 version=byte_load_be32(prefix+4);if(version!=0x10000&&version!=0x20000)return error(SfntStatus::unsupported);
            faces=byte_load_be32(prefix+8);if(!faces)return error(SfntStatus::malformed);
            if(faces>(bytes-12)/4||(version==0x20000&&bytes-12-faces*4<12))return error(SfntStatus::truncated);
            if(index>=faces)return error(SfntStatus::face_out_of_range);
            u8 value[4];status=fetch(12+index*4,value,4);if(!status.ok())return status;offset=byte_load_be32(value);
        }else if(index)return error(SfntStatus::face_out_of_range);
        u8 header[12];status=fetch(offset,header,12);if(!status.ok())return status;
        const u32 version=byte_load_be32(header);
        if(version!=0x10000&&version!=sfnt_tag('O','T','T','O')&&version!=sfnt_tag('t','r','u','e'))return error(SfntStatus::unsupported);
        const u32 count=byte_load_be16(header+4);if(!count)return error(SfntStatus::malformed);
        if(count>(bytes-offset-12)/16)return error(SfntStatus::truncated);
        u8 directory[1024];if(count>sizeof(directory)/16)return error(SfntStatus::output_too_small);
        status=fetch(offset+12,directory,count*16);if(!status.ok())return status;
        u32 selected[4]{},selected_count=0,required=0,previous=0;
        for(u32 i=0;i<count;++i){const auto* record=directory+i*16;const u32 tag=byte_load_be32(record),at=byte_load_be32(record+8),size=byte_load_be32(record+12);
            if(i&&tag<=previous)return error(SfntStatus::malformed);previous=tag;
            if(at>bytes||size>bytes-at)return error(SfntStatus::truncated);
            if(tag==sfnt_tag('O','S','/','2')||tag==sfnt_tag('S','T','A','T')||tag==sfnt_tag('f','v','a','r')||tag==sfnt_tag('n','a','m','e')){
                if(size>storage.count-required||storage.count-required-size<16)return error(SfntStatus::output_too_small);
                required+=size+16;selected[selected_count++]=i;}
        }
        // No needed table: keep a one-entry directory (first record, zero offset/length).
        if(!selected_count){if(storage.count<16)return error(SfntStatus::output_too_small);copy_bytes(storage.data,directory,16);byte_store_be32(storage.data+8,0);byte_store_be32(storage.data+12,0);required=16;}
        u32 used=selected_count*16;
        for(u32 i=0;i<selected_count;++i){const auto* record=directory+selected[i]*16;const u32 at=byte_load_be32(record+8),size=byte_load_be32(record+12);
            copy_bytes(storage.data+i*16,record,16);byte_store_be32(storage.data+i*16+8,used);
            status=fetch(at,storage.data+used,size);if(!status.ok())return status;used+=size;
        }
        *output={{storage.data,required},{storage.data,maximum<u32>(selected_count,1)*16},index,faces,version};return {NativeFontError::none,SfntStatus::ok,0};
    }
};
inline NativeFontResult read(HANDLE file,u32 bytes,u32 index,Slice<u8> storage,SfntFace* output) noexcept {
    Reader reader(file,bytes);return reader.read(index,storage,output);
}
}
