#include "maicord/core/base.hpp"
namespace maicord {
void test_native_notification_audio_faults(void(*)(bool,const char*) noexcept) noexcept;
namespace {
u32 failures;
void check(bool passed,const char* message) noexcept {
    if(passed)return;++failures;u32 count{};while(message[count])++count;DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message,count,&written,nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),"\r\n",2,&written,nullptr);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_notification_audio_faults_tests_entry() noexcept {
    maicord::test_native_notification_audio_faults(maicord::check);ExitProcess(maicord::failures?1:0);
}
