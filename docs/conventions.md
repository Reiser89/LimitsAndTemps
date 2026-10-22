# Conventions & constraints

## C++ (`src/`)

- Keep to C++11 features. MSVC can't target C++11 directly, so the build uses `/std:c++14` (lowest available).
- Source is compiled with `/utf-8`. Use wide literals (`L"..."`) for non-ASCII text passed to `wxString`, otherwise it's decoded as the ANSI codepage.
- New source files must be added to the `cl` line in `build.bat`.
- Only `wxcore`/`wxbase` are available (see [build.md](build.md)).
- Code that runs in helper mode (`helper.cpp`, `cputemp.cpp`, `sensors.cpp` via `--dump`) must not use wx: wx is not initialized there.
- Files that include `<windows.h>` and use `std::min`/`std::max` define `NOMINMAX` first.
- Prefer sources that need no extra DLLs and no admin; anything that loads a large library (e.g. NVML) is a fallback only.

## General

- Match the surrounding code: comment density, naming, idiom.
- Comments explain why, not what; no authoring or tooling remarks in code or docs.
