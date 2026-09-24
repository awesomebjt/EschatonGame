# src/ecs — Components & Systems

Scope: EnTT component definitions (`components/`) and system implementations
(`systems/`). See root `CLAUDE.md` → "Entity Component System Design" for the
canonical component/system tables — keep this file for things that table doesn't
capture (ordering constraints, registry lifetime quirks, query performance notes).

Key constraint carried over from the world-generation rework: **static world content
is not entities.** At ~375k buildings per cylinder, only *interactive* buildings
(enterable, scripted, quest-relevant) get an EnTT entity, spawned from an override
list keyed by position on chunk load — everything else is a flat instance array owned
by the world/render layers. Don't reach for `registry.create()` for bulk world dressing.

## Decisions & Notes

_(none yet — add short entries here as design choices are made: what was decided, why,
and what alternatives were rejected)_
