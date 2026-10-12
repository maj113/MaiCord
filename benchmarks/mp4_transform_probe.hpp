#pragma once
#include "maicord/core/base.hpp"
struct IMFTransform;
namespace maicord {
// Diagnostic-only attribute snapshots, never part of a timing run.
void mp4_inspect_transform(IMFTransform* transform,
    void (*metric)(const char*, u64)) noexcept;
}
