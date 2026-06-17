# Thumbnail Performance — Step #3 & #4 Plan

Kelanjutan dari optimisasi sebelumnya. Step #1 (early visibility cull,
sudah ada) + #2 (gate request saat scroll cepat) + queue 64→256 sudah
diimplementasi. Dokumen ini detail untuk #3 (multi-worker) dan #4 (cache
lebih besar).

Semua file yang disebut relatif ke `src/main.c` kecuali disebutkan lain.

---

## Step #3 — Multi-worker decode

### Tujuan

Worker tunggal sekarang serial: 1 file decode ~50-150ms via
`IShellItemImageFactory::GetImage`. Untuk folder 200 gambar yang baru
masuk view, ini >10 detik wall-clock. 3 worker → throughput ~3× karena
decode mostly I/O + libjpeg/libwebp bound, bukan CPU-bound.

### Perubahan kode

**a. Globals jadi array** (main.c:~573)

Sekarang:
```c
static HANDLE g_thumb_event = NULL;
static HANDLE g_thumb_thread = NULL;
```

Jadi:
```c
#define THUMB_WORKERS 3
static HANDLE g_thumb_event = NULL;          /* shared semaphore-like, lihat (b) */
static HANDLE g_thumb_threads[THUMB_WORKERS];
```

**b. Ganti event jadi semaphore**

`g_thumb_event` (`CreateEventW`, auto-reset) cuma wake 1 worker per
`SetEvent`. Dengan banyak worker, idle worker bisa missed wake-up dan
queue numpuk. Pakai semaphore:

```c
g_thumb_event = CreateSemaphoreW(NULL, 0, THUMB_Q_CAP, NULL);
```

Di `thumb_request()`:
```c
SetEvent(g_thumb_event);   /* lama */
ReleaseSemaphore(g_thumb_event, 1, NULL);   /* baru */
```

Di `thumb_worker()`:
```c
WaitForSingleObject(g_thumb_event, INFINITE);   /* tetap, tapi sekarang dari semaphore */
```

Setiap `ReleaseSemaphore(+1)` = wake satu worker. Worker pop satu request,
loop balik wait. Tidak ada race condition antar worker karena
`EnterCriticalSection(&g_thumb_cs)` proteksi queue pop.

**c. Spawn workers di `thumb_init()`** (main.c:~704)

Sekarang:
```c
g_thumb_thread = CreateThread(NULL, 0, thumb_worker, NULL, 0, NULL);
```

Jadi:
```c
for (int i = 0; i < THUMB_WORKERS; i++)
    g_thumb_threads[i] = CreateThread(NULL, 0, thumb_worker, NULL, 0, NULL);
```

**d. Shutdown** (kalau ada `thumb_shutdown()`, kalau belum tambahkan)

```c
InterlockedExchange(&g_thumb_quit, 1);
ReleaseSemaphore(g_thumb_event, THUMB_WORKERS, NULL);   /* wake semua biar exit */
WaitForMultipleObjects(THUMB_WORKERS, g_thumb_threads, TRUE, 2000);
for (int i = 0; i < THUMB_WORKERS; i++) CloseHandle(g_thumb_threads[i]);
CloseHandle(g_thumb_event);
DeleteCriticalSection(&g_thumb_cs);
```

### Risiko & mitigasi

- **COM apartment** — `CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)`
  sudah ada di worker. STA artinya setiap worker punya apartment sendiri,
  aman untuk parallel `IShellItemImageFactory`.
- **`g_thumb_done` queue size** — sekarang `THUMB_Q_CAP` (256). Worker
  push hasil ke sini di bawah `g_thumb_cs`. Dengan 3 producer, contention
  rendah karena critical section pendek. Tidak perlu ubah.
- **`PostMessageW(g_hwnd, WM_APP+3, 0, 0)`** — 3 worker bisa post >1 msg
  per batch, fine (handler drain semua done_count dalam satu turn).
- **Order tidak deterministik** — visible items selesai sesuai decode
  speed, bukan urutan request. UX tidak terpengaruh (placeholder muncul
  sementara).

### Tunable

- `THUMB_WORKERS` — coba 3. Naik ke 4 kalau folder gambar besar masih
  lambat; turun ke 2 kalau CPU spike terlalu tinggi di laptop low-end.

---

## Step #4 — Cache lebih besar

### Tujuan

`THUMB_CACHE_CAP=200` sekarang. Folder dengan 500 gambar berarti scroll
ke bawah → evict thumbnail atas → scroll naik → decode ulang. Naikkan
ke 800-1500 supaya seluruh folder tipikal muat sekali.

### Hitung memory

- `THUMB_PX=192`, RGBA = 192×192×4 = 147 KB per texture (GL-side).
- 800 × 147 KB ≈ 118 MB.
- 1500 × 147 KB ≈ 220 MB.

CPU RAM nyaris nol karena `tmp` di `hbmp_to_gl` freed setelah
`glTexImage2D`. Yang besar adalah GPU memory.

### Perubahan

**a. Array `g_thumb_cache`** (main.c:~564) sekarang statik di .data:

```c
static ThumbEntry g_thumb_cache[THUMB_CACHE_CAP];
```

Naikin dari 200 → 800 berarti `.data` += (`sizeof(ThumbEntry)` × 600).
`ThumbEntry` = `char[MAX_PATH]` + `GLuint` + 2 `int` + `DWORD` ≈ 280B.
→ extra 170 KB di .data. OK.

Ubah:
```c
#define THUMB_CACHE_CAP 800
```

**b. Linear scan di `thumb_cache_find()`**

Sekarang O(n) `_stricmp`. n=800 + dipanggil per visible item per frame =
puluhan ribu strcmp per frame. Bisa jadi bottleneck baru.

Mitigasi: tambah hash bucket cepat. Simpel:
```c
static int g_thumb_bucket[1024];   /* idx ke g_thumb_cache atau -1 */
static int g_thumb_next[THUMB_CACHE_CAP];  /* chain */
```

Hash: `djb2(path) & 1023`. Find = ikuti chain. Insert = prepend.
Evict = scan chain untuk path lama, unlink.

Atau sederhana dulu: biarkan O(n) sampai kelihatan bermasalah di
profiler. 800 strcmp × 50 visible items = 40k strcmp/frame, biasanya
~1ms — masih OK di 60fps budget 16ms.

### Risiko

- **GPU memory pressure** di laptop dengan iGPU shared memory — kalau
  user laporan lag setelah browse banyak folder, turunkan cap.
- **LRU eviction** sudah ada (`last_used`), tinggal kerja dengan cap
  lebih besar.

### Tunable

- `THUMB_CACHE_CAP` — start 800. Bump ke 1500 kalau RAM lega dan user
  sering mondar-mandir folder gambar besar.

---

## Urutan implementasi yang aman

1. Implementasi #4 dulu (cache 800) → 1 line, langsung test.
2. Kalau scroll-back terasa lebih responsif tapi initial-load masih
   lambat → lanjut #3 (multi-worker).
3. #3 lebih kompleks karena ubah event→semaphore. Test sebentar di
   folder 200+ image (Pictures, Downloads), pastikan tidak ada
   thumbnail yang miss / leak texture.

## Verifikasi

- Folder uji: `%USERPROFILE%\Pictures` atau folder dengan >300 image.
- Sebelum: catat berapa detik sampai semua visible thumbnail muncul.
- Sesudah #3: harusnya ~3× lebih cepat untuk view pertama.
- Sesudah #4: scroll naik-turun antar tab harus instant (no decode).

## Kalau masih kurang

- **Decode resolusi lebih kecil di small icons** — sekarang
  `THUMB_PX=192` dipakai untuk small (48px) dan large (130px). Small
  icon over-decoded. Bisa dua sized cache atau resize on demand.
- **Generate placeholder mipmap** — generic file icon per ekstensi, di-
  cache dengan resolusi 16/32/64/128, supaya fallback selama decode
  juga konteks-aware (PDF placeholder beda dari video).
- **Prefetch off-screen tetangga viewport** — request 1 row di atas + 1
  row di bawah viewport supaya scroll halus tidak ada gap.
