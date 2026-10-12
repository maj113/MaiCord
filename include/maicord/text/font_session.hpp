#pragma once
#include "maicord/text/font_instance.hpp"
namespace maicord {
struct NativeFontSession;
struct NativeFontRunStore;
struct NativeFontRunHandle;
struct NativeFontSelection;
struct NativeFontSessionStats {
    NativeFontStoreStats live;
    usize metadata_committed,instance_committed;
    u64 peak_mapped_bytes;
    u32 instances,peak_files,peak_faces,peak_instances,references;
};
// Shared owning-thread store. Pools expose stable prefixes in groups of eight up to
// 1024 slots; the first 64 file records share the owner's committed page, and the file
// tail and other pools share one reservation that commits on demand. Exposed prefixes
// and live mappings/views never move on growth. Face acquisition grows around its open
// read handle without reopening the path. Each consumer retains the session while
// holding any store-local handle.
bool native_font_session_create(NativeFontSession**) noexcept;
bool native_font_session_retain(NativeFontSession*) noexcept;
// Last release fails without clearing the pointer if live handles remain.
bool native_font_session_release(NativeFontSession**) noexcept;
NativeFontStore* native_font_session_fonts(NativeFontSession*) noexcept;
NativeFontInstanceStore* native_font_session_instances(NativeFontSession*) noexcept;
NativeFontRunStore* native_font_session_runs(NativeFontSession*) noexcept;
NativeFontResult native_font_session_run(NativeFontSession*,NativeFontInstanceHandle,const NativeFontSelection&,u32 simulations,NativeFontRunHandle*) noexcept;
NativeFontSessionStats native_font_session_stats(const NativeFontSession*) noexcept;
NativeFontResult native_font_session_face(NativeFontSession*,const wchar_t* path,u32 index,NativeFontHandle*) noexcept;
NativeFontResult native_font_session_instance(NativeFontSession*,NativeFontHandle,ConstSlice<FontAxisSetting>,
    bool instructions,NativeFontPrepareScratch,NativeFontInstanceHandle*) noexcept;
}
