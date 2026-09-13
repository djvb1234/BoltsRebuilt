# Source and publication boundary

This preview was assembled from the project's committed development snapshot
`96b24b7964900b7dc06a8fed3aeb80c4bc19634e`. Private development history and subsequent
uncommitted experiments are not imported into the public history.

## Original implementation and SDK components

The application, custom renderer and tools are published under GPL-3.0-only for
the original work owned by the project contributors. Pre-existing third-party
portions retain their notices and licenses. See [third-party notices](../THIRD_PARTY_NOTICES.md).

RexGlue v0.10.0 provides the runtime and Xenia-derived graphics infrastructure.
Modified graphics files are under `src/gpu/vendored`; their original headers and
upstream change record are retained. The separately downloaded SDK is not part
of this repository's source archive.

## Compatibility data

`config/compatibility.toml` contains numeric function boundaries/extents and only
the short symbol aliases referenced by the host hooks. It is a mechanical,
address-sorted reduction of the earlier imported compatibility tables. The bulk
symbol catalogue and copied commentary are omitted. These values were researched
using [reNut](https://github.com/masterspike52/reNut), including its main and Renderer
branch tables, and checked against the PAL executable in the local project.

This does **not** claim the values were independently discovered, or that reNut
granted this project a software license. No license was found on its public
repository during the September 2026 review. The original imported files and
reference exports are not redistributed here.

Under U.S. Copyright Office guidance, individual facts and short names are not
copyrightable; an original selection/arrangement or accompanying expression may
be. This file reduction is a technical publication decision, not a legal opinion
or a guarantee that no rights apply in any jurisdiction. Provenance remains
explicit so questions can be reviewed without concealing the source.

References: [Copyright Office database guidance](https://www.copyright.gov/register/tx-databases.html),
[GitHub licensing guidance](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository).

## Material kept local

- Game images, XEX/XEXP executables, assets, audio, textures and extracted bundles.
- Generated/recompiled game C++, local game executables and object files.
- Captured shader programs, translated game shaders, compiled shader libraries and caches.
- Six earlier embedded game-shader transcriptions; their optional paths fall back.
- Saves, memory dumps, private logs, research exports and unreviewed screenshots.

The original HLSL helper/prelude and procedural diagnostic shader remain source
components. They are not a distributed library of the game's captured shaders.

## Before publishing later changes

Reviewed documentation screenshots are listed in
[`screenshots/manifest.json`](screenshots/manifest.json), with source capture
names and exact content hashes. They illustrate local development builds and
do not grant rights to the game content shown. See the
[gallery](SCREENSHOTS.md) for context and image rights.

The file gate has a narrow exception for these PNGs: the same staged/committed
manifest must list each image, its SHA-256 must match, and the PNG structure,
size and chunk checks must pass. Unlisted images, text/EXIF payloads and other
binary files are still rejected.

Review every staged path and its provenance, retain required notices, then run:

```powershell
python tools/check_publication.py
git diff --cached --check
```

The check reads staged blob contents, so forcing an ignored file into Git does not
bypass it. CI checks committed files too. It catches common artifacts and credential
patterns; it cannot establish ownership, detect every secret, or audit historical
commits. Never upload a private Git history or generated binaries as a shortcut.
