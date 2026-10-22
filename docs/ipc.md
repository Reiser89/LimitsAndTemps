# IPC protocol (app ↔ helper)

Both sides live in `src/helper.cpp`: `HelperClient` (app, reader) and `RunHelper` (helper, writer). **Keep both in sync** when changing anything here.

## Objects

All created by the app before launching the helper, named `Local\LimitsAndTemps-Helper-<pid>-<n>` plus a suffix, fresh per helper start so a previous helper that is still shutting down can't write into them. Creation fails if a name already exists (squatting).

| Name suffix | Object | Use |
|---|---|---|
| (none) | 16 KB file mapping | header + payload |
| `-request` | auto-reset event | app → helper: take a sample now |
| `-done` | auto-reset event | helper → app: sample published |

On startup the helper writes its PID to the header and sets `-done` once; the app waits for that (≤5 s) and, when the helper came from the start task, opens the process by that PID.

The helper has no timer: it samples only when the app asks, so the app alone decides the sampling rate. The app resets `-done`, sets `-request` and waits up to 200 ms; on timeout it keeps the previous readings.

The helper only attaches to names starting with `Local\LimitsAndTemps-Helper-`, since the start task can be run with arbitrary arguments.

## Header (16 bytes)

| Offset | Type | Meaning |
|---|---|---|
| 0 | int32 | seq – seqlock counter, odd while the helper is writing |
| 4 | int32 | byte length of the payload |
| 8 | int32 | quit flag – set by LimitsAndTemps to stop the helper |
| 12 | int32 | helper PID – written by the helper at startup |

## Payload

UTF-16 lines after the header:

```
T\t<name>\t<tenths of °C, or empty>
S\t<status>
```

- `T` = one CPU package. The temperature is an integer in tenths of a degree, so no locale-dependent decimal separator is involved.
- `S` = status text from the helper (e.g. driver missing); absent when all is well.

## Lifetime

- The elevated helper can't be terminated by the unelevated app; the app sets the quit flag and signals `-request` instead.
- The helper also exits when the parent PID exits.
