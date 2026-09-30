# libsdl3-sunxifb: fork policy for AI-assisted work

This repository is PocketForge's owned fork of SDL3 (vendored at `sdl3/`) with the
`sunxifb` video backend and PocketForge's KMSDRM changes. Cross-repo doctrine lives in
[pocketforge-os/mission-control](https://github.com/pocketforge-os/mission-control) AGENTS.md.

## Scope of `sdl3/AGENTS.md`

`sdl3/AGENTS.md` is upstream SDL's contribution policy, vendored unchanged with the SDL
tree. It governs contributions submitted to the upstream project (libsdl-org/SDL). It does
**not** govern changes made in this fork.

**Owner decision (2026-09-30):** AI/LLM-generated code is permitted in this fork. The
owner's words: "We will be using AI to generate code for this project, obviously ... This
is our fork. We can do what we want here."

## Rules for fork changes

- Disclose AI assistance on every commit with an `Assisted-by: LLM` trailer (never a
  `Signed-off-by` added by an agent).
- Never submit fork changes to upstream SDL as-is. An upstream submission must follow
  `sdl3/AGENTS.md` (human-authored, under the Zlib license).
- Do not edit `sdl3/AGENTS.md`: it stays byte-identical to the vendored upstream file.
