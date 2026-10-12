/* MaiCord's freestanding Opus invariant failure boundary. */
#include <intrin.h>
__declspec(noreturn) void celt_fatal(const char* message, const char* file, int line) {
    (void)message;
    (void)file;
    (void)line;
    __fastfail(7); /* FAST_FAIL_FATAL_APP_EXIT; no CRT abort or console formatting. */
}
