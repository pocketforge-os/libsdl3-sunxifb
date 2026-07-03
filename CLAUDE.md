# libsdl3-sunxifb

SDL3 mainline + forward-ported `sunxifb` video backend (from the vendor `SDL2-2.26.1.GE8300` `mali-fbdev` patch). Ships as `libSDL3-pocketforge.so.0` — the ONE SDL3 shared library that Steam Link loads via `SDL3_DYNAMIC_API`, the launcher links against directly, and every first-party PocketForge app links against. Cross-repo doctrine lives in [`pocketforge-os/mission-control`](https://github.com/pocketforge-os/mission-control); this file is orientation only.

## Session startup + working norms

Run `bd prime` (auto-injected), `bd dolt pull`, register + background your pf-wall listener, `bd update <id> --claim`, and edit in a fresh `pf-wt create <bead-id> --repos libsdl3-sunxifb[,…]` worktree — never in `/home/matt/libsdl3-sunxifb` directly. Full checklist: [mission-control CLAUDE.md](https://github.com/pocketforge-os/mission-control/blob/master/CLAUDE.md); worktree/branch→PR→merge norm: [mission-control git-workflow rules](https://github.com/pocketforge-os/mission-control/blob/master/.claude/rules/git-workflow.md). Every change is a `<bead-id>` branch → PR → merge; no straight-to-default pushes.

## Shared Claude Code substrate

`.claude/settings.json` enables the shared `pf@pocketforge` plugin ([pocketforge-os/claude-plugins](https://github.com/pocketforge-os/claude-plugins)): skills (`/build-image`, `/close-bead`, `/file-bead`, `/flash`, `/kickoff`, `/plan-doc`, `/screen-check`, `/serial-review`), custom agents (`log-triage`, `researcher`, `screen-reviewer`), enforcement hooks (PreToolUse deny+redirect, Stop DoD gate, InstructionsLoaded audit).

## Repo-specific gotchas

- **Cross-built with ARM A-Profile 10.3-2021.07 / gcc 10.3.1 / glibc 2.33 in the pinned container** — same toolchain as the rest of the substrate. Native-glibc `.so`s break every consumer at runtime; always go through the `pf build` path via [`/build-image`](https://github.com/pocketforge-os/claude-plugins/blob/main/plugins/pf/skills/build-image/SKILL.md), not host `cmake`.
- **Tags on this repo imply ABI + dynapi commitments** — bumping the SONAME breaks Steam Link's dynapi override. Do NOT bump SONAME without also planning a Steam-Link-side check and coordinating the release with the launcher + apps repos.
