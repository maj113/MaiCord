# Release tooling

Scripts used by `.github/workflows/release.yml`, the preview job in `ci.yml`
and `manifest-refresh.yml` to package, sign and publish updates. They need only
the Python standard library and Windows CNG (`bcrypt.dll`, through `ctypes`):
any Windows machine with Python 3.10 or newer will do.

| File | Purpose |
| --- | --- |
| `maicord_release.py` | `version`, `identity`, `release`, `preview`, `changelogs`, `channels`, `verify`, `portable`, `notes`, `checksums` subcommands |
| `update_channels.ps1` | Checks out the `update-channels` branch, runs `channels`, replaces the branch with one commit of the new site |
| `cng_p256.py` | ECDSA P-256 key generation, signing and verification through CNG |
| `generate_update_key.py` | Creates a signing key pair and prints the public key forms |
| `plugin_index.py` | Renders, signs and verifies the catalog index (format 2: plugins, themes and effects) with the client's strict schema |
| `test_plugin_index_tools.py` | Checks `plugin_index.py` against the vectors in `tests/fixtures/plugin_index/format2`; CI runs it on every build |
| `test_release_tools.py` | Self-test with a throwaway key and a scratch git history; CI runs it on every build |

## Formats (they match the client's verifiers)

- **Update package**: `MaiCord-x64-p2-<version>.zip`. Exactly one *stored*
  member, `MaiCord.exe`, an AMD64 executable image, with no flags, extra
  fields or comments. The client swaps that one file in place of itself, so a
  release can't add other files (`PACKAGE_EXECUTABLE`). Timestamps are fixed,
  so the same binary always gives the same bytes. At most 64 MiB. Checked by
  `src/app/update/package.cpp`.
- **Linux package**: `maicord-<X.Y.Z>-linux-x86_64.tar.gz`, the release tarball of
  `packaging/build_tarball.py` byte for byte (the manifest's `sha256` is the release asset's).
  One gzip member of a tar in the version's directory: `MaiCord`, an x86-64
  position-independent ELF executable, and the flat `fonts/` and `licenses/` files (plain
  names of at most 64 characters), which the client stages beside the install and swaps in
  with the program; everything else in the tarball is left alone, and any other member
  type, a name outside the directory, a nested or repeated file or a second program fails
  the package. At most 64 MiB packed and 192 MiB unpacked. Checked by
  `src/app/update/linux/package.cpp`; `validate_linux_tarball` and `validate_linux_elf` in
  `maicord_release.py` mirror it.
- **AppImage package**: `MaiCord-<X.Y.Z>-x86_64.AppImage`, the glibc image of
  `packaging/appimage/build_appimage.py` byte for byte (at most 64 MiB). The client stages it whole beside
  `$APPIMAGE` after checking the type 2 runtime's identification (`\x7fELF`, `AI\x02` at byte 8);
  `validate_appimage` also checks the SquashFS behind the runtime's ELF. The install swaps one file, not a tree.
- **Manifest**: `maicord-<channel>.manifest.json`. Compact JSON with exactly the
  fields `format` (the update protocol, 2), `product`, `arch`, `channel`,
  `version`, `issued`, `expires`, `bytes`, `sha256` and `artifact`; at most 4096
  bytes; no string uses escapes. Checked by `src/app/update/manifest.cpp`.
- **Changelog**: `maicord-<channel>.changelog.json`, beside its manifest:
  `{"format":1,"product":"MaiCord","channel":…,"version":V,"base":B,"entries":[{"version":v,"date":unix,"commit":"<40 hex>"|"","text":…}]}`.
  It lists every change in (B, V], newest first: up to 10 stable releases with
  their release notes, or up to 100 commit subjects, each numbered as the master
  build that first contained it. At most 64 KiB and 8 KiB per text. The client
  keeps the verified document of the channel (`changelog-<channel>` beside the
  update state, signature then document, verified again on every load) and
  renders it on Settings > Updates with the chat's Markdown parser. Checked by
  `src/app/update/changelog.cpp`.
- **Signatures**: `<file>.sig`, 64 raw bytes: an ECDSA P-256 signature in P1363
  form (r ‖ s) over `SHA-256(domain ‖ file bytes)`, with the domain
  `MaiCord update manifest v1\n` or `MaiCord update changelog v1\n`.
- **Version number**: `((major·1000 + minor)·1000 + patch)·1000 + n`. A stable
  release `vX.Y.Z` has `n = 999`. A master build `N` commits after the last
  stable tag is `X.Y.Z-dev.N` (`n = N`, 1–998), where `X.Y.Z` is the next patch
  after that tag, or the CMake project version if it was already bumped
  further. So `0.3.3` = `3003999` < `0.3.4-dev.16` = `3004016` < `0.3.4` =
  `3004999`. The commit travels in the build (`MAICORD_UPDATE_COMMIT`) and the
  changelog, and the client shows `0.3.4-dev.16+581fd44`. Local builds are
  numbered the same way from their checkout (see Client configuration).
- **Validity**: `issued` is 10 minutes before signing, to allow for clock skew;
  `expires` is 90 days later by default. `manifest-refresh.yml` re-signs a
  manifest weekly once it is within 30 days of expiry. Changelogs don't expire;
  the client uses one only when its version and channel match the manifest.

## Where clients fetch updates

Releases are immutable once published, so nothing a client polls lives in a
release. GitHub Pages serves the site of the `update-channels` branch:

| Path under `https://<owner>.github.io/<repo>/` | Contents |
| --- | --- |
| `updates/p2/maicord-stable.{manifest,changelog}.json[.sig]` | the stable channel |
| `updates/p2/preview/maicord-preview.{manifest,changelog}.json[.sig]` | the preview feed |
| `updates/p2/preview/MaiCord-x64-p2-<number>.zip` | the current master build and the one before it |
| `updates/p2/linux/maicord-stable.manifest.json[.sig]` | the Linux client's stable channel (its package is the release tarball) |
| `updates/p2/linux/preview/maicord-preview.manifest.json[.sig]` | the Linux preview feed |
| `updates/p2/linux/preview/maicord-<X.Y.Z>-dev.<N>-linux-x86_64.tar.gz` | its current master build and the one before it |
| `updates/p2/linux/appimage/maicord-stable.manifest.json[.sig]` | the glibc AppImage's stable channel (its package is the release's `MaiCord-<X.Y.Z>-x86_64.AppImage`) |
| `updates/p2/linux/appimage/preview/maicord-preview.manifest.json[.sig]` | the AppImage's preview feed |
| `updates/p2/linux/appimage/preview/MaiCord-<X.Y.Z>-dev.<N>-x86_64.AppImage` | its current master build and the one before it |

A stable package downloads from its release,
`https://github.com/<owner>/<repo>/releases/download/v<X.Y.Z>/MaiCord-x64-p2-<number>.zip`;
a dev build's package from the preview feed beside its manifest. These are plain
static files and download redirects, so the GitHub API and its rate limits are
not involved.

- **Stable** (`release.yml`, on a `vX.Y.Z` tag): the release is created as a
  draft and every asset is uploaded while static analysis (`analysis.yml`:
  CodeQL) runs beside the build. If analysis finds a
  gated finding missing from its baseline in `tools/analysis`, the draft is
  deleted and the tag stays for a rerun; otherwise it is published as latest. Then
  `changelogs` writes the stable changelog from the published release notes and
  the preview changelog from the commits since the previous release, and
  `update_channels.ps1` installs them with the manifests attached to the
  *published* release. A release advances stable and the preview feed when it
  is newer there. A rerun after publishing leaves the
  release alone and only updates the channels.
- **Release notes** (`notes`): the tagged commit's
  `.github/release-notes/vX.Y.Z.md`, when it has one, then the commit subjects
  since the previous tag grouped as features, fixes, performance and other.
  Put what users must know before updating, such as manual steps, in that file
  and commit it before tagging. The in-app changelog shows both parts.
- **Preview** (the `preview` job of `ci.yml`): every push to master of
  `maj113/MaiCord` whose client and test jobs pass is packaged from the CI
  client artifact, signed with its changelog (`preview`) and installed on the
  preview feed. Older dev packages are removed; a stable release retires them all.
- **Refreshing** (`manifest-refresh.yml`): weekly, `channels
  --refresh-within-days 30` re-signs expiring manifests and redeploys Pages. A
  manual run always redeploys the current channels.
- A channel never moves to a lower version, and an equal version keeps the
  site's (possibly re-signed) copy. Every manifest left on the site must verify
  with `MAICORD_UPDATE_PUBLIC_KEY` and be currently valid, every changelog must
  match its manifest's version (a stale one is removed), and a dev build's
  package must be present, or the job fails before deploying.
- **The Linux entry** (`MAICORD_LINUX_RELEASES` repository variable `true`): the Linux
  client reads its own manifests, `updates/p2/linux/maicord-stable.manifest.json[.sig]` and `updates/p2/linux/preview/maicord-preview.manifest.json[.sig]`,
  signed with the same key, whose `arch` is `linux-x64` and whose `artifact` is the release
  tarball `maicord-<X.Y.Z>-linux-x86_64.tar.gz` that `packaging/build_tarball.py` made (the
  `linux` job of `release.yml` attaches it to the draft). The `publish` job signs them from
  the *published* tarball (`maicord_release.py linux`, which first checks the tarball the
  way the client's reader does: one gzip member of a tar with the program, an x86-64 ELF,
  and flat `fonts/` and `licenses/` in the version's directory) and `update_channels.ps1`
  (`-Linux`, or the same variable in its environment) installs them beside the Windows
  files, verifies them and, when they near expiry, re-signs them. Without the flag the Linux
  files on the site are left alone. A manifest the other system's reader would meet fails
  its checks on the signed `arch` and `artifact` (a Windows manifest at the Linux path, or
  a Linux one at the Windows path), so the schema needs no new member. The Linux preview
  feed works like the Windows one: every green master build of `maj113/MaiCord` is
  packaged by the `linux` job of `ci.yml` as a tarball of its own version
  (`maicord-<X.Y.Z>-dev.<N>-linux-x86_64.tar.gz`, `build_tarball.py --version X.Y.Z-dev.N`),
  which the `preview` job signs (`maicord_release.py linux --preview`) and
  `update_channels.ps1` installs as `updates/p2/linux/preview/maicord-preview.manifest.json`
  with the tarball beside it (the current one and the one before it are kept; a stable
  release retires them). The Linux job gates nothing: without its tarball the Windows
  build is still published and the Linux feed stays where it was. A stable release's
  tarball needs no copy on the site (it is a release asset). The changelogs are the
  Windows client's files, which describe versions, so a master build's one document
  serves both; the Linux client shows an older cached one while its feed lags.
  **The AppImage** has a flavor of its own on the same feed (`maicord_release.py linux --appimage`, `arch`
  `linux-x64-appimage`, `artifact` `MaiCord-<X.Y.Z>-x86_64.AppImage` as `packaging/appimage/build_appimage.py` names it),
  signed with the same key and laid out below `updates/p2/linux/appimage/` the way the tarball's are: the `publish` job
  signs it from the published glibc AppImage, the `preview` job from the `linux` job's dev AppImage, and
  `update_channels.ps1 -Linux` installs both, with the dev AppImages beside the preview manifest (retired like the
  tarballs). A reader rejects the other flavor's manifest on `arch` and `artifact`. The musl AppImage is not in the feed.
- The branch is the only state and holds a single commit: each update replaces
  it, so retired packages leave no history. Writers in different concurrency
  groups lease the push on the commit they read and retry from the new tip.
  `deploy-pages.yml` deploys the branch as it is when it runs.

## Update protocol

The package layout and manifest schema form the update protocol, `PROTOCOL` in
`maicord_release.py` and `kUpdateProtocol` in `app/update/manifest.hpp`. A client
reads only `updates/p<protocol>/` and accepts only manifests whose `format` is
its protocol, so a release a client cannot install is never offered to it.
`channels` maintains only the current protocol's directory and leaves every
other one as published.

A release that changes the protocol bumps both constants, and with them the
artifact prefix and the site directory. Clients of the previous protocol keep
the newest release in their directory. To move them on, also publish a bridge
there: a build of the new protocol, packaged and signed in the old format. Old
clients install it like any update, and from then on it reads the new
directory, so a 0.5 client reaches 1.0 through a 0.9 bridge without ever being
offered a package it cannot install.

Two earlier updaters are not migrated. Releases up to 0.4.0 and master builds
up to 0.4.1-dev.44 use protocol 1: they read `updates/maicord-stable.*` and
`updates/preview/maicord-preview.*`, accept only `format` 1 and install only a
`MaiCord.exe` and `MaiCordAccount.dll` pair, which no later release has.
`channels` never touches those files, so these clients stay on 0.4.0 (or
0.4.1-dev.43 on preview) and report an expired manifest once its signature
lapses in January 2027. Master builds 0.4.1-dev.45 to dev.90, before the
in-place swap, read protocol 2 but install packages as "generations" and start
them through a dispatcher that waits for a startup handoff, which current
builds don't perform: a newer build fails to start and they stay on their
version. Both kinds keep installed updates under
`%LOCALAPPDATA%\MaiCord\updates\generations` and start them through the
`MaiCord.exe` the user launches. To move such an install on, quit MaiCord and
replace that `MaiCord.exe` (not a copy under `generations`) with a current
release's. Once it has started, `updates\generations`,
`updates\enrollment.bin` and `MaiCordAccount.dll` beside the executable are
unused and can be deleted; `updates\channel`, `updates\interval` and
`updates\work` keep the same format and stay in use. A bridge can't help.

## Provenance

Each release attests its assets (every file listed in `SHA256SUMS.txt`) with
`actions/attest-build-provenance`. Check a download with the GitHub CLI:

```powershell
gh attestation verify MaiCord-0.2.0-windows-x64.zip -R maj113/MaiCord
```

Preview builds are not attested; their signed manifest is their provenance.

## Repository settings

- **Visibility**: public. GitHub Pages, anonymous release downloads and
  artifact attestations all need a public repository on the free plan. While
  the repository is private, `release.yml` still publishes the release but
  skips the attestation and the Pages update, the preview job does not run, and
  `manifest-refresh.yml` does nothing.
- **Pages**: Settings > Pages > Source: **GitHub Actions**. No custom domain:
  the client accepts only `<owner>.github.io`, and a custom domain would
  redirect the manifests away from it, stopping updates for every installed
  client. The same goes for a custom domain on a `<owner>.github.io` user
  site, which project sites inherit. The website lives in its own repository,
  [maj113/MaiCord-Site](https://github.com/maj113/MaiCord-Site), which
  serves maicord.com. In Settings > Environments > `github-pages`, allow
  `master` and the tag pattern `v*` to deploy.
- **Immutable releases**: Settings > General > Releases. No workflow modifies a
  published release, so this can stay enabled.
- **`update-channels` branch**: workflows force-push it; don't protect it.

## Key handling

1. **Generate the key on a trusted machine**, outside the repository:
   `python tools/release/generate_update_key.py %USERPROFILE%\maicord-update-key.jwk`
2. **Save the private key in GitHub.** Store the JWK file contents as the
   secret `MAICORD_UPDATE_SIGNING_KEY` in the `release` environment.
   Restrict that environment's deployment branches and tags to `master` (the
   preview job and the weekly refresh) and `v*` tags. A required reviewer would
   have to approve every preview build.
3. **Back up and delete the local file.** Keep an offline backup, for example
   in a password manager or on encrypted removable media. Then delete the file.
4. **Publish the public key.** Store the printed 130-digit hex point as the
   repository variable `MAICORD_UPDATE_PUBLIC_KEY`. The signing jobs refuse to
   sign with any other key.
5. **Build the client with the same point.** `release.yml` passes the
   repository variable to the build as the CMake cache variable
   `MAICORD_UPDATE_PUBLIC_KEY`, together with `MAICORD_UPDATE_REPOSITORY`
   (`github.repository`). CI builds use the defaults in `cmake/client.cmake`,
   which must name the same point.

Never commit a private key. `*.jwk` is git-ignored.

Rotating the key needs a client release that trusts the new key *before* any
release is signed with it.

## Client configuration

The updater is always compiled in. It is enabled only when both the repository
and the key are set; otherwise Settings > Updates says updates aren't configured
for this build. `cmake/client.cmake` defaults them to the official publisher.

| Variable | Value |
| --- | --- |
| `MAICORD_UPDATE_REPOSITORY` | `owner/repo` that publishes the releases; the owner also names the Pages host |
| `MAICORD_UPDATE_PUBLIC_KEY` | the 130-digit hex point from `generate_update_key.py` |
| `MAICORD_UPDATE_VERSION` | `X.Y.Z` or `X.Y.Z-dev.N` (`maicord_release.py identity`); empty for the checkout's git build identity |
| `MAICORD_UPDATE_COMMIT` | the 40-digit source commit, shown with dev builds; empty for `HEAD` with a git build identity |

Each defaults to the environment variable of the same name when the cache is
first created. Set the repository and key empty to build the updater disabled.
The installed version selects the default channel.

CI and `release.yml` pass the version and commit. A local build leaves them
empty, and every configure (each `tools/build.ps1` run) numbers it with
`maicord_release.py identity` from git, exactly as CI numbers master builds:
a build of the published commit, with or without uncommitted edits, is the
same version, and one with commits after it is newer, so the updater never
offers a local build an older one. Without a git build identity (no stable
tag reachable, a shallow clone, more than 998 commits since the tag) the
configure warns and builds the project version with the updater disabled. For
a local build of another publisher, configure once:

```powershell
cmake --preset x64-release -DMAICORD_UPDATE_REPOSITORY=owner/repo -DMAICORD_UPDATE_PUBLIC_KEY=04...
```

Client behaviour:

- It checks about 1–3 minutes after start, then every 24 hours ±1 hour, and
  when you select **Check now**. Failures back off exponentially from five
  minutes to 24 hours. `Retry-After` and an exhausted `x-ratelimit-reset` are
  honoured.
- Requests use HTTPS only and follow at most five redirects. Manifests,
  changelogs and preview packages come from `<owner>.github.io`, which may
  redirect only to itself. Release packages start at `github.com`, which may
  redirect only to `objects.githubusercontent.com` or
  `release-assets.githubusercontent.com`. The manifest is at most 4096 bytes,
  the changelog 64 KiB, and each signature exactly 64 bytes. The package may be
  at most the signed size, and at most 64 MiB.
- Signatures are verified before anything is stored. The stable channel refuses
  a dev build whatever its manifest says. The channel changelog is fetched with
  every check whose manifest version differs from the cached one, up to date or
  not, so the running version's notes are available. A newer offer is shown in
  Settings; the version is then durably admitted, which raises the
  rollback floor, and only then is the package downloaded. A missing or invalid
  changelog only leaves the list out (an older cached one stays). A 404 for the manifest means nothing is
  published for the channel; a 404 for the package means it is missing, and the
  next check tries again. Manifests may be valid for at most 100 days.
- Switching a dev build to stable keeps it until a stable release newer than it
  comes out; preview moves to a stable release whenever that is the newest.
- Every phase writes one `[update]` line to `%LOCALAPPDATA%\MaiCord\logs\maicord.log`:
  each HTTP response (host, status, sizes; never paths or query strings), the
  offer, changelog, admission, preparation and staging results with their
  error codes, the exit swap, the restart launch and startup cleanup. The
  numbers are the ordinals of the result enums in `include/maicord/app/update/*.hpp`.
- The acceptance record and the package download live in
  `%LOCALAPPDATA%\MaiCord\updates\work`. After the download's size and SHA-256
  match the signed manifest, the package's `MaiCord.exe` is checked (ZIP layout,
  CRC, AMD64 executable image), written beside the running executable as
  `MaiCord.exe.update`, read back and held open so nothing can change it. The
  install folder must be writable. Settings then reads **MaiCord x.y.z is ready to install**.
- Nothing is installed until you choose **Restart to update** in Settings >
  Updates or on the title bar badge. MaiCord then exits normally, renames the
  running `MaiCord.exe` to `MaiCord.exe.old`, moves the stage to `MaiCord.exe`
  (if that second move fails it moves the old executable back), deletes the
  package and starts `MaiCord.exe`, now the new version. Any other exit keeps
  the running version. Only a stage the exiting process verified itself is ever
  moved; the next start deletes `MaiCord.exe.old` and any stage an earlier
  session left behind. There is no rollback: an older release is never
  installed over a newer one.
- An update you haven't installed keeps its package. The next start's first
  check fetches the manifest as usual and, when the kept package still has the
  signed size and SHA-256, verifies and stages it again exactly like a fresh
  download instead of downloading it; otherwise it downloads the package. A
  check that finds nothing newer than the installed version deletes the kept
  package, and a newer offer replaces it.
- A package carries only `MaiCord.exe`. New fonts or licenses need a fresh
  download of the portable zip.

## Plugin catalog

The official catalog lives in
[maj113/MaiCord-plugins](https://github.com/maj113/MaiCord-plugins): plugin
sources, themes and background effects. Its `catalog.yml` workflow builds every
item, renders the format-2 index with `plugin_index.py` from this repository's
`master` once per client reader (`plugin_index.INDEXES`), signs them, verifies
them, and deploys them to GitHub Pages:

- `https://maj113.github.io/MaiCord-plugins/index-4.json` (+ `.sig`): the Linux
  client's index (`plugin_index.reader_source(source, 4)`), signed three
  sequences above `index.json`. It is `index-3.json` with each effect's
  interface 4 package: the same package with the manifest naming `interface`
  4 and a GLSL section (type 10) holding the effect's shaders translated to
  GLSL ES 3.10 (`maicord-plugin effect build --glsl yes --dxbc-from`), which
  the Linux GLES backend runs where Windows runs the DXBC. The source names it
  in the effect item's `universal` member (a file record like `file`, for the
  same version): only the source has it, `reader_source` swaps it in as `file`
  for reader 4 and drops it for every other reader, and `check_source`
  validates it. `plugin_index.check_universal(plain, universal)` checks the
  two packages (every other section byte for byte the same, the GLSL section
  one well-formed entry per shader, DXBC order and stage). A Linux client
  refuses a package without the section (`McfxError::payload`), which is why
  an effect's DXBC package stays what the other three indexes list.
- `https://maj113.github.io/MaiCord-plugins/index-3.json` (+ `.sig`): every
  item, which MaiCord 0.6.4 and later read on Windows. Any item (plugin, theme or effect)
  may name the interface it needs in `interface`: an effect's `.mcfx` interface,
  a theme's document interface (2 for a theme with `variants`, which changes
  with the time of day), a plugin entry's level (1). The client lists an item
  above its own level as needing a newer MaiCord and doesn't download it. Its
  reader skips members and item kinds it doesn't know (and still refuses a
  malformed value of one it knows), so later fields land in this file without
  another split.
- `https://maj113.github.io/MaiCord-plugins/index-full.json` (+ `.sig`): the
  index for 0.6.3 (`plugin_index.reader_source(source, 2)`), signed one
  sequence lower. Its reader refuses the whole index over an unknown member and
  takes `interface` on effects only, so it leaves out the themes and plugins
  that name one.
- `https://maj113.github.io/MaiCord-plugins/index.json` (+ `.sig`): the index for
  0.6.0 to 0.6.2 (`reader_source(source, 1)`), signed two sequences lower.
  Their reader refuses `interface` anywhere and their package reader any
  interface but 1, so it also leaves out the effects that need one, and a
  theme's link to them.
- `details/<sha256>.md` and `previews/<sha256>.png` (the item pages the
  site and the client show) and `files/<sha256>.dll|.mcfx|.json` (the packages)

The index signature covers SHA-256 of `"MaiCord plugin index v2\n"` and the
document (64-byte P1363). The schema is `plugin_index.py`; the shared vectors
are `tests/fixtures/plugin_index/format2`. The workflow runs on every push to
`main`, on manual dispatch and weekly. Each run issues an index valid for 30
days (the client accepts at most 35) whose `sequence` is its issue time, so
every run outranks the last one. Each newer client's index takes one more
(`index-full.json` +1, `index-3.json` +2, `index-4.json` +3), so updating MaiCord from 0.6.2 to
0.6.3 to 0.6.4 always moves to an index above the one it last accepted. A
scheduled run also lands an empty commit when `main` is 45 days old, because
GitHub disables the schedules of a public repository after 60 days without one.

Keys:

- **Current** key: `%USERPROFILE%\maicord-plugin-catalog-key.jwk`. Its contents
  are the secret `MAICORD_PLUGIN_CATALOG_SIGNING_KEY` in the plugins
  repository's `catalog` environment (deployment branch `main`, no required
  reviewer, which would hold every weekly re-signing). Its point is the
  variable `MAICORD_PLUGIN_CATALOG_PUBLIC_KEY` in both repositories; the
  workflow verifies against it before deploying.
- **Next** key: `%USERPROFILE%\maicord-plugin-catalog-next-key.jwk`, kept
  offline and used for nothing until a rotation. Its point is this
  repository's variable `MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY`.

Create a key pair with `generate_update_key.py` (ignore the updater names it
prints) and back both files up offline. `cmake/client.cmake` compiles both
points in as the defaults, as it does the updater key, so every build, local
ones included,
trusts the official catalog. `ci.yml` and `release.yml` also pass both
variables to the client build as the environment variables of the same names,
which override those defaults; setting both empty compiles the official catalog
as not configured. They seed only a new cache: a `CMakeCache.txt` kept by
`-KeepIntermediates` keeps the values it was created with. A rotation updates
the defaults in `cmake/client.cmake` along with the variables.

Rotating to the next key needs no client release, because clients already
trust it:

1. Store the next JWK as `MAICORD_PLUGIN_CATALOG_SIGNING_KEY` and its point as
   the plugins repository's `MAICORD_PLUGIN_CATALOG_PUBLIC_KEY`, then run the
   workflow.
2. Generate a new next key. In this repository set
   `MAICORD_PLUGIN_CATALOG_PUBLIC_KEY` to the point now signing and
   `MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY` to the new one, and release. Keep
   the retired key's backup until no supported build trusts it alone.

Never commit a private key or print its contents; only the 130-digit points
belong in variables, logs and documents.

## Local dry run

```powershell
python -B tools\release\test_release_tools.py
python tools\release\generate_update_key.py $env:TEMP\throwaway.jwk
python tools\release\maicord_release.py release --tag v0.0.1 `
  --binaries ..\MaiCord-build\Release --output-dir $env:TEMP\maicord-dist --key-file $env:TEMP\throwaway.jwk
python tools\release\maicord_release.py preview --binaries ..\MaiCord-build\Release `
  --output-dir $env:TEMP\maicord-preview --key-file $env:TEMP\throwaway.jwk
```

To replay a set of published files through the real client pipeline offline,
put the manifest, its `.sig`, optionally the changelog and its `.sig`, the
package and `public-key.txt` (the hex point) in one directory and run the client
tests with `MAICORD_UPDATE_REPLAY_DIR` pointing at it
(`MAICORD_UPDATE_REPLAY_OWN` sets the installed version number, default
`1000999`); see `tests/app/update/client.cpp`, which also replays a synthetic
preview build on every run.

Delete the throwaway key and output directories afterwards.
