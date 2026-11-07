# Rapikan dan percepat CLOCK.cpp

Status: IN_PROGRESS
Plan Depth: STANDARD
Current Phase: Phase 1 — Harness dan golden baseline
Last Updated: 2026-11-07

## Executive Snapshot

`CLOCK.cpp` (mod Windhawk "Desktop Glass Clock", 1710 baris) dirapikan dalam dua langkah. Langkah pertama: ganti nama yang tidak jelas, hapus duplikasi, dan bersihkan komentar. Langkah kedua: tambah cache layer konten, sehingga setiap frame hanya menggambar ulang titik dua dan bar detik.

Baseline: render ~5,3 ms CPU per frame pada 30 fps. Dari jumlah itu, angka jam memakan ~3,8 ms.

Uji urutan gambar baru di salinan scratch: 154 frame, 14 variasi setting, 0 piksel beda. Kode di repo belum diubah.

## Objective

Kode lebih mudah dibaca, dan biaya CPU per frame turun. Output piksel dan cara build tidak berubah.

## Acceptance Criteria

- [ ] R1: `CLOCK.cpp` ter-build dengan perintah Windhawk 1.7.3 yang sama (`build.sh`). Exit 0, tanpa warning dengan `-Wall -Wextra`.
- [ ] R2: Matriks harness (16 variasi setting × 12 frame) menunjukkan `mismatched=0` terhadap golden baseline.
- [ ] R3: Median ms/frame dan CPU thread per frame di harness tidak lebih buruk dari baseline (target: jauh lebih baik), diukur dengan perintah dan jumlah frame yang sama.
- [ ] R4: Tidak ada lagi identifier dari daftar rename (bagian "Rename map") di `CLOCK.cpp`.
- [ ] R5: Header Windhawk (`@id`, `@name`, `@version`, `@compilerOptions`), readme, dan blok settings tidak berubah satu byte pun.

## Scope

In scope: `CLOCK.cpp` saja. Ini mencakup rename, dedupe yang tidak mengubah perilaku, perapian komentar (antislop-code, mode DURING), dan cache layer konten.

Out of scope:
- Teks readme/settings.
- Bug di `Wh_ModUninit` (wait 3 detik lalu DLL tetap di-unload) dan akses `g_clockWindow` lintas thread.
- Rebuild canvas ganda saat startup.
- `TASKBAR MOD.cpp`.
- Commit/push (belum diizinkan).

## User Decisions

- D-001: Cakupan = rapikan + cache layer (dipilih Andino).
- D-002: Ukuran visual = identik per piksel, 0 piksel beda di semua skenario harness.
- D-003: `ai-codebase-rescue` dipakai terbatas, hanya untuk tabel disposisi di bawah.
- D-004: Teks readme dan deskripsi setting tidak diubah.
- D-005: antislop mode DURING, hanya untuk komentar kode.
- D-006: Council (5 advisor + 2 reviewer, sonnet) merekomendasikan opsi B bertahap: rapikan dulu, cache kemudian, masing-masing dengan gerbang 0 piksel beda.

## Baseline / Starting Evidence

Alat ada di scratchpad sesi: `C:\Users\Andino\AppData\Local\Temp\claude\307eea11e3f8219c\379f44dc-9aa8-467b-9647-4a99b474b180\scratchpad\`. Di bawah ini path ditulis relatif terhadap folder itu.

- `build.sh <src> <out.dll>`: menyalin `compileModInternal` Windhawk 1.7.3 (`clang++ -std=c++23 -O2 -shared ... windhawk.lib -include windhawk_api.h -target x86_64-w64-mingw32 -Wl,--export-all-symbols` + lib dari `@compilerOptions`), ditambah `-Wall -Wextra`. Baseline: exit 0, 0 warning.
- `harness/harness.cpp`: meng-`#include` mod dengan stub Windhawk API, jam palsu (`GetLocalTime`, `GetTickCount64`), `SetTimer` no-op, dan hook `UpdateLayeredWindow` yang menangkap DIB. Harness hanya bergantung pada nama kelas jendela `WindhawkDesktopGlassClock`, global `g_threadId`, `Wh_ModInit`/`Wh_ModUninit`, dan `WM_TIMER` wParam 1.
- `harness/build_harness.sh <mod.cpp> <out.exe>`, `harness/matrix.sh <exe> <outdir>`, `harness/cmp.sh <exe> <dirA> <dirB>`.
- Golden baseline: `harness/m_base` dari `harness/CLOCK_baseline.cpp` (salinan HEAD `d18928e`). Dua run baseline: `mismatched=0` (154 frame).
- Bench baseline (`h_base.exe bench x 1800`, 3 run): median 5,30–5,39 ms, CPU thread 5,16–5,67 ms/frame, p95 ~6,0 ms.
- Profil: `DrawDigits` dimatikan → median 1,8 ms. Jadi digit ≈ 3,8 ms. Toggle lain tidak stabil.
- Probe urutan gambar (`CLOCK_reorder.cpp`: titik dua dan bar detik dipindah ke akhir): `mismatched=0` dari 154.

## Constraints & Invariants

- Urutan gambar di dalam layer konten: digit → AM/PM → sapaan → tanggal. Lalu per frame: titik dua → bar detik. Urutan ini sudah dibuktikan identik dengan urutan lama, karena area glow titik dua dan bar detik tidak beririsan dengan teks.
- Semua `Graphics` yang menggambar konten memakai 4 setting yang sama seperti `RenderFrame` lama: AntiAlias, PixelOffsetHalf, TextRenderingHintAntiAlias, CompositingQualityHighQuality.
- Nilai registry `Software\Windows-OCT` (`Accent`, `Accent2`, `Tint`) dan nama kelas jendela tidak berubah.
- `g_threadId`, `Wh_ModInit`, `Wh_ModSettingsChanged`, `Wh_ModUninit` tetap.

## Disposisi per area (ai-codebase-rescue, terbatas)

| Area | Disposisi | Alasan |
| --- | --- | --- |
| Render per frame (`RenderFrame`) | REFACTOR | 70% biaya ada di digit yang identik 59 dari 60 detik. Cache layer konten dengan gerbang piksel. |
| Nama variabel/fungsi (daftar rename) | REFACTOR | Nama satu-dua huruf di fungsi panjang. |
| Copy `LockBits` di `BuildCanvas`/`BuildGlass` | REMOVE | `Bitmap` dibuat langsung di atas buffer `std::vector`, sama seperti `g_static` sekarang. Loop copy hilang. |
| Bongkar-pasang ARGB (4 tempat) | REFACTOR | Satu helper `UnpackRgb`. Packing memakai `PackRgb` yang sudah ada. |
| `greetFont` == `dateFont` | REMOVE | Parameter identik. Disatukan jadi `labelFont`. |
| `#include <atomic>` | REMOVE | Tidak dipakai. |
| Pilihan aksen ganda (`AnalyzeRegion` vs fallback `BuildGlass`) | KEEP | Kelihatannya duplikat, tapi semantik fallback-nya berbeda. Tanpa canvas, mode wallpaper memakai palet default, dan mode Windows yang gagal juga memakai default mentah. Kalau disatukan, perilakunya berubah. |
| `StringFormat` di-copy per panggilan | KEEP | Setelah cache, panggilannya ~1 kali per detik. Tidak sebanding dengan tambahan state. |
| `GetDC` per frame, polling wallpaper 5 detik | KEEP | Gain kecil dan tidak terukur. Di luar target. |
| Banner komentar `// ----` | REFACTOR | Diganti satu baris label biasa (antislop-code: decorative separators). |
| Komentar bernomor `// 1. Frosted wallpaper.` | REFACTOR | Nomor dibuang, labelnya tetap. |

## Rename map

Globals dan tipe:

| Lama | Baru |
| --- | --- |
| `kTimerId` / `kReloadTimerId` | `kFrameTimerId` / `kWallpaperReloadTimerId` |
| `WallCanvas` / `g_canvas` | `WallpaperCanvas` / `g_wallpaper` |
| `WallCanvas::px/w/h/k` | `pixels/width/height/scale` |
| `WallInfo` / `WallKey()` / `g_wallKey` | `WallpaperInfo` / `WallpaperKey()` / `g_wallpaperKey` |
| `Palette::accent2` / `average` / `avgLum` | `accentEnd` / `averageColor` / `averageLuminance` |
| `Layout::s` | `scale` |
| `Layout::greetY/greetH` | `greetingY/greetingH` |
| `Layout::cellW/innerGap/colonGap/cellX` | `digitCellW/digitGap/colonGap/digitCellX` |
| `Layout::textFamily/textStyle` | `labelFamily/labelStyle` |
| `Layout::greetFont` + `dateFont` | `labelFont` |
| `Layout::smallFont` | `percentFont` |
| `g_static` / `g_glass` | `g_staticLayer` / `g_glassBitmap` |
| `g_memDc` / `g_dib` / `g_oldBitmap` / `g_frameBmp` | `g_frameDc` / `g_frameDib` / `g_frameDcOldBitmap` / `g_frameBitmap` |
| `g_forceWallCheck` / `g_lastWallCheck` / `g_lastFsCheck` / `g_fadeStart` | `g_forceWallpaperCheck` / `g_lastWallpaperCheck` / `g_lastFullscreenCheck` / `g_fadeInStart` |
| `DigitAnim{cur,prev,start}` / `g_digitAnim` | `DigitRoll{current,previous,startTick}` / `g_digitRolls` |
| `ShadowK()` / `AnalyzeRegion()` / `BuildStatic()` | `ShadowStrength()` / `AnalyzeWallpaperRegion()` / `BuildStaticLayer()` |

Locals: `Layout& L` → `layout`, `s` → `scale`, `k` → `canvasScale`, `px/py/pw/ph` → `panelX/panelY/panelW/panelH` (canvas px), `cw/ch` → `cropW/cropH` atau `canvasW/canvasH`, `bd` dihapus bersama LockBits, `tb/db/pb` → `textBounds/dateBounds/percentBounds`, `pp/sp/gp/bp` → `panelPath/shadowPath/glowPath/edgePath`, `pg` → `glowBrush`, `eb` → `edgeBrush`, `lb` → `fillBrush`, `sb` → `shadowBrush`, `sk` → `shadowStrength`, `br/bg/bb` (array bin) → `binR/binG/binB`, `rs/gs/bs` → `sumR/sumG/sumB`, `mx/mn` → `maxC/minC`, `hk` → `hue01`, `fad` → `attributes`, `dw` → `wallpaperApi`, `mer` → `meridiem`, `pct` → `percentText`, `rr` → `ringRect`, `th` → `ringWidth`, `hx/hy/hr` → `headX/headY/headR`, `a` (DigitRoll&) → `roll`, `e` → `eased`.

## Execution Board

| Phase | Status | Goal | Scope | Evidence / Result |
| --- | --- | --- | --- | --- |
| Phase 1 — Harness dan golden | IN_PROGRESS | Matriks mencakup skala 50/200 dan reuse cache setelah roll | scratchpad harness | Pending |
| Phase 2 — Rapikan tanpa ubah piksel | TODO | Rename, dedupe, komentar | `CLOCK.cpp` | Pending |
| Phase 3 — Cache layer konten | TODO | Per frame hanya titik dua + bar | `CLOCK.cpp::RenderFrame` dan state baru | Pending |
| Phase 4 — Verifikasi akhir | TODO | R1–R5 | build, matriks, bench, grep | Pending |

# Detailed Execution

## Phase 1 — Harness dan golden

#### T1 — Lengkapi skenario harness

Status: TODO
Readiness: READY
Requirements: R2
Depends on: none
Owns: scratchpad `harness/harness.cpp` (mode render), `harness/matrix.sh`

Change Specification:
- `harness.cpp::wmain`, mode render: setelah snapshot `roll_done_1400` (tick `t0 + 4000`), tambahkan satu frame di detik yang sama. Frame ini menguji reuse cache setelah animasi selesai:

```cpp
Step(Hms(14, 0, 0, 933), t0 + 4033);
Snap(dir, L"roll_after_reuse");
```

- `matrix.sh`: tambahkan baris variasi `scale50|ScalePercent=50` dan `scale200|ScalePercent=200`.

Actions:
- [ ] Terapkan kedua perubahan, rebuild `h_base.exe` dari `CLOCK_baseline.cpp`.
- [ ] `rm -rf m_base m_base2`, jalankan `matrix.sh` dua kali, lalu `cmp.sh h_base.exe m_base m_base2`. Harapan: `compared=192 mismatched=0` (16 × 12).

Result / Evidence: Not run.

## Phase 2 — Rapikan tanpa ubah piksel

#### T2 — Rename, dedupe, komentar

Status: TODO
Readiness: READY
Requirements: R1, R2, R4, R5
Depends on: T1
Owns: `CLOCK.cpp` (semua fungsi di bawah header settings)

Change Specification:
- Terapkan Rename map di seluruh file. Pertahankan `g_threadId`, `g_clockWindow`, `g_settings`, `g_layout`, `g_palette`, `g_hInstance`, `g_gdiplusToken`, `g_thread`, `g_userName`, `g_frameBits`, `g_paused`, dan flag `*Dirty`.
- Hapus `#include <atomic>`.
- Tambahkan helper berikut di bagian helper (dekat `FromColorRef`), lalu pakai di `AnalyzeWallpaperRegion`, saturasi di `BuildGlass`, dan `WindowsAccent`. Pindahkan `PackRgb` ke dekatnya. Saturasi menulis `0xFF000000u | PackRgb(...)`. Hasil float dan pembulatannya tetap identik.

```cpp
static Rgb UnpackRgb(uint32_t argb) {
    return {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f};
}
```

- `BuildCanvas`: resize `g_wallpaper.pixels` ke `canvasW*canvasH` lebih dulu, buat `Bitmap canvas(canvasW, canvasH, canvasW * 4, PixelFormat32bppARGB, reinterpret_cast<BYTE*>(g_wallpaper.pixels.data()))`, gambar seperti sekarang, lalu hapus blok `LockBits`/`memcpy`/`UnlockBits`. Loop `|= 0xFF000000u` tetap ada.
- `BuildGlass`: ganti `Bitmap small(cw, ch, PixelFormat32bppARGB)` + blok `LockBits` dengan `Bitmap cropBitmap(cropW, cropH, cropW * 4, PixelFormat32bppARGB, reinterpret_cast<BYTE*>(crop.data()))`. `crop` tetap hidup sampai `DrawImage` selesai.
- `Layout`: satukan `greetFont` dan `dateFont` menjadi `labelFont` (`19.0f * scale`, `labelStyle`, `UnitPixel`). Semua pemakai (sapaan, tanggal, AM/PM) memakai `labelFont`.
- Komentar: banner 3 baris `// ----` diganti satu baris label biasa. Nomor di `// 1. Frosted wallpaper.` dst. dibuang. Komentar yang menjelaskan "kenapa" tetap ada, misalnya slideshow/TranscodedWallpaper, debounce registry, dan odometer window.
- Fallback aksen di `BuildGlass` tidak disentuh (KEEP).

Actions:
- [ ] Terapkan perubahan dengan Edit per fungsi.
- [ ] `sh build.sh CLOCK.cpp out.dll`: exit 0, tanpa output warning.
- [ ] `build_harness.sh CLOCK.cpp h_new.exe`, `matrix.sh h_new.exe m_new`, `cmp.sh h_base.exe m_base m_new`: `mismatched=0`.
- [ ] Grep R4 (lihat Phase 4) tidak menemukan apa pun.
- [ ] Jika ada mismatch: `git diff` per fungsi, cari fungsi yang mengubah piksel, lalu kembalikan bagian itu.

Result / Evidence: Not run.

## Phase 3 — Cache layer konten

#### T3 — Layer konten di-reuse dalam detik yang sama

Status: TODO
Readiness: READY
Requirements: R2, R3
Depends on: T2
Owns: `CLOCK.cpp` (globals frame buffer, `BuildStaticLayer`, `RenderFrame`, `ReleaseGraphicsResources`)

Change Specification:
- State baru setelah `g_staticLayer`:

```cpp
// Static layer plus digits, AM/PM, greeting and date; redrawn only when its second changes or a digit rolls.
static std::vector<uint32_t> g_contentLayer;
static SYSTEMTIME g_contentTime{};  // milliseconds zeroed
static bool g_contentDirty = true;
```

- `BuildStaticLayer`: di akhir fungsi set `g_contentDirty = true;`. Semua perubahan layout, settings, palet, dan glass melewati fungsi ini. Ini juga berlaku untuk jalur `return` lebih awal saat `showPanel` mati.
- `RenderFrame` (bentuk akhir, bagian yang menentukan):

```cpp
SYSTEMTIME st{};
GetLocalTime(&st);
int hour = ...;  // as before
const int digits[4] = {...};
UpdateDigits(digits, now);
bool rolling = false;
for (const DigitRoll& roll : g_digitRolls) rolling |= roll.previous >= 0;  // checked before DrawDigits clears it

SYSTEMTIME second = st;
second.wMilliseconds = 0;
if (g_contentDirty || rolling || memcmp(&second, &g_contentTime, sizeof(second)) != 0) {
    g_contentLayer = g_staticLayer;
    Bitmap content(layout.winW, layout.winH, layout.winW * 4, PixelFormat32bppPARGB,
                   reinterpret_cast<BYTE*>(g_contentLayer.data()));
    Graphics g(&content);
    SetDrawQuality(g);
    DrawDigits(g, now);
    if (!g_settings.use24Hour) { ...AM/PM as before... }
    if (g_settings.showGreeting) DrawGreeting(g, st);
    if (g_settings.showDate) DrawDateRow(g, st);
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
// fade + UpdateLayeredWindow as before
```

  `SetDrawQuality(Graphics&)` adalah helper 4 baris berisi setting yang dulu ada di `RenderFrame`, karena sekarang dipakai dua kali. `DrawGreeting`/`DrawDateRow` menerima `st` dengan milidetik. Tidak masalah, karena keduanya hanya membaca jam, menit, detik, dan tanggal.
- `ReleaseGraphicsResources`: `g_contentLayer.clear(); g_contentDirty = true;`.
- Guard di awal `RenderFrame` tetap memeriksa ukuran `g_staticLayer`.

Actions:
- [ ] Terapkan, lalu build `build.sh` (exit 0, 0 warning).
- [ ] Matriks + `cmp.sh` terhadap `m_base`: `mismatched=0` (192). Frame kritis: `steady_next_frame` (reuse), `steady_next_second` (rebuild), `roll_*`, `roll_after_reuse`, `fade_in`, `h12`, `nosec`, `scale50`, `scale200`.
- [ ] Bench: `h_new.exe bench x 1800` 3 kali dan `h_base.exe bench x 1800` 3 kali, bergantian, tanpa beban lain. Bandingkan median-of-median ms/frame, CPU thread/frame, dan p95. Harapan: median turun jauh di bawah 5,3 ms, dan p95 tidak naik di atas baseline.

Result / Evidence: Not run.

## Phase 4 — Verifikasi akhir

- R1: `sh build.sh <repo>/CLOCK.cpp final.dll`, exit 0, stderr kosong.
- R2: matriks penuh terhadap `m_base`, `mismatched=0`.
- R3: tabel bench before/after (3 run masing-masing).
- R4: `grep -nwE 'g_static|g_glass|g_canvas|g_wallKey|g_memDc|g_dib|g_oldBitmap|g_frameBmp|g_digitAnim|DigitAnim|WallCanvas|WallInfo|WallKey|ShadowK|AnalyzeRegion|BuildStatic|greetFont|dateFont|smallFont|textFamily|textStyle|avgLum|accent2|kTimerId|kReloadTimerId|g_fadeStart|g_lastFsCheck|g_forceWallCheck|g_lastWallCheck' CLOCK.cpp` tidak menemukan apa pun. Pengecualian: string registry `L"Accent2"` tetap ada.
- R5: `git diff -U0 CLOCK.cpp` tidak menyentuh baris 1–85.
- Tidak terverifikasi oleh agen: tampilan dan CPU di explorer asli setelah mod di-compile ulang di Windhawk. Ini langkah Andino (salin `CLOCK.cpp` → Windhawk → Compile Mod → bandingkan dengan screenshot).

## Known Limitations

- Bench mengukur thread render di harness (SendMessage sinkron). Biaya komposisi DWM tidak diukur.
- Layer konten menambah satu buffer seukuran jendela (~1 MB di skala 1,0; ~4 MB di skala 2,0).

## Result / Evidence (2026-11-07)

- T1 DONE: dua run baseline `compared=192 mismatched=0`.
- T2 DONE: `build.sh` exit 0, 0 warning. Matriks `mismatched=0`.
- T3 DONE: cache layer konten. `build.sh` exit 0, 0 warning. Matriks final `compared=192 mismatched=0`.
- Bench (bergantian, 1800 frame per run, beban sistem ~22% CPU): baseline median 11,5–12,0 ms dan CPU thread 9,6–10,2 ms/frame. Baru median 0,74–0,76 ms dan CPU thread 0,92–0,98 ms/frame. p95 15,3 → 1,3–1,7 ms. Di sesi lebih awal (beban lebih rendah) baseline terukur 5,3 ms.
- R4: hanya tersisa string registry `L"Accent2"` dan variabel lokal `cellX` (posisi sel yang sedang digambar), keduanya disengaja. R5: baris 1–85 identik dengan HEAD. File tetap CRLF.
- Review: self-review saja, tanpa reviewer independen.
- NOT_VERIFIED: tampilan dan CPU di explorer asli setelah compile ulang di Windhawk.

## NEXT ACTION

Andino: salin `CLOCK.cpp` ke Windhawk, klik Compile Mod, lalu bandingkan dengan screenshot acuan. Commit belum dibuat.
