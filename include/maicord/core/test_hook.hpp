#pragma once

// Environment switches that exist for the tests and nothing else: a library name that does not exist (a machine without
// libpulse), a driver without an extension (GL_EXT_disjoint_timer_query), a compositor without fractional scale. A
// shipped client has none of them: test_hook() is a constant null there and the branch it guards compiles away. Only a
// build with tests (cmake -DMAICORD_BUILD_TESTS=ON, tools/build_linux.sh --test) defines MAICORD_TEST_HOOKS and reads
// the process's environment; such a build also carries kTestHooksMarker, which `tools/audit_elf.py --no-test-hooks`
// (tools/build_linux.sh without --test, packaging/build_tarball.py) and tools/audit_pe.ps1 -ForbidTestHooks reject, so a
// shipped image can never hold a hook. Overrides that a user or an operator may want (MAICORD_LIBRARY_PATH,
// MAICORD_VAAPI=off, MAICORD_SECRET_SERVICE, ...) are not hooks: they call environment_get() and are documented in
// CONTRIBUTING.md.
#if defined(MAICORD_TEST_HOOKS)

#if defined(_WIN32)
#error "test hooks are a Linux test facility: the Windows build has none"
#endif

#include "maicord/sys/linux/process.hpp"

namespace maicord {

// The value of the environment variable `name` (a MAICORD_ switch), or null when it is unset. The marker string stays in
// every image that holds a hook: the asm names it as an input, so no optimizer or linker pass drops it.
inline const char* test_hook(const char* name) noexcept {
    static const char kTestHooksMarker[] = "maicord-test-hooks-compiled-in";
    asm volatile("" : : "r"(kTestHooksMarker));
    return environment_get(name);
}

} // namespace maicord

#else

namespace maicord {

constexpr const char* test_hook(const char*) noexcept {
    return nullptr;
}

} // namespace maicord

#endif
