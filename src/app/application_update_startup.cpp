#include "application_update_startup.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/app/update_service.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
UpdateHandoffChild g_child;
bool g_child_open;
constexpr wchar_t child_family[]=L"--maicord-update-child";
constexpr u32 family_characters=sizeof(child_family)/sizeof(wchar_t)-1;
bool space(wchar_t c) noexcept {return c==L' ' || (c>=L'\t' && c<=L'\r');}
wchar_t fold(wchar_t c) noexcept {return c>=L'A' && c<=L'Z'?static_cast<wchar_t>(c+(L'a'-L'A')):c;}
struct Prefix {
    u32 matched{};
    bool possible{true};
    bool character(wchar_t c) noexcept {
        if(!possible) return false;
        if(fold(c)!=child_family[matched]) {possible=false;return false;}
        return ++matched==family_characters;
    }
};
}
ConstSlice<wchar_t> application_update_startup_command(const wchar_t* raw) noexcept {
    u32 n{};
    if(raw) while(n<kApplicationUpdateStartupCommandCharacters && raw[n]) ++n;
    return {raw,n};
}
ApplicationUpdateStartupCommandKind application_update_startup_classify(ConstSlice<wchar_t> line) noexcept {
    using Kind=ApplicationUpdateStartupCommandKind;
    if(!line.data || !line.count || line.count>=kApplicationUpdateStartupCommandCharacters) return Kind::invalid;
    // Reject an explicit snapshot with a hidden NUL rather than ignoring its
    // tail. All reads remain within the declared complete command-line bound.
    for(u32 i=0;i<line.count;++i) if(!line[i]) return Kind::invalid;
    u32 at{};
    while(at<line.count && space(line[at])) ++at;
    if(at==line.count) return Kind::invalid;
    // argv0 has special program-name quoting: quotes protect spaces; do not
    // classify executable filenames or apply argument backslash unescaping.
    bool quoted=false, program=false;
    while(at<line.count) {
        const wchar_t c=line[at];
        if(!quoted && space(c)) break;
        ++at;
        if(c==L'"') quoted=!quoted;
        else program=true;
    }
    if(quoted || !program) return Kind::invalid;
    while(at<line.count) {
        while(at<line.count && space(line[at])) ++at;
        if(at==line.count) break;
        Prefix prefix{}; quoted=false;
        // Decode only enough logical argument characters to recognize the
        // reserved prefix. No copied argv, CommandLineToArgvW, or allocation.
        while(at<line.count && (quoted || !space(line[at]))) {
            if(line[at]==L'\\') {
                u32 slashes{};
                while(at<line.count && line[at]==L'\\') {++slashes;++at;}
                const bool quote=at<line.count && line[at]==L'"';
                const u32 literal=quote?slashes/2:slashes;
                for(u32 i=0;i<literal;++i) if(prefix.character(L'\\')) return Kind::update_child;
                if(quote) {
                    ++at;
                    if(slashes&1) {if(prefix.character(L'"')) return Kind::update_child;}
                    else quoted=!quoted;
                }
            } else if(line[at]==L'"') {quoted=!quoted;++at;}
            else if(prefix.character(line[at++])) return Kind::update_child;
        }
        if(quoted) return Kind::invalid;
    }
    return Kind::ordinary;
}
ApplicationUpdateStartupDecision application_update_startup_gate(ConstSlice<wchar_t> line) noexcept {
    const auto kind=application_update_startup_classify(line);
    if(kind==ApplicationUpdateStartupCommandKind::ordinary)
        return {ApplicationUpdateStartupMode::ordinary,ApplicationUpdateStartupReason::none,0};
    return {ApplicationUpdateStartupMode::refused,
        kind==ApplicationUpdateStartupCommandKind::update_child?ApplicationUpdateStartupReason::unenrolled_child:
            ApplicationUpdateStartupReason::invalid_command,
        kApplicationUpdateStartupRefusalExitCode};
}
ApplicationUpdateStartupDecision application_update_startup_run(ConstSlice<wchar_t> line, const UpdateServiceConfig& config) noexcept {
    using Mode = ApplicationUpdateStartupMode;
    const auto kind = application_update_startup_classify(line);
    if (kind == ApplicationUpdateStartupCommandKind::ordinary) {
        // Generations are deleted after later updates; registrations made by a
        // handed-off child must name this stable dispatcher instead.
        wchar_t module[kUpdatePathCapacity]{};
        const DWORD n = GetModuleFileNameW(nullptr, module, kUpdatePathCapacity);
        if (n && n < kUpdatePathCapacity && !application_update_startup_generation_module({module, n}))
            SetEnvironmentVariableW(kApplicationUpdateDispatcherVariable, module);
    }
    if (!config.configured || kind == ApplicationUpdateStartupCommandKind::invalid) return application_update_startup_gate(line);
    UpdatePaths paths{};
    const bool located = update_paths_default(&paths);
    if (kind == ApplicationUpdateStartupCommandKind::update_child) {
        wchar_t module[kUpdatePathCapacity]{};
        const DWORD n = GetModuleFileNameW(nullptr, module, kUpdatePathCapacity);
        const auto opened = located && n && n < kUpdatePathCapacity
            ? update_service_child_open(config, paths, {module, n}, line, &g_child) : UpdateServiceError::storage;
        // The application log opens later; record the handoff outcome now.
        if (g_update_log_session) g_update_log_session(true);
        UpdateLog("child_open").field("error", static_cast<u8>(opened)).write(opened == UpdateServiceError::none ? LogLevel::info : LogLevel::error);
        if (g_update_log_session) g_update_log_session(false);
        if (opened != UpdateServiceError::none) {
            update_handoff_child_release(&g_child);
            return application_update_startup_gate(line);
        }
        g_child_open = true;
        return {Mode::child, ApplicationUpdateStartupReason::none, 0};
    }
    // Nothing to dispatch before the first update is installed.
    if (!located || GetFileAttributesW(paths.journal) == INVALID_FILE_ATTRIBUTES)
        return {Mode::ordinary, ApplicationUpdateStartupReason::none, 0};
    auto* service = static_cast<UpdateService*>(VirtualAlloc(nullptr, sizeof(UpdateService), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    bool dispatched = false;
    if (service) {
        if (g_update_log_session) g_update_log_session(true);
        if (update_service_initialize(service, config, paths) == UpdateServiceError::none) dispatched = update_service_dispatch(service);
        if (g_update_log_session) g_update_log_session(false);
        update_service_release(service);
        VirtualFree(service, 0, MEM_RELEASE);
    }
    return {dispatched ? Mode::dispatched : Mode::ordinary, ApplicationUpdateStartupReason::none, 0};
}
UpdateHandoffChild* application_update_startup_child() noexcept { return g_child_open ? &g_child : nullptr; }
} // namespace maicord::app_detail
