# Docs

Engineering reference for the HavenCore satellite firmware. Each doc
has a single purpose; pick the one that matches what you're trying to
do. The top-level [`README.md`](../README.md) is the project pitch and
quick-start; this directory is the deep-end.

## Reading order

If you're new to the codebase, read in this order:

1. [`ARCHITECTURE.md`](ARCHITECTURE.md) — the 10-minute mental model:
   hardware, components, boot sequence, turn-level FSM, HTTP
   contracts, UI.
2. [`PROVISIONING.md`](PROVISIONING.md) — how a fresh device becomes a
   working satellite (UF2 mass-storage flow + esptool appendix).
3. [`ROADMAP.md`](ROADMAP.md) — what's landed, what's known-broken,
   and what's intentionally deferred.

Then dive into a topic doc as needed.

## Topic docs

| Doc | What's in it | When to open it |
|-----|--------------|-----------------|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Hardware, component graph, boot sequence, turn FSM, HTTP contracts, UI | First read; anything cross-cutting |
| [`AUDIO.md`](AUDIO.md) | Wake-word (microWakeWord), simple_vad, listen-window endpointing, follow-up window onset rules and pitfalls | Touching the audio path, AEC, barge-in, VAD tuning |
| [`OTA.md`](OTA.md) | OTA push/pull, sidecar version-skip, auto-publish, partition layout, factory boot trick, gotchas | Anything OTA, partition table, recovery flow |
| [`PROVISIONING.md`](PROVISIONING.md) | UF2 mass-storage flow, CONFIG.INI keys, esptool/CSV recovery appendix | Bringing up a new board, re-provisioning, mass deploy |
| [`SETTINGS.md`](SETTINGS.md) | NVS schema, Settings screen row pattern, recipe for adding a new user-editable setting end-to-end | Adding/editing any setting that lands in NVS |
| [`ROADMAP.md`](ROADMAP.md) | Current status, known issues, deferred work, MVP checklist | Planning the next chunk of work |

## Where to put new docs

- **Subsystem deep-dive** (e.g. WebSocket client, AEC) → its own
  topic doc, lowercase or uppercase to match the existing convention
  (uppercase). Add a row to the table above.
- **How-to / runbook** (e.g. mass-flashing, log capture) → a new
  topic doc; keep `PROVISIONING.md` focused on the UF2 + esptool
  bring-up path.
- **Engineering post-mortem / hard-won lesson** → fold into the topic
  doc that owns the affected subsystem (see the `Follow-up window`
  section in `AUDIO.md` or the `Gotchas` section in `OTA.md` for the
  pattern). Don't park war stories in `ROADMAP.md`.
- **Project status / planned work** → `ROADMAP.md`.

## Cross-doc conventions

- Use relative links between docs (`[`OTA.md`](OTA.md)`), not absolute
  paths.
- When a section starts to outgrow its host doc, extract it to a new
  topic doc and leave a one-line pointer behind.
- Prefer fewer, focused docs over many tiny ones. The bar for a new
  doc is "covers a topic that's growing or referenced from multiple
  places."

## Writing guidelines

Style notes, derived from the docs that work well today.

**Structure every topic doc the same way.** Open with a 1–2 sentence
purpose statement, then a `Related docs:` block with relative links to
adjacent topics, then headings. The reader should know in five seconds
whether they're in the right doc.

**Capture the *why*, not just the *what*.** A doc that lists what the
code does is redundant with the code. The doc earns its place when it
explains hidden constraints, design trade-offs, or war stories. Good
examples already in-tree: the `app, test` subtype trap in `OTA.md` §
Gotchas; the `simple_vad_reset()` footgun in `AUDIO.md`. If a future
contributor would have made the same mistake without the note, write
the note.

**Reference code with file paths (and line numbers when stable).**
Prefer `main/app/app_sr.c:FOLLOW_UP_SILENCE_FRAMES_REQ` over "the
follow-up constant in app_sr." Identifiers survive renames better than
prose summaries.

**Use the right block for the job.**

- ASCII pipeline diagrams in fenced ` ``` ` blocks (see the turn flow
  in `ARCHITECTURE.md`, the pipeline tasks in `AUDIO.md`).
- Tables for schemas, contracts, partition layouts (`SETTINGS.md`'s
  schema table, `OTA.md`'s flash layout, `ARCHITECTURE.md`'s HTTP
  contracts).
- Plain prose for everything else. Avoid bullet lists where one
  sentence works.

**Keep voice consistent.** Conversational, dense, present tense.
"Settings → factory-reset already works end-to-end" beats "The
factory-reset functionality has been implemented." Don't apologize for
limitations; state them and move on.

**Where new content goes.**

| You learned… | Put it in… |
|--------------|-----------|
| A new subsystem behavior or contract | The topic doc that owns that subsystem |
| A non-obvious gotcha / post-mortem | The same topic doc, in a `Gotchas` or named subsection — **never** in `ROADMAP.md` |
| A planned feature or known bug | `ROADMAP.md` |
| A new NVS key | The schema table in `SETTINGS.md`, the example in `PROVISIONING.md`, and the recipe section if it's user-editable |
| A new partition or boot-flow change | `OTA.md` (and a one-line cross-ref in `ARCHITECTURE.md` § Boot sequence if relevant) |
| A bring-up step that isn't UF2 or esptool | A new topic doc — `PROVISIONING.md` is reserved for the canonical bring-up paths |

**When a deferred item lands.** Move the explanation into its topic
doc (with the *why* + any non-obvious lessons), then delete the
section from `ROADMAP.md`. The roadmap shouldn't grow indefinitely.

**Date-stamp landings, not status sentences.** `ROADMAP.md` § Current
status uses absolute dates (`2026-04-29`) for shipped work so it's
still readable a year later. Don't write "shipped last week."
