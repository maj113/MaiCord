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
| `test_release_tools.py` | Self-test with a throwaway key and a scratch git history; CI runs it on every build |

## Formats (they match the client's verifiers)

- **Update package**: `MaiCord-x64-p2-<version>.zip`. One to four *stored*
  members, `MaiCord.exe` first, with no flags, extra fields or comments. Member
  names are 1–32 ASCII letters, digits, `.`, `-` or `_`, start with a letter or
  digit, differ ignoring case and are never device names, `offer.json`,
  `offer.sig`, `package.zip` or `MaiCordAccount.dll` (which marks protocol 1); `.exe` and `.dll` members must be AMD64 images
  of that kind. `MEMBER_NAMES` lists what a release ships (today only
  `MaiCord.exe`). Timestamps are fixed, so the same binaries always give the same
  bytes. At most 64 MiB. Checked by `src/app/update_package.cpp`.
- **Manifest**: `maicord-<channel>.manifest.json`. Compact JSON with exactly the
  fields `format` (the update protocol, 2), `product`, `arch`, `channel`,
  `version`, `issued`, `expires`, `bytes`, `sha256` and `artifact`; at most 4096
  bytes; no string uses escapes. Checked by `src/app/update_manifest.cpp`.
- **Changelog**: `maicord-<channel>.changelog.json`, beside its manifest:
  `{"format":1,"product":"MaiCord","channel":…,"version":V,"base":B,"entries":[{"version":v,"date":unix,"commit":"<40 hex>"|"","text":…}]}`.
  It lists every change in (B, V], newest first: up to 10 stable releases with
  their release notes, or up to 100 commit subjects, each numbered as the master
  build that first contained it. At most 64 KiB and 8 KiB per text. The client
  shows the entries newer than itself as plain text. Checked by
  `src/app/update_changelog.cpp`.
- **Signatures**: `<file>.sig`, 64 raw bytes: an ECDSA P-256 signature in P1363
  form (r ‖ s) over `SHA-256(domain ‖ file bytes)`, with the domain
  `MaiCord update manifest v1\n` or `MaiCord update changelog v1\n`.
- **Version number**: `((major·1000 + minor)·1000 + patch)·1000 + n`. A stable
  release `vX.Y.Z` has `n = 999`. A master build `N` commits after the last
  stable tag is `X.Y.Z-dev.N` (`n = N`, 1–998), where `X.Y.Z` is the next patch
  after that tag, or the CMake project version if it was already bumped
  further. So `0.3.3` = `3003999` < `0.3.4-dev.16` = `3004016` < `0.3.4` =
  `3004999`. The commit travels in the build (`MAICORD_UPDATE_COMMIT`) and the
  changelog, and the client shows `0.3.4-dev.16+581fd44`.
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

A stable package downloads from its release,
`https://github.com/<owner>/<repo>/releases/download/v<X.Y.Z>/MaiCord-x64-p2-<number>.zip`;
a dev build's package from the preview feed beside its manifest. These are plain
static files and download redirects, so the GitHub API and its rate limits are
not involved.

- **Stable** (`release.yml`, on a `vX.Y.Z` tag): the release is created as a
  draft and every asset is uploaded while static analysis (`analysis.yml`:
  CodeQL and MSVC code analysis) runs beside the build. If analysis finds a
  gated finding missing from its baseline in `tools/analysis`, the draft is
  deleted and the tag stays for a rerun; otherwise it is published as latest. Then
  `changelogs` writes the stable changelog from the published release notes and
  the preview changelog from the commits since the previous release, and
  `update_channels.ps1` installs them with the manifests attached to the
  *published* release. A release advances stable and the preview feed when it
  is newer there. A rerun after publishing leaves the
  release alone and only updates the channels.
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
- The branch is the only state and holds a single commit: each update replaces
  it, so retired packages leave no history. Writers in different concurrency
  groups lease the push on the commit they read and retry from the new tip.
  `deploy-pages.yml` deploys the branch as it is when it runs.

## Update protocol

The package layout and manifest schema form the update protocol, `PROTOCOL` in
`maicord_release.py` and `kUpdateProtocol` in `update_manifest.hpp`. A client
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
offered a package it cannot install. `MaiCord.exe` in the original folder stays
the dispatcher across updates (see Client behaviour), so a bridge must keep the
generation record and handoff that dispatcher reads.

Releases up to 0.4.0 use protocol 1: they read `updates/maicord-*.json` and
install only the two-file `MaiCord.exe` and `MaiCordAccount.dll` package, which
no later release has. Replace such an install's `MaiCord.exe` once by hand with
that of a newer release. The newer client continues the existing installation:
it reads the v2 generation journal 0.4.x wrote, still verifies the protocol-1
generations under their signed offers, installs the next release beside them
and rewrites the journal as v3, then disposes the old generation with the
following update. A 0.4.x generation is never activated or rolled back to once
the journal is v3, because it cannot read that journal. A protocol-1 bridge
would not help without replacing the dispatcher: a 0.4.x dispatcher reads only
v2 journals and protocol-1 generations.

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
   which must name the same point. The tool also prints the publisher-inventory
   fingerprint that `update_generation_inventory_fingerprint` computes from it.

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
| `MAICORD_UPDATE_VERSION` | `X.Y.Z` or `X.Y.Z-dev.N` (`maicord_release.py identity`); empty for the project version |
| `MAICORD_UPDATE_COMMIT` | the 40-digit source commit, shown with dev builds; may be empty |

Each defaults to the environment variable of the same name when the cache is
first created. Set the repository and key empty to build the updater disabled.
The installed version selects the default channel. For a local build, configure
once:

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
  a dev build whatever its manifest says. A newer offer and its changelog are
  shown in Settings; the version is then durably admitted, which raises the
  rollback floor, and only then is the package downloaded. A missing or invalid
  changelog only leaves the list out. A 404 for the manifest means nothing is
  published for the channel; a 404 for the package means it is missing, and the
  next check tries again. Manifests may be valid for at most 100 days.
- Switching a dev build to stable keeps it until a stable release newer than it
  comes out; preview moves to a stable release whenever that is the newest.
- Every phase writes one `[update]` line to `%LOCALAPPDATA%\MaiCord\logs\maicord.log`:
  each HTTP response (host, status, sizes; never paths or query strings), the
  offer, changelog, admission, staging and install results with their error
  codes, and restart, handoff and startup dispatch outcomes. The numbers are the
  ordinals of the result enums in `include/maicord/app/update_*.hpp`.
- Updates live in `%LOCALAPPDATA%\MaiCord\updates`. The first update creates
  the generation root and provisions a stable generation. Later updates are
  prepared beside it and activated by **Restart**. The new process must confirm
  that it started, or the client rolls back. **Roll back to previous version**
  swaps back to the retained generation. The version you left is not reinstalled until
  a newer release appears.
- `MaiCord.exe` in the original folder acts as the dispatcher. On start, it
  launches the newest authenticated generation and exits once that generation
  confirms startup. That dispatcher is never replaced, so the package checks,
  the generation record and the handoff must stay acceptable to every
  released version of its protocol.
- A package carries the members in `MEMBER_NAMES`, today only `MaiCord.exe`.
  New fonts or licenses need a fresh download of the portable zip.

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
`1000999`); see `tests/update_client.cpp`, which also replays a synthetic
preview build on every run.

Delete the throwaway key and output directories afterwards.
