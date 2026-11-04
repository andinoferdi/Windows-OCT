// ==WindhawkMod==
// @id              desktop-flip-clock
// @name            Desktop Glass Clock
// @description     Frosted-glass desktop clock that adapts to any wallpaper, any resolution and any display scale.
// @version         1.0.0
// @author          Levelup iD
// @include         explorer.exe
// @compilerOptions -lgdiplus -ldwmapi -lgdi32 -luser32 -lole32 -lshell32 -lshlwapi -lsecur32 -lnetapi32
// @architecture    x86-64
// ==/WindhawkMod==
//
// ==WindhawkModReadme==
/*
# Desktop Glass Clock

Jam desktop dengan panel **kaca frosted asli**: blur diambil dari wallpaper kamu sendiri,
jadi panelnya selalu menyatu dengan wallpaper apa pun.

## Fitur
- **Adaptif wallpaper**: warna aksen (titik dua, bar detik, ring) diambil otomatis dari
  warna dominan wallpaper. Kontras teks menyesuaikan di wallpaper terang maupun gelap.
  Ikut berubah otomatis saat wallpaper diganti (termasuk slideshow / Spotlight).
- **Adaptif layar**: ukuran menyesuaikan resolusi dan skala (100%, 125%, 150%, 4K, ultrawide, portrait).
- **Angka bergulir** halus setiap kali menit/jam berganti.
- **Sapaan**: "Selamat pagi/siang/sore/malam, <nama>" dengan ikon matahari/bulan.
- **Bar detik** dengan titik glow, dan **progress hari** (ring kecil di samping tanggal).
- Click-through, tidak mengganggu desktop. Otomatis berhenti render saat game/app fullscreen.

## Tips
- `Clock scale` = ukuran tambahan di atas skala otomatis (100 = ukuran ideal).
- `Accent color`: pilih *Auto dari wallpaper*, *Warna aksen Windows*, atau *Custom*.
- `Glass tint` lebih tinggi = panel lebih gelap/pekat, lebih rendah = lebih bening.
*/
// ==/WindhawkModReadme==
//
// ==WindhawkModSettings==
/*
- CenterVertically: true
  $name: Center on screen
  $description: On = tepat di tengah layar. Off = pakai Top offset.
- TopOffset: 80
  $name: Top offset
  $description: Jarak dari atas layar (dalam ukuran 1080p, ikut diskalakan). Hanya dipakai kalau Center on screen mati.
- ScalePercent: 100
  $name: Clock scale (%)
  $description: Ukuran jam di atas skala otomatis (50-200). 100 = ukuran ideal untuk layar apa pun.
- Use24Hour: true
  $name: Use 24-hour time
- ShowDate: true
  $name: Show date
- IndonesianDate: true
  $name: Bahasa Indonesia
  $description: Tanggal dan sapaan dalam bahasa Indonesia. Off = English.
- ShowGreeting: true
  $name: Show greeting
  $description: Tampilkan sapaan "Selamat pagi/siang/sore/malam, nama".
- CustomName: ""
  $name: Greeting name
  $description: Kosongkan untuk memakai nama akun Windows. Isi "-" untuk sapaan tanpa nama.
- ShowSeconds: true
  $name: Show seconds bar
- ShowDayProgress: true
  $name: Show day progress
  $description: Ring kecil di samping tanggal yang menunjukkan berapa persen hari ini sudah berjalan.
- AnimateDigits: true
  $name: Rolling digit animation
- ShowPanel: true
  $name: Show glass panel
  $description: Off = hanya angka dan teks melayang tanpa panel.
- AccentMode: wallpaper
  $name: Accent color
  $options:
  - wallpaper: Auto dari wallpaper
  - windows: Warna aksen Windows
  - custom: Custom (pakai Custom accent)
- CustomAccent: "#7CFFB2"
  $name: Custom accent
  $description: Warna hex #RRGGBB, dipakai kalau Accent color = Custom.
- GlassTint: 38
  $name: Glass tint (0-100)
  $description: 0 = kaca sangat bening, 100 = kaca gelap pekat. Otomatis ditambah sedikit di wallpaper terang.
- BlurStrength: 60
  $name: Blur strength (0-100)
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <lm.h>
#define SECURITY_WIN32
#include <security.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Gdiplus;

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static HINSTANCE g_hInstance = nullptr;
static HWND g_clockWindow = nullptr;
static ULONG_PTR g_gdiplusToken = 0;
static HANDLE g_thread = nullptr;
static DWORD g_threadId = 0;

static constexpr UINT WM_APP_REFRESH = WM_APP + 42;
static constexpr UINT_PTR kTimerId = 1;
static constexpr UINT_PTR kReloadTimerId = 2;
static constexpr float kPi = 3.14159265358979f;

enum class AccentMode { Wallpaper, Windows, Custom };

struct Settings {
    bool centerVertically = true;
    int topOffset = 80;
    int scalePercent = 100;
    bool use24Hour = true;
    bool showDate = true;
    bool indonesian = true;
    bool showGreeting = true;
    std::wstring customName;
    bool showSeconds = true;
    bool showDayProgress = true;
    bool animateDigits = true;
    bool showPanel = true;
    AccentMode accentMode = AccentMode::Wallpaper;
    COLORREF customAccent = RGB(124, 255, 178);
    int glassTint = 38;
    int blurStrength = 60;
};

static Settings g_settings;
static std::wstring g_userName;

struct Rgb {
    float r, g, b;  // 0..1
};

// Wallpaper rendered (downscaled) for the target monitor.
struct WallCanvas {
    bool valid = false;
    std::vector<uint32_t> px;  // ARGB, not premultiplied, opaque
    int w = 0, h = 0;
    float k = 1.0f;  // canvas px per screen px
    RECT monitor{};
};

// Colors derived from the wallpaper area behind the clock.
struct Palette {
    Rgb accent{0.49f, 1.0f, 0.70f};
    Rgb accent2{0.24f, 1.0f, 0.77f};
    Rgb tint{0.02f, 0.18f, 0.22f};
    Rgb average{0.1f, 0.3f, 0.3f};
    float avgLum = 0.3f;
};

struct Layout {
    float s = 1.0f;
    int winW = 0, winH = 0;
    POINT pos{};
    RECT monitor{};
    RectF panel;
    float radius = 30.0f;

    float greetY = 0, greetH = 0;
    float digitsY = 0, digitsH = 0;
    float barY = 0;
    float dateY = 0, dateH = 0;

    // Digit glyph cache.
    std::unique_ptr<GraphicsPath> digitPath[10];
    RectF digitBounds[10];
    float glyphTop = 0, glyphBottom = 0;  // union of digit bounds (path coords)
    float cellW = 0, innerGap = 0, colonGap = 0;
    float cellX[4]{};
    float colonX = 0;

    std::unique_ptr<FontFamily> textFamily;
    int textStyle = FontStyleRegular;
    std::unique_ptr<Font> greetFont;
    std::unique_ptr<Font> dateFont;
    std::unique_ptr<Font> smallFont;
};

static Layout g_layout;
static WallCanvas g_canvas;
static Palette g_palette;
static std::wstring g_wallKey;

static std::vector<uint32_t> g_static;  // premultiplied ARGB, winW*winH
static std::unique_ptr<Bitmap> g_glass;  // blurred wallpaper sized to panel

// Frame buffer.
static HDC g_memDc = nullptr;
static HBITMAP g_dib = nullptr;
static HGDIOBJ g_oldBitmap = nullptr;
static uint32_t* g_frameBits = nullptr;
static std::unique_ptr<Bitmap> g_frameBmp;

static bool g_layoutDirty = true;
static bool g_canvasDirty = true;
static bool g_glassDirty = true;
static bool g_staticDirty = true;
static bool g_forceWallCheck = true;
static bool g_paused = false;

static ULONGLONG g_lastWallCheck = 0;
static ULONGLONG g_lastFsCheck = 0;
static ULONGLONG g_fadeStart = 0;

struct DigitAnim {
    int cur = -1;
    int prev = -1;
    ULONGLONG start = 0;
};
static DigitAnim g_digitAnim[4];

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static float EaseOutCubic(float t) {
    t = Clamp01(t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static BYTE ToByte(float v01) { return static_cast<BYTE>(Clamp01(v01) * 255.0f + 0.5f); }

static Color MakeColor(float a01, const Rgb& c) {
    return Color(ToByte(a01), ToByte(c.r), ToByte(c.g), ToByte(c.b));
}

static Rgb Mix(const Rgb& a, const Rgb& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

static void RgbToHsl(const Rgb& c, float& h, float& s, float& l) {
    const float mx = (std::max)({c.r, c.g, c.b});
    const float mn = (std::min)({c.r, c.g, c.b});
    l = (mx + mn) * 0.5f;
    if (mx - mn < 1e-5f) {
        h = 0;
        s = 0;
        return;
    }
    const float d = mx - mn;
    s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == c.r) {
        h = (c.g - c.b) / d + (c.g < c.b ? 6.0f : 0.0f);
    } else if (mx == c.g) {
        h = (c.b - c.r) / d + 2.0f;
    } else {
        h = (c.r - c.g) / d + 4.0f;
    }
    h *= 60.0f;
}

static float HueToRgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 1.0f / 2) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

static Rgb HslToRgb(float h, float s, float l) {
    h = std::fmod(h, 360.0f);
    if (h < 0) h += 360.0f;
    if (s <= 0) return {l, l, l};
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    const float p = 2 * l - q;
    const float hk = h / 360.0f;
    return {HueToRgb(p, q, hk + 1.0f / 3), HueToRgb(p, q, hk), HueToRgb(p, q, hk - 1.0f / 3)};
}

static Rgb FromColorRef(COLORREF c) {
    return {GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f};
}

static bool ParseHexColor(const std::wstring& text, COLORREF& out) {
    std::wstring t = text;
    t.erase(std::remove_if(t.begin(), t.end(), [](wchar_t ch) { return ch == L' ' || ch == L'#'; }),
            t.end());
    if (t.size() != 6) return false;
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(t.c_str(), &end, 16);
    if (!end || *end) return false;
    out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return true;
}

static void AddRoundRect(GraphicsPath& path, const RectF& r, REAL radius) {
    radius = (std::min)(radius, (std::min)(r.Width, r.Height) / 2.0f);
    const REAL d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

static void AddCapsule(GraphicsPath& path, REAL x, REAL y, REAL w, REAL h) {
    if (w < h) w = h;
    path.AddArc(x, y, h, h, 90, 180);
    path.AddArc(x + w - h, y, h, h, 270, 180);
    path.CloseFigure();
}

// Box blur on 32-bit pixels (all four channels), edge-clamped. 3 passes ~ gaussian.
static void BoxBlurPass(const uint32_t* src, uint32_t* dst, int w, int h, int r, bool horizontal) {
    const int len = horizontal ? w : h;
    const int lines = horizontal ? h : w;
    const int step = horizontal ? 1 : w;
    const int win = 2 * r + 1;
    for (int line = 0; line < lines; ++line) {
        const uint32_t* s = horizontal ? src + static_cast<size_t>(line) * w : src + line;
        uint32_t* d = horizontal ? dst + static_cast<size_t>(line) * w : dst + line;
        auto at = [&](int i) -> uint32_t {
            i = i < 0 ? 0 : (i >= len ? len - 1 : i);
            return s[static_cast<size_t>(i) * step];
        };
        int sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
        for (int i = -r; i <= r; ++i) {
            const uint32_t p = at(i);
            sum0 += p & 0xFF;
            sum1 += (p >> 8) & 0xFF;
            sum2 += (p >> 16) & 0xFF;
            sum3 += (p >> 24) & 0xFF;
        }
        for (int i = 0; i < len; ++i) {
            d[static_cast<size_t>(i) * step] =
                static_cast<uint32_t>(sum0 / win) | (static_cast<uint32_t>(sum1 / win) << 8) |
                (static_cast<uint32_t>(sum2 / win) << 16) | (static_cast<uint32_t>(sum3 / win) << 24);
            const uint32_t a = at(i + r + 1);
            const uint32_t b = at(i - r);
            sum0 += static_cast<int>(a & 0xFF) - static_cast<int>(b & 0xFF);
            sum1 += static_cast<int>((a >> 8) & 0xFF) - static_cast<int>((b >> 8) & 0xFF);
            sum2 += static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF);
            sum3 += static_cast<int>((a >> 24) & 0xFF) - static_cast<int>((b >> 24) & 0xFF);
        }
    }
}

static void BoxBlur(std::vector<uint32_t>& buf, int w, int h, int r, int passes) {
    if (r <= 0 || w <= 0 || h <= 0) return;
    std::vector<uint32_t> tmp(buf.size());
    for (int p = 0; p < passes; ++p) {
        BoxBlurPass(buf.data(), tmp.data(), w, h, r, true);
        BoxBlurPass(tmp.data(), buf.data(), w, h, r, false);
    }
}

// ---------------------------------------------------------------------------
// Settings / user name
// ---------------------------------------------------------------------------

static std::wstring ReadStringSetting(PCWSTR name) {
    PCWSTR v = Wh_GetStringSetting(name);
    std::wstring out = v ? v : L"";
    Wh_FreeStringSetting(v);
    return out;
}

static void LoadSettings() {
    g_settings.centerVertically = Wh_GetIntSetting(L"CenterVertically") != 0;
    g_settings.topOffset = (std::max)(0, Wh_GetIntSetting(L"TopOffset"));
    g_settings.scalePercent = (std::clamp)(Wh_GetIntSetting(L"ScalePercent"), 50, 200);
    g_settings.use24Hour = Wh_GetIntSetting(L"Use24Hour") != 0;
    g_settings.showDate = Wh_GetIntSetting(L"ShowDate") != 0;
    g_settings.indonesian = Wh_GetIntSetting(L"IndonesianDate") != 0;
    g_settings.showGreeting = Wh_GetIntSetting(L"ShowGreeting") != 0;
    g_settings.customName = ReadStringSetting(L"CustomName");
    g_settings.showSeconds = Wh_GetIntSetting(L"ShowSeconds") != 0;
    g_settings.showDayProgress = Wh_GetIntSetting(L"ShowDayProgress") != 0;
    g_settings.animateDigits = Wh_GetIntSetting(L"AnimateDigits") != 0;
    g_settings.showPanel = Wh_GetIntSetting(L"ShowPanel") != 0;

    const std::wstring mode = ReadStringSetting(L"AccentMode");
    if (mode == L"windows") {
        g_settings.accentMode = AccentMode::Windows;
    } else if (mode == L"custom") {
        g_settings.accentMode = AccentMode::Custom;
    } else {
        g_settings.accentMode = AccentMode::Wallpaper;
    }

    COLORREF c;
    g_settings.customAccent =
        ParseHexColor(ReadStringSetting(L"CustomAccent"), c) ? c : RGB(124, 255, 178);

    g_settings.glassTint = (std::clamp)(Wh_GetIntSetting(L"GlassTint"), 0, 100);
    g_settings.blurStrength = (std::clamp)(Wh_GetIntSetting(L"BlurStrength"), 0, 100);
}

static std::wstring FirstWord(std::wstring s) {
    const size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return L"";
    s = s.substr(b);
    const size_t e = s.find_first_of(L" \t");
    return e == std::wstring::npos ? s : s.substr(0, e);
}

static std::wstring DetectUserName() {
    wchar_t buf[256]{};
    ULONG n = ARRAYSIZE(buf);
    if (GetUserNameExW(NameDisplay, buf, &n) && buf[0]) {
        return FirstWord(buf);
    }

    wchar_t user[257]{};
    DWORD un = ARRAYSIZE(user);
    if (!GetUserNameW(user, &un)) return L"";

    std::wstring full;
    USER_INFO_10* info = nullptr;
    if (NetUserGetInfo(nullptr, user, 10, reinterpret_cast<LPBYTE*>(&info)) == NERR_Success && info) {
        if (info->usri10_full_name && info->usri10_full_name[0]) full = info->usri10_full_name;
        NetApiBufferFree(info);
    }
    return FirstWord(full.empty() ? std::wstring(user) : full);
}

static std::wstring GreetingName() {
    if (g_settings.customName == L"-") return L"";
    if (!g_settings.customName.empty()) return g_settings.customName;
    return g_userName;
}

// 0 = morning, 1 = midday, 2 = afternoon/evening, 3 = night
static int DayPeriod(int hour) {
    if (g_settings.indonesian) {
        if (hour >= 4 && hour < 11) return 0;
        if (hour >= 11 && hour < 15) return 1;
        if (hour >= 15 && hour < 18) return 2;
        return 3;
    }
    if (hour >= 5 && hour < 12) return 0;
    if (hour >= 12 && hour < 17) return 1;
    if (hour >= 17 && hour < 21) return 2;
    return 3;
}

static std::wstring GreetingText(int period) {
    static const wchar_t* id[] = {L"Selamat pagi", L"Selamat siang", L"Selamat sore", L"Selamat malam"};
    static const wchar_t* en[] = {L"Good morning", L"Good afternoon", L"Good evening", L"Good night"};
    std::wstring text = g_settings.indonesian ? id[period] : en[period];
    const std::wstring name = GreetingName();
    if (!name.empty()) text += L", " + name;
    return text;
}

static std::wstring FormatDate(const SYSTEMTIME& st) {
    wchar_t buffer[128]{};
    if (g_settings.indonesian &&
        GetDateFormatEx(L"id-ID", 0, &st, L"dddd, d MMMM yyyy", buffer, ARRAYSIZE(buffer), nullptr)) {
        return buffer;
    }
    if (GetDateFormatEx(g_settings.indonesian ? LOCALE_NAME_USER_DEFAULT : L"en-US", 0, &st,
                        L"dddd, d MMMM yyyy", buffer, ARRAYSIZE(buffer), nullptr)) {
        return buffer;
    }
    swprintf_s(buffer, L"%02u/%02u/%04u", st.wDay, st.wMonth, st.wYear);
    return buffer;
}

// ---------------------------------------------------------------------------
// Monitor
// ---------------------------------------------------------------------------

static RECT PrimaryMonitorRect() {
    RECT r{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) r = mi.rcMonitor;
    return r;
}

// ---------------------------------------------------------------------------
// Wallpaper discovery + canvas
// ---------------------------------------------------------------------------

enum WallPos { kPosCenter = 0, kPosTile = 1, kPosStretch = 2, kPosFit = 3, kPosFill = 4, kPosSpan = 5 };

struct WallInfo {
    std::wstring path;
    int position = kPosFill;
    COLORREF background = RGB(0, 0, 0);
    RECT monitor{};
};

static const GUID kClsidDesktopWallpaper = {
    0xC2CF3110, 0x460E, 0x4FC1, {0xB9, 0xD0, 0x8A, 0x1C, 0x0C, 0x9C, 0xC4, 0xBD}};
static const GUID kIidDesktopWallpaper = {
    0xB92B56A9, 0x8B55, 0x4E14, {0x9A, 0x89, 0x01, 0x99, 0xBB, 0xB6, 0xF9, 0x3B}};

static bool FileExists(const std::wstring& p) {
    if (p.empty()) return false;
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring TranscodedWallpaperPath() {
    wchar_t buf[MAX_PATH]{};
    if (!ExpandEnvironmentStringsW(L"%APPDATA%\\Microsoft\\Windows\\Themes\\TranscodedWallpaper", buf,
                                   ARRAYSIZE(buf))) {
        return L"";
    }
    return buf;
}

static WallInfo QueryWallpaper(const RECT& monitor) {
    WallInfo info;
    info.monitor = monitor;
    info.background = GetSysColor(COLOR_DESKTOP);

    bool gotPos = false;
    IDesktopWallpaper* dw = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidDesktopWallpaper, nullptr, CLSCTX_ALL, kIidDesktopWallpaper,
                                   reinterpret_cast<void**>(&dw))) &&
        dw) {
        UINT count = 0;
        LPWSTR monitorId = nullptr;
        if (SUCCEEDED(dw->GetMonitorDevicePathCount(&count))) {
            for (UINT i = 0; i < count; ++i) {
                LPWSTR id = nullptr;
                if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) continue;
                RECT r{};
                if (SUCCEEDED(dw->GetMonitorRECT(id, &r)) && EqualRect(&r, &monitor)) {
                    monitorId = id;
                    break;
                }
                CoTaskMemFree(id);
            }
        }
        LPWSTR path = nullptr;
        if (SUCCEEDED(dw->GetWallpaper(monitorId, &path)) && path) {
            info.path = path;
            CoTaskMemFree(path);
        }
        if (monitorId) CoTaskMemFree(monitorId);

        DESKTOP_WALLPAPER_POSITION pos;
        if (SUCCEEDED(dw->GetPosition(&pos))) {
            info.position = static_cast<int>(pos);
            gotPos = true;
        }
        COLORREF bg;
        if (SUCCEEDED(dw->GetBackgroundColor(&bg))) info.background = bg;
        dw->Release();
    }

    if (info.path.empty()) {
        wchar_t buf[MAX_PATH]{};
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(buf), buf, 0)) info.path = buf;
    }

    if (!gotPos) {
        wchar_t style[16]{}, tile[16]{};
        DWORD cb = sizeof(style);
        RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", RRF_RT_REG_SZ,
                     nullptr, style, &cb);
        cb = sizeof(tile);
        RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper", RRF_RT_REG_SZ,
                     nullptr, tile, &cb);
        const int st = _wtoi(style);
        if (st == 10) info.position = kPosFill;
        else if (st == 6) info.position = kPosFit;
        else if (st == 2) info.position = kPosStretch;
        else if (st == 22) info.position = kPosSpan;
        else info.position = _wtoi(tile) ? kPosTile : kPosCenter;
    }

    // Slideshow / Spotlight / unsupported formats: Windows keeps a JPEG copy here.
    if (!FileExists(info.path)) info.path = TranscodedWallpaperPath();
    return info;
}

static std::wstring WallKey(const WallInfo& info) {
    ULONGLONG t = 0, size = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!info.path.empty() && GetFileAttributesExW(info.path.c_str(), GetFileExInfoStandard, &fad)) {
        t = (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
            fad.ftLastWriteTime.dwLowDateTime;
        size = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }
    wchar_t buf[128];
    swprintf_s(buf, L"|%d|%06X|%llu|%llu|%ld,%ld,%ld,%ld", info.position,
               static_cast<unsigned>(info.background), t, size, info.monitor.left, info.monitor.top,
               info.monitor.right, info.monitor.bottom);
    return info.path + buf;
}

static Bitmap* LoadImageFromFile(const std::wstring& path, IStream** streamOut) {
    *streamOut = nullptr;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    std::vector<BYTE> data;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 512ll * 1024 * 1024) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) ||
            read != data.size()) {
            data.clear();
        }
    }
    CloseHandle(f);
    if (data.empty()) return nullptr;

    IStream* stream = SHCreateMemStream(data.data(), static_cast<UINT>(data.size()));
    if (!stream) return nullptr;
    Bitmap* bmp = Bitmap::FromStream(stream);
    if (!bmp || bmp->GetLastStatus() != Ok || bmp->GetWidth() == 0 || bmp->GetHeight() == 0) {
        delete bmp;
        stream->Release();
        return nullptr;
    }
    *streamOut = stream;
    return bmp;
}

static bool BuildCanvas(const WallInfo& info) {
    g_canvas.valid = false;
    const int mw = info.monitor.right - info.monitor.left;
    const int mh = info.monitor.bottom - info.monitor.top;
    if (mw <= 0 || mh <= 0) return false;

    const float k = (std::min)(0.34f, 720.0f / static_cast<float>(mw));
    const int cw = (std::max)(16, static_cast<int>(mw * k + 0.5f));
    const int ch = (std::max)(16, static_cast<int>(mh * k + 0.5f));

    Bitmap canvas(cw, ch, PixelFormat32bppARGB);
    if (canvas.GetLastStatus() != Ok) return false;
    {
        Graphics cg(&canvas);
        cg.Clear(Color(255, GetRValue(info.background), GetGValue(info.background),
                       GetBValue(info.background)));
        cg.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        cg.SetPixelOffsetMode(PixelOffsetModeHalf);

        IStream* stream = nullptr;
        Bitmap* img = LoadImageFromFile(info.path, &stream);
        if (!img) {
            // Format GDI+ can't decode (WebP/HEIC/...): use the JPEG copy Windows keeps.
            const std::wstring transcoded = TranscodedWallpaperPath();
            if (transcoded != info.path) img = LoadImageFromFile(transcoded, &stream);
        }
        if (img) {
            const float iw = static_cast<float>(img->GetWidth());
            const float ih = static_cast<float>(img->GetHeight());

            // Area the wallpaper is laid out in, relative to this monitor.
            float ax = 0, ay = 0, aw = static_cast<float>(mw), ah = static_cast<float>(mh);
            if (info.position == kPosSpan) {
                ax = static_cast<float>(GetSystemMetrics(SM_XVIRTUALSCREEN) - info.monitor.left);
                ay = static_cast<float>(GetSystemMetrics(SM_YVIRTUALSCREEN) - info.monitor.top);
                aw = static_cast<float>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
                ah = static_cast<float>(GetSystemMetrics(SM_CYVIRTUALSCREEN));
            }

            ImageAttributes attr;
            attr.SetWrapMode(WrapModeTileFlipXY);

            auto draw = [&](float x, float y, float w, float h) {
                RectF dst(x * k, y * k, w * k, h * k);
                cg.DrawImage(img, dst, 0, 0, iw, ih, UnitPixel, &attr);
            };

            switch (info.position) {
                case kPosStretch:
                    draw(ax, ay, aw, ah);
                    break;
                case kPosFit: {
                    const float sc = (std::min)(aw / iw, ah / ih);
                    draw(ax + (aw - iw * sc) / 2, ay + (ah - ih * sc) / 2, iw * sc, ih * sc);
                    break;
                }
                case kPosCenter:
                    draw(ax + (aw - iw) / 2, ay + (ah - ih) / 2, iw, ih);
                    break;
                case kPosTile:
                    for (float y = 0; y < mh; y += ih) {
                        for (float x = 0; x < mw; x += iw) draw(x, y, iw, ih);
                    }
                    break;
                case kPosFill:
                case kPosSpan:
                default: {
                    const float sc = (std::max)(aw / iw, ah / ih);
                    draw(ax + (aw - iw * sc) / 2, ay + (ah - ih * sc) / 2, iw * sc, ih * sc);
                    break;
                }
            }
            delete img;
        }
        if (stream) stream->Release();
    }

    BitmapData bd{};
    Rect rect(0, 0, cw, ch);
    if (canvas.LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return false;
    g_canvas.px.resize(static_cast<size_t>(cw) * ch);
    for (int y = 0; y < ch; ++y) {
        memcpy(&g_canvas.px[static_cast<size_t>(y) * cw], static_cast<BYTE*>(bd.Scan0) + y * bd.Stride,
               static_cast<size_t>(cw) * 4);
    }
    canvas.UnlockBits(&bd);

    for (auto& p : g_canvas.px) p |= 0xFF000000u;
    g_canvas.w = cw;
    g_canvas.h = ch;
    g_canvas.k = k;
    g_canvas.monitor = info.monitor;
    g_canvas.valid = true;
    return true;
}

static void CheckWallpaper(bool force) {
    const WallInfo info = QueryWallpaper(PrimaryMonitorRect());
    const std::wstring key = WallKey(info);
    if (!force && key == g_wallKey && g_canvas.valid) return;
    g_wallKey = key;
    BuildCanvas(info);
    g_glassDirty = true;
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

static Rgb WindowsAccent(bool& ok) {
    DWORD argb = 0;
    BOOL opaque = FALSE;
    ok = SUCCEEDED(DwmGetColorizationColor(&argb, &opaque));
    return {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f};
}

static void SetAccentFromBase(const Rgb& base, bool keepExact) {
    float h, s, l;
    RgbToHsl(base, h, s, l);
    if (keepExact) {
        g_palette.accent = base;
        g_palette.accent2 = HslToRgb(h + 26.0f, s, (std::max)(0.35f, l - 0.06f));
        return;
    }
    s = (std::clamp)((std::max)(s, 0.75f), 0.0f, 0.95f);
    g_palette.accent = HslToRgb(h, s, 0.64f);
    g_palette.accent2 = HslToRgb(h + 28.0f, s, 0.58f);
}

static void AnalyzeRegion(int x0, int y0, int x1, int y1) {
    x0 = (std::clamp)(x0, 0, g_canvas.w - 1);
    x1 = (std::clamp)(x1, x0 + 1, g_canvas.w);
    y0 = (std::clamp)(y0, 0, g_canvas.h - 1);
    y1 = (std::clamp)(y1, y0 + 1, g_canvas.h);

    float bins[36]{}, br[36]{}, bg[36]{}, bb[36]{};
    double lumSum = 0, rs = 0, gs = 0, bs = 0;
    int count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const uint32_t p = g_canvas.px[static_cast<size_t>(y) * g_canvas.w + x];
            const Rgb c{((p >> 16) & 0xFF) / 255.0f, ((p >> 8) & 0xFF) / 255.0f, (p & 0xFF) / 255.0f};
            const float lum = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
            lumSum += lum;
            rs += c.r;
            gs += c.g;
            bs += c.b;
            ++count;
            float h, s, l;
            RgbToHsl(c, h, s, l);
            if (s > 0.18f && l > 0.10f && l < 0.92f) {
                const float w = s * s * (1.0f - std::fabs(l - 0.5f) * 1.3f);
                if (w <= 0) continue;
                const int bin = static_cast<int>(h / 10.0f) % 36;
                bins[bin] += w;
                br[bin] += c.r * w;
                bg[bin] += c.g * w;
                bb[bin] += c.b * w;
            }
        }
    }
    if (count == 0) return;

    g_palette.avgLum = static_cast<float>(lumSum / count);
    g_palette.average = {static_cast<float>(rs / count), static_cast<float>(gs / count),
                         static_cast<float>(bs / count)};

    int best = -1;
    float bestW = 0, total = 0;
    for (int i = 0; i < 36; ++i) {
        total += bins[i];
        const float w = bins[i] + 0.5f * (bins[(i + 35) % 36] + bins[(i + 1) % 36]);
        if (w > bestW) {
            bestW = w;
            best = i;
        }
    }

    const bool colorful = best >= 0 && bins[best] > 0 && total > count * 0.015f;
    switch (g_settings.accentMode) {
        case AccentMode::Custom:
            SetAccentFromBase(FromColorRef(g_settings.customAccent), true);
            break;
        case AccentMode::Windows: {
            bool ok;
            const Rgb wa = WindowsAccent(ok);
            SetAccentFromBase(ok ? wa : Rgb{0.49f, 1.0f, 0.70f}, false);
            break;
        }
        case AccentMode::Wallpaper:
        default:
            if (colorful) {
                SetAccentFromBase({br[best] / bins[best], bg[best] / bins[best], bb[best] / bins[best]},
                                  false);
            } else {
                // Grey / black / white wallpaper: fall back to the Windows accent.
                bool ok;
                const Rgb wa = WindowsAccent(ok);
                float h, s, l;
                RgbToHsl(wa, h, s, l);
                SetAccentFromBase(ok && s > 0.15f ? wa : Rgb{0.49f, 1.0f, 0.70f}, false);
            }
            break;
    }

    // Dark glass tint harmonised with the wallpaper.
    float h, s, l;
    RgbToHsl(g_palette.average, h, s, l);
    g_palette.tint = HslToRgb(h, (std::min)(s, 0.55f) * 0.9f, 0.10f);
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

static std::unique_ptr<FontFamily> PickFamily(std::initializer_list<std::pair<const wchar_t*, int>> list,
                                              int& styleOut) {
    for (const auto& c : list) {
        auto fam = std::make_unique<FontFamily>(c.first);
        if (fam->GetLastStatus() == Ok && fam->IsAvailable() && fam->IsStyleAvailable(c.second)) {
            styleOut = c.second;
            return fam;
        }
    }
    styleOut = FontStyleBold;
    return std::make_unique<FontFamily>(L"Segoe UI");
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

static void FreeFrameBuffer() {
    g_frameBmp.reset();
    if (g_memDc) {
        if (g_oldBitmap) SelectObject(g_memDc, g_oldBitmap);
        DeleteDC(g_memDc);
        g_memDc = nullptr;
    }
    if (g_dib) {
        DeleteObject(g_dib);
        g_dib = nullptr;
    }
    g_oldBitmap = nullptr;
    g_frameBits = nullptr;
}

static void BuildLayout() {
    Layout& L = g_layout;
    L.monitor = PrimaryMonitorRect();
    const float mw = static_cast<float>(L.monitor.right - L.monitor.left);
    const float mh = static_cast<float>(L.monitor.bottom - L.monitor.top);

    // Same visual proportion on every resolution / DPI: 1.0 at 1920x1080.
    const float base = (std::min)(mh / 1080.0f, mw / 1200.0f);
    const float s = (std::clamp)(base * g_settings.scalePercent / 100.0f, 0.35f, 4.0f);
    L.s = s;

    const float margin = std::round(36.0f * s);
    const float panelW = std::round(620.0f * s);

    float y = 26.0f;
    if (g_settings.showGreeting) {
        L.greetY = y * s;
        L.greetH = 28.0f * s;
        y += 28.0f + 4.0f;
    }
    L.digitsY = y * s;
    L.digitsH = 132.0f * s;
    y += 132.0f;
    if (g_settings.showSeconds) {
        y += 16.0f;
        L.barY = y * s;
        y += 6.0f;
    }
    if (g_settings.showDate) {
        y += g_settings.showSeconds ? 16.0f : 10.0f;
        L.dateY = y * s;
        L.dateH = 28.0f * s;
        y += 28.0f;
    }
    y += 26.0f;
    const float panelH = std::round(y * s);

    L.winW = static_cast<int>(panelW + margin * 2);
    L.winH = static_cast<int>(panelH + margin * 2);
    L.panel = RectF(margin, margin, panelW, panelH);
    L.radius = 30.0f * s;

    // Shift rows into window coordinates.
    L.greetY += margin;
    L.digitsY += margin;
    L.barY += margin;
    L.dateY += margin;

    const int monW = L.monitor.right - L.monitor.left;
    const int monH = L.monitor.bottom - L.monitor.top;
    L.pos.x = L.monitor.left + (monW - L.winW) / 2;
    L.pos.y = g_settings.centerVertically
                  ? L.monitor.top + (monH - L.winH) / 2
                  : L.monitor.top + static_cast<int>(g_settings.topOffset * s - margin);

    // Digit glyphs.
    int digitStyle = FontStyleBold;
    auto digitFamily = PickFamily({{L"Segoe UI Variable Display", FontStyleBold},
                                   {L"Segoe UI", FontStyleBold},
                                   {L"Segoe UI Semibold", FontStyleRegular}},
                                  digitStyle);
    const float em = 150.0f * s;
    StringFormat typo(StringFormat::GenericTypographic());
    float maxW = 0;
    L.glyphTop = 1e9f;
    L.glyphBottom = -1e9f;
    for (int d = 0; d < 10; ++d) {
        wchar_t ch[2] = {static_cast<wchar_t>(L'0' + d), 0};
        L.digitPath[d] = std::make_unique<GraphicsPath>();
        L.digitPath[d]->AddString(ch, 1, digitFamily.get(), digitStyle, em, PointF(0, 0), &typo);
        L.digitPath[d]->GetBounds(&L.digitBounds[d]);
        maxW = (std::max)(maxW, L.digitBounds[d].Width);
        L.glyphTop = (std::min)(L.glyphTop, L.digitBounds[d].Y);
        L.glyphBottom = (std::max)(L.glyphBottom, L.digitBounds[d].GetBottom());
    }
    L.cellW = maxW + 4.0f * s;
    L.innerGap = 8.0f * s;
    L.colonGap = 54.0f * s;
    const float total = L.cellW * 4 + L.innerGap * 2 + L.colonGap;
    const float cx = L.panel.X + L.panel.Width / 2.0f;
    const float x0 = cx - total / 2.0f;
    L.cellX[0] = x0;
    L.cellX[1] = x0 + L.cellW + L.innerGap;
    L.colonX = cx;
    L.cellX[2] = x0 + L.cellW * 2 + L.innerGap + L.colonGap;
    L.cellX[3] = L.cellX[2] + L.cellW + L.innerGap;

    // Text fonts.
    L.smallFont.reset();
    L.dateFont.reset();
    L.greetFont.reset();
    L.textFamily = PickFamily({{L"Segoe UI Variable Text Semibold", FontStyleRegular},
                               {L"Segoe UI Variable Text Semib", FontStyleRegular},
                               {L"Segoe UI Semibold", FontStyleRegular},
                               {L"Segoe UI", FontStyleBold}},
                              L.textStyle);
    L.greetFont = std::make_unique<Font>(L.textFamily.get(), 19.0f * s, L.textStyle, UnitPixel);
    L.dateFont = std::make_unique<Font>(L.textFamily.get(), 19.0f * s, L.textStyle, UnitPixel);
    L.smallFont = std::make_unique<Font>(L.textFamily.get(), 15.0f * s, L.textStyle, UnitPixel);

    // Frame buffer.
    FreeFrameBuffer();
    HDC screenDc = GetDC(nullptr);
    g_memDc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = L.winW;
    bi.bmiHeader.biHeight = -L.winH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_dib = CreateDIBSection(g_memDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (g_dib && bits) {
        g_oldBitmap = SelectObject(g_memDc, g_dib);
        g_frameBits = static_cast<uint32_t*>(bits);
        g_frameBmp = std::make_unique<Bitmap>(L.winW, L.winH, L.winW * 4, PixelFormat32bppPARGB,
                                              static_cast<BYTE*>(bits));
    }

    g_layoutDirty = false;
    g_glassDirty = true;
    g_staticDirty = true;
}

// ---------------------------------------------------------------------------
// Glass (blurred wallpaper behind the panel) + palette
// ---------------------------------------------------------------------------

static void BuildGlass() {
    g_glass.reset();
    g_glassDirty = false;
    g_staticDirty = true;

    const Layout& L = g_layout;
    if (!g_canvas.valid) {
        // Keep defaults but still honour accent settings.
        g_palette = Palette{};
        if (g_settings.accentMode == AccentMode::Custom) {
            SetAccentFromBase(FromColorRef(g_settings.customAccent), true);
        } else if (g_settings.accentMode == AccentMode::Windows) {
            bool ok;
            const Rgb wa = WindowsAccent(ok);
            if (ok) SetAccentFromBase(wa, false);
        }
        return;
    }

    const float k = g_canvas.k;
    const float px = (L.pos.x - g_canvas.monitor.left + L.panel.X) * k;
    const float py = (L.pos.y - g_canvas.monitor.top + L.panel.Y) * k;
    const float pw = L.panel.Width * k;
    const float ph = L.panel.Height * k;

    // Palette from a slightly larger area so the accent represents the surroundings.
    AnalyzeRegion(static_cast<int>(px - pw * 0.25f), static_cast<int>(py - ph * 0.25f),
                  static_cast<int>(px + pw * 1.25f), static_cast<int>(py + ph * 1.25f));

    const float realRadius = (6.0f + g_settings.blurStrength * 0.55f) * L.s;
    const int r = (std::max)(1, static_cast<int>(realRadius * k / 1.7f + 0.5f));
    const int pad = r * 3;
    const int x0 = static_cast<int>(std::floor(px)) - pad;
    const int y0 = static_cast<int>(std::floor(py)) - pad;
    const int cw = static_cast<int>(std::ceil(pw)) + pad * 2 + 1;
    const int ch = static_cast<int>(std::ceil(ph)) + pad * 2 + 1;
    if (cw <= 0 || ch <= 0) return;

    std::vector<uint32_t> crop(static_cast<size_t>(cw) * ch);
    for (int y = 0; y < ch; ++y) {
        const int sy = (std::clamp)(y0 + y, 0, g_canvas.h - 1);
        for (int x = 0; x < cw; ++x) {
            const int sx = (std::clamp)(x0 + x, 0, g_canvas.w - 1);
            crop[static_cast<size_t>(y) * cw + x] = g_canvas.px[static_cast<size_t>(sy) * g_canvas.w + sx];
        }
    }
    if (g_settings.blurStrength > 0) BoxBlur(crop, cw, ch, r, 3);

    // Acrylic-like saturation boost.
    for (auto& p : crop) {
        float rr = ((p >> 16) & 0xFF) / 255.0f, gg = ((p >> 8) & 0xFF) / 255.0f, bb = (p & 0xFF) / 255.0f;
        const float lum = 0.2126f * rr + 0.7152f * gg + 0.0722f * bb;
        rr = Clamp01(lum + (rr - lum) * 1.25f);
        gg = Clamp01(lum + (gg - lum) * 1.25f);
        bb = Clamp01(lum + (bb - lum) * 1.25f);
        p = 0xFF000000u | (static_cast<uint32_t>(ToByte(rr)) << 16) |
            (static_cast<uint32_t>(ToByte(gg)) << 8) | ToByte(bb);
    }

    Bitmap small(cw, ch, PixelFormat32bppARGB);
    if (small.GetLastStatus() != Ok) return;
    BitmapData bd{};
    Rect rect(0, 0, cw, ch);
    if (small.LockBits(&rect, ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Ok) return;
    for (int y = 0; y < ch; ++y) {
        memcpy(static_cast<BYTE*>(bd.Scan0) + y * bd.Stride, &crop[static_cast<size_t>(y) * cw],
               static_cast<size_t>(cw) * 4);
    }
    small.UnlockBits(&bd);

    const int gw = static_cast<int>(std::ceil(L.panel.Width));
    const int gh = static_cast<int>(std::ceil(L.panel.Height));
    g_glass = std::make_unique<Bitmap>(gw, gh, PixelFormat32bppPARGB);
    if (g_glass->GetLastStatus() != Ok) {
        g_glass.reset();
        return;
    }
    Graphics g(g_glass.get());
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    ImageAttributes attr;
    attr.SetWrapMode(WrapModeTileFlipXY);
    const float sx = px - static_cast<float>(x0);
    const float sy = py - static_cast<float>(y0);
    g.DrawImage(&small, RectF(0, 0, static_cast<REAL>(gw), static_cast<REAL>(gh)), sx, sy, pw, ph,
                UnitPixel, &attr);
}

// ---------------------------------------------------------------------------
// Static layer (shadow, glass, tint, sheen, border)
// ---------------------------------------------------------------------------

static void BuildStatic() {
    const Layout& L = g_layout;
    const size_t n = static_cast<size_t>(L.winW) * L.winH;
    g_static.assign(n, 0);
    g_staticDirty = false;
    if (!g_settings.showPanel || n == 0) return;

    const float s = L.s;

    // Soft drop shadow.
    {
        Bitmap b(L.winW, L.winH, L.winW * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(g_static.data()));
        Graphics g(&b);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        GraphicsPath sp;
        AddRoundRect(sp, RectF(L.panel.X + 6 * s, L.panel.Y + 14 * s, L.panel.Width - 12 * s, L.panel.Height - 10 * s),
                     L.radius);
        SolidBrush br(Color(static_cast<BYTE>(110 + 50 * g_palette.avgLum), 0, 0, 0));
        g.FillPath(&br, &sp);
    }
    BoxBlur(g_static, L.winW, L.winH, (std::max)(1, static_cast<int>(10 * s)), 3);

    Bitmap b(L.winW, L.winH, L.winW * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(g_static.data()));
    Graphics g(&b);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetInterpolationMode(InterpolationModeHighQualityBilinear);

    GraphicsPath pp;
    AddRoundRect(pp, L.panel, L.radius);

    // 1. Frosted wallpaper.
    if (g_glass) {
        TextureBrush tb(g_glass.get(), WrapModeTileFlipXY);
        tb.TranslateTransform(L.panel.X, L.panel.Y);
        g.FillPath(&tb, &pp);
    } else {
        SolidBrush fallback(MakeColor(0.85f, Mix(g_palette.tint, Rgb{0.05f, 0.25f, 0.32f}, 0.5f)));
        g.FillPath(&fallback, &pp);
    }

    // 2. Tint (stronger on bright wallpapers so white text always reads well).
    float tintA = g_settings.glassTint / 100.0f;
    tintA += (std::max)(0.0f, g_palette.avgLum - 0.42f) * 0.75f;
    tintA = (std::clamp)(tintA, 0.0f, 0.92f);
    SolidBrush tint(MakeColor(tintA, g_palette.tint));
    g.FillPath(&tint, &pp);

    // 3. Accent glow rising from the bottom.
    {
        g.SetClip(&pp);
        GraphicsPath gp;
        const float gw = L.panel.Width * 0.95f;
        const float gh = L.panel.Height * 0.9f;
        gp.AddEllipse(L.panel.X + (L.panel.Width - gw) / 2, L.panel.GetBottom() - gh * 0.5f, gw, gh);
        PathGradientBrush pg(&gp);
        pg.SetCenterColor(MakeColor(0.20f, g_palette.accent));
        Color surround = MakeColor(0.0f, g_palette.accent);
        int cnt = 1;
        pg.SetSurroundColors(&surround, &cnt);
        g.FillPath(&pg, &gp);
        g.ResetClip();
    }

    // 4. Glass sheen on the upper half.
    {
        LinearGradientBrush sheen(PointF(0, L.panel.Y - 1), PointF(0, L.panel.GetBottom() + 1),
                                  Color(40, 255, 255, 255), Color(0, 255, 255, 255));
        Color cols[3] = {Color(44, 255, 255, 255), Color(10, 255, 255, 255), Color(0, 255, 255, 255)};
        REAL pos[3] = {0.0f, 0.45f, 1.0f};
        sheen.SetInterpolationColors(cols, pos, 3);
        g.FillPath(&sheen, &pp);
    }

    // 5. Edge light: bright top edge fading to a faint bottom edge.
    {
        const float inset = 0.6f * s;
        GraphicsPath bp;
        AddRoundRect(bp, RectF(L.panel.X + inset, L.panel.Y + inset, L.panel.Width - inset * 2, L.panel.Height - inset * 2),
                     L.radius - inset);
        LinearGradientBrush eb(PointF(0, L.panel.Y - 1), PointF(0, L.panel.GetBottom() + 1),
                               Color(110, 255, 255, 255), Color(22, 255, 255, 255));
        Pen pen(&eb, (std::max)(1.0f, 1.2f * s));
        g.DrawPath(&pen, &bp);
    }
}

// ---------------------------------------------------------------------------
// Frame drawing
// ---------------------------------------------------------------------------

static float ShadowK() {
    float k = 0.8f + g_palette.avgLum * 1.0f;
    if (!g_settings.showPanel) k *= 1.7f;
    return k;
}

static void DrawGlyph(Graphics& g, int digit, float cellX, float alpha, float dy) {
    const Layout& L = g_layout;
    if (digit < 0 || digit > 9 || alpha <= 0.003f) return;
    const RectF& b = L.digitBounds[digit];
    const float glyphH = L.glyphBottom - L.glyphTop;
    const float tx = cellX + (L.cellW - b.Width) / 2.0f - b.X;
    const float ty = L.digitsY + (L.digitsH - glyphH) / 2.0f - L.glyphTop + dy;
    const float s = L.s;
    const float sk = ShadowK();

    const float offs[3] = {2.0f, 4.5f, 8.0f};
    const float alph[3] = {0.20f, 0.12f, 0.06f};
    for (int i = 0; i < 3; ++i) {
        g.TranslateTransform(tx, ty + offs[i] * s);
        SolidBrush sb(Color(ToByte(alph[i] * sk * alpha), 0, 10, 16));
        g.FillPath(&sb, L.digitPath[digit].get());
        g.ResetTransform();
    }

    g.TranslateTransform(tx, ty);
    const Rgb top{1.0f, 1.0f, 1.0f};
    const Rgb bottom = Mix(top, g_palette.accent, 0.22f);
    LinearGradientBrush lb(PointF(0, L.glyphTop - 1), PointF(0, L.glyphBottom + 1), MakeColor(alpha, top),
                           MakeColor(alpha, bottom));
    g.FillPath(&lb, L.digitPath[digit].get());
    g.ResetTransform();
}

static void UpdateDigits(const int digits[4], ULONGLONG now) {
    int order = 0;
    for (int i = 3; i >= 0; --i) {
        DigitAnim& a = g_digitAnim[i];
        if (a.cur == digits[i]) continue;
        if (a.cur < 0 || !g_settings.animateDigits) {
            a.prev = -1;
        } else {
            a.prev = a.cur;
            a.start = now + static_cast<ULONGLONG>(order) * 80;
            ++order;
        }
        a.cur = digits[i];
    }
}

static void DrawDigits(Graphics& g, ULONGLONG now) {
    const Layout& L = g_layout;
    const float glyphH = L.glyphBottom - L.glyphTop;
    const float glyphY = L.digitsY + (L.digitsH - glyphH) / 2.0f;
    const float roll = glyphH * 0.85f;
    for (int i = 0; i < 4; ++i) {
        DigitAnim& a = g_digitAnim[i];
        const float cx = L.cellX[i];
        // Odometer window: digits roll in/out of the glyph band only.
        g.SetClip(RectF(cx - 12 * L.s, glyphY - 3 * L.s, L.cellW + 24 * L.s, glyphH + 14 * L.s));
        if (a.prev >= 0) {
            const float t = now < a.start ? 0.0f : static_cast<float>(now - a.start) / 520.0f;
            if (t >= 1.0f) {
                a.prev = -1;
                DrawGlyph(g, a.cur, cx, 1.0f, 0);
            } else {
                const float e = EaseOutCubic(t);
                DrawGlyph(g, a.prev, cx, 1.0f - e, -e * roll);
                DrawGlyph(g, a.cur, cx, e, (1.0f - e) * roll);
            }
        } else {
            DrawGlyph(g, a.cur, cx, 1.0f, 0);
        }
        g.ResetClip();
    }
}

static void FillGlow(Graphics& g, float cx, float cy, float radius, const Rgb& c, float alpha) {
    GraphicsPath p;
    p.AddEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
    PathGradientBrush pg(&p);
    pg.SetCenterColor(MakeColor(alpha, c));
    Color surround = MakeColor(0.0f, c);
    int n = 1;
    pg.SetSurroundColors(&surround, &n);
    g.FillPath(&pg, &p);
}

static void DrawColon(Graphics& g, ULONGLONG now) {
    const Layout& L = g_layout;
    const float s = L.s;
    const float glyphH = L.glyphBottom - L.glyphTop;
    const float mid = L.digitsY + L.digitsH / 2.0f;
    const float breath = 0.5f + 0.5f * std::sin(static_cast<float>(now % 2000) / 2000.0f * 2 * kPi);
    const float a = 0.55f + 0.45f * breath;
    const float r = 6.0f * s;
    for (float cy : {mid - glyphH * 0.19f, mid + glyphH * 0.19f}) {
        FillGlow(g, L.colonX, cy, r * 3.6f, g_palette.accent, 0.42f * a);
        SolidBrush dot(MakeColor(a, g_palette.accent));
        g.FillEllipse(&dot, L.colonX - r, cy - r, r * 2, r * 2);
    }
}

static void DrawShadowedText(Graphics& g, const std::wstring& text, const Font& font, const PointF& at,
                             const StringFormat& fmt, const Color& color) {
    const float s = g_layout.s;
    SolidBrush sh(Color(ToByte(0.30f * ShadowK()), 0, 10, 16));
    g.DrawString(text.c_str(), -1, &font, PointF(at.X, at.Y + 1.5f * s), &fmt, &sh);
    SolidBrush br(color);
    g.DrawString(text.c_str(), -1, &font, at, &fmt, &br);
}

static void DrawSunMoon(Graphics& g, int period, float cx, float cy, float size) {
    const float s = g_layout.s;
    if (period == 3) {
        // Crescent moon.
        GraphicsPath outer, inner;
        outer.AddEllipse(cx - size, cy - size, size * 2, size * 2);
        inner.AddEllipse(cx - size * 0.35f, cy - size * 1.25f, size * 2, size * 2);
        Region rg(&outer);
        rg.Exclude(&inner);
        FillGlow(g, cx, cy, size * 2.4f, Rgb{0.80f, 0.86f, 1.0f}, 0.22f);
        SolidBrush br(Color(240, 232, 240, 255));
        g.FillRegion(&br, &rg);
        return;
    }
    const Rgb c = period == 0 ? Rgb{1.0f, 0.86f, 0.45f} : (period == 1 ? Rgb{1.0f, 0.82f, 0.30f} : Rgb{1.0f, 0.62f, 0.35f});
    FillGlow(g, cx, cy, size * 2.6f, c, 0.30f);
    SolidBrush core(MakeColor(1.0f, c));
    const float cr = size * 0.55f;
    g.FillEllipse(&core, cx - cr, cy - cr, cr * 2, cr * 2);
    Pen ray(MakeColor(0.95f, c), (std::max)(1.0f, 1.8f * s));
    ray.SetStartCap(LineCapRound);
    ray.SetEndCap(LineCapRound);
    for (int i = 0; i < 8; ++i) {
        const float ang = i * kPi / 4.0f;
        const float r0 = size * 0.85f, r1 = size * 1.2f;
        g.DrawLine(&ray, cx + std::cos(ang) * r0, cy + std::sin(ang) * r0, cx + std::cos(ang) * r1,
                   cy + std::sin(ang) * r1);
    }
}

static void DrawGreeting(Graphics& g, const SYSTEMTIME& st) {
    const Layout& L = g_layout;
    const float s = L.s;
    const int period = DayPeriod(st.wHour);
    const std::wstring text = GreetingText(period);

    StringFormat fmt(StringFormat::GenericTypographic());
    RectF tb;
    g.MeasureString(text.c_str(), -1, L.greetFont.get(), PointF(0, 0), &fmt, &tb);
    const float icon = 9.0f * s;
    const float gap = 12.0f * s;
    const float total = icon * 2 + gap + tb.Width;
    const float cx = L.panel.X + L.panel.Width / 2.0f;
    const float x = cx - total / 2.0f;
    const float cy = L.greetY + L.greetH / 2.0f;

    DrawSunMoon(g, period, x + icon, cy, icon);
    DrawShadowedText(g, text, *L.greetFont, PointF(x + icon * 2 + gap, cy - tb.Height / 2.0f), fmt,
                     Color(220, 240, 250, 255));
}

static void DrawSecondsBar(Graphics& g, const SYSTEMTIME& st) {
    const Layout& L = g_layout;
    const float s = L.s;
    const float trackW = 360.0f * s;
    const float trackH = 4.0f * s;
    const float trackX = L.panel.X + (L.panel.Width - trackW) / 2.0f;
    const float trackY = L.barY;

    GraphicsPath track;
    AddCapsule(track, trackX, trackY, trackW, trackH);
    SolidBrush trackBrush(Color(46, 255, 255, 255));
    g.FillPath(&trackBrush, &track);

    const float progress = (st.wSecond + st.wMilliseconds / 1000.0f) / 60.0f;
    const float fillW = trackW * progress;
    if (fillW >= trackH) {
        GraphicsPath fill;
        AddCapsule(fill, trackX, trackY, fillW, trackH);
        LinearGradientBrush fb(PointF(trackX - 1, trackY), PointF(trackX + trackW + 1, trackY),
                               MakeColor(1.0f, g_palette.accent), MakeColor(1.0f, g_palette.accent2));
        g.FillPath(&fb, &fill);
    }

    const float hx = trackX + (std::max)(fillW, trackH / 2.0f);
    const float hy = trackY + trackH / 2.0f;
    const float hr = 5.0f * s;
    FillGlow(g, hx, hy, hr * 3.2f, g_palette.accent, 0.45f);
    SolidBrush head(MakeColor(1.0f, Mix(g_palette.accent, Rgb{1, 1, 1}, 0.45f)));
    g.FillEllipse(&head, hx - hr, hy - hr, hr * 2, hr * 2);
}

static void DrawDateRow(Graphics& g, const SYSTEMTIME& st) {
    const Layout& L = g_layout;
    const float s = L.s;
    const std::wstring date = FormatDate(st);
    StringFormat fmt(StringFormat::GenericTypographic());
    RectF db;
    g.MeasureString(date.c_str(), -1, L.dateFont.get(), PointF(0, 0), &fmt, &db);

    wchar_t pct[16]{};
    const float dayProgress = (st.wHour * 3600.0f + st.wMinute * 60.0f + st.wSecond) / 86400.0f;
    swprintf_s(pct, L"%d%%", static_cast<int>(dayProgress * 100.0f));
    RectF pb;
    const bool ring = g_settings.showDayProgress;
    const float ringD = 17.0f * s;
    const float sep = 18.0f * s;
    float total = db.Width;
    if (ring) {
        g.MeasureString(pct, -1, L.smallFont.get(), PointF(0, 0), &fmt, &pb);
        total += sep + ringD + 7.0f * s + pb.Width;
    }

    const float cx = L.panel.X + L.panel.Width / 2.0f;
    float x = cx - total / 2.0f;
    const float cy = L.dateY + L.dateH / 2.0f;
    DrawShadowedText(g, date, *L.dateFont, PointF(x, cy - db.Height / 2.0f), fmt, Color(230, 244, 252, 255));
    x += db.Width;

    if (!ring) return;
    x += sep;
    const float th = (std::max)(1.5f, 2.6f * s);
    RectF rr(x, cy - ringD / 2.0f, ringD, ringD);
    Pen trackPen(Color(60, 255, 255, 255), th);
    g.DrawEllipse(&trackPen, rr);
    LinearGradientBrush ab(PointF(rr.X, rr.Y), PointF(rr.GetRight(), rr.GetBottom()),
                           MakeColor(1.0f, g_palette.accent), MakeColor(1.0f, g_palette.accent2));
    Pen arc(&ab, th);
    arc.SetStartCap(LineCapRound);
    arc.SetEndCap(LineCapRound);
    const float sweep = (std::max)(2.0f, 360.0f * dayProgress);
    g.DrawArc(&arc, rr, -90.0f, sweep);
    x += ringD + 7.0f * s;
    DrawShadowedText(g, pct, *L.smallFont, PointF(x, cy - pb.Height / 2.0f), fmt,
                     MakeColor(0.95f, Mix(g_palette.accent, Rgb{1, 1, 1}, 0.35f)));
}

static void RenderFrame(ULONGLONG now) {
    const Layout& L = g_layout;
    if (!g_frameBmp || !g_frameBits || g_static.size() != static_cast<size_t>(L.winW) * L.winH) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);

    memcpy(g_frameBits, g_static.data(), g_static.size() * 4);

    {
        Graphics g(g_frameBmp.get());
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetCompositingQuality(CompositingQualityHighQuality);

        int hour = st.wHour;
        if (!g_settings.use24Hour) {
            hour %= 12;
            if (hour == 0) hour = 12;
        }
        const int digits[4] = {hour / 10, hour % 10, st.wMinute / 10, st.wMinute % 10};
        UpdateDigits(digits, now);
        DrawDigits(g, now);
        DrawColon(g, now);

        if (!g_settings.use24Hour) {
            StringFormat fmt(StringFormat::GenericTypographic());
            const std::wstring mer = st.wHour >= 12 ? L"PM" : L"AM";
            const float x = L.cellX[3] + L.cellW + 10.0f * L.s;
            const float y = L.digitsY + L.digitsH * 0.62f;
            DrawShadowedText(g, mer, *L.greetFont, PointF(x, y), fmt,
                             MakeColor(1.0f, Mix(g_palette.accent, Rgb{1, 1, 1}, 0.2f)));
        }

        if (g_settings.showGreeting) DrawGreeting(g, st);
        if (g_settings.showSeconds) DrawSecondsBar(g, st);
        if (g_settings.showDate) DrawDateRow(g, st);
    }

    const float fade = EaseOutCubic(static_cast<float>(now - g_fadeStart) / 700.0f);
    POINT dst = L.pos;
    SIZE size{L.winW, L.winH};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, ToByte(fade), AC_SRC_ALPHA};
    HDC screenDc = GetDC(nullptr);
    UpdateLayeredWindow(g_clockWindow, screenDc, &dst, &size, g_memDc, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screenDc);
}

static bool IsFullscreenAppRunning() {
    QUERY_USER_NOTIFICATION_STATE state;
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE;
}

static void Tick() {
    if (!g_clockWindow) return;
    const ULONGLONG now = GetTickCount64();

    if (now - g_lastFsCheck >= 1000) {
        g_lastFsCheck = now;
        g_paused = IsFullscreenAppRunning();
    }
    if (g_paused) return;

    if (g_canvasDirty || g_forceWallCheck || now - g_lastWallCheck >= 5000) {
        g_lastWallCheck = now;
        CheckWallpaper(g_canvasDirty || g_forceWallCheck);
        g_canvasDirty = false;
        g_forceWallCheck = false;
    }
    if (g_layoutDirty) BuildLayout();
    if (g_glassDirty) BuildGlass();
    if (g_staticDirty) BuildStatic();
    RenderFrame(now);
}

// ---------------------------------------------------------------------------
// Window / thread
// ---------------------------------------------------------------------------

static LRESULT CALLBACK ClockWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            if (wParam == kReloadTimerId) {
                KillTimer(hwnd, kReloadTimerId);
                g_forceWallCheck = true;
            }
            Tick();
            return 0;

        case WM_APP_REFRESH:
            LoadSettings();
            g_userName = DetectUserName();
            for (auto& a : g_digitAnim) a = DigitAnim{};
            g_layoutDirty = true;
            g_forceWallCheck = true;
            Tick();
            return 0;

        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETDESKWALLPAPER) {
                // Give Windows a moment to finish writing the new wallpaper.
                SetTimer(hwnd, kReloadTimerId, 600, nullptr);
            } else {
                g_glassDirty = true;  // accent colour / theme may have changed
            }
            return 0;

        case WM_DWMCOLORIZATIONCOLORCHANGED:
            g_glassDirty = true;
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            g_layoutDirty = true;
            g_canvasDirty = true;
            return 0;

        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            KillTimer(hwnd, kReloadTimerId);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ReleaseGraphicsResources() {
    FreeFrameBuffer();
    g_glass.reset();
    for (auto& p : g_layout.digitPath) p.reset();
    g_layout.greetFont.reset();
    g_layout.dateFont.reset();
    g_layout.smallFont.reset();
    g_layout.textFamily.reset();
    g_static.clear();
    g_canvas = WallCanvas{};
}

static DWORD WINAPI ClockThreadProc(void*) {
    g_threadId = GetCurrentThreadId();

    // Work in physical pixels on every monitor / scale.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    GdiplusStartupInput startupInput;
    if (GdiplusStartup(&g_gdiplusToken, &startupInput, nullptr) != Ok) {
        Wh_Log(L"Desktop Glass Clock: GDI+ startup failed");
        if (SUCCEEDED(coHr)) CoUninitialize();
        return 0;
    }

    LoadSettings();
    g_userName = DetectUserName();

    const wchar_t kClassName[] = L"WindhawkDesktopGlassClock";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = g_hInstance;
    wc.lpfnWndProc = ClockWndProc;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_clockWindow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                    kClassName, L"", WS_POPUP | WS_VISIBLE, 0, 0, 1, 1, nullptr, nullptr,
                                    g_hInstance, nullptr);
    if (!g_clockWindow) {
        Wh_Log(L"Desktop Glass Clock: CreateWindowEx failed: %u", GetLastError());
        UnregisterClassW(kClassName, g_hInstance);
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
        if (SUCCEEDED(coHr)) CoUninitialize();
        return 0;
    }

    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        SetWindowPos(g_clockWindow, progman, 0, 0, 1, 1,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    ShowWindow(g_clockWindow, SW_SHOWNOACTIVATE);

    g_fadeStart = GetTickCount64();
    Tick();
    SetTimer(g_clockWindow, kTimerId, 33, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_clockWindow = nullptr;
    UnregisterClassW(kClassName, g_hInstance);

    ReleaseGraphicsResources();
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
    if (SUCCEEDED(coHr)) CoUninitialize();
    return 0;
}

BOOL Wh_ModInit() {
    g_hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    g_thread = CreateThread(nullptr, 0, ClockThreadProc, nullptr, 0, nullptr);
    if (!g_thread) {
        Wh_Log(L"Desktop Glass Clock: failed to create UI thread");
        return FALSE;
    }
    Wh_Log(L"Desktop Glass Clock initialized");
    return TRUE;
}

void Wh_ModSettingsChanged() {
    if (g_clockWindow) PostMessageW(g_clockWindow, WM_APP_REFRESH, 0, 0);
}

void Wh_ModUninit() {
    if (g_clockWindow) {
        PostMessageW(g_clockWindow, WM_CLOSE, 0, 0);
    } else if (g_threadId) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    Wh_Log(L"Desktop Glass Clock uninitialized");
}
