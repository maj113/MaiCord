# Release tooling

Scripts used by `.github/workflows/release.yml` and `manifest-refresh.yml` to
package, sign and publish updates. They need only the Python standard library and
Windows CNG (`bcrypt.dll`, through `ctypes`): any Windows machine with Python 3.10
or newer will do.

| File | Purpose |
| --- | --- |
| `maicord_release.py` | `version`, `release`, `channels`, `verify`, `portable`, `notes`, `checksums` subcommands |
| `update_channels.ps1` | Checks out the `update-channels` branch, runs `channels`, commits and pushes changes |
| `cng_p256.py` | ECDSA P-256 key generation, signing and verification through CNG |
| `generate_update_key.py` | Creates a signing key pair and prints the public key forms |
| `test_release_tools.py` | Self-test with a throwaway key; CI runs it on every build |

## Formats (they match the client's verifiers)

- **Update package**: `MaiCord-x64-<version>.zip`. Exactly two *stored* members,
  `MaiCord.exe` then `MaiCordAccount.dll`, with no flags, extra fields or comments.
  Timestamps are fixed, so the same binaries always give the same bytes. Checked by
  `src/app/update_pair_package.cpp`.
- **Manifest**: `maicord-<channel>.manifest.json`. Compact JSON with exactly the
  fields `format`, `product`, `arch`, `channel`, `version`, `issued`, `expires`,
  `bytes`, `sha256` and `artifact`; at most 4096 bytes; no string uses escapes.
  Checked by `src/app/update_manifest.cpp`.
- **Signature**: `maicord-<channel>.manifest.json.sig`. It is 64 raw bytes:
  an ECDSA P-256 signature in P1363 form (r ‖ s), over
  `SHA-256("MaiCord update manifest v1\n" ‖ manifest bytes)`.
- **Version number**: `((major·1000 + minor)·1000 + patch)·1000 + n`, where `n`
  is the preview number (1–998) for a `vX.Y.Z-preview.n` tag and 999 for a stable
  `vX.Y.Z` tag. For example, `v1.2.3` is `1002003999` and `v1.2.3-preview.4` is
  `1002003004`. This keeps the client's rollback floor monotonic, and the client
  derives a package's release tag from it.
- **Validity**: `issued` is 10 minutes before signing, to allow for clock skew;
  `expires` is 90 days later by default. `manifest-refresh.yml` re-signs a channel
  manifest weekly once it is within 30 days of expiry.

## Where clients fetch updates

Releases are immutable once published, so nothing a client polls lives in a
release. The channel manifests are served by GitHub Pages; packages come from
the release of their version.

| What | URL |
| --- | --- |
| Channel manifest (+ `.sig`) | `https://<owner>.github.io/<repo>/updates/maicord-<channel>.manifest.json` |
| Package | `https://github.com/<owner>/<repo>/releases/download/v<version>/MaiCord-x64-<number>.zip` |

For `maj113/MaiCord` that is
`https://maj113.github.io/MaiCord/updates/maicord-stable.manifest.json`, and
version `1002003004` downloads from `…/releases/download/v1.2.3-preview.4/`.
These are plain static files and download redirects, so the GitHub API and its
rate limits are not involved.

- **Publishing** (`release.yml`): the release is created as a draft, every
  asset is uploaded, and then it is published. A stable tag is marked latest;
  a preview tag is a normal prerelease. Then `update_channels.ps1` downloads
  the manifests attached to the *published* release, installs them on the
  `update-channels` branch (`channels --publish`), and the workflow deploys
  that branch to Pages. A stable tag advances both channels, a preview tag only
  `preview`, and a channel never moves to a lower version. A rerun after
  publishing leaves the release alone and only updates the channels.
- **Refreshing** (`manifest-refresh.yml`): weekly, `channels
  --refresh-within-days 30` re-signs expiring manifests on the branch and
  redeploys Pages. A manual run always redeploys the current channels.
- The `update-channels` branch is the only state: `updates/` holds the
  current manifests and signatures, and every Pages deployment is that tree.
  Every manifest on it must verify with `MAICORD_UPDATE_PUBLIC_KEY` and be
  currently valid, or the job fails before deploying.

### Clients from 0.1.x

Clients up to 0.1.1 read
`https://github.com/<repo>/releases/latest/download/maicord-stable.manifest.json`
and download the package from `releases/latest/download/`, so every release also
attaches its freshly signed manifests. Those copies are immutable and valid for
90 days after publishing; after that a 0.1.x client reports an expired update
until a newer stable release is published. The rolling `update-preview`
prerelease is retired.

0.1.0 and 0.1.1 cannot install any later package: their installer requires the
package's `MaiCordAccount.dll` to export the installer's own account API ordinal
(114 or 115), and later releases export a newer one. The download succeeds and
the install fails with "Couldn't install the update." Replace such an install once
by hand with the portable zip of a newer release. Later installers accept the
package's own API ordinal, and the new `MaiCord.exe` checks its DLL when it
confirms startup.

## Provenance

Each release attests its assets (every file listed in `SHA256SUMS.txt`) with
`actions/attest-build-provenance`. Check a download with the GitHub CLI:

```powershell
gh attestation verify MaiCord-0.2.0-windows-x64.zip -R maj113/MaiCord
```

## Repository settings

- **Visibility**: public. GitHub Pages, anonymous release downloads and
  artifact attestations all need a public repository on the free plan. While
  the repository is private, `release.yml` still publishes the release but
  skips the attestation and the Pages update, and `manifest-refresh.yml` does
  nothing.
- **Pages**: Settings > Pages > Source: **GitHub Actions**. No custom domain:
  the client accepts only `<owner>.github.io`, and a custom domain would
  redirect the manifests away from it, stopping updates for every installed
  client. The same goes for a custom domain on the `<owner>.github.io` user
  site, which project sites inherit. In Settings > Environments >
  `github-pages`, allow the tag pattern `v*` to deploy, because `release.yml`
  runs on tags.
- **Website**: Pages also serves the website in `site/`. Every deploy
  replaces the whole Pages site, so `release.yml`, `manifest-refresh.yml` and
  `site.yml` (which runs when `site/` changes) all deploy the output of
  `assemble_pages.ps1`: `site/` plus `updates/` from the update-channels
  branch. All three share the `update-channels` concurrency group. GitHub
  keeps one pending run per group, so a release still waiting for another run
  is cancelled by a newer pending run; re-run it from the Actions tab.
- **Immutable releases**: Settings > General > Releases. Neither workflow
  modifies a published release, so this can be enabled. It applies to releases
  published afterwards. The retired `update-preview` prerelease can be deleted.

## Key handling

1. **Generate the key on a trusted machine**, outside the repository:
   `python tools/release/generate_update_key.py %USERPROFILE%\maicord-update-key.jwk`
2. **Save the private key in GitHub.** Store the JWK file contents as the
   secret `MAICORD_UPDATE_SIGNING_KEY` in the `release` environment.
   Restrict that environment to `v*` tags and the default branch (the weekly
   refresh runs there), and optionally require a reviewer.
3. **Back up and delete the local file.** Keep an offline backup, for example
   in a password manager or on encrypted removable media. Then delete the file.
4. **Publish the public key.** Store the printed 130-digit hex point as the
   repository variable `MAICORD_UPDATE_PUBLIC_KEY`. The release job refuses to
   sign with any other key.
5. **Build the client with the same point.** `release.yml` passes the
   repository variable to the build as the CMake cache variable
   `MAICORD_UPDATE_PUBLIC_KEY`, together with `MAICORD_UPDATE_REPOSITORY`
   (`github.repository`) and `MAICORD_UPDATE_PREVIEW` (the tag's preview
   number). The tool also prints the publisher-inventory fingerprint that
   `update_generation_inventory_fingerprint` computes from that point.

Never commit a private key. `*.jwk` is git-ignored.

Rotating the key needs a client release that trusts the new key *before* any
release is signed with it.

## Client configuration

The updater is always compiled in. It is enabled only when both CMake cache
variables are set; otherwise Settings > Updates says updates aren't configured
for this build. `cmake/client.cmake` defaults them to the official publisher.

| Variable | Value |
| --- | --- |
| `MAICORD_UPDATE_REPOSITORY` | `owner/repo` that publishes the releases; the owner also names the Pages host |
| `MAICORD_UPDATE_PUBLIC_KEY` | the 130-digit hex point from `generate_update_key.py` |
| `MAICORD_UPDATE_PREVIEW` | `N` for a `vX.Y.Z-preview.N` build; empty or `0` for stable |

Each defaults to the environment variable of the same name when the cache is
first created. Set both empty to build the updater disabled. The installed
version is derived from `project(MaiCord VERSION X.Y.Z)` and
`MAICORD_UPDATE_PREVIEW` with the formula above, and selects the default
channel. For a local build, configure once:

```powershell
cmake --preset x64-release -DMAICORD_UPDATE_REPOSITORY=owner/repo -DMAICORD_UPDATE_PUBLIC_KEY=04...
```

Client behaviour:

- It checks about 1–3 minutes after start, then every 24 hours ±1 hour, and
  when you select **Check now**. Failures back off exponentially from five
  minutes to 24 hours. `Retry-After` and an exhausted `x-ratelimit-reset` are
  honoured.
- Requests use HTTPS only and follow at most five redirects. Manifests come
  from `<owner>.github.io`, which may redirect only to itself. Packages start
  at `github.com`, which may redirect only to `objects.githubusercontent.com`
  or `release-assets.githubusercontent.com`. The manifest is at most 4096
  bytes, and the signature is exactly 64 bytes. The package may be at most the
  signed size, and at most 64 MiB.
- Signatures are verified before anything is stored. The version is then
  durably admitted, which raises the rollback floor, and only then is the
  package downloaded. A 404 for the manifest means nothing is published for the
  channel; a 404 for the package means its release is missing, and the next
  check tries again. Manifests may be valid for at most 100 days.
- Every phase writes one `[update]` line to `%LOCALAPPDATA%\MaiCord\logs\maicord.log`:
  each HTTP response (host, status, sizes; never paths or query strings), the
  offer, admission, staging and install results with their error codes, and
  restart, handoff and startup dispatch outcomes. The numbers are the ordinals
  of the result enums in `include/maicord/app/update_*.hpp`.
- Updates live in `%LOCALAPPDATA%\MaiCord\updates`. The first update creates
  the generation root and provisions a stable generation. Later updates are
  prepared beside it and activated by **Restart**. The new process must confirm
  that it started, or the client rolls back. **Roll back to previous version**
  swaps back to the retained pair. The version you left is not reinstalled until
  a newer release appears.
- `MaiCord.exe` in the original folder acts as the dispatcher. On start, it
  launches the newest authenticated generation and exits once that generation
  confirms startup. That dispatcher is never replaced, so the package checks,
  the generation record and the handoff must stay acceptable to every
  released version: they may not depend on version-specific values such as
  the account API ordinal.
- A package carries only `MaiCord.exe` and `MaiCordAccount.dll`. New fonts or
  licenses need a fresh download of the portable zip.

## Local dry run

```powershell
python -B tools\release\test_release_tools.py
python tools\release\generate_update_key.py $env:TEMP\throwaway.jwk
python tools\release\maicord_release.py release --tag v0.0.1 `
  --binaries ..\MaiCord-build\Release --output-dir $env:TEMP\maicord-dist --key-file $env:TEMP\throwaway.jwk
```

To replay a set of release files through the real client pipeline offline,
put the manifest, its `.sig`, the package and `public-key.txt` (the hex point)
in one directory and run the client tests with `MAICORD_UPDATE_REPLAY_DIR`
pointing at it (`MAICORD_UPDATE_REPLAY_OWN` sets the installed version number,
default `1000999`); see `tests/update_client.cpp`.

Delete the throwaway key and output directory afterwards.
