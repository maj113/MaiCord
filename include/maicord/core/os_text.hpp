#pragma once


// User-visible text that names the operating system, one wording per system: MAICORD_OS_TEXT("Windows ...", "Your
// desktop ...") is the first literal in the Windows build and the second in the Linux build, as a Utf8View, and the
// other literal is not compiled in. Windows keeps its words exactly; Linux says "your desktop" or "the system" where
// Windows names itself, and keeps the sentence as short.
#if defined(_WIN32)
#define MAICORD_OS_TEXT(windows_text, other_text) ::maicord::utf8_literal(windows_text)
#else
#define MAICORD_OS_TEXT(windows_text, other_text) ::maicord::utf8_literal(other_text)
#endif
