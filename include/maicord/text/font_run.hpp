#pragma once
#include "maicord/text/font_prepare.hpp"
#include "maicord/text/font_run_handle.hpp"
namespace maicord {
struct NativeFontRun {NativeFontSelection source;NativeFontInstanceHandle instance;u32 simulations;};
struct NativeFontRunSlot {NativeFontRun run;u32 references,generation;};
struct NativeFontRunStore {NativeFontSession* session;Slice<NativeFontRunSlot> slots;};
// Stable slots used from the owning thread, with store-local generation handles.
// Each live record holds one raw instance; consumers keep the session alive while
// they hold handles. source must be the face/settings used to prepare instance. Equivalent
// normalized instances and simulations share a record and its copied selection.
NativeFontResult native_font_run_acquire(NativeFontRunStore*,NativeFontInstanceHandle,const NativeFontSelection&,u32 simulations,NativeFontRunHandle*) noexcept;
const NativeFontRun* native_font_run_get(const NativeFontRunStore&,NativeFontRunHandle) noexcept;
bool native_font_run_retain(NativeFontRunStore*,NativeFontRunHandle) noexcept;
bool native_font_run_release(NativeFontRunStore*,NativeFontRunHandle) noexcept;
// Prepare an outline instance directly from a native run record.
// Scratch resets. Simulation permissions match native_font_prepare.
NativeFontResult native_font_run_prepare(NativeFontRunStore*,NativeFontRunHandle,Arena*,NativeFontInstanceHandle*,bool* oblique=nullptr,bool* bold=nullptr,bool instructions=true) noexcept;
static_assert(sizeof(NativeFontRunHandle)==8);
}
