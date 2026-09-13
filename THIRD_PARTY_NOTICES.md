# Third-party notices

BoltsRebuilt's original contributions are licensed under GPL-3.0-only. This does
not replace the licenses or copyright notices on pre-existing third-party work.

| Component | Source | License and scope |
| --- | --- | --- |
| RexGlue SDK v0.10.0 | https://github.com/rexglue/rexglue-sdk/tree/v0.10.0 | BSD-3-Clause for the SDK's own code; separately downloaded subcomponents have their own terms. |
| Xenia-derived graphics/runtime portions | https://github.com/xenia-project/xenia | BSD-3-Clause notices retained in the corresponding RexGlue/vendored source. |
| SDL GameControllerDB | https://github.com/mdqinc/SDL_GameControllerDB | zlib; complete notice at `third_party/sdl_gamecontrollerdb/LICENSE`. |
| Numerical compatibility research | https://github.com/masterspike52/reNut | Attribution, not a grant of license. See `docs/PROVENANCE.md`; original imported files are excluded. |

The complete RexGlue/Xenia notice accompanying the selected SDK revision is in
`LICENSES/RexGlue-BSD-3-Clause.txt`. Source patches in `sdk-patches/` modify that SDK;
preserve upstream notices when applying or redistributing them. SDK-generated
build integration retains its upstream origin. Modified vendored files document
their changes in `src/gpu/vendored/UPSTREAM.md`.

The SDK's dependency tree includes components with different licenses, including
FFmpeg and libmspack. A source-only preview that downloads them does not establish
permission or compliance for redistributing a prebuilt dependency bundle. Review
the exact build configuration and corresponding-source obligations before any
future binary release.

Banjo-Kazooie: Nuts & Bolts and its game content belong to their respective rights
holders. The project license grants no rights to that content or to their trademarks.
This project is not affiliated with or endorsed by Rare or Microsoft.

The documentation screenshots show copyrighted game artwork and trademarks;
they are not licensed under GPL-3.0-only by this project. See the
[screenshot gallery](docs/SCREENSHOTS.md) for their capture context and provenance.
