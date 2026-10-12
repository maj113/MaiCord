#include "maicord/core/base.hpp"
namespace maicord {
void test_notification_pcm(void(*)(bool,const char*)noexcept) noexcept;
namespace {u32 failures;
void check(bool passed,const char* message) noexcept {
    if(passed)return;++failures;u32 n=0;while(message[n])++n;DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message,n,&written,nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),"\r\n",2,&written,nullptr);
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_notification_pcm_tests_entry() noexcept {
    maicord::test_notification_pcm(maicord::check);ExitProcess(maicord::failures?1:0);
}
