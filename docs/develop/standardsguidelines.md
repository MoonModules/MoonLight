
# Standards and Guidelines

## Development Principles

| Principle | Detail |
|-----------|--------|
| **One firmware per board** | Everything in a single binary — keeps management simple. Target ≤ 3 MB flash. |
| **Docs with every PR** | Functionality and documentation go in the same Pull Request. |
| **Minimal upstream changes** | Stay in sync with [ESP32-sveltekit](https://github.com/theelims/ESP32-sveltekit). Mark any upstream change with `// 🌙` (MoonBase) or `// 💫` (MoonLight). |
| **Main branch is protected** | No direct code commits to `main`. Branch → PR → merge. Doc-only changes may go directly to `main` (docs folder = website source). |
| **Compilable PRs** | Every PR must compile and not crash the system. Must support boards without PSRAM (e.g. ESP32-D0). Work-in-progress code is fine if it compiles. |

### Where code lives

```
src/          MoonBase + MoonLight nodes and modules (backend)
lib/          Upstream SvelteKit (do not modify)
interface/    Frontend — SvelteKit + generic module/node UI
```

The UI for modules and nodes is **generated automatically** — no module-specific Svelte code is needed.

---

## Artificial Intelligence

MoonLight supports AI tooling. Opinions on AI vary — we use it to stay informed, document what we learn, and adjust as we go.

> **Most important principle: MoonLight must never depend on AI.**  
> Every workflow must remain fully functional without any AI tool.

### AI Principles

Five principles govern all AI use: **4EP · Unit tests · Reversible · Attribution · Documentation**

---

**4 Eyes Principle (4EP)**

AI-generated code must always be reviewed by a human before it lands in the repo:

1. Developer reviews Claude Code output 👀 before committing to a feature branch
2. CodeRabbit automatically reviews each commit 👀 in the PR
3. Developer processes and resolves CodeRabbit's findings

---

**Unit Tests**

Unit tests are required for both AI-generated and human-written code.

| Layer | Tool | Where |
|-------|------|--------|
| Backend — static analysis | cppcheck | `platformio.ini` (compile) · `lint.yml` (CI) |
| Backend — unit tests | doctest | `test/test_native/` · `pio test -e native` |
| Frontend — unit tests | Vitest | `*.test.ts` co-located with source · `npm run test` |
| Frontend — style | ESLint + Prettier | `lint.yml` CI |
| Experimental | IWYU | `lint.yml` (`continue-on-error: true`) |

All checks run in CI: [lint.yml](https://github.com/MoonModules/MoonLight/blob/main/.github/workflows/lint.yml)

---

**Reversible**

- Development must never depend on any AI tool
- Committing without AI assistance must always be possible
- Any AI tool can be replaced or removed at any time

---

**Attribution**

AI models are trained on third-party code and research. When that knowledge surfaces in MoonLight, document the source — link to the GitHub repo, algorithm paper, or website the AI drew from.

---

**Documentation**

AI-generated code must be documented to the same standard as human-written code.

---

### Tooling: with or without AI

You're free to contribute **with or without AI tools**. Both approaches are fully supported.

**Without AI:** You can write, review, and commit code entirely on your own using your favorite editor and tools. This is fast for small changes and gives you complete control.

**With AI:** Tools like Claude Code or Mistral can help with planning, writing code, and addressing review feedback. This is useful for complex tasks, exploring unfamiliar areas, or getting a second opinion.

**Most developers use both** — writing some code manually, using AI for other parts, and letting CodeRabbit guide the process. Pick what works for you.

---

### Contributing with AI

If you use AI tools, remember that you're ultimately responsible for the code. Take time to understand and review what the AI produces:

- **Understand it** — don't just accept output because it "seems to work"
- **Review changes to existing code carefully** — AI can silently drop comments or break subtle logic
- **Verify technical accuracy** — AI is good with language, but technical terms and concepts can sometimes be wrong

Mark larger AI-generated sections with a comment:
```cpp
// Below section generated with AI assistance
```

#### Using CodeRabbit's AI prompts

When CodeRabbit reviews your PR, it may provide **Prompt for AI agents** — ready-to-use prompts for Claude Code or Mistral. If you'd like help addressing feedback, you can copy these prompts directly into your AI tool. They can help with:

- Fixing code style issues
- Improving documentation
- Resolving failing tests
- Addressing other feedback

Of course, you're also welcome to address feedback manually — just do what works best for you.

#### AI Context for contributions

If you use AI tools to contribute, it helps to give them context about MoonLight:

- **Claude Code**: Reference [CLAUDE.md](CLAUDE.md) — it has an overview of the architecture, build process, coding rules, and project structure. Share this file with Claude Code when asking for help with significant contributions.

- **Mistral Vibe**: Mistral doesn't have a dedicated context file. Instead, you can share relevant excerpts from this document, the [AI Models](#ai-models) section, or other architecture docs in your prompt as needed.

---

### AI Models

> **Status: April 2026.** AI models evolve rapidly — re-evaluate model choices every few months.

MoonLight uses two AI coding tools: **Claude Code** (Anthropic CLI) and **Mistral** (vibe coding via Le Chat / API).

#### Claude Code

Claude Code is a CLI tool (`claude`) that integrates with your terminal and editor.

| Model | ID | Speed | Use when |
|-------|----|-------|----------|
| **Opus 4.6** | `claude-opus-4-6` | Slower | Hardest tasks: architecture, cross-cutting refactors, complex debugging |
| **Sonnet 4.6** | `claude-sonnet-4-6` | Fast | Default workhorse — 90% of tasks |
| **Haiku 4.5** | `claude-haiku-4-5-20251001` | Fastest | Quick lookups, single-line fixes, bulk repetitive edits |

Switch model with `/model <model-id>` inside a Claude Code session. Enable Fast mode with `/fast` (same Opus model, faster output).

#### Mistral

Mistral's code generation is available via [Le Chat](https://chat.mistral.ai) or the Mistral API.

| Model | Use when |
|-------|----------|
| **devstral-2** | General-purpose coding tasks, debugging, and documentation — solid alternative to Claude Code |

#### Task → model guide

**Add a new node (e.g. a new effect `E_MyEffect.h`)**

> Complexity: medium — mostly self-contained  
> Model: **Sonnet 4.6** (Claude) · **devstral-2** (Mistral)

```
Create a new effect node E_MyEffect in src/MoonLight/Nodes/Effects/.
Base it on an existing simple effect like E_Sinelon.h.
The effect should [describe behaviour].
Add controls for [list controls].
Register it in ModuleEffects.
```

---

**Add a new module (e.g. `ModuleFoo`)**

> Complexity: high — touches backend state, HTTP/WS endpoints, and the generic UI  
> Model: **Opus 4.6** (Claude) · **devstral-2** (Mistral)

```
Create a new MoonBase module ModuleFoo following the pattern in src/MoonBase/.
State fields: [list fields and types].
Add loop20ms() that [describe periodic work].
Register it in main.cpp alongside existing modules.
Show how the generic UI will render it.
```

---

**Change a node implementation (tweak an existing effect/layout/driver)**

> Complexity: low–medium — scoped to one file  
> Model: **Sonnet 4.6** (Claude) · **devstral-2** (Mistral)

```
In src/MoonLight/Nodes/Effects/E_Plasma.h, change the speed control
from a float [0–1] range to an int [1–255] range.
Update the loop() calculation accordingly.
Do not change anything outside this file.
```

---

**Change a module implementation (tweak state, controls, or loop logic)**

> Complexity: medium — may affect UI rendering and persistence  
> Model: **Sonnet 4.6** (Claude) · **devstral-2** for cross-module impact

```
In ModuleDrivers, add a new boolean field `autoRestart` to the state struct.
Default to false. When true, restart the driver node if loop() hasn't produced
a frame in 5 seconds. Persist it via FSPersistence.
```

---

**Change the UI (SvelteKit frontend)**

> Complexity: medium — must not touch upstream files  
> Model: **Sonnet 4.6** (Claude) · **devstral-2** (Mistral)  
> Key constraint: only edit files under `src/routes/moonbase/`, `src/lib/components/moonbase/`, `moonbase_utilities.ts`, `moonbase_models.ts`

```
In interface/src/lib/components/moonbase/FieldRenderer.svelte,
add rendering support for control type "color" (currently unhandled).
Render it as an <input type="color"> bound to the field value.
Follow the Svelte 5 runes pattern used in the rest of the file.
Do not reformat any surrounding code.
```

---

**Debugging a crash / hard-to-reproduce bug**

> Complexity: high — needs full context  
> Model: **Opus 4.6** (Claude) · **devstral-2** (Mistral) with image if you have a screenshot of the stack trace

```
The ESP32 crashes with the following stack trace (decoded):
[paste decoded trace]
The crash happens after [describe steps].
Relevant files: [list files].
Identify the root cause and suggest a minimal fix.
```

---

**Writing or extending unit tests**

> Model: **Sonnet 4.6** (Claude) · **devstral-2** (Mistral)

```
Add doctest unit tests for the function fastDiv255() in
src/MoonBase/utilities/PureFunctions.h.
Tests go in test/test_native/test_utilities.cpp.
Cover: zero, one, mid-range, max (255), and a known rounding boundary.
Do not copy the function — include the header.
```

---

## Code Style

### Formatting

| Layer | Tool | When to run |
|-------|------|-------------|
| C/C++ | Clang-format (see `.clang-format`) | Right-click → Format Document before every commit |
| Frontend (MoonLight files only) | Prettier | `npm run format` from `interface/` |

Do **not** run Prettier on upstream frontend files — they are excluded in `.prettierignore` to avoid merge conflicts.

### Comments

Every function must have a comment describing what it does and what its arguments mean. Comments are formatted by Clang-format / Prettier along with the code.

```cpp
// Single-line comment.

/* Inline CSS comment */

/*
 * Multi-line comment — used for file headers and function explanations.
 * Wrap at the width of your editor window.
 */
```

```html
<!-- HTML comment -->
```

Inline comments are fine when they describe only that line and are not too wide.

*(Adapted from WLED-MM — 🙏 @softhack007)*
