# Packaging MaiCord for Linux

The application id is `io.github.maj113.MaiCord`; it is the Wayland `app_id`, the single-instance D-Bus name, the desktop
entry's file name and the AppStream id. Every format installs the same files, which `build_tarball.py` stages: the program,
`fonts/` and `licenses/` next to it, the desktop entry and hicolor icons (`tools/generate_desktop_entry.py --icons`) and the
AppStream metadata. Everything that ships is built **without `--test`** (a test build carries test-only switches;
`build_tarball.py` runs `tools/audit_elf.py --no-test-hooks` on the program and refuses one that has them).

## Installing (the text for the project README)

| You | Do |
| --- | --- |
| any distribution, one command | `curl -fsSL https://github.com/maj113/MaiCord/releases/latest/download/install.sh \| sh` (installs under `~/.local`, no root; verifies a signature and a SHA-256 first; updates itself) |
| any distribution, one file | download `MaiCord-<version>-x86_64.AppImage` from the [latest release](https://github.com/maj113/MaiCord/releases/latest), `chmod +x`, run it; `./MaiCord-….AppImage --integrate` adds the menu entry and the `discord://` handler |
| Debian, Ubuntu, Mint | `sudo curl -fsSLo /etc/apt/sources.list.d/maicord.sources https://maj113.github.io/MaiCord/repo/maicord.sources && sudo apt update && sudo apt install maicord` |
| Fedora, openSUSE, RHEL-likes | `sudo curl -fsSLo /etc/yum.repos.d/maicord.repo https://maj113.github.io/MaiCord/repo/maicord.repo && sudo dnf install maicord` (openSUSE: `zypper addrepo` the same file) |
| Arch | `yay -S maicord-bin` (or `maicord`, the source build) |
| NixOS, Nix | `nix run 'github:maj113/MaiCord?dir=packaging/nix'` |
| Alpine, Void (musl) | the `-musl` tarball or AppImage of the release; `alpine/APKBUILD` and `void/maicord-bin/template` build packages of it |

All of these need kernel 6.1 or newer, Wayland, and the system libraries listed under *What the program opens*. The installer,
the AppImage and the tarball need no package manager; the others are updated by it. The repository and AUR rows work once
the owner has done the steps in *Owner checklist* below.

## What is here

| Path | What it is |
| --- | --- |
| `build_tarball.py` | the release tarball `maicord-<version>-linux-x86_64[-musl].tar.gz` and its `.sha256`; `--libc musl` retargets the glibc image to musl's loader |
| `tarball/install.sh`, `uninstall.sh` | per-user install under `~/.local`, copied into the tarball |
| `online/install.sh.in`, `render_installer.py`, `test_installer.py` | the one-line installer (the release job renders it with the update key) and its test |
| `appimage/` | `build_appimage.py`, `AppRun`, `runtime.json` (the pinned type 2 runtime and the zsync update information) |
| `nfpm/` | `.deb` and `.rpm` from the tarball (`nfpm.yaml`, `build.sh`, `install_nfpm.sh` pins nfpm) |
| `repo/` | `build_repo.py` makes the signed apt and dnf/zypper repositories, `publish_site.sh` puts them on the Pages site |
| `debian/` | the `debian/` directory and `build_source.sh` for a Launchpad PPA (source package around the release tarball) |
| `rpm/` | `maicord.spec` for COPR and the openSUSE Build Service, `_service` (OBS download), `.copr/Makefile` |
| `aur/maicord-bin`, `aur/maicord` | the Arch User Repository packages: the tarball, and a source build with Arch's LLVM 23 |
| `alpine/APKBUILD`, `void/maicord-bin/template`, `gentoo/*.ebuild` | musl and source-distribution packages of the tarball (untested where a note says so) |
| `flatpak/`, `nix/` | the Flatpak manifest (not for Flathub, see below) and the Nix flake |
| `metainfo/` | the AppStream template (`@RELEASES@` is filled per version) |
| `check.sh`, `check_artifacts.sh` | the checks (see *Verification*) |

## The tarball and its self-update

```
maicord-<version>-linux-x86_64/
  MaiCord  fonts/  licenses/  share/{applications,icons/hicolor,metainfo}/  .maicord-portable  install.sh  uninstall.sh  README.md  LICENSE
```

The archive is reproducible (sorted entries, root ownership, `SOURCE_DATE_EPOCH` or the commit date, no gzip timestamp).
`./install.sh` installs for the current user: the program, fonts and licenses to `~/.local/opt/MaiCord` (user-owned, because
this install updates itself), a link `~/.local/bin/maicord`, the desktop entry (with the program's full path in `Exec`), icons
and metadata under `~/.local/share`, and makes MaiCord the handler of `discord://` and `maicord://` unless `--no-url-handler`;
`--prefix DIR` installs under `DIR`. The program's data (`$XDG_DATA_HOME/maicord`, state, cache) is never touched by install or
uninstall; `uninstall.sh --purge` removes it too.

**The self-updating channel marker.** `.maicord-portable` is a file next to the executable whose first line is
`maicord-portable 1`. Only the tarball install has it: the package managers' files are theirs, and an AppImage is a read-only
mount. The updater (`application_update_build_config()`, `src/app/update/entry.cpp`) enables itself only when the marker is in the
directory of the real executable (`readlink /proc/self/exe`), that directory is writable, the process is not musl and the build was
told the release channels carry the tarball (`MAICORD_LINUX_UPDATES_PUBLISHED`, from the `MAICORD_LINUX_RELEASES` variable at
configure time; `update_portable_gate`, `src/app/update/linux/portable.cpp`). `maicord_release.py linux` signs, with the Windows
manifests' key, a stable and a preview manifest of the glibc tarball (`arch` `linux-x64`) into `updates/p2/linux/`; the client
checks signature and freshness, downloads the tarball, verifies its SHA-256, stages `MaiCord`, `fonts/` and `licenses/` beside the
install and swaps them at exit (`src/app/update/linux/{package,stage}.cpp`), then `execve`s itself. The musl image does not update itself.

## The AppImage

`MaiCord-<version>-x86_64.AppImage` (and `…-x86_64-musl.AppImage` for musl systems) is the pinned
[type 2 runtime](https://github.com/AppImage/type2-runtime) (release `20251108`, SHA-256 in `appimage/runtime.json`) with a
SquashFS (zstd) of an AppDir appended: `AppRun`, `usr/bin/MaiCord` with `fonts/` and `licenses/` beside it (the program finds
them next to its own executable), the desktop entry, icons and metainfo. `build_appimage.py` assembles it itself, not through
`appimagetool` (whose AppImage needs FUSE to run, and which would be a second pinned download): it writes the update
information into the runtime's `.upd_info` section, the bytes `appimagetool` writes, and makes the `.zsync` with `zsyncmake`.

* **FUSE.** The runtime is static (musl) and carries libfuse3, so no `libfuse2` is needed (the old type 2 runtime needed it).
  It still calls `fusermount3` or `fusermount` (setuid) to mount; when that fails it prints "Cannot mount AppImage, please check
  your FUSE setup" and stops (runtime.c: no automatic fallback). `APPIMAGE_EXTRACT_AND_RUN=1` (or `--appimage-extract-and-run`)
  unpacks to `$TMPDIR/appimage_extracted_<md5>` and runs from there with no FUSE at all (containers, minimal systems); `--appimage-extract`
  unpacks to `squashfs-root/`. Both work with this image (`check.sh`). WSLg and the distribution containers run it either way.
* **Desktop integration.** An AppImage cannot register itself. `./MaiCord-….AppImage --integrate` (handled by `AppRun`, no
  program code) writes the user's desktop entry naming the AppImage file, the icons and the `discord://` and `maicord://` default
  under `~/.local/share` and `mimeapps.list`; `--integrate --no-url-handler` leaves the links alone and `--unintegrate`
  removes it all. Inside MaiCord, Settings > System > "Open discord:// links" and "Start with system" register `$APPIMAGE`
  (not the mount, which changes with every launch): `desktop_appimage_path` in `src/platform/desktop/linux/integration.cpp`
  (it counts only when the running program is inside `$APPDIR`, so a MaiCord started from another AppImage's shell is not
  mistaken). AppImageLauncher, Gear Lever and Dolphin integrate AppImages themselves from the same desktop entry.
* **Updates: AppImageUpdate, not the in-app updater.** The runtime carries
  `gh-releases-zsync|maj113|MaiCord|latest|MaiCord-*-x86_64.AppImage.zsync` (a development build carries none), so
  `appimageupdatetool`, Gear Lever, AppManager and AppImageLauncher update the file in place with a zsync delta from the release's
  `.zsync`. The in-app updater stays off for an AppImage (no marker beside the mount): replacing the file would need a second
  manifest flavor (`maicord_release.py`, the channel slots, the site layout) and a second package reader and swap in a client
  that is at its 6.5 MB limit (the Linux client was 3 KB under it when this was decided), for an update path the AppImage tools already provide. The cost: a zsync download is
  checked against GitHub's HTTPS and SHA-1 from the `.zsync`, not against MaiCord's own signing key as the tarball's updates
  are; the release's `SHA256SUMS` and build provenance attestation cover the file for anyone who checks.
* **Start-up.** The FUSE mount and `AppRun` (a shell script that `exec`s the program) add about 6 ms to a launch (a stand-in that exits
  at once: 1.3 ms directly, 7.5 ms through the AppImage, on a busy machine); nothing after. `APPIMAGE_EXTRACT_AND_RUN=1` also writes the
  whole image to `$TMPDIR` on every launch, which costs more (the program is 6.5 MB).

## The one-line installer

`online/install.sh.in` becomes `install.sh` on the release (`render_installer.py` writes the repository and the update public key
into it). It downloads `updates/p2/linux/maicord-stable.manifest.json` and its `.sig` from the Pages site, checks the ECDSA P-256
signature over `SHA-256("MaiCord update manifest v1\n" + manifest)` with `openssl` (r and s become a DER sequence through
`openssl asn1parse -genconf`; the key is a PEM in the script), checks `arch`, freshness and the file name, downloads
`maicord-<version>-linux-x86_64.tar.gz` from the release, checks size and SHA-256 against the signed manifest, unpacks it into a
temporary directory and runs the tarball's `install.sh`. Anything that does not verify stops before the tarball is unpacked.
The key is the one MaiCord's own updater trusts, so the trust is the same as an update's. `--channel preview` takes the master
build; `--site`/`--releases` point at a mirror. It needs `curl` or `wget`, `tar`, `sha256sum`, `od`, `awk`, `sed`, `cut` and the
`openssl` tool; without them it says so and stops. `test_installer.py` (run by `check.sh`) signs a stand-in release with a throwaway
key and checks the good install and each refusal: another key, an edited manifest, a tarball of the right size with other bytes, a short
tarball, an expired manifest, another system's manifest. A real CNG-signed manifest was verified by the same script.

Security perception: `curl | sh` is what many people distrust; this one is short, readable (`curl -fsSL URL | less` first), runs as the
user, writes only under the prefix, and verifies before it unpacks. Anyone who prefers can download `install.sh` or the tarball
and its `.sha256`, or use the AppImage or a package. Both routes are in the README text above.

## Distribution packages

All of them install the tarball's contents and leave the marker out; `depends` are what the program cannot start without (the C
library, OpenSSL, the Wayland client libraries, xkbcommon, EGL and GLESv2), everything it `dlopen`s is recommended or suggested.

* **`.deb` and `.rpm` (`nfpm/`).** `build.sh <tarball> <dir>` runs the pinned [nfpm](https://nfpm.goreleaser.com/). The rpm's
  dependencies are library capabilities (`libssl.so.3()(64bit)`, `libEGL.so.1()(64bit)`, ...), which resolve on Fedora, openSUSE
  and the like; the package owns its directories.
* **Signed repositories (`repo/`).** `build_repo.py` writes `repo/apt` (pool, `dists/stable`, `InRelease`, `Release.gpg`) and `repo/rpm`
  (`createrepo_c`, `repomd.xml.asc`, each rpm signed with `rpmsign`, `gpgcheck=1 repo_gpgcheck=1`), plus `maicord.gpg`/`.asc`,
  the deb822 `maicord.sources` (the key inline) and `maicord.repo`. The newest three versions stay. The secret key is the armored
  key in the `MAICORD_REPO_SIGNING_KEY` secret (`MAICORD_REPO_SIGNING_PASSPHRASE` if it has one). `publish_site.sh` checks out the
  `update-channels` branch, runs the generator and pushes one parentless commit (leased, retried), like `update_channels.ps1`;
  the `repository` job of `release.yml` runs it after the release is published (variable `MAICORD_LINUX_REPOSITORY` = `true`).
  Tested with a throwaway key in containers: `apt install` on Ubuntu 24.04, 22.04 and Debian 12 (a changed pool file and an unknown
  key are refused), `dnf install` on Fedora and `zypper` on Tumbleweed, removal leaves nothing.
* **PPA (`debian/`).** `build_source.sh --tarball … --series noble --maintainer 'Name <address>'` makes the source package (orig
  tarball = the release tarball, `debian/` installs its contents; no compiler is needed, which is why it works on a builder without
  LLVM 23). Tested: `dpkg-source -x`, `dpkg-buildpackage -b` and install in `ubuntu:24.04`.
* **COPR and OBS (`rpm/`).** `maicord.spec` installs the tarball for Fedora and openSUSE (one spec); `rpm/.copr/Makefile` is the COPR
  "make srpm" step and `_service` the OBS download. Tested: `rpmbuild -bb`, install and removal on Fedora and Tumbleweed.
* **AUR (`aur/`).** `maicord-bin` takes the tarball (Arch's rule: prebuilt means `-bin`, [AUR submission
  guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines)); tested with `makepkg`, `namcap`, `pacman -U` and `-R` in
  `archlinux`. `maicord` builds from source with Arch's own clang, lld and llvm (23.1.x:
  [llvm](https://archlinux.org/packages/extra/x86_64/llvm/), [clang](https://archlinux.org/packages/extra/x86_64/clang)): DXC and
  SPIRV-Cross come from the pinned sources of `tools/linux_shader_toolchain.json` (`prepare()` fills the build script's cache and
  refuses mismatching pins); it breaks the day Arch moves to another LLVM major until the project follows, `-bin` does not. Per release:
  `pkgver`, `updpkgsums`, `makepkg --printsrcinfo > .SRCINFO`, push.
* **Alpine, Void, Gentoo.** `alpine/APKBUILD` and `void/maicord-bin/template` take the musl tarball; the ebuild takes the glibc one
  (`net-im/maicord-bin` in the GURU overlay). They are set up for a personal repository: all three distributions build official packages
  from source, and none has LLVM 23 and DXC.
* **Flatpak (`flatpak/`).** For a self-hosted repository or a `.flatpak` bundle, not Flathub (below).
* **Nix (`nix/`).** The flake: the source build with `llvmPackages_23` and the pinned shader toolchain made offline, and `-bin`. `buildTests`
  defaults to `false`; only `build-check` (a test image, never shipped) sets it.

### What the program opens

`libssl.so.3` (OpenSSL), `libwayland-client/-egl/-cursor`, `libxkbcommon`, `libEGL`/`libGLESv2` are required; `libpulse`, `fontconfig`,
hunspell or enchant, `libfido2`, speech-dispatcher, VA-API drivers and the portals and Secret Service over D-Bus are used when present.

## Channels: who acts, what they build

| Channel | Builds from source? | Who acts | Effort | Here |
| --- | --- | --- | --- | --- |
| GitHub release assets, `install.sh`, AppImage | no (the release job builds with LLVM 23) | automation (`release.yml`) | done | yes |
| apt and dnf repository on Pages | no (`.deb`/`.rpm` of the release) | automation; owner makes the GPG key once | done | `repo/` |
| AUR `maicord-bin`, `maicord` | `-bin` no; `maicord` yes (Arch has LLVM 23.1) | owner: AUR account and SSH key, push per release (a bot such as `KSXGitHub/github-actions-deploy-aur` can, with the key as a secret) | small | `aur/` |
| Arch `extra` | yes | a Package Maintainer must adopt it, normally once the AUR package is popular ([moving packages from the AUR](https://wiki.archlinux.org/title/Package_Maintainer_guidelines)) | not in our hands | - |
| Launchpad PPA | no: a PPA builder takes build-dependencies from the Ubuntu archive and PPAs ([building a source package](https://ubuntu.com/docs/launchpad/user/reference/packaging/ppas/building-a-source-package)) and has no LLVM 23 or DXC, so the source package wraps the tarball | owner: Launchpad account, key, `debsign` + `dput` per series and release | medium | `debian/` |
| Debian, Ubuntu archives | needs a sponsor or a Debian Developer, free-software build from source (LLVM 23, DXC, SPIRV-Cross are not packaged) | not realistic now | - | - |
| Fedora COPR | `-bin` as the spec is (COPR builds have no network unless enabled; the SRPM step downloads the tarball) | owner: COPR account and project, webhook or manual build per release | small | `rpm/` |
| openSUSE Build Service | `-bin`; OBS builds `.rpm` for openSUSE, Fedora and others from one spec, and `.deb` from a `debian/` ([OBS Debian builds](https://en.opensuse.org/openSUSE:Build_Service_Debian_builds)); `download_url` service fetches the tarball | owner: OBS account and project | small | `rpm/` |
| nixpkgs | yes: `nix/package.nix` is the start, with nixpkgs' own `directx-shader-compiler` and `spirv-cross` | owner (a person): the [Automation/AI policy](https://github.com/NixOS/nixpkgs/blob/master/CONTRIBUTING.md) requires a responsible person who reviews the change and an `Assisted-by:` trailer on any commit made with AI tools; check the GPU golden tests with nixpkgs' shader tools | medium | `nix/` |
| Flathub | must build from source with no network and the manifest must be written and submitted by a person (the owner's rule: none is written here) | owner only | large | `flatpak/` is for a self-hosted repo |
| Snap Store | `-bin` from the tarball in a strict snap (`wayland`, `opengl`, `audio-playback`, `audio-record`, `network`, `desktop`, `password-manager-service` plugs; libraries from `core24` and `stage-packages`; the GPU driver from the graphics content snap) | owner: Snap Store account, name registration, review for the plugs that need approval | medium and ongoing; not done: a second sandbox to keep working (fonts, tray, Secret Service, VA-API, io_uring under snapd's seccomp) for few users | - |
| Homebrew on Linux | no | not planned | - | - |
| Gentoo GURU | `-bin` (an ebuild with `QA_PREBUILT`) | owner or a GURU contributor | small | `gentoo/` |
| Alpine `testing`, Void | upstream wants source builds; musl tarball in a personal repository | owner | small | `alpine/`, `void/` |

Only the first two rows need no human after the one-time setup. The rows marked as the owner's need an account on that service.

## Releases

`release.yml`'s `linux` job (gated by the repository variable `MAICORD_LINUX_RELEASES` = `true`; off, the Windows release is exactly what
it was) builds with the tests, then the shipping client in its own directory without them, and packages: the glibc and musl
tarballs, the two AppImages with their `.zsync`, the `.deb` and `.rpm`, `install.sh` and a `SHA256SUMS` file; `check_artifacts.sh`
checks them, the provenance of the sums is attested, and everything goes onto the draft; the release is published only if the job
succeeded. `publish` signs the glibc tarball's manifests onto the update channels. With `MAICORD_LINUX_REPOSITORY` = `true` and the
signing secret, the `repository` job adds the `.deb` and `.rpm` to the signed repositories and `pages` deploys them.

## Verification

`packaging/check.sh` (python3, desktop-file-utils, appstream, squashfs-tools, zsync, openssl; nfpm for the packages) builds the
stand-in tarballs (`build_tarball.py --stub`), checks reproducibility and the desktop and metadata files, installs and uninstalls the
tarball under a private home, builds and runs the AppImage unpacked and mounted, integrates and unintegrates it, runs
`online/test_installer.py` and, with nfpm, builds the packages and runs `check_artifacts.sh`. Distribution installs were tested
in podman containers (see the channel notes above). The installed client is checked on headless sway
(`tools/test_client_wayland.sh --binary MaiCord-….AppImage`).

## Owner checklist (only the owner can do these)

1. **Variables and secrets** (repository settings): `MAICORD_LINUX_RELEASES=true` when the client is ready to ship; for the repositories,
   `MAICORD_LINUX_REPOSITORY=true`, and in the `release` environment the secret `MAICORD_REPO_SIGNING_KEY` (and
   `MAICORD_REPO_SIGNING_PASSPHRASE` if the key has one): `gpg --quick-generate-key "MaiCord Packages <https://github.com/maj113/MaiCord/issues>" ed25519 sign 5y`,
   `gpg --armor --export-secret-keys <id>`, keep an offline backup. Pages must already serve the `update-channels` branch.
2. **First release**: tag, let the job run, install each artifact once on a real desktop (the AppImage and the one-line installer on at
   least Ubuntu and Fedora, and WSLg), then check `https://maj113.github.io/MaiCord/repo/` serves `maicord.sources` and `maicord.repo`.
3. **Screenshots**: `metainfo/…metainfo.xml.in` has a placeholder entry and a `TODO(owner)` comment; AppStream and Flathub need real
   16:9 screenshots of at least 1248x702, redacted, at a tag-pinned URL. Review the OARS rating.
4. **AUR**: an account, the SSH key, then `git clone ssh://aur@aur.archlinux.org/maicord-bin.git` (and `maicord`), copy the directory's
   `PKGBUILD` and `.SRCINFO`, push. Per release: `pkgver`, `updpkgsums`, `makepkg --printsrcinfo > .SRCINFO`.
5. **COPR / OBS** (optional): create the project, point it at `packaging/rpm` (see `rpm/.copr/Makefile` and `_service`), bump `Version` and
   `%changelog` per release. **PPA** (optional): `debsign` and `dput` the output of `debian/build_source.sh` per Ubuntu series.
6. **nixpkgs**: a pull request adding `pkgs/by-name/ma/maicord/package.nix` from `nix/package.nix`, written and reviewed by you, with the
   `Assisted-by:` trailer if a tool helped.
7. **Flathub**: write and submit the manifest yourself (requirements: build from source with no network; no AI-generated manifest; a verifiable
   `io.github.maj113` id; real screenshots). If MaiCord ships its own Flatpak repository first, a Flathub submission by someone else is refused
   ("conflicting submissions").
8. **Decide** whether the one-line installer is advertised in the README (it is documented above) and whether to publish the musl artifacts.
