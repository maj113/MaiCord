# Packaging MaiCord for Linux

Everything here is prepared ahead of the Linux client: the formats are built and tested with a stand-in program
(`build_tarball.py --stub`, a shell script that answers `--version`) or with a test image from the Linux build, and take the
real `MaiCord` executable the moment `tools/build_linux.sh` produces one. The application id is `io.github.maj113.MaiCord`; it
is the Wayland `app_id`, the single-instance D-Bus name, the desktop entry's file name and the AppStream id.

| Path | What it is |
| --- | --- |
| `metainfo/io.github.maj113.MaiCord.metainfo.xml.in` | AppStream metadata template (`@RELEASES@` is filled per version) |
| `build_tarball.py` | the release tarball `maicord-<version>-linux-x86_64.tar.gz` and its `.sha256` |
| `tarball/install.sh`, `uninstall.sh` | per-user install under `~/.local`, copied into the tarball |
| `aur/PKGBUILD`, `.SRCINFO` | `maicord-bin` for the Arch User Repository |
| `nfpm/nfpm.yaml`, `build.sh` | `.deb` and `.rpm` |
| `flatpak/io.github.maj113.MaiCord.yml` | Flatpak manifest (freedesktop runtime 26.08) |
| `nix/flake.nix`, `package.nix`, `package-bin.nix` | Nix flake: source build with LLVM 23, and the tarball |

The desktop entry and the hicolor icons come from `tools/generate_desktop_entry.py` (`--icons`, which runs
`tools/generate_app_icon.py`); `build_tarball.py` runs it, so every format installs the same files. Validate them with
`desktop-file-validate` and `appstreamcli validate --no-net`.

## The tarball

```
python3 packaging/build_tarball.py --binary build/MaiCord --version 0.6.6 --output-dir dist   # or --stub
```

```
maicord-<version>-linux-x86_64/
  MaiCord                 the program
  fonts/  licenses/       as next to MaiCord.exe (cmake/client.cmake lists them)
  share/applications/     io.github.maj113.MaiCord.desktop      (Exec=maicord %U)
  share/icons/hicolor/    16, 24, 32, 48, 64 and 256 pixel PNGs
  share/metainfo/         io.github.maj113.MaiCord.metainfo.xml (with this version's <release>)
  .maicord-portable       the marker of the self-updating channel
  install.sh  uninstall.sh  README.md  LICENSE
```

The archive is reproducible: the same inputs give the same bytes (sorted entries, root ownership, `SOURCE_DATE_EPOCH` or the
commit date, no gzip timestamp). The release version is the project version of `CMakeLists.txt` unless `--version` says
otherwise (the release job passes the tag's).

`./install.sh` installs for the current user: the program, fonts and licenses to `~/.local/opt/MaiCord` (owned by the user,
because this install updates itself), a link `~/.local/bin/maicord`, and the desktop entry (with the program's full path in
`Exec`), icons and metadata under `~/.local/share`. It makes MaiCord the handler of `discord://` and `maicord://` links
(`xdg-mime default`) unless `--no-url-handler` is given; `--prefix DIR` installs under `DIR` instead of `~/.local`. The
program's data (`$XDG_DATA_HOME/maicord`, state and cache) is a different directory and is never touched by install or
uninstall. `~/.local/opt/MaiCord/uninstall.sh` removes everything it installed and the default-handler lines it added
(`--purge` also removes the data).

### The self-updating channel marker

`.maicord-portable` is a file next to the executable whose first line is `maicord-portable 1` (later lines may carry
`key=value` fields, for example a channel; unknown lines are ignored). **Only the tarball install has it.** The AUR, Nix,
Flatpak, `.deb` and `.rpm` packages leave it out: a package manager owns those files, and a program that replaced itself
under it would break the package's verification and the next upgrade.

The updater's Linux mode enables itself only when the marker exists in the directory of the real executable
(`readlink /proc/self/exe`, not the `~/.local/bin` link), it is a regular file with the first line above, and that
directory is writable (`update_portable_install`, `src/app/update/linux/portable.cpp`). The place is
`application_update_build_config()` in `src/app/update/entry.cpp`: it returns the `UpdateServiceConfig`, and a config with
`configured == false` is what makes `ApplicationUpdateState::disabled` (src/app/update/updates.cpp, `update_service_init`'s
`config.configured ? idle : disabled`). On Linux `update_portable_gate` clears `configured` when the marker check fails.
It also clears it in a build that is not told that the release channels carry the tarball: the CMake option
`MAICORD_LINUX_UPDATES_PUBLISHED` (default: the `MAICORD_LINUX_RELEASES` environment variable is `true`), which
`release.yml`'s `linux` job sets from the repository variable of that name, so only the released tarball's program can
update itself. The Windows build never looks for any of this.

### How the tarball install updates itself

The release channels carry the tarball. `maicord_release.py linux` signs, with the Windows manifests' key, a stable and a
preview manifest for it (`arch` `linux-x64`, the artifact `maicord-<X.Y.Z>-linux-x86_64.tar.gz` of the version, which is
exactly this tarball) into `updates/p2/linux/` of the Pages site; the Windows client reads other files, and a manifest
of the other system fails its reader on the signed architecture and artifact name. The preview feed
(`updates/p2/linux/preview/`) carries master builds: the `linux` job of `ci.yml` packages one tarball per master commit
(`build_tarball.py --version X.Y.Z-dev.N`), and a stable release moves it to the release. The check, the
signature and freshness, the durable rollback floor and the download are the Windows updater's own (`update_service_check`).
Staging is the Linux package reader (`src/app/update/linux/package.cpp`): the package is read whole while its SHA-256 is
taken and compared with the signed one, gunzipped (`gzip_inflate` checks CRC-32 and size), and its tar walked in the
producer's layout only: the program (an x86-64 ELF executable, checked structurally), and the flat `fonts/` and `licenses/`
files go into `MaiCord.update/` beside the install; everything else in the tarball (desktop entry, icons, scripts, the
marker) is left alone. A failure leaves nothing. At exit after *Restart to update* the swap (`update_swap`,
`src/app/update/linux/stage.cpp`) moves the live `fonts/`, `licenses/` and `MaiCord` into `MaiCord.old/` and the staged
ones in, the program first, undoing every move when one fails; renaming a running image is allowed. The process then
replaces itself (`execve` of the real executable path), and the next start deletes `MaiCord.old/`.

## Distribution packages

All of them install the tarball's contents (or build the same tree) and leave the marker out.

**AUR (`aur/`).** `maicord-bin`: `/usr/lib/maicord` holds the program, fonts and licenses, `/usr/bin/maicord` links to it.
`depends` are what the program cannot start without (glibc, openssl, wayland, libxkbcommon, libglvnd, hicolor-icon-theme);
`optdepends` are the libraries it opens with `dlopen` when present (libpulse, fontconfig, hunspell and a dictionary, enchant,
fonts, portals, a Secret Service, speech-dispatcher for "Speak message", libfido2 for security keys) and the Mesa drivers. Per release: bump `pkgver`, `updpkgsums`,
`makepkg --printsrcinfo > .SRCINFO`, push to the AUR git repository of `maicord-bin`. Tested in an `archlinux` container
with `makepkg`, `namcap` and `pacman -U`.

**`.deb` and `.rpm` (`nfpm/`).** `nfpm/build.sh <tarball> <output-directory>` runs [nfpm](https://nfpm.goreleaser.com/) in
the unpacked tarball. Hard dependencies are the C library, OpenSSL, the Wayland client libraries, xkbcommon, EGL and GLES;
the `dlopen`ed extras are recommended or suggested. Tested by installing the `.deb` in `ubuntu:24.04` (apt resolved the
dependencies) and the `.rpm` in `fedora:latest`, and removing them again.

**Flatpak (`flatpak/`).** Runtime `org.freedesktop.Platform` 26.08, the newest on Flathub today. The runtime already
provides every library the program opens (OpenSSL, fontconfig, Hunspell with its dictionaries, libpulse, the Wayland client
libraries, xkbcommon, EGL/GLES); the GPU drivers come from its GL extension. Permissions: `--socket=wayland`, `--device=dri`,
`--socket=pulseaudio`, `--share=network`, `--talk-name=org.kde.StatusNotifierWatcher` and `--own-name=org.kde.*` (the tray
item's own name), `--talk-name=org.freedesktop.secrets` and `org.freedesktop.Notifications`. No file system access: the
file chooser, link opening, notifications, settings and global shortcuts go through portals, which every sandbox may call.

```
flatpak-builder --user --install --force-clean build-dir packaging/flatpak/io.github.maj113.MaiCord.yml
flatpak run io.github.maj113.MaiCord
```

The manifest takes the release tarball by URL and checksum (the owner fills both per version). Tested in WSL with
`flatpak-builder` against a tarball of the stub: built, exported, bundled with `flatpak build-bundle`, installed and run
inside its sandbox (`--version` answered), the exported desktop entry checked.

**Nix (`nix/`).** `nix build 'github:maj113/MaiCord?dir=packaging/nix'` is the source build, `#maicord-bin` the tarball.
nixpkgs has `llvmPackages_23` (23.1.3 at the time of writing), which the source build uses unwrapped (the project's own
`-nostdinc` contract, no nixpkgs hardening flags: `hardeningDisable = [ "all" ]`). The shader toolchain that
`tools/fetch_linux_shader_toolchain.py` would download is made offline from the pins of `tools/linux_shader_toolchain.json`
(DXC's tarball, SPIRV-Cross at its commit) and handed to that script as its cache. `autoPatchelfHook` gives the image the Nix
loader and glibc; `runtimeDependencies` (the library outputs of openssl, wayland, libxkbcommon, libglvnd, fontconfig,
libpulseaudio, hunspell and enchant) and `appendRunpaths = [ "/run/opengl-driver/lib" ]` are its run path, which is how the
`dlopen`s are found. `buildTarget` is the CMake target that becomes the program (`MaiCord` once it exists); until then
`nix build .#build-check` builds the shader pack test image through the same pipeline. Tested on NixOS (WSL) with Nix 2.34:
the `-bin` package from a tarball of a real Linux test image (it runs; `patchelf` shows the interpreter and a run path in which
every `dlopen`ed library resolves) and the source build's `build-check`.

## Releases

`.github/workflows/release.yml` has a `linux` job that builds with the same `tools/build_linux.sh` as CI, packages the
tarball with `build_tarball.py` at the tag's version, attests its build provenance and attaches the tarball and its
`.sha256` to the draft release. It runs only when the repository variable `MAICORD_LINUX_RELEASES` is `true` (otherwise the
Windows release is exactly what it was); when it runs, the release is published only if it succeeded, and a failure deletes
the draft like a failed analysis does. The tarball is not on the self-update channels yet: those manifests are the Windows
client's.

## What the owner still has to do

* **Screenshots.** `metainfo/…metainfo.xml.in` has a placeholder entry and a `TODO(owner)` comment. AppStream distributors
  and Flathub need real ones (16:9, at least 1248x702, redacted), at a tag-pinned URL. Review the OARS content rating too.
* **Flathub.** The manifest here is fit for a self-hosted repository or a `.flatpak` bundle attached to releases, not for
  Flathub as it stands, for three reasons from [Flathub's requirements](https://docs.flathub.org/docs/for-app-authors/requirements):
  (1) a source-available application must be built entirely from source with no network during the build, so the manifest
  would have to build MaiCord with LLVM 23 (no freedesktop SDK extension has it: the extension would be a module that builds
  LLVM), DXC and SPIRV-Cross from pinned sources, or ship a pre-generated shader pack; exceptions are for well-known vendors;
  (2) Flathub manifests must not contain AI-generated or AI-assisted content, and AI tools must not open the submission pull
  request: the Flathub manifest has to be written and submitted by a person (this file can be read as a list of finish-args
  and paths, not copied); (3) the application needs a verifiable `io.github.maj113` id (the GitHub account) and real
  screenshots. If the project later ships its own Flatpak repository, a Flathub submission of the same application by someone
  else would be refused ("conflicting submissions").
* **nixpkgs.** A pull request adding `pkgs/by-name/ma/maicord/package.nix`, built from `fetchFromGitHub` and, as nixpkgs
  expects, from nixpkgs' own `directx-shader-compiler` and `spirv-cross` instead of the pinned tarball (which may change the
  generated GLSL slightly; run the GPU golden tests). `nix/package.nix` is the starting point; `nix/package-bin.nix` is not
  acceptable there (binary releases are not packaged from the tarball in nixpkgs proper).
* **AUR.** An account and the first push of `maicord-bin`; after that, the per-release steps above.
* **Release.** Set `MAICORD_LINUX_RELEASES=true` once the client builds on Linux; fill the checksum placeholders of the AUR,
  Flatpak and Nix files per version.
