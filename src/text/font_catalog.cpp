#include "font_catalog_append.hpp"
#include "font_metadata_read.hpp"
#include "font_path_stat.hpp"
#include <Windows.h>
#include <stddef.h>
namespace maicord {
static_assert(sizeof(Win32FileTime)==sizeof(FILETIME)&&alignof(Win32FileTime)==alignof(FILETIME)&&
              offsetof(Win32FileTime,dwHighDateTime)==offsetof(FILETIME,dwHighDateTime));
static_assert(sizeof(Win32FileId128)==sizeof(FILE_ID_128)&&sizeof(Win32FileIdInfo)==sizeof(FILE_ID_INFO)&&
              alignof(Win32FileIdInfo)==alignof(FILE_ID_INFO)&&offsetof(Win32FileIdInfo,FileId)==offsetof(FILE_ID_INFO,FileId));
namespace {
template<class T> bool valid(Slice<T> s,u32 used) noexcept {return used<=s.count&&(!s.count||s.data);}
bool valid(const NativeFontCatalog& c) noexcept {return valid(c.files,c.count.files)&&valid(c.faces,c.count.faces)&&valid(c.candidates,c.count.candidates)&&valid(c.settings,c.count.settings)&&valid(c.strings,c.count.strings);}
NativeFontResult result(NativeFontError e,SfntStatus s=SfntStatus::ok,u32 native=0) noexcept {return {e,s,native};}
bool same(const Win32FileIdInfo& a,const Win32FileIdInfo& b) noexcept {return a.VolumeSerialNumber==b.VolumeSerialNumber&&bytes_equal(&a.FileId,&b.FileId,sizeof(FILE_ID_128));}
bool room(u32 used,u32 capacity,u32 amount) noexcept {return used<=capacity&&amount<=capacity-used;}
ConstSlice<u16> string(const NativeFontCatalog& c,u32 offset,u32 units) noexcept {return units&&room(offset,c.count.strings,units)?ConstSlice<u16>{c.strings.data+offset,units}:ConstSlice<u16>{};}
}
NativeFontResult native_font_catalog_append(NativeFontCatalog* catalog,const wchar_t* path,Slice<u16> scratch,u32* output,u32 maximum_faces,u32 work_per_face,NativeFontCatalogCounts* needed,bool default_face_only,Slice<u8> metadata_scratch,NativeFontCatalogGrowth growth,NativeFontFileStamp* stamp,bool* no_reparse) noexcept {
    
    if(no_reparse)*no_reparse=false;
    if(stamp)*stamp={};
    if(needed)*needed={};
    if(output)*output=0xffffffffu;
    if(!catalog||!valid(*catalog)||!path||!*path||!output||!scratch.data||!scratch.count||scratch.count>32768||(metadata_scratch.count&&!metadata_scratch.data))return result(NativeFontError::invalid_input);
    u32 path_units=0;while(path_units<32768&&path[path_units])++path_units;if(path_units==32768)return result(NativeFontError::invalid_input);
    // Metadata discovery reads scattered small tables, not a sequential font stream.
    HANDLE file;
    {
        file=font_path_stat::open_read({path,path_units},{reinterpret_cast<wchar_t*>(scratch.data),scratch.count},no_reparse);}
    if(file==INVALID_HANDLE_VALUE)return result(NativeFontError::io,SfntStatus::ok,GetLastError());
    const u8* data=nullptr;struct Cleanup {HANDLE f;const u8*& p;~Cleanup() noexcept {if(p)UnmapViewOfFile(p);CloseHandle(f);}} cleanup{file,data};
    NativeFontFileStamp actual{};
    {
    if(!font_path_stat::read_handle(file,&actual)){
        BY_HANDLE_FILE_INFORMATION info{};FILE_BASIC_INFO basic{};
        if(GetFileType(file)!=FILE_TYPE_DISK)return result(NativeFontError::io,SfntStatus::ok,ERROR_FILE_INVALID);
        if(!GetFileInformationByHandle(file,&info)||!GetFileInformationByHandleEx(file,FileIdInfo,&actual.identity,sizeof(actual.identity))||
           !GetFileInformationByHandleEx(file,FileBasicInfo,&basic,sizeof(basic)))return result(NativeFontError::io,SfntStatus::ok,GetLastError());
        if(no_reparse&&(basic.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))*no_reparse=false;
        if(!bytes_equal(&basic.LastWriteTime,&info.ftLastWriteTime,8))return result(NativeFontError::io,SfntStatus::ok,ERROR_RETRY);
        actual.bytes=(static_cast<u64>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;*actual.write.native()=info.ftLastWriteTime;copy_bytes(&actual.change,&basic.ChangeTime,8);
    }
    }
    if(stamp)*stamp=actual;
    if(!actual.bytes||actual.bytes>0xffffffffu)return result(NativeFontError::font,SfntStatus::unsupported);
    NativeFontCatalogFile item{};item.identity=actual.identity;item.bytes=static_cast<u32>(actual.bytes);item.last_write=actual.write;item.change_time=actual.change;
    auto& c=*catalog;
    if(needed)*needed=c.count;
    const auto capacity=[&](NativeFontCatalogCounts counts) noexcept {if(needed)*needed=counts;return growth.reserve&&growth.reserve(growth.context,counts);};
    for(u32 i=0;i<c.count.files;++i){const auto& old=c.files[i];if(!same(old.identity,item.identity))continue;
        if(old.bytes!=item.bytes||(!bytes_equal(&old.last_write,&item.last_write,sizeof(FILETIME))||!bytes_equal(&old.change_time,&item.change_time,8)))return result(NativeFontError::io,SfntStatus::ok,ERROR_RETRY);
        *output=i;return result(NativeFontError::none);}
    if(!room(c.count.files,c.files.count,1)){auto counts=c.count;counts.files++;if(!capacity(counts))return result(NativeFontError::capacity);}
    if(!room(c.count.strings,c.strings.count,path_units+1)){auto counts=c.count;counts.strings+=path_units+1;if(!capacity(counts))return result(NativeFontError::capacity);}
    SfntFace face{};
    font_metadata::Reader metadata_reader(file,static_cast<u32>(item.bytes));
    const auto open_face=[&](u32 index) noexcept -> NativeFontResult {
        
        if(!data&&metadata_scratch.count){const auto read=metadata_reader.read(index,metadata_scratch,&face);
            if(read.error!=NativeFontError::font||read.font_status!=SfntStatus::output_too_small)return read;}
        if(!data){const HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping)return result(NativeFontError::io,SfntStatus::ok,GetLastError());
            data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));const u32 map_error=data?0:GetLastError();CloseHandle(mapping);if(!data)return result(NativeFontError::io,SfntStatus::ok,map_error);}
        const auto status=sfnt_open({data,static_cast<u32>(item.bytes)},index,&face);return result(status==SfntStatus::ok?NativeFontError::none:NativeFontError::font,status);
    };
    auto opened=open_face(0);if(!opened.ok())return opened;SfntStatus status=SfntStatus::ok;
    const u32 face_count=default_face_only?1u:face.face_count;
    if(face_count>maximum_faces||face_count>65536)return result(NativeFontError::font,SfntStatus::resource_limit);
    if(!room(c.count.faces,c.faces.count,face_count)){auto counts=c.count;counts.faces+=face_count;if(!capacity(counts))return result(NativeFontError::capacity);}
    auto staged=c.count;item.path_offset=staged.strings;item.path_units=static_cast<u16>(path_units);item.first_face=staged.faces;item.face_count_minus_one=static_cast<u16>(face_count-1);
    copy_bytes(c.strings.data+staged.strings,path,(path_units+1)*2);staged.strings+=path_units+1;
    for(u32 i=0;i<item.face_count();++i){
        if(i){opened=open_face(i);if(!opened.ok())return opened;}
        NativeFontFamily family{};NativeFontCandidates candidates{};
        {status=native_font_family_candidates(face,scratch,&family,&candidates,work_per_face);}
        if(status!=SfntStatus::ok)return result(NativeFontError::font,status);
        
        u32 default_candidate=0;const u32 candidate_count=default_face_only?1u:candidates.count;
        if(default_face_only&&candidates.axes.axis_count){bool found=false;
            for(u32 j=0;j<candidates.count;++j){FontAxisSetting settings[2];u32 count=0;status=native_font_candidate_settings(candidates,j,{settings,2},&count);if(status!=SfntStatus::ok)return result(NativeFontError::font,status);
                bool normal=true;for(u32 k=0;k<count;++k){FontAxis axis{};font_axis(candidates.axes,k,&axis);normal=normal&&settings[k].value==axis.normal/65536.0;}
                if(normal){default_candidate=j;found=true;break;}}
            if(!found)return result(NativeFontError::font,SfntStatus::unsupported);}
        if(!room(staged.candidates,c.candidates.count,candidate_count)){auto counts=staged;counts.candidates+=candidate_count;if(!capacity(counts))return result(NativeFontError::capacity);}
        u32 name_offset=0xffffffffu;
        for(u32 j=0;j<staged.faces;++j){const auto& prior=c.faces[j];if(prior.name_units==family.units&&bytes_equal(c.strings.data+prior.name_offset,scratch.data,family.units*2)){name_offset=prior.name_offset;break;}}
        if(name_offset==0xffffffffu){if(!room(staged.strings,c.strings.count,family.units+1)){auto counts=staged;counts.strings+=family.units+1;if(!capacity(counts))return result(NativeFontError::capacity);}
            name_offset=staged.strings;copy_bytes(c.strings.data+staged.strings,scratch.data,family.units*2);c.strings[staged.strings+family.units]=0;staged.strings+=family.units+1;}
        const u32 face_slot=staged.faces;NativeFontCatalogFace metadata{staged.files,i,name_offset,family.units,staged.candidates,candidate_count};
        for(u32 j=0;j<candidate_count;++j){const u32 selected=default_face_only?default_candidate:j;NativeFontCandidate candidate{};status=native_font_candidate(candidates,selected,&candidate);if(status!=SfntStatus::ok)return result(NativeFontError::font,status);
            if(!room(staged.settings,c.settings.count,candidates.axes.axis_count)){auto counts=staged;counts.settings+=candidates.axes.axis_count;if(!capacity(counts))return result(NativeFontError::capacity);}u32 settings=0;
            Slice<FontAxisSetting> target{c.settings.data?c.settings.data+staged.settings:nullptr,c.settings.count-staged.settings};
            status=native_font_candidate_settings(candidates,selected,target,&settings);if(status!=SfntStatus::ok)return result(NativeFontError::font,status);
            c.candidates[staged.candidates++]={candidate.properties,face_slot,candidate.named_index,staged.settings,settings};staged.settings+=settings;
        }c.faces[staged.faces++]=metadata;
    }
    c.files[staged.files++]=item;c.count=staged;*output=staged.files-1;return result(NativeFontError::none);
}
NativeFontResult native_font_catalog_add(NativeFontCatalog* catalog,const wchar_t* path,Slice<u16> scratch,u32* output,u32 maximum_faces,u32 work_per_face,NativeFontCatalogCounts* needed,bool default_face_only,Slice<u8> metadata_scratch) noexcept {
    return native_font_catalog_append(catalog,path,scratch,output,maximum_faces,work_per_face,needed,default_face_only,metadata_scratch,{});
}
ConstSlice<u16> native_font_catalog_path(const NativeFontCatalog& c,u32 file) noexcept {if(!valid(c)||file>=c.count.files)return {};const auto& f=c.files[file];return string(c,f.path_offset,f.path_units);}
ConstSlice<u16> native_font_catalog_name(const NativeFontCatalog& c,u32 face) noexcept {if(!valid(c)||face>=c.count.faces)return {};const auto& f=c.faces[face];return string(c,f.name_offset,f.name_units);}
ConstSlice<FontAxisSetting> native_font_catalog_settings(const NativeFontCatalog& c,u32 candidate) noexcept {
    if(!valid(c)||candidate>=c.count.candidates)return {};const auto& v=c.candidates[candidate];if(!v.setting_count||!room(v.first_setting,c.count.settings,v.setting_count))return {};
    return {c.settings.data+v.first_setting,v.setting_count};
}
SfntStatus native_font_catalog_match(const NativeFontCatalog& c,ConstSlice<u16> name,NativeFontProperties requested,Slice<NativeFontProperties> properties,Slice<u32> indices,NativeFontCatalogMatch* output,u32 maximum_faces) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!valid(c)||!name.data||!name.count||name.count>32767||!valid(properties,0)||!valid(indices,0))return SfntStatus::invalid_input;
    if(c.count.faces>maximum_faces)return SfntStatus::resource_limit;u32 count=0;
    for(u32 i=0;i<c.count.faces;++i){const auto& face=c.faces[i];const auto family=native_font_catalog_name(c,i);
        if(family.count!=name.count)continue;
        const int comparison=CompareStringOrdinal(reinterpret_cast<const wchar_t*>(family.data),static_cast<int>(family.count),reinterpret_cast<const wchar_t*>(name.data),static_cast<int>(name.count),TRUE);
        if(!comparison)return SfntStatus::invalid_input;if(comparison!=CSTR_EQUAL)continue;
        if(!room(face.first_candidate,c.count.candidates,face.candidate_count))return SfntStatus::invalid_input;
        if(!room(count,properties.count,face.candidate_count)||!room(count,indices.count,face.candidate_count))return SfntStatus::output_too_small;
        for(u32 j=0;j<face.candidate_count;++j){const u32 index=face.first_candidate+j;properties[count]=c.candidates[index].properties;indices[count++]=index;}
    }
    NativeFontMatch match{};const auto status=native_font_match({properties.data,count},requested,&match,properties.count);if(status!=SfntStatus::ok)return status;
    *output={indices[match.index],match.properties,match.simulations};return SfntStatus::ok;
}
NativeFontResult native_font_catalog_entry(const NativeFontCatalog& c,u32 candidate,NativeFontCatalogEntry* output) noexcept {
    if(output)*output={};if(!output||!valid(c)||candidate>=c.count.candidates)return result(NativeFontError::invalid_input);
    const u32 face_index=c.candidates[candidate].face;if(face_index>=c.count.faces)return result(NativeFontError::invalid_input);
    const auto& face=c.faces[face_index];if(face.file>=c.count.files)return result(NativeFontError::invalid_input);const auto& file=c.files[face.file];
    const auto path=native_font_catalog_path(c,face.file);if(!path.count||!room(file.path_offset,c.count.strings,path.count+1)||c.strings[file.path_offset+path.count])return result(NativeFontError::invalid_input);
    const auto& value=c.candidates[candidate];if(!room(value.first_setting,c.count.settings,value.setting_count))return result(NativeFontError::invalid_input);
    const auto settings=native_font_catalog_settings(c,candidate);*output={path,settings,&file,face.index};return result(NativeFontError::none);
}
NativeFontResult native_font_catalog_validate(const NativeFontCatalogEntry& entry,const NativeFontStore& store,NativeFontHandle handle) noexcept {
    if(!entry.file)return result(NativeFontError::invalid_input);const auto* selected=native_font_get(store,handle);if(!selected)return result(NativeFontError::invalid_input);
    const auto& file=*entry.file;const auto& opened=store.files[store.faces[handle.slot-1].file_slot];FILE_BASIC_INFO basic{};
    if(selected->geometry.face.face_index!=entry.face_index)return result(NativeFontError::io,SfntStatus::ok,ERROR_RETRY);
    if(!GetFileInformationByHandleEx(opened.file,FileBasicInfo,&basic,sizeof(basic)))return result(NativeFontError::io,SfntStatus::ok,GetLastError());
    if(!bytes_equal(&file.identity,&opened.identity,sizeof(opened.identity))||file.bytes!=opened.bytes.count||(!bytes_equal(&basic.LastWriteTime,&file.last_write,8)||!bytes_equal(&basic.ChangeTime,&file.change_time,8)))return result(NativeFontError::io,SfntStatus::ok,ERROR_RETRY);
    return result(NativeFontError::none);
}
NativeFontResult native_font_catalog_acquire(const NativeFontCatalog& c,u32 candidate,NativeFontStore* store,NativeFontHandle* output) noexcept {
    if(output)*output={};if(!store||!output)return result(NativeFontError::invalid_input);
    NativeFontCatalogEntry entry{};auto status=native_font_catalog_entry(c,candidate,&entry);if(!status.ok())return status;
    NativeFontHandle handle{};status=native_font_acquire(store,reinterpret_cast<const wchar_t*>(entry.path.data),entry.face_index,&handle);if(!status.ok())return status;
    status=native_font_catalog_validate(entry,*store,handle);
    if(!status.ok()){native_font_release(store,handle);return status;}*output=handle;return status;
}
}
