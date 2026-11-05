// Precompiled for MaiCord's application sources; see
// maicord_precompile_application_sources in cmake/client.cmake.
#pragma once

// state.hpp and account_ui_runtime.hpp (the account owner struct, used by about
// four of five sources) plus the Win32 declarations nearly every source needs: parsing them
// once costs less than parsing them in every source.
#include "state.hpp"
#include "../account/account_ui_runtime.hpp"
#include "maicord/core/win32_kernel.hpp"
