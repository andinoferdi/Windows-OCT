# Windows OCT

Setup Windows 11 saya: taskbar melayang seperti dock dan jam kaca (frosted glass) di tengah desktop, dengan wallpaper nebula. Kedua mod dipasang lewat [Windhawk](https://windhawk.net/).

![Tampilan desktop Windows OCT](Screenshot.png)

## Isi repo

| File | Isi |
| --- | --- |
| `TASKBAR MOD.cpp` | Mod Windhawk TAI (Taskbar as Island) v1.5.259 yang di-patch untuk Windows 11 26H2. Taskbar melayang di tengah dengan sudut membulat, dan tray menempel di samping ikon aplikasi. Saya tambahkan efek aksen yang senada dengan jam dan animasi dock. |
| `CLOCK.cpp` | Mod Windhawk Desktop Glass Clock v1.1.0. Jam kaca di desktop berisi sapaan, tanggal, bar detik, dan progress hari. Warna aksennya diambil dari wallpaper dan dibagikan ke taskbar. |
| `space-art-nebula-8k-jy.jpg` | Wallpaper nebula 8K. |
| `Screenshot.png` | Hasil akhir. |

## Syarat

Windows 11 x64. Kedua mod hanya dibuat untuk x86-64, jadi tidak jalan di Windows ARM.

## Langkah 1: Download repo

```bash
git clone https://github.com/andinoferdi/Windows-OCT.git
```

Tanpa Git: di halaman GitHub klik **Code → Download ZIP**, lalu extract.

## Langkah 2: Atur Windows

Blur di taskbar dan jam butuh mode Dark dan Transparency effects.

1. Buka **Settings → Personalization → Colors**.
2. Di **Choose your mode**, pilih **Dark**.
3. Nyalakan **Transparency effects**.

Kalau **Energy saver** aktif, Windows mematikan efek transparan. Taskbar tetap jalan, tapi tampil dengan warna solid tanpa blur.

## Langkah 3: Pasang wallpaper

Pasang wallpaper sebelum mod jam, karena warna aksen jam diambil dari wallpaper.

Klik kanan `space-art-nebula-8k-jy.jpg` → **Set as desktop background**. Bisa juga lewat **Settings → Personalization → Background** → **Picture** → **Browse photos**, dengan **Choose a fit** diatur ke **Fill**.

## Langkah 4: Install Windhawk

Download installer dari [windhawk.net](https://windhawk.net/) dan jalankan, atau install lewat terminal:

```bash
winget install --id RamenSoftware.Windhawk
```

Lalu buka Windhawk.

## Langkah 5: Pasang mod taskbar (TAI)

Matikan dulu mod Windhawk lain yang mengubah tinggi atau ikon taskbar. Fitur itu sudah ada di TAI, dan dua mod yang mengatur hal yang sama bisa bentrok.

1. Copy seluruh isi `TASKBAR MOD.cpp`: buka di Notepad atau VS Code, tekan `Ctrl+A`, lalu `Ctrl+C`. Bisa juga dari PowerShell di folder repo:

   ```powershell
   Get-Content -Raw "TASKBAR MOD.cpp" | Set-Clipboard
   ```

2. Di Windhawk, buka **Explore** → **Create a new mod**.
3. Di editor, tekan `Ctrl+A` lalu `Ctrl+V` untuk mengganti template bawaan.
4. Klik **Compile Mod** di kiri atas dan tunggu sampai selesai.
5. Klik **Exit editing mode**. Taskbar berubah jadi dock melayang.

## Langkah 6: Pasang mod jam (Desktop Glass Clock)

Ulangi langkah 1 sampai 5 di atas dengan `CLOCK.cpp`:

```powershell
Get-Content -Raw "CLOCK.cpp" | Set-Clipboard
```

Setelah **Exit editing mode**, jam kaca muncul di tengah desktop. Kedua mod sekarang ada di tab **Home** Windhawk sebagai `local@taskbar-dock-like` dan `local@desktop-flip-clock`.

## Langkah 7: Atur mod (opsional)

Screenshot memakai pengaturan bawaan kedua mod, jadi langkah ini boleh dilewati. Untuk mengubah pengaturan, klik **Details** pada mod di tab **Home**, lalu buka tab **Settings**. Tabel di bawah hanya memuat sebagian pengaturan.

### Desktop Glass Clock

| Pengaturan | Bawaan | Keterangan |
| --- | --- | --- |
| Greeting name | *(kosong)* | Kosong = sapaan memakai nama akun Windows. Isi `-` untuk sapaan tanpa nama. |
| Center on screen | On | Off = posisi jam diatur lewat **Top offset**. |
| Clock scale (%) | 100 | Ukuran jam, 50 sampai 200. |
| Bahasa Indonesia | On | Off = sapaan dan tanggal dalam bahasa Inggris. |
| Accent color | Auto dari wallpaper | Bisa diganti ke warna aksen Windows atau warna custom. |
| Glass tint | 38 | Makin tinggi, panel makin gelap. |
| Blur strength | 60 | Kekuatan blur panel. |

### TAI: bentuk taskbar

| Pengaturan | Bawaan | Keterangan |
| --- | --- | --- |
| Taskbar height | 56 | Tinggi taskbar. |
| Taskbar icon size | 30 | Ukuran ikon. Harus lebih kecil dari button size. |
| Taskbar vertical offset | 8 | Jarak taskbar dari bawah layar. |
| Taskbar corner radius | 22 | Makin besar, sudut makin bulat. |
| Background blur amount | 30 | Kekuatan blur di belakang taskbar. |
| Full-width taskbar background | Off | On = latar taskbar selebar layar seperti bawaan Windows. |

### TAI: efek aksen

Taskbar membaca warna aksen dari mod jam lewat registry `HKCU\Software\Windows-OCT`. Kalau mod jam tidak dipasang, taskbar memakai warna aksen Windows. Setelah update repo ini, compile ulang kedua mod.

| Pengaturan | Bawaan | Keterangan |
| --- | --- | --- |
| Accent effects (twin of Desktop Glass Clock) | On | Saklar utama semua efek di tabel ini. Off = tampilan TAI biasa. |
| Accent source | Desktop Glass Clock | Sumber warna aksen. Pilihan lain: *Windows accent color* dan *Custom color*. |
| Custom accent color | `#F05CD0` | Dipakai kalau Accent source = Custom. |
| Accent glass tint (%) | 38 | Tint gelap kaca yang senada wallpaper, sama seperti panel jam. 0 = pakai pengaturan tint biasa. |
| Accent glow (%) | 30 | Glow aksen yang naik dari bawah dock. 0 = mati. |
| Glass sheen | On | Kilau putih tipis di setengah atas dock. |
| Edge light | On | Garis tepi putih di atas yang memudar ke warna aksen di bawah. |
| Accent running indicators | On | Garis di bawah ikon aplikasi berwarna aksen. Aplikasi yang berkedip minta perhatian tetap memakai warna Windows. |
| Accent hover glow | On | Tombol berpendar aksen saat di-hover atau diklik. Tidak tampil selama Dock animation aktif. |

### TAI: animasi dock

Hover ala macOS: tombol di dekat kursor membesar dan tetangganya bergeser memberi ruang. Animasi mati sendiri kalau *Animation effects* Windows dimatikan.

| Pengaturan | Bawaan | Keterangan |
| --- | --- | --- |
| Dock animation | On | Saklar utama animasi dock. |
| Dock animation size (%) | 130 | Ukuran tombol tepat di bawah kursor. |
| Dock animation reach (px) | 100 | Jarak dari kursor yang tombolnya masih ikut membesar. |
| Dock animation curve | Cosine | Bentuk penurunan ukuran: *Cosine* (halus), *Linear*, atau *Exponential* (puncak tajam). |
| Dock animation spacing (%) | 50 | Seberapa jauh tetangga bergeser. 0 = tidak bergeser. |
| Dock animation follow speed | 60 | Kecepatan tombol mengikuti kursor. 0 = langsung. |
| Dock animation fade (ms) | 150 | Durasi efek muncul saat kursor masuk dan hilang saat keluar. |
| Dock animation bounce | On | Tombol di bawah kursor yang diam memantul pelan. |
| Dock animation bounce delay (ms) | 500 | Lama kursor diam sebelum tombol mulai memantul. |
| Dock animation bounce lift | On | Pantulan mengangkat tombol. Off = hanya membesar-mengecil. |
| Dock animation labels mode | Off | Nyalakan kalau label taskbar ditampilkan. |
| Dock animation system buttons | All buttons | Menentukan apakah Start, Search, Task View, dan Widgets ikut beranimasi. |

## Troubleshooting

| Masalah | Solusi |
| --- | --- |
| Compile gagal | Pastikan seluruh isi file ter-copy, dari baris `// ==WindhawkMod==` paling atas sampai baris terakhir. Ulangi `Ctrl+A` → `Ctrl+V` di editor. |
| Taskbar tidak berubah atau tampil aneh | Matikan mod taskbar lain di Windhawk. Kalau belum berubah, restart Explorer: buka Task Manager → **Windows Explorer** → **Restart**. |
| Taskbar tidak blur, warnanya solid | Nyalakan **Transparency effects** dan matikan **Energy saver**. |
| Jam tidak kelihatan | Jam disembunyikan otomatis saat ada game atau aplikasi fullscreen, jadi tutup dulu aplikasinya. Pastikan juga mod jam **Enabled** di tab Home. |
| Warna jam tidak cocok setelah ganti wallpaper | Kalau warnanya tidak ikut berubah, matikan lalu nyalakan lagi mod jam. |
| Warna taskbar beda dari jam | Pastikan mod jam **Enabled** dan sudah di-compile ulang (v1.1.0), lalu cek **Accent source** di TAI = *Desktop Glass Clock*. |
| Ikon taskbar beranimasi dobel atau melompat | TAI sudah punya animasi dock. Matikan mod **Taskbar Dock Animation** atau mod dock lain di Windhawk. |
| Efek aksen bikin taskbar terasa aneh | Matikan **Accent effects** di TAI untuk kembali ke tampilan TAI biasa, atau matikan efeknya satu per satu. |
| Mod error setelah update Windows | Klik **Details** → **Edit**, lalu **Compile Mod** ulang. Kalau tetap error, cek versi terbaru TAI di repo aslinya (lihat Kredit). |

## Kembali ke tampilan bawaan

Matikan atau hapus kedua mod di tab **Home** Windhawk. Taskbar dan desktop langsung kembali normal. Untuk membersihkan semuanya, uninstall Windhawk dari **Settings → Apps → Installed apps**.

## Kredit

- TAI (Taskbar as Island) dibuat oleh [DarkionAvey](https://github.com/DarkionAvey/windhawk-taskbar-centered-condensed), dengan lisensi GNU GPL v3.0. Versi di repo ini ditambah patch kompatibilitas untuk Windows 11 26H2.
- Desktop Glass Clock dibuat oleh Levelup iD.
- [Windhawk](https://windhawk.net/) dibuat oleh Michael Maltsev ([m417z](https://github.com/m417z)).
