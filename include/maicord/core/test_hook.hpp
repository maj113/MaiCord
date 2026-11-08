#pragma once

// Environment switches that exist for the tests and nothing else: a library name that does not exist (a machine without
// libpulse), a driver without an extension (GL_EXT_disjoint_timer_query), a compositor without fractional scale, the
// Windows client pointed at a fake Discord (platform/net/test_server.hpp). A shipped client has none of them: test_hook()
// is a constant null there and the branch it guards compiles away (the Windows hooks, which have wide names and
// parameters, are not declared at all: their code sits under #if defined(MAICORD_TEST_HOOKS)). Only a build with tests (cmake
// -DMAICORD_BUILD_TESTS=ON, tools/build_linux.sh --test; Windows: tools/build.ps1 -TestHooks, which builds the client alone)
// defines MAICORD_TEST_HOOKS and reads the process's environment; such a build also carries kTestHooksMarker, which
// `tools/audit_image.py --no-test-hooks` (the build's audit of every shipping client, packaging/build_tarball.py)
// rejects, so a shipped image can never hold a hook. Overrides that a user or an
// operator may want (MAICORD_LIBRARY_PATH, MAICORD_VAAPI=off, MAICORD_SECRET_SERVICE, ...) are not hooks: they call
// environment_get() and are documented in CONTRIBUTING.md.
#if defined(MAICORD_TEST_HOOKS)

#if defined(_WIN32)

#include "maicord/core/data_path.hpp"

namespace maicord {

// The value of the environment variable `name` (a MAICORD_ switch) written to `output`: its length, 0 when it is unset or
// empty, and at least `capacity` when it does not fit (environment_variable's convention). A volatile read of the marker
// keeps it in every image that holds a hook.
inline u32 test_hook(const wchar_t* name, wchar_t* output, u32 capacity) noexcept {
    static const char kTestHooksMarker[] = "maicord-test-hooks-compiled-in";
    (void)*static_cast<const volatile char*>(kTestHooksMarker);
    return environment_variable(name, output, capacity);
}

} // namespace maicord

#else

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

#endif

#else

namespace maicord {

constexpr const char* test_hook(const char*) noexcept {
    return nullptr;
}

} // namespace maicord

#endif
