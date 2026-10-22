# LimitsAndTemps – project docs

Start here. These docs are the project's working memory: sessions start without prior context, so everything needed to continue work must be written down here.

LimitsAndTemps is a lightweight wxWidgets 3.3.1 tray app (Windows 10+, MSVC x64) that shows hardware temperatures (with history sparklines and max-since-start) and AI agent usage limits.

## Hard requirements

- The deliverable is a **single self-contained `LimitsAndTemps.exe`**: no DLLs or side files, only Windows system DLLs as dependencies.
- It must run on other Windows 10 PCs (no runtime installs), including old hardware (e.g. older dual-socket Xeons).
- It must handle multiple CPUs and GPUs.
- Low resource use (RAM, CPU, wakeups) is the top priority after correctness.

## Layout

```
LICENSE                GPL-3.0 text
build.bat              build script (x64 Release) -> build\LimitsAndTemps.exe
src\                   C++/wx app
  main.cpp             entry point (UI vs. helper mode), UI, tray, merging of sensor sources, agent limits
  sensors.cpp/.h       admin-free sensors (GPUs via D3DKMT/NVML, drives, ACPI)
  helper.cpp/.h        elevated helper mode + its client (launch, shared memory), PawnIO install, --dump
  cputemp.cpp/.h       CPU temperatures through the PawnIO driver (Intel, AMD Zen, AMD K10-family)
  task.cpp/.h          scheduled task + Program Files copy that start the helper without a UAC prompt
  usage.cpp/.h         AI agent limits (Claude Code, Codex) via the agents' own sign-in or Claude Code's status line, WinHTTP, mini JSON parser
  version.h            app version (single source for the version resource, tray menu and --dump)
  app.rc               icon, manifest, version resource, embedded PawnIO installer and modules
third_party\           PawnIO_setup.exe and PawnIO.Modules\ (official, signed, hash-pinned; fetched by build.bat)
docs\                  these docs
build\                 build output (git-ignored)
```

## Docs

| File | Content |
|---|---|
| [architecture.md](architecture.md) | Two-process design, sensor sources, resource budget, UI, tray, settings |
| [ipc.md](ipc.md) | Shared-memory protocol between app and helper |
| [build.md](build.md) | Building, running, wxWidgets static linkage, verification |
| [conventions.md](conventions.md) | Language/toolchain constraints and coding rules |

## Working rules

- Keep these docs current: when behavior, layout, build steps or the IPC protocol change, update the matching file in the same change.
- Write docs as facts about the project, not as a change log.
- There are no tests or linters; verify by building (`build.bat`) and, where possible, running the app (`--dump` covers the sensors without UI).
