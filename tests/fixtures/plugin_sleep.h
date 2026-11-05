/* Sleep(milliseconds) for the fixtures that must outlast a call budget: the Win32 import on Windows, and on Linux
   nanosleep(2) as a bare system call, so that the shared object imports nothing. */
#ifndef MAICORD_PLUGIN_FIXTURE_SLEEP_H
#define MAICORD_PLUGIN_FIXTURE_SLEEP_H

#if defined(_WIN32)
__declspec(dllimport) void __stdcall Sleep(unsigned long milliseconds);
#else
static void __attribute__((unused)) Sleep(unsigned long milliseconds) {
    long request[2] = {(long)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000L};
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "0"(35L), "D"(request), "S"(0L) : "rcx", "r11", "memory");
}
#endif

#endif
