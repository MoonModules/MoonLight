# Code hosting platforms compared (2026-10-08)

Four homes for MoonLight compared: GitHub, where it lives, [Codeberg](https://codeberg.org), a self-hosted [Forgejo](https://forgejo.org), and [GitLab.com](https://gitlab.com).
Their content rules come first, since they decide whether MoonLight may be hosted at all; then each GitHub feature MoonLight depends on, with its counterpart on the other three.

## The overview

| | GitHub | Codeberg | Self-hosted Forgejo | GitLab.com |
|---|---|---|---|---|
| Allows an agent-written project | yes | no, Terms §2(1)7 | yes, our own rules | yes, no rule on it |
| Run by | Microsoft | a German non-profit | us | GitLab Inc. |
| Hosted CI runners | Linux x86 and arm64, macOS, Windows | Linux x86 on request (Woodpecker) | none: self-hosted only | Linux x86; macOS and Windows in beta |
| Release API as MoonLight reads it | yes | Gitea shape | Gitea shape | its own shape |
| Static site with custom domain | Pages | Codeberg Pages | any static host beside it | GitLab Pages |
| Container registry | ghcr.io | Forgejo packages | Forgejo packages | GitLab registry |
| Code scanning | CodeQL | none built in | none built in | SAST, with Ultimate |
| CodeRabbit review | yes | no | no | yes |
| Cost for MoonLight | free for public repos | free, donation-funded | a server and its upkeep | free; the open-source program adds Ultimate |

## Content rules

**GitHub** has no rule on how code is written.

**Codeberg**'s [Terms of Use](https://codeberg.org/Codeberg/org/src/branch/main/TermsOfUse.md), §2(1)7, adopted by vote on 2026-07-22:

> You must not share projects that mostly consist of code written by "generative AI"-tools (including services such as Claude, OpenAI Codex).

The [blog post](https://blog.codeberg.org/protecting-our-floss-commons-from-llms.html) explaining the vote names as unwelcome *"projects written and maintained with heavy use of LLMs"* and *"projects created by LLM 'agents' in autonomous ways"*, and as welcome *"projects who have an active community"* and *"projects with a significant pre-LLM history"*.
Enforcement is by human moderators, with no automatic scanning.
MoonLight is written by agents and says so, in the roles of [CLAUDE.md](../../../CLAUDE.md#roles) and in [why we write our own code](../../explanation/why-we-write-our-own.md#why-this-became-possible), which places it under §2(1)7.
The two exceptions are uncertain ground: the community is small, and the pre-LLM history belongs to WLED-MM rather than to this repository.
The same terms bar cryptocurrency projects, which does not concern MoonLight.

**A self-hosted Forgejo** runs the software Codeberg runs, under rules we set.

**GitLab.com** has no rule on how code is written.
Its [open-source program](https://about.gitlab.com/solutions/open-source/join/) gives Ultimate features and 50,000 compute minutes to a project that is public, under an OSI-approved license, and run without selling services or add-ons.

## What MoonLight depends on

Each GitHub feature MoonLight uses, and what replaces it elsewhere.
Plain git moves unchanged: the repo-health and KPI scripts, MoonCloud on Cloudflare, and the Discord links.

| GitHub | MoonLight uses it for | Codeberg and Forgejo | GitLab.com |
|---|---|---|---|
| Actions, 4 workflows | build, test, release, docs deploy | Forgejo Actions reads GitHub-style workflow files; most `actions/*` steps run | `.gitlab-ci.yml`, a rewrite of the four workflows |
| Hosted runners: Linux x86 and arm64, macOS, Windows | the desktop builds: `.deb` on both architectures, `.dmg`, `setup.exe` | Codeberg's Woodpecker is `linux/amd64` only and *"provided as-is"*; the others need self-hosted runners | macOS and Windows in beta, outside the service-level agreement; arm64 needs a self-hosted runner |
| Releases and `api.github.com` | the installer, the UI's update badge and OTA picker, MoonBase's recovery page and the Dockerfile read releases, the release `name` and the `prerelease` flag | the Gitea-shaped releases API: similar fields, different URLs | the Releases API, with assets as links to files in a package registry |
| `raw.githubusercontent.com` | MoonLive script downloads, the Gallery | `/raw/` URLs, CORS to verify | `/-/raw/` URLs, CORS to verify |
| Pages, custom domain | moonmodules.org/MoonLight and the installer at `/install/` | Codeberg Pages from a `pages` branch or an Actions build; with self-hosting, any static host | GitLab Pages |
| ghcr.io | the Docker image | the package registry | the container registry |
| CodeQL and the Security tab | static analysis, read by `check_codeql.py` | CodeQL CLI or Semgrep in CI, the report kept in the repository | SAST in CI, with Ultimate |
| CodeRabbit | review of every pushed commit | the Reviewer agent instead | CodeRabbit |
| The web editor's fork flow | the "contribute a script" button | Forgejo's web editor, a different URL shape | GitLab's Web IDE, a different URL shape |
| Personal access tokens and an API | scripts and agents | tokens under Settings → Applications | personal and project access tokens |

On every platform, three readers of the release API need an adapter for its shape: the UI, MoonBase and the installer.
The ESP32 OTA path's redirect and TLS handling also needs checking against the new host.
The URLs in the docs, the README, `mkdocs.yml` and the packaging metadata are a search and replace, and the privacy policy names GitHub as a data recipient.

## The deciding costs

Any move pays the same three costs:
- **Runners:** macOS, Windows and arm64 for the desktop builds, hosted on GitHub alone.
- **The release API:** read by the firmware, MoonBase and the installer, with its shape followed exactly on GitHub alone.
- **Review:** CodeRabbit, which reaches GitHub and GitLab.
