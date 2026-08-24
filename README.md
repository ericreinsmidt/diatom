# New libretro frontend - design phase

A minimal libretro frontend for low-power Linux handhelds (~1GB RAM), intended
as a permissively licensed replacement for the GPL minarch that PlayOS
currently patches and rebuilds, and as a reusable base for future firmware
projects.

**Status: scoping closed 2026-08-23. Nine decisions recorded, no code yet.**
Currently in Phase 2 - spikes. See `docs/working-agreement.md` for phases and their exit criteria.

Ten systems across five or six cores: NES · Master System · Game Gear ·
PC Engine · PC Engine CD · Genesis · SNES · Game Boy/GBC · GBA. All 2D, all
digital input, all light enough to keep every core resident at once.

---

## Navigating this folder

| File | What it's for |
|---|---|
| `docs/working-agreement.md` | The working agreement - how we operate here, and the exit criteria for this phase. Read first. |
| `docs/scoping-register.md` | The living checklist. Every question, tagged by weight. The single source of what's still open. |
| `docs/glossary.md` | Terms, and the ones that are dangerously overloaded. |
| `docs/decisions/` | ADRs. One file per decision, with the reasoning. |
| `docs/discussion/` | Dated session logs - what we talked about and what came of it. |

## Where to start reading

1. `docs/working-agreement.md` - the rules and the definition of done
2. `docs/scoping-register.md` §0 - the seam test that governs the design argument
3. `docs/discussion/` - most recent entry, for current state of thinking

## Why this folder exists

Two goals, and the second is not secondary:

1. Design a frontend worth building.
2. Practise running a project sustainably - decisions recorded, questions
   tracked, scope defended, implementation deferred until the thinking is done.

No code exists yet, and that is deliberate - see `docs/working-agreement.md` for the exit
criteria that gate implementation.
