#pragma once
#include "maicord/text/font_catalog.hpp"
#include "maicord/text/font_prepare.hpp"
#include "maicord/text/font_catalog_coverage.hpp"
#include "maicord/text/font_fallback_range.hpp"
namespace maicord {
// Scope zero is the system catalog; private scopes are registered font files.
// Serialize access on the text context's thread. Initialize a zeroed, address-stable catalog.
struct NativeTextCatalogScope {
    const wchar_t* path;NativeFontCatalog catalog;
};
// Only the system scope is embedded. Registered private scopes and their paths
// share one arena allocation each; committed addresses never move.
struct NativeTextCatalogScopes {
    NativeTextCatalogScope system;
    NativeTextCatalogScope* entries[32];
    NativeTextCatalogScope& operator[](u32 index) noexcept {return *entries[index];}
    const NativeTextCatalogScope& operator[](u32 index) const noexcept {return *entries[index];}
};
static_assert(sizeof(NativeTextCatalogScopes)==368);
struct NativeTextCatalogs {
    // One attempted-build bit per stable scope, including cached failures.
    Arena storage;NativeTextCatalogScopes scopes;u32 count,operation_depth,attempted;
    // Sticky acquisition-mismatch bits, one per scope. Catalogs are never cleared
    // or replaced during active shaping.
    u32 stale_scopes;
    u64 builds,selections,unsupported,failures,build_ticks;
    // Failure epoch. A binding attempt must not keep a negative result after a
    // temporary capacity failure.
    u32 retryable_failures;
    struct FamilyOrder* families;
    struct NativeSystemFontFile* files;
    u32 next_candidate;
    NativeFontCoverageCache coverage;NativeFontCoverageSlot coverage_slots[16];
};
bool native_text_catalogs_initialize(NativeTextCatalogs*) noexcept;
void native_text_catalogs_release(NativeTextCatalogs*) noexcept;
// Declares face zero at default axes of the file at path; copies the path.
// Repeating the same path returns the existing scope.
bool native_text_catalog_register_path(NativeTextCatalogs*,const wchar_t*,u32* scope) noexcept;
// Borrowed request independent of a format object. Scope indices are stable for
// this catalog. Family storage belongs to the caller; selection does not keep it.
struct NativeTextFontQuery {u32 scope;ConstSlice<u16> family;NativeFontProperties properties;};
// Supplying selected_scope also opts into the default-face fallback for
// a missing requested family. Other failures never trigger that fallback.
// Optional missing_family reports that successful fallback without repeating
// family matching. It is false on a direct family match or failure.
bool native_text_catalog_select_query(NativeTextCatalogs*,const NativeTextFontQuery&,NativeFontSession*,Arena*,
    NativeFontInstanceHandle*,NativeFontSelection*,u32* candidate=nullptr,u32* simulations=nullptr,u32* selected_scope=nullptr,bool* missing_family=nullptr) noexcept;
struct NativeTextFontRange {NativeFontCatalogMatch match;u32 bytes,units,scope;};
// en-US policy: preserves the base-family and fixed system-route preference,
// including a missing base family. Returns the actual selected scope. First use
// can prepare cached family orders and reset scratch; warmed queries borrow
// immutable catalog state. Unknown collections and IO/parser failures stay errors.
bool native_text_catalog_range(NativeTextCatalogs*,const NativeTextFontQuery&,NativeFontSession*,Arena*,Utf8View,NativeTextFontRange*) noexcept;
// Prepare an already selected catalog face with its simulation policy.
bool native_text_catalog_select_face(NativeTextCatalogs*,u32 scope,u32 candidate,u32 simulations,NativeFontSession*,Arena*,NativeFontInstanceHandle*,NativeFontSelection*,bool refresh=false) noexcept;
}
