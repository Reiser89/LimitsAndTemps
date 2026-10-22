# Architecture

Two processes of the same exe, because CPU temperature needs admin rights plus a kernel driver, while everything else must work without admin.

`wWinMain` (`main.cpp`) dispatches on the command line before wx is initialized:

- `--helper ...` → `RunHelper` (elevated helper, no UI, no wx)
- `--unregister-task` → `HelperTask::Unregister` (elevated; removes the start task, see below)
- `--statusline [--pass]` → `RunStatusLine` (run by Claude Code as its status-line command; saves the agent limits, see below)
- `--dump <file>` → `RunDump` (writes all readings and agent limits as text and exits; CPU only when run as admin; for diagnosing other PCs)
- anything else → the wx app (`--tray` starts hidden)

## Resource budget

Measured on a Windows 10 laptop with one Intel CPU and one NVIDIA GPU:

| | Private memory | CPU |
|---|---|---|
| App (wx UI) | ~7 MB | ~0.1 % of one core while the window is visible, unmeasurably low while in the tray |
| Helper | ~4 MB, 1 thread | only works when asked; idle otherwise |

Rules that keep it there:

- No .NET, no LibreHardwareMonitor: CPU sensors are read natively through PawnIO.
- No NVML on current drivers: GPU temperatures come from D3DKMT, which costs nothing; NVML (≈16 MB once initialized) is only loaded for NVIDIA drivers older than WDDM 2.4.
- Sampling every 2 s while the window is on screen, every 5 s while hidden or minimized.
- Drives are queried every 30 s and never while spun down (each query is a command to the drive and could wake it).
- Agent limits are fetched every minute at most (15 min while hidden), on a short-lived thread; Claude's come from a local file when its status line is connected.
- `pdh.dll` (ACPI fallback), `winhttp.dll` (agent limits) and wx-only DLLs are delay-loaded, so they are never mapped unless used.
- The tray tooltip is only pushed to Explorer while the pointer hovers over the icon.

## LimitsAndTemps.exe UI process (unelevated)

### Sensors (`sensors.cpp`)

Admin-free native sources, discovered once at startup:

- **GPUs** via D3DKMT adapter perf data (`KMTQAITYPE_ADAPTERPERFDATA`, the source of Task Manager's GPU temperature; any vendor, WDDM 2.4+ drivers). Software and display-only adapters are skipped. A powered-down GPU reports 0 °C; the UI shows "sleeping". If a query fails (driver update/reset) the adapters are re-enumerated 30 s later.
- **Old NVIDIA drivers** (no WDDM 2.4 perf data) fall back to dynamically loaded `nvml.dll` for all NVIDIA GPUs.
- **Drives** via `IOCTL_STORAGE_QUERY_PROPERTY` / `StorageDeviceTemperatureProperty` on `\\.\PhysicalDriveN`, opened with zero access rights. `GetDevicePowerState` guards against waking spun-down drives (shown as "sleeping").
- **ACPI thermal zones** via PDH. Often fake; only read when the helper isn't delivering CPU readings.

### Helper client (`helper.cpp`, `HelperClient`)

Starts the helper one of two ways, then waits (≤5 s) for its startup signal:

1. **Start task, no prompt** (`task.cpp`, `HelperTask`): when this user's scheduled task exists and its copy is byte-identical to the running exe, `IRegisteredTask::Run` starts it with the PID and channel name as `$(Arg0)`/`$(Arg1)`. The helper's PID comes from the shared header.
2. **UAC**: otherwise (first enable, PawnIO install, exe updated, task broken) it relaunches its own exe with `runas` (`ShellExecuteEx`, unless already elevated) and `--helper --pid <pid> --map <name> --register-task [--install-pawnio]`. Until the helper signals startup, a handle on the exe denies writes and renames, so the file that gets elevated and copied for the task is the running one.

Each sample then sets a request event and waits (≤200 ms) for the helper's answer in shared memory (see [ipc.md](ipc.md)).

### Start task (`task.cpp`)

- `--register-task` (in the elevated helper) copies the exe to `%ProgramFiles%\LimitsAndTemps\LimitsAndTemps.exe` and registers the hidden, trigger-less task `\LimitsAndTemps CPU helper <user SID>`: interactive token of that user, highest run level, normal priority, no time limit, runs on battery, parallel instances. Action: `<copy> --helper --pid $(Arg0) --map $(Arg1)`. The task's DACL gives the user read/execute only.
- The task only ever runs the admin-only copy, so unelevated code can't swap what gets elevated. Anything running as the user can start the task with arbitrary arguments, so the helper only accepts its own flags and channel names starting with `Local\LimitsAndTemps-Helper-`; `--dump` and other modes are unreachable because `--helper` is always `argv[1]`.
- Standard users who elevate with another account's credentials get that account's task, not their own, so they keep seeing UAC prompts.
- "Disable CPU temperature" in the tray menu clears the setting and runs `--unregister-task` elevated (one UAC prompt): deletes this user's task, and the copy when no other user's task remains.

`HelperClient::IsPawnIoInstalled()` checks unelevated whether the PawnIO service key exists and its driver file is present. The app uses it to word the consent dialog and to pass `--install-pawnio` only when the driver is missing.

### UI (`main.cpp`)

- Merges sources in display order: helper CPUs → GPUs → drives → ACPI fallback. GPUs that have never reported a temperature (most iGPUs, or a dGPU asleep since start) are left out until they do.
- Rows are keyed by sensor name; the layout is rebuilt when the set of sensors changes. History (time-stamped, last 3 minutes) and max per name live in `m_history`, which survives rebuilds. The sparkline maps time to x and breaks the line at gaps (e.g. sleep).
- Look: always dark. `MSWEnableDarkMode(DarkMode_Always)` handles native parts (title bar, buttons, menus); everything custom uses the `Theme` palette in `main.cpp`. Numbers use `MonoFont()` (Cascadia Mono, falling back to Consolas).
- Agent limits (`m_usage`, see below) show one row per installed agent and one column per limit, each with a reset line. A "% used" / "% left" label next to ↻ says which is shown; clicking it flips all values (and the tray tooltip), persisted as `HKCU\Software\LimitsAndTemps\ShowRemaining`. It is a `wxGenericStaticText` because the native static control passes clicks to its parent. Colours always follow the used part: default text colour, turning amber from 50% used (5h) or 75% used (1w, Fable 1w) and red from 90%. The Fable 1w column only appears when an agent reports it; it shares the week reset, so it has no reset line. The week reset line is two labels: weekday and time ("We, 19:30") in a lighter colour (`Theme::Subtle`), then the muted countdown "(3d 15h 1m)". A missing value shows "-"; an error is shown in small muted text under the agent name, keeping the last good values. The section and its divider are left out when no agent is installed.

### Agent limits (`usage.cpp`)

- Uses the sign-in each agent already keeps on disk; there is no login of its own. Tokens are re-read from the file on every fetch and never renewed here: refresh tokens rotate, so renewing would sign the agent out. An expired or rejected token shows "sign-in expired, open … to renew it".
- **Claude Code**: installed when `%CLAUDE_CONFIG_DIR%` or `%USERPROFILE%\.claude` exists. Token from `.credentials.json` (`claudeAiOauth.accessToken`, skipped locally once `expiresAt` has passed). `GET https://api.anthropic.com/api/oauth/usage` with `anthropic-beta: oauth-2025-04-20`; reads `five_hour`, `seven_day`, `seven_day_fable` (`utilization` in %, `resets_at` ISO 8601).
  - **Status line** (no request, no readable sign-in needed): Claude Code passes `rate_limits` to its status-line command after every reply. Claude Code inside the Claude desktop app keeps its sign-in to itself, so `.credentials.json` goes stale there; this is then the only source. "Connect Claude Code..." (shown under Claude Code while it has no values) copies the exe to `%LOCALAPPDATA%\LimitsAndTemps\LimitsAndTemps.exe` (so moving or deleting the started exe doesn't matter) and registers `"<copy>" --statusline` as `statusLine.command` in Claude Code's `settings.json`, editing only that part of the file (byte offsets from the JSON parser). An existing command is kept as `"<exe>" --statusline --pass | <old command>` (a pipe works in cmd and Git Bash; `--pass` echoes the input). "Disconnect Claude Code" in the tray menu restores the old command or removes the member, and deletes the saved limits and the copy. Paths use forward slashes. While connected, every app start refreshes the copy when its size or write time differs from the running exe (a copy Claude Code is running is renamed to `.old` first, deleted on the next update) and repairs the registered path; `settings.json` is only written when its text changes.
  - `--statusline` saves only `rate_limits` to `limitsandtemps-usage.json` in the Claude config directory (written aside, then renamed) and prints "5h 10% · 1w 35%" unless `--pass`. Reads `five_hour`/`seven_day`/`seven_day_fable` (`used_percentage`, `resets_at` Unix seconds); a window whose reset has passed counts as 0%.
  - Order per fetch: status-line file if written within the last minute; else the API with a valid token; else (or on 401/403) the status-line file of any age.
- **Codex**: installed when `%CODEX_HOME%` or `%USERPROFILE%\.codex` exists (CLI and desktop app share it). Token from `auth.json` (`tokens.access_token`, `tokens.account_id`); API-key sign-ins have no plan limits. `GET https://chatgpt.com/backend-api/wham/usage` with `ChatGPT-Account-Id`; reads `rate_limit.primary_window`/`secondary_window` (`used_percent`, `reset_at` Unix seconds), assigned to 5h/1w by `limit_window_seconds`.
- WinHTTP (delay-loaded, system proxy), JSON parsed by a small built-in parser.
- Fetches run on a detached thread (`UsageJob`) and post a `wxThreadEvent`; the window clears the job's target when destroyed, so closing never waits for the network.
- Rate: once at start, then every 1 min while the window is on screen and every 15 min while hidden (the tooltip shows the limits too); 30 min after a 429. The ↻ button next to "AI agent limits" refreshes manually, at most every 30 s (5 min after a 429); it is disabled while a fetch runs or the cooldown lasts.
- The status-line file's timestamp is checked on every sample (2 s / 5 s); a change is read at once without a fetch, so Claude's limits follow each reply within seconds.

### Tray and lifecycle

- The tray menu starts with a disabled item showing the app version.
- Closing hides the window (and removes it from the taskbar). Left-clicking the tray icon shows it. Exit is in the tray menu.
- `--tray` starts hidden.
- One instance per user; a second launch shows the running instance's window.

### Settings

- `HKCU\Software\LimitsAndTemps\ShowRemaining`: agent limits shown as remaining instead of used.
- The Claude Code connection lives in Claude Code's own `settings.json` (see Agent limits), not in the registry.
- The "Enable CPU temperature" consent persists as `HKCU\Software\LimitsAndTemps\CpuSensors`. Once set, the helper starts on every launch: through the start task without a prompt, or with a UAC prompt when the task isn't usable.

## Helper mode (elevated) – `helper.cpp`, `cputemp.cpp`

- With `--register-task`, sets up the start task first (failures only mean UAC next time), then writes its PID to the header and signals `-done`.
- With `--install-pawnio` and the driver missing, extracts the embedded official `PawnIO_setup.exe` (2.2.0) into a fresh admin-only directory under `%SystemRoot%\Temp` and runs it with `-install -silent`.
- `CpuTemps` opens `\\?\GLOBALROOT\Device\PawnIO` and loads one embedded signed module from PawnIO.Modules 0.2.11:
  - **Intel** (`IntelMSR`): TjMax from `IA32_TEMPERATURE_TARGET` (datasheet values for Core 2 / old Atom). Reads `IA32_PACKAGE_THERM_STATUS` when CPUID reports it (one read per socket, no thread migration on single-socket systems), otherwise the hottest core's `IA32_THERM_STATUS` (e.g. Nehalem/Westmere, pinned to each core in turn).
  - **AMD Zen, family 17h–1Ah** (`AMDFamily17`): Tctl from SMN `0x59800`, minus 49 °C when the range bit is set, minus the k10temp Tdie offsets for 1600X/1700X/1800X/2700X/Threadripper 19xx/29xx. SMN only reaches the first socket, so one reading is reported.
  - **AMD family 10h–16h** (Phenom, FX, A-series; `AMDFamily10`): misc-control register `0xA4` per node, or the SMU register for family 15h models 60h–7Fh. The module's TSC measurement is never called: it freezes FX CPUs.
  - SMN/SMU access takes the `Global\Access_PCI` mutex shared with other monitoring tools.
- Multi-socket systems get one reading per package; identical names get a ` #n` suffix.
- Idles in `WaitForMultipleObjects` until the app requests a sample; exits when the quit flag is set or the parent process exits.
