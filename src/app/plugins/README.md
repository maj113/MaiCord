# Native plugin loader

`loader.cpp` loads native plugins. The `_WIN32` half is the PE loader; the `#else` half is the Linux ELF loader: freestanding shared
objects. The descriptor rules (`maicord_plugin_v6`) are shared.

## Linux loader

- `plugin_directory_open` pins every ancestor with `O_PATH|O_DIRECTORY|O_NOFOLLOW`: owned by root or the user, not writable by
  others unless sticky; the plugins directory the user's alone.
- `plugin_image_load` opens the leaf relative to the pin with `O_NOFOLLOW` (a regular file of the user that nobody else can write,
  whose `/proc/self/fd` path is the pinned path and name), copies it into a sealed `memfd` while it hashes it, checks the ELF on
  that copy and `dlopen`s `/proc/self/fd/N` of the copy with `RTLD_NOW|RTLD_LOCAL`: what was hashed and checked is what runs. The
  memfd takes no `MFD_EXEC`/`MFD_NOEXEC_SEAL` (Linux 6.3's flags); a system with `vm.memfd_noexec=2` refuses plugins.
- The copy stays open while the plugin is loaded (`PluginImage::sealed_token`): glibc returns an already loaded module for a
  repeated name.
- `elf_check` requires: x86-64 `ET_DYN`, no entry, `PT_INTERP` or `PT_TLS`; readable and never W+X loads; a non-executable
  `PT_GNU_STACK`; no `DT_NEEDED`, initializer, finalizer or text relocation; no undefined, TLS or IFUNC symbol; one exported symbol,
  `maicord_plugin_v6`, as data; only `R_X86_64_RELATIVE` relocations.
- `MC_CALL` is empty (System V) and `MC_EXPORT` a default-visibility attribute off Windows. `tools/maicord-plugin build|check|dev|pack
  --target linux` builds and checks a `.so` from the same `plugin.cpp` (`_check_elf` mirrors the loader). A catalog plugin version's
  Linux file is the source member `linux` of `plugin_index.py` (reader 4 lists it as the version's `file`, `files/<sha256>.so`; a
  version without one is not in `index-4.json`).

## Tests

`maicord_plugin_loader_elf` is the policy test (a fixture per defect, symlinks, modes, a renamed pin). `maicord_plugin_abi` checks the
samples' image shape (read from the file's section headers), descriptors and entry points. `maicord_plugin_binding|network|pipeline|
ui|surfaces` and `maicord_command_ui` run the Windows runtime tests over `.so` builds (`cmake/linux_plugin_tests.cmake`;
`maicord_plugin()` in `cmake/test_support.cmake` is the one build recipe; the built plugins are in `plugins/` of the build directory).
`maicord_plugin_compile_*` check the SDK's diagnostics.

## Catalog and effects

The official catalog (MaiCord-plugins) lists each effect's interface 4 package for the Linux client in `index-4.json`
(`kCatalogIndexFile` in `catalog_index.hpp`; `index-3.json` on Windows keeps the DXBC packages; `tools/release/plugin_index.py`:
reader level 4, the effect item's source member `universal`, `check_source`, `check_universal`). `maicord_catalog_effects` takes a
signed catalog of `tests/gpu/effects` packages through verify, list, download, parse, install and removal; with
`MAICORD_CATALOG_E2E_DIR` and `MAICORD_CATALOG_E2E_KEY` set it takes the effects of a site the catalog scripts built.
`tools/test_catalog_wayland.sh` runs the whole client against such a site over real TLS (see CONTRIBUTING.md, "Linux QA").
`application_catalog_browse` restores the catalog itself when the account session has not; the Themes page is signed-in only
(`page_allowed`).
