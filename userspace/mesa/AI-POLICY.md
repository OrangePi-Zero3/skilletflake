# The Mesa fork and AI-written code

panfrost-win's Mesa is a **hard fork** of upstream Mesa, locked at `MESA_COMMIT`. It is built
from the source tarball of that commit (checked against `MESA_TARBALL_SHA256`) with one diff
applied, `patches/panwin-mesa.diff`. The project owner decided this on 2026-10-30.

Upstream Mesa asks AI agents to stay out of some directories, `src/panfrost` and
`src/gallium/drivers/panfrost` among them (its `AGENTS.md`, and the Panfrost AI policy in
`docs/drivers/panfrost.rst`). That policy covers code submitted to Mesa. This fork submits
nothing, so it follows its own rules instead:

- The fork's diff deletes `AGENTS.md` (and its `CLAUDE.md` symlink). Changes anywhere in the
  tree, `src/panfrost` included, may be LLM-written.
- Every change is labelled: each commit on the fork branch carries `Generated-by: LLM`, and the
  diff's header lists those commits.
- Nothing from the fork goes to Mesa upstream. No merge requests, issues or comments on
  gitlab.freedesktop.org/mesa, by people or agents.
- If any of it should ever go upstream, a human rewrites it from scratch under Mesa's and
  Panfrost's policies. This diff is not a starting point for that.

Before the fork, Windows behaviour was kept outside `src/panfrost` where possible (libdrm-win,
`src/util`, compat headers, the kernel driver accepting what unmodified panvk sends). That is
still preferred when it is the cleaner fix, because it keeps the diff small and rebasing on a
newer Mesa cheap.
