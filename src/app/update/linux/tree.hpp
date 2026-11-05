#pragma once

#include "maicord/core/base.hpp"

// The updater's on-disk helpers on Linux (stage.cpp). The stage and the replaced install are directories there, where
// Windows has one executable file each, so what Windows removes with file_remove is a tree here.
namespace maicord {

// Removes the file at `path`, or the directory and everything below it; a link is removed, never followed. True when
// nothing is left at `path` (a missing path included). Directories nest no deeper than a stage does (the install
// directory's children, two levels): anything deeper fails.
bool update_tree_remove(const wchar_t* path) noexcept;

} // namespace maicord
