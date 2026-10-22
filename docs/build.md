# Build & run

```
build.bat                                   # -> build\LimitsAndTemps.exe (x64 Release, /MT, LTCG)
build\LimitsAndTemps.exe                    # or: build\LimitsAndTemps.exe --tray
build\LimitsAndTemps.exe --dump out.txt     # all sensor readings + agent limits as text; run as admin to include the CPU
```

- `build.bat` calls Visual Studio 18 Professional's `vcvars64.bat`. Its `'vswhere.exe' is not recognized` message is harmless.
- `build.bat` downloads `third_party\PawnIO_setup.exe` (PawnIO.Setup 2.2.0) and `third_party\PawnIO.Modules\{IntelMSR,AMDFamily10,AMDFamily17}.bin` + `COPYING` (PawnIO.Modules 0.2.11, LGPL-2.1) if missing, and always checks them against pinned SHA-256 hashes. Updating either means changing URL and hashes together.
- The app version lives only in `srcersion.h` (`APP_VERSION_*`); bump it there. It feeds the exe's version resource (`app.rc`), the first line of the tray menu and the `--dump` header.
- `src\app.rc` embeds those files. `rc` runs from the project root, so paths inside `app.rc` are relative to the root (`third_party\...`).
- Testing the elevated path pops a UAC prompt. Enabling CPU sensors installs a kernel driver (PawnIO) on first use, copies the exe to `%ProgramFiles%\LimitsAndTemps\` and registers the scheduled task `\LimitsAndTemps CPU helper <SID>`; "Disable CPU temperature" (or `LimitsAndTemps.exe --unregister-task` as admin) removes the last two.
- After rebuilding, the first start asks UAC once more: the copy in Program Files no longer matches, so the helper refreshes it.
- New source files must be added to the `cl` line; new system libraries that are rarely used belong on the `/DELAYLOAD` list.
- No tests or linters.

## wxWidgets linkage

- `wxwin` env var = the wxWidgets source folder (required, no default). Its stock `lib\vc_*` builds use the `/MD` DLL runtime and **must not** be used (target PCs would need vcruntime DLLs).
- The app links against a separate static-runtime build in `%wxwin%\build_mt\lib\vc_x64_lib`, made with CMake + Ninja (VS dev shell):
  `-DwxBUILD_SHARED=OFF -DwxBUILD_USE_STATIC_RUNTIME=ON` with webview/stc/mediactrl/opengl/samples/tests off, building only target `wxcore`.
  Using another wx library (adv, aui, …) means building that target there too and adding it to `build.bat`.
- `include\msvc` is intentionally **not** on the include path: its auto-link `setup.h` points at the stock `/MD` lib dir. Instead `build_mt\lib\vc_x64_lib\mswu` provides `wx/setup.h`, and all libs are listed explicitly in `build.bat`.
- The entry point is our own `wWinMain` (`wxIMPLEMENT_APP_NO_MAIN`), so helper mode runs without initializing wx.

## Verifying the single-exe requirement

```
dumpbin /dependents build\LimitsAndTemps.exe
```

must list only Windows system DLLs (normal and delay-load).
