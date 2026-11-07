// ==WindhawkMod==
// @id              desktop-flip-clock
// @name            Desktop Glass Clock
// @description     Frosted-glass desktop clock that adapts to any wallpaper, any resolution and any display scale.
// @version         1.1.0
// @author          Andino Ferdiansah 
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
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Gdiplus;

extern "C" IMAGE_DOS_HEADER __ImageBase;

// Globals

static HINSTANCE g_hInstance = nullptr;
static HWND g_clockWindow = nullptr;
static ULONG_PTR g_gdiplusToken = 0;
static HANDLE g_thread = nullptr;
static DWORD g_threadId = 0;

static constexpr UINT WM_APP_REFRESH = WM_APP + 42;
static constexpr UINT_PTR kFrameTimerId = 1;
static constexpr UINT_PTR kWallpaperReloadTimerId = 2;
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
struct WallpaperCanvas {
    bool valid = false;
    std::vector<uint32_t> pixels;  // ARGB, not premultiplied, opaque
    int width = 0, height = 0;
    float scale = 1.0f;  // canvas px per screen px
    RECT monitor{};
};

// Colors derived from the wallpaper area behind the clock.
struct Palette {
    Rgb accent{0.49f, 1.0f, 0.70f};
    Rgb accentEnd{0.24f, 1.0f, 0.77f};  // gradient end, hue-shifted from accent
    Rgb tint{0.02f, 0.18f, 0.22f};
    Rgb averageColor{0.1f, 0.3f, 0.3f};
    float averageLuminance = 0.3f;
};

struct Layout {
    float scale = 1.0f;
    int winW = 0, winH = 0;
    POINT pos{};
    RECT monitor{};
    RectF panel;
    float radius = 30.0f;

    float greetingY = 0, greetingH = 0;
    float digitsY = 0, digitsH = 0;
    float barY = 0;
    float dateY = 0, dateH = 0;

    // Digit glyph cache.
    std::unique_ptr<GraphicsPath> digitPath[10];
    RectF digitBounds[10];
    float glyphTop = 0, glyphBottom = 0;  // union of digit bounds (path coords)
    float digitCellW = 0, digitGap = 0, colonGap = 0;
    float digitCellX[4]{};
    float colonX = 0;

    std::unique_ptr<FontFamily> labelFamily;
    int labelStyle = FontStyleRegular;
    std::unique_ptr<Font> labelFont;    // greeting, date, AM/PM
    std::unique_ptr<Font> percentFont;  // day-progress percentage
};

static Layout g_layout;
static WallpaperCanvas g_wallpaper;
static Palette g_palette;
static std::wstring g_wallpaperKey;

static std::vector<uint32_t> g_staticLayer;     // premultiplied ARGB, winW*winH
static std::unique_ptr<Bitmap> g_glassBitmap;  // blurred wallpaper sized to panel

// Static layer plus digits, AM/PM, greeting and date. Redrawn only when the second changes or a
// digit rolls; the colon and seconds bar go on top every frame and never overlap these rows.
static std::vector<uint32_t> g_contentLayer;
static SYSTEMTIME g_contentTime{};  // second the content layer shows, milliseconds zeroed
static bool g_contentDirty = true;

// Frame buffer.
static HDC g_frameDc = nullptr;
static HBITMAP g_frameDib = nullptr;
static HGDIOBJ g_frameDcOldBitmap = nullptr;
static uint32_t* g_frameBits = nullptr;
static std::unique_ptr<Bitmap> g_frameBitmap;

static bool g_layoutDirty = true;
static bool g_canvasDirty = true;
static bool g_glassDirty = true;
static bool g_staticDirty = true;
static bool g_forceWallpaperCheck = true;
static bool g_paused = false;

static ULONGLONG g_lastWallpaperCheck = 0;
static ULONGLONG g_lastFullscreenCheck = 0;
static ULONGLONG g_fadeInStart = 0;

struct DigitRoll {
    int current = -1;
    int previous = -1;  // digit rolling out, -1 when idle
    ULONGLONG startTick = 0;
};
static DigitRoll g_digitRolls[4];

// Small helpers

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
    const float maxC = (std::max)({c.r, c.g, c.b});
    const float minC = (std::min)({c.r, c.g, c.b});
    l = (maxC + minC) * 0.5f;
    if (maxC - minC < 1e-5f) {
        h = 0;
        s = 0;
        return;
    }
    const float delta = maxC - minC;
    s = l > 0.5f ? delta / (2.0f - maxC - minC) : delta / (maxC + minC);
    if (maxC == c.r) {
        h = (c.g - c.b) / delta + (c.g < c.b ? 6.0f : 0.0f);
    } else if (maxC == c.g) {
        h = (c.b - c.r) / delta + 2.0f;
    } else {
        h = (c.r - c.g) / delta + 4.0f;
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
    const float hue01 = h / 360.0f;
    return {HueToRgb(p, q, hue01 + 1.0f / 3), HueToRgb(p, q, hue01), HueToRgb(p, q, hue01 - 1.0f / 3)};
}

static Rgb FromColorRef(COLORREF c) {
    return {GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f};
}

static Rgb UnpackRgb(uint32_t argb) {
    return {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f};
}

// 0x00RRGGBB.
static DWORD PackRgb(const Rgb& c) {
    return (static_cast<DWORD>(ToByte(c.r)) << 16) | (static_cast<DWORD>(ToByte(c.g)) << 8) |
           static_cast<DWORD>(ToByte(c.b));
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
    const int window = 2 * r + 1;
    for (int line = 0; line < lines; ++line) {
        const uint32_t* srcLine = horizontal ? src + static_cast<size_t>(line) * w : src + line;
        uint32_t* dstLine = horizontal ? dst + static_cast<size_t>(line) * w : dst + line;
        auto at = [&](int i) -> uint32_t {
            i = i < 0 ? 0 : (i >= len ? len - 1 : i);
            return srcLine[static_cast<size_t>(i) * step];
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
            dstLine[static_cast<size_t>(i) * step] =
                static_cast<uint32_t>(sum0 / window) | (static_cast<uint32_t>(sum1 / window) << 8) |
                (static_cast<uint32_t>(sum2 / window) << 16) | (static_cast<uint32_t>(sum3 / window) << 24);
            const uint32_t entering = at(i + r + 1);
            const uint32_t leaving = at(i - r);
            sum0 += static_cast<int>(entering & 0xFF) - static_cast<int>(leaving & 0xFF);
            sum1 += static_cast<int>((entering >> 8) & 0xFF) - static_cast<int>((leaving >> 8) & 0xFF);
            sum2 += static_cast<int>((entering >> 16) & 0xFF) - static_cast<int>((leaving >> 16) & 0xFF);
            sum3 += static_cast<int>((entering >> 24) & 0xFF) - static_cast<int>((leaving >> 24) & 0xFF);
        }
    }
}

static void BoxBlur(std::vector<uint32_t>& buf, int w, int h, int r, int passes) {
    if (r <= 0 || w <= 0 || h <= 0) return;
    std::vector<uint32_t> tmp(buf.size());
    for (int pass = 0; pass < passes; ++pass) {
        BoxBlurPass(buf.data(), tmp.data(), w, h, r, true);
        BoxBlurPass(tmp.data(), buf.data(), w, h, r, false);
    }
}

// Settings and user name

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
    wchar_t displayName[256]{};
    ULONG displayNameLen = ARRAYSIZE(displayName);
    if (GetUserNameExW(NameDisplay, displayName, &displayNameLen) && displayName[0]) {
        return FirstWord(displayName);
    }

    wchar_t user[257]{};
    DWORD userLen = ARRAYSIZE(user);
    if (!GetUserNameW(user, &userLen)) return L"";

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

// Monitor

static RECT PrimaryMonitorRect() {
    RECT r{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) r = mi.rcMonitor;
    return r;
}

// Wallpaper discovery and canvas

enum WallPos { kPosCenter = 0, kPosTile = 1, kPosStretch = 2, kPosFit = 3, kPosFill = 4, kPosSpan = 5 };

struct WallpaperInfo {
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

static WallpaperInfo QueryWallpaper(const RECT& monitor) {
    WallpaperInfo info;
    info.monitor = monitor;
    info.background = GetSysColor(COLOR_DESKTOP);

    bool gotPos = false;
    IDesktopWallpaper* wallpaperApi = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidDesktopWallpaper, nullptr, CLSCTX_ALL, kIidDesktopWallpaper,
                                   reinterpret_cast<void**>(&wallpaperApi))) &&
        wallpaperApi) {
        UINT count = 0;
        LPWSTR monitorId = nullptr;
        if (SUCCEEDED(wallpaperApi->GetMonitorDevicePathCount(&count))) {
            for (UINT i = 0; i < count; ++i) {
                LPWSTR id = nullptr;
                if (FAILED(wallpaperApi->GetMonitorDevicePathAt(i, &id)) || !id) continue;
                RECT rect{};
                if (SUCCEEDED(wallpaperApi->GetMonitorRECT(id, &rect)) && EqualRect(&rect, &monitor)) {
                    monitorId = id;
                    break;
                }
                CoTaskMemFree(id);
            }
        }
        LPWSTR path = nullptr;
        if (SUCCEEDED(wallpaperApi->GetWallpaper(monitorId, &path)) && path) {
            info.path = path;
            CoTaskMemFree(path);
        }
        if (monitorId) CoTaskMemFree(monitorId);

        DESKTOP_WALLPAPER_POSITION pos;
        if (SUCCEEDED(wallpaperApi->GetPosition(&pos))) {
            info.position = static_cast<int>(pos);
            gotPos = true;
        }
        COLORREF background;
        if (SUCCEEDED(wallpaperApi->GetBackgroundColor(&background))) info.background = background;
        wallpaperApi->Release();
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
        const int styleValue = _wtoi(style);
        if (styleValue == 10) info.position = kPosFill;
        else if (styleValue == 6) info.position = kPosFit;
        else if (styleValue == 2) info.position = kPosStretch;
        else if (styleValue == 22) info.position = kPosSpan;
        else info.position = _wtoi(tile) ? kPosTile : kPosCenter;
    }

    // Slideshow / Spotlight / unsupported formats: Windows keeps a JPEG copy here.
    if (!FileExists(info.path)) info.path = TranscodedWallpaperPath();
    return info;
}

// Changes whenever the wallpaper file, its layout or the monitor changes.
static std::wstring WallpaperKey(const WallpaperInfo& info) {
    ULONGLONG writeTime = 0, fileSize = 0;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!info.path.empty() && GetFileAttributesExW(info.path.c_str(), GetFileExInfoStandard, &attributes)) {
        writeTime = (static_cast<ULONGLONG>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
                    attributes.ftLastWriteTime.dwLowDateTime;
        fileSize = (static_cast<ULONGLONG>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    }
    wchar_t suffix[128];
    swprintf_s(suffix, L"|%d|%06X|%llu|%llu|%ld,%ld,%ld,%ld", info.position,
               static_cast<unsigned>(info.background), writeTime, fileSize, info.monitor.left,
               info.monitor.top, info.monitor.right, info.monitor.bottom);
    return info.path + suffix;
}

static Bitmap* LoadImageFromFile(const std::wstring& path, IStream** streamOut) {
    *streamOut = nullptr;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    std::vector<BYTE> data;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 512ll * 1024 * 1024) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) ||
            read != data.size()) {
            data.clear();
        }
    }
    CloseHandle(file);
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

static bool BuildCanvas(const WallpaperInfo& info) {
    g_wallpaper.valid = false;
    const int monitorW = info.monitor.right - info.monitor.left;
    const int monitorH = info.monitor.bottom - info.monitor.top;
    if (monitorW <= 0 || monitorH <= 0) return false;

    const float canvasScale = (std::min)(0.34f, 720.0f / static_cast<float>(monitorW));
    const int canvasW = (std::max)(16, static_cast<int>(monitorW * canvasScale + 0.5f));
    const int canvasH = (std::max)(16, static_cast<int>(monitorH * canvasScale + 0.5f));

    g_wallpaper.pixels.resize(static_cast<size_t>(canvasW) * canvasH);
    Bitmap canvas(canvasW, canvasH, canvasW * 4, PixelFormat32bppARGB,
                  reinterpret_cast<BYTE*>(g_wallpaper.pixels.data()));
    if (canvas.GetLastStatus() != Ok) return false;
    {
        Graphics canvasGraphics(&canvas);
        canvasGraphics.Clear(Color(255, GetRValue(info.background), GetGValue(info.background),
                       GetBValue(info.background)));
        canvasGraphics.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        canvasGraphics.SetPixelOffsetMode(PixelOffsetModeHalf);

        IStream* stream = nullptr;
        Bitmap* image = LoadImageFromFile(info.path, &stream);
        if (!image) {
            // Format GDI+ can't decode (WebP/HEIC/...): use the JPEG copy Windows keeps.
            const std::wstring transcoded = TranscodedWallpaperPath();
            if (transcoded != info.path) image = LoadImageFromFile(transcoded, &stream);
        }
        if (image) {
            const float imageW = static_cast<float>(image->GetWidth());
            const float imageH = static_cast<float>(image->GetHeight());

            // Area the wallpaper is laid out in, relative to this monitor.
            float areaX = 0, areaY = 0, areaW = static_cast<float>(monitorW), areaH = static_cast<float>(monitorH);
            if (info.position == kPosSpan) {
                areaX = static_cast<float>(GetSystemMetrics(SM_XVIRTUALSCREEN) - info.monitor.left);
                areaY = static_cast<float>(GetSystemMetrics(SM_YVIRTUALSCREEN) - info.monitor.top);
                areaW = static_cast<float>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
                areaH = static_cast<float>(GetSystemMetrics(SM_CYVIRTUALSCREEN));
            }

            ImageAttributes attr;
            attr.SetWrapMode(WrapModeTileFlipXY);

            auto draw = [&](float x, float y, float w, float h) {
                RectF dst(x * canvasScale, y * canvasScale, w * canvasScale, h * canvasScale);
                canvasGraphics.DrawImage(image, dst, 0, 0, imageW, imageH, UnitPixel, &attr);
            };

            switch (info.position) {
                case kPosStretch:
                    draw(areaX, areaY, areaW, areaH);
                    break;
                case kPosFit: {
                    const float fit = (std::min)(areaW / imageW, areaH / imageH);
                    draw(areaX + (areaW - imageW * fit) / 2, areaY + (areaH - imageH * fit) / 2, imageW * fit,
                         imageH * fit);
                    break;
                }
                case kPosCenter:
                    draw(areaX + (areaW - imageW) / 2, areaY + (areaH - imageH) / 2, imageW, imageH);
                    break;
                case kPosTile:
                    for (float y = 0; y < monitorH; y += imageH) {
                        for (float x = 0; x < monitorW; x += imageW) draw(x, y, imageW, imageH);
                    }
                    break;
                case kPosFill:
                case kPosSpan:
                default: {
                    const float fill = (std::max)(areaW / imageW, areaH / imageH);
                    draw(areaX + (areaW - imageW * fill) / 2, areaY + (areaH - imageH * fill) / 2, imageW * fill,
                         imageH * fill);
                    break;
                }
            }
            delete image;
        }
        if (stream) stream->Release();
    }

    for (auto& p : g_wallpaper.pixels) p |= 0xFF000000u;
    g_wallpaper.width = canvasW;
    g_wallpaper.height = canvasH;
    g_wallpaper.scale = canvasScale;
    g_wallpaper.monitor = info.monitor;
    g_wallpaper.valid = true;
    return true;
}

static void CheckWallpaper(bool force) {
    const WallpaperInfo info = QueryWallpaper(PrimaryMonitorRect());
    const std::wstring key = WallpaperKey(info);
    if (!force && key == g_wallpaperKey && g_wallpaper.valid) return;
    g_wallpaperKey = key;
    BuildCanvas(info);
    g_glassDirty = true;
}

// Palette

static Rgb WindowsAccent(bool& ok) {
    DWORD argb = 0;
    BOOL opaque = FALSE;
    ok = SUCCEEDED(DwmGetColorizationColor(&argb, &opaque));
    return UnpackRgb(argb);
}

static void SetAccentFromBase(const Rgb& base, bool keepExact) {
    float h, s, l;
    RgbToHsl(base, h, s, l);
    if (keepExact) {
        g_palette.accent = base;
        g_palette.accentEnd = HslToRgb(h + 26.0f, s, (std::max)(0.35f, l - 0.06f));
        return;
    }
    s = (std::clamp)((std::max)(s, 0.75f), 0.0f, 0.95f);
    g_palette.accent = HslToRgb(h, s, 0.64f);
    g_palette.accentEnd = HslToRgb(h + 28.0f, s, 0.58f);
}

// Picks the accent (dominant saturated hue, weighted toward mid lightness) and the glass tint.
static void AnalyzeWallpaperRegion(int x0, int y0, int x1, int y1) {
    x0 = (std::clamp)(x0, 0, g_wallpaper.width - 1);
    x1 = (std::clamp)(x1, x0 + 1, g_wallpaper.width);
    y0 = (std::clamp)(y0, 0, g_wallpaper.height - 1);
    y1 = (std::clamp)(y1, y0 + 1, g_wallpaper.height);

    // 36 hue bins of 10 degrees: total weight and weighted color sums.
    float bins[36]{}, binR[36]{}, binG[36]{}, binB[36]{};
    double lumSum = 0, sumR = 0, sumG = 0, sumB = 0;
    int count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb c = UnpackRgb(g_wallpaper.pixels[static_cast<size_t>(y) * g_wallpaper.width + x]);
            const float lum = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
            lumSum += lum;
            sumR += c.r;
            sumG += c.g;
            sumB += c.b;
            ++count;
            float h, s, l;
            RgbToHsl(c, h, s, l);
            if (s > 0.18f && l > 0.10f && l < 0.92f) {
                const float weight = s * s * (1.0f - std::fabs(l - 0.5f) * 1.3f);
                if (weight <= 0) continue;
                const int bin = static_cast<int>(h / 10.0f) % 36;
                bins[bin] += weight;
                binR[bin] += c.r * weight;
                binG[bin] += c.g * weight;
                binB[bin] += c.b * weight;
            }
        }
    }
    if (count == 0) return;

    g_palette.averageLuminance = static_cast<float>(lumSum / count);
    g_palette.averageColor = {static_cast<float>(sumR / count), static_cast<float>(sumG / count),
                              static_cast<float>(sumB / count)};

    int best = -1;
    float bestWeight = 0, total = 0;
    for (int i = 0; i < 36; ++i) {
        total += bins[i];
        const float weight = bins[i] + 0.5f * (bins[(i + 35) % 36] + bins[(i + 1) % 36]);
        if (weight > bestWeight) {
            bestWeight = weight;
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
                SetAccentFromBase({binR[best] / bins[best], binG[best] / bins[best], binB[best] / bins[best]},
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
    RgbToHsl(g_palette.averageColor, h, s, l);
    g_palette.tint = HslToRgb(h, (std::min)(s, 0.55f) * 0.9f, 0.10f);
}

// Shares the clock palette with the TAI taskbar mod (Windows-OCT accent twin).
// The taskbar watches HKCU\Software\Windows-OCT and restyles itself on change.
static void PublishAccent() {
    static DWORD lastAccent = 0xFFFFFFFF, lastAccentEnd = 0xFFFFFFFF, lastTint = 0xFFFFFFFF;
    const DWORD accent = PackRgb(g_palette.accent);
    const DWORD accentEnd = PackRgb(g_palette.accentEnd);
    const DWORD tint = PackRgb(g_palette.tint);
    if (accent == lastAccent && accentEnd == lastAccentEnd && tint == lastTint) return;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Windows-OCT", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    // Accent is written last; the taskbar debounces, so it never keeps a half-updated palette.
    RegSetValueExW(key, L"Tint", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&tint), sizeof(tint));
    RegSetValueExW(key, L"Accent2", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&accentEnd), sizeof(accentEnd));
    RegSetValueExW(key, L"Accent", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&accent), sizeof(accent));
    RegCloseKey(key);
    lastAccent = accent;
    lastAccentEnd = accentEnd;
    lastTint = tint;
}

// Fonts

static std::unique_ptr<FontFamily> PickFamily(std::initializer_list<std::pair<const wchar_t*, int>> candidates,
                                              int& styleOut) {
    for (const auto& [name, style] : candidates) {
        auto family = std::make_unique<FontFamily>(name);
        if (family->GetLastStatus() == Ok && family->IsAvailable() && family->IsStyleAvailable(style)) {
            styleOut = style;
            return family;
        }
    }
    styleOut = FontStyleBold;
    return std::make_unique<FontFamily>(L"Segoe UI");
}

// Layout

static void FreeFrameBuffer() {
    g_frameBitmap.reset();
    if (g_frameDc) {
        if (g_frameDcOldBitmap) SelectObject(g_frameDc, g_frameDcOldBitmap);
        DeleteDC(g_frameDc);
        g_frameDc = nullptr;
    }
    if (g_frameDib) {
        DeleteObject(g_frameDib);
        g_frameDib = nullptr;
    }
    g_frameDcOldBitmap = nullptr;
    g_frameBits = nullptr;
}

static void BuildLayout() {
    Layout& layout = g_layout;
    layout.monitor = PrimaryMonitorRect();
    const int monitorW = layout.monitor.right - layout.monitor.left;
    const int monitorH = layout.monitor.bottom - layout.monitor.top;

    // Same visual proportion on every resolution / DPI: 1.0 at 1920x1080.
    const float fitScale = (std::min)(monitorH / 1080.0f, monitorW / 1200.0f);
    const float scale = (std::clamp)(fitScale * g_settings.scalePercent / 100.0f, 0.35f, 4.0f);
    layout.scale = scale;

    const float margin = std::round(36.0f * scale);
    const float panelW = std::round(620.0f * scale);

    // Rows stacked top to bottom, in 1080p units until multiplied by scale.
    float y = 26.0f;
    if (g_settings.showGreeting) {
        layout.greetingY = y * scale;
        layout.greetingH = 28.0f * scale;
        y += 28.0f + 4.0f;
    }
    layout.digitsY = y * scale;
    layout.digitsH = 132.0f * scale;
    y += 132.0f;
    if (g_settings.showSeconds) {
        y += 16.0f;
        layout.barY = y * scale;
        y += 6.0f;
    }
    if (g_settings.showDate) {
        y += g_settings.showSeconds ? 16.0f : 10.0f;
        layout.dateY = y * scale;
        layout.dateH = 28.0f * scale;
        y += 28.0f;
    }
    y += 26.0f;
    const float panelH = std::round(y * scale);

    layout.winW = static_cast<int>(panelW + margin * 2);
    layout.winH = static_cast<int>(panelH + margin * 2);
    layout.panel = RectF(margin, margin, panelW, panelH);
    layout.radius = 30.0f * scale;

    // Shift rows into window coordinates.
    layout.greetingY += margin;
    layout.digitsY += margin;
    layout.barY += margin;
    layout.dateY += margin;

    layout.pos.x = layout.monitor.left + (monitorW - layout.winW) / 2;
    layout.pos.y = g_settings.centerVertically
                       ? layout.monitor.top + (monitorH - layout.winH) / 2
                       : layout.monitor.top + static_cast<int>(g_settings.topOffset * scale - margin);

    // Digit glyphs.
    int digitStyle = FontStyleBold;
    auto digitFamily = PickFamily({{L"Segoe UI Variable Display", FontStyleBold},
                                   {L"Segoe UI", FontStyleBold},
                                   {L"Segoe UI Semibold", FontStyleRegular}},
                                  digitStyle);
    const float emSize = 150.0f * scale;
    StringFormat typographic(StringFormat::GenericTypographic());
    float maxGlyphW = 0;
    layout.glyphTop = 1e9f;
    layout.glyphBottom = -1e9f;
    for (int d = 0; d < 10; ++d) {
        const wchar_t digitText[2] = {static_cast<wchar_t>(L'0' + d), 0};
        layout.digitPath[d] = std::make_unique<GraphicsPath>();
        layout.digitPath[d]->AddString(digitText, 1, digitFamily.get(), digitStyle, emSize, PointF(0, 0),
                                       &typographic);
        layout.digitPath[d]->GetBounds(&layout.digitBounds[d]);
        maxGlyphW = (std::max)(maxGlyphW, layout.digitBounds[d].Width);
        layout.glyphTop = (std::min)(layout.glyphTop, layout.digitBounds[d].Y);
        layout.glyphBottom = (std::max)(layout.glyphBottom, layout.digitBounds[d].GetBottom());
    }
    layout.digitCellW = maxGlyphW + 4.0f * scale;
    layout.digitGap = 8.0f * scale;
    layout.colonGap = 54.0f * scale;
    const float rowW = layout.digitCellW * 4 + layout.digitGap * 2 + layout.colonGap;
    const float centerX = layout.panel.X + layout.panel.Width / 2.0f;
    const float rowX = centerX - rowW / 2.0f;
    layout.digitCellX[0] = rowX;
    layout.digitCellX[1] = rowX + layout.digitCellW + layout.digitGap;
    layout.colonX = centerX;
    layout.digitCellX[2] = rowX + layout.digitCellW * 2 + layout.digitGap + layout.colonGap;
    layout.digitCellX[3] = layout.digitCellX[2] + layout.digitCellW + layout.digitGap;

    // Text fonts.
    layout.percentFont.reset();
    layout.labelFont.reset();
    layout.labelFamily = PickFamily({{L"Segoe UI Variable Text Semibold", FontStyleRegular},
                                     {L"Segoe UI Variable Text Semib", FontStyleRegular},
                                     {L"Segoe UI Semibold", FontStyleRegular},
                                     {L"Segoe UI", FontStyleBold}},
                                    layout.labelStyle);
    layout.labelFont =
        std::make_unique<Font>(layout.labelFamily.get(), 19.0f * scale, layout.labelStyle, UnitPixel);
    layout.percentFont =
        std::make_unique<Font>(layout.labelFamily.get(), 15.0f * scale, layout.labelStyle, UnitPixel);

    // Frame buffer.
    FreeFrameBuffer();
    HDC screenDc = GetDC(nullptr);
    g_frameDc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = layout.winW;
    bitmapInfo.bmiHeader.biHeight = -layout.winH;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_frameDib = CreateDIBSection(g_frameDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (g_frameDib && bits) {
        g_frameDcOldBitmap = SelectObject(g_frameDc, g_frameDib);
        g_frameBits = static_cast<uint32_t*>(bits);
        g_frameBitmap = std::make_unique<Bitmap>(layout.winW, layout.winH, layout.winW * 4,
                                                 PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
    }

    g_layoutDirty = false;
    g_glassDirty = true;
    g_staticDirty = true;
}

// Glass (blurred wallpaper behind the panel) and palette

static void BuildGlass() {
    g_glassBitmap.reset();
    g_glassDirty = false;
    g_staticDirty = true;

    const Layout& layout = g_layout;
    if (!g_wallpaper.valid) {
        // Keep defaults but still honour accent settings.
        g_palette = Palette{};
        if (g_settings.accentMode == AccentMode::Custom) {
            SetAccentFromBase(FromColorRef(g_settings.customAccent), true);
        } else if (g_settings.accentMode == AccentMode::Windows) {
            bool ok;
            const Rgb wa = WindowsAccent(ok);
            if (ok) SetAccentFromBase(wa, false);
        }
        PublishAccent();
        return;
    }

    // Panel rectangle in wallpaper-canvas pixels.
    const float canvasScale = g_wallpaper.scale;
    const float panelX = (layout.pos.x - g_wallpaper.monitor.left + layout.panel.X) * canvasScale;
    const float panelY = (layout.pos.y - g_wallpaper.monitor.top + layout.panel.Y) * canvasScale;
    const float panelW = layout.panel.Width * canvasScale;
    const float panelH = layout.panel.Height * canvasScale;

    // Palette from a slightly larger area so the accent represents the surroundings.
    AnalyzeWallpaperRegion(static_cast<int>(panelX - panelW * 0.25f), static_cast<int>(panelY - panelH * 0.25f),
                           static_cast<int>(panelX + panelW * 1.25f), static_cast<int>(panelY + panelH * 1.25f));
    PublishAccent();

    const float blurRadius = (6.0f + g_settings.blurStrength * 0.55f) * layout.scale;
    // Three box passes approximate a gaussian, hence the 1.7 divisor.
    const int boxRadius = (std::max)(1, static_cast<int>(blurRadius * canvasScale / 1.7f + 0.5f));
    const int pad = boxRadius * 3;
    const int cropX = static_cast<int>(std::floor(panelX)) - pad;
    const int cropY = static_cast<int>(std::floor(panelY)) - pad;
    const int cropW = static_cast<int>(std::ceil(panelW)) + pad * 2 + 1;
    const int cropH = static_cast<int>(std::ceil(panelH)) + pad * 2 + 1;
    if (cropW <= 0 || cropH <= 0) return;

    std::vector<uint32_t> crop(static_cast<size_t>(cropW) * cropH);
    for (int y = 0; y < cropH; ++y) {
        const int srcY = (std::clamp)(cropY + y, 0, g_wallpaper.height - 1);
        for (int x = 0; x < cropW; ++x) {
            const int srcX = (std::clamp)(cropX + x, 0, g_wallpaper.width - 1);
            crop[static_cast<size_t>(y) * cropW + x] =
                g_wallpaper.pixels[static_cast<size_t>(srcY) * g_wallpaper.width + srcX];
        }
    }
    if (g_settings.blurStrength > 0) BoxBlur(crop, cropW, cropH, boxRadius, 3);

    // Acrylic-like saturation boost.
    for (auto& p : crop) {
        Rgb c = UnpackRgb(p);
        const float lum = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
        c = {Clamp01(lum + (c.r - lum) * 1.25f), Clamp01(lum + (c.g - lum) * 1.25f),
             Clamp01(lum + (c.b - lum) * 1.25f)};
        p = 0xFF000000u | PackRgb(c);
    }

    Bitmap cropBitmap(cropW, cropH, cropW * 4, PixelFormat32bppARGB, reinterpret_cast<BYTE*>(crop.data()));
    if (cropBitmap.GetLastStatus() != Ok) return;

    const int glassW = static_cast<int>(std::ceil(layout.panel.Width));
    const int glassH = static_cast<int>(std::ceil(layout.panel.Height));
    g_glassBitmap = std::make_unique<Bitmap>(glassW, glassH, PixelFormat32bppPARGB);
    if (g_glassBitmap->GetLastStatus() != Ok) {
        g_glassBitmap.reset();
        return;
    }
    Graphics g(g_glassBitmap.get());
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    ImageAttributes attr;
    attr.SetWrapMode(WrapModeTileFlipXY);
    g.DrawImage(&cropBitmap, RectF(0, 0, static_cast<REAL>(glassW), static_cast<REAL>(glassH)),
                panelX - static_cast<float>(cropX), panelY - static_cast<float>(cropY), panelW, panelH,
                UnitPixel, &attr);
}

// Static layer (shadow, glass, tint, sheen, border)

static void BuildStaticLayer() {
    const Layout& layout = g_layout;
    const RectF& panel = layout.panel;
    const size_t pixelCount = static_cast<size_t>(layout.winW) * layout.winH;
    g_staticLayer.assign(pixelCount, 0);
    g_staticDirty = false;
    g_contentDirty = true;
    if (!g_settings.showPanel || pixelCount == 0) return;

    const float scale = layout.scale;
    BYTE* layerBytes = reinterpret_cast<BYTE*>(g_staticLayer.data());

    // Soft drop shadow.
    {
        Bitmap shadowBitmap(layout.winW, layout.winH, layout.winW * 4, PixelFormat32bppPARGB, layerBytes);
        Graphics g(&shadowBitmap);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        GraphicsPath shadowPath;
        AddRoundRect(shadowPath,
                     RectF(panel.X + 6 * scale, panel.Y + 14 * scale, panel.Width - 12 * scale,
                           panel.Height - 10 * scale),
                     layout.radius);
        SolidBrush shadowBrush(Color(static_cast<BYTE>(110 + 50 * g_palette.averageLuminance), 0, 0, 0));
        g.FillPath(&shadowBrush, &shadowPath);
    }
    BoxBlur(g_staticLayer, layout.winW, layout.winH, (std::max)(1, static_cast<int>(10 * scale)), 3);

    Bitmap layerBitmap(layout.winW, layout.winH, layout.winW * 4, PixelFormat32bppPARGB, layerBytes);
    Graphics g(&layerBitmap);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetInterpolationMode(InterpolationModeHighQualityBilinear);

    GraphicsPath panelPath;
    AddRoundRect(panelPath, panel, layout.radius);

    // Frosted wallpaper.
    if (g_glassBitmap) {
        TextureBrush glassBrush(g_glassBitmap.get(), WrapModeTileFlipXY);
        glassBrush.TranslateTransform(panel.X, panel.Y);
        g.FillPath(&glassBrush, &panelPath);
    } else {
        SolidBrush fallback(MakeColor(0.85f, Mix(g_palette.tint, Rgb{0.05f, 0.25f, 0.32f}, 0.5f)));
        g.FillPath(&fallback, &panelPath);
    }

    // Tint, stronger on bright wallpapers so white text always reads well.
    float tintAlpha = g_settings.glassTint / 100.0f;
    tintAlpha += (std::max)(0.0f, g_palette.averageLuminance - 0.42f) * 0.75f;
    tintAlpha = (std::clamp)(tintAlpha, 0.0f, 0.92f);
    SolidBrush tintBrush(MakeColor(tintAlpha, g_palette.tint));
    g.FillPath(&tintBrush, &panelPath);

    // Accent glow rising from the bottom.
    {
        g.SetClip(&panelPath);
        GraphicsPath glowPath;
        const float glowW = panel.Width * 0.95f;
        const float glowH = panel.Height * 0.9f;
        glowPath.AddEllipse(panel.X + (panel.Width - glowW) / 2, panel.GetBottom() - glowH * 0.5f, glowW, glowH);
        PathGradientBrush glowBrush(&glowPath);
        glowBrush.SetCenterColor(MakeColor(0.20f, g_palette.accent));
        Color surround = MakeColor(0.0f, g_palette.accent);
        int surroundCount = 1;
        glowBrush.SetSurroundColors(&surround, &surroundCount);
        g.FillPath(&glowBrush, &glowPath);
        g.ResetClip();
    }

    // Glass sheen on the upper half.
    {
        LinearGradientBrush sheen(PointF(0, panel.Y - 1), PointF(0, panel.GetBottom() + 1),
                                  Color(40, 255, 255, 255), Color(0, 255, 255, 255));
        Color colors[3] = {Color(44, 255, 255, 255), Color(10, 255, 255, 255), Color(0, 255, 255, 255)};
        REAL positions[3] = {0.0f, 0.45f, 1.0f};
        sheen.SetInterpolationColors(colors, positions, 3);
        g.FillPath(&sheen, &panelPath);
    }

    // Edge light: bright top edge fading to a faint bottom edge.
    {
        const float inset = 0.6f * scale;
        GraphicsPath edgePath;
        AddRoundRect(edgePath,
                     RectF(panel.X + inset, panel.Y + inset, panel.Width - inset * 2, panel.Height - inset * 2),
                     layout.radius - inset);
        LinearGradientBrush edgeBrush(PointF(0, panel.Y - 1), PointF(0, panel.GetBottom() + 1),
                                      Color(110, 255, 255, 255), Color(22, 255, 255, 255));
        Pen edgePen(&edgeBrush, (std::max)(1.0f, 1.2f * scale));
        g.DrawPath(&edgePen, &edgePath);
    }
}

// Frame drawing

// Text and digit shadows get darker on bright wallpapers and without the panel behind them.
static float ShadowStrength() {
    float strength = 0.8f + g_palette.averageLuminance * 1.0f;
    if (!g_settings.showPanel) strength *= 1.7f;
    return strength;
}

static void DrawGlyph(Graphics& g, int digit, float cellX, float alpha, float dy) {
    const Layout& layout = g_layout;
    if (digit < 0 || digit > 9 || alpha <= 0.003f) return;
    const RectF& bounds = layout.digitBounds[digit];
    const float glyphH = layout.glyphBottom - layout.glyphTop;
    const float glyphX = cellX + (layout.digitCellW - bounds.Width) / 2.0f - bounds.X;
    const float glyphY = layout.digitsY + (layout.digitsH - glyphH) / 2.0f - layout.glyphTop + dy;
    const float shadowStrength = ShadowStrength();

    const float shadowOffsets[3] = {2.0f, 4.5f, 8.0f};
    const float shadowAlphas[3] = {0.20f, 0.12f, 0.06f};
    for (int i = 0; i < 3; ++i) {
        g.TranslateTransform(glyphX, glyphY + shadowOffsets[i] * layout.scale);
        SolidBrush shadowBrush(Color(ToByte(shadowAlphas[i] * shadowStrength * alpha), 0, 10, 16));
        g.FillPath(&shadowBrush, layout.digitPath[digit].get());
        g.ResetTransform();
    }

    g.TranslateTransform(glyphX, glyphY);
    const Rgb top{1.0f, 1.0f, 1.0f};
    const Rgb bottom = Mix(top, g_palette.accent, 0.22f);
    LinearGradientBrush fillBrush(PointF(0, layout.glyphTop - 1), PointF(0, layout.glyphBottom + 1),
                                  MakeColor(alpha, top), MakeColor(alpha, bottom));
    g.FillPath(&fillBrush, layout.digitPath[digit].get());
    g.ResetTransform();
}

// Starts a roll for every digit that changed; right-most digit first, 80 ms apart.
static void UpdateDigits(const int digits[4], ULONGLONG now) {
    int order = 0;
    for (int i = 3; i >= 0; --i) {
        DigitRoll& roll = g_digitRolls[i];
        if (roll.current == digits[i]) continue;
        if (roll.current < 0 || !g_settings.animateDigits) {
            roll.previous = -1;
        } else {
            roll.previous = roll.current;
            roll.startTick = now + static_cast<ULONGLONG>(order) * 80;
            ++order;
        }
        roll.current = digits[i];
    }
}

static void DrawDigits(Graphics& g, ULONGLONG now) {
    const Layout& layout = g_layout;
    const float scale = layout.scale;
    const float glyphH = layout.glyphBottom - layout.glyphTop;
    const float glyphY = layout.digitsY + (layout.digitsH - glyphH) / 2.0f;
    const float rollDistance = glyphH * 0.85f;
    for (int i = 0; i < 4; ++i) {
        DigitRoll& roll = g_digitRolls[i];
        const float cellX = layout.digitCellX[i];
        // Odometer window: digits roll in/out of the glyph band only.
        g.SetClip(RectF(cellX - 12 * scale, glyphY - 3 * scale, layout.digitCellW + 24 * scale,
                        glyphH + 14 * scale));
        if (roll.previous >= 0) {
            const float t = now < roll.startTick ? 0.0f : static_cast<float>(now - roll.startTick) / 520.0f;
            if (t >= 1.0f) {
                roll.previous = -1;
                DrawGlyph(g, roll.current, cellX, 1.0f, 0);
            } else {
                const float eased = EaseOutCubic(t);
                DrawGlyph(g, roll.previous, cellX, 1.0f - eased, -eased * rollDistance);
                DrawGlyph(g, roll.current, cellX, eased, (1.0f - eased) * rollDistance);
            }
        } else {
            DrawGlyph(g, roll.current, cellX, 1.0f, 0);
        }
        g.ResetClip();
    }
}

static void FillGlow(Graphics& g, float cx, float cy, float radius, const Rgb& c, float alpha) {
    GraphicsPath glowPath;
    glowPath.AddEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
    PathGradientBrush glowBrush(&glowPath);
    glowBrush.SetCenterColor(MakeColor(alpha, c));
    Color surround = MakeColor(0.0f, c);
    int surroundCount = 1;
    glowBrush.SetSurroundColors(&surround, &surroundCount);
    g.FillPath(&glowBrush, &glowPath);
}

static void DrawColon(Graphics& g, ULONGLONG now) {
    const Layout& layout = g_layout;
    const float glyphH = layout.glyphBottom - layout.glyphTop;
    const float midY = layout.digitsY + layout.digitsH / 2.0f;
    // Breathes on a 2 s sine cycle.
    const float breath = 0.5f + 0.5f * std::sin(static_cast<float>(now % 2000) / 2000.0f * 2 * kPi);
    const float alpha = 0.55f + 0.45f * breath;
    const float dotR = 6.0f * layout.scale;
    for (float cy : {midY - glyphH * 0.19f, midY + glyphH * 0.19f}) {
        FillGlow(g, layout.colonX, cy, dotR * 3.6f, g_palette.accent, 0.42f * alpha);
        SolidBrush dot(MakeColor(alpha, g_palette.accent));
        g.FillEllipse(&dot, layout.colonX - dotR, cy - dotR, dotR * 2, dotR * 2);
    }
}

static void DrawShadowedText(Graphics& g, const std::wstring& text, const Font& font, const PointF& at,
                             const StringFormat& format, const Color& color) {
    SolidBrush shadowBrush(Color(ToByte(0.30f * ShadowStrength()), 0, 10, 16));
    g.DrawString(text.c_str(), -1, &font, PointF(at.X, at.Y + 1.5f * g_layout.scale), &format, &shadowBrush);
    SolidBrush textBrush(color);
    g.DrawString(text.c_str(), -1, &font, at, &format, &textBrush);
}

static void DrawSunMoon(Graphics& g, int period, float cx, float cy, float size) {
    if (period == 3) {
        // Crescent moon.
        GraphicsPath outer, inner;
        outer.AddEllipse(cx - size, cy - size, size * 2, size * 2);
        inner.AddEllipse(cx - size * 0.35f, cy - size * 1.25f, size * 2, size * 2);
        Region crescent(&outer);
        crescent.Exclude(&inner);
        FillGlow(g, cx, cy, size * 2.4f, Rgb{0.80f, 0.86f, 1.0f}, 0.22f);
        SolidBrush moonBrush(Color(240, 232, 240, 255));
        g.FillRegion(&moonBrush, &crescent);
        return;
    }
    const Rgb sunColor = period == 0 ? Rgb{1.0f, 0.86f, 0.45f}
                                     : (period == 1 ? Rgb{1.0f, 0.82f, 0.30f} : Rgb{1.0f, 0.62f, 0.35f});
    FillGlow(g, cx, cy, size * 2.6f, sunColor, 0.30f);
    SolidBrush core(MakeColor(1.0f, sunColor));
    const float coreR = size * 0.55f;
    g.FillEllipse(&core, cx - coreR, cy - coreR, coreR * 2, coreR * 2);
    Pen ray(MakeColor(0.95f, sunColor), (std::max)(1.0f, 1.8f * g_layout.scale));
    ray.SetStartCap(LineCapRound);
    ray.SetEndCap(LineCapRound);
    for (int i = 0; i < 8; ++i) {
        const float angle = i * kPi / 4.0f;
        const float innerR = size * 0.85f, outerR = size * 1.2f;
        g.DrawLine(&ray, cx + std::cos(angle) * innerR, cy + std::sin(angle) * innerR,
                   cx + std::cos(angle) * outerR, cy + std::sin(angle) * outerR);
    }
}

static void DrawGreeting(Graphics& g, const SYSTEMTIME& st) {
    const Layout& layout = g_layout;
    const float scale = layout.scale;
    const int period = DayPeriod(st.wHour);
    const std::wstring text = GreetingText(period);

    StringFormat typographic(StringFormat::GenericTypographic());
    RectF textBounds;
    g.MeasureString(text.c_str(), -1, layout.labelFont.get(), PointF(0, 0), &typographic, &textBounds);
    const float iconR = 9.0f * scale;
    const float gap = 12.0f * scale;
    const float rowW = iconR * 2 + gap + textBounds.Width;
    const float x = layout.panel.X + layout.panel.Width / 2.0f - rowW / 2.0f;
    const float cy = layout.greetingY + layout.greetingH / 2.0f;

    DrawSunMoon(g, period, x + iconR, cy, iconR);
    DrawShadowedText(g, text, *layout.labelFont, PointF(x + iconR * 2 + gap, cy - textBounds.Height / 2.0f),
                     typographic, Color(220, 240, 250, 255));
}

static void DrawSecondsBar(Graphics& g, const SYSTEMTIME& st) {
    const Layout& layout = g_layout;
    const float scale = layout.scale;
    const float trackW = 360.0f * scale;
    const float trackH = 4.0f * scale;
    const float trackX = layout.panel.X + (layout.panel.Width - trackW) / 2.0f;
    const float trackY = layout.barY;

    GraphicsPath track;
    AddCapsule(track, trackX, trackY, trackW, trackH);
    SolidBrush trackBrush(Color(46, 255, 255, 255));
    g.FillPath(&trackBrush, &track);

    const float progress = (st.wSecond + st.wMilliseconds / 1000.0f) / 60.0f;
    const float fillW = trackW * progress;
    if (fillW >= trackH) {
        GraphicsPath fill;
        AddCapsule(fill, trackX, trackY, fillW, trackH);
        LinearGradientBrush fillBrush(PointF(trackX - 1, trackY), PointF(trackX + trackW + 1, trackY),
                                      MakeColor(1.0f, g_palette.accent), MakeColor(1.0f, g_palette.accentEnd));
        g.FillPath(&fillBrush, &fill);
    }

    const float headX = trackX + (std::max)(fillW, trackH / 2.0f);
    const float headY = trackY + trackH / 2.0f;
    const float headR = 5.0f * scale;
    FillGlow(g, headX, headY, headR * 3.2f, g_palette.accent, 0.45f);
    SolidBrush head(MakeColor(1.0f, Mix(g_palette.accent, Rgb{1, 1, 1}, 0.45f)));
    g.FillEllipse(&head, headX - headR, headY - headR, headR * 2, headR * 2);
}

static void DrawDateRow(Graphics& g, const SYSTEMTIME& st) {
    const Layout& layout = g_layout;
    const float scale = layout.scale;
    const std::wstring date = FormatDate(st);
    StringFormat typographic(StringFormat::GenericTypographic());
    RectF dateBounds;
    g.MeasureString(date.c_str(), -1, layout.labelFont.get(), PointF(0, 0), &typographic, &dateBounds);

    wchar_t percentText[16]{};
    const float dayProgress = (st.wHour * 3600.0f + st.wMinute * 60.0f + st.wSecond) / 86400.0f;
    swprintf_s(percentText, L"%d%%", static_cast<int>(dayProgress * 100.0f));
    RectF percentBounds;
    const bool showRing = g_settings.showDayProgress;
    const float ringD = 17.0f * scale;
    const float ringGap = 18.0f * scale;
    float rowW = dateBounds.Width;
    if (showRing) {
        g.MeasureString(percentText, -1, layout.percentFont.get(), PointF(0, 0), &typographic, &percentBounds);
        rowW += ringGap + ringD + 7.0f * scale + percentBounds.Width;
    }

    float x = layout.panel.X + layout.panel.Width / 2.0f - rowW / 2.0f;
    const float cy = layout.dateY + layout.dateH / 2.0f;
    DrawShadowedText(g, date, *layout.labelFont, PointF(x, cy - dateBounds.Height / 2.0f), typographic,
                     Color(230, 244, 252, 255));
    x += dateBounds.Width;

    if (!showRing) return;
    x += ringGap;
    const float ringWidth = (std::max)(1.5f, 2.6f * scale);
    RectF ringRect(x, cy - ringD / 2.0f, ringD, ringD);
    Pen trackPen(Color(60, 255, 255, 255), ringWidth);
    g.DrawEllipse(&trackPen, ringRect);
    LinearGradientBrush arcBrush(PointF(ringRect.X, ringRect.Y), PointF(ringRect.GetRight(), ringRect.GetBottom()),
                                 MakeColor(1.0f, g_palette.accent), MakeColor(1.0f, g_palette.accentEnd));
    Pen arc(&arcBrush, ringWidth);
    arc.SetStartCap(LineCapRound);
    arc.SetEndCap(LineCapRound);
    const float sweep = (std::max)(2.0f, 360.0f * dayProgress);
    g.DrawArc(&arc, ringRect, -90.0f, sweep);
    x += ringD + 7.0f * scale;
    DrawShadowedText(g, percentText, *layout.percentFont, PointF(x, cy - percentBounds.Height / 2.0f),
                     typographic, MakeColor(0.95f, Mix(g_palette.accent, Rgb{1, 1, 1}, 0.35f)));
}

static void SetDrawQuality(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetCompositingQuality(CompositingQualityHighQuality);
}

static void DrawContentLayer(const SYSTEMTIME& st, ULONGLONG now) {
    const Layout& layout = g_layout;
    g_contentLayer = g_staticLayer;
    Bitmap contentBitmap(layout.winW, layout.winH, layout.winW * 4, PixelFormat32bppPARGB,
                         reinterpret_cast<BYTE*>(g_contentLayer.data()));
    Graphics g(&contentBitmap);
    SetDrawQuality(g);

    DrawDigits(g, now);
    if (!g_settings.use24Hour) {
        StringFormat typographic(StringFormat::GenericTypographic());
        const std::wstring meridiem = st.wHour >= 12 ? L"PM" : L"AM";
        const float x = layout.digitCellX[3] + layout.digitCellW + 10.0f * layout.scale;
        const float y = layout.digitsY + layout.digitsH * 0.62f;
        DrawShadowedText(g, meridiem, *layout.labelFont, PointF(x, y), typographic,
                         MakeColor(1.0f, Mix(g_palette.accent, Rgb{1, 1, 1}, 0.2f)));
    }
    if (g_settings.showGreeting) DrawGreeting(g, st);
    if (g_settings.showDate) DrawDateRow(g, st);
}

static void RenderFrame(ULONGLONG now) {
    const Layout& layout = g_layout;
    if (!g_frameBitmap || !g_frameBits || g_staticLayer.size() != static_cast<size_t>(layout.winW) * layout.winH)
        return;

    SYSTEMTIME st{};
    GetLocalTime(&st);

    int hour = st.wHour;
    if (!g_settings.use24Hour) {
        hour %= 12;
        if (hour == 0) hour = 12;
    }
    const int digits[4] = {hour / 10, hour % 10, st.wMinute / 10, st.wMinute % 10};
    UpdateDigits(digits, now);
    // Checked before drawing: the frame that finishes a roll must still be redrawn and cached.
    bool rolling = false;
    for (const DigitRoll& roll : g_digitRolls) rolling |= roll.previous >= 0;

    SYSTEMTIME second = st;
    second.wMilliseconds = 0;
    if (g_contentDirty || rolling || memcmp(&second, &g_contentTime, sizeof(second)) != 0) {
        DrawContentLayer(st, now);
        g_contentTime = second;
        g_contentDirty = false;
    }

    memcpy(g_frameBits, g_contentLayer.data(), g_contentLayer.size() * 4);
    {
        Graphics g(g_frameBitmap.get());
        SetDrawQuality(g);
        DrawColon(g, now);
        if (g_settings.showSeconds) DrawSecondsBar(g, st);
    }

    const float fade = EaseOutCubic(static_cast<float>(now - g_fadeInStart) / 700.0f);
    POINT dst = layout.pos;
    SIZE size{layout.winW, layout.winH};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, ToByte(fade), AC_SRC_ALPHA};
    HDC screenDc = GetDC(nullptr);
    UpdateLayeredWindow(g_clockWindow, screenDc, &dst, &size, g_frameDc, &src, 0, &blend, ULW_ALPHA);
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

    if (now - g_lastFullscreenCheck >= 1000) {
        g_lastFullscreenCheck = now;
        g_paused = IsFullscreenAppRunning();
    }
    if (g_paused) return;

    if (g_canvasDirty || g_forceWallpaperCheck || now - g_lastWallpaperCheck >= 5000) {
        g_lastWallpaperCheck = now;
        CheckWallpaper(g_canvasDirty || g_forceWallpaperCheck);
        g_canvasDirty = false;
        g_forceWallpaperCheck = false;
    }
    if (g_layoutDirty) BuildLayout();
    if (g_glassDirty) BuildGlass();
    if (g_staticDirty) BuildStaticLayer();
    RenderFrame(now);
}

// Window and thread

static LRESULT CALLBACK ClockWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            if (wParam == kWallpaperReloadTimerId) {
                KillTimer(hwnd, kWallpaperReloadTimerId);
                g_forceWallpaperCheck = true;
            }
            Tick();
            return 0;

        case WM_APP_REFRESH:
            LoadSettings();
            g_userName = DetectUserName();
            for (auto& roll : g_digitRolls) roll = DigitRoll{};
            g_layoutDirty = true;
            g_forceWallpaperCheck = true;
            Tick();
            return 0;

        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETDESKWALLPAPER) {
                // Give Windows a moment to finish writing the new wallpaper.
                SetTimer(hwnd, kWallpaperReloadTimerId, 600, nullptr);
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
            KillTimer(hwnd, kFrameTimerId);
            KillTimer(hwnd, kWallpaperReloadTimerId);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ReleaseGraphicsResources() {
    FreeFrameBuffer();
    g_glassBitmap.reset();
    for (auto& path : g_layout.digitPath) path.reset();
    g_layout.labelFont.reset();
    g_layout.percentFont.reset();
    g_layout.labelFamily.reset();
    g_staticLayer.clear();
    g_contentLayer.clear();
    g_contentDirty = true;
    g_wallpaper = WallpaperCanvas{};
}

static DWORD WINAPI ClockThreadProc(void*) {
    g_threadId = GetCurrentThreadId();

    // Work in physical pixels on every monitor / scale.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    GdiplusStartupInput startupInput;
    if (GdiplusStartup(&g_gdiplusToken, &startupInput, nullptr) != Ok) {
        Wh_Log(L"Desktop Glass Clock: GDI+ startup failed");
        if (SUCCEEDED(comInit)) CoUninitialize();
        return 0;
    }

    LoadSettings();
    g_userName = DetectUserName();

    const wchar_t kClassName[] = L"WindhawkDesktopGlassClock";
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = g_hInstance;
    windowClass.lpfnWndProc = ClockWndProc;
    windowClass.lpszClassName = kClassName;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&windowClass);

    g_clockWindow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                    kClassName, L"", WS_POPUP | WS_VISIBLE, 0, 0, 1, 1, nullptr, nullptr,
                                    g_hInstance, nullptr);
    if (!g_clockWindow) {
        Wh_Log(L"Desktop Glass Clock: CreateWindowEx failed: %u", GetLastError());
        UnregisterClassW(kClassName, g_hInstance);
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
        if (SUCCEEDED(comInit)) CoUninitialize();
        return 0;
    }

    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        SetWindowPos(g_clockWindow, progman, 0, 0, 1, 1,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    ShowWindow(g_clockWindow, SW_SHOWNOACTIVATE);

    g_fadeInStart = GetTickCount64();
    Tick();
    SetTimer(g_clockWindow, kFrameTimerId, 33, nullptr);

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
    if (SUCCEEDED(comInit)) CoUninitialize();
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
