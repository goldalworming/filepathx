/* Auto-update via GitHub Releases.
 *
 * Check:   GET https://api.github.com/repos/goldalworming/filepathx/releases/latest
 *          on a worker thread, parse "tag_name", compare against APP_VERSION.
 *          Throttled to once per 24 h via a small state file in %APPDATA%.
 * Install: download FilePathX-win64.zip from the release into %TEMP%, then
 *          launch a PowerShell script that waits for our exe's file lock to
 *          drop, extracts the zip, copies FilePathX.exe over the running
 *          binary and relaunches it. The app quits right after launching
 *          the script (main.c handles UPDATE_RESTART_PENDING).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "update.h"
#include "version.h"

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

#define WIDEN2(x) L##x
#define WIDEN(x)  WIDEN2(x)

#define UPDATE_REPO_OWNER  L"goldalworming"
#define UPDATE_REPO_NAME   L"filepathx"
#define UPDATE_ASSET_NAME  "FilePathX-win64.zip"
#define UPDATE_CHECK_EVERY (24 * 60 * 60)   /* seconds */

static HWND  g_notify_hwnd = NULL;
static UINT  g_notify_msg  = 0;
static char  g_state_file[MAX_PATH] = "";

static CRITICAL_SECTION g_upd_cs;
static int  g_upd_cs_ready = 0;
static int  g_upd_status   = UPDATE_IDLE;
static char g_upd_latest[64] = "";       /* e.g. "v0.8.0" */
static char g_upd_error [160] = "";
static volatile LONG g_upd_busy = 0;     /* a worker thread is running */

/* ---- Status plumbing (worker threads write, UI thread reads) ---- */

static void upd_set(int status, const char* latest, const char* err) {
    EnterCriticalSection(&g_upd_cs);
    g_upd_status = status;
    if (latest) {
        strncpy(g_upd_latest, latest, sizeof(g_upd_latest) - 1);
        g_upd_latest[sizeof(g_upd_latest) - 1] = 0;
    }
    if (err) {
        strncpy(g_upd_error, err, sizeof(g_upd_error) - 1);
        g_upd_error[sizeof(g_upd_error) - 1] = 0;
    }
    LeaveCriticalSection(&g_upd_cs);
    if (g_notify_hwnd) PostMessageW(g_notify_hwnd, g_notify_msg, 0, 0);
}

int update_status(void) {
    if (!g_upd_cs_ready) return UPDATE_IDLE;
    EnterCriticalSection(&g_upd_cs);
    int s = g_upd_status;
    LeaveCriticalSection(&g_upd_cs);
    return s;
}

void update_latest_version(char* out, int n) {
    if (n <= 0) return;
    out[0] = 0;
    if (!g_upd_cs_ready) return;
    EnterCriticalSection(&g_upd_cs);
    strncpy(out, g_upd_latest, n - 1);
    out[n - 1] = 0;
    LeaveCriticalSection(&g_upd_cs);
}

const char* update_error(void) {
    /* Read without the lock: the buffer is only appended-to-then-status-set,
       and the UI only shows it after status says UPDATE_ERROR. */
    return g_upd_error;
}

/* ---- Version compare ---- */

static void parse_ver(const char* s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    if (*s == 'v' || *s == 'V') s++;
    sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

/* 1 if tag (e.g. "v0.8.0") is newer than the running APP_VERSION */
static int ver_is_newer(const char* tag) {
    int a[3], b[3];
    parse_ver(tag, a);
    parse_ver(APP_VERSION, b);
    if (a[0] != b[0]) return a[0] > b[0];
    if (a[1] != b[1]) return a[1] > b[1];
    return a[2] > b[2];
}

/* ---- Check-throttle state file ---- */

static void state_load(long long* last_check, char* latest, int latest_n) {
    *last_check = 0;
    if (latest_n > 0) latest[0] = 0;
    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, g_state_file, -1, wpath, MAX_PATH);
    FILE* f = _wfopen(wpath, L"rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!strncmp(line, "last_check = ", 13))
            *last_check = _atoi64(line + 13);
        else if (!strncmp(line, "latest = ", 9)) {
            strncpy(latest, line + 9, latest_n - 1);
            latest[latest_n - 1] = 0;
        }
    }
    fclose(f);
}

static void state_save(long long last_check, const char* latest) {
    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, g_state_file, -1, wpath, MAX_PATH);
    FILE* f = _wfopen(wpath, L"wb");
    if (!f) return;
    fprintf(f, "last_check = %lld\r\n", last_check);
    fprintf(f, "latest = %s\r\n", latest);
    fclose(f);
}

/* ---- HTTP (WinHTTP) ---- */

/* GET https://<host><path>. On success returns a malloc'd NUL-terminated
   body (*out_len excludes the NUL); returns NULL and fills err on failure.
   When dest_file is non-NULL the body is streamed there instead and the
   return value is (char*)1. */
static char* http_get(const WCHAR* host, const WCHAR* path,
                      FILE* dest_file, DWORD* out_len,
                      char* err, int err_n)
{
    char*     body = NULL;
    DWORD     body_len = 0, body_cap = 0;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    char*     result = NULL;

    err[0] = 0;
    ses = WinHttpOpen(L"FilePathX/" WIDEN(APP_VERSION),
                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { _snprintf(err, err_n, "WinHttpOpen failed"); goto done; }
    con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) { _snprintf(err, err_n, "connect failed"); goto done; }
    req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { _snprintf(err, err_n, "request failed"); goto done; }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        _snprintf(err, err_n, "no response (offline?)"); goto done;
    }
    DWORD code = 0, code_len = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &code_len,
                        WINHTTP_NO_HEADER_INDEX);
    if (code != 200) { _snprintf(err, err_n, "HTTP %lu", (unsigned long)code); goto done; }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            _snprintf(err, err_n, "read failed"); goto done;
        }
        if (avail == 0) break;
        if (avail > 65536) avail = 65536;
        char chunk[65536];
        DWORD got = 0;
        if (!WinHttpReadData(req, chunk, avail, &got)) {
            _snprintf(err, err_n, "read failed"); goto done;
        }
        if (got == 0) break;
        if (dest_file) {
            if (fwrite(chunk, 1, got, dest_file) != got) {
                _snprintf(err, err_n, "disk write failed"); goto done;
            }
        } else {
            if (body_len + got + 1 > body_cap) {
                body_cap = (body_cap ? body_cap * 2 : 65536);
                while (body_cap < body_len + got + 1) body_cap *= 2;
                if (body_cap > 4u * 1024 * 1024) {  /* sanity cap */
                    _snprintf(err, err_n, "response too large"); goto done;
                }
                char* nb = (char*)realloc(body, body_cap);
                if (!nb) { _snprintf(err, err_n, "out of memory"); goto done; }
                body = nb;
            }
            memcpy(body + body_len, chunk, got);
            body_len += got;
        }
    }
    if (dest_file) {
        result = (char*)1;
    } else {
        if (!body) body = (char*)malloc(1);
        if (!body) { _snprintf(err, err_n, "out of memory"); goto done; }
        body[body_len] = 0;
        if (out_len) *out_len = body_len;
        result = body;
        body = NULL;   /* ownership passed to caller */
    }
done:
    if (err_n > 0) err[err_n - 1] = 0;
    free(body);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return result;
}

/* ---- Check worker ---- */

/* Extract the value of a top-level "key":"value" string from a JSON blob.
   Good enough for the GitHub release object we query. */
static int json_str(const char* json, const char* key, char* out, int n) {
    char pat[64];
    _snprintf(pat, sizeof(pat), "\"%s\"", key);
    pat[sizeof(pat) - 1] = 0;
    const char* p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < n - 1) {
        if (*p == '\\' && p[1]) p++;   /* skip escapes, keep escaped char */
        out[i++] = *p++;
    }
    out[i] = 0;
    return 1;
}

static DWORD WINAPI check_worker(LPVOID param) {
    (void)param;
    char err[128];
    DWORD len = 0;
    char* body = http_get(L"api.github.com",
                          L"/repos/" UPDATE_REPO_OWNER L"/" UPDATE_REPO_NAME
                          L"/releases/latest",
                          NULL, &len, err, sizeof(err));
    if (!body) {
        upd_set(UPDATE_ERROR, NULL, err);
        InterlockedExchange(&g_upd_busy, 0);
        return 0;
    }
    char tag[64] = "";
    if (!json_str(body, "tag_name", tag, sizeof(tag)) || !tag[0]) {
        free(body);
        upd_set(UPDATE_ERROR, NULL, "bad API response");
        InterlockedExchange(&g_upd_busy, 0);
        return 0;
    }
    free(body);
    state_save((long long)time(NULL), tag);
    upd_set(ver_is_newer(tag) ? UPDATE_AVAILABLE : UPDATE_UP_TO_DATE, tag, NULL);
    InterlockedExchange(&g_upd_busy, 0);
    return 0;
}

/* ---- Install worker ---- */

/* Append src to a growing buffer, doubling any single quotes so the result
   can sit inside a PowerShell single-quoted literal. */
static void ps_quote_append(char* dst, int dst_n, const char* src) {
    int di = (int)strlen(dst);
    for (const char* s = src; *s && di < dst_n - 3; s++) {
        if (*s == '\'') { dst[di++] = '\''; dst[di++] = '\''; }
        else            dst[di++] = *s;
    }
    dst[di] = 0;
}

static DWORD WINAPI install_worker(LPVOID param) {
    (void)param;
    char tag[64];
    update_latest_version(tag, sizeof(tag));

    WCHAR wtmp[MAX_PATH];
    GetTempPathW(MAX_PATH, wtmp);
    WCHAR wzip[MAX_PATH], wps1[MAX_PATH], wdir[MAX_PATH], wexe[MAX_PATH];
    _snwprintf(wzip, MAX_PATH, L"%sFilePathX-update.zip", wtmp);
    _snwprintf(wps1, MAX_PATH, L"%sFilePathX-update.ps1", wtmp);
    _snwprintf(wdir, MAX_PATH, L"%sFilePathX-update",     wtmp);
    wzip[MAX_PATH-1] = wps1[MAX_PATH-1] = wdir[MAX_PATH-1] = 0;
    GetModuleFileNameW(NULL, wexe, MAX_PATH);
    wexe[MAX_PATH-1] = 0;

    /* 1. Download the release zip */
    WCHAR wpath[512];
    WCHAR wtag[64];
    MultiByteToWideChar(CP_UTF8, 0, tag, -1, wtag, 64);
    _snwprintf(wpath, 512,
               L"/" UPDATE_REPO_OWNER L"/" UPDATE_REPO_NAME
               L"/releases/download/%s/" L"FilePathX-win64.zip", wtag);
    wpath[511] = 0;
    FILE* zf = _wfopen(wzip, L"wb");
    if (!zf) {
        upd_set(UPDATE_ERROR, NULL, "cannot write to %TEMP%");
        InterlockedExchange(&g_upd_busy, 0);
        return 0;
    }
    char err[128];
    char* ok = http_get(L"github.com", wpath, zf, NULL, err, sizeof(err));
    fclose(zf);
    if (!ok) {
        DeleteFileW(wzip);
        upd_set(UPDATE_ERROR, NULL, err);
        InterlockedExchange(&g_upd_busy, 0);
        return 0;
    }

    /* 2. Write the swap-and-restart PowerShell script (UTF-8 + BOM so
          non-ASCII install paths survive powershell -File). */
    char zip8[MAX_PATH * 3], dir8[MAX_PATH * 3], exe8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, wzip, -1, zip8, sizeof(zip8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, wdir, -1, dir8, sizeof(dir8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, wexe, -1, exe8, sizeof(exe8), NULL, NULL);

    char script[8192];
    script[0] = 0;
    strcat(script, "$zip = '"); ps_quote_append(script, sizeof(script), zip8);
    strcat(script, "'\r\n$dir = '"); ps_quote_append(script, sizeof(script), dir8);
    strcat(script, "'\r\n$exe = '"); ps_quote_append(script, sizeof(script), exe8);
    strcat(script,
        "'\r\n"
        "for ($i = 0; $i -lt 30; $i++) {\r\n"
        "  try { Expand-Archive -LiteralPath $zip -DestinationPath $dir -Force -ErrorAction Stop; break }\r\n"
        "  catch { Start-Sleep -Seconds 1 }\r\n"
        "}\r\n"
        "$new = Join-Path $dir 'FilePathX.exe'\r\n"
        "$ok = $false\r\n"
        "if (Test-Path -LiteralPath $new) {\r\n"
        /* the copy fails while the old exe still runs (image is locked) —
           retry until the process is gone */
        "  for ($i = 0; $i -lt 60; $i++) {\r\n"
        "    try { Copy-Item -LiteralPath $new -Destination $exe -Force -ErrorAction Stop; $ok = $true; break }\r\n"
        "    catch { Start-Sleep -Seconds 1 }\r\n"
        "  }\r\n"
        "}\r\n"
        "if ($ok) { Start-Process -FilePath $exe }\r\n"
        "Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue\r\n"
        "Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue\r\n"
        "Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue\r\n");

    FILE* sf = _wfopen(wps1, L"wb");
    if (!sf) {
        DeleteFileW(wzip);
        upd_set(UPDATE_ERROR, NULL, "cannot write to %TEMP%");
        InterlockedExchange(&g_upd_busy, 0);
        return 0;
    }
    fwrite("\xEF\xBB\xBF", 1, 3, sf);
    fwrite(script, 1, strlen(script), sf);
    fclose(sf);

    /* 3. Launch it hidden and tell the UI thread to quit */
    WCHAR cmdline[MAX_PATH + 128];
    _snwprintf(cmdline, MAX_PATH + 128,
               L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\"", wps1);
    cmdline[MAX_PATH + 127] = 0;
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DeleteFileW(wzip);
        DeleteFileW(wps1);
        upd_set(UPDATE_ERROR, NULL, "could not start updater");
        InterlockedExchange(&g_upd_busy, 0);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    upd_set(UPDATE_RESTART_PENDING, NULL, NULL);
    InterlockedExchange(&g_upd_busy, 0);
    return 0;
}

/* ---- Public API ---- */

void update_init(HWND hwnd, UINT notify_msg, const char* state_file) {
    if (!g_upd_cs_ready) {
        InitializeCriticalSection(&g_upd_cs);
        g_upd_cs_ready = 1;
    }
    g_notify_hwnd = hwnd;
    g_notify_msg  = notify_msg;
    strncpy(g_state_file, state_file, MAX_PATH - 1);
    g_state_file[MAX_PATH - 1] = 0;
}

void update_check_async(int force) {
    if (!g_upd_cs_ready) return;
    if (InterlockedCompareExchange(&g_upd_busy, 1, 0) != 0) return;

    if (!force) {
        long long last = 0;
        char latest[64];
        state_load(&last, latest, sizeof(latest));
        long long now = (long long)time(NULL);
        if (last > 0 && now >= last && now - last < UPDATE_CHECK_EVERY && latest[0]) {
            /* Answer from cache — no network */
            upd_set(ver_is_newer(latest) ? UPDATE_AVAILABLE : UPDATE_UP_TO_DATE,
                    latest, NULL);
            InterlockedExchange(&g_upd_busy, 0);
            return;
        }
    }
    upd_set(UPDATE_CHECKING, NULL, NULL);
    HANDLE h = CreateThread(NULL, 0, check_worker, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else {
        upd_set(UPDATE_ERROR, NULL, "thread create failed");
        InterlockedExchange(&g_upd_busy, 0);
    }
}

void update_install_async(void) {
    if (!g_upd_cs_ready) return;
    if (update_status() != UPDATE_AVAILABLE) return;
    if (InterlockedCompareExchange(&g_upd_busy, 1, 0) != 0) return;
    upd_set(UPDATE_DOWNLOADING, NULL, NULL);
    HANDLE h = CreateThread(NULL, 0, install_worker, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else {
        upd_set(UPDATE_ERROR, NULL, "thread create failed");
        InterlockedExchange(&g_upd_busy, 0);
    }
}
