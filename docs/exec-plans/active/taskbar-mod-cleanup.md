# TASKBAR MOD.cpp cleanup
Status: IN_PROGRESS
Plan Depth: STANDARD
Current Phase: Phase 1 — provable cleanups
Last Updated: 2026-11-07

## Executive Snapshot
Clean `TASKBAR MOD.cpp` (TAI v1.5.259, 10,390 lines) without changing visuals or
regressing performance. Decisions are settled: whole file in scope, behavior
identical, large structure kept (ApplyStyle is not split), `Tai` suffix kept,
only genuinely unclear names renamed. Council (5 advisors + 3 anonymous reviewers)
favored small compiler-provable batches; per-frame work limited to provably
equivalent changes. Baseline captured; no source edits yet.

## Objective
Remove leftover diagnostics, dead code, comment noise and duplicate helpers; cut
provable wasted work in hot paths; rename the few unclear names.

## Acceptance Criteria
- [ ] R1: Windhawk clang++ 20 build: 0 errors, warnings fall from 13 to 0, no new warning text.
- [ ] R2: Metadata blocks (`==WindhawkMod==`, `==WindhawkModReadme==`, `==WindhawkModSettings==`, lines 1-313) byte-identical.
- [ ] R3: Comment-only batch proven code-neutral: preprocessed token stream identical.
- [ ] R4: Taskbar screenshots (revealed-static, hover-center) match baseline side by side.
- [ ] R5: explorer.exe CPU median (idle, sweep) not above the highest baseline run (idle 1.477%, sweep 2.044%); private RAM not above 407 MB + 5%.
- [ ] R6: Agreed slop/inefficiency/name items T1-T8 done.

## Approach and Constraints
- Every change keeps runtime behavior; only Wh_Log output may change (T1, T6).
- Upstream helpers are kept; local duplicates are deleted and their callers
  pointed at the upstream helper, so upstream lines stay untouched.
- Reverse-engineering comments (disassembly, build numbers, workaround reasons,
  issue links) and GPL/credit lines stay. "Yoinked" is reworded, sources kept.
- No commits/push authorized. Rollback: `git checkout -- "TASKBAR MOD.cpp"`
  (HEAD = baseline) or the scratchpad copy `build/original.cpp`.

## Baseline (observed 2026-11-07, mod source identical to file, CRLF aside)
- Build: `exit=0 errors=0 warnings=13` (8 deprecated `Wh_SetFunctionHookT`, 5 unused constants).
- explorer.exe, 3 runs: idle 1.477 / 0.188 / 0.262 % (median 0.262); sweep 2.044 / 1.862 / 2.018 % (median 2.018); WS 411 MB, private 407 MB.
- Screenshots: scratchpad `baseline/revealed-static.png`, `baseline/hover-center.png`.

## Tools (scratchpad = C:\Users\Andino\AppData\Local\Temp\claude\19040f34f0dd5dda\9c9f2f8c-1932-43ac-9ddf-bc733e58fee2\scratchpad)
- Build: `cp "TASKBAR MOD.cpp" $S/build/work.cpp && $S/build/build.sh $S/build/work.cpp $S/build/work` → prints `exit= seconds= warnings= errors=`.
- Measure: `& $S/measure.ps1 -Tag after` (user hands off mouse ~2.5 min).

## Execution Board
| Phase | Status | Goal | Scope | Evidence / Result |
| --- | --- | --- | --- | --- |
| Phase 1 — provable cleanups (T1-T5, T8) | DONE | R1-R3, R6 | TASKBAR MOD.cpp | See Evidence log |
| Phase 2 — hot paths (T6, T7) | DONE (static) | R5, R6 | TASKBAR MOD.cpp | Builds clean; runtime effect pending T9 |
| Phase 3 — runtime check (T9) | IN_PROGRESS | R4, R5 | Windhawk install + measure | R4 pass; R5 sweep not settled (see T9 result) |

## Evidence log (2026-11-07, Windhawk clang++ 20, build.sh, scratchpad/build)
| Task | Check | Result |
| --- | --- | --- |
| T1 diagnostics | grep `DOCK-DIAG|moveEvents|// Wh_Log` = 0; build | exit=0 errors=0 warnings=13 |
| T2 dead code | grep of 8 removed names = 0; build | exit=0 errors=0 warnings=8 (5 unused-const gone) |
| T3 comments | `pp.sh` before/after: 1,923,528 tokens each; 283 differ, all numeric `__LINE__` values inside `Wh_Log` (non-numeric diffs = 0); header lines 1-313 `cmp` identical | CODE-IDENTICAL except log line numbers; HEADER-IDENTICAL |
| T4 helpers | grep `ClampIntTai|TrimTai|GetProcessExeName` = 0; build (first try failed on a stray `}` left by a short delete range, fixed) | exit=0 errors=0 warnings=8 |
| T5 hook API | 9 call sites replaced; build | exit=0 errors=0 warnings=0 |
| T6-T8 | build | exit=0 errors=0 warnings=0 |
| Final | `final.cpp` 10,279 lines, sha256 646f313c0dd0fb35…; header identical | exit=0 errors=0 warnings=0 |

Ruling: Wh_Log expands to `if (InternalWh_IsLogEnabled(...)) { ... }`, so log arguments were
never evaluated with logging off — T1's runtime saving is the per-frame counter only.
T6 still saves `OpenProcess`/`QueryFullProcessImageName`/2 setting reads per unrelated
SetWindowPos call, because those ran before (outside) the Wh_Log macro.

## Coverage
| Requirement | Task | Check |
| --- | --- | --- |
| R1 | T1-T8 | build.sh after each task |
| R2 | T3 gate | `cmp <(head -313 original) <(head -313 new)` |
| R3 | T3 | preprocessed diff empty |
| R4, R5 | T9 | measure.ps1 + side-by-side images |
| R6 | T1-T8 | grep checks per task |

## Detailed Execution
All tasks own `TASKBAR MOD.cpp` and run in order (shared file).

### T1 — Remove leftover diagnostics
Status: TODO · Readiness: READY · Depends on: none
- `OnDockMagnifyRenderingTai`: delete the `// [DOCK-DIAG] temporary` comment and the
  whole block from `static auto diagStart = now;` through its closing `}` (the
  per-second `Wh_Log(L"[DOCK-DIAG] ...")`).
- `DockMagnifyContextTai`: delete `int moveEvents{0};  // [DOCK-DIAG]`.
- `UpdateDockMagnifyHooksTai` moved handler: delete `++c->moveEvents;`.
- Geometry diagnostics header (`// ---- Temporary taskbar geometry diagnostics`):
  set `constexpr bool kDebugTaskbarGeometry = false;` and replace the 3 comment
  lines with `// Set to true to log throttled taskbar geometry from ApplyStyle.`
  (`if constexpr` already skips everything; the code stays for future debugging.)
- Delete the 9 commented-out `// Wh_Log(L"%S", result*.text);` lines.
Check: `grep -c 'DOCK-DIAG\|moveEvents\|// Wh_Log' ` → 0; build exit 0, errors 0.

### T2 — Remove dead code
Status: TODO · Readiness: READY · Depends on: T1
- Delete constants `kDefaultTrayIconSize`, `kDefaultTrayButtonSize`,
  `kWorkerShutdownPollMs`, `kDelayedApplyWorkerShutdownTimeoutMs`,
  `kAnimationFollowupWorkerShutdownTimeoutMs` (compiler: unused).
- Delete `bool InitializeDebounce();` declaration and its definition (never called;
  grep shows only decl + def).
- Delete `DispatcherTimer debounceTimer{nullptr};` (never referenced).
- Delete `std::atomic<bool> g_already_requested_debounce_initializing` and its
  write in `CleanupDebounce` (never read).
Check: grep of each name → 0 hits; build warnings 13 → 8.

### T3 — Comment noise (comment-only, code-neutral)
Status: TODO · Readiness: READY · Depends on: T2
Remove: the three ASCII-art banners after the settings block, before
`XamlRootFromTaskbarHostSharedPtr` section, and before the TAI section (each the
`////` art block); the lone `/////////////////////////////////////` line in
`HookTaskbarDllSymbolsStartButtonPosition`; `// ==== ... ====` begin/end
separators (keep their descriptive sentence as a plain one-line comment where it
names the section, e.g. `// Windows-OCT accent twin: palette shared with the Desktop Glass Clock.`);
`// ----` header decorations (keep text); empty `//` lines that stand alone
right after a function's opening brace; label comments `// Booleans`,
`// Height & Sizes`, `// Corner radii`, `// String list`.
Shorten: second copy of the badge comment in `TaskListButton_UpdateBadge_Hook` to
`// Same small-dot badge caveat as TaskListButton_OverlayIcon_Hook.` + its own
`// This hook handles UWP badges (e.g. Unigram).`; second copy of the
"Logically these should be marginValue too" block to
`// Right/bottom margins stay 0, see the note above (windhawk-mods#726).`;
the 3-line "26H2 realtime island forward declarations" to one line.
Reword: WindhawkBlur provenance to
`// Based on Windows 11 Taskbar Styler's WindhawkBlur, which derives from`
`// https://github.com/TranslucentTB/TranslucentTB/blob/release/ExplorerTAP/XamlBlurBrush.cpp`.
Keep: lines 1-313 untouched; every disassembly/build/workaround/issue comment.
Check (from scratchpad/build):
```sh
pp() { "/c/Program Files/Windhawk/Compiler/bin/clang++.exe" -E -P -x c++ -std=c++23 -target x86_64-w64-mingw32 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000008 -DWH_MOD -include windhawk_api.h "$1" | tr -s ' \t\r\n' '\n'; }
pp before-T3.cpp > a.txt; pp after-T3.cpp > b.txt; cmp a.txt b.txt && echo CODE-IDENTICAL
cmp <(head -313 original.cpp) <(head -313 after-T3.cpp) && echo HEADER-IDENTICAL
```

### T4 — Merge duplicate helpers into the upstream ones
Status: TODO · Readiness: READY · Depends on: T3
- Delete `static int ClampIntTai(...)`; in `ClampRectCenterXToBoundsTai` call
  `ClampInt(currentCenterX, minCenterX, maxCenterX)` (same clamp semantics; min≤max is ensured just above).
- Delete `static std::wstring TrimTai(...)`; its 3 callers use `TrimWide(...)`
  (same whitespace set `L" \t\r\n"`, same result; by-value parameter).
- Delete `GetProcessExeName`; its caller in `SetWindowPos_Hook` uses
  `GetProcessFileName` (same image-name extraction; only the failure value differs,
  "" vs "<unknown>", which never equals the compared exe names).
Check: grep `ClampIntTai|TrimTai|GetProcessExeName` → 0; build.

### T5 — Replace deprecated hook API alias
Status: TODO · Readiness: READY · Depends on: T4
`WindhawkUtils::Wh_SetFunctionHookT(` → `WindhawkUtils::SetFunctionHook(` at all 9
sites. Evidence: `windhawk_utils.h` defines the old name as a `[[deprecated]]`
alias with the identical body (`Wh_SetFunctionHook(reinterpret_cast<void*>...)`).
Check: build warnings 8 → 0.

### T6 — SetWindowPos hook: skip work for unrelated windows
Status: TODO · Readiness: READY · Depends on: T5
`SetWindowPos_Hook` is installed in all four processes (explorer included) and,
for every call, reads 2 settings, opens the target process, queries its image
path and logs a line, although only two window classes are acted on.
New order (positioning logic below the class check unchanged):
```cpp
WCHAR className[256] = L"<unknown>";
GetClassNameW(hWnd, className, ARRAYSIZE(className));  // fails harmlessly for null hWnd
const bool controlCenterClass = _wcsicmp(className, L"ControlCenterWindow") == 0;
const bool coreWindowClass = _wcsicmp(className, L"Windows.UI.Core.CoreWindow") == 0;
if (!controlCenterClass && !coreWindowClass) return callOriginal();
DWORD processId = 0;
if (!GetWindowThreadProcessId(hWnd, &processId)) return callOriginal();
const bool moveControlCenter = Wh_GetIntSetting(L"MoveFlyoutControlCenter") != 0;
const bool moveNotificationCenter = Wh_GetIntSetting(L"MoveFlyoutNotificationCenter") != 0;
const std::wstring processFileName = GetProcessFileName(processId);
Wh_Log(L"[SetWindowPos] ...same format...");   // now only for candidate windows
const bool isControlCenterWindow = moveControlCenter && controlCenterClass &&
    _wcsicmp(processFileName.c_str(), L"ShellHost.exe") == 0;
const bool isNotificationCenterWindow = moveNotificationCenter && coreWindowClass &&
    _wcsicmp(processFileName.c_str(), L"ShellExperienceHost.exe") == 0;
```
Equivalence: the original only changes `X` when one of these two class names
matches; every other path calls the original with unchanged arguments.
Check: build; T9 flyout check (open Control Center and Notification Center, both still align with the tray).

### T7 — Dock magnify: reuse per-frame buffers
Status: TODO · Readiness: READY · Depends on: T6
`StepDockMagnifyContextTai` allocates three `std::vector<double>` every frame.
Add to `DockMagnifyContextTai`: `std::vector<double> frameScales, frameWidths, frameShifts;`
(per-frame scratch). In the step function replace the declaration with:
```cpp
auto& scales = ctx.frameScales;
auto& widths = ctx.frameWidths;
auto& shifts = ctx.frameShifts;
scales.assign(n, 1.0);
widths.assign(n, 0.0);
shifts.assign(n, 0.0);
```
`assign` gives the same size and values as the old constructors; capacity is kept.
Check: build; T9 hover screenshot + sweep CPU.

### T8 — Rename unclear names
Status: TODO · Readiness: READY · Depends on: T7
- `IsWeirdFrameworkElement` → `IsUnusableLayoutElement` (def + 1 caller).
- `StyleTaiAccentLayers(TaskbarBackgroundCompositionResourcesTai& r)` → parameter `layers`.
- `CleanupDebounce` → `StopApplyWorkers` (def + 2 callers; no debounce timer exists anymore).
Conventional math names (`r,g,b,h,s,l`, loop `i`, `n`) stay.
Check: grep old names → 0; build 0 errors 0 warnings.

### T9 — Runtime verification
Status: TODO · Readiness: READY (needs user) · Depends on: T8
1. User pastes the new file into the Windhawk editor for `local@taskbar-dock-like`, compiles, confirms.
2. Confirm loaded DLL changed (explorer module list).
3. `measure.ps1 -Tag after` with user away from mouse; compare to baseline per R4/R5.
4. Open Control Center and Notification Center once; both align with the tray as before.
Result / Evidence (2026-11-07 16:49-16:53):
- Loaded DLL `local@taskbar-dock-like_1.5.259_648468.dll` (was `_637132`); Windhawk editor source equals repo file (CR stripped).
- R4 PASS (visual, side by side): island shape, accent tint/glow, edge light, running indicators and hover magnify (center icon enlarged, neighbours spread) match baseline. Differences seen are content only: app order, one extra tray icon, a different window behind the taskbar.
- Flyouts PASS: Win+A Control Center right edge ~1556 px and Win+N Notification Center right edge ~1572 px both sit at the island's right edge (~1570 px), not the screen edge.
- R5: idle median 0.074% (runs 0.066/0.086/0.074) vs baseline 0.262% — PASS. Sweep median 2.113% (runs 1.985/2.113/2.497) vs baseline 2.018% (max run 2.044) — FAILS the pre-set bound by 0.07 pp; run spread (0.51 pp) exceeds the difference, and the code only removed work on this path. NOT_VERIFIED until an interleaved A/B run. Private RAM 408 MB vs 407 MB — PASS (≤ +5%).

## NEXT ACTION
Settle R5 sweep with an interleaved A/B: original (HEAD) → measure, new → measure, repeat once,
same windows open, user away from the mouse; pass if new sweep median ≤ original median of the same session.
