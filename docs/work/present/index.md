# Present: what we are building

One file per plan being built now, as `Plan-YYYYMMDD - <title>.md`.

A plan is written in plan mode, approved before work starts, and edited as reality changes. **It leaves at its PR**: the PR description links it, and the file moves to [past/plans](../past/index.md) in that same PR ([CLAUDE.md, Branch](../../../CLAUDE.md#main-and-branch)). The plan is then the design record beside the merged PR, which carries the diff it describes.

A file here that no longer has work behind it is either finished, and belongs in past, or abandoned, and moves there as `(cancelled)`.

Inside a plan each step carries its status: ✅ built and verified, 🚧 partly built, with what is missing named beside it, ⏳ not started.

A file name counts them, `(3 of 7)` for three steps ✅ out of seven, when the product owner asks for the count, since a renamed file is a new file to git; `(7 of 7)` is a plan whose every step is built and verified, the judgment steps included, ready for the product owner to move to [past](../past/index.md).
