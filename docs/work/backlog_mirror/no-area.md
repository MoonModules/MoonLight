# Backlog: No area

Generated from [the Project](https://github.com/users/MoonModules/projects/5) by `moondeck/report/project_mirror.py`: edit the cards, not this file.

## Across modules

### Coding standards: an extracted hot-path function stays inline

[Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=300204509)

One sentence in the hot path section of docs/contributing/coding-standards.md: a function extracted from a per-light loop, to satisfy the complex-function rule, stays inline in the same header, and the target's `-O2 -S` shows no call left; where one is, `[[gnu::always_inline]]` on that helper. The section already teaches the probe ("A host benchmark ranks nothing for Xtensa"); this names the refactor as a time to run it. From the product owner, 2026-11-10, for the next commit.

### Render-path sweep: blocking-call findings (198) down

[Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=300209744)

The code check's "blocking call on the render path" counts call sites the compiler cannot prove nonblocking: 198 on 2026-11-10. A sweep, its own change: (1) mark the leaf platform functions that provably never block (platform::freeHeap, getMacAddress, Scheduler::elapsed and their kind) MM_NONBLOCKING, which drops the unproven-but-harmless findings; (2) list what remains, the real network and file work on the render task (HttpServerModule's serveConnections, drainSends and pushStateToWebSockets in tick20ms/tick1s; Discovery's sockets in tick1s, the mDNS browse and the added-IP probe among them), and move it to its own task, as the encode worker is for drivers. From the product owner, 2026-11-10.
