// Precompiled for MaiCord's application sources; see
// maicord_precompile_application_sources in cmake/client.cmake.
#pragma once

// application_state.hpp and account_ui_runtime.hpp (the account owner struct, used by about
// four of five sources) plus the Win32 declarations nearly every source needs: parsing them
// once costs less than parsing them in every source.
#include "application_state.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
