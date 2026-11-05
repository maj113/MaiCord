// The compiler helpers (memcpy, memmove, memset, memcmp) that the plain C plugin fixtures need on Linux, where no
// maicord_runtime object can be linked into a shared object: plugin.hpp defines them under MC_RUNTIME, which
// cmake/test_support.cmake (maicord_plugin) sets for C++ sources, and -fvisibility=hidden keeps them out of the exports.
#include "maicord/sdk/plugin.hpp"
