#include "../src/app/application_update_startup.hpp"

namespace maicord {
namespace {
using namespace app_detail;
template<u32 N> ConstSlice<wchar_t> command(const wchar_t (&s)[N]) noexcept {return {s,N-1};}
bool ordinary(ConstSlice<wchar_t> s) noexcept {
    const auto decision=application_update_startup_gate(s);
    return application_update_startup_classify(s)==ApplicationUpdateStartupCommandKind::ordinary &&
        decision.mode==ApplicationUpdateStartupMode::ordinary && decision.reason==ApplicationUpdateStartupReason::none && !decision.exit_code;
}
bool child(ConstSlice<wchar_t> s) noexcept {
    const auto decision=application_update_startup_gate(s);
    return application_update_startup_classify(s)==ApplicationUpdateStartupCommandKind::update_child &&
        decision.mode==ApplicationUpdateStartupMode::refused && decision.reason==ApplicationUpdateStartupReason::unenrolled_child &&
        decision.exit_code==kApplicationUpdateStartupRefusalExitCode;
}
bool invalid(ConstSlice<wchar_t> s) noexcept {
    const auto decision=application_update_startup_gate(s);
    return application_update_startup_classify(s)==ApplicationUpdateStartupCommandKind::invalid &&
        decision.mode==ApplicationUpdateStartupMode::refused && decision.reason==ApplicationUpdateStartupReason::invalid_command &&
        decision.exit_code==kApplicationUpdateStartupRefusalExitCode;
}
// QA-only static storage: no large stack frame or dynamic argv allocation.
wchar_t long_command[kApplicationUpdateStartupCommandCharacters];
}
void test_application_update_startup(void(*check)(bool,const char*) noexcept) noexcept {
    check(ordinary(command(L"\"D:\\MaiCord\\MaiCord.exe\"")) &&
        ordinary(command(L"MaiCord.exe /Embedding")) &&
        ordinary(command(L"\"C:\\Program Files\\MaiCord.exe\" --reset \"https://example.invalid/recovery?token=test\"")),
        "update startup: ordinary process and protocol-link arguments retain normal startup");
    check(ordinary(command(L"\"D:\\--maicord-update-child-v1\\MaiCord.exe\"")) &&
        ordinary(command(L"--maicord-update-child-v1.exe /Embedding")) &&
        ordinary(command(L"\"C:\\Program Files\"\\MaiCord.exe --normal")),
        "update startup: program-name quoting excludes updater text inside argv0");
    check(child(command(L"\"MaiCord.exe\" --maicord-update-child-v1")) &&
        child(command(L"MaiCord.exe --maicord-update-child-v1 malformed 0 0 0")),
        "update startup: canonical child name refuses missing or malformed untrusted payload before normal startup");
    check(child(command(L"MaiCord.exe --maicord-update-child-v2")) &&
        child(command(L"MaiCord.exe --maicord-update-child")) &&
        child(command(L"MaiCord.exe --maicord-update-child-v1suffix")),
        "update startup: unknown versions and malformed reserved child-family suffixes cannot fall through");
    check(child(command(L"MaiCord.exe  --maicord-update-child-v1")) &&
        child(command(L"MaiCord.exe\t--maicord-update-child-v1")) &&
        child(command(L"MaiCord.exe\r\n--maicord-update-child-v1")) &&
        child(command(L"MaiCord.exe --MAICORD-UPDATE-CHILD-V1")),
        "update startup: whitespace and ASCII case variants of reserved child mode refuse");
    check(child(command(L"MaiCord.exe \"--maicord-update-child-v1\"")) &&
        child(command(L"MaiCord.exe --maicord-update-\"\"child-v1")) &&
        child(command(L"MaiCord.exe --normal \"value\" --maicord-update-child-v1 --maicord-update-child-v1")),
        "update startup: quoted fragmented later and duplicate child arguments refuse without importing handles");
    check(child(command(L"MaiCord.exe \"--maicord-update-child-v1")) &&
        ordinary(command(L"MaiCord.exe \"value --maicord-update-child-v1\"")) &&
        ordinary(command(L"MaiCord.exe https://example.invalid/--maicord-update-child-v1")) &&
        child(command(L"MaiCord.exe --maicord-update-childish")),
        "update startup: child prefix reserves its namespace while embedded ordinary values stay ordinary");
    check(ordinary(command(LR"(MaiCord.exe "a\"quoted value" --normal)")) &&
        ordinary(command(LR"(MaiCord.exe \"--maicord-update-child-v1\")")) &&
        child(command(LR"(MaiCord.exe "a\"quoted value" --maicord-update-child-v1)")) &&
        child(command(L"MaiCord.exe \"\" --maicord-update-child-v1")),
        "update startup: backslash quote handling and empty arguments cannot bypass a following child token");
    check(invalid({}) && invalid({L"",0}) && invalid(command(L"   \t")) &&
        invalid(command(L"\"\" --normal")) && invalid(command(L"\"MaiCord.exe --normal")) &&
        invalid(command(L"MaiCord.exe \"unterminated")) && invalid({L"x",~u32{0}}),
        "update startup: invalid or impossible command snapshots refuse with bounded reads");
    constexpr wchar_t hidden[]{L'M',L'.',L'e',L'x',L'e',L' ',0,L'-',L'-'};
    check(invalid({hidden,sizeof(hidden)/sizeof(wchar_t)}),"update startup: an embedded NUL cannot hide the rest of a declared snapshot");
    constexpr wchar_t head[]=L"MaiCord.exe --reset https://example.invalid/";
    u32 at{};
    for(;at<sizeof(head)/sizeof(wchar_t)-1;++at) long_command[at]=head[at];
    for(;at<kApplicationUpdateStartupCommandCharacters-1;++at) long_command[at]=L'a';
    long_command[at]=0;
    auto bounded=application_update_startup_command(long_command);
    check(bounded.count==kApplicationUpdateStartupCommandCharacters-1 && ordinary(bounded) && bounded.count>1024,
        "update startup: long ordinary protocol links use the Win32 bound rather than child payload limit");
    constexpr wchar_t flag[]=L" --maicord-update-child-v1";
    at=kApplicationUpdateStartupCommandCharacters-1-(sizeof(flag)/sizeof(wchar_t)-1);
    for(u32 i=0;i<sizeof(flag)/sizeof(wchar_t)-1;++i) long_command[at+i]=flag[i];
    check(child(application_update_startup_command(long_command)),
        "update startup: reserved child mode late in a long command cannot hide behind the 1024-unit payload bound");
    long_command[kApplicationUpdateStartupCommandCharacters-1]=L'x';
    bounded=application_update_startup_command(long_command);
    check(bounded.count==kApplicationUpdateStartupCommandCharacters && invalid(bounded) &&
        invalid(application_update_startup_command(nullptr)) && invalid(application_update_startup_command(L"")),
        "update startup: entry adapter refuses null empty and unterminated bounded input without any OS startup work");
    constexpr wchar_t stable[]=L"D:\\Apps\\MaiCord\\MaiCord.exe";
    constexpr wchar_t generation[]=L"C:\\Users\\a\\AppData\\Local\\MaiCord\\updates\\Generations\\g-"
        L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\\MaiCord.exe";
    constexpr wchar_t upper_hex[]=L"C:\\updates\\generations\\g-"
        L"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\\MaiCord.exe";
    constexpr wchar_t short_hex[]=L"C:\\updates\\generations\\g-0123\\MaiCord.exe";
    check(!application_update_startup_generation_module(command(stable)) &&
        application_update_startup_generation_module(command(generation)) &&
        !application_update_startup_generation_module(command(upper_hex)) &&
        !application_update_startup_generation_module(command(short_hex)) &&
        !application_update_startup_generation_module({}),
        "update startup: only <generations>\\g-<64 hex> folders count as disposable generations");
    const auto own=application_update_startup_registration_path(command(stable),command(generation));
    const auto handed=application_update_startup_registration_path(command(generation),command(stable));
    check(own.data==stable && own.count==sizeof(stable)/sizeof(wchar_t)-1 &&
        handed.data==stable && handed.count==own.count,
        "update startup: shortcut registration keeps the dispatcher path and a generation uses the inherited one");
    check(!application_update_startup_registration_path(command(generation),{}).count &&
        !application_update_startup_registration_path(command(generation),command(generation)).count &&
        !application_update_startup_registration_path(command(generation),command(L"MaiCord.exe")).count &&
        !application_update_startup_registration_path({},command(stable)).count,
        "update startup: a generation never registers itself, another generation or a relative dispatcher");
}
} // namespace maicord
