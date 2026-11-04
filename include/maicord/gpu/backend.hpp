#pragma once

// The GPU backend of this build, chosen at compile time: Direct3D 11 on Windows, OpenGL ES elsewhere. Each backend
// header declares the Gpu* handle types, GpuStatus and gpu_release; maicord/gpu/gpu.hpp adds the portable rest.
// Code that needs the raw API (the backends, the Windows tests) includes its backend header explicitly.

#if defined(_WIN32)
#include "maicord/gpu/d3d11/backend.hpp"
#else
#include "maicord/gpu/gles/backend.hpp"
#endif
