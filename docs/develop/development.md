
# Development

See [Standards and Guidelines](standardsguidelines.md) for coding rules, code style, and guidance on using AI tools (optional) before contributing.

---

## Pull Requests

Complete workflow: create branch → create PR → make changes → merge.

Found a bug while developing? See [Reporting Issues](../moonlight/overview.md#reporting-issues) for how to document and report it properly.

### Creating a PR

**Step 1: Create a feature branch** from `main` — always target `main`.

**Step 2: Make your first commit**

For complex tasks, optionally create an implementation plan in [Work in progress](workinprogress.md):

- **Option A (for simpler tasks)**: Skip planning, or write a brief plan yourself and commit to `workinprogress.md`.

- **Option B (if you prefer)**: If you'd like help planning, you can ask an AI agent (Claude Code or Mistral) to draft a detailed plan. The result is an updated `workinprogress.md` — commit that to your branch before writing code.

  **Good times to ask an AI for a plan:**
  - Adding a new module or feature area
  - Cross-cutting refactors affecting multiple files
  - Complex architectural decisions

  **Prompt template (if using AI):**

  ```
  Create a detailed implementation plan for [task description], outlining:
  - The steps involved
  - Affected files and modules
  - Architectural decisions
  - Any risks or edge cases

  Add this plan to /docs/develop/workinprogress.md and refer to it in your commits.
  Check the Work in Progress document to see current plans, their status, and phases.
  ```

Make changes to the code base, see [Make changes](#make-changes)

**Step 3: Open the PR**

1. Push the first commit to your branch
2. Create a Pull Request with an **empty description** — CodeRabbit will analyze your commits and auto-generate one
3. Review CodeRabbit's description — optionally edit it before requesting review

### Updating an open PR

Push additional commits to your branch — GitHub keeps the PR up-to-date automatically.

!!! warning "Do not force-push while your PR is open"
    Force-pushing causes review comments to disappear and has other subtle side effects on the repository history.

See [Make changes](#make-changes) for how to edit frontend/backend code, format, and commit.

**If you'd like to use AI for code changes:**

If you choose to use Claude Code or Mistral Vibe, see [AI Context for contributions](standardsguidelines.md#ai-context-for-contributions) for tips on providing the right context to get helpful results.

**Address feedback:**

1. **CodeRabbit's recommendations** — address in subsequent commits
    - Use icons in commit title/description to identify the source: 🐰 (CodeRabbit), ✴️ (Claude), 🐱 (Mistral Vibe)
    - Example: `🐰 issues solved by ✴️` (CodeRabbit issues fixed by Claude Code)
    - CodeRabbit provides [ready-to-use prompts for AI agents](standardsguidelines.md#using-coderabbit-s-ai-prompts) if you'd like help addressing its recommendations
    - Commit description: if you use AI to fix issues, you can use its summary — it provides a clear summary of the changes
2. **Code review feedback** — address per reviewer comments
3. **Ensure docs are updated** in your commits, AI can help here.

### Merging a PR

**Keep PRs moving** — target **a week or less** from opening to merge. Stale PRs slow down development and cause conflicts.

Before merging:

1. Ensure all documentation is updated
2. Ask CodeRabbit for a final review:

```
@coderabbitai, I am about to merge this PR. Please produce three outputs:

1. **PR review** — in-depth review of all commits: a concise summary of what changed
   and why, a merge recommendation, and a prioritised list of follow-up actions.
   For the most urgent items (blockers or high-risk changes), include a ready-to-paste
   prompt that a Claude Code agent can execute immediately before merge.

2. **End-user docs prompt** — a ready-to-paste prompt for a Claude Code agent to update
   `/docs`. Rules: only describe usage implications (what changed for the user);
   no internals, no code, no architecture; check existing pages before adding —
   update in place rather than duplicating; keep additions compact and user-friendly.

3. **Developer docs prompt** — a ready-to-paste prompt for a Claude Code agent to update
   `/docs/develop`. Rules: target contributors, not end users; be concise — if the
   detail is already in the code or commit messages, do not repeat it; focus on
   decisions, patterns, and guidance that are not obvious from reading the source.
```

After merging, **delete the feature branch** — it keeps the repository clean and is always recoverable via git if needed.

---

## Make changes

Instructions for editing and committing code (used while [updating an open PR](#updating-an-open-pr)).

### Frontend (UI)

- Files live in `interface/src/routes/moonbase/` (MoonBase and MoonLight modules)
- See [Prepare for development](installation.md#prepare-for-development) for Node.js setup

```bash
npm install
npm run dev
```

See [Troubleshooting](installation.md#troubleshooting) for common issues.

**UI development server:** Deploy backend to an ESP32 and proxy API calls through the local dev server — no reflashing needed for UI changes. After setup, open [localhost:5173](http://localhost:5173/).  
See [Setup Proxy for Development](installation.md#prepare-for-development) for details.

!!! tip "Node.js"
    If Node.js is not installed: see [Prepare for development](installation.md#prepare-for-development).

### Backend (Server)

Three levels to add functionality:

| Level | Where | Use when |
|-------|-------|----------|
| **ESP32-SvelteKit standard** | `lib/` | Connections, WiFi, System — do not modify |
| **[MoonBase Modules](../moonbase/overview.md)** | `src/` | New modules: subclass `Module`, implement `setupDefinition`, `onUpdate`, optional `loop` |
| **[MoonLight Nodes](nodes.md)** | `src/MoonLight/nodes/` | New effects, layouts, modifiers, drivers — easiest approach, closest to WLED usermods |

### Steps

1. Edit the file(s), build with ☑️ and/or upload with ➡️
2. Format code before committing: right-click → **Format Document** (clang-format for C/C++, Prettier for frontend)
3. Clear browser cache if UI changes are not visible
4. Commit with a descriptive message and extended description
5. Update documentation in `/docs` alongside code changes

---

## Additional reference

### Emoji coding

Serial log shows which library the output is from:

| Emoji | Library |
|-------|---------|
| 🐼 | ESP-SvelteKit |
| 🔮 | PsychicHTTP |
| 🐸 | Live Scripts |
| 🌙 | MoonBase |
| 💫 | MoonLight |

`🌙` and `💫` are also used in code comments to mark changes to upstream ESP32-SvelteKit files.

### Release and merged firmware binaries

Firmware binaries come in two flavours:

| Type | Folder | Used by | Flash offset |
|------|--------|---------|--------------|
| Merged (boot + partition + firmware) | `build/merged/` | [MoonLight Installer](../gettingstarted/installer.md) | `0x0` |
| Release (firmware only) | `build/release/` | [System update](../system/update.md) (OTA) · [GitHub releases](https://github.com/MoonModules/MoonLight/releases) | `0x10000` |

All partition schemes have a 3 MB firmware size. ESP32-D0 has no OTA partition — system update works but has no fallback if it fails (flash via USB in that case).

!!! tip "Flash firmware using esptool"
    ```bash
    esptool --port /dev/cu.usbserial-1130 write-flash -b 2000000 0x0 ./build/merged/MoonLight_esp32-s3-n16r8v_0-6-1_webflash.bin
    ```
    Replace port and filename to match your setup. Optionally add `erase-flash` before `write-flash`. Use `0x10000` and the `build/release/` binary to flash only the MoonLight partition.

### Adding an ESP32 device definition

Before starting, ensure you have the datasheet confirmed. Many modules have near-identical markings that can hide varying hardware.

Three files to create or modify:

```
boards/BOARD_NAME.csv
boards/BOARD_NAME.json
firmware/BOARD_TYPE_NAME.ini   (e.g. esp32dev, esp32-s3 — one file may contain multiple boards)
```
