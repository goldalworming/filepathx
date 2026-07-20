# FilePathX themes

## Cara ganti tema

1. Pilih file `.ini` di folder ini.
2. Copy jadi `theme.ini` di `%APPDATA%\filepathx\`:
   ```
   copy themes\darkmatter.ini "%APPDATA%\filepathx\theme.ini"
   ```
3. Restart FilePathX, atau kalau sudah running tekan **`Ctrl+Shift+T`** untuk reload tema tanpa restart.

Hapus `theme.ini` kembali untuk kembali ke default (Catppuccin Mocha).

## Format

Sederhana `key = value` (INI). Nilai warna dalam hex RGB atau RRGGBB atau AARRGGBB
(dengan/atau tanpa `#`). Key yang tidak dikenal diabaikan — jadi kalau kamu
export dari [tweakcn.com](https://tweakcn.com/), tinggal paste variabel-nya.

Key yang dipakai:

| Key                                | Fungsi                                          |
|------------------------------------|-------------------------------------------------|
| `background` / `bg`                | Warna dasar file list / content area            |
| `foreground` / `text`              | Warna teks utama                                |
| `muted-foreground` / `subtext`     | Teks lebih redup (path, ukuran, dsb)            |
| `dim`                              | Teks paling redup                               |
| `primary` / `accent`               | Warna aksen (tab aktif, tombol utama, checkbox) |
| `card` / `surface`                 | Panel / kartu                                   |
| `popover` / `header`               | Header panel + popover                          |
| `sidebar` / `mantle`               | Sidebar background                              |
| `muted` / `hover`                  | Warna hover pada tombol / list item             |
| `selected` / `selection`           | Row / item yang sedang dipilih                  |
| `border` / `input`                 | Garis pembatas + input border                   |
| `ring` / `overlay`                 | Fokus ring / overlay                            |
| `scrollbar`                        | Scrollbar thumb                                 |
| `destructive` / `red`              | Warna hapus / peringatan / merah                |
| `chart-1` / `yellow`               | Aksen kuning (folder icon)                      |
| `chart-2` / `green`                | Aksen hijau                                     |
| `chart-3` / `peach`                | Aksen peach                                     |

## Preset yang tersedia

- `catppuccin-mocha.ini` — Dark biru soft (default kalau tidak ada `theme.ini`)
- `sage-garden.ini` — Light earthy hijau
- `amber-minimal.ini` — Light minimalis dengan aksen orange
- `elegant-luxury.ini` — Dark warm dengan aksen merah crimson
- `darkmatter.ini` — Dark neutral dengan aksen amber

## Import dari tweakcn.com

1. Di tweakcn, klik **Code** di kanan atas
2. Copy `:root { --primary: ...; --background: ...; }` block
3. Convert manual ke format INI (hapus `--` dan `;`, ganti `:` jadi `=`)
   atau save langsung sebagai `theme.ini` — parser akan skip syntax CSS yang
   tidak dikenal.

Contoh dari tweakcn "Sage Garden" `--primary: oklch(0.5765 0.0611 149.63)` →
convert ke hex → `primary = 7c9082`.
