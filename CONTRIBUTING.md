# Testing and contributing

Start with [the local build guide](docs/BUILDING.md). Use the playtest issue form
for results and the setup issue form if you cannot build or prepare shaders.

Include small reproduction steps and identify your commit and settings. Keep
performance comparisons in the same scene and distinguish cold shader compilation
from steady play. Do not upload game content, shader programs, generated game code,
saves or memory dumps. Trim identifying paths from any log excerpt you choose to post.

Contributions to original project code use GPL-3.0-only. Preserve existing
third-party notices and document the source/license of newly introduced code.
Do not copy code or comments from a project merely because its repository is public.

Stage specific reviewed paths, run `python tools/check_publication.py`, and inspect
`git diff --cached` before committing. Include a focused test or reproduction for
behavioral changes. Keep experimental renderer switches off until their results
and fallback behavior are understood.
