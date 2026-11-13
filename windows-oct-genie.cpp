// ==WindhawkMod==
// @id              windows-oct-genie
// @name            Windows-OCT Genie
// @description     macOS-style genie minimize/restore that lands on the Windows-OCT Dock icon, paced to the display refresh rate
// @version         1.1.0
// @author          Abdullah Masood; Windows-OCT changes by Andino Ferdiansah
// @github          https://github.com/Abdullah-Masood-05
// @include         *
// @compilerOptions -ldwmapi -lgdi32 -ld2d1 -lole32 -loleaut32 -luuid -lshell32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Windows-OCT Genie

macOS-style genie for **minimize**, **restore** and **app launch**, based on
[MacOS Minimize Animation](https://windhawk.net/mods/macos-minimize-animation) 3.2.1
by Abdullah Masood (modern Direct2D genie engine by Potassiumuncher).

Changes in this version:
- Lands on the app's icon in the **Windows-OCT Dock** (`local@windows-oct-dock`): the dock
  reports where the icon is drawn, magnification included. Without the dock it falls back to
  the original taskbar search.
- One frame per display refresh, timed to when it is shown (e.g. 144 Hz = 6.9 ms steps).
- Separate minimize and restore durations, and a choice of speed curve.
- The launch genie also plays when a window hidden to the tray comes back (an app started
  with Windows, or closed to the tray and opened again).
- Two animation styles, as in the original: **Classic** (default, lighter) and **Modern**
  (Direct2D mesh). Both land on the Dock icon and follow the frame pacing setting.
- Close was removed.

Do not enable this together with **MacOS Minimize Animation**: both hook the same functions.
Disable that mod first.

Windows that Windows scales because they are not DPI aware use the normal Windows animation.

Debug: turn on **Frame log**, enable debug logging for this mod in Windhawk and open its log.
Every animation writes one line starting with `genie`.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- minimize_ms: 450
  $name: Minimize duration (ms)
  $description: Clamped to 100-1500.
- restore_ms: 450
  $name: Restore duration (ms)
  $description: Clamped to 100-1500.
- open_animation: true
  $name: Animate window restore
  $description: Play the reverse genie when a window is restored.
- launch_animation: true
  $name: Animate app launch
  $description: >-
    Play the reverse genie when an app's window first appears, and when a window
    hidden to the tray (an app started with Windows, or closed to the tray) is
    shown again.
- engine: classic
  $name: Animation style
  $description: >-
    Classic is the original strip genie: light, so it keeps up with fast displays.
    Modern is the Direct2D mesh genie: a curved lamp shape, but much more work per
    frame. Speed curve, squeeze-to-slide blend and mesh tile count apply to Modern only.
  $options:
  - classic: Classic (lighter)
  - modern: Modern (Direct2D mesh)
- speed_curve: smoothstep
  $name: Speed curve
  $description: How the animation speeds up and slows down (Modern style).
  $options:
  - smoothstep: Smoothstep (original)
  - easeInOutCubic: Ease in-out (cubic)
  - easeOutCubic: Quick start, soft landing
- lamp_blend: smooth
  $name: Squeeze-to-slide blend
  $description: >-
    Sharp is the original hand-off at 30% of the animation, where parts of the
    window stop or start moving at once. Smooth overlaps the two phases.
  $options:
  - smooth: Smooth
  - sharp: Sharp (original)
- tile_count: 35
  $name: Mesh tile count
  $description: >-
    Modern style only. Higher = smoother curve, more work per frame. Clamped to 8-96.
- dock_targeting: true
  $name: Land on the Windows-OCT Dock icon
  $description: >-
    Ask the Windows-OCT Dock where the app's icon is drawn. Off = the original
    taskbar search.
- pacing: vblank
  $name: Frame pacing
  $description: >-
    Vblank draws one frame per display refresh, timed to when it is shown.
    Legacy is the original timer at twice the refresh rate.
  $options:
  - vblank: Vblank (refresh rate)
  - legacy: Legacy timer
- frame_log: false
  $name: Frame log (debug)
  $description: Write one timing line per animation to this mod's log.
- multi_monitor: false
  $name: Multi-monitor support (experimental)
  $description: >-
    Play the genie on the monitor the window is on and target that monitor's
    taskbar edge, instead of always the primary monitor's.
- unhide_taskbar: true
  $name: Unhide taskbar (for taskbar auto-hide)
  $description: >-
    If the taskbar is set to auto-hide, briefly reveal it during the minimize so
    the genie has a visible dock to flow into, then let it slide back afterwards.
- unhide_duration_ms: 450
  $name: Taskbar unhide duration (ms)
  $description: >-
    How long to keep the taskbar revealed before performing the deferred minimize
    on the auto-hide path. Clamped to 0-5000.
- excluded_programs: [""]
  $name: Excluded programs
  $description: >-
    Programs whose windows should never animate (one per entry, e.g.
    WindowsTerminal.exe - with or without .exe). Matches the process that OWNS
    the window, wherever the animation is triggered from.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <math.h>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <new>
#include <vector>
#include <string>
#include <algorithm>
#include <cwctype>
#include <uiautomation.h>
#include <shellapi.h>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 2
#endif

// High-resolution waitable timer flag (Win10 1803+). The kernel rejects the
// flag on 1709 and earlier (and Win7/8.1), so CreateWaitableTimerExW is retried
// without it; the pacer then just ticks coarser. A plain Sleep(1) floor keeps
// the loop from ever degrading to a busy spin.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Ported from Potassiumuncher's genie engine (github.com/Potassiumuncher).
#define PI 3.14159265f
// Mesh tile-count bounds for the modern engine (Potassiumuncher's v1.5 update:
// resolution is now a user setting instead of a fixed 20x20 grid).
#define TILE_COUNT_MIN 8
#define TILE_COUNT_MAX 96
#define TILE_COUNT_DEFAULT 35
#define COMPAT_TASKBAR_HEIGHT 70
#define COMPAT_ICON_SIZE      33

// Ported from Potassiumuncher's genie engine (github.com/Potassiumuncher).
struct Geometry { float x, y, width, height; };

typedef LRESULT (WINAPI *DefWindowProcW_t)(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
DefWindowProcW_t DefWindowProcW_Original;

typedef BOOL (WINAPI *ShowWindow_t)(HWND hWnd, int nCmdShow);
ShowWindow_t ShowWindow_Original;

typedef BOOL (WINAPI *ShowWindowAsync_t)(HWND hWnd, int nCmdShow);
ShowWindowAsync_t ShowWindowAsync_Original;

typedef BOOL (WINAPI *SetWindowPlacement_t)(HWND hWnd, const WINDOWPLACEMENT* lpwndpl);
SetWindowPlacement_t SetWindowPlacement_Original;

typedef BOOL (WINAPI *CloseWindow_t)(HWND hWnd);
CloseWindow_t CloseWindow_Original;

typedef BOOL (WINAPI *SetWindowPos_t)(HWND hWnd, HWND hWndInsertAfter, int X, int Y,
                                      int cx, int cy, UINT uFlags);
SetWindowPos_t SetWindowPos_Original;

// Shared Direct2D factory (multi-threaded: several worker threads may render at
// once). Ported from Potassiumuncher's genie engine.
ID2D1Factory* g_d2dFactory = nullptr;

struct MacGenieAnimData {
    HWND hRealWnd;
    HBITMAP hBitmap;      // 32-bit top-down premultiplied DIB snapshot (w x h)
    void* pBits;          // its bits (fed straight to D2D CreateBitmap)
    RECT targetRect;      // the window's DWM extended-frame rect when anim started
    HMONITOR hMon;        // monitor the genie targets (window's when multi-monitor
                          // support is on, else the primary)
    int width;
    int height;
    int targetDockX;      // taskbar button X located via UI Automation
    BOOL isRising;        // FALSE = minimize (flow in), TRUE = restore (flow out)
    LONG_PTR originalExStyle;
    BOOL hiddenByCloak;   // rising only: TRUE = hidden via DWM cloak (every restore
                          // path), FALSE = via WS_EX_LAYERED + alpha 0
    HANDLE hFirstFrameShown; // falling only: event signaled once the ghost's first
                             // frame is composed; NULL when unused
    int durationMs;
    // Auto-hide "unhide" (deferred minimize) - Potassiumuncher's engine. Only used
    // on the DefWindowProcW SC_MINIMIZE path when the taskbar is set to auto-hide.
    BOOL requestedUnhide;
    HWND hNextApp;
    int  unhideDurationMs;
    BOOL deferredMinimize;
    // Windows-OCT Dock: the app's icon as the dock drew it (physical pixels).
    BOOL hasDockIcon;
    RECT dockIcon;
    // Windows-OCT frame log: time from the hook to the worker start, where the
    // target came from ("dock", "cursor", "taskbar") and the dock query time.
    double prestartMs;
    const wchar_t* targetSource;
    double dockQueryMs;
};

// Cached snapshot for the restore animation. The DIB section owns pBits, so
// DeleteObject(hBmp) frees both. Snapshot model from Potassiumuncher's engine.
struct SnapCache { HBITMAP hBmp; void* pBits; int w; int h; };

// --- THE VAULTS ---
std::unordered_map<HWND, SnapCache> g_SnapshotCache;
std::unordered_map<HWND, int> g_IconPositions;            // per-window learned icon X
std::unordered_map<std::wstring, int> g_ProcessIconPositions; // per-process fallback
std::unordered_set<HWND> g_AnimActive;   // windows with a genie currently in flight
std::unordered_set<HWND> g_LaunchSeen;   // windows already shown or animated once
// Windows-OCT: when a shown window was hidden (GetTickCount64), so that showing it
// again after a while - back from the tray - plays the launch genie once more.
std::unordered_map<HWND, ULONGLONG> g_HiddenAt;
std::mutex g_CacheMutex;

// Handed to MacGenieLaunchThread: the window, its TRUE pre-hide extended style
// and how it was hidden (TRUE = DWM cloak, FALSE = WS_EX_LAYERED + alpha 0).
struct MacGenieLaunchData {
    HWND hWnd;
    LONG_PTR originalExStyle;
    BOOL hiddenByCloak;
};

// --- SETTINGS ---
std::atomic<int>  g_minimizeMs{450};
std::atomic<int>  g_restoreMs{450};
std::atomic<bool> g_openAnimation{true};
std::atomic<bool> g_launchAnimation{true};
std::atomic<bool> g_multiMonitor{false};
std::atomic<int>  g_unhideDurationMs{450};
std::atomic<bool> g_unhideEnabled{true};
// true = the original strip-based GDI genie ("classic", the default), false = the
// Direct2D mesh genie (Potassiumuncher's engine).
std::atomic<bool> g_classicEngine{true};
// Mesh resolution of the modern engine (Potassiumuncher's tile-count setting).
std::atomic<int>  g_tileCount{TILE_COUNT_DEFAULT};
// Windows-OCT: speed curve (0 smoothstep, 1 ease in-out cubic, 2 ease-out cubic),
// lamp phase blend, dock targeting, vblank pacing and the debug frame log.
std::atomic<int>  g_speedCurve{0};
std::atomic<bool> g_lampSmooth{true};
std::atomic<bool> g_dockTargeting{true};
std::atomic<bool> g_vblankPacing{true};
std::atomic<bool> g_frameLog{false};
// Lowercased entries of the "Excluded programs" setting. Guarded by g_CacheMutex
// (written only from LoadSettings, read once per animation start).
std::vector<std::wstring> g_ExcludedPrograms;

// --- UNLOAD COORDINATION ---
// Windhawk unmaps the mod DLL right after Wh_ModUninit returns, so a worker thread
// still running mod code at that point (even just its function epilogue) would
// crash its host process. Every worker is started through MacGenieSpawnWorker,
// which keeps its handle and refuses new workers once g_unloading is set (checked
// under the same lock). Wh_ModBeforeUninit sets the flag, the workers notice it
// and wind down, and Wh_ModUninit - which runs after the hooks are disabled, so no
// new worker can appear - joins every handle before the DLL goes away. The join
// doesn't pump messages (that could re-enter hooks), and no worker sends a message
// to the unloading thread once g_unloading is set.
std::atomic<bool> g_unloading{false};
std::mutex g_WorkerThreadsMutex;
std::vector<HANDLE> g_WorkerThreads;

// Returns false (nothing started) when unloading or CreateThread fails; callers
// then undo their hide exactly as on any other failed start.
static bool MacGenieSpawnWorker(LPTHREAD_START_ROUTINE proc, void* param) {
    std::lock_guard<std::mutex> lock(g_WorkerThreadsMutex);
    if (g_unloading.load(std::memory_order_relaxed)) return false;
    // Drop handles of workers that already finished.
    for (size_t i = g_WorkerThreads.size(); i-- > 0;) {
        if (WaitForSingleObject(g_WorkerThreads[i], 0) == WAIT_OBJECT_0) {
            CloseHandle(g_WorkerThreads[i]);
            g_WorkerThreads.erase(g_WorkerThreads.begin() + i);
        }
    }
    HANDLE hThread = CreateThread(NULL, 0, proc, param, 0, NULL);
    if (!hThread) return false;
    g_WorkerThreads.push_back(hThread);
    return true;
}

static int MacGenieClampMs(PCWSTR name) {
    int ms = Wh_GetIntSetting(name);
    return ms < 100 ? 100 : (ms > 1500 ? 1500 : ms);
}

void MacGenieLoadSettings() {
    g_minimizeMs.store(MacGenieClampMs(L"minimize_ms"), std::memory_order_relaxed);
    g_restoreMs.store(MacGenieClampMs(L"restore_ms"), std::memory_order_relaxed);
    g_openAnimation.store(Wh_GetIntSetting(L"open_animation") != 0, std::memory_order_relaxed);
    g_launchAnimation.store(Wh_GetIntSetting(L"launch_animation") != 0, std::memory_order_relaxed);
    g_multiMonitor.store(Wh_GetIntSetting(L"multi_monitor") != 0, std::memory_order_relaxed);

    int unhide_ms = Wh_GetIntSetting(L"unhide_duration_ms");
    if (unhide_ms < 0) unhide_ms = 0;
    if (unhide_ms > 5000) unhide_ms = 5000;
    g_unhideDurationMs.store(unhide_ms, std::memory_order_relaxed);
    g_unhideEnabled.store(Wh_GetIntSetting(L"unhide_taskbar") != 0, std::memory_order_relaxed);

    PCWSTR engine = Wh_GetStringSetting(L"engine");
    g_classicEngine.store(wcscmp(engine, L"modern") != 0, std::memory_order_relaxed);
    Wh_FreeStringSetting(engine);

    int tiles = Wh_GetIntSetting(L"tile_count");
    if (tiles < TILE_COUNT_MIN) tiles = TILE_COUNT_MIN;
    if (tiles > TILE_COUNT_MAX) tiles = TILE_COUNT_MAX;
    g_tileCount.store(tiles, std::memory_order_relaxed);

    PCWSTR curve = Wh_GetStringSetting(L"speed_curve");
    g_speedCurve.store(wcscmp(curve, L"easeInOutCubic") == 0 ? 1 :
                       wcscmp(curve, L"easeOutCubic") == 0   ? 2 : 0,
                       std::memory_order_relaxed);
    Wh_FreeStringSetting(curve);
    PCWSTR blend = Wh_GetStringSetting(L"lamp_blend");
    g_lampSmooth.store(wcscmp(blend, L"sharp") != 0, std::memory_order_relaxed);
    Wh_FreeStringSetting(blend);
    g_dockTargeting.store(Wh_GetIntSetting(L"dock_targeting") != 0, std::memory_order_relaxed);
    PCWSTR pacing = Wh_GetStringSetting(L"pacing");
    g_vblankPacing.store(wcscmp(pacing, L"legacy") != 0, std::memory_order_relaxed);
    Wh_FreeStringSetting(pacing);
    g_frameLog.store(Wh_GetIntSetting(L"frame_log") != 0, std::memory_order_relaxed);

    // "Excluded programs" list: lowercase each entry once here so the per-anim
    // check is a straight comparison.
    std::vector<std::wstring> excluded;
    for (int i = 0;; i++) {
        PCWSTR prog = Wh_GetStringSetting(L"excluded_programs[%d]", i);
        bool has = *prog;   // L"" on unset, so *prog alone signals presence
        if (has) {
            std::wstring s = prog;
            std::transform(s.begin(), s.end(), s.begin(), ::towlower);
            excluded.push_back(std::move(s));
        }
        Wh_FreeStringSetting(prog);
        if (!has) break;
    }
    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        g_ExcludedPrograms.swap(excluded);
    }
}

// TRUE if the process that OWNS hWnd is on the "Excluded programs" list. This is
// checked at the single animation choke point (StartMacGenieAnim), NOT via
// Windhawk's own exclusion list: that list only prevents the mod from being
// LOADED into a process, but this mod's hooks animate a window from whichever
// process manipulates it (Explorer's taskbar click, the shell, the app itself),
// so an app excluded there would still animate. Matching the window's owning
// process here respects the exclusion no matter where the trigger came from.
bool MacGenieIsExcluded(HWND hWnd) {
    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        if (g_ExcludedPrograms.empty()) return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (!pid) return false;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    WCHAR path[MAX_PATH];
    DWORD len = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(hProc, 0, path, &len) != 0;
    CloseHandle(hProc);
    if (!ok) return false;

    std::wstring full = path;
    std::transform(full.begin(), full.end(), full.begin(), ::towlower);
    size_t slash = full.find_last_of(L"\\/");
    std::wstring base = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
    std::wstring baseNoExt = base;
    if (baseNoExt.size() > 4 && baseNoExt.compare(baseNoExt.size() - 4, 4, L".exe") == 0) {
        baseNoExt.resize(baseNoExt.size() - 4);
    }

    std::lock_guard<std::mutex> lock(g_CacheMutex);
    for (const std::wstring& e : g_ExcludedPrograms) {
        if (e == full || e == base || e == baseNoExt) return true;
    }
    return false;
}

// Windows-OCT: physical pixels everywhere. The hooks run inside whatever process
// minimizes the window, and in a DPI-unaware process GetWindowRect / GetCursorPos
// return scaled (logical) coordinates while DWMWA_EXTENDED_FRAME_BOUNDS is always
// physical - the genie then lands beside the icon. This scope makes the calling
// thread per-monitor aware for the genie's own work and puts it back afterwards.
struct MacGenieDpiScope {
    DPI_AWARENESS_CONTEXT prev;
    MacGenieDpiScope()
        : prev(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~MacGenieDpiScope() {
        if (prev) SetThreadDpiAwarenessContext(prev);
    }
    MacGenieDpiScope(const MacGenieDpiScope&) = delete;
    MacGenieDpiScope& operator=(const MacGenieDpiScope&) = delete;
};

// A window Windows bitmap-scales (DPI unaware, or system aware on a monitor with
// another DPI): its rect differs between its own DPI context and physical pixels.
// PrintWindow would capture it at the logical size, so such windows keep the
// normal Windows animation. Call inside a MacGenieDpiScope.
static bool MacGenieIsDpiVirtualized(HWND hWnd) {
    RECT physical, logical;
    if (!GetWindowRect(hWnd, &physical)) return true;
    DPI_AWARENESS_CONTEXT prev =
        SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(hWnd));
    BOOL ok = GetWindowRect(hWnd, &logical);
    if (prev) SetThreadDpiAwarenessContext(prev);
    return !ok ||
           (physical.right - physical.left) != (logical.right - logical.left) ||
           (physical.bottom - physical.top) != (logical.bottom - logical.top);
}

void MacGenieSetDwmTransitions(HWND hWnd, BOOL enable) {
    BOOL disable = !enable;
    DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
}

// Hide / show a window at the DWM level. A cloaked window is simply not rendered
// (frame included) while staying "visible" to Win32 and painting normally
// underneath, so a restore can happen invisibly under the genie with no
// layered-surface rebuild flash. Kept from MINE in preference to HIS's
// WS_EX_LAYERED + alpha 0 restore hide, which flashes the bare frame.
void MacGenieSetCloak(HWND hWnd, BOOL cloak) {
    DwmSetWindowAttribute(hWnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
}

// Undo a rising caller's pre-animation hide (used whenever the animation can't run
// or is refused): uncloak or un-hide depending on how the window was hidden, and
// re-enable its DWM transitions.
static void MacGenieUndoRisingHide(HWND hWnd, LONG_PTR originalExStyle, BOOL cloakHidden) {
    if (cloakHidden) {
        MacGenieSetCloak(hWnd, FALSE);
    } else {
        SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
        if (!(originalExStyle & WS_EX_LAYERED)) {
            SetWindowLongPtrW(hWnd, GWL_EXSTYLE, originalExStyle);
        }
    }
    MacGenieSetDwmTransitions(hWnd, TRUE);
}

// Should we animate this window at all? Skip child / tiny windows so the effect
// only fires on real top-level windows. The size check stays on the placement /
// window rect; the animation geometry (below) uses the DWM extended frame bounds.
static bool MacGenieShouldAnimate(HWND hWnd) {
    if (!hWnd) return false;
    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    if (style & WS_CHILD) return false;

    RECT r;
    if (IsIconic(hWnd)) {
        WINDOWPLACEMENT wp;
        wp.length = sizeof(wp);
        if (!GetWindowPlacement(hWnd, &wp)) return false;
        r = wp.rcNormalPosition;
    } else if (!GetWindowRect(hWnd, &r)) {
        return false;
    }
    if ((r.right - r.left) < 40 || (r.bottom - r.top) < 40) return false;
    return true;
}

// A real, top-level window with a title bar (skips child windows, tool windows,
// and borderless popups / menus / tooltips / splash screens).
static bool MacGenieIsLaunchWindow(HWND hWnd) {
    if (!hWnd) return false;
    if (GetAncestor(hWnd, GA_ROOT) != hWnd) return false;
    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    if (!(style & WS_CAPTION)) return false;
    LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;
    return MacGenieShouldAnimate(hWnd);
}

// -------------------------------------------------------------------------
// Direct2D genie renderer helpers - ported verbatim from Potassiumuncher's
// genie engine (github.com/Potassiumuncher).
// -------------------------------------------------------------------------

// Ported from Potassiumuncher's genie engine (github.com/Potassiumuncher).
static ID2D1PathGeometry* CreateQuadGeo(
    ID2D1Factory* factory,
    D2D1_POINT_2F p0, D2D1_POINT_2F p1,
    D2D1_POINT_2F p2, D2D1_POINT_2F p3)
{
    ID2D1PathGeometry* geo = nullptr;
    factory->CreatePathGeometry(&geo);
    if (!geo) return nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geo->Open(&sink)) || !sink) {
        if (sink) sink->Release();
        geo->Release();
        return nullptr;
    }
    sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(p1);
    sink->AddLine(p2);
    sink->AddLine(p3);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();
    return geo;
}

// Ported from Potassiumuncher's genie engine (github.com/Potassiumuncher).
static D2D1_POINT_2F BloatPoint(D2D1_POINT_2F p, D2D1_POINT_2F c, float amount = 0.5f) {
    float dx = p.x - c.x; float dy = p.y - c.y;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) return p;
    return D2D1::Point2F(p.x + (dx/len)*amount, p.y + (dy/len)*amount);
}

// Ported from Potassiumuncher's genie engine (github.com/Potassiumuncher).
HWND FindTaskbarForMonitor(HMONITOR hMon) {
    HWND hMainTray = FindWindowW(L"Shell_TrayWnd", NULL);
    HMONITOR mainMon = MonitorFromWindow(hMainTray, MONITOR_DEFAULTTOPRIMARY);
    if (hMon == mainMon || !hMon) return hMainTray;

    HWND hSecTray = NULL;
    while ((hSecTray = FindWindowExW(NULL, hSecTray, L"Shell_SecondaryTrayWnd", NULL)) != NULL) {
        if (MonitorFromWindow(hSecTray, MONITOR_DEFAULTTONULL) == hMon) {
            return hSecTray;
        }
    }
    return hMainTray;
}

// Ported from Potassiumuncher's genie engine (github.com/Potassiumuncher).
// Locates the app's taskbar button via UI Automation and returns its center X
// (falls back to fallbackX, then to the per-window / per-process learned cache).
// Runs on the hook (app UI) thread: CoInitializeEx(APARTMENTTHREADED) matches the
// typical GUI-thread apartment (S_FALSE) or reports RPC_E_CHANGED_MODE on an MTA
// thread; either way CoUninitialize is balanced only when we actually initialized.
int GetTaskbarButtonX(HWND hWndApp, int fallbackX, HMONITOR hMon) {
    int targetX = fallbackX;
    bool uiaFound = false;

    std::wstring procNameLower = L"";
    DWORD ownerPid = 0;
    GetWindowThreadProcessId(hWndApp, &ownerPid);
    if (ownerPid) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ownerPid);
        if (hProc) {
            WCHAR exePath[MAX_PATH] = {0};
            DWORD exePathLen = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, exePath, &exePathLen)) {
                WCHAR* name = wcsrchr(exePath, L'\\');
                if (name) {
                    procNameLower = (name + 1);
                    size_t dotPos = procNameLower.find(L'.');
                    if (dotPos != std::wstring::npos) procNameLower = procNameLower.substr(0, dotPos);
                    std::transform(procNameLower.begin(), procNameLower.end(), procNameLower.begin(), ::towlower);
                }
            }
            CloseHandle(hProc);
        }
    }

    std::wstring processKey = procNameLower;
    if (!processKey.empty() && hMon) {
        processKey += L"_" + std::to_wstring(reinterpret_cast<size_t>(hMon));
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool coInit = (hr == S_OK || hr == S_FALSE);

    if (hr == S_OK || hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
        IUIAutomation* pAutomation = nullptr;
        HRESULT hrUia = CoCreateInstance(__uuidof(CUIAutomation8), NULL, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&pAutomation);
        if (FAILED(hrUia)) {
            hrUia = CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&pAutomation);
        }

        if (SUCCEEDED(hrUia) && pAutomation) {
            HWND hTray = FindTaskbarForMonitor(hMon);
            if (hTray) {
                IUIAutomationElement* pTrayElement = nullptr;
                if (SUCCEEDED(pAutomation->ElementFromHandle(hTray, &pTrayElement)) && pTrayElement) {

                    WCHAR titleW[512] = {0};
                    GetWindowTextW(hWndApp, titleW, 512);
                    std::wstring titleLower = titleW;
                    std::transform(titleLower.begin(), titleLower.end(), titleLower.begin(), ::towlower);

                    std::wstring procHintLower = procNameLower;
                    if (procNameLower == L"chrome") procHintLower = L"google chrome";
                    else if (procNameLower == L"msedge") procHintLower = L"microsoft edge";
                    else if (procNameLower == L"firefox") procHintLower = L"firefox";
                    else if (procNameLower == L"brave") procHintLower = L"brave";
                    else if (procNameLower == L"opera") procHintLower = L"opera";
                    else if (procNameLower == L"vivaldi") procHintLower = L"vivaldi";

                    IUIAutomationCondition* pButtonCond = nullptr;
                    IUIAutomationCondition* pListItemCond = nullptr;
                    IUIAutomationCondition* pOrCond = nullptr;

                    VARIANT varBtn; varBtn.vt = VT_I4; varBtn.lVal = UIA_ButtonControlTypeId;
                    pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varBtn, &pButtonCond);

                    VARIANT varList; varList.vt = VT_I4; varList.lVal = UIA_ListItemControlTypeId;
                    pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varList, &pListItemCond);

                    if (pButtonCond && pListItemCond) {
                        pAutomation->CreateOrCondition(pButtonCond, pListItemCond, &pOrCond);
                    }

                    IUIAutomationElementArray* pArray = nullptr;
                    if (pOrCond && SUCCEEDED(pTrayElement->FindAll(TreeScope_Descendants, pOrCond, &pArray)) && pArray) {
                        int length = 0;
                        pArray->get_Length(&length);

                        MONITORINFO mi = {0};
                        mi.cbSize = sizeof(MONITORINFO);
                        GetMonitorInfoW(hMon, &mi);
                        int monRight = mi.rcMonitor.right;

                        int bestScore = 0;

                        for (int i = 0; i < length; i++) {
                            IUIAutomationElement* pItem = nullptr;
                            if (SUCCEEDED(pArray->GetElement(i, &pItem)) && pItem) {
                                BSTR name;
                                if (SUCCEEDED(pItem->get_CurrentName(&name)) && name) {
                                    std::wstring uiaNameLower = name;
                                    std::transform(uiaNameLower.begin(), uiaNameLower.end(), uiaNameLower.begin(), ::towlower);

                                    if (!uiaNameLower.empty()) {
                                        int score = 0;

                                        if (titleLower == uiaNameLower) score += 1000;
                                        if (!titleLower.empty() && titleLower.find(uiaNameLower) != std::wstring::npos) score += 500;
                                        if (!uiaNameLower.empty() && uiaNameLower.find(titleLower) != std::wstring::npos) score += 500;

                                        if (!procNameLower.empty() && uiaNameLower.find(procNameLower) != std::wstring::npos) score += 400;
                                        if (!procHintLower.empty() && procHintLower != procNameLower &&
                                            uiaNameLower.find(procHintLower) != std::wstring::npos) score += 900;

                                        std::wstring currentWord;
                                        for (wchar_t c : titleLower) {
                                            if (iswalnum(c)) {
                                                currentWord += c;
                                            } else {
                                                if (currentWord.length() >= 4 && uiaNameLower.find(currentWord) != std::wstring::npos) score += 50;
                                                currentWord.clear();
                                            }
                                        }
                                        if (currentWord.length() >= 4 && uiaNameLower.find(currentWord) != std::wstring::npos) score += 50;

                                        if (uiaNameLower.find(L"start") != std::wstring::npos) score -= 500;
                                        if (uiaNameLower.find(L"search") != std::wstring::npos) score -= 500;
                                        if (uiaNameLower.find(L"task view") != std::wstring::npos) score -= 500;
                                        if (uiaNameLower.find(L"widgets") != std::wstring::npos) score -= 500;

                                        if (score > bestScore) {
                                            RECT bRect;
                                            if (SUCCEEDED(pItem->get_CurrentBoundingRectangle(&bRect))) {
                                                if (bRect.right > bRect.left && bRect.left < monRight - 50) {
                                                    bestScore = score;
                                                    targetX = bRect.left + (bRect.right - bRect.left) / 2;
                                                    uiaFound = true;
                                                }
                                            }
                                        }
                                    }
                                    SysFreeString(name);
                                }
                                pItem->Release();
                            }
                        }
                        pArray->Release();
                    }
                    if (pButtonCond) pButtonCond->Release();
                    if (pListItemCond) pListItemCond->Release();
                    if (pOrCond) pOrCond->Release();
                    pTrayElement->Release();
                }
            }
            pAutomation->Release();
        }
        if (coInit) CoUninitialize();
    }

    std::lock_guard<std::mutex> lock(g_CacheMutex);
    if (uiaFound) {
        g_IconPositions[hWndApp] = targetX;
        if (!processKey.empty()) {
            g_ProcessIconPositions[processKey] = targetX;
        }
    } else if (g_IconPositions.count(hWndApp)) {
        targetX = g_IconPositions[hWndApp];
    } else if (!processKey.empty() && g_ProcessIconPositions.count(processKey)) {
        targetX = g_ProcessIconPositions[processKey];
    }

    return targetX;
}

// Windows-OCT: the speed curve ("Speed curve" setting). p and the result are 0..1.
// Minimize uses t = ease(p) and restore t = 1 - ease(p), so "quick start, soft
// landing" holds both ways (out of the window into the icon, and back out).
static float MacGenieEase(float p, int curve) {
    if (p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return 1.0f;
    switch (curve) {
        case 1: {   // ease in-out cubic
            if (p < 0.5f) return 4.0f * p * p * p;
            const float q = -2.0f * p + 2.0f;
            return 1.0f - q * q * q * 0.5f;
        }
        case 2: {   // ease-out cubic: quick start, soft landing
            const float q = 1.0f - p;
            return 1.0f - q * q * q;
        }
        default:    // smoothstep (original)
            return p * p * (3.0f - 2.0f * p);
    }
}

// Ported from Potassiumuncher's genie engine (github.com/Potassiumuncher).
// The macOS "lamp" curve: maps a mesh vertex (tx,ty in 0..1) at progress p to its
// warped screen position, given the window rect w and the taskbar icon rect i.
static void CalculateLampVertexMacOS(float tx, float ty, float p, const Geometry& w, const Geometry& i, float *outX, float *outY, bool smooth) {
    float split = 0.3f;
    float k, j;
    if (smooth) {
        // Windows-OCT: overlapping eased phases (squeeze 0-0.4, slide 0.2-1), so no
        // part of the window stops or starts at once at the old 0.3 hand-off.
        k = fminf(fmaxf(p / 0.4f, 0.0f), 1.0f);
        j = fminf(fmaxf((p - 0.2f) / 0.8f, 0.0f), 1.0f);
        k = k * k * (3.0f - 2.0f * k);
        j = j * j * (3.0f - 2.0f * j);
    } else {
        k = (p <= split) ? (p / split) : 1.0f;
        j = (p > split) ? ((p - split) / (1.0f - split)) : 0.0f;
    }

    float expandHeight = (i.y - w.y - w.height);
    float fullHeight = (i.y - w.y) - (expandHeight * (1.0f - k));
    float height = fullHeight - (j * fullHeight);

    float y = ty * height;
    float x = tx * (i.width) + tx * (w.width - i.width) * (1.0f - j) * (1.0f - ty) + tx * (w.width - i.width) * (1.0f - k) * ty;

    float offsetX = (i.x - w.x) * (y / (fullHeight + 0.1f)) * k + (i.x - w.x) * j;
    float offsetY = i.y - w.y - height - (expandHeight * (1.0f - k));

    float effectX = sinf(((height - y) / fullHeight) * 2.0f * PI + PI) * (w.x + w.width * tx - (i.x + i.width * tx)) / 7.0f * k;

    *outX = w.x + x + offsetX + effectX;
    *outY = w.y + y + offsetY;
}

// The lamp curve assumes the target is BELOW the window. For a target above it (a
// close toward the top edge, a top taskbar, a side edge with the window in the
// lower half) evaluate it in vertically mirrored space and mirror the result back;
// otherwise the window sweeps past itself and the snapshot plays upside down.
static void CalculateLampVertex(float tx, float ty, float p, const Geometry& w,
                                const Geometry& i, float* outX, float* outY, bool smooth) {
    if (i.y + i.height * 0.5f >= w.y + w.height * 0.5f) {
        CalculateLampVertexMacOS(tx, ty, p, w, i, outX, outY, smooth);
        return;
    }
    Geometry wm = { w.x, -(w.y + w.height), w.width, w.height };
    Geometry im = { i.x, -(i.y + i.height), i.width, i.height };
    float yM;
    CalculateLampVertexMacOS(tx, 1.0f - ty, p, wm, im, outX, &yM, smooth);
    *outY = -yM;
}

// Shared by both engines: union of the monitors involved (the window's own
// monitor and the monitor the genie funnels to). With Multi-monitor support off
// (the default) data->hMon is the primary monitor, so a window sitting on a
// secondary display still needs its own monitor in the union or the canvas would
// collapse onto the primary and clip the first half of the animation.
static RECT GetGenieMonitorUnion(const RECT& winRect, HMONITOR hDockMon) {
    RECT winMon;
    HMONITOR hWinMon = MonitorFromRect(&winRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO wmi = {0};
    wmi.cbSize = sizeof(wmi);
    if (hWinMon && GetMonitorInfoW(hWinMon, &wmi)) {
        winMon = wmi.rcMonitor;
    } else {
        winMon = winRect;
    }

    RECT dockMon;
    MONITORINFO dmi = {0};
    dmi.cbSize = sizeof(dmi);
    if (hDockMon && GetMonitorInfoW(hDockMon, &dmi)) {
        dockMon = dmi.rcMonitor;
    } else {
        dockMon = winMon;
    }

    RECT u;
    u.left   = (winMon.left   < dockMon.left)   ? winMon.left   : dockMon.left;
    u.top    = (winMon.top    < dockMon.top)    ? winMon.top    : dockMon.top;
    u.right  = (winMon.right  > dockMon.right)  ? winMon.right  : dockMon.right;
    u.bottom = (winMon.bottom > dockMon.bottom) ? winMon.bottom : dockMon.bottom;
    return u;
}

// Shared by both engines: clamp a proposed canvas rect to the union of the
// monitors involved, then re-expand it so it can never clip the window rect or
// the dock target column away. The monitor clamp only trims the box to the
// screens actually used; it must never cut geometry the animation draws.
static void ClampGenieCanvas(int& left, int& top, int& right, int& bottom,
                             const RECT& winRect, int dockX, const RECT& monUnion) {
    if (left   < monUnion.left)   left   = monUnion.left;
    if (right  > monUnion.right)  right  = monUnion.right;
    if (top    < monUnion.top)    top    = monUnion.top;
    if (bottom > monUnion.bottom) bottom = monUnion.bottom;

    if (left  > winRect.left)  left  = winRect.left;
    if (right < winRect.right) right = winRect.right;
    if (top   > winRect.top)   top   = winRect.top;
    if (bottom < winRect.bottom) bottom = winRect.bottom;
    if (left  > dockX - 16)    left  = dockX - 16;   // iGeom is dockX ± 11
    if (right < dockX + 16)    right = dockX + 16;
}

// Largest snapshot or canvas the genie will allocate: 64 MPixels (256 MB at 32
// bits). Far above any real monitor setup (two 8K screens side by side fit), so
// it only stops pathological sizes - e.g. a huge window, which the hook then
// handles with the system animation instead.
static const long long MACGENIE_MAX_CANVAS_PIXELS = 64LL * 1024 * 1024;

// Shared by both engines, after ClampGenieCanvas: drop the parts of the canvas
// that lie outside the virtual screen. No monitor can show them, and both
// renderers already clip whatever falls outside the canvas, so on-screen output
// is unchanged - but a window parked far off-screen no longer asks for a canvas
// spanning tens of thousands of pixels (gigabytes of DIB). Returns false when
// nothing visible is left or the canvas is still over the cap; the caller then
// skips rendering.
static bool ClipGenieCanvasToDesktop(int& left, int& top, int& right, int& bottom) {
    const int vsLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vsTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vsWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vsHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vsWidth > 0 && vsHeight > 0) {
        if (left   < vsLeft)            left   = vsLeft;
        if (top    < vsTop)             top    = vsTop;
        if (right  > vsLeft + vsWidth)  right  = vsLeft + vsWidth;
        if (bottom > vsTop + vsHeight)  bottom = vsTop + vsHeight;
    }
    if (right <= left || bottom <= top) return false;
    return (long long)(right - left) * (bottom - top) <= MACGENIE_MAX_CANVAS_PIXELS;
}

// ---- Windows-OCT: frame pacing on the display's vblank ----
// From DWM's composition clock: the vblank at which a frame handed to DWM now is
// on screen (DWM takes the update at the next vblank and shows it at the one
// after), and the refresh period, both in QPC ticks. qpcVBlank can already be in
// the future (observed: ~5 ms ahead), so the next vblank is found on its grid in
// both directions. False when DWM can't tell; the caller then uses legacy timing.
static bool MacGenieNextPresent(LONGLONG nowQpc, LONGLONG* presentQpc, LONGLONG* periodQpc) {
    DWM_TIMING_INFO ti = {};
    ti.cbSize = sizeof(ti);
    if (FAILED(DwmGetCompositionTimingInfo(NULL, &ti))) return false;
    const LONGLONG period = (LONGLONG)ti.qpcRefreshPeriod;
    const LONGLONG vblank = (LONGLONG)ti.qpcVBlank;
    if (period <= 0 || vblank <= 0) return false;
    LONGLONG next;
    if (vblank >= nowQpc) {
        next = vblank - ((vblank - nowQpc) / period) * period;   // first one at or after now
    } else {
        next = vblank + ((nowQpc - vblank) / period + 1) * period;
    }
    *presentQpc = next + period;
    *periodQpc = period;
    return true;
}

// Sleeps on the high-resolution timer until QPC time `untilQpc` (at most 50 ms per
// call, so an unload is noticed); returns at once when that time has passed.
static void MacGenieWaitUntil(HANDLE timer, LONGLONG untilQpc, double qpcPerMs) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double waitMs = (double)(untilQpc - now.QuadPart) / qpcPerMs;
    if (waitMs <= 0.05) return;
    if (waitMs > 50.0) waitMs = 50.0;
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(waitMs * 10000.0);   // 100 ns units, relative
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, 100);
            return;
        }
    }
    Sleep((DWORD)waitMs);   // coarser, but never a spin
}

struct MacGenieFrameStats {
    std::vector<float> renderMs;   // frame start -> after UpdateLayeredWindow
    int frames = 0;
    int missedVblanks = 0;         // refreshes that showed no new frame (vblank pacing)
    double periodMs = 0;
};

// q in 0..1 (0.5 = median, 1 = max); 0 for an empty list.
static float MacGeniePercentile(std::vector<float> v, float q) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)(q * (float)(v.size() - 1) + 0.5f);
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

// Frame timing shared by both engines. Vblank: each frame is drawn for the refresh
// it will be shown at, and the animation time is that refresh's time, so every
// refresh advances it by exactly one period. Legacy: the original rate limiter at
// 2x the refresh rate, with the animation time taken when the frame starts.
// Per frame: BeginFrame, draw and hand the frame to DWM, EndFrame, then Wait.
struct MacGeniePacer {
    HANDLE timer = nullptr;
    LARGE_INTEGER qpcFreq{}, qpcStart{}, frameStart{};
    double qpcPerMs = 1.0;
    double frameIntervalMs = 1000.0 / 240.0;   // legacy interval (fallback 240 fps)
    bool vblank = false;
    bool frameLog = false;
    bool onVblank = false;                     // this frame is timed on a vblank
    LONGLONG present = 0, period = 0, presentStart = 0, lastPresent = 0;
    MacGenieFrameStats stats;

    MacGeniePacer() {
        QueryPerformanceFrequency(&qpcFreq);
        QueryPerformanceCounter(&qpcStart);
        qpcPerMs = (double)qpcFreq.QuadPart / 1000.0;
        vblank = g_vblankPacing.load(std::memory_order_relaxed);
        frameLog = g_frameLog.load(std::memory_order_relaxed);
        if (frameLog) stats.renderMs.reserve(512);
        DWM_TIMING_INFO ti = { sizeof(ti) };
        if (SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &ti)) && ti.rateRefresh.uiDenominator) {
            double hz = (double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator;
            if (hz > 0) frameIntervalMs = 1000.0 / (hz * 2.0);
        }
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer) {
            // Pre-1803: the high-resolution flag isn't recognized. A plain waitable
            // timer paces coarser, but it still isn't a spin.
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
    }
    ~MacGeniePacer() {
        if (timer) CloseHandle(timer);
    }
    MacGeniePacer(const MacGeniePacer&) = delete;
    MacGeniePacer& operator=(const MacGeniePacer&) = delete;

    // Starts a frame. True: *elapsedMs is the animation time this frame shows.
    // False: that refresh already has its frame, so this waited for the next one;
    // the caller checks for unload and calls again.
    bool BeginFrame(double* elapsedMs) {
        QueryPerformanceCounter(&frameStart);
        onVblank = vblank && MacGenieNextPresent(frameStart.QuadPart, &present, &period);
        if (!onVblank) {
            stats.periodMs = frameIntervalMs * 2.0;
            *elapsedMs = (double)(frameStart.QuadPart - qpcStart.QuadPart) / qpcPerMs;
            return true;
        }
        if (lastPresent != 0) {
            const LONGLONG steps =
                (LONGLONG)((double)(present - lastPresent) / (double)period + 0.5);
            if (steps < 1) {
                MacGenieWaitUntil(timer, lastPresent - period + period / 8, qpcPerMs);
                return false;
            }
            stats.missedVblanks += (int)(steps - 1);
        }
        if (presentStart == 0) presentStart = present;
        lastPresent = present;
        stats.periodMs = (double)period / qpcPerMs;
        *elapsedMs = (double)(present - presentStart) / qpcPerMs;
        return true;
    }

    // The frame has been handed to DWM: count it and note its render time.
    void EndFrame() {
        if (frameLog) {
            LARGE_INTEGER after;
            QueryPerformanceCounter(&after);
            stats.renderMs.push_back(
                (float)((double)(after.QuadPart - frameStart.QuadPart) / qpcPerMs));
        }
        stats.frames++;
    }

    // Sleeps until the next frame should be drawn (never a spin).
    void Wait() {
        if (onVblank) {
            // DWM takes this frame at the vblank before `present`; draw the next one
            // just after that vblank, for the refresh after it.
            MacGenieWaitUntil(timer, present - period + period / 8, qpcPerMs);
        } else if (timer) {
            // Legacy: the period is max(render, interval), measured from the frame
            // start; an overrun just yields the quantum.
            LARGE_INTEGER after;
            QueryPerformanceCounter(&after);
            double waitMs = frameIntervalMs -
                            (double)(after.QuadPart - frameStart.QuadPart) / qpcPerMs;
            if (waitMs > 0.1) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)(waitMs * 10000.0);   // 100 ns units
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 100);
            } else {
                SwitchToThread();
            }
        } else {
            Sleep(1);   // last resort: never spin
        }
    }

    void Log(const MacGenieAnimData* data, const wchar_t* engine) const {
        if (!frameLog || stats.frames == 0) return;
        Wh_Log(L"genie %s: engine=%s frames=%d dur=%dms pacing=%s period=%.2fms "
               L"render p50=%.2f p95=%.2f max=%.2f ms missed=%d prestart=%.1fms "
               L"target=%s query=%.1fms",
               data->isRising ? L"restore" : L"minimize", engine, stats.frames,
               data->durationMs, vblank ? L"vblank" : L"legacy", stats.periodMs,
               MacGeniePercentile(stats.renderMs, 0.5f),
               MacGeniePercentile(stats.renderMs, 0.95f),
               MacGeniePercentile(stats.renderMs, 1.0f), stats.missedVblanks,
               data->prestartMs, data->targetSource, data->dockQueryMs);
    }
};

// ---- Windows-OCT Dock: where the app's icon is drawn ----
// Asks local@windows-oct-dock (in explorer.exe) through its message-only window;
// protocol WindowsOct.DockIconRect.v1 (see OctQueryIconRect in TASKBAR MOD-v2.cpp):
// the answer packs left, top, width, height as int16 physical pixels. Bounded by a
// 40 ms timeout; any failure returns false and the caller keeps the old search.
static const UINT MACGENIE_DOCK_TIMEOUT_MS = 40;
// How the genie ends inside the icon: neck width as a fraction of the icon, and
// how far down the icon the collapse line sits.
static const float MACGENIE_DOCK_NECK = 0.6f;
static const float MACGENIE_DOCK_LAND = 0.35f;

static bool MacGenieUnpackDockRect(DWORD_PTR packed, RECT* out) {
    const LONG left = (INT16)(packed & 0xFFFF);
    const LONG top = (INT16)((packed >> 16) & 0xFFFF);
    const LONG width = (INT16)((packed >> 32) & 0xFFFF);
    const LONG height = (INT16)((packed >> 48) & 0xFFFF);
    if (width <= 0 || height <= 0) return false;
    *out = { left, top, left + width, top + height };
    return true;
}

static bool MacGenieQueryDockIcon(HWND hWnd, RECT* out, double* elapsedMs) {
    *elapsedMs = 0;
    static const UINT msg = RegisterWindowMessageW(L"WindowsOct.DockIconRect.v1");
    if (!msg) return false;
    HWND dock = FindWindowExW(HWND_MESSAGE, nullptr, L"Windhawk_local@windows-oct-dock", nullptr);
    if (!dock) return false;
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    DWORD_PTR packed = 0;
    LRESULT ok = SendMessageTimeoutW(dock, msg, (WPARAM)hWnd, 0,
                                     SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
                                     MACGENIE_DOCK_TIMEOUT_MS, &packed);
    QueryPerformanceCounter(&b);
    *elapsedMs = (double)(b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    return ok && MacGenieUnpackDockRect(packed, out);
}

// -------------------------------------------------------------------------
// Genie Animation Thread (MODERN engine)
//
// MINE's loop and lifecycle - joined worker, g_unloading early-out, the
// first-frame sync event, g_AnimActive teardown - with HIS's Direct2D mesh
// renderer as the per-frame draw call. Frames are paced by MacGeniePacer
// (never a busy loop). Heavy per frame: one path geometry per mesh tile, drawn
// into a GDI DC target, so the result is copied back from the GPU every frame.
// -------------------------------------------------------------------------
static void MacGenieRevealRealWindow(MacGenieAnimData* data);   // common teardown,
static void MacGenieFinishAnim(MacGenieAnimData* data);         // defined below

DWORD WINAPI MacGenieAnimThread(LPVOID lpParam) {
    MacGenieAnimData* data = (MacGenieAnimData*)lpParam;
    MacGenieDpiScope dpiScope;

    // Matches the in-repo standalone genie mod (genie-minimize-animation.wh.cpp):
    // HIGHEST, not TIME_CRITICAL. A TIME_CRITICAL thread that never yields would
    // preempt the host app's UI thread for the whole animation on slow or 2-core
    // machines; the pacer below keeps the frame rate up without that.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // --- Target geometry (Potassiumuncher's engine), using the monitor picked by
    // the multi-monitor setting (data->hMon), so "off" keeps the primary-monitor
    // behavior and "on" targets the window's monitor taskbar edge. ---
    Geometry wGeom = { (float)data->targetRect.left, (float)data->targetRect.top, (float)data->width, (float)data->height };

    MONITORINFO mi = {0};
    mi.cbSize = sizeof(MONITORINFO);
    HMONITOR hMon = data->hMon;
    GetMonitorInfoW(hMon, &mi);

    UINT dpiX = 96, dpiY = 96;
    // System32-only search: Shcore.dll is not a KnownDLL, and under @include *
    // this code runs inside every process - a bare-name LoadLibrary would search
    // the host .exe's own directory first, letting a planted Shcore.dll next to
    // any portable app execute in that process (windhawk-mods #2063 pattern).
    HMODULE hShcore = LoadLibraryExW(L"Shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hShcore) {
        typedef HRESULT (WINAPI *GetDpiForMonitor_t)(HMONITOR, int, UINT*, UINT*);
        auto pGetDpiForMonitor = (GetDpiForMonitor_t)GetProcAddress(hShcore, "GetDpiForMonitor");
        if (pGetDpiForMonitor) pGetDpiForMonitor(hMon, 0, &dpiX, &dpiY);
        FreeLibrary(hShcore);
    }
    float dpiScale = dpiY / 96.0f;
    float scaledTaskbarHeight = COMPAT_TASKBAR_HEIGHT * dpiScale;
    float scaledIconSize = COMPAT_ICON_SIZE * dpiScale;

    float iGeomTaskbarTop = (float)mi.rcMonitor.bottom - scaledTaskbarHeight;

    HWND hTrayGeom = FindTaskbarForMonitor(hMon);
    if (hTrayGeom) {
        RECT tr;
        if (GetWindowRect(hTrayGeom, &tr)) {
            int th = tr.bottom - tr.top;
            if (th > 0) iGeomTaskbarTop = (float)tr.top;
        }
    }

    Geometry iGeom = {
        (float)data->targetDockX - 11.0f,
        iGeomTaskbarTop,
        22.0f,
        scaledIconSize
    };
    // Windows-OCT Dock: end inside the icon the dock reported, a bit narrower than
    // the icon and a third of the way down it (the lamp collapses onto iGeom's top).
    if (data->hasDockIcon) {
        const float iconW = (float)(data->dockIcon.right - data->dockIcon.left);
        const float iconH = (float)(data->dockIcon.bottom - data->dockIcon.top);
        const float neckW = iconW * MACGENIE_DOCK_NECK;
        iGeom.x = (float)(data->dockIcon.left + data->dockIcon.right) * 0.5f - neckW * 0.5f;
        iGeom.y = (float)data->dockIcon.top + iconH * MACGENIE_DOCK_LAND;
        iGeom.width = neckW;
        iGeom.height = iconH * (1.0f - MACGENIE_DOCK_LAND);
    }
    // Mesh resolution from the tile-count setting (Potassiumuncher's v1.5).
    int xTiles = g_tileCount.load(std::memory_order_relaxed);
    int yTiles = xTiles;

    // Tight bounding-box canvas: the genie mesh is written in screen-space
    // coordinates and only ever occupies the region between the window rect and
    // the dock target. Rendering over the full virtual desktop wasted CPU on
    // every frame - UpdateLayeredWindow of a whole-screen layered window is slow,
    // most visibly on AMD drivers. A ~W/2 pad alone can't bound the mesh's sway
    // term in CalculateLampVertexMacOS (its amplitude scales with the
    // window-to-dock distance, not the window width), so the horizontal pad is
    // the exact sway bound: max(|w.x-i.x|, |w.right-i.right|)/7.
    const int origLeftB = data->targetRect.left;
    const int wB = data->width;
    const int dockXB = data->targetDockX;

    const float iLeftF = iGeom.x;
    const float iWidthF = iGeom.width;
    const float sway = fmaxf(fabsf((float)origLeftB - iLeftF),
                             fabsf((float)origLeftB + wB - iLeftF - iWidthF)) / 7.0f;
    int padX = (int)((wB / 2 > sway ? wB / 2 : sway) + 1);

    // iGeom is known here, so a top/side taskbar (iGeom.y above/beside the
    // window) is covered: the box spans min..max of the window and the icon rect,
    // with no band below the taskbar top ever wasted.
    int boundLeft   = ((origLeftB < dockXB) ? origLeftB : dockXB) - padX;
    int boundRight  = (((origLeftB + wB) > dockXB) ? (origLeftB + wB) : dockXB) + padX;
    int boundTop    = (int)fminf(wGeom.y, iGeom.y);
    int boundBottom = (int)fmaxf(wGeom.y + wGeom.height, iGeom.y + iGeom.height);

    // Trim the box to the union of the monitors actually involved, but never
    // clip the window rect or the dock target column away.
    RECT monUnion = GetGenieMonitorUnion(data->targetRect, data->hMon);
    ClampGenieCanvas(boundLeft, boundTop, boundRight, boundBottom,
                     data->targetRect, dockXB, monUnion);
    bool gdiOk = ClipGenieCanvasToDesktop(boundLeft, boundTop, boundRight, boundBottom);

    int vLeft = boundLeft;
    int vTop = boundTop;
    int vWidth = boundRight - boundLeft;
    int vHeight = (boundBottom - boundTop) - 1;
    if (vWidth < 1) vWidth = 1;
    if (vHeight < 1) vHeight = 1;

    // GDI side of the render target. Any allocation failing (out of memory or GDI
    // objects) skips the D2D setup and the frame loop below; the common teardown
    // still completes the operation.
    HDC hScreenDC = NULL;
    HDC hMemDC = NULL;
    void* pTargetBits = nullptr;
    HBITMAP hTargetBmp = NULL;
    HBITMAP hOldBmp = NULL;
    HWND hGhost = NULL;
    if (gdiOk) {
        hScreenDC = GetDC(NULL);
        if (hScreenDC) hMemDC = CreateCompatibleDC(hScreenDC);

        BITMAPINFO bmi = {{0}};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = vWidth;
        bmi.bmiHeader.biHeight = -vHeight;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        if (hMemDC) hTargetBmp = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pTargetBits, NULL, 0);
        if (hTargetBmp) hOldBmp = (HBITMAP)SelectObject(hMemDC, hTargetBmp);

        // Full-screen layered ghost. Created hidden (plain STATIC class, torn down
        // by this thread) so we keep MINE's in-thread ghost lifecycle instead of
        // HIS's registered window class + g_activeGhosts vector.
        hGhost = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
            L"STATIC", NULL, WS_POPUP,
            vLeft, vTop, vWidth, vHeight,
            NULL, NULL, NULL, NULL);

        gdiOk = hScreenDC && hMemDC && hTargetBmp && pTargetBits && hOldBmp && hGhost;
    }

    // --- Direct2D setup (Potassiumuncher's engine) ---
    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1Bitmap* snapshotBmp = nullptr;
    ID2D1BitmapBrush* bmpBrush = nullptr;

    if (g_d2dFactory && gdiOk) {
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE, D2D1_FEATURE_LEVEL_DEFAULT
        );
        g_d2dFactory->CreateDCRenderTarget(&rtProps, &rt);
        if (rt) {
            // Potassiumuncher's v1.5: text AA fixed once at creation (the geometry
            // AA mode is set per frame in the draw loop below).
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );
            rt->CreateBitmap(D2D1::SizeU(data->width, data->height), data->pBits, data->width * 4, bmpProps, &snapshotBmp);
            if (snapshotBmp) {
                D2D1_BITMAP_BRUSH_PROPERTIES brushProps = D2D1::BitmapBrushProperties(
                    D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                );
                rt->CreateBitmapBrush(snapshotBmp, &brushProps, nullptr, &bmpBrush);
            }
        }
    }
    bool d2dOk = (gdiOk && rt && bmpBrush);

    const double animDur = (double)data->durationMs;
    const int speedCurve = g_speedCurve.load(std::memory_order_relaxed);
    const bool lampSmooth = g_lampSmooth.load(std::memory_order_relaxed);

    std::vector<std::vector<D2D1_POINT_2F>> grid(yTiles + 1, std::vector<D2D1_POINT_2F>(xTiles + 1));

    ID2D1PathGeometry* cachedOutlineGeo = nullptr;
    float cachedLeft = -1e9f, cachedRight = -1e9f, cachedTop = -1e9f, cachedBottom = -1e9f;

    BOOL firstFrame = TRUE;

    // Frame pacing (vblank or legacy, see MacGeniePacer). Never a per-frame
    // DwmFlush: that serialized render time + vsync and halved delivered frames.
    MacGeniePacer pacer;

    if (d2dOk) {
        for (;;) {
            double elapsedMs;
            if (!pacer.BeginFrame(&elapsedMs)) {
                if (g_unloading.load(std::memory_order_relaxed)) break;
                continue;
            }
            BOOL lastFrame = (elapsedMs >= animDur);

            float raw_p = (float)fmin(elapsedMs / animDur, 1.0);
            float eased_p = MacGenieEase(raw_p, speedCurve);
            // Minimize runs 0->1, restore runs 1->0 so the same warp plays in reverse.
            float t = data->isRising ? (1.0f - eased_p) : eased_p;

            RECT bindRect = { 0, 0, vWidth, vHeight };
            rt->BindDC(hMemDC, &bindRect);
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

            for (int y = 0; y <= yTiles; y++) {
                for (int x = 0; x <= xTiles; x++) {
                    float tx = (float)x / xTiles, ty = (float)y / yTiles;
                    float px, py;
                    // Warp this mesh vertex. CalculateLampVertexMacOS returns SCREEN
                    // (virtual-desktop) coordinates - wGeom / iGeom are both
                    // screen-space (targetRect, targetDockX, taskbar-top Y are all
                    // virtual-screen). The ONLY conversion to the ghost's
                    // bounding-box canvas is the (px - vLeft, py - vTop)
                    // subtraction below - vLeft/vTop are negative on monitors left of
                    // / above the primary. Every mesh coordinate passes through here
                    // exactly once; nothing downstream re-offsets.
                    CalculateLampVertex(tx, ty, t, wGeom, iGeom, &px, &py, lampSmooth);
                    grid[y][x] = D2D1::Point2F(px - vLeft, py - vTop);
                }
            }

            bool degenerate = false;
            ID2D1PathGeometry* outlineGeo = nullptr;
            {
                float left   = grid[0][0].x, right  = grid[0][0].x;
                float top    = grid[0][0].y, bottom = grid[0][0].y;
                for (int gy = 0; gy <= yTiles; gy++) {
                    for (int gx = 0; gx <= xTiles; gx++) {
                        left   = fminf(left,   grid[gy][gx].x);
                        right  = fmaxf(right,  grid[gy][gx].x);
                        top    = fminf(top,    grid[gy][gx].y);
                        bottom = fmaxf(bottom, grid[gy][gx].y);
                    }
                }

                // Belt-and-suspenders guard: if the warped mesh has collapsed to a
                // near-zero-width/height box, don't present it - that is what would
                // flash as a single horizontal line for one frame (seen at the tail
                // of the collapse, and at the very start of a restore where the mesh
                // emerges from the dock). Skipping these frames removes the flash
                // regardless of monitor / arrangement.
                degenerate = (right - left) < 2.0f || (bottom - top) < 2.0f;

                bool needRebuild = (cachedOutlineGeo == nullptr)
                    || fabsf(left   - cachedLeft)   > 0.5f
                    || fabsf(right  - cachedRight)  > 0.5f
                    || fabsf(top    - cachedTop)    > 0.5f
                    || fabsf(bottom - cachedBottom) > 0.5f;

                if (needRebuild) {
                    if (cachedOutlineGeo) { cachedOutlineGeo->Release(); cachedOutlineGeo = nullptr; }
                    cachedLeft = left; cachedRight = right; cachedTop = top; cachedBottom = bottom;

                    float w = right - left;
                    float h = bottom - top;
                    float r = fminf(12.0f, fminf(w, h) / 2.0f);   // Potassiumuncher's v1.5 radius
                    const float K = 0.5523f;
                    float L = left, T = top, R = right, B = bottom;

                    g_d2dFactory->CreatePathGeometry(&cachedOutlineGeo);
                    ID2D1GeometrySink* sink = nullptr;
                    if (cachedOutlineGeo &&
                        (FAILED(cachedOutlineGeo->Open(&sink)) || !sink)) {
                        // Out of memory: no outline this frame (nothing is drawn),
                        // rebuilt on the next one.
                        if (sink) sink->Release();
                        cachedOutlineGeo->Release();
                        cachedOutlineGeo = nullptr;
                    }
                    if (cachedOutlineGeo) {
                        sink->BeginFigure(D2D1::Point2F(L + r, T), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddLine(D2D1::Point2F(R - r, T));
                        { D2D1_BEZIER_SEGMENT s;
                          s.point1 = D2D1::Point2F(R - r + K*r, T);
                          s.point2 = D2D1::Point2F(R, T + r - K*r);
                          s.point3 = D2D1::Point2F(R, T + r);
                          sink->AddBezier(s); }
                        sink->AddLine(D2D1::Point2F(R, B - r));
                        { D2D1_BEZIER_SEGMENT s;
                          s.point1 = D2D1::Point2F(R, B - r + K*r);
                          s.point2 = D2D1::Point2F(R - r + K*r, B);
                          s.point3 = D2D1::Point2F(R - r, B);
                          sink->AddBezier(s); }
                        sink->AddLine(D2D1::Point2F(L + r, B));
                        { D2D1_BEZIER_SEGMENT s;
                          s.point1 = D2D1::Point2F(L + r - K*r, B);
                          s.point2 = D2D1::Point2F(L, B - r + K*r);
                          s.point3 = D2D1::Point2F(L, B - r);
                          sink->AddBezier(s); }
                        sink->AddLine(D2D1::Point2F(L, T + r));
                        { D2D1_BEZIER_SEGMENT s;
                          s.point1 = D2D1::Point2F(L, T + r - K*r);
                          s.point2 = D2D1::Point2F(L + r - K*r, T);
                          s.point3 = D2D1::Point2F(L + r, T);
                          sink->AddBezier(s); }
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        sink->Release();
                    }
                }
                outlineGeo = cachedOutlineGeo;
            }

            if (outlineGeo) {
                D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters();
                layerParams.geometricMask = outlineGeo;
                // Per-primitive AA on the outline mask (Potassiumuncher's v1.5):
                // smooth silhouette edges without per-tile seam artifacts.
                layerParams.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
                // nullptr layer: D2D manages its own layer pool, avoiding a fresh
                // ID2D1Layer create/release on every frame.
                rt->PushLayer(&layerParams, nullptr);

                for (int y = 0; y < yTiles; y++) {
                    for (int x = 0; x < xTiles; x++) {
                        D2D1_POINT_2F p1 = grid[y][x];
                        D2D1_POINT_2F p2 = grid[y][x+1];
                        D2D1_POINT_2F p3 = grid[y+1][x];
                        D2D1_POINT_2F p4 = grid[y+1][x+1];

                        D2D1_POINT_2F c = D2D1::Point2F((p1.x+p2.x+p3.x+p4.x)/4.0f, (p1.y+p2.y+p3.y+p4.y)/4.0f);

                        float quadW = fmaxf(fabsf(p2.x - p1.x), fabsf(p4.x - p3.x));
                        float quadH = fmaxf(fabsf(p3.y - p1.y), fabsf(p4.y - p2.y));
                        // Potassiumuncher's v1.5 bloat: slightly larger overlap so
                        // tile seams stay closed at higher tile counts.
                        float bloatAmt = fminf(quadW, quadH) * 0.05f;
                        bloatAmt = fmaxf(0.20f, fminf(0.40f, bloatAmt));

                        ID2D1PathGeometry* quadGeo = CreateQuadGeo(g_d2dFactory,
                            BloatPoint(p1, c, bloatAmt), BloatPoint(p2, c, bloatAmt),
                            BloatPoint(p4, c, bloatAmt), BloatPoint(p3, c, bloatAmt));

                        if (quadGeo) {
                            float sx = ((float)x / xTiles) * wGeom.width;
                            float sy = ((float)y / yTiles) * wGeom.height;
                            float sw = wGeom.width / xTiles;
                            float sh = wGeom.height / yTiles;

                            float m11 = (p2.x - p1.x) / sw;
                            float m12 = (p2.y - p1.y) / sw;
                            float m21 = (p3.x - p1.x) / sh;
                            float m22 = (p3.y - p1.y) / sh;
                            float m31 = p1.x - sx * m11 - sy * m21;
                            float m32 = p1.y - sx * m12 - sy * m22;

                            bmpBrush->SetTransform(D2D1::Matrix3x2F(m11, m12, m21, m22, m31, m32));
                            rt->FillGeometry(quadGeo, bmpBrush);
                            quadGeo->Release();
                        }
                    }
                }
                rt->PopLayer();
            }
            rt->EndDraw();

            // Only present / reveal the ghost on a non-degenerate frame, so a
            // collapsed mesh never flashes as a line. The ghost stays hidden until
            // the first real frame (restore) and simply holds its last good frame if
            // the tail collapses (minimize) - both invisible to the eye.
            if (!degenerate) {
                POINT ptDst = { vLeft, vTop };
                SIZE sz = { vWidth, vHeight };
                POINT ptSrc = { 0, 0 };
                BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                UpdateLayeredWindow(hGhost, hScreenDC, &ptDst, &sz, hMemDC, &ptSrc, 0, &bf, ULW_ALPHA);

                if (firstFrame) {
                    ShowWindow(hGhost, SW_SHOWNOACTIVATE);
                }
            }

            pacer.EndFrame();

            if (lastFrame) break;

            // Mod is being unloaded: cut the animation short. The teardown below
            // still reveals the real window (rising) and re-enables its DWM
            // transitions.
            if (g_unloading.load(std::memory_order_relaxed)) break;

            // The first real frame has been presented on the ghost. For a
            // minimize the hook is holding the REAL minimize (or, on the
            // auto-hide path, the cloak-hide) back until this moment - otherwise
            // the window would vanish a few frames before the genie appears.
            // One DwmFlush here guarantees it is composed on screen before the
            // handoff; every subsequent frame is capped only by the pacer below.
            if (firstFrame && !degenerate) {
                DwmFlush();
                firstFrame = FALSE;
                if (data->hFirstFrameShown) SetEvent(data->hFirstFrameShown);
            }

            pacer.Wait();
        }
    }

    pacer.Log(data, L"modern");
    if (cachedOutlineGeo) { cachedOutlineGeo->Release(); cachedOutlineGeo = nullptr; }

    // Show the real window again before the ghost goes away (no gap between them).
    MacGenieRevealRealWindow(data);

    if (bmpBrush) { bmpBrush->Release(); bmpBrush = nullptr; }
    if (snapshotBmp) { snapshotBmp->Release(); snapshotBmp = nullptr; }
    if (rt) { rt->Release(); rt = nullptr; }

    // Only what was actually created is freed (render setup can stop part-way).
    if (hMemDC) {
        if (hOldBmp) SelectObject(hMemDC, hOldBmp);
        DeleteDC(hMemDC);
    }
    if (hTargetBmp) DeleteObject(hTargetBmp);
    if (hScreenDC) ReleaseDC(NULL, hScreenDC);
    if (hGhost) DestroyWindow(hGhost);

    MacGenieFinishAnim(data);
    return 0;
}

// -------------------- COMMON TEARDOWN (both engines) --------------------
// Run before the engine destroys its ghost: reveal the real window the same way it
// was hidden (restore / launch) and give DWM transitions back - except on the
// auto-hide deferred path, which keeps them off through the real minimize in
// MacGenieFinishAnim so the system's own minimize animation never plays.
static void MacGenieRevealRealWindow(MacGenieAnimData* data) {
    if (data->isRising) {
        if (data->hiddenByCloak) {
            MacGenieSetCloak(data->hRealWnd, FALSE);
        } else {
            SetLayeredWindowAttributes(data->hRealWnd, 0, 255, LWA_ALPHA);
            if (!(data->originalExStyle & WS_EX_LAYERED)) {
                SetWindowLongPtrW(data->hRealWnd, GWL_EXSTYLE, data->originalExStyle);
            }
        }
    }
    if (!data->deferredMinimize) {
        MacGenieSetDwmTransitions(data->hRealWnd, TRUE);
    }
}

// Run last, after the engine freed its own resources: free the snapshot, release
// the minimize hook, finish the auto-hide deferred minimize and end the animation.
// Deletes data.
static void MacGenieFinishAnim(MacGenieAnimData* data) {
    DeleteObject(data->hBitmap);   // frees data->pBits (DIB section)

    // Unblock the waiting minimize hook (in case we bailed before the first frame
    // was ever composed), then release the event (this thread owns it).
    if (data->hFirstFrameShown) {
        SetEvent(data->hFirstFrameShown);
        CloseHandle(data->hFirstFrameShown);
    }

    // Auto-hide "unhide" completion - Potassiumuncher's engine, reconciled with
    // MINE's cloak hide. The caller cloaked the real window after our first frame
    // (flash-free, unlike HIS's WS_EX_LAYERED+alpha hide) and deferred the real
    // minimize to us so the revealed taskbar didn't slide away mid-animation.
    if (data->deferredMinimize) {
        int unhideMs = data->unhideDurationMs;
        int animMs = data->durationMs;
        if (data->requestedUnhide && unhideMs > animMs) {
            // Chunked sleep so an unload (which joins this thread) never waits out
            // the up-to-~5 s unhide delay.
            int remaining = unhideMs - animMs;
            while (remaining > 0 && !g_unloading.load(std::memory_order_relaxed)) {
                int chunk = remaining < 20 ? remaining : 20;
                Sleep(chunk);
                remaining -= chunk;
            }
        }

        // Perform the delayed minimize with transitions still disabled (instant, no
        // system animation). GenieBypass stops our own DefWindowProcW hook from
        // re-triggering the genie. Hardened with IsWindow guards (HIS's unhide path
        // could race a closed window / silent SetForegroundWindow failure).
        if (!g_unloading.load(std::memory_order_relaxed) && IsWindow(data->hRealWnd)) {
            SetPropW(data->hRealWnd, L"GenieBypass", (HANDLE)1);
            SendMessageTimeoutW(data->hRealWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_NORMAL, 1000, NULL);
            RemovePropW(data->hRealWnd, L"GenieBypass");
        }
        // Always uncloak so we never leave an invisible, non-minimized window, then
        // re-enable transitions for future system animations.
        if (IsWindow(data->hRealWnd)) {
            MacGenieSetCloak(data->hRealWnd, FALSE);
            MacGenieSetDwmTransitions(data->hRealWnd, TRUE);
        }
        // Shift focus so the taskbar slides back down natively.
        if (data->requestedUnhide && !g_unloading.load(std::memory_order_relaxed)) {
            if (data->hNextApp && IsWindow(data->hNextApp) && IsWindowVisible(data->hNextApp)) {
                SetForegroundWindow(data->hNextApp);
            } else {
                SetForegroundWindow(FindWindowW(L"Progman", NULL));
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        g_AnimActive.erase(data->hRealWnd);
    }
    delete data;
}

// -------------------------------------------------------------------------
// CLASSIC engine: the original (v2.2.0) per-output-scanline GDI strip warp from
// MacOS Minimize Animation 3.2.1, its default style. Every frame maps each source
// row to a screen row (rows nearest the dock lead), squeezes it toward the neck
// and copies the pixels into one DIB - no geometry objects, no GPU readback - so
// it fits in a refresh even on fast displays. Windows-OCT: it funnels into the
// Dock icon when the dock reported one, and uses the shared frame pacer.
// -------------------------------------------------------------------------
DWORD WINAPI MacGenieAnimThreadClassic(LPVOID lpParam) {
    MacGenieAnimData* data = (MacGenieAnimData*)lpParam;
    MacGenieDpiScope dpiScope;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    const int W = data->width;
    const int H = data->height;
    // The monitor the genie plays on (primary, or the window's with multi-monitor).
    RECT mon;
    MONITORINFO mmi;
    mmi.cbSize = sizeof(mmi);
    if (data->hMon && GetMonitorInfoW(data->hMon, &mmi)) {
        mon = mmi.rcMonitor;
    } else {
        mon.left = 0; mon.top = 0;
        mon.right = GetSystemMetrics(SM_CXSCREEN);
        mon.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    const int origLeft = data->targetRect.left;
    const int origTop  = data->targetRect.top;
    const float origCenterX = (float)origLeft + W * 0.5f;

    // Where the genie funnels to and how narrow its neck gets there.
    int dockX = data->targetDockX;
    float dockY;
    float neckW;
    if (data->hasDockIcon) {
        // Windows-OCT Dock: into the icon as drawn, the neck a bit narrower than the
        // icon and the collapse line a third of the way down it (as the modern style).
        const float iconW = (float)(data->dockIcon.right - data->dockIcon.left);
        const float iconH = (float)(data->dockIcon.bottom - data->dockIcon.top);
        dockY = (float)data->dockIcon.top + iconH * MACGENIE_DOCK_LAND;
        neckW = iconW * MACGENIE_DOCK_NECK;
        if (neckW < 12.0f) neckW = 12.0f;
    } else {
        // The taskbar edge: bottom of the monitor, or its top when the taskbar sits
        // in the upper half.
        if (dockX < mon.left) dockX = mon.left;
        if (dockX > mon.right) dockX = mon.right;
        dockY = (float)mon.bottom;
        if (HWND hTray = FindTaskbarForMonitor(data->hMon)) {
            RECT tr;
            if (GetWindowRect(hTray, &tr) && tr.bottom > tr.top &&
                (tr.top + tr.bottom) < (mon.top + mon.bottom)) {
                dockY = (float)mon.top;
            }
        }
        neckW = W * 0.03f;
        if (neckW < 12.0f) neckW = 12.0f;
        if (neckW > 60.0f) neckW = 60.0f;
    }
    const float dockXf = (float)dockX;
    // Which end leads the morph, from the window's mid-line. The dock is kept
    // outside the window's own vertical span so the yb[] map below stays monotonic
    // (a hard requirement of the scanline walk).
    const float origBottomF = (float)(origTop + H);
    const bool dockAbove = dockY < ((float)origTop + origBottomF) * 0.5f;
    if (dockAbove) { if (dockY > (float)origTop) dockY = (float)origTop; }
    else           { if (dockY < origBottomF)    dockY = origBottomF; }

    // Bounding box the funnel can occupy, trimmed to the monitors involved but
    // never clipping the window rect or the dock column away.
    int boundLeft   = (origLeft < dockX ? origLeft : dockX) - W / 2;
    int boundRight  = ((origLeft + W) > dockX ? (origLeft + W) : dockX) + W / 2;
    int boundTop    = std::min<int>(origTop, (int)dockY);
    int boundBottom = std::max<int>(origTop + H, (int)ceilf(dockY));
    RECT monUnion = GetGenieMonitorUnion(data->targetRect, data->hMon);
    ClampGenieCanvas(boundLeft, boundTop, boundRight, boundBottom,
                     data->targetRect, dockX, monUnion);
    bool renderOk = ClipGenieCanvasToDesktop(boundLeft, boundTop, boundRight, boundBottom);
    int boundW = boundRight - boundLeft;
    int boundH = boundBottom - boundTop;
    if (boundW < 1) boundW = 1;
    if (boundH < 1) boundH = 1;

    // Source pixels: the snapshot itself (32-bit top-down W x H, stride W * 4).
    const BYTE* srcBits = (const BYTE*)data->pBits;
    const int srcStride = W * 4;

    // Any allocation failing (out of memory or GDI objects) skips the frame loop;
    // the common teardown still completes the operation.
    HWND hGhost = NULL;
    HDC hScreenDC = NULL;
    BYTE* pBits = NULL;
    HBITMAP hCanvas = NULL;
    HDC hCanvasDC = NULL;
    HBITMAP hOldCanvas = NULL;
    float* yb = nullptr;
    if (renderOk) {
        hGhost = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
            L"STATIC", NULL, WS_POPUP,
            boundLeft, boundTop, boundW, boundH,
            NULL, NULL, NULL, NULL);

        hScreenDC = GetDC(NULL);

        BITMAPINFO bmi;
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = boundW;
        bmi.bmiHeader.biHeight = -boundH;   // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        if (hScreenDC) {
            hCanvas = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, (void**)&pBits, NULL, 0);
            hCanvasDC = CreateCompatibleDC(hScreenDC);
        }
        if (hCanvas && hCanvasDC) hOldCanvas = (HBITMAP)SelectObject(hCanvasDC, hCanvas);

        yb = new (std::nothrow) float[H + 1];

        renderOk = hGhost && hScreenDC && hCanvas && pBits && hCanvasDC && hOldCanvas && yb;
    }
    const int canvasStride = boundW * 4;
    const size_t canvasBytes = (size_t)boundW * 4 * boundH;

    const float SPREAD = 0.65f;   // rows nearest the dock lead the morph
    const double totalMs = (double)data->durationMs;

    auto morphAt = [&](float v, float tt) -> float {
        float m = tt * (1.0f + SPREAD) - (1.0f - v) * SPREAD;
        if (m < 0.0f) m = 0.0f;
        if (m > 1.0f) m = 1.0f;
        return m * m * (3.0f - 2.0f * m);
    };

    MacGeniePacer pacer;

    BOOL firstFrame = TRUE;
    if (renderOk) {
        for (;;) {
            double elapsedMs;
            if (!pacer.BeginFrame(&elapsedMs)) {
                if (g_unloading.load(std::memory_order_relaxed)) break;
                continue;
            }
            BOOL lastFrame = (elapsedMs >= totalMs);
            float progress = lastFrame ? 1.0f : (float)(elapsedMs / totalMs);

            // 0 = full window at its place, 1 = collapsed at the dock; restore reverses.
            float tt = data->isRising ? (1.0f - progress) : progress;

            float fade = 1.0f;
            if (tt > 0.8f) fade = (1.0f - tt) / 0.2f;
            if (fade < 0.0f) fade = 0.0f;
            if (fade > 1.0f) fade = 1.0f;

            memset(pBits, 0, canvasBytes);

            // Vertical map: where each source row lands on screen this frame.
            for (int k = 0; k <= H; ++k) {
                float v = (float)k / (float)H;
                float e = morphAt(dockAbove ? (1.0f - v) : v, tt);
                float idY = (float)origTop + (float)H * v;
                yb[k] = idY + (dockY - idY) * e;
            }

            int kSeg = 0;
            for (int yC = 0; yC < boundH; ++yC) {
                float screenY = (float)(yC + boundTop) + 0.5f;
                if (screenY < yb[0] || screenY >= yb[H]) continue;

                while (kSeg < H - 1 && yb[kSeg + 1] <= screenY) kSeg++;
                float segH = yb[kSeg + 1] - yb[kSeg];
                float frac = segH > 1e-4f ? (screenY - yb[kSeg]) / segH : 0.0f;
                float v = ((float)kSeg + frac) / (float)H;

                float em = morphAt(dockAbove ? (1.0f - v) : v, tt);
                float width = (float)W + (neckW - (float)W) * em;
                if (width < 1.0f) width = 1.0f;
                float cx = origCenterX + (dockXf - origCenterX) * em;
                float leftCanvas = (cx - width * 0.5f) - (float)boundLeft;

                int srcRow = (int)(v * (float)H);
                if (srcRow < 0) srcRow = 0;
                if (srcRow > H - 1) srcRow = H - 1;
                const BYTE* srcRowPtr = srcBits + (size_t)srcRow * srcStride;
                BYTE* dstRowPtr = pBits + (size_t)yC * canvasStride;

                int xStart = (int)leftCanvas;
                int xEnd   = (int)(leftCanvas + width) + 1;
                if (xStart < 0) xStart = 0;
                if (xEnd > boundW) xEnd = boundW;

                float invW = 1.0f / width;
                for (int xC = xStart; xC < xEnd; ++xC) {
                    float u = ((float)xC + 0.5f - leftCanvas) * invW;
                    if (u < 0.0f || u >= 1.0f) continue;
                    int srcX = (int)(u * (float)W);
                    if (srcX < 0) srcX = 0;
                    if (srcX > W - 1) srcX = W - 1;
                    // Premultiplied per-pixel alpha is copied as is, so translucent
                    // regions stay see-through; the fade applies on top below.
                    ((DWORD*)dstRowPtr)[xC] = ((const DWORD*)srcRowPtr)[srcX];
                }
            }

            POINT ptDst = { boundLeft, boundTop };
            SIZE  sz    = { boundW, boundH };
            POINT ptSrc = { 0, 0 };
            BLENDFUNCTION bf;
            bf.BlendOp = AC_SRC_OVER;
            bf.BlendFlags = 0;
            bf.SourceConstantAlpha = (BYTE)(255.0f * fade);
            bf.AlphaFormat = AC_SRC_ALPHA;
            UpdateLayeredWindow(hGhost, hScreenDC, &ptDst, &sz, hCanvasDC, &ptSrc, 0, &bf, ULW_ALPHA);

            if (firstFrame) {
                ShowWindow(hGhost, SW_SHOWNOACTIVATE);
            }

            pacer.EndFrame();

            if (lastFrame) break;
            if (g_unloading.load(std::memory_order_relaxed)) break;

            // First frame is on screen; release the minimize hook it was holding.
            if (firstFrame) {
                DwmFlush();
                firstFrame = FALSE;
                if (data->hFirstFrameShown) SetEvent(data->hFirstFrameShown);
            }

            pacer.Wait();
        }
    }

    pacer.Log(data, L"classic");

    MacGenieRevealRealWindow(data);

    // Only what was actually created is freed (render setup can stop part-way).
    delete[] yb;
    if (hCanvasDC) {
        if (hOldCanvas) SelectObject(hCanvasDC, hOldCanvas);
        DeleteDC(hCanvasDC);
    }
    if (hCanvas) DeleteObject(hCanvas);
    if (hScreenDC) ReleaseDC(NULL, hScreenDC);
    if (hGhost) DestroyWindow(hGhost);

    MacGenieFinishAnim(data);
    return 0;
}

// -------------------------------------------------------------------------
// Core Setup Engine & Smart Tracking Logic
// -------------------------------------------------------------------------
// Returns TRUE if the worker thread was spawned (the animation is now in flight).
// `originalExStyle` is the window's TRUE extended style, captured by the caller
// BEFORE any WS_EX_LAYERED was added for the hide (rising callers hide first).
// `cloakHidden` says how a rising caller hid the window: TRUE = DWM cloak (every
// restore path), FALSE = WS_EX_LAYERED + alpha 0. The deferred* args drive the auto-hide
// "unhide" path (falling only): the caller cloaks the window after our first frame
// and we perform the real minimize at teardown.
bool StartMacGenieAnim(HWND hWnd, BOOL rising, LONG_PTR originalExStyle,
                       BOOL cloakHidden = FALSE, BOOL deferredMinimize = FALSE,
                       BOOL requestedUnhide = FALSE, HWND hNextApp = NULL) {
    MacGenieDpiScope dpiScope;
    LARGE_INTEGER prestartQpc;
    QueryPerformanceCounter(&prestartQpc);
    // Bound the snapshot cache: windows closed while minimized (the common
    // WM_DESTROY + PostQuitMessage path never reaches DefWindowProcW, where the
    // usual cleanup lives) would otherwise leave their full DIB behind until
    // unload. Cheap sweep at the single animation entry point.
    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        for (auto it = g_SnapshotCache.begin(); it != g_SnapshotCache.end();) {
            if (!IsWindow(it->first)) {
                DeleteObject(it->second.hBmp);
                it = g_SnapshotCache.erase(it);
            } else {
                ++it;
            }
        }
        // Same for the launch bookkeeping, so a reused HWND value is new again.
        std::erase_if(g_LaunchSeen, [](HWND w) { return !IsWindow(w); });
        std::erase_if(g_HiddenAt, [](auto const& e) { return !IsWindow(e.first); });
    }

    // "Excluded programs" gate - single choke point for every animation path
    // (all five minimize/restore hooks funnel through here).
    // Bail exactly like the other early-outs so hook-side state is restored: a
    // rising caller gets its hide undone, a falling caller gets DWM transitions
    // back so the DEFAULT system animation plays for the excluded app.
    if (MacGenieIsExcluded(hWnd)) {
        if (rising) MacGenieUndoRisingHide(hWnd, originalExStyle, cloakHidden);
        else MacGenieSetDwmTransitions(hWnd, TRUE);
        return false;
    }
    if (MacGenieIsDpiVirtualized(hWnd)) {
        if (rising) MacGenieUndoRisingHide(hWnd, originalExStyle, cloakHidden);
        else MacGenieSetDwmTransitions(hWnd, TRUE);
        return false;
    }

    // Animation geometry via DWM extended frame bounds (Goal D / bug #4670):
    // GetWindowRect returns the legacy frame (invisible resize borders / shadow),
    // which is offset a few px from where the pixels actually are on composited
    // windows. Snapshot AND warp both use the extended-frame rect, so keyboard /
    // AutoHotkey minimizes are pixel-aligned. Scheme from Potassiumuncher's engine.
    RECT winRect;
    if (!GetWindowRect(hWnd, &winRect)) {
        if (rising) MacGenieUndoRisingHide(hWnd, originalExStyle, cloakHidden);
        else MacGenieSetDwmTransitions(hWnd, TRUE);
        return false;
    }
    RECT rect = winRect, extRect;
    if (SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &extRect, sizeof(extRect)))) {
        rect = extRect;
    }
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    int offsetX = rect.left - winRect.left;
    int offsetY = rect.top - winRect.top;
    int rawW = winRect.right - winRect.left;
    int rawH = winRect.bottom - winRect.top;

    // A window too large to snapshot safely (see MACGENIE_MAX_CANVAS_PIXELS) gets
    // the system animation instead of a multi-hundred-MB allocation.
    if (w <= 0 || h <= 0 || rawW <= 0 || rawH <= 0 ||
        (long long)w * h > MACGENIE_MAX_CANVAS_PIXELS ||
        (long long)rawW * rawH > MACGENIE_MAX_CANVAS_PIXELS) {
        if (rising) MacGenieUndoRisingHide(hWnd, originalExStyle, cloakHidden);
        else MacGenieSetDwmTransitions(hWnd, TRUE);
        return false;
    }

    // One genie per window at a time (MINE). Any window we animate also counts as
    // "seen", so the launch path won't fire on it later. Refused while unloading.
    bool blocked = g_unloading.load(std::memory_order_relaxed);
    if (!blocked) {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        blocked = !g_AnimActive.insert(hWnd).second;
        if (!blocked) g_LaunchSeen.insert(hWnd);
    }
    if (blocked) {
        if (rising) {
            MacGenieUndoRisingHide(hWnd, originalExStyle, cloakHidden);
        } else {
            bool owned;
            {
                std::lock_guard<std::mutex> lock(g_CacheMutex);
                owned = g_AnimActive.count(hWnd) != 0;
            }
            if (!owned) MacGenieSetDwmTransitions(hWnd, TRUE);
        }
        return false;
    }

    // Monitor the genie targets. Multi-monitor support (experimental) targets the
    // window's monitor; otherwise the primary, which reproduces the old behavior.
    // A close always flows to an edge of the window's own monitor.
    bool multiMon = g_multiMonitor.load(std::memory_order_relaxed);
    HMONITOR hMon = multiMon
        ? MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) {
        mi.rcMonitor.left = 0; mi.rcMonitor.top = 0;
        mi.rcMonitor.right = GetSystemMetrics(SM_CXSCREEN);
        mi.rcMonitor.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int monWidth = mi.rcMonitor.right - mi.rcMonitor.left;

    // Default dock target before UIA (Potassiumuncher's engine): left-aligned
    // taskbars start near the monitor's left, centered ones at its middle.
    // A close doesn't target the taskbar (the render thread picks the edge from the
    // close-direction setting), so it skips the registry, cursor and UIA lookups.
    int learnedTargetX = mi.rcMonitor.left + monWidth / 2;
    // Windows-OCT Dock first: the icon exactly as drawn, magnification included.
    RECT dockIcon = {};
    double dockQueryMs = 0;
    const bool haveDockIcon =
        g_dockTargeting.load(std::memory_order_relaxed) &&
        MacGenieQueryDockIcon(hWnd, &dockIcon, &dockQueryMs) &&
        MonitorFromRect(&dockIcon, MONITOR_DEFAULTTONULL) == hMon;
    const wchar_t* targetSource = L"taskbar";
    if (haveDockIcon) {
        learnedTargetX = (dockIcon.left + dockIcon.right) / 2;
        targetSource = L"dock";
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        g_IconPositions[hWnd] = learnedTargetX;
    } else {
        DWORD alignVal = 1, dataSize = sizeof(alignVal);
        RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                     L"TaskbarAl", RRF_RT_REG_DWORD, NULL, &alignVal, &dataSize);
        learnedTargetX = (alignVal == 0) ? (mi.rcMonitor.left + 160)
                                         : (mi.rcMonitor.left + monWidth / 2);

        // --- Per-window targeting ---
        // UI Automation (GetTaskbarButtonX) matches taskbar buttons by title / process
        // name, which can't reliably tell apart several windows of the SAME process
        // (e.g. multiple Brave profiles): they tie on the process-name score and it
        // resolves them all to the first-opened window's button. So MINE's cursor +
        // per-window signal takes priority; UIA is only a fallback for a window we've
        // never seen minimized from the taskbar (frame 0 of the warp is the identity,
        // so targetDockX is only needed for later frames).
        //
        // 1. Cursor over the taskbar = the user clicked THIS window's own taskbar
        //    button, so its X is exactly the icon - the only fully reliable per-window
        //    signal when windows share a process. Learn it, keyed by HWND (never shared
        //    across sibling windows).
        POINT pt;
        GetCursorPos(&pt);
        RECT workArea;
        MONITORINFO cursorMi;
        cursorMi.cbSize = sizeof(cursorMi);
        if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &cursorMi)) {
            workArea = cursorMi.rcWork;
        } else {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        }

        if (!PtInRect(&workArea, pt)) {
            learnedTargetX = pt.x;                        // steal the click X
            targetSource = L"cursor";
            std::lock_guard<std::mutex> lock(g_CacheMutex);
            g_IconPositions[hWnd] = learnedTargetX;       // remember it for this window
        } else {
            // 2. Title-bar / keyboard minimize: locate the taskbar button via UI
            //    Automation EVERY time (Potassiumuncher's targeting). Reusing a
            //    per-window cache here made the button-minimize land past the icon when
            //    the cached spot was stale; running UIA fresh lands it on the icon.
            //    GetTaskbarButtonX still falls back to the per-window / per-process cache
            //    (including a position learned from an earlier taskbar click) internally
            //    if UIA can't match, so nothing is lost.
            learnedTargetX = GetTaskbarButtonX(hWnd, learnedTargetX, hMon);
        }
    }

    MacGenieAnimData* data = new MacGenieAnimData();
    data->hRealWnd = hWnd;
    data->targetRect = rect;
    data->hMon = hMon;
    data->width = w;
    data->height = h;
    data->isRising = rising;
    data->targetDockX = learnedTargetX;
    data->originalExStyle = originalExStyle;
    data->hiddenByCloak = cloakHidden;
    data->hFirstFrameShown = NULL;
    data->durationMs = (rising ? g_restoreMs : g_minimizeMs).load(std::memory_order_relaxed);
    data->requestedUnhide = requestedUnhide;
    data->hNextApp = hNextApp;
    data->unhideDurationMs = g_unhideDurationMs.load(std::memory_order_relaxed);
    data->deferredMinimize = deferredMinimize;
    data->pBits = nullptr;
    data->hBitmap = NULL;
    data->prestartMs = 0;
    data->targetSource = targetSource;
    data->dockQueryMs = dockQueryMs;
    data->hasDockIcon = haveDockIcon ? TRUE : FALSE;
    data->dockIcon = dockIcon;

    HDC hScreenDC = GetDC(NULL);

    // Premultiplied 32-bit top-down DIB - D2D's CreateBitmap reads pBits directly.
    BITMAPINFO bmi = {{0}};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    data->hBitmap = hScreenDC
        ? CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &data->pBits, NULL, 0)
        : NULL;

    // Undo everything set up so far (snapshot allocation or capture failed, e.g.
    // the process is out of memory or GDI objects): the hook then performs the
    // operation itself with the system animation.
    auto BailOut = [&]() {
        if (hScreenDC) ReleaseDC(NULL, hScreenDC);
        if (rising) MacGenieUndoRisingHide(hWnd, originalExStyle, cloakHidden);
        else MacGenieSetDwmTransitions(hWnd, TRUE);
        if (data->hBitmap) DeleteObject(data->hBitmap);
        delete data;
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        g_AnimActive.erase(hWnd);
    };

    if (!data->hBitmap || !data->pBits) {
        BailOut();
        return false;
    }

    // Copy a rawW x rawH PrintWindow capture into the w x h snapshot at the
    // extended-frame offset. Potassiumuncher's v1.5 grey-screen fix: PrintWindow
    // (PW_RENDERFULLCONTENT) returns pixels that are ALREADY premultiplied, so pass
    // them through untouched and keep a==0 pixels fully TRANSPARENT. The old code
    // forced every pixel opaque, which flattened acrylic / Mica backdrop regions
    // (alpha 0 or partial) into a solid grey - the "grey top" on translucent
    // windows. With alpha preserved, those regions stay see-through during the
    // genie and the desktop shows through them, matching the window's real look.
    auto CopySnapshot = [&](void* srcBits) {
        DWORD* src = (DWORD*)srcBits;
        DWORD* dst = (DWORD*)data->pBits;
        int inBounds = 0, zeroAlpha = 0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int sx = x + offsetX;
                int sy = y + offsetY;
                if (sx >= 0 && sx < rawW && sy >= 0 && sy < rawH) {
                    DWORD p = src[sy * rawW + sx];
                    BYTE a = (BYTE)((p >> 24) & 0xFF);
                    inBounds++;
                    if (a == 0) zeroAlpha++;
                    dst[y * w + x] = (a == 0) ? 0 : p;
                } else {
                    dst[y * w + x] = 0;
                }
            }
        }
        // Fallback: on NON-PRIMARY monitors DWM often returns the entire Mica /
        // acrylic backdrop region as a==0 - with alpha preserved the whole window
        // background would simply VANISH during the genie (reported on Explorer on
        // a secondary display). If most of the window came back fully transparent,
        // the alpha channel clearly isn't describing real translucency, so force
        // those pixels opaque - restoring the pre-3.1 dark-backdrop look for this
        // case, which beats a see-through hole. Windows where transparency is
        // sparse (rounded corners, small blur regions) keep the faithful alpha.
        if (inBounds > 0 && zeroAlpha * 2 > inBounds) {
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int sx = x + offsetX;
                    int sy = y + offsetY;
                    if (sx >= 0 && sx < rawW && sy >= 0 && sy < rawH) {
                        DWORD p = src[sy * rawW + sx];
                        if (((p >> 24) & 0xFF) == 0) {
                            dst[y * w + x] = 0xFF000000u | (p & 0x00FFFFFFu);
                        }
                    }
                }
            }
        }
    };

    // Window-only capture: grab the window into a rawW x rawH DIB via PrintWindow,
    // then CopySnapshot into the extended-frame snapshot. PrintWindow renders only
    // the window's own content, so the taskbar / other windows can never bleed into
    // the animation (Potassiumuncher's taskbar fix) - and it also works on a
    // cloaked window (the rising-no-cache path), where a screen grab would not.
    auto CaptureNow = [&]() -> bool {
        HDC hTempDC = CreateCompatibleDC(hScreenDC);
        if (!hTempDC) return false;
        BITMAPINFO bmiTemp = bmi;
        bmiTemp.bmiHeader.biWidth = rawW;
        bmiTemp.bmiHeader.biHeight = -rawH;
        void* pTempBits = nullptr;
        HBITMAP hTempBmp = CreateDIBSection(hScreenDC, &bmiTemp, DIB_RGB_COLORS, &pTempBits, NULL, 0);
        if (!hTempBmp || !pTempBits) {
            if (hTempBmp) DeleteObject(hTempBmp);
            DeleteDC(hTempDC);
            return false;
        }
        HBITMAP hOldTempBmp = (HBITMAP)SelectObject(hTempDC, hTempBmp);
        if (!hOldTempBmp) {
            DeleteObject(hTempBmp);
            DeleteDC(hTempDC);
            return false;
        }
        PrintWindow(hWnd, hTempDC, PW_RENDERFULLCONTENT);
        GdiFlush();
        CopySnapshot(pTempBits);
        SelectObject(hTempDC, hOldTempBmp);
        DeleteObject(hTempBmp);
        DeleteDC(hTempDC);
        return true;
    };

    if (rising) {
        BOOL fromCache = FALSE;
        {
            std::lock_guard<std::mutex> lock(g_CacheMutex);
            auto it = g_SnapshotCache.find(hWnd);
            if (it != g_SnapshotCache.end()) {
                SnapCache& c = it->second;
                if (c.w == w && c.h == h) {
                    memcpy(data->pBits, c.pBits, (size_t)w * h * 4);
                    fromCache = TRUE;
                }
                DeleteObject(c.hBmp);
                g_SnapshotCache.erase(it);
            }
        }
        // No cached minimize snapshot (e.g. window was already minimized before the
        // mod loaded): snapshot the freshly-restored (cloaked) window directly.
        // A failed capture (out of memory / GDI objects) would animate an empty
        // snapshot, so the window is just revealed instead.
        if (!fromCache && !CaptureNow()) {
            BailOut();
            return false;
        }
    } else {
        // Falling: capture the WINDOW ONLY via PrintWindow (Potassiumuncher's
        // method), NOT a screen BitBlt. A screen grab of the window rect also pulls
        // in whatever is behind / around the window - most visibly the taskbar on
        // maximized / fullscreen apps, which then warps inside the genie. PrintWindow
        // renders just the window's own content, so the taskbar (a separate window)
        // can never bleed in. Tradeoff: DWM acrylic/translucency backdrops aren't
        // part of the window's own render, so translucent windows animate opaque - a
        // documented limitation, far less jarring than the taskbar appearing.
        // A failed capture falls back to the system animation, as above.
        if (!CaptureNow()) {
            BailOut();
            return false;
        }
        // Cache a copy keyed by HWND for the later restore animation (not for a
        // closing window - there's no restore after a close).
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        auto it = g_SnapshotCache.find(hWnd);
        if (it != g_SnapshotCache.end()) {
            DeleteObject(it->second.hBmp);
            g_SnapshotCache.erase(it);
        }
        void* pCacheBits = nullptr;
        // Not while unloading: Wh_ModUninit may already have cleared the cache, and
        // nothing would ever free a bitmap added after that.
        HBITMAP hCacheBmp = g_unloading.load(std::memory_order_relaxed) ? NULL :
            CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pCacheBits, NULL, 0);
        if (hCacheBmp && pCacheBits) {
            memcpy(pCacheBits, data->pBits, (size_t)w * h * 4);
            g_SnapshotCache[hWnd] = { hCacheBmp, pCacheBits, w, h };
        } else if (hCacheBmp) {
            DeleteObject(hCacheBmp);
        }
    }

    ReleaseDC(NULL, hScreenDC);

    // For a minimize (normal OR deferred) hold the caller back until the ghost's
    // first frame is composed - otherwise the window (or, on the deferred path, the
    // cloak-hide) would happen a few frames before the genie appears (a visible
    // gap, especially on power-throttled CPUs). Two handles to one event: the
    // thread owns its duplicate, we own and wait on the original.
    HANDLE hFirstShown = NULL;
    if (!rising) {
        hFirstShown = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (hFirstShown &&
            !DuplicateHandle(GetCurrentProcess(), hFirstShown, GetCurrentProcess(),
                             &data->hFirstFrameShown, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            data->hFirstFrameShown = NULL;
        }
    }
    bool waitForFirstFrame = (data->hFirstFrameShown != NULL);

    // Same setup and capture for both styles; only the render thread differs.
    {
        LARGE_INTEGER f, now;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&now);
        data->prestartMs = (double)(now.QuadPart - prestartQpc.QuadPart) * 1000.0 / f.QuadPart;
    }
    LPTHREAD_START_ROUTINE renderThread =
        g_classicEngine.load(std::memory_order_relaxed) ? MacGenieAnimThreadClassic
                                                        : MacGenieAnimThread;
    if (MacGenieSpawnWorker(renderThread, data)) {
        if (hFirstShown) {
            if (waitForFirstFrame) {
                WaitForSingleObject(hFirstShown, 200);   // capped: worst case = old behavior
            }
            CloseHandle(hFirstShown);
        }
        return true;
    }

    // Thread couldn't start (or the mod is unloading) - undo our state so the window
    // isn't left invisible or with transitions permanently disabled.
    if (rising) {
        MacGenieUndoRisingHide(hWnd, data->originalExStyle, data->hiddenByCloak);
    } else {
        MacGenieSetDwmTransitions(hWnd, TRUE);
    }
    if (hFirstShown) CloseHandle(hFirstShown);
    if (data->hFirstFrameShown) CloseHandle(data->hFirstFrameShown);
    DeleteObject(data->hBitmap);
    delete data;
    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        g_AnimActive.erase(hWnd);
    }
    return false;
}

// -------------------------------------------------------------------------
// App-launch animation (from MacOS Minimize Animation 3.2.1)
// -------------------------------------------------------------------------
DWORD WINAPI MacGenieLaunchThread(LPVOID lpParam);   // defined below

// Windows-OCT: a shown window hidden for at least this long and then shown again
// (back from the tray) gets the launch genie again. Shorter hides are an app
// rebuilding its window, not the user opening it.
static const ULONGLONG MACGENIE_RESHOW_MIN_HIDDEN_MS = 500;

// Remember when a shown launch-type window gets hidden (see g_HiddenAt).
static void MacGenieNoteHidden(HWND hWnd) {
    if (!IsWindowVisible(hWnd) || IsIconic(hWnd) || !MacGenieIsLaunchWindow(hWnd)) return;
    std::lock_guard<std::mutex> lock(g_CacheMutex);
    g_HiddenAt[hWnd] = GetTickCount64();
}

// Firefox-family browsers (Firefox, Zen, Floorp... all MozillaWindowClass): is
// hWnd the process's first browser window? See MacGenieLaunchPrepare.
struct MacGenieGeckoSearch { HWND self; DWORD pid; bool found; };

static BOOL CALLBACK MacGenieFindOtherGeckoWindow(HWND w, LPARAM lp) {
    MacGenieGeckoSearch* s = (MacGenieGeckoSearch*)lp;
    if (w == s->self || (!IsWindowVisible(w) && !IsIconic(w)) ||
        GetAncestor(w, GA_ROOT) != w) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    WCHAR cls[64];
    if (pid == s->pid && GetClassNameW(w, cls, ARRAYSIZE(cls)) &&
        _wcsicmp(cls, L"MozillaWindowClass") == 0) {
        s->found = true;
        return FALSE;
    }
    return TRUE;
}

static bool MacGenieIsFirstGeckoWindow(HWND hWnd) {
    WCHAR cls[64];
    if (!GetClassNameW(hWnd, cls, ARRAYSIZE(cls)) ||
        _wcsicmp(cls, L"MozillaWindowClass") != 0) {
        return false;
    }
    // The hooks run in whichever process makes the call, so match the window's
    // owner, not the calling process.
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    MacGenieGeckoSearch search = { hWnd, pid, false };
    EnumWindows(MacGenieFindOtherGeckoWindow, (LPARAM)&search);
    return !search.found;
}

// Arm the launch animation: check eligibility, kill DWM transition and hide the
// window *before* the real show call, so neither the window nor the system's open
// animation appears before the genie. Writes the TRUE pre-hide extended style to
// *origExOut and which hide was used to *cloakedOut.
//
// The hide is a DWM cloak, not WS_EX_LAYERED + alpha 0: winit / tao windows
// (Slint, Tauri) rewrite GWL_EXSTYLE right after the show and drop WS_EX_LAYERED,
// and Windows strips it from flip-model swap-chain and DirectComposition windows
// (WebView2, Skia/wgpu). PrintWindow still captures a cloaked window. The layered
// hide is kept where a cloak isn't appropriate:
//  - the first browser window of a Firefox-family process: a cloak before its
//    first show can make it open a second real window;
//  - a window the app already cloaked itself (WPF / WinUI white-flash
//    workarounds): our uncloak would reveal it early;
//  - a window DWM won't let us cloak.
static bool MacGenieLaunchPrepare(HWND hWnd, int nCmdShow, LONG_PTR* origExOut,
                                  BOOL* cloakedOut) {
    if (g_unloading.load(std::memory_order_relaxed)) return false;
    if (!g_launchAnimation.load(std::memory_order_relaxed)) return false;
    if (nCmdShow != SW_SHOW && nCmdShow != SW_SHOWNORMAL &&
        nCmdShow != SW_SHOWDEFAULT && nCmdShow != SW_SHOWMAXIMIZED) return false;
    if (IsWindowVisible(hWnd) || IsIconic(hWnd)) return false;
    if (!MacGenieIsLaunchWindow(hWnd)) return false;
    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        bool firstShow = g_LaunchSeen.insert(hWnd).second;
        auto hidden = g_HiddenAt.find(hWnd);
        bool backFromTray = hidden != g_HiddenAt.end() &&
                            GetTickCount64() - hidden->second >= MACGENIE_RESHOW_MIN_HIDDEN_MS;
        if (hidden != g_HiddenAt.end()) g_HiddenAt.erase(hidden);
        if (!firstShow && !backFromTray) return false;
    }
    // Excluded programs show untouched (StartMacGenieAnim would refuse them anyway,
    // but only after the hide had already delayed the window).
    if (MacGenieIsExcluded(hWnd)) return false;
    LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    *origExOut = exStyle;
    MacGenieSetDwmTransitions(hWnd, FALSE);

    UINT cloaked = 0;
    bool appCloaked =
        SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        (cloaked & DWM_CLOAKED_APP);
    BOOL cloak = TRUE;
    if (!appCloaked && !MacGenieIsFirstGeckoWindow(hWnd) &&
        SUCCEEDED(DwmSetWindowAttribute(hWnd, DWMWA_CLOAK, &cloak, sizeof(cloak)))) {
        *cloakedOut = TRUE;
        return true;
    }
    SetWindowLongPtrW(hWnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);
    *cloakedOut = FALSE;
    return true;
}

// Second half of the launch sequence: spawn the reveal worker (or roll back).
static void MacGenieLaunchCommit(HWND hWnd, LONG_PTR originalExStyle, BOOL cloaked) {
    MacGenieLaunchData* ld = new MacGenieLaunchData();
    ld->hWnd = hWnd;
    ld->originalExStyle = originalExStyle;
    ld->hiddenByCloak = cloaked;
    if (!MacGenieSpawnWorker(MacGenieLaunchThread, ld)) {
        delete ld;
        MacGenieUndoRisingHide(hWnd, originalExStyle, cloaked);
    }
}

DWORD WINAPI MacGenieLaunchThread(LPVOID lpParam) {
    MacGenieLaunchData* ld = (MacGenieLaunchData*)lpParam;
    HWND hWnd = ld->hWnd;
    LONG_PTR originalExStyle = ld->originalExStyle;
    BOOL hiddenByCloak = ld->hiddenByCloak;
    delete ld;

    // Let the freshly-shown (still hidden) window paint; UWP apps stay DWM-cloaked
    // by the shell behind their splash for a while - wait (bounded) for that cloak
    // to lift. When we cloaked the window ourselves, our cloak reports as
    // DWM_CLOAKED_APP, so that bit is masked out here.
    UINT ownCloak = hiddenByCloak ? (UINT)DWM_CLOAKED_APP : 0;
    Sleep(60);
    for (int i = 0; i < 30; ++i) {
        if (!IsWindow(hWnd) || g_unloading.load(std::memory_order_relaxed)) break;
        UINT cloaked = 0;
        if (FAILED(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) ||
            !(cloaked & ~ownCloak)) {
            break;
        }
        Sleep(50);
    }

    if (g_unloading.load(std::memory_order_relaxed) ||
        !IsWindow(hWnd) || IsIconic(hWnd) || !IsWindowVisible(hWnd)) {
        if (IsWindow(hWnd)) {
            MacGenieUndoRisingHide(hWnd, originalExStyle, hiddenByCloak);
        }
        return 0;
    }

    // Rising genie; reveals the real window at teardown the same way it was hidden.
    StartMacGenieAnim(hWnd, TRUE, originalExStyle, hiddenByCloak);
    return 0;
}

// -------------------------------------------------------------------------
// Hooks
// -------------------------------------------------------------------------
BOOL WINAPI ShowWindow_Hook(HWND hWnd, int nCmdShow) {
    if (nCmdShow == SW_HIDE) {
        MacGenieNoteHidden(hWnd);
        return ShowWindow_Original(hWnd, nCmdShow);
    }

    if (nCmdShow == SW_MINIMIZE || nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_SHOWMINNOACTIVE) {
        if (MacGenieShouldAnimate(hWnd)) {
            MacGenieSetDwmTransitions(hWnd, FALSE);
            StartMacGenieAnim(hWnd, FALSE, GetWindowLongPtrW(hWnd, GWL_EXSTYLE));
        }
        return ShowWindow_Original(hWnd, nCmdShow);
    }

    if ((nCmdShow == SW_RESTORE || nCmdShow == SW_SHOWNORMAL) && IsIconic(hWnd)) {
        if (g_openAnimation.load(std::memory_order_relaxed) && MacGenieShouldAnimate(hWnd)) {
            MacGenieSetDwmTransitions(hWnd, FALSE);
            // Cloak (frame-level hide, no layered-surface rebuild flash) - MINE.
            MacGenieSetCloak(hWnd, TRUE);
            BOOL res = ShowWindow_Original(hWnd, nCmdShow);
            StartMacGenieAnim(hWnd, TRUE, GetWindowLongPtrW(hWnd, GWL_EXSTYLE), TRUE);
            return res;
        }
        return ShowWindow_Original(hWnd, nCmdShow);
    }

    LONG_PTR launchOrigEx;
    BOOL launchCloaked;
    if (MacGenieLaunchPrepare(hWnd, nCmdShow, &launchOrigEx, &launchCloaked)) {
        BOOL res = ShowWindow_Original(hWnd, nCmdShow);
        MacGenieLaunchCommit(hWnd, launchOrigEx, launchCloaked);
        return res;
    }
    return ShowWindow_Original(hWnd, nCmdShow);
}

LRESULT WINAPI DefWindowProcW_Hook(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    if (Msg == WM_DESTROY) {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        auto it = g_SnapshotCache.find(hWnd);
        if (it != g_SnapshotCache.end()) {
            DeleteObject(it->second.hBmp);
            g_SnapshotCache.erase(it);
        }
        g_IconPositions.erase(hWnd);
        g_LaunchSeen.erase(hWnd);
        g_HiddenAt.erase(hWnd);
    }

    if (Msg == WM_SYSCOMMAND) {
        UINT cmd = wParam & 0xFFF0;
        if (cmd == SC_MINIMIZE) {
            // Our own deferred minimize (auto-hide path) re-enters here via
            // SendMessageTimeoutW - let it pass straight through, no recursion.
            if (GetPropW(hWnd, L"GenieBypass")) {
                return DefWindowProcW_Original(hWnd, Msg, wParam, lParam);
            }
            if (MacGenieShouldAnimate(hWnd)) {
                // Auto-hide taskbar? Reveal it, defer the real minimize until the
                // animation finishes so it doesn't slide away mid-genie.
                // (Potassiumuncher's engine, reconciled with MINE's cloak hide.)
                BOOL requestedUnhide = FALSE;
                HWND hNext = NULL;
                if (g_unhideEnabled.load(std::memory_order_relaxed)) {
                    HWND hTray = FindTaskbarForMonitor(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST));
                    if (hTray) {
                        APPBARDATA abd = { sizeof(APPBARDATA) };
                        abd.hWnd = hTray;
                        UINT uState = (UINT)SHAppBarMessage(ABM_GETSTATE, &abd);
                        if (uState & ABS_AUTOHIDE) {
                            requestedUnhide = TRUE;
                            HWND hwndIter = GetWindow(hWnd, GW_HWNDNEXT);
                            while (hwndIter) {
                                if (IsWindowVisible(hwndIter) && !IsIconic(hwndIter) &&
                                    GetAncestor(hwndIter, GA_ROOT) == hwndIter &&
                                    GetWindowTextLengthW(hwndIter) > 0 &&
                                    hwndIter != hWnd && hwndIter != hTray) {
                                    DWORD exStyle = GetWindowLongPtrW(hwndIter, GWL_EXSTYLE);
                                    if (!(exStyle & WS_EX_TOOLWINDOW)) { hNext = hwndIter; break; }
                                }
                                hwndIter = GetWindow(hwndIter, GW_HWNDNEXT);
                            }
                            SetForegroundWindow(hTray);
                        }
                    }
                }

                MacGenieSetDwmTransitions(hWnd, FALSE);
                if (requestedUnhide) {
                    // Deferred path: animate, then (in the worker) cloak-hide is
                    // applied by us here after the first frame, and the worker does
                    // the real minimize + focus juggle at the end.
                    bool ok = StartMacGenieAnim(hWnd, FALSE, GetWindowLongPtrW(hWnd, GWL_EXSTYLE),
                                                FALSE, TRUE, TRUE, hNext);
                    if (ok) {
                        // First frame is up (StartMacGenieAnim waited): hide the real
                        // window with a flash-free cloak; worker minimizes it later.
                        MacGenieSetCloak(hWnd, TRUE);
                        return 0;
                    }
                    // Spawn failed: fall back to a normal immediate minimize.
                    return DefWindowProcW_Original(hWnd, Msg, wParam, lParam);
                }
                // Normal minimize (MINE): first-frame gate, then real minimize.
                StartMacGenieAnim(hWnd, FALSE, GetWindowLongPtrW(hWnd, GWL_EXSTYLE));
            }
            return DefWindowProcW_Original(hWnd, Msg, wParam, lParam);
        }
        else if (cmd == SC_RESTORE && IsIconic(hWnd) &&
                 g_openAnimation.load(std::memory_order_relaxed) &&
                 MacGenieShouldAnimate(hWnd)) {
            MacGenieSetDwmTransitions(hWnd, FALSE);
            MacGenieSetCloak(hWnd, TRUE);
            LRESULT res = DefWindowProcW_Original(hWnd, Msg, wParam, lParam);
            StartMacGenieAnim(hWnd, TRUE, GetWindowLongPtrW(hWnd, GWL_EXSTYLE), TRUE);
            return res;
        }
    }
    return DefWindowProcW_Original(hWnd, Msg, wParam, lParam);
}

// Custom-title-bar apps (Zed/GPUI, some Electron) minimize via ShowWindowAsync,
// SetWindowPlacement, or CloseWindow. Shared minimize kick-off for those paths (no
// unhide handling - that lives on the SC_MINIMIZE path only).
static void MacGenieTryMinimizeAnim(HWND hWnd) {
    if (!IsWindowVisible(hWnd) || IsIconic(hWnd)) return;
    if (!MacGenieShouldAnimate(hWnd)) return;
    MacGenieSetDwmTransitions(hWnd, FALSE);
    StartMacGenieAnim(hWnd, FALSE, GetWindowLongPtrW(hWnd, GWL_EXSTYLE));
}

BOOL WINAPI ShowWindowAsync_Hook(HWND hWnd, int nCmdShow) {
    if (nCmdShow == SW_HIDE) {
        MacGenieNoteHidden(hWnd);
    } else if (nCmdShow == SW_MINIMIZE || nCmdShow == SW_SHOWMINIMIZED ||
               nCmdShow == SW_SHOWMINNOACTIVE) {
        MacGenieTryMinimizeAnim(hWnd);
    } else {
        LONG_PTR launchOrigEx;
        BOOL launchCloaked;
        if (MacGenieLaunchPrepare(hWnd, nCmdShow, &launchOrigEx, &launchCloaked)) {
            BOOL res = ShowWindowAsync_Original(hWnd, nCmdShow);
            MacGenieLaunchCommit(hWnd, launchOrigEx, launchCloaked);
            return res;
        }
    }
    return ShowWindowAsync_Original(hWnd, nCmdShow);
}

BOOL WINAPI SetWindowPlacement_Hook(HWND hWnd, const WINDOWPLACEMENT* lpwndpl) {
    if (lpwndpl && (lpwndpl->showCmd == SW_MINIMIZE ||
                    lpwndpl->showCmd == SW_SHOWMINIMIZED ||
                    lpwndpl->showCmd == SW_SHOWMINNOACTIVE)) {
        MacGenieTryMinimizeAnim(hWnd);
    }
    return SetWindowPlacement_Original(hWnd, lpwndpl);
}

BOOL WINAPI CloseWindow_Hook(HWND hWnd) {
    MacGenieTryMinimizeAnim(hWnd);
    return CloseWindow_Original(hWnd);
}

// Store / UWP apps show their first window via SetWindowPos + SWP_SHOWWINDOW.
BOOL WINAPI SetWindowPos_Hook(HWND hWnd, HWND hWndInsertAfter, int X, int Y,
                              int cx, int cy, UINT uFlags) {
    if (uFlags & SWP_HIDEWINDOW) {
        MacGenieNoteHidden(hWnd);
    } else if (uFlags & SWP_SHOWWINDOW) {
        LONG_PTR launchOrigEx;
        BOOL launchCloaked;
        if (MacGenieLaunchPrepare(hWnd, SW_SHOW, &launchOrigEx, &launchCloaked)) {
            BOOL res = SetWindowPos_Original(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
            MacGenieLaunchCommit(hWnd, launchOrigEx, launchCloaked);
            return res;
        }
    }
    return SetWindowPos_Original(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

BOOL Wh_ModInit() {
    MacGenieLoadSettings();

    // Direct2D factory for the genie renderer (Potassiumuncher's engine). Released
    // in Wh_ModUninit. If it fails, workers skip the animation gracefully.
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
                                   __uuidof(ID2D1Factory),
                                   reinterpret_cast<void**>(&g_d2dFactory));
    if (FAILED(hr)) g_d2dFactory = nullptr;

    Wh_SetFunctionHook((void*)DefWindowProcW, (void*)DefWindowProcW_Hook, (void**)&DefWindowProcW_Original);
    Wh_SetFunctionHook((void*)ShowWindow, (void*)ShowWindow_Hook, (void**)&ShowWindow_Original);
    Wh_SetFunctionHook((void*)ShowWindowAsync, (void*)ShowWindowAsync_Hook, (void**)&ShowWindowAsync_Original);
    Wh_SetFunctionHook((void*)SetWindowPlacement, (void*)SetWindowPlacement_Hook, (void**)&SetWindowPlacement_Original);
    Wh_SetFunctionHook((void*)CloseWindow, (void*)CloseWindow_Hook, (void**)&CloseWindow_Original);
    Wh_SetFunctionHook((void*)SetWindowPos, (void*)SetWindowPos_Hook, (void**)&SetWindowPos_Original);
    return TRUE;
}

void Wh_ModSettingsChanged() {
    MacGenieLoadSettings();
}

// Signal workers to wind down (the animation loop checks the flag every frame)
// and stop new ones from starting. They're joined in
// Wh_ModUninit, once the hooks are gone (see UNLOAD COORDINATION).
void Wh_ModBeforeUninit() {
    std::lock_guard<std::mutex> lock(g_WorkerThreadsMutex);
    g_unloading.store(true, std::memory_order_relaxed);
}

void Wh_ModUninit() {
    {
        // Normally already set by Wh_ModBeforeUninit; cheap insurance.
        std::lock_guard<std::mutex> lock(g_WorkerThreadsMutex);
        g_unloading.store(true, std::memory_order_relaxed);
    }
    // Join every worker before releasing anything they use (D2D factory, caches).
    // Not under g_CacheMutex: workers take it during their teardown.
    std::vector<HANDLE> threads;
    {
        std::lock_guard<std::mutex> lock(g_WorkerThreadsMutex);
        threads.swap(g_WorkerThreads);
    }
    for (HANDLE h : threads) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }

    if (g_d2dFactory) {
        g_d2dFactory->Release();
        g_d2dFactory = nullptr;
    }
    std::lock_guard<std::mutex> lock(g_CacheMutex);
    for (auto& pair : g_SnapshotCache) {
        DeleteObject(pair.second.hBmp);
    }
    g_SnapshotCache.clear();
    g_IconPositions.clear();
    g_ProcessIconPositions.clear();
    g_LaunchSeen.clear();
    g_HiddenAt.clear();
}
