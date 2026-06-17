# Cara Upload Release ke GitHub

Catatan langkah-langkah untuk membuat / mengupdate release di
`https://github.com/goldalworming/filepathx`.

## Prasyarat

- `gcc` + `windres` dari [w64devkit](https://github.com/skeeto/w64devkit)
  diekstrak ke `w64devkit/` di root repo.
- Kredensial GitHub sudah tersimpan di Git Credential Manager (sekali pakai
  `git push` interaktif, token-nya disimpan otomatis).
- `powershell.exe` (untuk `Compress-Archive`) dan `curl` (bundle di Git for
  Windows).

## Langkah-langkah

### 1. Build binary stripped

```bash
export PATH="/c/data/code/filepathx/w64devkit/bin:$PATH"

# Compile resource (icon + manifest)
windres -I src src/resource.rc -O coff -o build/resource.o

# Compile dengan -O2 -s (strip symbols) → exe ~360 KB
gcc -O2 -s -Wall -o build/release/FilePathX.exe \
    src/main.c src/render.c src/ui.c -Isrc build/resource.o \
    -lopengl32 -lgdi32 -luser32 -lshell32 -lshlwapi \
    -ldwmapi -lole32 -luuid -luxtheme -mwindows
```

### 2. Package zip

```bash
cp README.md LICENSE build/release/

powershell.exe -NoProfile -Command \
  "Compress-Archive -Path build\\release\\* -DestinationPath build\\FilePathX-win64.zip -Force"
```

Hasil: `build/FilePathX-win64.zip` (~265 KB).

### 3. Pastikan main sudah commit + push

```bash
git status                 # working tree clean
git push                   # kalau ada commit yang belum di-push
```

### 4. Tag versi (skip kalau release-nya sudah ada)

Cukup sekali per versi:

```bash
git tag -a v0.1.0 -m "Initial release"
git push origin v0.1.0
```

### 5. Buat release object via API (skip kalau release-nya sudah ada)

```bash
# Ambil token dari credential helper
CRED=$(echo -e "protocol=https\nhost=github.com\n" | git credential fill 2>/dev/null)
TOKEN=$(echo "$CRED" | grep '^password=' | head -1 | cut -d= -f2-)

curl -sS -X POST \
  -H "Authorization: token $TOKEN" \
  -H "Accept: application/vnd.github+json" \
  https://api.github.com/repos/goldalworming/filepathx/releases \
  -d '{
    "tag_name": "v0.1.0",
    "name": "FilePathX v0.1.0",
    "body": "Pre-built Windows 64-bit binary."
  }' | grep -E '"(id|html_url)"'
```

Catat `id` (release id) yang keluar — dipakai untuk upload asset.

### 6. Upload (atau replace) asset

Release id saat ini: **`323668658`**.

```bash
# Ambil token
CRED=$(echo -e "protocol=https\nhost=github.com\n" | git credential fill 2>/dev/null)
TOKEN=$(echo "$CRED" | grep '^password=' | head -1 | cut -d= -f2-)
RELEASE_ID=323668658

# (opsional) hapus asset lama dengan nama yang sama
ASSET_ID=$(curl -sS -H "Authorization: token $TOKEN" \
  https://api.github.com/repos/goldalworming/filepathx/releases/$RELEASE_ID/assets \
  | grep -oE '"id": [0-9]+' | head -1 | grep -oE '[0-9]+')
curl -sS -X DELETE -H "Authorization: token $TOKEN" \
  "https://api.github.com/repos/goldalworming/filepathx/releases/assets/$ASSET_ID"

# Upload asset baru
curl -sS -X POST \
  -H "Authorization: token $TOKEN" \
  -H "Content-Type: application/zip" \
  --data-binary "@build/FilePathX-win64.zip" \
  "https://uploads.github.com/repos/goldalworming/filepathx/releases/$RELEASE_ID/assets?name=FilePathX-win64.zip" \
  | grep -oE '"browser_download_url":"[^"]+"'
```

URL download akhir:
`https://github.com/goldalworming/filepathx/releases/download/v0.1.0/FilePathX-win64.zip`

## Catatan

- `taskkill //F //IM FilePathX.exe` dulu kalau exe sedang running, supaya
  file lock-nya lepas dan `gcc` bisa overwrite.
- Endpoint upload pakai host `uploads.github.com`, beda dari host API biasa.
- Token dari credential helper sudah cukup punya `repo` scope kalau dulu
  pernah dipakai untuk `git push`.
- Kalau mau bikin versi baru (`v0.2.0` dst.), tinggal ulangi langkah
  4–6 dengan tag/nama baru.
