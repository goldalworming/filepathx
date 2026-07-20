#define WIN32_LEAN_AND_MEAN
#define OEMRESOURCE
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "render.h"
#include "ui.h"

#ifndef DROPEFFECT_COPY
#define DROPEFFECT_COPY 1
#define DROPEFFECT_MOVE 2
#endif
#ifndef SEE_MASK_INVOKEIDLIST
#define SEE_MASK_INVOKEIDLIST 0x0000000C
#endif

/* ---- WGL ---- */
#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);
typedef BOOL  (WINAPI *PFN_wglSwapIntervalEXT)(int);

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

/* MARGINS is defined in uxtheme.h, included by dwmapi.h in MinGW */

/* ---- Layout ---- */
#define TAB_BAR_H    28
#define TOOLBAR_H    26
#define COL_HDR_H    22
/* Runtime-tunable via Settings modal — g_row_h persisted in settings.ini */
static int g_row_h = 20;
#define ROW_H        g_row_h
#define STATUS_BAR_H 26
#define SIDEBAR_W    185
#define CONTENT_TOP  (TAB_BAR_H + TOOLBAR_H)
#define RESIZE_BORDER 6
#define WIN_BTN_W    46

/* Column widths */
#define COLW_TYPE    90
#define COLW_DATE    150
#define COLW_SIZE    55

/* ---- Context menu IDs ---- */
#define IDM_OPEN        1001
#define IDM_OPEN_TAB    1002
#define IDM_CUT         1003
#define IDM_COPY        1004
#define IDM_PASTE       1005
#define IDM_DELETE      1006
#define IDM_RENAME      1007
#define IDM_NEW_FOLDER  1008
#define IDM_NEW_FILE    1009
#define IDM_REFRESH     1010
#define IDM_PROPERTIES  1011
#define IDM_COPY_PATH   1012
#define IDM_OPEN_TERMINAL 1013
#define IDM_ADD_BOOKMARK    1014
#define IDM_REMOVE_BOOKMARK 1015
#define IDM_OPEN_WITH       1016
#define IDM_VIEW_DETAILS    1020
#define IDM_VIEW_SMALL      1021
#define IDM_VIEW_LARGE      1022

/* ---- Data ---- */
#define MAX_ENTRIES 2048
#define MAX_TABS    32
#define MAX_HIST    32

/* View modes (VM_* to avoid clash with commctrl.h VM_DETAILS) */
#define VM_DETAILS     0
#define VM_SMALL_ICONS 1
#define VM_LARGE_ICONS 2

#define MAX_GROUPS 5
static const char* GROUP_LABELS[MAX_GROUPS] = {
    "Today", "Earlier this week", "Last week", "Last month", "A long time ago"
};

typedef struct {
    char name[MAX_PATH];
    int  is_dir;
    ULONGLONG size;
    FILETIME  modified;
    int       group;
} FileEntry;

typedef struct {
    char path[MAX_PATH];
    char title[64];
    FileEntry entries[MAX_ENTRIES];
    int entry_count;
    int selected;
    int sel_anchor;
    unsigned char sel_mask[MAX_ENTRIES];
    float scroll_y, target_scroll;
    char history[MAX_HIST][MAX_PATH];
    int hist_count, hist_pos;
    int use_groups;
    int group_collapsed[MAX_GROUPS];
    int view_mode;  /* VM_DETAILS / SMALL_ICONS / LARGE_ICONS */
} Tab;

typedef struct {
    char name[48];
    char path[MAX_PATH];
    char short_path[24];
    uint32_t icon_color;
    GLuint icon_tex;
} SidebarItem;

typedef struct {
    char title[32];
    int expanded;
    SidebarItem items[16];
    int item_count;
} SidebarSection;

typedef struct {
    Tab tabs[MAX_TABS];
    int tab_count, active_tab;
    int last_click_item;
    DWORD last_click_time;
    float tab_scroll;
    float tab_scroll_target;
    int   tab_scroll_last_active;
} Panel;

typedef struct {
    Panel panels[2];
    int active_panel;          /* 0 = left/only, 1 = right */
    int split_active;          /* 1 = both panels visible */
    float split_ratio;         /* 0..1, left panel width fraction */
    SidebarSection sections[4];
    int section_count;
    int sort_col, sort_asc;
} App;

/* ---- Globals ---- */
static Renderer g_renderer;
static UI       g_ui;
static App      g_app;
static GLuint   g_logo_tex = 0;
static HWND     g_hwnd;
static int      g_width = 1000, g_height = 680;
static int      g_mouse_x, g_mouse_y, g_mouse_down;
static int      g_mouse_clicked, g_mouse_released, g_mouse_dblclick;
static float    g_scroll_delta;
static int      g_needs_redraw = 1;
static char     g_user_profile[MAX_PATH];
static char     g_downloads_path[MAX_PATH];

/* UTF-8 / UTF-16 conversion helpers. Buffers in UTF-8 throughout the app;
   convert to UTF-16 only when calling W APIs. */
static int u8_to_w(const char* u8, WCHAR* out, int outn) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, outn);
    if (n == 0 && outn > 0) out[0] = 0;
    return n;
}
static int w_to_u8(const WCHAR* w, char* out, int outn) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outn, NULL, NULL);
    if (n == 0 && outn > 0) out[0] = 0;
    return n;
}
/* Helper to combine path+name and convert to wide for a single API call. */
static void path_join_w(const char* dir, const char* name, WCHAR* out, int outn) {
    char tmp[MAX_PATH * 2];
    _snprintf(tmp, sizeof(tmp), "%s\\%s", dir, name);
    u8_to_w(tmp, out, outn);
}

/* ---- Folder change watcher (auto-refresh when files change) ---- */
static HANDLE g_watch_handle = INVALID_HANDLE_VALUE;
static char   g_watch_path[MAX_PATH] = {0};

static void watch_stop(void) {
    if (g_watch_handle != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(g_watch_handle);
        g_watch_handle = INVALID_HANDLE_VALUE;
    }
    g_watch_path[0] = 0;
}

static void watch_start(const char* path) {
    if (!path || !path[0]) { watch_stop(); return; }
    if (g_watch_handle != INVALID_HANDLE_VALUE &&
        _stricmp(g_watch_path, path) == 0) return;
    watch_stop();
    WCHAR wp[MAX_PATH]; u8_to_w(path, wp, MAX_PATH);
    g_watch_handle = FindFirstChangeNotificationW(wp, FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME |
        FILE_NOTIFY_CHANGE_DIR_NAME  |
        FILE_NOTIFY_CHANGE_SIZE      |
        FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (g_watch_handle != INVALID_HANDLE_VALUE) {
        strncpy(g_watch_path, path, MAX_PATH-1);
        g_watch_path[MAX_PATH-1] = 0;
    }
}

/* Forward decls used by IDropTarget below */
static void scan_directory(Tab* tab);
static void make_unique_name(const char* dir, const char* base_name, char* out, int out_n);

/* ---- IDropTarget: receive drops onto our window (drag between panels) ---- */
typedef struct { IDropTargetVtbl* lpVtbl; LONG refs; } DT;
static DT g_drop_target;
static int g_drop_hover_panel = -1; /* which panel is the cursor over while dragging in */

static HRESULT STDMETHODCALLTYPE DT_QI(IDropTarget* This, REFIID riid, void** ppv) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *ppv = This; ((DT*)This)->refs++; return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG   STDMETHODCALLTYPE DT_AddRef(IDropTarget* This)  { return ++((DT*)This)->refs; }
static ULONG   STDMETHODCALLTYPE DT_Release(IDropTarget* This) { return --((DT*)This)->refs; }

static int target_panel_for_screen_pt(POINTL pt) {
    POINT p = { pt.x, pt.y };
    ScreenToClient(g_hwnd, &p);
    if (p.x < SIDEBAR_W || p.y < CONTENT_TOP) return -1;
    if (p.y >= g_height - STATUS_BAR_H) return -1;
    if (!g_app.split_active) return 0;
    float split_x = SIDEBAR_W + g_app.split_ratio * (g_width - SIDEBAR_W);
    return (p.x < split_x) ? 0 : 1;
}

static DWORD effect_from_keystate(DWORD ks, DWORD allowed) {
    DWORD want = (ks & MK_CONTROL) ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
    if (allowed & want) return want;
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    return DROPEFFECT_NONE;
}

static HRESULT STDMETHODCALLTYPE DT_DragEnter(IDropTarget* This, IDataObject* pdo, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) {
    (void)This; (void)pdo;
    g_drop_hover_panel = target_panel_for_screen_pt(pt);
    *pdwEffect = (g_drop_hover_panel >= 0) ? effect_from_keystate(grfKeyState, *pdwEffect) : DROPEFFECT_NONE;
    g_needs_redraw = 1;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE DT_DragOver(IDropTarget* This, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) {
    (void)This;
    g_drop_hover_panel = target_panel_for_screen_pt(pt);
    *pdwEffect = (g_drop_hover_panel >= 0) ? effect_from_keystate(grfKeyState, *pdwEffect) : DROPEFFECT_NONE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE DT_DragLeave(IDropTarget* This) {
    (void)This;
    g_drop_hover_panel = -1;
    g_needs_redraw = 1;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE DT_Drop(IDropTarget* This, IDataObject* pdo, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) {
    (void)This;
    int target = target_panel_for_screen_pt(pt);
    g_drop_hover_panel = -1;
    if (target < 0) { *pdwEffect = DROPEFFECT_NONE; return S_OK; }
    DWORD effect = effect_from_keystate(grfKeyState, *pdwEffect);
    if (effect == DROPEFFECT_NONE) { *pdwEffect = DROPEFFECT_NONE; return S_OK; }

    FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM med = {0};
    if (FAILED(pdo->lpVtbl->GetData(pdo, &fmt, &med))) { *pdwEffect = DROPEFFECT_NONE; return S_OK; }
    HDROP hd = (HDROP)med.hGlobal;
    const char* dest_dir = g_app.panels[target].tabs[g_app.panels[target].active_tab].path;
    int count = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
    for (int i = 0; i < count; i++) {
        WCHAR wsrc[MAX_PATH + 2] = {0}, wdst[MAX_PATH + 2] = {0};
        DragQueryFileW(hd, i, wsrc, MAX_PATH);
        char src[MAX_PATH]; w_to_u8(wsrc, src, MAX_PATH);
        const char* nm = strrchr(src, '\\');
        nm = nm ? nm + 1 : src;
        char src_dir[MAX_PATH]; strncpy(src_dir, src, MAX_PATH-1); src_dir[MAX_PATH-1]=0;
        char* slash = strrchr(src_dir, '\\'); if (slash) *slash = 0;
        int same_dir = (_stricmp(src_dir, dest_dir) == 0);
        if (same_dir && effect == DROPEFFECT_MOVE) continue;
        char dst[MAX_PATH], dst_name[MAX_PATH];
        if (same_dir && effect == DROPEFFECT_COPY) {
            char stem[MAX_PATH], ext[MAX_PATH] = "";
            strncpy(stem, nm, MAX_PATH-1); stem[MAX_PATH-1]=0;
            char* dot = strrchr(stem, '.');
            if (dot && dot != stem) { strncpy(ext, dot, MAX_PATH-1); *dot=0; }
            char base[MAX_PATH]; _snprintf(base, MAX_PATH, "%s - Copy%s", stem, ext);
            make_unique_name(dest_dir, base, dst_name, MAX_PATH);
        } else {
            strncpy(dst_name, nm, MAX_PATH-1); dst_name[MAX_PATH-1]=0;
        }
        _snprintf(dst, MAX_PATH, "%s\\%s", dest_dir, dst_name);
        u8_to_w(dst, wdst, MAX_PATH);
        SHFILEOPSTRUCTW op = {0};
        op.hwnd = g_hwnd;
        op.wFunc = (effect == DROPEFFECT_MOVE) ? FO_MOVE : FO_COPY;
        op.pFrom = wsrc;
        op.pTo   = wdst;
        op.fFlags = FOF_ALLOWUNDO;
        SHFileOperationW(&op);
    }
    ReleaseStgMedium(&med);
    /* Refresh both panels so source + dest update */
    scan_directory(&g_app.panels[0].tabs[g_app.panels[0].active_tab]);
    if (g_app.panels[1].tab_count > 0)
        scan_directory(&g_app.panels[1].tabs[g_app.panels[1].active_tab]);
    *pdwEffect = effect;
    g_needs_redraw = 1;
    return S_OK;
}

static IDropTargetVtbl g_dt_vtbl = {
    DT_QI, DT_AddRef, DT_Release,
    DT_DragEnter, DT_DragOver, DT_DragLeave, DT_Drop
};

/* ---- Drag-out (drag selected files to other apps) ---- */
static int g_drag_idx = -1;
static int g_drag_panel = -1;        /* which panel the drag started in */
static int g_drag_x0  = 0, g_drag_y0 = 0;
static int g_dragging = 0;

/* ---- Bookmark reorder drag ---- */
static int   g_bm_drag_idx     = -1;
static int   g_bm_drag_y0      = 0;
static int   g_bm_drag_active  = 0;
static int   g_bm_drop_slot    = -1;

/* ---- Marquee (rubber-band) selection in icon views ---- */
static int   g_marquee_active = 0;
static int   g_marquee_panel  = -1;
static float g_marquee_x0     = 0;   /* content-space (scroll-adjusted) start */
static float g_marquee_y0     = 0;
static int   g_marquee_additive = 0;
static unsigned char g_marquee_anchor[MAX_ENTRIES];

static void start_drag_out(Tab* t) {
    if (g_dragging) return;
    /* Collect selected non-".." entries */
    int idxs[MAX_ENTRIES], n = 0;
    for (int i = 0; i < t->entry_count; i++) {
        if (t->sel_mask[i] && strcmp(t->entries[i].name, "..") != 0)
            idxs[n++] = i;
    }
    if (n == 0) return;

    WCHAR wdir[MAX_PATH];
    u8_to_w(t->path, wdir, MAX_PATH);
    PIDLIST_ABSOLUTE dir_pidl = NULL;
    if (SHParseDisplayName(wdir, NULL, &dir_pidl, 0, NULL) != S_OK || !dir_pidl) return;
    IShellFolder* desktop = NULL;
    if (SHGetDesktopFolder(&desktop) != S_OK) { CoTaskMemFree(dir_pidl); return; }
    IShellFolder* folder = NULL;
    HRESULT hr = IShellFolder_BindToObject(desktop, dir_pidl, NULL, &IID_IShellFolder, (void**)&folder);
    IShellFolder_Release(desktop);
    if (FAILED(hr) || !folder) { CoTaskMemFree(dir_pidl); return; }

    LPITEMIDLIST* children = (LPITEMIDLIST*)calloc(n, sizeof(LPITEMIDLIST));
    int got = 0;
    for (int i = 0; i < n; i++) {
        WCHAR wname[MAX_PATH];
        u8_to_w(t->entries[idxs[i]].name, wname, MAX_PATH);
        ULONG eaten = 0;
        LPITEMIDLIST p = NULL;
        if (SUCCEEDED(IShellFolder_ParseDisplayName(folder, NULL, NULL, wname, &eaten, &p, NULL)) && p)
            children[got++] = p;
    }
    if (got == 0) {
        free(children);
        IShellFolder_Release(folder);
        CoTaskMemFree(dir_pidl);
        return;
    }

    IDataObject* dobj = NULL;
    hr = SHCreateDataObject(dir_pidl, got, (PCUITEMID_CHILD_ARRAY)children, NULL,
                             &IID_IDataObject, (void**)&dobj);
    if (SUCCEEDED(hr) && dobj) {
        g_dragging = 1;
        DWORD effect = 0;
        SHDoDragDrop(g_hwnd, dobj, NULL,
                     DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        g_dragging = 0;
        IDataObject_Release(dobj);
        /* Reset input state so the post-drag mouse-up doesn't leak into a click */
        g_mouse_down = g_mouse_clicked = g_mouse_released = 0;
        g_drag_idx = -1;
    }
    for (int i = 0; i < got; i++) CoTaskMemFree(children[i]);
    free(children);
    IShellFolder_Release(folder);
    CoTaskMemFree(dir_pidl);
}

/* UI ID offset (set per-panel during render to avoid ID collisions) */
static int   g_ui_id_base = 0;
#define UIID(x) ((x) + g_ui_id_base)

/* Real focused panel (preserved across the render loop where active_panel
   is temporarily swapped per-panel). Used to color active tab differently. */
static int   g_focused_panel = 0;

/* Splitter drag state */
static int   g_splitter_dragging = 0;
#define SPLITTER_HIT_W 6

/* Sync scroll between panels (default off) */
static int   g_sync_scroll = 0;

/* ---- Settings modal ---- */
static int g_settings_open      = 0;
static int g_pref_font_size     = 11;   /* applied on next launch */
static int g_pref_row_h         = 20;   /* applied immediately    */
/* Session-scoped feedback for "font size will change on next launch" */
static int g_pref_font_needs_restart = 0;

/* Forward decls (defined later in this file) */
static void app_data_file(const char* name, char* out, int n);
static FILE* u8_fopen(const char* path, const char* mode);

static void settings_save(void) {
    char fp[MAX_PATH];
    app_data_file("settings.ini", fp, MAX_PATH);
    FILE* f = u8_fopen(fp, "wb");
    if (!f) return;
    fprintf(f, "font_size = %d\r\n", g_pref_font_size);
    fprintf(f, "row_h = %d\r\n",     g_pref_row_h);
    fclose(f);
}

static void settings_load(void) {
    char fp[MAX_PATH];
    app_data_file("settings.ini", fp, MAX_PATH);
    FILE* f = u8_fopen(fp, "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1]=='\n' || line[n-1]=='\r' ||
                         line[n-1]==' '  || line[n-1]=='\t')) line[--n] = 0;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == ';' || *p == '#' || *p == '[') continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        int kl = (int)strlen(p);
        while (kl > 0 && (p[kl-1]==' ' || p[kl-1]=='\t')) p[--kl] = 0;
        for (int i = 0; p[i]; i++)
            if (p[i] >= 'A' && p[i] <= 'Z') p[i] += 32;
        int v = atoi(val);
        if (!strcmp(p, "font_size"))    g_pref_font_size = v;
        else if (!strcmp(p, "row_h"))   g_pref_row_h = v;
    }
    fclose(f);
    /* Clamp */
    if (g_pref_font_size < 9)  g_pref_font_size = 9;
    if (g_pref_font_size > 16) g_pref_font_size = 16;
    if (g_pref_row_h < 14) g_pref_row_h = 14;
    if (g_pref_row_h > 32) g_pref_row_h = 32;
    g_row_h = g_pref_row_h;
}

/* ---- Hover tooltips ---- */
static char  g_tt_text[128]  = "";
static int   g_tt_ax = 0, g_tt_ay = 0;   /* anchor point (usually button top-centre) */
static DWORD g_tt_since       = 0;       /* when this tooltip text was first set */
static char  g_tt_last_text[128] = "";

static void tt_set(const char* text, int anchor_x, int anchor_y) {
    strncpy(g_tt_text, text, sizeof(g_tt_text)-1);
    g_tt_text[sizeof(g_tt_text)-1] = 0;
    g_tt_ax = anchor_x;
    g_tt_ay = anchor_y;
    if (strcmp(g_tt_text, g_tt_last_text) != 0) {
        strncpy(g_tt_last_text, g_tt_text, sizeof(g_tt_last_text)-1);
        g_tt_last_text[sizeof(g_tt_last_text)-1] = 0;
        g_tt_since = GetTickCount();
    }
}

static void tt_draw(void) {
    if (!g_tt_text[0]) { g_tt_last_text[0] = 0; return; }
    DWORD elapsed = GetTickCount() - g_tt_since;
    if (elapsed < 400) { g_needs_redraw = 1; return; }
    Renderer* r = &g_renderer;
    int tw  = render_text_width(r, g_tt_text);
    int pad = 8;
    int bw  = tw + pad * 2;
    int bh  = r->font_height + 8;
    int tx  = g_tt_ax - bw / 2;
    int ty  = g_tt_ay - bh - 6;         /* above anchor */
    if (tx < 4) tx = 4;
    if (tx + bw > g_width - 4) tx = g_width - bw - 4;
    if (ty < 4) ty = g_tt_ay + 20;      /* below if no room above */
    render_quad(r, tx - 1, ty - 1, bw + 2, bh + 2, COL_BORDER);
    render_quad(r, tx,     ty,     bw,     bh,     COL_HEADER);
    render_text(r, g_tt_text, tx + pad, ty + (bh - r->font_height) / 2, COL_TEXT);
}

/* Tab bar hit-test zones written by build_tab_bar each frame. */
static int   g_tab_bar_tabs_end_x  = 0;
static int   g_tab_bar_btns_start_x = 0;
static int   g_tab_bar_btn_end_x   = 0;

/* Tab reorder drag state */
static int   g_tab_drag_idx    = -1;
static int   g_tab_drag_panel  = -1;
static int   g_tab_drag_active = 0;
static int   g_tab_drag_x0     = 0;
static int   g_tab_drag_offset = 0;

/* ---- Type-ahead search ---- */
#define TYPEAHEAD_RESET_MS 300
static char  g_typeahead_buf[64];
static int   g_typeahead_len = 0;
static DWORD g_typeahead_time = 0;

/* ---- Shell context menu (active during TrackPopupMenu) ---- */
static IContextMenu2* g_shell_ctx2 = NULL;
static IContextMenu3* g_shell_ctx3 = NULL;

/* ---- Batch rename ---- */
static int  g_batch_active = 0;
static int  g_batch_focus  = 0;   /* 0 = suffix side, 1 = prefix side */
static char g_batch_prefix[MAX_PATH];
static int  g_batch_prefix_len = 0;
static int  g_batch_chop_left  = 0;   /* stem chars trimmed from start */
static int  g_batch_panel  = -1;
static char g_batch_typed[MAX_PATH];
static int  g_batch_typed_len = 0;
static int  g_batch_chop = 0;
static UINT     g_cf_drop_effect;

/* ---- Fuzzy finder (Ctrl+F) ---- */
#define FF_MAX_INDEX     20000
#define FF_QUERY_MAX     128
#define FF_RESULT_MAX    200
#define FF_MATCH_MARKS   32     /* max highlighted char positions per name */

typedef struct {
    char name[MAX_PATH];    /* file / folder name (leaf) */
    char rel[MAX_PATH];     /* path relative to root, without leaf */
    int  is_dir;
} FFEntry;

typedef struct {
    int   entry_idx;
    int   score;
    int   n_marks;
    short marks[FF_MATCH_MARKS];   /* byte offsets into name that matched */
} FFResult;

static int     g_ff_active   = 0;
static int     g_ff_panel    = -1;
static char    g_ff_query[FF_QUERY_MAX];
static int     g_ff_query_len = 0;
static int     g_ff_cursor    = 0;   /* byte offset into g_ff_query, 0..query_len */
static int     g_ff_recursive = 0;
static int     g_ff_selected  = 0;
static int     g_ff_scroll    = 0;
static FFEntry g_ff_index[FF_MAX_INDEX];
static int     g_ff_index_count = 0;
static FFResult g_ff_results[FF_RESULT_MAX];
static int     g_ff_result_count = 0;
static char    g_ff_root[MAX_PATH];   /* root path used to build the index */
/* Recursive scan runs on a worker so the UI stays live */
static HANDLE  g_ff_scan_thread = NULL;
static volatile LONG g_ff_scan_cancel = 0;
static volatile LONG g_ff_scan_gen    = 0;   /* bumped on each new scan */
static CRITICAL_SECTION g_ff_cs;
static int     g_ff_cs_init = 0;
static volatile int g_ff_scanning = 0;

/* Forward decls */
static void scroll_to_entry(Tab* t, int idx);
static void ff_open(void);
static void ff_close(void);
static void ff_refresh_results(void);
static void ff_build_index_from_tab(void);
static void ff_kick_recursive_scan(void);
static int  ff_score_match(const char* name, const char* query, int qlen,
                           short* marks, int* n_marks);
static void sel_clear(Tab* t);
static void sel_only(Tab* t, int i);
static void sel_range(Tab* t, int from, int to);
static int  sel_count(Tab* t);
static void tabs_save(void);
static void app_data_file(const char* name, char* out, int n);
static int  sort_prefs_lookup(const char* path, int* out_col, int* out_asc);
static void sort_prefs_set(const char* path, int col, int asc);
static int  view_prefs_lookup(const char* path);
static void view_prefs_set(const char* path, int mode);
static FILE* u8_fopen(const char* path, const char* mode);

/* Open a file using ShellExecute from a fresh worker thread. Photos UWP fails to
   enter gallery (next/prev) mode when launched from this app's main COM apartment,
   even though identical code in a standalone console app works. Isolating in a new
   thread + apartment seems to give Photos the context it needs. */
/* Photos UWP gallery (next/prev) mode does not activate when launched from this
   app, regardless of mechanism tried (ShellExecute variants, IContextMenu invoke,
   IApplicationActivationManager.ActivateForFile, worker threads, cmd intermediary).
   Identical code works from a console test app. Suspect manifest / token / parent
   process check on Photos's side. Documented as known limitation for now. */
static void open_file_async(const WCHAR* wp) {
    ShellExecuteW(NULL, L"open", wp, NULL, NULL, SW_SHOWNORMAL);
}

/* Today midnight (local) FILETIME ticks + current day-of-week, refreshed per scan */
static LONGLONG g_today_ft100 = 0;
static int      g_today_dow   = 0;

static void refresh_today(void) {
    SYSTEMTIME st; GetLocalTime(&st);
    g_today_dow = st.wDayOfWeek;
    st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
    FILETIME ft; SystemTimeToFileTime(&st, &ft);
    g_today_ft100 = ((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static int compute_group(FILETIME utc_modified) {
    FILETIME local_ft;
    if (!FileTimeToLocalFileTime(&utc_modified, &local_ft)) return MAX_GROUPS - 1;
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&local_ft, &st)) return MAX_GROUPS - 1;
    st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
    FILETIME mid_ft; SystemTimeToFileTime(&st, &mid_ft);
    LONGLONG mid = ((LONGLONG)mid_ft.dwHighDateTime << 32) | mid_ft.dwLowDateTime;
    LONGLONG days = (g_today_ft100 - mid) / 864000000000LL;
    if (days <= 0) return 0;
    if (days <= g_today_dow) return 1;
    if (days <= g_today_dow + 7) return 2;
    if (days <= 30) return 3;
    return 4;
}

static int path_eq_ci(const char* a, const char* b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    while (la > 0 && (a[la-1] == '\\' || a[la-1] == '/')) la--;
    while (lb > 0 && (b[lb-1] == '\\' || b[lb-1] == '/')) lb--;
    if (la != lb) return 0;
    return _strnicmp(a, b, la) == 0;
}

/* ---- Inline rename ---- */
static HWND     g_edit_hwnd;
static int      g_edit_idx = -1;
static int      g_edit_panel = -1;
static WNDPROC  g_edit_oldproc;
static HBRUSH   g_edit_bg_brush;
static HFONT    g_edit_font;

/* ---- Address bar edit ---- */
static HWND     g_addr_hwnd;
static int      g_addr_panel = -1;   /* which panel owns the path-edit overlay */
static WNDPROC  g_addr_oldproc;
static HBRUSH   g_addr_bg_brush;
static int      g_addr_x, g_addr_y, g_addr_w, g_addr_h;

/* Bookmark ribbon shape: rectangular body with V-notch at bottom. */
static void draw_bookmark(Renderer* r, float x, float y, float w, float h, uint32_t color) {
    int notch = (int)(h * 0.32f); if (notch < 3) notch = 3;
    float body_h = h - notch;
    render_quad(r, x, y, w, body_h, color);
    for (int i = 0; i < notch; i++) {
        float row_y = y + body_h + i;
        float v_half = ((float)(i + 1) / notch) * (w / 2 - 0.5f);
        float side_w = w / 2 - v_half;
        if (side_w > 0.5f) {
            render_quad(r, x, row_y, side_w, 1, color);
            render_quad(r, x + w - side_w, row_y, side_w, 1, color);
        }
    }
}

/* Draw a slightly rounded rect via 5 horizontal slabs (R=2 corner shave). */
static void render_round_rect(Renderer* r, float x, float y, float w, float h, uint32_t color) {
    render_quad(r, x+2, y,       w-4, 1,   color);
    render_quad(r, x+1, y+1,     w-2, 1,   color);
    render_quad(r, x,   y+2,     w,   h-4, color);
    render_quad(r, x+1, y+h-2,   w-2, 1,   color);
    render_quad(r, x+2, y+h-1,   w-4, 1,   color);
}

/* ---- Thumbnail / large-icon cache (async worker thread) ---- */
#define THUMB_CACHE_CAP 200
#define THUMB_Q_CAP     256
#define THUMB_PX        192
typedef struct {
    char  path[MAX_PATH];
    GLuint texture;
    int    w, h;
    DWORD  last_used;
} ThumbEntry;
typedef struct { char path[MAX_PATH]; } ThumbReq;
typedef struct { char path[MAX_PATH]; HBITMAP bmp; int w, h; int flip_v; } ThumbDone;

static ThumbEntry g_thumb_cache[THUMB_CACHE_CAP];
static int        g_thumb_count = 0;
static DWORD      g_thumb_frame = 0;
static ThumbReq   g_thumb_req[THUMB_Q_CAP];
static int        g_thumb_req_head = 0, g_thumb_req_tail = 0;
static ThumbDone  g_thumb_done[THUMB_Q_CAP];
static int        g_thumb_done_count = 0;
static CRITICAL_SECTION g_thumb_cs;
static HANDLE g_thumb_event = NULL;
static HANDLE g_thumb_thread = NULL;
static volatile LONG g_thumb_quit = 0;
static int g_thumb_initialized = 0;

static int thumb_cache_find(const char* path) {
    for (int i = 0; i < g_thumb_count; i++)
        if (_stricmp(g_thumb_cache[i].path, path) == 0) return i;
    return -1;
}
static GLuint thumb_cache_get(const char* path) {
    int i = thumb_cache_find(path);
    if (i < 0) return 0;
    g_thumb_cache[i].last_used = g_thumb_frame;
    return g_thumb_cache[i].texture;
}
static void thumb_cache_put(const char* path, GLuint tex, int w, int h) {
    int idx = thumb_cache_find(path);
    if (idx >= 0) {
        glDeleteTextures(1, &g_thumb_cache[idx].texture);
        g_thumb_cache[idx].texture = tex;
        g_thumb_cache[idx].w = w; g_thumb_cache[idx].h = h;
        g_thumb_cache[idx].last_used = g_thumb_frame;
        return;
    }
    if (g_thumb_count >= THUMB_CACHE_CAP) {
        int lru = 0;
        for (int i = 1; i < g_thumb_count; i++)
            if (g_thumb_cache[i].last_used < g_thumb_cache[lru].last_used) lru = i;
        glDeleteTextures(1, &g_thumb_cache[lru].texture);
        g_thumb_cache[lru] = g_thumb_cache[g_thumb_count - 1];
        g_thumb_count--;
    }
    int n = g_thumb_count++;
    strncpy(g_thumb_cache[n].path, path, MAX_PATH-1);
    g_thumb_cache[n].path[MAX_PATH-1] = 0;
    g_thumb_cache[n].texture = tex;
    g_thumb_cache[n].w = w; g_thumb_cache[n].h = h;
    g_thumb_cache[n].last_used = g_thumb_frame;
}

static int thumb_req_pending(const char* path) {
    int found = 0;
    EnterCriticalSection(&g_thumb_cs);
    for (int i = g_thumb_req_head; i != g_thumb_req_tail; i = (i+1) % THUMB_Q_CAP)
        if (_stricmp(g_thumb_req[i].path, path) == 0) { found = 1; break; }
    LeaveCriticalSection(&g_thumb_cs);
    return found;
}
static void thumb_request(const char* path) {
    if (!g_thumb_initialized) return;
    if (thumb_cache_get(path)) return;
    if (thumb_req_pending(path)) return;
    EnterCriticalSection(&g_thumb_cs);
    int next_tail = (g_thumb_req_tail + 1) % THUMB_Q_CAP;
    if (next_tail != g_thumb_req_head) {
        strncpy(g_thumb_req[g_thumb_req_tail].path, path, MAX_PATH-1);
        g_thumb_req[g_thumb_req_tail].path[MAX_PATH-1] = 0;
        g_thumb_req_tail = next_tail;
        SetEvent(g_thumb_event);
    }
    LeaveCriticalSection(&g_thumb_cs);
}

static DWORD WINAPI thumb_worker(LPVOID arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    while (!g_thumb_quit) {
        WaitForSingleObject(g_thumb_event, INFINITE);
        if (g_thumb_quit) break;
        for (;;) {
            ThumbReq req;
            EnterCriticalSection(&g_thumb_cs);
            if (g_thumb_req_head == g_thumb_req_tail) {
                LeaveCriticalSection(&g_thumb_cs); break;
            }
            req = g_thumb_req[g_thumb_req_head];
            g_thumb_req_head = (g_thumb_req_head + 1) % THUMB_Q_CAP;
            LeaveCriticalSection(&g_thumb_cs);

            WCHAR wp[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, req.path, -1, wp, MAX_PATH);
            IShellItem* item = NULL;
            if (SUCCEEDED(SHCreateItemFromParsingName(wp, NULL, &IID_IShellItem, (void**)&item)) && item) {
                IShellItemImageFactory* fac = NULL;
                if (SUCCEEDED(IShellItem_QueryInterface(item, &IID_IShellItemImageFactory, (void**)&fac)) && fac) {
                    SIZE sz = { THUMB_PX, THUMB_PX };
                    HBITMAP hbmp = NULL;
                    /* First try thumbnail-only — Windows returns these upright.
                       If no thumbnail provider is available (empty folder, .exe,
                       generic file types), fall back to the icon path which
                       comes back upside down and needs a vertical flip. */
                    HRESULT hr = fac->lpVtbl->GetImage(fac, sz,
                        SIIGBF_THUMBNAILONLY | SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &hbmp);
                    int is_thumb = SUCCEEDED(hr) && hbmp;
                    if (!is_thumb) {
                        if (hbmp) { DeleteObject(hbmp); hbmp = NULL; }
                        hr = fac->lpVtbl->GetImage(fac, sz,
                            SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &hbmp);
                    }
                    if (SUCCEEDED(hr) && hbmp) {
                        DIBSECTION ds = {0};
                        GetObject(hbmp, sizeof(ds), &ds);
                        EnterCriticalSection(&g_thumb_cs);
                        if (g_thumb_done_count < THUMB_Q_CAP) {
                            ThumbDone* d = &g_thumb_done[g_thumb_done_count++];
                            strncpy(d->path, req.path, MAX_PATH-1);
                            d->path[MAX_PATH-1] = 0;
                            d->bmp = hbmp;
                            d->w = ds.dsBm.bmWidth;
                            d->h = abs(ds.dsBm.bmHeight);
                            /* Thumbnails come back upright; icon fallbacks
                               come back upside down. */
                            d->flip_v = !is_thumb;
                        } else {
                            DeleteObject(hbmp);
                        }
                        LeaveCriticalSection(&g_thumb_cs);
                        PostMessageW(g_hwnd, WM_APP + 3, 0, 0);
                    }
                    fac->lpVtbl->Release(fac);
                }
                IShellItem_Release(item);
            }
        }
    }
    CoUninitialize();
    return 0;
}

static void thumb_cache_clear(void) {
    for (int i = 0; i < g_thumb_count; i++)
        glDeleteTextures(1, &g_thumb_cache[i].texture);
    g_thumb_count = 0;
    if (g_thumb_initialized) {
        EnterCriticalSection(&g_thumb_cs);
        g_thumb_req_head = g_thumb_req_tail = 0;  /* drop pending requests */
        LeaveCriticalSection(&g_thumb_cs);
    }
}

static void thumb_init(void) {
    if (g_thumb_initialized) return;
    InitializeCriticalSection(&g_thumb_cs);
    g_thumb_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thumb_thread = CreateThread(NULL, 0, thumb_worker, NULL, 0, NULL);
    g_thumb_initialized = 1;
}

static GLuint hbmp_to_gl(HBITMAP hbmp, int* out_w, int* out_h, int flip_v) {
    DIBSECTION ds = {0};
    if (GetObject(hbmp, sizeof(ds), &ds) == 0 || !ds.dsBm.bmBits) return 0;
    int w = ds.dsBm.bmWidth;
    int h = abs(ds.dsBm.bmHeight);
    int stride = ds.dsBm.bmWidthBytes;
    unsigned char* src = (unsigned char*)ds.dsBm.bmBits;
    unsigned char* tmp = (unsigned char*)malloc((size_t)w * h * 4);
    if (!tmp) return 0;
    for (int y = 0; y < h; y++) {
        int src_y = flip_v ? (h - 1 - y) : y;
        unsigned char* sr = src + src_y * stride;
        unsigned char* dr = tmp + y * w * 4;
        for (int x = 0; x < w; x++) {
            unsigned char b = sr[0], g = sr[1], r = sr[2], a = sr[3];
            if (a == 0 && (b | g | r) != 0) a = 255;
            dr[0] = r; dr[1] = g; dr[2] = b; dr[3] = a;
            sr += 4; dr += 4;
        }
    }
    /* Rounded corners: for content thumbnails (flip_v==0, i.e. real
       photo/video/pdf previews from Windows) fade the alpha inside a
       small quarter-circle at each corner. Shell-icon fallbacks
       (flip_v==1) already sit on a transparent background, so this
       loop is a no-op for them and their outlines stay intact. */
    if (!flip_v) {
        int radius = (w < h ? w : h) / 20;
        if (radius < 4)  radius = 4;
        if (radius > 12) radius = 12;
        for (int cy = 0; cy < radius; cy++) {
            for (int cx = 0; cx < radius; cx++) {
                float dx = (float)(radius - cx) - 0.5f;
                float dy = (float)(radius - cy) - 0.5f;
                float d  = dx*dx + dy*dy;
                float r_out = (float)(radius * radius);
                float r_in  = (float)((radius - 1) * (radius - 1));
                unsigned char am;
                if (d >= r_out)      am = 0;
                else if (d <= r_in)  continue;   /* fully inside — keep alpha */
                else {
                    /* Anti-alias band between r_in and r_out */
                    float t = (r_out - d) / (r_out - r_in);
                    am = (unsigned char)(t * 255.0f + 0.5f);
                }
                int px[4][2] = {
                    { cx,           cy           },
                    { w - 1 - cx,   cy           },
                    { cx,           h - 1 - cy   },
                    { w - 1 - cx,   h - 1 - cy   },
                };
                for (int k = 0; k < 4; k++) {
                    unsigned char* p = tmp + (px[k][1] * w + px[k][0]) * 4;
                    /* Multiply existing alpha by mask so pixels outside
                       the radius vanish and edge pixels blend. */
                    p[3] = (unsigned char)((p[3] * am + 127) / 255);
                }
            }
        }
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(tmp);
    *out_w = w; *out_h = h;
    return tex;
}

/* ---- Icon cache ---- */
#define MAX_ICON_CACHE 128
#define ICON_SIZE 16

typedef struct { char key[24]; GLuint texture; } IconCacheEntry;
static IconCacheEntry g_icons[MAX_ICON_CACHE];
static int g_icon_count = 0;

static GLuint hicon_to_gl(HICON icon) {
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = ICON_SIZE;
    bmi.bmiHeader.biHeight = -ICON_SIZE;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP old = (HBITMAP)SelectObject(dc, bmp);
    memset(bits, 0, ICON_SIZE * ICON_SIZE * 4);
    DrawIconEx(dc, 0, 0, icon, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);
    GdiFlush();
    uint8_t* p = (uint8_t*)bits;
    for (int i = 0; i < ICON_SIZE * ICON_SIZE; i++) {
        uint8_t b = p[0], g = p[1], rv = p[2], a = p[3];
        if (a == 0 && (rv|g|b)) a = 255;
        p[0]=rv; p[1]=g; p[2]=b; p[3]=a;
        p += 4;
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ICON_SIZE, ICON_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, bits);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return tex;
}

static GLuint load_logo_texture(HINSTANCE hInst) {
    const int sz = 64;
    HICON icon = (HICON)LoadImageW(hInst, L"IDI_APPICON", IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR);
    if (!icon) return 0;
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sz;
    bmi.bmiHeader.biHeight = -sz;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP old = (HBITMAP)SelectObject(dc, bmp);
    memset(bits, 0, sz * sz * 4);
    DrawIconEx(dc, 0, 0, icon, sz, sz, 0, NULL, DI_NORMAL);
    GdiFlush();
    /* Horizontally mirror the icon while converting BGRA -> RGBA. The source
       art has its chevron pointing the wrong way; flipping here gives the
       intended "X →" branding without needing to re-author the .ico. */
    uint8_t* out = (uint8_t*)malloc(sz * sz * 4);
    if (!out) { SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); DestroyIcon(icon); return 0; }
    uint8_t* src = (uint8_t*)bits;
    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            uint8_t* sp = src + (y * sz + x) * 4;
            uint8_t* dp = out + (y * sz + (sz - 1 - x)) * 4;
            uint8_t b = sp[0], g = sp[1], rv = sp[2], a = sp[3];
            if (a == 0 && (rv|g|b)) a = 255;
            dp[0] = rv; dp[1] = g; dp[2] = b; dp[3] = a;
        }
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
    free(out);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    DestroyIcon(icon);
    return tex;
}

static GLuint icon_cache_get(const char* key) {
    for (int i = 0; i < g_icon_count; i++)
        if (strcmp(g_icons[i].key, key) == 0) return g_icons[i].texture;
    return 0;
}

static GLuint icon_cache_put(const char* key, GLuint tex) {
    if (g_icon_count < MAX_ICON_CACHE) {
        strncpy(g_icons[g_icon_count].key, key, 23);
        g_icons[g_icon_count].texture = tex;
        g_icon_count++;
    }
    return tex;
}

static GLuint get_file_icon(const char* name, int is_dir) {
    char key[24] = {0};
    if (is_dir) {
        strcpy(key, "folder");
    } else {
        const char* dot = strrchr(name, '.');
        if (dot && strlen(dot) < 20) { strncpy(key, dot, 23); _strlwr(key); }
        else strcpy(key, ".file");
    }
    GLuint tex = icon_cache_get(key);
    if (tex) return tex;
    SHFILEINFOA sfi = {0};
    if (is_dir) {
        SHGetFileInfoA("folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    } else {
        char dummy[32];
        _snprintf(dummy, sizeof(dummy), "x%s", key);
        SHGetFileInfoA(dummy, FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    }
    if (!sfi.hIcon) return 0;
    tex = hicon_to_gl(sfi.hIcon);
    DestroyIcon(sfi.hIcon);
    return icon_cache_put(key, tex);
}

static GLuint get_path_icon(const char* path) {
    SHFILEINFOA sfi = {0};
    SHGetFileInfoA(path, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON);
    if (!sfi.hIcon) return get_file_icon("folder", 1);
    GLuint tex = hicon_to_gl(sfi.hIcon);
    DestroyIcon(sfi.hIcon);
    return tex;
}

static GLuint get_icon_by_pidl(LPITEMIDLIST pidl) {
    SHFILEINFOW sfi = {0};
    DWORD_PTR ok = SHGetFileInfoW((LPCWSTR)pidl, 0, &sfi, sizeof(sfi),
                                   SHGFI_ICON | SHGFI_SMALLICON | SHGFI_PIDL);
    if (!ok || !sfi.hIcon) return 0;
    GLuint tex = hicon_to_gl(sfi.hIcon);
    DestroyIcon(sfi.hIcon);
    return tex;
}

static GLuint get_special_folder_icon(int csidl) {
    LPITEMIDLIST pidl = NULL;
    if (SHGetSpecialFolderLocation(NULL, csidl, &pidl) != S_OK || !pidl) return 0;
    GLuint tex = get_icon_by_pidl(pidl);
    CoTaskMemFree(pidl);
    return tex;
}

static GLuint get_path_icon_shell(const char* path) {
    WCHAR wpath[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH)) return 0;
    PIDLIST_ABSOLUTE pidl = NULL;
    if (SHParseDisplayName(wpath, NULL, &pidl, 0, NULL) != S_OK || !pidl) return 0;
    GLuint tex = get_icon_by_pidl(pidl);
    CoTaskMemFree(pidl);
    return tex;
}

/* ---- Helpers ---- */
static void format_size(ULONGLONG bytes, char* buf, int n) {
    if (bytes < 1024) _snprintf(buf, n, "%llu B", bytes);
    else if (bytes < 1024*1024) _snprintf(buf, n, "%.1f KB", bytes/1024.0);
    else if (bytes < 1024ULL*1024*1024) _snprintf(buf, n, "%.1f MB", bytes/(1024.0*1024));
    else _snprintf(buf, n, "%.1f GB", bytes/(1024.0*1024*1024));
}

static void format_time(FILETIME ft, char* buf, int n) {
    FILETIME lft; SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    _snprintf(buf, n, "%d/%02d/%04d %d:%02d %s",
              st.wMonth, st.wDay, st.wYear,
              st.wHour > 12 ? st.wHour-12 : (st.wHour?st.wHour:12),
              st.wMinute, st.wHour >= 12 ? "PM" : "AM");
}

static void get_tab_title(const char* path, char* title, int n) {
    const char* last = strrchr(path, '\\');
    if (last && last[1]) _snprintf(title, n, "%s", last + 1);
    else _snprintf(title, n, "(%c:)", path[0]);
}

static void shorten_path(const char* full, char* out, int max_chars) {
    const char* disp = full;
    char buf[MAX_PATH];
    if (_strnicmp(full, g_user_profile, strlen(g_user_profile)) == 0) {
        const char* rel = full + strlen(g_user_profile);
        if (*rel == '\\') rel++;
        _snprintf(buf, sizeof(buf), "user\\%s", rel);
        disp = buf;
    }
    int len = (int)strlen(disp);
    if (len <= max_chars) { strcpy(out, disp); }
    else { strncpy(out, disp, max_chars - 3); out[max_chars-3] = 0; strcat(out, "..."); }
}

/* ---- Sort ---- */
static int g_sort_col_cmp, g_sort_asc_cmp, g_use_groups_cmp;
static int compare_entries(const void* a, const void* b) {
    const FileEntry* ea = (const FileEntry*)a;
    const FileEntry* eb = (const FileEntry*)b;
    int parent_a = (strcmp(ea->name, "..") == 0);
    int parent_b = (strcmp(eb->name, "..") == 0);
    if (parent_a != parent_b) return parent_b - parent_a;
    if (g_use_groups_cmp) {
        if (ea->group != eb->group) return ea->group - eb->group;
    } else if (ea->is_dir != eb->is_dir) {
        return eb->is_dir - ea->is_dir;
    }
    int r = 0;
    switch (g_sort_col_cmp) {
    case 0: r = _stricmp(ea->name, eb->name); break;
    case 1: r = _stricmp(ea->name, eb->name); break;
    case 2: r = CompareFileTime(&ea->modified, &eb->modified); break;
    case 5: r = (ea->size > eb->size) - (ea->size < eb->size); break;
    default: r = _stricmp(ea->name, eb->name); break;
    }
    return g_sort_asc_cmp ? r : -r;
}

/* ---- Directory scanning ---- */
static void scan_directory(Tab* tab) {
    /* Save UI state by NAME so refresh/auto-watcher doesn't lose selection */
    char saved_sel_name[MAX_PATH] = {0};
    char saved_anchor_name[MAX_PATH] = {0};
    if (tab->selected >= 0 && tab->selected < tab->entry_count)
        strncpy(saved_sel_name, tab->entries[tab->selected].name, MAX_PATH-1);
    if (tab->sel_anchor >= 0 && tab->sel_anchor < tab->entry_count)
        strncpy(saved_anchor_name, tab->entries[tab->sel_anchor].name, MAX_PATH-1);
    int n_sel = 0;
    char (*saved_names)[MAX_PATH] = NULL;
    for (int i = 0; i < tab->entry_count; i++) if (tab->sel_mask[i]) n_sel++;
    if (n_sel > 0) {
        saved_names = (char(*)[MAX_PATH])calloc(n_sel, MAX_PATH);
        int k = 0;
        for (int i = 0; i < tab->entry_count; i++)
            if (tab->sel_mask[i])
                strncpy(saved_names[k++], tab->entries[i].name, MAX_PATH-1);
    }
    float saved_target_scroll = tab->target_scroll;

    tab->entry_count = 0;
    tab->selected = -1;
    tab->sel_anchor = -1;
    memset(tab->sel_mask, 0, sizeof(tab->sel_mask));
    int is_downloads = (g_downloads_path[0] && path_eq_ci(tab->path, g_downloads_path));
    tab->view_mode   = view_prefs_lookup(tab->path);
    refresh_today();
    WCHAR wpattern[MAX_PATH + 4];
    {
        char pattern[MAX_PATH + 4];
        _snprintf(pattern, sizeof(pattern), "%s\\*", tab->path);
        u8_to_w(pattern, wpattern, MAX_PATH + 4);
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == 0) continue;
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.' && fd.cFileName[2] == 0) continue;
        if (tab->entry_count >= MAX_ENTRIES) break;
        FileEntry* e = &tab->entries[tab->entry_count++];
        w_to_u8(fd.cFileName, e->name, MAX_PATH);
        e->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e->size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e->modified = fd.ftLastWriteTime;
        e->group = compute_group(e->modified);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    /* Effective sort — respects the user's per-folder preference for
       every folder including Downloads. Date-groups only kick in on
       Downloads when the sort is date-modified descending (the natural
       "recent activity" arrangement). Any other sort (name, size, or
       ascending date) turns groups off and behaves like any other
       folder. */
    {
        int sc = g_app.sort_col, sa = g_app.sort_asc;
        if (!sort_prefs_lookup(tab->path, &sc, &sa) && is_downloads) {
            /* First visit to Downloads: default to date desc so the
               familiar date-grouped view shows up. */
            sc = 2; sa = 0;
        }
        g_app.sort_col = sc;
        g_app.sort_asc = sa;
        tab->use_groups = is_downloads && (sc == 2 && sa == 0);
        g_sort_col_cmp   = sc;
        g_sort_asc_cmp   = sa;
        g_use_groups_cmp = tab->use_groups;
    }
    qsort(tab->entries, tab->entry_count, sizeof(FileEntry), compare_entries);

    /* Restore selection by name match */
    if (saved_names) {
        for (int k = 0; k < n_sel; k++)
            for (int i = 0; i < tab->entry_count; i++)
                if (strcmp(tab->entries[i].name, saved_names[k]) == 0) {
                    tab->sel_mask[i] = 1; break;
                }
        free(saved_names);
    }
    if (saved_sel_name[0])
        for (int i = 0; i < tab->entry_count; i++)
            if (strcmp(tab->entries[i].name, saved_sel_name) == 0) {
                tab->selected = i; break;
            }
    if (saved_anchor_name[0])
        for (int i = 0; i < tab->entry_count; i++)
            if (strcmp(tab->entries[i].name, saved_anchor_name) == 0) {
                tab->sel_anchor = i; break;
            }
    tab->target_scroll = saved_target_scroll;

    get_tab_title(tab->path, tab->title, sizeof(tab->title));
    g_needs_redraw = 1;
}

/* ---- Navigation ---- */
static void tab_navigate(Tab* tab, const char* path, int add_hist) {
    char norm[MAX_PATH];
    strncpy(norm, path, MAX_PATH-1); norm[MAX_PATH-1] = 0;
    int nlen = (int)strlen(norm);
    if (nlen > 3 && norm[nlen-1] == '\\') norm[nlen-1] = 0;
    if (!add_hist || tab->hist_count == 0) {
        /* Seed: history becomes just [norm], pos=0. */
        strncpy(tab->history[0], norm, MAX_PATH-1);
        tab->history[0][MAX_PATH-1] = 0;
        tab->hist_count = 1;
        tab->hist_pos = 0;
    } else {
        /* Skip if already on this path (avoid bogus duplicate history entries) */
        if (strcmp(tab->history[tab->hist_pos], norm) != 0) {
            /* Truncate forward stack */
            tab->hist_count = tab->hist_pos + 1;
            if (tab->hist_count < MAX_HIST) {
                strncpy(tab->history[tab->hist_count], norm, MAX_PATH-1);
                tab->history[tab->hist_count][MAX_PATH-1] = 0;
                tab->hist_pos = tab->hist_count;
                tab->hist_count++;
            } else {
                /* Full: shift left, keep newest */
                for (int i = 0; i < MAX_HIST - 1; i++)
                    memcpy(tab->history[i], tab->history[i+1], MAX_PATH);
                strncpy(tab->history[MAX_HIST-1], norm, MAX_PATH-1);
                tab->history[MAX_HIST-1][MAX_PATH-1] = 0;
                tab->hist_pos = MAX_HIST - 1;
            }
        }
    }
    strncpy(tab->path, norm, MAX_PATH-1);
    tab->path[MAX_PATH-1] = 0;
    /* Navigation: reset UI state so scan_directory's name-preserving logic
       starts with a clean slate (no leaks from old folder). */
    tab->selected = -1;
    tab->sel_anchor = -1;
    memset(tab->sel_mask, 0, sizeof(tab->sel_mask));
    tab->target_scroll = 0;
    tab->scroll_y = 0;
    scan_directory(tab);
    watch_start(tab->path);
    tabs_save();
}

static void tab_go_up(Tab* tab) {
    char parent[MAX_PATH], leaf[MAX_PATH] = {0};
    strncpy(parent, tab->path, MAX_PATH);
    const char* slash = strrchr(parent, '\\');
    if (slash && slash[1]) strncpy(leaf, slash + 1, MAX_PATH - 1);
    PathRemoveFileSpecA(parent);
    if (strlen(parent) < 2) return;
    tab_navigate(tab, parent, 1);
    if (leaf[0]) {
        for (int i = 0; i < tab->entry_count; i++) {
            if (strcmp(tab->entries[i].name, leaf) == 0) {
                tab->selected = i;
                tab->sel_anchor = i;
                tab->sel_mask[i] = 1;
                scroll_to_entry(tab, i);
                break;
            }
        }
    }
}

static void tab_go_back(Tab* tab) {
    if (tab->hist_pos > 0) {
        tab->hist_pos--;
        strncpy(tab->path, tab->history[tab->hist_pos], MAX_PATH-1);
        tab->path[MAX_PATH-1] = 0;
        tab->selected = -1;
        tab->sel_anchor = -1;
        memset(tab->sel_mask, 0, sizeof(tab->sel_mask));
        tab->target_scroll = 0;
        tab->scroll_y = 0;
        scan_directory(tab);
        watch_start(tab->path);
        tabs_save();
    }
}

static void tab_go_forward(Tab* tab) {
    if (tab->hist_pos + 1 < tab->hist_count) {
        tab->hist_pos++;
        strncpy(tab->path, tab->history[tab->hist_pos], MAX_PATH-1);
        tab->path[MAX_PATH-1] = 0;
        tab->selected = -1;
        tab->sel_anchor = -1;
        memset(tab->sel_mask, 0, sizeof(tab->sel_mask));
        tab->target_scroll = 0;
        tab->scroll_y = 0;
        scan_directory(tab);
        watch_start(tab->path);
        tabs_save();
    }
}

static Tab* active_tab(void) { return &g_app.panels[g_app.active_panel].tabs[g_app.panels[g_app.active_panel].active_tab]; }

static void new_tab(const char* path) {
    if (g_app.panels[g_app.active_panel].tab_count >= MAX_TABS) return;
    Tab* t = &g_app.panels[g_app.active_panel].tabs[g_app.panels[g_app.active_panel].tab_count];
    memset(t, 0, sizeof(Tab));
    g_app.panels[g_app.active_panel].active_tab = g_app.panels[g_app.active_panel].tab_count++;
    tab_navigate(t, path, 0);
    tabs_save();
}

static void close_tab(int idx) {
    if (g_app.panels[g_app.active_panel].tab_count <= 1) return;
    for (int i = idx; i < g_app.panels[g_app.active_panel].tab_count - 1; i++)
        g_app.panels[g_app.active_panel].tabs[i] = g_app.panels[g_app.active_panel].tabs[i+1];
    g_app.panels[g_app.active_panel].tab_count--;
    if (g_app.panels[g_app.active_panel].active_tab >= g_app.panels[g_app.active_panel].tab_count) g_app.panels[g_app.active_panel].active_tab = g_app.panels[g_app.active_panel].tab_count - 1;
    tabs_save();
    g_needs_redraw = 1;
}

static void tabs_load(const char* fallback) {
    char fp[MAX_PATH];
    app_data_file("tabs.txt", fp, MAX_PATH);
    FILE* f = u8_fopen(fp, "rb");
    int saved_active0 = 0, saved_active1 = 0;
    int saved_split = 0, saved_active_panel = 0;
    float saved_ratio = 0.5f;
    int section = 0;
    if (f) {
        char line[MAX_PATH];
        while (fgets(line, sizeof(line), f)) {
            int n = (int)strlen(line);
            while (n > 0 && (line[n-1]=='\n' || line[n-1]=='\r' || line[n-1]==' ')) line[--n] = 0;
            if (n == 0) continue;
            if (strncmp(line, "active=", 7) == 0) { saved_active0 = atoi(line + 7); continue; }
            if (strncmp(line, "split_active=", 13) == 0) { saved_split = atoi(line + 13); continue; }
            if (strncmp(line, "split_ratio=", 12) == 0) { saved_ratio = (float)atof(line + 12); continue; }
            if (strncmp(line, "active_panel=", 13) == 0) { saved_active_panel = atoi(line + 13); continue; }
            if (strncmp(line, "panel1_active=", 14) == 0) {
                saved_active1 = atoi(line + 14);
                section = 1;
                continue;
            }
            {
                WCHAR wline[MAX_PATH]; u8_to_w(line, wline, MAX_PATH);
                if (GetFileAttributesW(wline) != INVALID_FILE_ATTRIBUTES) {
                    int saved_p = g_app.active_panel;
                    g_app.active_panel = section;
                    new_tab(line);
                    g_app.active_panel = saved_p;
                }
            }
        }
        fclose(f);
    }
    /* Ensure panel 0 has at least one tab */
    if (g_app.panels[0].tab_count == 0) {
        int sp = g_app.active_panel;
        g_app.active_panel = 0;
        new_tab(fallback);
        g_app.active_panel = sp;
    }
    if (saved_active0 >= 0 && saved_active0 < g_app.panels[0].tab_count)
        g_app.panels[0].active_tab = saved_active0;
    if (saved_active1 >= 0 && saved_active1 < g_app.panels[1].tab_count)
        g_app.panels[1].active_tab = saved_active1;
    g_app.split_active = saved_split;
    g_app.split_ratio  = (saved_ratio > 0.1f && saved_ratio < 0.9f) ? saved_ratio : 0.5f;
    if (saved_active_panel == 0 || saved_active_panel == 1)
        g_app.active_panel = saved_active_panel;
    tabs_save();
}

/* ---- Sidebar init ---- */
#define BOOKMARKS_SECTION 0

static void add_sb(SidebarSection* s, const char* name, const char* path,
                   const char* sp, uint32_t col) {
    if (s->item_count >= 16) return;
    SidebarItem* it = &s->items[s->item_count++];
    strncpy(it->name, name, sizeof(it->name)-1);
    strncpy(it->path, path, sizeof(it->path)-1);
    strncpy(it->short_path, sp, sizeof(it->short_path)-1);
    it->icon_color = col;
}

static void app_data_file(const char* name, char* out, int n) {
    WCHAR wappdata[MAX_PATH] = {0};
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, wappdata);
    char appdata[MAX_PATH] = {0};
    w_to_u8(wappdata, appdata, MAX_PATH);
    _snprintf(out, n, "%s\\filepathx", appdata);
    {
        WCHAR wdir[MAX_PATH];
        u8_to_w(out, wdir, MAX_PATH);
        CreateDirectoryW(wdir, NULL);
    }
    _snprintf(out, n, "%s\\filepathx\\%s", appdata, name);
}

/* fopen wrapper that handles UTF-8 paths via _wfopen */
static FILE* u8_fopen(const char* path, const char* mode) {
    WCHAR wpath[MAX_PATH], wmode[16];
    u8_to_w(path, wpath, MAX_PATH);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16);
    return _wfopen(wpath, wmode);
}

static void bookmarks_filepath(char* out, int n) {
    app_data_file("bookmarks.txt", out, n);
}

static void tabs_save(void) {
    char fp[MAX_PATH];
    app_data_file("tabs.txt", fp, MAX_PATH);
    FILE* f = u8_fopen(fp, "wb");
    if (!f) return;
    fprintf(f, "active=%d\n", g_app.panels[0].active_tab);
    for (int i = 0; i < g_app.panels[0].tab_count; i++)
        fprintf(f, "%s\n", g_app.panels[0].tabs[i].path);
    if (g_app.split_active || g_app.panels[1].tab_count > 0) {
        fprintf(f, "split_active=%d\n", g_app.split_active);
        fprintf(f, "split_ratio=%.3f\n", g_app.split_ratio);
        fprintf(f, "active_panel=%d\n", g_app.active_panel);
        fprintf(f, "panel1_active=%d\n", g_app.panels[1].active_tab);
        for (int i = 0; i < g_app.panels[1].tab_count; i++)
            fprintf(f, "%s\n", g_app.panels[1].tabs[i].path);
    }
    fclose(f);
}

/* ---- Per-folder sort preferences (LRU, bounded) ---- */
#define MAX_SORT_PREFS 200
typedef struct { char path[MAX_PATH]; char col; char asc; } SortPref;
static SortPref g_sort_prefs[MAX_SORT_PREFS];
static int      g_sort_pref_count = 0;

static int sort_prefs_find(const char* path) {
    for (int i = 0; i < g_sort_pref_count; i++)
        if (_stricmp(g_sort_prefs[i].path, path) == 0) return i;
    return -1;
}

static void sort_prefs_move_to_front(int idx) {
    if (idx <= 0) return;
    SortPref tmp = g_sort_prefs[idx];
    memmove(&g_sort_prefs[1], &g_sort_prefs[0], idx * sizeof(SortPref));
    g_sort_prefs[0] = tmp;
}

static int sort_prefs_lookup(const char* path, int* out_col, int* out_asc) {
    int idx = sort_prefs_find(path);
    if (idx < 0) return 0;
    *out_col = g_sort_prefs[idx].col;
    *out_asc = g_sort_prefs[idx].asc;
    sort_prefs_move_to_front(idx);
    return 1;
}

static void sort_prefs_save(void) {
    char fp[MAX_PATH];
    app_data_file("sort.txt", fp, MAX_PATH);
    FILE* f = u8_fopen(fp, "wb");
    if (!f) return;
    for (int i = 0; i < g_sort_pref_count; i++)
        fprintf(f, "%d|%d|%s\n", g_sort_prefs[i].col, g_sort_prefs[i].asc, g_sort_prefs[i].path);
    fclose(f);
}

static void sort_prefs_set(const char* path, int col, int asc) {
    int idx = sort_prefs_find(path);
    if (idx >= 0) {
        g_sort_prefs[idx].col = (char)col;
        g_sort_prefs[idx].asc = (char)asc;
        sort_prefs_move_to_front(idx);
    } else {
        int keep = g_sort_pref_count;
        if (keep >= MAX_SORT_PREFS) keep = MAX_SORT_PREFS - 1; /* evict last */
        memmove(&g_sort_prefs[1], &g_sort_prefs[0], keep * sizeof(SortPref));
        g_sort_pref_count = keep + 1;
        strncpy(g_sort_prefs[0].path, path, MAX_PATH-1);
        g_sort_prefs[0].path[MAX_PATH-1] = 0;
        g_sort_prefs[0].col = (char)col;
        g_sort_prefs[0].asc = (char)asc;
    }
    sort_prefs_save();
}

static void sort_prefs_load(void) {
    char fp[MAX_PATH];
    app_data_file("sort.txt", fp, MAX_PATH);
    FILE* f = u8_fopen(fp, "rb");
    if (!f) return;
    char line[MAX_PATH + 16];
    while (fgets(line, sizeof(line), f) && g_sort_pref_count < MAX_SORT_PREFS) {
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1]=='\n' || line[n-1]=='\r')) line[--n] = 0;
        char* p1 = strchr(line, '|'); if (!p1) continue;
        char* p2 = strchr(p1+1, '|'); if (!p2) continue;
        *p1 = 0; *p2 = 0;
        SortPref* sp = &g_sort_prefs[g_sort_pref_count++];
        sp->col = (char)atoi(line);
        sp->asc = (char)atoi(p1 + 1);
        strncpy(sp->path, p2 + 1, MAX_PATH-1); sp->path[MAX_PATH-1] = 0;
    }
    fclose(f);
}

/* ---- Theme (INI-style, key=6- or 8-hex value) ---- */
static uint32_t parse_hex_color(const char* s) {
    /* Accept "RGB", "RRGGBB", or "AARRGGBB". Ignore leading #. */
    if (*s == '#') s++;
    int len = 0;
    while (s[len] && ((s[len] >= '0' && s[len] <= '9') ||
                      (s[len] >= 'a' && s[len] <= 'f') ||
                      (s[len] >= 'A' && s[len] <= 'F'))) len++;
    unsigned int v = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        int d = (c <= '9') ? c - '0' : ((c & ~0x20) - 'A' + 10);
        v = (v << 4) | (unsigned)d;
    }
    if (len == 3) {
        unsigned int r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
        return 0xFF000000 | ((r*17) << 16) | ((g*17) << 8) | (b*17);
    }
    if (len == 6) return 0xFF000000 | (v & 0xFFFFFF);
    if (len == 8) return v;
    return 0xFFFF00FF;   /* obvious magenta for parse errors */
}

/* ---- Theme discovery ---- */
static void theme_load(void);   /* forward decl for theme_apply_file below */
#define MAX_THEMES  32
typedef struct {
    char display[64];    /* human-readable name from `name=` line, or filename */
    char path[MAX_PATH]; /* absolute path to .ini */
} ThemeEntry;
static ThemeEntry g_themes[MAX_THEMES];
static int g_theme_count = 0;
static char g_theme_current[64] = "Catppuccin Mocha";

static void theme_read_display_name(const char* path, char* out, int n) {
    out[0] = 0;
    FILE* f = u8_fopen(path, "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1]=='\n' || line[len-1]=='\r' ||
                           line[len-1]==' '  || line[len-1]=='\t')) line[--len] = 0;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == ';' || *p == '#') continue;
        /* case-insensitive match "name" */
        if ((p[0]=='n'||p[0]=='N') && (p[1]=='a'||p[1]=='A') &&
            (p[2]=='m'||p[2]=='M') && (p[3]=='e'||p[3]=='E')) {
            char* eq = strchr(p, '=');
            if (eq) {
                eq++; while (*eq==' '||*eq=='\t') eq++;
                strncpy(out, eq, n-1); out[n-1] = 0;
                break;
            }
        }
    }
    fclose(f);
}

static void theme_discover(void) {
    g_theme_count = 0;
    WCHAR wexe[MAX_PATH];
    if (GetModuleFileNameW(NULL, wexe, MAX_PATH) == 0) return;
    /* Strip filename */
    for (int i = (int)wcslen(wexe) - 1; i >= 0; i--) {
        if (wexe[i] == L'\\' || wexe[i] == L'/') { wexe[i] = 0; break; }
    }
    WCHAR wpat[MAX_PATH];
    _snwprintf(wpat, MAX_PATH, L"%ls\\themes\\*.ini", wexe);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        /* Try one level up (source-tree layout: build\FilePathX.exe → repo\themes) */
        _snwprintf(wpat, MAX_PATH, L"%ls\\..\\themes\\*.ini", wexe);
        h = FindFirstFileW(wpat, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        /* update base to parent for the loop below */
        for (int i = (int)wcslen(wexe) - 1; i >= 0; i--) {
            if (wexe[i] == L'\\' || wexe[i] == L'/') { wexe[i] = 0; break; }
        }
    }
    do {
        if (g_theme_count >= MAX_THEMES) break;
        WCHAR wfull[MAX_PATH];
        _snwprintf(wfull, MAX_PATH, L"%ls\\themes\\%ls", wexe, fd.cFileName);
        char full[MAX_PATH];
        w_to_u8(wfull, full, MAX_PATH);
        char disp[64] = {0};
        theme_read_display_name(full, disp, sizeof(disp));
        if (!disp[0]) {
            /* Fallback: filename without .ini */
            char fnu8[128];
            w_to_u8(fd.cFileName, fnu8, sizeof(fnu8));
            int fl = (int)strlen(fnu8);
            if (fl > 4 && (fnu8[fl-4]=='.')) fnu8[fl-4] = 0;
            strncpy(disp, fnu8, sizeof(disp)-1);
        }
        ThemeEntry* t = &g_themes[g_theme_count++];
        strncpy(t->display, disp, sizeof(t->display)-1); t->display[sizeof(t->display)-1] = 0;
        strncpy(t->path, full, MAX_PATH-1);              t->path[MAX_PATH-1] = 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Copy a theme file into %APPDATA%\filepathx\theme.ini, then reload. */
static void theme_apply_file(const char* src_path) {
    char dst[MAX_PATH];
    app_data_file("theme.ini", dst, MAX_PATH);
    WCHAR wsrc[MAX_PATH], wdst[MAX_PATH];
    u8_to_w(src_path, wsrc, MAX_PATH);
    u8_to_w(dst, wdst, MAX_PATH);
    CopyFileW(wsrc, wdst, FALSE);
    /* Remember the display name so the sidebar dropdown reflects it */
    theme_read_display_name(src_path, g_theme_current, sizeof(g_theme_current));
    theme_load();
    g_needs_redraw = 1;
}

static void theme_load(void) {
    theme_reset_defaults();
    char fp[MAX_PATH];
    app_data_file("theme.ini", fp, MAX_PATH);
    /* Refresh display name from the active file */
    {
        char nm[64] = {0};
        theme_read_display_name(fp, nm, sizeof(nm));
        if (nm[0]) { strncpy(g_theme_current, nm, sizeof(g_theme_current)-1);
                     g_theme_current[sizeof(g_theme_current)-1] = 0; }
        else strcpy(g_theme_current, "Default");
    }
    FILE* f = u8_fopen(fp, "rb");
    if (!f) { strcpy(g_theme_current, "Default"); return; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* Trim ends and comments */
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1]=='\n' || line[n-1]=='\r' ||
                         line[n-1]==' '  || line[n-1]=='\t')) line[--n] = 0;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == ';' || *p == '#' || *p == '[') continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = p;
        char* val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        /* Trim trailing space on key */
        int kl = (int)strlen(key);
        while (kl > 0 && (key[kl-1]==' ' || key[kl-1]=='\t')) key[--kl] = 0;
        /* Case-insensitive key compare */
        for (int i = 0; key[i]; i++)
            if (key[i] >= 'A' && key[i] <= 'Z') key[i] += 32;

        uint32_t c = parse_hex_color(val);
        /* Support tweakcn / shadcn style names AND our internal names. */
        if      (!strcmp(key, "background") || !strcmp(key, "bg"))        g_theme.bg = c;
        else if (!strcmp(key, "foreground") || !strcmp(key, "text"))      g_theme.text = c;
        else if (!strcmp(key, "muted-foreground") || !strcmp(key, "subtext")) g_theme.subtext = c;
        else if (!strcmp(key, "dim"))                                     g_theme.dim = c;
        else if (!strcmp(key, "primary") || !strcmp(key, "accent"))       g_theme.accent = c;
        else if (!strcmp(key, "card") || !strcmp(key, "surface"))         g_theme.surface = c;
        else if (!strcmp(key, "popover") || !strcmp(key, "header"))       g_theme.header = c;
        else if (!strcmp(key, "sidebar") || !strcmp(key, "mantle"))       g_theme.mantle = c;
        else if (!strcmp(key, "muted") || !strcmp(key, "hover"))          g_theme.hover = c;
        else if (!strcmp(key, "selected") || !strcmp(key, "selection"))   g_theme.selected = c;
        else if (!strcmp(key, "border") || !strcmp(key, "input"))         g_theme.border = c;
        else if (!strcmp(key, "ring") || !strcmp(key, "overlay"))         g_theme.overlay = c;
        else if (!strcmp(key, "scrollbar"))                               g_theme.scrollbar = c;
        else if (!strcmp(key, "destructive") || !strcmp(key, "red"))      g_theme.red = c;
        else if (!strcmp(key, "chart-2") || !strcmp(key, "green"))        g_theme.green = c;
        else if (!strcmp(key, "chart-1") || !strcmp(key, "yellow"))       g_theme.yellow = c;
        else if (!strcmp(key, "chart-3") || !strcmp(key, "peach"))        g_theme.peach = c;
        /* unknown keys ignored on purpose so tweakcn exports work as-is */
    }
    fclose(f);
    /* Update DWM title bar brightness so it matches the new theme. */
    if (g_hwnd) {
        uint32_t c = COL_BG;
        int lum = (77 * ((c>>16)&0xFF) + 150 * ((c>>8)&0xFF) + 29 * (c&0xFF)) >> 8;
        BOOL dark = (lum < 128);
        DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
    g_needs_redraw = 1;
}

/* ---- Per-folder view mode preferences ---- */
#define MAX_VIEW_PREFS 200
typedef struct { char path[MAX_PATH]; char mode; } ViewPref;
static ViewPref g_view_prefs[MAX_VIEW_PREFS];
static int      g_view_pref_count = 0;

static int view_prefs_find(const char* path) {
    for (int i = 0; i < g_view_pref_count; i++)
        if (_stricmp(g_view_prefs[i].path, path) == 0) return i;
    return -1;
}
static void view_prefs_move_to_front(int idx) {
    if (idx <= 0) return;
    ViewPref tmp = g_view_prefs[idx];
    memmove(&g_view_prefs[1], &g_view_prefs[0], idx * sizeof(ViewPref));
    g_view_prefs[0] = tmp;
}
static int view_prefs_lookup(const char* path) {
    int idx = view_prefs_find(path);
    if (idx < 0) return VM_DETAILS;
    int m = g_view_prefs[idx].mode;
    view_prefs_move_to_front(idx);
    return m;
}
static void view_prefs_save(void) {
    char fp[MAX_PATH];
    app_data_file("view.txt", fp, MAX_PATH);
    FILE* f = u8_fopen(fp, "wb");
    if (!f) return;
    for (int i = 0; i < g_view_pref_count; i++)
        fprintf(f, "%d|%s\n", g_view_prefs[i].mode, g_view_prefs[i].path);
    fclose(f);
}
static void view_prefs_set(const char* path, int mode) {
    int idx = view_prefs_find(path);
    if (idx >= 0) {
        g_view_prefs[idx].mode = (char)mode;
        view_prefs_move_to_front(idx);
    } else {
        int keep = g_view_pref_count;
        if (keep >= MAX_VIEW_PREFS) keep = MAX_VIEW_PREFS - 1;
        memmove(&g_view_prefs[1], &g_view_prefs[0], keep * sizeof(ViewPref));
        g_view_pref_count = keep + 1;
        strncpy(g_view_prefs[0].path, path, MAX_PATH-1);
        g_view_prefs[0].path[MAX_PATH-1] = 0;
        g_view_prefs[0].mode = (char)mode;
    }
    view_prefs_save();
}
static void view_prefs_load(void) {
    char fp[MAX_PATH];
    app_data_file("view.txt", fp, MAX_PATH);
    FILE* f = u8_fopen(fp, "rb");
    if (!f) return;
    char line[MAX_PATH + 16];
    while (fgets(line, sizeof(line), f) && g_view_pref_count < MAX_VIEW_PREFS) {
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1]=='\n' || line[n-1]=='\r')) line[--n] = 0;
        char* p1 = strchr(line, '|'); if (!p1) continue;
        *p1 = 0;
        ViewPref* vp = &g_view_prefs[g_view_pref_count++];
        vp->mode = (char)atoi(line);
        strncpy(vp->path, p1 + 1, MAX_PATH-1); vp->path[MAX_PATH-1] = 0;
    }
    fclose(f);
}

static void bookmarks_save(void) {
    char fp[MAX_PATH];
    bookmarks_filepath(fp, MAX_PATH);
    FILE* f = u8_fopen(fp, "wb");
    if (!f) return;
    SidebarSection* s = &g_app.sections[BOOKMARKS_SECTION];
    for (int i = 0; i < s->item_count; i++)
        fprintf(f, "%s\n", s->items[i].path);
    fclose(f);
}

static int bookmark_find(const char* path) {
    SidebarSection* s = &g_app.sections[BOOKMARKS_SECTION];
    for (int i = 0; i < s->item_count; i++)
        if (_stricmp(s->items[i].path, path) == 0) return i;
    return -1;
}

static void bookmark_add(const char* path) {
    if (bookmark_find(path) >= 0) return;
    SidebarSection* s = &g_app.sections[BOOKMARKS_SECTION];
    const char* nm = strrchr(path, '\\');
    nm = (nm && nm[1]) ? nm + 1 : path;
    char shortp[24] = {0};
    strncpy(shortp, path, sizeof(shortp)-1);
    add_sb(s, nm, path, shortp, COL_YELLOW);
    SidebarItem* it = &s->items[s->item_count - 1];
    it->icon_tex = get_path_icon(path);
    bookmarks_save();
}

static void bookmark_remove_at(int idx) {
    SidebarSection* s = &g_app.sections[BOOKMARKS_SECTION];
    if (idx < 0 || idx >= s->item_count) return;
    for (int i = idx; i < s->item_count - 1; i++) s->items[i] = s->items[i+1];
    s->item_count--;
    bookmarks_save();
}

static void bookmarks_load(void) {
    char fp[MAX_PATH];
    bookmarks_filepath(fp, MAX_PATH);
    FILE* f = u8_fopen(fp, "rb");
    if (!f) return;
    SidebarSection* s = &g_app.sections[BOOKMARKS_SECTION];
    char line[MAX_PATH];
    while (fgets(line, sizeof(line), f)) {
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' ')) line[--n] = 0;
        if (n == 0) continue;
        const char* nm = strrchr(line, '\\');
        nm = (nm && nm[1]) ? nm + 1 : line;
        char shortp[24] = {0};
        strncpy(shortp, line, sizeof(shortp)-1);
        add_sb(s, nm, line, shortp, COL_YELLOW);
        s->items[s->item_count - 1].icon_tex = get_path_icon(line);
    }
    fclose(f);
}

/* Hit-test sidebar: returns 1 if (mx,my) is on a bookmark item, sets *out_idx */
static int sidebar_hit_bookmark(int mx, int my, int* out_idx) {
    if (mx < 0 || mx >= SIDEBAR_W) return 0;
    float sy = TAB_BAR_H + 4;
    for (int s = 0; s < g_app.section_count; s++) {
        SidebarSection* sec = &g_app.sections[s];
        sy += 22;
        if (!sec->expanded) continue;
        for (int i = 0; i < sec->item_count; i++) {
            if (my >= sy && my < sy + 21) {
                if (s == BOOKMARKS_SECTION) { *out_idx = i; return 1; }
                return 0;
            }
            sy += 21;
        }
        sy += 4;
    }
    return 0;
}

#define STORAGE_SECTION 1

/* Repopulate the Storage section from the current set of mounted drives.
   Called at init and whenever WM_DEVICECHANGE fires so a freshly-plugged
   USB / external drive shows up without restarting the app. */
static void refresh_storage_drives(void) {
    SidebarSection* s = &g_app.sections[STORAGE_SECTION];
    s->item_count = 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        char root[8];
        _snprintf(root, sizeof(root), "%c:\\", 'A' + i);
        UINT dt = GetDriveTypeA(root);
        if (dt == DRIVE_NO_ROOT_DIR || dt == DRIVE_UNKNOWN) continue;
        /* Use the volume label when available, fall back to drive letter. */
        char label[64] = {0};
        WCHAR wroot[8]; u8_to_w(root, wroot, 8);
        WCHAR wlabel[64] = {0};
        if (GetVolumeInformationW(wroot, wlabel, 64, NULL, NULL, NULL, NULL, 0) && wlabel[0])
            w_to_u8(wlabel, label, sizeof(label));
        char name[96], shortp[8];
        const char* prefix =
            (dt == DRIVE_REMOVABLE) ? "Removable" :
            (dt == DRIVE_CDROM)     ? "CD/DVD"    :
            (dt == DRIVE_REMOTE)    ? "Network"   :
                                      "Local Disk";
        if (label[0]) _snprintf(name, sizeof(name), "%s (%c:)", label, 'A' + i);
        else          _snprintf(name, sizeof(name), "%s (%c:)", prefix, 'A' + i);
        _snprintf(shortp, sizeof(shortp), "%c:", 'A' + i);
        add_sb(s, name, root, shortp, COL_ACCENT);
        s->items[s->item_count - 1].icon_tex = get_path_icon(root);
    }
    g_needs_redraw = 1;
}

static void init_sidebar(void) {
    SidebarSection* s;

    s = &g_app.sections[0];
    strcpy(s->title, "Bookmarks"); s->expanded = 1;
    bookmarks_load();

    s = &g_app.sections[1];
    strcpy(s->title, "Storage"); s->expanded = 1;
    refresh_storage_drives();

    s = &g_app.sections[2];
    strcpy(s->title, "Places"); s->expanded = 1;
    add_sb(s, "This PC", "C:\\", "", COL_ACCENT);
    {
        char p[MAX_PATH], sp[24];
        add_sb(s, "user", g_user_profile, "C:\\Users\\user", COL_YELLOW);
        const char* folders[] = {"Desktop","Downloads","Documents","Music","Pictures","Videos"};
        uint32_t fcols[] = {COL_YELLOW, COL_ACCENT, COL_ACCENT, COL_GREEN, COL_GREEN, COL_PEACH};
        for (int i = 0; i < 6; i++) {
            _snprintf(p, MAX_PATH, "%s\\%s", g_user_profile, folders[i]);
            shorten_path(p, sp, 18);
            add_sb(s, folders[i], p, sp, fcols[i]);
        }
        add_sb(s, "Recycle Bin", "shell:RecycleBinFolder", "", COL_ACCENT);
        s->items[s->item_count - 1].icon_tex = get_special_folder_icon(CSIDL_BITBUCKET);
    }
    g_app.section_count = 3;

    /* Load icons. Places use PIDL shell icons; This PC uses CSIDL_DRIVES.
       Bookmarks/Storage use simple path icons (drives don't need shell namespace). */
    for (int si = 0; si < g_app.section_count; si++) {
        SidebarSection* sec = &g_app.sections[si];
        int is_places = (si == 2);
        for (int i = 0; i < sec->item_count; i++) {
            SidebarItem* it = &sec->items[i];
            if (it->icon_tex) continue; /* pre-set (e.g., Recycle Bin) */
            GLuint tex = 0;
            if (is_places && i == 0) /* This PC */
                tex = get_special_folder_icon(CSIDL_DRIVES);
            if (!tex && is_places) tex = get_path_icon_shell(it->path);
            if (!tex) tex = get_path_icon(it->path);
            it->icon_tex = tex;
        }
    }
}

/* ---- Breadcrumb ---- */
#define MAX_CRUMBS 16
typedef struct { char label[64]; char path[MAX_PATH]; } Crumb;

static int parse_breadcrumbs(const char* path, Crumb* crumbs) {
    int n = 0;
    strcpy(crumbs[n].label, "This PC");
    strcpy(crumbs[n].path, "C:\\"); n++;
    if (path[1] == ':') {
        _snprintf(crumbs[n].label, 64, "Local Disk (%c:)", path[0]);
        _snprintf(crumbs[n].path, MAX_PATH, "%c:\\", path[0]); n++;
        const char* p = path + 3;
        char built[MAX_PATH];
        _snprintf(built, MAX_PATH, "%c:\\", path[0]);
        while (*p && n < MAX_CRUMBS) {
            const char* sep = strchr(p, '\\');
            int len = sep ? (int)(sep-p) : (int)strlen(p);
            if (len > 0) {
                strncpy(crumbs[n].label, p, len < 63 ? len : 63);
                crumbs[n].label[len < 63 ? len : 63] = 0;
                if (built[strlen(built)-1] != '\\') strcat(built, "\\");
                strncat(built, p, len);
                strncpy(crumbs[n].path, built, MAX_PATH); n++;
            }
            if (sep) p = sep + 1; else break;
        }
    }
    return n;
}

/* ================================================================
 *  CONTEXT MENU & FILE OPS
 * ================================================================ */

/* ---- Input dialog (for New Folder / New File) ---- */
/* Build unique name for a new folder/file: appends " (n)" before extension if needed */
static int path_exists(const char* path) {
    WCHAR w[MAX_PATH]; u8_to_w(path, w, MAX_PATH);
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

static void make_unique_name(const char* dir, const char* base_name, char* out, int out_n) {
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s\\%s", dir, base_name);
    if (!path_exists(path)) {
        strncpy(out, base_name, out_n - 1); out[out_n-1] = 0; return;
    }
    char stem[MAX_PATH], ext[MAX_PATH] = "";
    strncpy(stem, base_name, MAX_PATH - 1); stem[MAX_PATH-1] = 0;
    char* dot = strrchr(stem, '.');
    if (dot && dot != stem) { strncpy(ext, dot, MAX_PATH-1); *dot = 0; }
    for (int i = 2; i < 1000; i++) {
        _snprintf(out, out_n, "%s (%d)%s", stem, i, ext);
        _snprintf(path, MAX_PATH, "%s\\%s", dir, out);
        if (!path_exists(path)) return;
    }
    strncpy(out, base_name, out_n - 1); out[out_n-1] = 0;
}

/* ---- Inline rename ---- */
static void inline_rename_cancel(void) {
    if (g_edit_hwnd) { DestroyWindow(g_edit_hwnd); g_edit_hwnd = NULL; }
    g_edit_idx = -1;
    g_edit_panel = -1;
    g_needs_redraw = 1;
    SetFocus(g_hwnd);
}

static void inline_rename_commit(void) {
    if (!g_edit_hwnd || g_edit_idx < 0) { inline_rename_cancel(); return; }
    Tab* t = active_tab();
    if (g_edit_idx >= t->entry_count) { inline_rename_cancel(); return; }
    WCHAR wnew[MAX_PATH] = {0};
    GetWindowTextW(g_edit_hwnd, wnew, MAX_PATH);
    char new_name[MAX_PATH] = {0};
    w_to_u8(wnew, new_name, MAX_PATH);
    const char* old_name = t->entries[g_edit_idx].name;
    if (new_name[0] && strcmp(new_name, old_name) != 0) {
        WCHAR wold_p[MAX_PATH], wnew_p[MAX_PATH];
        path_join_w(t->path, old_name, wold_p, MAX_PATH);
        path_join_w(t->path, new_name, wnew_p, MAX_PATH);
        if (MoveFileW(wold_p, wnew_p)) {
            scan_directory(t);
            for (int i = 0; i < t->entry_count; i++) {
                if (strcmp(t->entries[i].name, new_name) == 0) {
                    sel_only(t, i);
                    t->selected = i;
                    t->sel_anchor = i;
                    scroll_to_entry(t, i);
                    break;
                }
            }
        }
    }
    inline_rename_cancel();
}

static LRESULT CALLBACK edit_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { inline_rename_commit(); return 0; }
        if (wp == VK_ESCAPE) { inline_rename_cancel(); return 0; }
        break;
    case WM_KILLFOCUS:
        /* Commit on focus loss */
        PostMessageW(g_hwnd, WM_APP + 1, 0, 0);
        return 0;
    }
    return CallWindowProcW(g_edit_oldproc, hwnd, msg, wp, lp);
}

static void addr_edit_cancel(void) {
    if (g_addr_hwnd) { DestroyWindow(g_addr_hwnd); g_addr_hwnd = NULL; }
    g_addr_panel = -1;
    g_needs_redraw = 1;
    SetFocus(g_hwnd);
}

static void addr_edit_commit(void) {
    if (!g_addr_hwnd) return;
    WCHAR wbuf[MAX_PATH] = {0};
    GetWindowTextW(g_addr_hwnd, wbuf, MAX_PATH);
    char buf[MAX_PATH] = {0};
    w_to_u8(wbuf, buf, MAX_PATH);
    int n = (int)strlen(buf);
    while (n > 3 && (buf[n-1] == '\\' || buf[n-1] == '/')) buf[--n] = 0;
    int wlen = (int)wcslen(wbuf);
    while (wlen > 3 && (wbuf[wlen-1] == L'\\' || wbuf[wlen-1] == L'/')) wbuf[--wlen] = 0;
    if (buf[0] && PathIsDirectoryW(wbuf)) {
        tab_navigate(active_tab(), buf, 1);
    }
    addr_edit_cancel();
}

static int is_path_word_sep(char c) {
    return c == '\\' || c == '/' || c == ' ' || c == '.';
}

static int is_path_word_sep_w(WCHAR c) {
    return c == L'\\' || c == L'/' || c == L' ' || c == L'.';
}

static LRESULT CALLBACK addr_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { addr_edit_commit(); return 0; }
        if (wp == VK_ESCAPE) { addr_edit_cancel(); return 0; }
        break;
    case WM_CHAR:
        if (wp == 0x7F) { /* Ctrl+Backspace → delete previous word */
            DWORD sel_s = 0, sel_e = 0;
            SendMessageW(hwnd, EM_GETSEL, (WPARAM)&sel_s, (LPARAM)&sel_e);
            if (sel_s != sel_e) {
                SendMessageW(hwnd, EM_REPLACESEL, TRUE, (LPARAM)L"");
            } else if (sel_s > 0) {
                WCHAR buf[MAX_PATH];
                GetWindowTextW(hwnd, buf, MAX_PATH);
                int pos = (int)sel_s;
                while (pos > 0 && is_path_word_sep_w(buf[pos-1])) pos--;
                while (pos > 0 && !is_path_word_sep_w(buf[pos-1])) pos--;
                SendMessageW(hwnd, EM_SETSEL, pos, sel_s);
                SendMessageW(hwnd, EM_REPLACESEL, TRUE, (LPARAM)L"");
            }
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        PostMessageW(g_hwnd, WM_APP + 2, 0, 0);
        return 0;
    }
    return CallWindowProcW(g_addr_oldproc, hwnd, msg, wp, lp);
}

static void addr_apply_round_region(int w, int h) {
    HRGN rgn = CreateRoundRectRgn(0, 0, w+1, h+1, 6, 6);
    SetWindowRgn(g_addr_hwnd, rgn, TRUE);
}

static void addr_edit_start(float x, float y, float w, float h) {
    if (g_addr_hwnd) addr_edit_cancel();
    if (g_edit_hwnd) inline_rename_cancel();
    Tab* t = active_tab();
    g_addr_panel = g_app.active_panel;

    g_addr_x = (int)x; g_addr_y = (int)y; g_addr_w = (int)w; g_addr_h = (int)h;
    WCHAR wpath[MAX_PATH];
    u8_to_w(t->path, wpath, MAX_PATH);
    g_addr_hwnd = CreateWindowExW(0, L"EDIT", wpath,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_MULTILINE,
        g_addr_x, g_addr_y, g_addr_w, g_addr_h,
        g_hwnd, NULL, GetModuleHandle(NULL), NULL);

    if (!g_edit_font)
        g_edit_font = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Segoe UI Semibold");
    SendMessageW(g_addr_hwnd, WM_SETFONT, (WPARAM)g_edit_font, TRUE);
    SendMessageW(g_addr_hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(6, 6));
    {
        int fh = g_renderer.font_height;
        int top = (g_addr_h - fh) / 2;
        if (top < 0) top = 0;
        RECT fmt = { 6, top, g_addr_w - 6, top + fh };
        SendMessageW(g_addr_hwnd, EM_SETRECT, 0, (LPARAM)&fmt);
    }
    SendMessageW(g_addr_hwnd, EM_SETSEL, 0, -1);

    addr_apply_round_region(g_addr_w, g_addr_h);

    g_addr_oldproc = (WNDPROC)SetWindowLongPtrW(g_addr_hwnd, GWLP_WNDPROC, (LONG_PTR)addr_subclass_proc);
    SetFocus(g_addr_hwnd);
    g_needs_redraw = 1;
}

static void inline_rename_start(int idx) {
    Tab* t = active_tab();
    if (idx < 0 || idx >= t->entry_count) return;
    if (strcmp(t->entries[idx].name, "..") == 0) return;
    if (g_edit_hwnd) inline_rename_cancel();

    t->selected = idx;
    g_edit_idx = idx;
    g_edit_panel = g_app.active_panel;

    /* Compute position of the name cell */
    float lx = SIDEBAR_W;
    float ly = CONTENT_TOP + COL_HDR_H;
    float ry = ly + idx * ROW_H - t->scroll_y;
    float name_x = lx + 23;
    float right = lx + (g_width - SIDEBAR_W) - 10;
    float type_x = right - COLW_SIZE - COLW_DATE - COLW_TYPE;
    float name_w = type_x - name_x - 2;

    WCHAR wname[MAX_PATH];
    u8_to_w(t->entries[idx].name, wname, MAX_PATH);
    g_edit_hwnd = CreateWindowExW(0, L"EDIT", wname,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_MULTILINE,
        (int)name_x, (int)ry, (int)name_w, ROW_H,
        g_hwnd, NULL, GetModuleHandle(NULL), NULL);

    /* Style the edit: use same font, dark theme */
    if (!g_edit_font)
        g_edit_font = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Segoe UI Semibold");
    SendMessageW(g_edit_hwnd, WM_SETFONT, (WPARAM)g_edit_font, TRUE);
    SendMessageW(g_edit_hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(2, 2));
    {
        int fh = g_renderer.font_height;
        int top = (ROW_H - fh) / 2; if (top < 0) top = 0;
        RECT fmt = { 2, top, (int)name_w - 2, top + fh };
        SendMessageW(g_edit_hwnd, EM_SETRECT, 0, (LPARAM)&fmt);
    }

    /* Select filename without extension. Compute split on WCHARs. */
    {
        int wlen = (int)wcslen(wname);
        int sel_end = wlen;
        for (int k = wlen - 1; k > 0; k--) {
            if (wname[k] == L'.') { sel_end = k; break; }
        }
        SendMessageW(g_edit_hwnd, EM_SETSEL, 0, (LPARAM)sel_end);
    }

    g_edit_oldproc = (WNDPROC)SetWindowLongPtrW(g_edit_hwnd, GWLP_WNDPROC, (LONG_PTR)edit_subclass_proc);
    SetFocus(g_edit_hwnd);
    g_needs_redraw = 1;
}

/* ---- Clipboard ---- */
static void clipboard_copy_text(const char* text) {
    /* Put the path on the clipboard as Unicode so Russian/Arabic/emoji file
       names survive the round-trip. Also set CF_TEXT as an ANSI fallback —
       Windows would auto-synthesise it anyway, but doing it explicitly avoids
       weird shell extensions that try to "enrich" the clipboard (e.g. some
       video shell-ext that injects a thumbnail when only CF_TEXT is present
       for a media path). */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    HGLOBAL hw = GlobalAlloc(GHND, wlen * sizeof(WCHAR));
    if (!hw) return;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, (WCHAR*)GlobalLock(hw), wlen);
    GlobalUnlock(hw);

    int alen = (int)strlen(text) + 1;
    HGLOBAL ha = GlobalAlloc(GHND, alen);
    if (ha) { memcpy(GlobalLock(ha), text, alen); GlobalUnlock(ha); }

    /* Other processes (e.g. Windows Snipping Tool right after a capture)
       sometimes hold the clipboard for a few ms. If we silent-fail, the
       *previous* clipboard contents (the snip's PNG) remain — and the user
       thinks "Copy Path" pasted an image. Retry briefly. */
    int opened = 0;
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(g_hwnd)) { opened = 1; break; }
        Sleep(15);
    }
    if (!opened) { GlobalFree(hw); if (ha) GlobalFree(ha); return; }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, hw);
    if (ha) SetClipboardData(CF_TEXT, ha);
    CloseClipboard();
}

static void clipboard_set_files(const char* path, DWORD effect) {
    int plen = (int)strlen(path);
    int total = (int)sizeof(DROPFILES) + plen + 2;
    HGLOBAL hg = GlobalAlloc(GHND, total);
    DROPFILES* df = (DROPFILES*)GlobalLock(hg);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = FALSE;
    memcpy((char*)df + sizeof(DROPFILES), path, plen + 1);
    GlobalUnlock(hg);

    HGLOBAL he = GlobalAlloc(GHND, sizeof(DWORD));
    *(DWORD*)GlobalLock(he) = effect;
    GlobalUnlock(he);

    OpenClipboard(g_hwnd);
    EmptyClipboard();
    SetClipboardData(CF_HDROP, hg);
    if (g_cf_drop_effect) SetClipboardData(g_cf_drop_effect, he);
    CloseClipboard();
}

static void clipboard_paste(const char* dest_dir) {
    if (!IsClipboardFormatAvailable(CF_HDROP)) return;
    if (!OpenClipboard(g_hwnd)) return;
    HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
    if (!hd) { CloseClipboard(); return; }

    DWORD effect = DROPEFFECT_COPY;
    if (g_cf_drop_effect) {
        HGLOBAL he = (HGLOBAL)GetClipboardData(g_cf_drop_effect);
        if (he) { DWORD* pe = (DWORD*)GlobalLock(he); if (pe) { effect = *pe; GlobalUnlock(he); } }
    }

    int count = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
    for (int i = 0; i < count; i++) {
        WCHAR wsrc[MAX_PATH + 2] = {0}, wdst[MAX_PATH + 2] = {0};
        DragQueryFileW(hd, i, wsrc, MAX_PATH);
        char src[MAX_PATH], dst[MAX_PATH];
        w_to_u8(wsrc, src, MAX_PATH);
        const char* nm = strrchr(src, '\\');
        nm = nm ? nm + 1 : src;

        char src_dir[MAX_PATH];
        strncpy(src_dir, src, MAX_PATH - 1); src_dir[MAX_PATH-1] = 0;
        char* slash = strrchr(src_dir, '\\');
        if (slash) *slash = 0;
        int same_dir = (_stricmp(src_dir, dest_dir) == 0);

        if (same_dir && effect == DROPEFFECT_MOVE) continue;

        char dst_name[MAX_PATH];
        if (same_dir && effect == DROPEFFECT_COPY) {
            char stem[MAX_PATH], ext[MAX_PATH] = "";
            strncpy(stem, nm, MAX_PATH - 1); stem[MAX_PATH-1] = 0;
            char* dot = strrchr(stem, '.');
            if (dot && dot != stem) { strncpy(ext, dot, MAX_PATH-1); *dot = 0; }
            char base[MAX_PATH];
            _snprintf(base, MAX_PATH, "%s - Copy%s", stem, ext);
            make_unique_name(dest_dir, base, dst_name, MAX_PATH);
        } else {
            strncpy(dst_name, nm, MAX_PATH - 1); dst_name[MAX_PATH-1] = 0;
        }
        _snprintf(dst, MAX_PATH, "%s\\%s", dest_dir, dst_name);
        u8_to_w(dst, wdst, MAX_PATH);

        SHFILEOPSTRUCTW op = {0};
        op.hwnd = g_hwnd;
        op.wFunc = (effect == DROPEFFECT_MOVE) ? FO_MOVE : FO_COPY;
        op.pFrom = wsrc;
        op.pTo = wdst;
        op.fFlags = FOF_ALLOWUNDO;
        SHFileOperationW(&op);
    }
    CloseClipboard();
    if (effect == DROPEFFECT_MOVE) {
        OpenClipboard(g_hwnd); EmptyClipboard(); CloseClipboard();
    }
}

/* ---- File operations ---- */
static void do_delete_file(const char* path) {
    WCHAR wfrom[MAX_PATH + 2] = {0};
    u8_to_w(path, wfrom, MAX_PATH);
    SHFILEOPSTRUCTW op = {0};
    op.hwnd = g_hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = wfrom;
    op.fFlags = FOF_ALLOWUNDO;
    SHFileOperationW(&op);
}

/* Build double-null-terminated UTF-16 path list of selected entries.
   Returns allocated buffer (free with free) and total WCHAR count via *out_wlen. */
static WCHAR* build_selected_path_list_w(Tab* t, int* out_wlen) {
    int wcap = 0, n = 0;
    char tmp[MAX_PATH * 2];
    for (int i = 0; i < t->entry_count; i++) {
        if (!t->sel_mask[i]) continue;
        if (strcmp(t->entries[i].name, "..") == 0) continue;
        _snprintf(tmp, sizeof(tmp), "%s\\%s", t->path, t->entries[i].name);
        wcap += MultiByteToWideChar(CP_UTF8, 0, tmp, -1, NULL, 0); /* includes null */
        n++;
    }
    if (n == 0) { *out_wlen = 0; return NULL; }
    wcap += 1;
    WCHAR* buf = (WCHAR*)calloc(wcap, sizeof(WCHAR));
    int off = 0;
    for (int i = 0; i < t->entry_count; i++) {
        if (!t->sel_mask[i]) continue;
        if (strcmp(t->entries[i].name, "..") == 0) continue;
        _snprintf(tmp, sizeof(tmp), "%s\\%s", t->path, t->entries[i].name);
        int w = MultiByteToWideChar(CP_UTF8, 0, tmp, -1, buf + off, wcap - off);
        off += w; /* includes null terminator */
    }
    buf[off] = 0;
    *out_wlen = off + 1;
    return buf;
}

static void clipboard_set_selected(Tab* t, DWORD effect) {
    int wlen = 0;
    WCHAR* list = build_selected_path_list_w(t, &wlen);
    if (!list) return;
    int total = (int)sizeof(DROPFILES) + wlen * (int)sizeof(WCHAR);
    HGLOBAL hg = GlobalAlloc(GHND, total);
    DROPFILES* df = (DROPFILES*)GlobalLock(hg);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy((char*)df + sizeof(DROPFILES), list, wlen * sizeof(WCHAR));
    GlobalUnlock(hg);
    free(list);

    HGLOBAL he = GlobalAlloc(GHND, sizeof(DWORD));
    *(DWORD*)GlobalLock(he) = effect;
    GlobalUnlock(he);

    OpenClipboard(g_hwnd);
    EmptyClipboard();
    SetClipboardData(CF_HDROP, hg);
    if (g_cf_drop_effect) SetClipboardData(g_cf_drop_effect, he);
    CloseClipboard();
}

static void do_delete_selected(Tab* t) {
    int wlen = 0;
    WCHAR* list = build_selected_path_list_w(t, &wlen);
    if (!list) return;
    SHFILEOPSTRUCTW op = {0};
    op.hwnd = g_hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = list;
    op.fFlags = FOF_ALLOWUNDO;
    SHFileOperationW(&op);
    free(list);
}

static void do_permanent_delete_selected(Tab* t) {
    int n = sel_count(t);
    if (n <= 0) return;
    /* Build a confirmation message. Show the single file name when only one
       is selected, otherwise just the count — keeps the dialog readable. */
    WCHAR msg[1024];
    if (n == 1) {
        char only_name[MAX_PATH] = {0};
        for (int i = 0; i < t->entry_count; i++) {
            if (t->sel_mask[i] && strcmp(t->entries[i].name, "..") != 0) {
                strncpy(only_name, t->entries[i].name, MAX_PATH-1);
                break;
            }
        }
        WCHAR wname[MAX_PATH];
        u8_to_w(only_name, wname, MAX_PATH);
        _snwprintf(msg, 1024,
            L"Permanently delete '%ls'?\n\nThis file will NOT go to the Recycle Bin "
            L"and cannot be undone.", wname);
    } else {
        _snwprintf(msg, 1024,
            L"Permanently delete %d items?\n\nThese files will NOT go to the Recycle Bin "
            L"and cannot be undone.", n);
    }
    int btn = MessageBoxW(g_hwnd, msg, L"Confirm permanent delete",
                          MB_YESNO | MB_ICONWARNING);
    if (btn != IDYES) return;

    int wlen = 0;
    WCHAR* list = build_selected_path_list_w(t, &wlen);
    if (!list) return;
    SHFILEOPSTRUCTW op = {0};
    op.hwnd = g_hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = list;
    op.fFlags = FOF_NOCONFIRMATION;  /* permanent + skip Windows' own prompt */
    SHFileOperationW(&op);
    free(list);
}

/* ---- Batch rename ---- */
static void split_stem_ext(const char* name, char* stem, char* ext, int n) {
    strncpy(stem, name, n - 1); stem[n - 1] = 0;
    ext[0] = 0;
    char* dot = strrchr(stem, '.');
    if (dot && dot != stem) {
        strncpy(ext, dot, n - 1); ext[n - 1] = 0;
        *dot = 0;
    }
}

static void batch_rename_start(Tab* t) {
    (void)t;
    g_batch_active = 1;
    g_batch_panel  = g_app.active_panel;
    g_batch_typed_len = 0;
    g_batch_typed[0] = 0;
    g_batch_chop = 0;
    g_batch_prefix_len = 0;
    g_batch_prefix[0] = 0;
    g_batch_chop_left = 0;
    g_batch_focus = 0;      /* suffix side by default (original behaviour) */
    g_needs_redraw = 1;
}

static void batch_rename_cancel(void) {
    g_batch_active = 0;
    g_batch_panel  = -1;
    g_batch_typed_len = 0;
    g_batch_typed[0] = 0;
    g_batch_chop = 0;
    g_batch_prefix_len = 0;
    g_batch_prefix[0] = 0;
    g_batch_chop_left = 0;
    g_batch_focus = 0;
    g_needs_redraw = 1;
}

static void batch_rename_commit(void) {
    if (!g_batch_active) return;
    Panel* P = &g_app.panels[g_batch_panel >= 0 ? g_batch_panel : g_app.active_panel];
    Tab* t = &P->tabs[P->active_tab];
    int any_change = (g_batch_typed_len > 0 || g_batch_chop > 0 ||
                      g_batch_prefix_len > 0 || g_batch_chop_left > 0);
    if (any_change) {
        for (int i = 0; i < t->entry_count; i++) {
            if (!t->sel_mask[i]) continue;
            if (strcmp(t->entries[i].name, "..") == 0) continue;
            char stem[MAX_PATH], ext[MAX_PATH];
            split_stem_ext(t->entries[i].name, stem, ext, MAX_PATH);
            int slen = (int)strlen(stem);
            int cl = g_batch_chop_left;  if (cl > slen) cl = slen;
            int cr = g_batch_chop;       if (cr > slen - cl) cr = slen - cl;
            int keep = slen - cl - cr;
            char middle[MAX_PATH];
            if (keep > 0) memcpy(middle, stem + cl, keep);
            middle[keep] = 0;
            char new_name[MAX_PATH];
            _snprintf(new_name, MAX_PATH, "%s%s%s%s",
                      g_batch_prefix, middle, g_batch_typed, ext);
            if (new_name[0] == 0) continue;
            WCHAR wold[MAX_PATH], wnew[MAX_PATH];
            path_join_w(t->path, t->entries[i].name, wold, MAX_PATH);
            path_join_w(t->path, new_name, wnew, MAX_PATH);
            MoveFileW(wold, wnew);
        }
        scan_directory(t);
    }
    batch_rename_cancel();
}

static void do_rename(Tab* t, int idx) {
    if (sel_count(t) > 1) batch_rename_start(t);
    else inline_rename_start(idx);
}

static void focus_and_rename(Tab* t, const char* name) {
    scan_directory(t);
    for (int i = 0; i < t->entry_count; i++) {
        if (strcmp(t->entries[i].name, name) == 0) {
            sel_only(t, i);
            t->selected = i;
            t->sel_anchor = i;
            scroll_to_entry(t, i);
            inline_rename_start(i);
            return;
        }
    }
}

/* ---- Fuzzy finder ---- */

/* Case-insensitive char compare */
static int ff_ceq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

/* Score one token against `name`. Prefers a contiguous case-insensitive
   substring hit; falls back to a subsequence match. Appends match
   positions to `marks[]` in-place. Returns -1 if the token doesn't match. */
static int ff_score_token(const char* name, const char* tok, int tlen,
                          short* marks, int* n_marks)
{
    int nlen = (int)strlen(name);
    if (tlen == 0) return 0;
    /* 1. Try contiguous substring — this is what a user typically means. */
    int best_pos = -1;
    for (int i = 0; i + tlen <= nlen; i++) {
        int ok = 1;
        for (int k = 0; k < tlen; k++) {
            if (!ff_ceq(name[i+k], tok[k])) { ok = 0; break; }
        }
        if (ok) { best_pos = i; break; }
    }
    if (best_pos >= 0) {
        for (int k = 0; k < tlen && *n_marks < FF_MATCH_MARKS; k++)
            marks[(*n_marks)++] = (short)(best_pos + k);
        int score = 40 + tlen * 12;
        if (best_pos == 0) score += 20;
        else {
            char prev = name[best_pos - 1];
            if (prev == '_' || prev == '-' || prev == '.' ||
                prev == ' ' || prev == '/' || prev == '\\') score += 10;
        }
        return score;
    }
    /* 2. Fallback: in-order subsequence match. */
    int qi = 0, score = 0, consec = 0;
    int start_marks = *n_marks;
    for (int i = 0; i < nlen && qi < tlen; i++) {
        if (ff_ceq(name[i], tok[qi])) {
            if (*n_marks < FF_MATCH_MARKS) marks[(*n_marks)++] = (short)i;
            score += 10 + consec * 8;
            if (i == 0) score += 4;
            else {
                char prev = name[i-1];
                if (prev == '_' || prev == '-' || prev == '.' ||
                    prev == ' ' || prev == '/' || prev == '\\') score += 3;
            }
            consec++;
            qi++;
        } else {
            consec = 0;
        }
    }
    if (qi < tlen) { *n_marks = start_marks; return -1; }
    return score;
}

/* Score a whitespace-separated query against `name`. Each token must
   match independently (order between tokens doesn't matter). Returns -1
   if any token misses. Marks are the deduplicated & sorted union of the
   per-token match positions. */
static int ff_score_match(const char* name, const char* query, int qlen,
                          short* marks, int* n_marks)
{
    if (qlen == 0) { *n_marks = 0; return 1; }
    int total = 0;
    int nmarks = 0;
    int i = 0;
    int tokens = 0;
    while (i < qlen) {
        while (i < qlen && query[i] == ' ') i++;
        if (i >= qlen) break;
        int j = i;
        while (j < qlen && query[j] != ' ') j++;
        int s = ff_score_token(name, query + i, j - i, marks, &nmarks);
        if (s < 0) return -1;
        total += s;
        tokens++;
        i = j;
    }
    if (tokens == 0) { *n_marks = 0; return 1; }
    /* Dedup + insertion-sort marks so highlighting walks left-to-right */
    for (int a = 1; a < nmarks; a++) {
        short t = marks[a]; int b = a;
        while (b > 0 && marks[b-1] > t) { marks[b] = marks[b-1]; b--; }
        marks[b] = t;
    }
    int out = 0;
    for (int a = 0; a < nmarks; a++)
        if (a == 0 || marks[a] != marks[a-1]) marks[out++] = marks[a];
    /* Slight bias toward shorter names */
    total -= (int)strlen(name) / 4;
    *n_marks = out;
    return total;
}

/* Build the flat index from the currently active tab's entries — no
   recursion, no file-system I/O beyond what scan_directory already did. */
static void ff_build_index_from_tab(void) {
    Tab* t = active_tab();
    g_ff_index_count = 0;
    strncpy(g_ff_root, t->path, MAX_PATH-1);
    g_ff_root[MAX_PATH-1] = 0;
    for (int i = 0; i < t->entry_count && g_ff_index_count < FF_MAX_INDEX; i++) {
        FileEntry* e = &t->entries[i];
        if (strcmp(e->name, "..") == 0) continue;
        FFEntry* fe = &g_ff_index[g_ff_index_count++];
        strncpy(fe->name, e->name, MAX_PATH-1); fe->name[MAX_PATH-1] = 0;
        fe->rel[0] = 0;
        fe->is_dir = e->is_dir;
    }
}

/* Recursive BFS scan — runs on a worker; pushes results into g_ff_index
   under g_ff_cs and bumps g_ff_result_count via ff_refresh_results-like
   updates each 200 entries so the list feels live. */
static int ff_should_skip_dir(const char* name) {
    /* Skip heavy noise directories by default */
    if (name[0] == '.') return 1;   /* .git, .svn, .venv, ... */
    if (strcmp(name, "node_modules") == 0) return 1;
    if (strcmp(name, "__pycache__") == 0) return 1;
    if (strcmp(name, "target") == 0) return 1;   /* rust / java */
    return 0;
}

typedef struct {
    char full[MAX_PATH];
    char rel[MAX_PATH];
} FFScanFrame;

static DWORD WINAPI ff_scan_worker(LPVOID arg) {
    LONG my_gen = (LONG)(LONG_PTR)arg;
    /* BFS: two arrays swapped */
    static FFScanFrame stack_a[512];
    static FFScanFrame stack_b[512];
    FFScanFrame* cur  = stack_a;
    FFScanFrame* next = stack_b;
    int cur_n = 0, next_n = 0;
    strncpy(cur[0].full, g_ff_root, MAX_PATH-1); cur[0].full[MAX_PATH-1] = 0;
    cur[0].rel[0] = 0;
    cur_n = 1;

    while (cur_n > 0 && !g_ff_scan_cancel && my_gen == g_ff_scan_gen) {
        next_n = 0;
        for (int i = 0; i < cur_n && !g_ff_scan_cancel && my_gen == g_ff_scan_gen; i++) {
            WCHAR wpat[MAX_PATH + 4];
            {
                char pat[MAX_PATH + 4];
                _snprintf(pat, sizeof(pat), "%s\\*", cur[i].full);
                u8_to_w(pat, wpat, MAX_PATH + 4);
            }
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(wpat, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.cFileName[0] == L'.' &&
                    (fd.cFileName[1] == 0 ||
                     (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
                char u8name[MAX_PATH];
                w_to_u8(fd.cFileName, u8name, MAX_PATH);
                int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

                EnterCriticalSection(&g_ff_cs);
                if (g_ff_index_count < FF_MAX_INDEX) {
                    FFEntry* fe = &g_ff_index[g_ff_index_count++];
                    strncpy(fe->name, u8name, MAX_PATH-1); fe->name[MAX_PATH-1] = 0;
                    strncpy(fe->rel, cur[i].rel, MAX_PATH-1); fe->rel[MAX_PATH-1] = 0;
                    fe->is_dir = is_dir;
                }
                LeaveCriticalSection(&g_ff_cs);

                if (is_dir && !ff_should_skip_dir(u8name) && next_n < 512) {
                    FFScanFrame* nf = &next[next_n++];
                    if (cur[i].rel[0])
                        _snprintf(nf->rel, MAX_PATH, "%s\\%s", cur[i].rel, u8name);
                    else
                        _snprintf(nf->rel, MAX_PATH, "%s", u8name);
                    _snprintf(nf->full, MAX_PATH, "%s\\%s", cur[i].full, u8name);
                }
            } while (FindNextFileW(h, &fd) && !g_ff_scan_cancel && my_gen == g_ff_scan_gen);
            FindClose(h);
            /* Periodically nudge UI to re-run match with growing index */
            if ((g_ff_index_count & 0xFF) == 0)
                PostMessageW(g_hwnd, WM_APP + 20, 0, 0);
        }
        /* swap */
        FFScanFrame* tmp = cur; cur = next; next = tmp;
        cur_n = next_n;
    }
    g_ff_scanning = 0;
    PostMessageW(g_hwnd, WM_APP + 20, 0, 0);
    return 0;
}

static void ff_stop_scan(void) {
    if (!g_ff_scan_thread) { g_ff_scanning = 0; return; }
    InterlockedIncrement(&g_ff_scan_gen);
    g_ff_scan_cancel = 1;
    /* Non-blocking hint: worker checks g_ff_scan_cancel in its inner
       loops; give it up to 300 ms to notice, else move on and let the
       thread finish in the background (it will exit on its own since
       g_ff_scan_gen changed and PostMessage-fail is safe). */
    if (WaitForSingleObject(g_ff_scan_thread, 300) == WAIT_OBJECT_0) {
        CloseHandle(g_ff_scan_thread);
        g_ff_scan_thread = NULL;
    } else {
        /* Detach — worker will finish and self-exit; handle leaks but
           we won't block the UI further. Bump gen so any later message
           it posts is treated as stale. */
        CloseHandle(g_ff_scan_thread);
        g_ff_scan_thread = NULL;
    }
    g_ff_scanning = 0;
    g_ff_scan_cancel = 0;
}

static void ff_kick_recursive_scan(void) {
    if (!g_ff_cs_init) { InitializeCriticalSection(&g_ff_cs); g_ff_cs_init = 1; }
    ff_stop_scan();

    EnterCriticalSection(&g_ff_cs);
    g_ff_index_count = 0;
    strncpy(g_ff_root, active_tab()->path, MAX_PATH-1);
    g_ff_root[MAX_PATH-1] = 0;
    LeaveCriticalSection(&g_ff_cs);

    g_ff_scanning = 1;
    LONG gen = g_ff_scan_gen;
    g_ff_scan_thread = CreateThread(NULL, 0, ff_scan_worker, (LPVOID)(LONG_PTR)gen, 0, NULL);
}

static void ff_refresh_results(void) {
    g_ff_result_count = 0;
    int min_score = 0;
    int min_idx   = -1;
    /* Cap loop under critical section-free read; ok because worker only
       APPENDS and we read count once — it monotonically grows. */
    int count = g_ff_index_count;
    for (int i = 0; i < count; i++) {
        FFEntry* fe = &g_ff_index[i];
        short marks[FF_MATCH_MARKS];
        int n_marks = 0;
        int s = ff_score_match(fe->name, g_ff_query, g_ff_query_len, marks, &n_marks);
        if (s < 0) continue;
        if (fe->is_dir) s += 5;   /* directories rank a bit higher */
        if (g_ff_result_count < FF_RESULT_MAX) {
            FFResult* r = &g_ff_results[g_ff_result_count++];
            r->entry_idx = i;
            r->score = s;
            r->n_marks = n_marks;
            memcpy(r->marks, marks, sizeof(short) * n_marks);
            if (r->score < min_score || min_idx < 0) { min_score = r->score; min_idx = g_ff_result_count - 1; }
        } else if (s > min_score) {
            FFResult* r = &g_ff_results[min_idx];
            r->entry_idx = i;
            r->score = s;
            r->n_marks = n_marks;
            memcpy(r->marks, marks, sizeof(short) * n_marks);
            /* recompute min */
            min_score = g_ff_results[0].score; min_idx = 0;
            for (int k = 1; k < g_ff_result_count; k++)
                if (g_ff_results[k].score < min_score) { min_score = g_ff_results[k].score; min_idx = k; }
        }
    }
    /* Simple insertion sort by score desc — result_count <= 200 */
    for (int i = 1; i < g_ff_result_count; i++) {
        FFResult tmp = g_ff_results[i];
        int j = i;
        while (j > 0 && g_ff_results[j-1].score < tmp.score) {
            g_ff_results[j] = g_ff_results[j-1];
            j--;
        }
        g_ff_results[j] = tmp;
    }
    if (g_ff_selected >= g_ff_result_count) g_ff_selected = g_ff_result_count - 1;
    if (g_ff_selected < 0) g_ff_selected = 0;
}

/* UTF-8 helpers: byte offset of previous / next codepoint boundary. */
static int ff_prev_cp(const char* s, int pos) {
    if (pos <= 0) return 0;
    int p = pos - 1;
    while (p > 0 && (s[p] & 0xC0) == 0x80) p--;
    return p;
}
static int ff_next_cp(const char* s, int len, int pos) {
    if (pos >= len) return len;
    int p = pos + 1;
    while (p < len && (s[p] & 0xC0) == 0x80) p++;
    return p;
}
static int ff_is_word(unsigned char c) {
    /* Simple word class: letters + digits + underscore */
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}
static int ff_prev_word_boundary(const char* s, int pos) {
    if (pos <= 0) return 0;
    /* Skip trailing non-word chars, then skip a run of word chars */
    while (pos > 0 && !ff_is_word((unsigned char)s[pos-1])) pos--;
    while (pos > 0 && ff_is_word((unsigned char)s[pos-1])) pos--;
    return pos;
}
static int ff_next_word_boundary(const char* s, int len, int pos) {
    if (pos >= len) return len;
    while (pos < len && ff_is_word((unsigned char)s[pos])) pos++;
    while (pos < len && !ff_is_word((unsigned char)s[pos])) pos++;
    return pos;
}

static void ff_open(void) {
    g_ff_active   = 1;
    g_ff_panel    = g_app.active_panel;
    g_ff_query[0] = 0;
    g_ff_query_len = 0;
    g_ff_cursor   = 0;
    g_ff_selected = 0;
    g_ff_scroll   = 0;
    if (!g_ff_cs_init) { InitializeCriticalSection(&g_ff_cs); g_ff_cs_init = 1; }
    if (g_ff_recursive) {
        ff_kick_recursive_scan();
    } else {
        ff_build_index_from_tab();
    }
    ff_refresh_results();
    g_needs_redraw = 1;
}

static void ff_close(void) {
    g_ff_active = 0;
    g_ff_panel  = -1;
    ff_stop_scan();
    g_ff_index_count = 0;
    g_ff_result_count = 0;
    g_needs_redraw = 1;
}

static void ff_open_selected(void) {
    if (g_ff_selected < 0 || g_ff_selected >= g_ff_result_count) return;
    FFResult* r = &g_ff_results[g_ff_selected];
    FFEntry*  e = &g_ff_index[r->entry_idx];
    char full[MAX_PATH];
    if (e->rel[0])
        _snprintf(full, MAX_PATH, "%s\\%s\\%s", g_ff_root, e->rel, e->name);
    else
        _snprintf(full, MAX_PATH, "%s\\%s", g_ff_root, e->name);
    Tab* t = active_tab();
    if (e->is_dir) {
        tab_navigate(t, full, 1);
    } else {
        /* Navigate to parent directory and select the file */
        char parent[MAX_PATH];
        strncpy(parent, full, MAX_PATH-1); parent[MAX_PATH-1] = 0;
        char* slash = strrchr(parent, '\\');
        if (slash) *slash = 0;
        if (_stricmp(parent, t->path) != 0) tab_navigate(t, parent, 1);
        /* select the file by name */
        for (int i = 0; i < t->entry_count; i++) {
            if (_stricmp(t->entries[i].name, e->name) == 0) {
                sel_only(t, i);
                t->selected = i;
                t->sel_anchor = i;
                scroll_to_entry(t, i);
                break;
            }
        }
    }
    ff_close();
}

static void do_new_folder(Tab* t) {
    char name[MAX_PATH];
    make_unique_name(t->path, "New folder", name, MAX_PATH);
    WCHAR wp[MAX_PATH];
    path_join_w(t->path, name, wp, MAX_PATH);
    if (CreateDirectoryW(wp, NULL)) focus_and_rename(t, name);
}

static void do_new_file(Tab* t) {
    char name[MAX_PATH];
    make_unique_name(t->path, "New Text File.txt", name, MAX_PATH);
    WCHAR wp[MAX_PATH];
    path_join_w(t->path, name, wp, MAX_PATH);
    HANDLE h = CreateFileW(wp, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); focus_and_rename(t, name); }
}

static void do_properties(const char* path) {
    SHELLEXECUTEINFOA sei = {0};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = "properties";
    sei.lpFile = path;
    sei.nShow = SW_SHOW;
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    ShellExecuteExA(&sei);
}

/* ---- Image viewer (separate window, GDI rendering) ---- */

static int     g_viewer_skip_delete_warn = 0;   /* session-scoped */
static WNDPROC g_viewer_edit_orig_proc   = NULL;

/* ---- Async image-load worker for the viewer ---- */
#define WM_VIEWER_LOADED  (WM_APP + 10)
#define VLOAD_Q_CAP       16

typedef struct {
    HWND hwnd;
    char path[MAX_PATH];
    int  request_id;
} ViewerLoadReq;

typedef struct {
    HWND     hwnd;
    int      request_id;
    HBITMAP  bmp;
    int      w, h;
    char     date_taken[64];
} ViewerLoadResult;

static CRITICAL_SECTION g_vload_cs;
static HANDLE           g_vload_event   = NULL;
static HANDLE           g_vload_thread  = NULL;
static int              g_vload_init    = 0;
static ViewerLoadReq    g_vload_q[VLOAD_Q_CAP];
static int              g_vload_q_head  = 0, g_vload_q_tail = 0;

static int is_image_ext(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    char ext[8] = {0};
    for (int i = 0; i < 7 && dot[i+1]; i++) {
        char c = dot[i+1];
        ext[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    static const char* exts[] = {
        "jpg","jpeg","png","gif","bmp","webp","tif","tiff",
        "heic","heif","avif","jfif","ico", NULL
    };
    for (int i = 0; exts[i]; i++) if (strcmp(ext, exts[i]) == 0) return 1;
    return 0;
}

#define VIEWER_MAX_SIBLINGS 4096
typedef struct {
    HWND   hwnd;
    HBITMAP bmp;
    int    img_w, img_h;
    char   date_taken[64];
    char   dir[MAX_PATH];
    char (*siblings)[MAX_PATH];
    int    sibling_count;
    int    cur_index;
    float  zoom;
    float  pan_x, pan_y;
    int    dragging;
    int    drag_x0, drag_y0;
    float  pan_x0, pan_y0;
    int    client_w, client_h;
    HWND   edit_hwnd;       /* rename overlay */
    int    hover_btn;       /* 0=none, 1=rename, 2=delete */
    int    next_request_id; /* monotonic; result discarded if != latest */
    int    loading;         /* 1 while async decode in flight */
} ViewerState;

static void viewer_collect_siblings(ViewerState* v, const char* startname) {
    if (v->siblings) { free(v->siblings); v->siblings = NULL; }
    v->sibling_count = 0;
    v->cur_index = 0;
    v->siblings = (char(*)[MAX_PATH])calloc(VIEWER_MAX_SIBLINGS, MAX_PATH);
    if (!v->siblings) return;
    char pattern[MAX_PATH+4];
    _snprintf(pattern, sizeof(pattern), "%s\\*", v->dir);
    WCHAR wpat[MAX_PATH+4];
    u8_to_w(pattern, wpat, MAX_PATH+4);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char u8name[MAX_PATH];
        w_to_u8(fd.cFileName, u8name, MAX_PATH);
        if (!is_image_ext(u8name)) continue;
        if (v->sibling_count >= VIEWER_MAX_SIBLINGS) break;
        strncpy(v->siblings[v->sibling_count], u8name, MAX_PATH-1);
        v->sibling_count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    /* Insertion sort case-insensitively — matches typical Details default. */
    for (int i = 1; i < v->sibling_count; i++) {
        char tmp[MAX_PATH];
        strcpy(tmp, v->siblings[i]);
        int j = i;
        while (j > 0 && _stricmp(v->siblings[j-1], tmp) > 0) {
            strcpy(v->siblings[j], v->siblings[j-1]);
            j--;
        }
        strcpy(v->siblings[j], tmp);
    }
    for (int i = 0; i < v->sibling_count; i++) {
        if (_stricmp(v->siblings[i], startname) == 0) { v->cur_index = i; break; }
    }
}

static void viewer_update_title(ViewerState* v);
static void viewer_rename_cancel(ViewerState* v);

static DWORD WINAPI viewer_load_worker(LPVOID arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    while (1) {
        WaitForSingleObject(g_vload_event, INFINITE);
        for (;;) {
            ViewerLoadReq req;
            int superseded = 0;
            EnterCriticalSection(&g_vload_cs);
            if (g_vload_q_head == g_vload_q_tail) {
                LeaveCriticalSection(&g_vload_cs); break;
            }
            req = g_vload_q[g_vload_q_head];
            g_vload_q_head = (g_vload_q_head + 1) % VLOAD_Q_CAP;
            /* If a newer request for the same viewer is already queued, skip
               this one — user clicked Next several times while we were still
               decoding the first. */
            for (int i = g_vload_q_head; i != g_vload_q_tail; i = (i + 1) % VLOAD_Q_CAP) {
                if (g_vload_q[i].hwnd == req.hwnd) { superseded = 1; break; }
            }
            LeaveCriticalSection(&g_vload_cs);
            if (superseded) continue;

            HBITMAP hbmp = NULL;
            int w = 0, h = 0;
            char date_taken[64] = {0};
            WCHAR wp[MAX_PATH];
            u8_to_w(req.path, wp, MAX_PATH);

            IShellItem* item = NULL;
            if (SUCCEEDED(SHCreateItemFromParsingName(wp, NULL, &IID_IShellItem, (void**)&item)) && item) {
                IShellItemImageFactory* fac = NULL;
                if (SUCCEEDED(item->lpVtbl->QueryInterface(item, &IID_IShellItemImageFactory, (void**)&fac)) && fac) {
                    SIZE sz = { 2048, 2048 };
                    if (SUCCEEDED(fac->lpVtbl->GetImage(fac, sz, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &hbmp)) && hbmp) {
                        DIBSECTION ds = {0};
                        GetObject(hbmp, sizeof(ds), &ds);
                        w = ds.dsBm.bmWidth;
                        h = abs(ds.dsBm.bmHeight);
                    }
                    fac->lpVtbl->Release(fac);
                }
                item->lpVtbl->Release(item);
            }

            IPropertyStore* ps = NULL;
            if (SUCCEEDED(SHGetPropertyStoreFromParsingName(wp, NULL, GPS_DEFAULT,
                                                            &IID_IPropertyStore, (void**)&ps)) && ps) {
                PROPERTYKEY pkey;
                pkey.fmtid.Data1 = 0x14B81DA1;
                pkey.fmtid.Data2 = 0x0135; pkey.fmtid.Data3 = 0x4D31;
                pkey.fmtid.Data4[0]=0x96; pkey.fmtid.Data4[1]=0xD9;
                pkey.fmtid.Data4[2]=0x6C; pkey.fmtid.Data4[3]=0xBF;
                pkey.fmtid.Data4[4]=0xC9; pkey.fmtid.Data4[5]=0x67;
                pkey.fmtid.Data4[6]=0x1A; pkey.fmtid.Data4[7]=0x99;
                pkey.pid = 36867;
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &pkey, &pv)) && pv.vt == VT_FILETIME) {
                    SYSTEMTIME st_utc, st_local;
                    FileTimeToSystemTime(&pv.filetime, &st_utc);
                    SystemTimeToTzSpecificLocalTime(NULL, &st_utc, &st_local);
                    _snprintf(date_taken, sizeof(date_taken),
                              "%04d-%02d-%02d %02d:%02d:%02d",
                              st_local.wYear, st_local.wMonth, st_local.wDay,
                              st_local.wHour, st_local.wMinute, st_local.wSecond);
                }
                PropVariantClear(&pv);
                ps->lpVtbl->Release(ps);
            }

            ViewerLoadResult* res = (ViewerLoadResult*)calloc(1, sizeof(ViewerLoadResult));
            if (res) {
                res->hwnd       = req.hwnd;
                res->request_id = req.request_id;
                res->bmp        = hbmp;
                res->w          = w;
                res->h          = h;
                memcpy(res->date_taken, date_taken, sizeof(date_taken));
                if (!PostMessageW(req.hwnd, WM_VIEWER_LOADED, 0, (LPARAM)res)) {
                    /* Window already gone */
                    if (hbmp) DeleteObject(hbmp);
                    free(res);
                }
            } else if (hbmp) {
                DeleteObject(hbmp);
            }
        }
    }
    return 0;
}

static void viewer_async_init(void) {
    if (g_vload_init) return;
    InitializeCriticalSection(&g_vload_cs);
    g_vload_event  = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_vload_thread = CreateThread(NULL, 0, viewer_load_worker, NULL, 0, NULL);
    g_vload_init   = 1;
}

static void viewer_async_request(HWND hwnd, const char* path, int req_id) {
    viewer_async_init();
    EnterCriticalSection(&g_vload_cs);
    int next = (g_vload_q_tail + 1) % VLOAD_Q_CAP;
    if (next != g_vload_q_head) {
        ViewerLoadReq* r = &g_vload_q[g_vload_q_tail];
        r->hwnd = hwnd;
        strncpy(r->path, path, MAX_PATH-1);
        r->path[MAX_PATH-1] = 0;
        r->request_id = req_id;
        g_vload_q_tail = next;
        SetEvent(g_vload_event);
    }
    LeaveCriticalSection(&g_vload_cs);
}

static void viewer_load_current(ViewerState* v) {
    /* Drop the current bitmap immediately so the UI can show "Loading…" with
       the new index. The actual decode happens on the worker thread; the
       result message bumps v->bmp + v->img_w/h once it arrives. */
    if (v->bmp) { DeleteObject(v->bmp); v->bmp = NULL; }
    v->img_w = v->img_h = 0;
    v->zoom  = 1.0f; v->pan_x = v->pan_y = 0;
    v->date_taken[0] = 0;
    v->loading = 0;
    if (v->cur_index < 0 || v->cur_index >= v->sibling_count) {
        viewer_update_title(v);
        InvalidateRect(v->hwnd, NULL, FALSE);
        return;
    }
    char fullp[MAX_PATH];
    _snprintf(fullp, MAX_PATH, "%s\\%s", v->dir, v->siblings[v->cur_index]);
    v->next_request_id++;
    v->loading = 1;
    viewer_update_title(v);
    InvalidateRect(v->hwnd, NULL, FALSE);
    viewer_async_request(v->hwnd, fullp, v->next_request_id);
}

static void viewer_update_title(ViewerState* v) {
    if (v->cur_index < 0 || v->cur_index >= v->sibling_count) return;
    WCHAR title[MAX_PATH+128];
    WCHAR wname[MAX_PATH];
    u8_to_w(v->siblings[v->cur_index], wname, MAX_PATH);
    if (v->loading) {
        _snwprintf(title, MAX_PATH+128,
                   L"%ls  —  Loading…  —  FilePathX Viewer", wname);
    } else if (v->date_taken[0]) {
        WCHAR wdate[64]; u8_to_w(v->date_taken, wdate, 64);
        _snwprintf(title, MAX_PATH+128,
                   L"%ls  (%d × %d)  —  Taken %ls  —  FilePathX Viewer",
                   wname, v->img_w, v->img_h, wdate);
    } else {
        _snwprintf(title, MAX_PATH+128,
                   L"%ls  (%d × %d)  —  FilePathX Viewer",
                   wname, v->img_w, v->img_h);
    }
    SetWindowTextW(v->hwnd, title);
}

static void viewer_next(ViewerState* v, int dir) {
    if (v->sibling_count == 0) return;
    int n = v->sibling_count;
    v->cur_index = (v->cur_index + dir + n) % n;
    viewer_load_current(v);
}

/* ---- Rename overlay ---- */
static void viewer_rename_commit(ViewerState* v);

static LRESULT CALLBACK viewer_edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        HWND parent = GetParent(hwnd);
        ViewerState* v = (ViewerState*)GetWindowLongPtrW(parent, GWLP_USERDATA);
        if (wp == VK_RETURN) { if (v) viewer_rename_commit(v); return 0; }
        if (wp == VK_ESCAPE) { if (v) viewer_rename_cancel(v); return 0; }
    }
    return CallWindowProcW(g_viewer_edit_orig_proc, hwnd, msg, wp, lp);
}

static void viewer_rename_cancel(ViewerState* v) {
    if (v->edit_hwnd) {
        DestroyWindow(v->edit_hwnd);
        v->edit_hwnd = NULL;
        SetFocus(v->hwnd);
        InvalidateRect(v->hwnd, NULL, FALSE);
    }
}

static void viewer_rename_commit(ViewerState* v) {
    if (!v->edit_hwnd) return;
    if (v->cur_index < 0 || v->cur_index >= v->sibling_count) {
        viewer_rename_cancel(v); return;
    }
    WCHAR wnew[MAX_PATH] = {0};
    GetWindowTextW(v->edit_hwnd, wnew, MAX_PATH);
    char new_stem[MAX_PATH] = {0};
    w_to_u8(wnew, new_stem, MAX_PATH);
    if (!new_stem[0]) { viewer_rename_cancel(v); return; }

    const char* old = v->siblings[v->cur_index];
    const char* dot = strrchr(old, '.');
    char new_name[MAX_PATH];
    if (dot) _snprintf(new_name, MAX_PATH, "%s%s", new_stem, dot);
    else { strncpy(new_name, new_stem, MAX_PATH-1); new_name[MAX_PATH-1] = 0; }
    if (strcmp(new_name, old) == 0) { viewer_rename_cancel(v); return; }

    char old_full[MAX_PATH], new_full[MAX_PATH];
    _snprintf(old_full, MAX_PATH, "%s\\%s", v->dir, old);
    _snprintf(new_full, MAX_PATH, "%s\\%s", v->dir, new_name);
    WCHAR wold[MAX_PATH], wnewf[MAX_PATH];
    u8_to_w(old_full, wold, MAX_PATH);
    u8_to_w(new_full, wnewf, MAX_PATH);
    if (MoveFileW(wold, wnewf)) {
        strncpy(v->siblings[v->cur_index], new_name, MAX_PATH-1);
        v->siblings[v->cur_index][MAX_PATH-1] = 0;
        viewer_update_title(v);
    }
    viewer_rename_cancel(v);
}

static HFONT g_viewer_edit_font = NULL;

static void viewer_rename_start(ViewerState* v) {
    if (v->edit_hwnd) return;
    if (v->cur_index < 0 || v->cur_index >= v->sibling_count) return;
    const char* name = v->siblings[v->cur_index];
    char stem[MAX_PATH];
    strncpy(stem, name, MAX_PATH-1); stem[MAX_PATH-1] = 0;
    char* dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = 0;
    WCHAR wstem[MAX_PATH];
    u8_to_w(stem, wstem, MAX_PATH);

    /* Layout: centred panel near bottom, comfortably above the counter.
       Panel provides the rounded background + label + hint; the EDIT is
       borderless and lives inside the panel. */
    RECT rc; GetClientRect(v->hwnd, &rc);
    int ew = 520;
    int eh = 34;
    int ex = (rc.right - ew) / 2;
    int ey = rc.bottom - 110;
    v->edit_hwnd = CreateWindowExW(0, L"EDIT", wstem,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        ex, ey, ew, eh, v->hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!v->edit_hwnd) return;
    if (!g_viewer_edit_font) {
        g_viewer_edit_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Segoe UI");
    }
    SendMessageW(v->edit_hwnd, WM_SETFONT, (WPARAM)g_viewer_edit_font, TRUE);
    /* Left/right margin inside the EDIT so text isn't glued to the border */
    SendMessageW(v->edit_hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(10, 10));
    WNDPROC prev = (WNDPROC)SetWindowLongPtrW(v->edit_hwnd, GWLP_WNDPROC,
                                              (LONG_PTR)viewer_edit_subclass);
    if (!g_viewer_edit_orig_proc) g_viewer_edit_orig_proc = prev;
    SendMessageW(v->edit_hwnd, EM_SETSEL, 0, -1);
    SetFocus(v->edit_hwnd);
}

/* ---- Delete with optional skip-warning checkbox ---- */
static void viewer_delete(ViewerState* v) {
    if (v->cur_index < 0 || v->cur_index >= v->sibling_count) return;

    WCHAR wname[MAX_PATH];
    u8_to_w(v->siblings[v->cur_index], wname, MAX_PATH);

    if (!g_viewer_skip_delete_warn) {
        TASKDIALOGCONFIG cfg = {0};
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = v->hwnd;
        cfg.hInstance = GetModuleHandleW(NULL);
        cfg.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
        cfg.pszWindowTitle      = L"Move to Recycle Bin";
        cfg.pszMainIcon         = TD_WARNING_ICON;
        cfg.pszMainInstruction  = L"Move this image to the Recycle Bin?";
        cfg.pszContent          = wname;
        cfg.pszVerificationText = L"Don't ask again this session";
        int btn = 0;
        BOOL skip = FALSE;
        HRESULT hr = TaskDialogIndirect(&cfg, &btn, NULL, &skip);
        if (FAILED(hr)) {
            int r = MessageBoxW(v->hwnd, wname,
                                L"Move to Recycle Bin?",
                                MB_YESNO | MB_ICONQUESTION);
            if (r != IDYES) return;
        } else {
            if (btn != IDYES) return;
            if (skip) g_viewer_skip_delete_warn = 1;
        }
    }

    char fullp[MAX_PATH];
    _snprintf(fullp, MAX_PATH, "%s\\%s", v->dir, v->siblings[v->cur_index]);
    /* SHFileOperation needs double-null-terminated; over-allocate and zero it. */
    WCHAR wpath[MAX_PATH + 2] = {0};
    u8_to_w(fullp, wpath, MAX_PATH);

    SHFILEOPSTRUCTW op = {0};
    op.hwnd   = v->hwnd;
    op.wFunc  = FO_DELETE;
    op.pFrom  = wpath;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) return;

    int idx = v->cur_index;
    for (int i = idx; i < v->sibling_count - 1; i++)
        strcpy(v->siblings[i], v->siblings[i+1]);
    v->sibling_count--;
    if (v->sibling_count == 0) { DestroyWindow(v->hwnd); return; }
    if (v->cur_index >= v->sibling_count) v->cur_index = v->sibling_count - 1;
    viewer_load_current(v);
}

/* ---- Toolbar layout ---- */
/* Button IDs (hover_btn values):
   1=Rename, 2=Delete, 3=Rotate Left, 4=Rotate Right, 5=Flip H, 6=Flip V. */
typedef struct { int x, y, w, h, id; const WCHAR* label; int accent_kind; } ViewerBtn;
/* accent_kind: 0=none (neutral text), 1=accent color, 2=red */

static int viewer_layout_buttons(int cw, ViewerBtn* out /* size >= 6 */) {
    int bh = 28, by = 12;
    int gap = 6;
    /* Left cluster (transform actions) — icon-like square-ish buttons */
    int transform_w = 40;
    int lx = 12;
    out[0].x = lx;                            out[0].y = by; out[0].w = transform_w; out[0].h = bh;
    out[0].id = 3; out[0].label = L"↺";       out[0].accent_kind = 0;
    out[1].x = lx + transform_w + gap;        out[1].y = by; out[1].w = transform_w; out[1].h = bh;
    out[1].id = 4; out[1].label = L"↻";       out[1].accent_kind = 0;
    out[2].x = lx + 2*(transform_w + gap);    out[2].y = by; out[2].w = transform_w; out[2].h = bh;
    out[2].id = 5; out[2].label = L"⇔";      out[2].accent_kind = 0;
    out[3].x = lx + 3*(transform_w + gap);    out[3].y = by; out[3].w = transform_w; out[3].h = bh;
    out[3].id = 6; out[3].label = L"⇕";      out[3].accent_kind = 0;
    /* Right cluster: Rename, Delete */
    int rename_w = 86, delete_w = 86;
    int rx = cw - 12 - delete_w - gap - rename_w;
    out[4].x = rx;                            out[4].y = by; out[4].w = rename_w; out[4].h = bh;
    out[4].id = 1; out[4].label = L"Rename";  out[4].accent_kind = 1;
    out[5].x = rx + rename_w + gap;           out[5].y = by; out[5].w = delete_w; out[5].h = bh;
    out[5].id = 2; out[5].label = L"Delete";  out[5].accent_kind = 2;
    return 6;
}

static int viewer_hit_btn(ViewerState* v, int x, int y) {
    ViewerBtn btns[6];
    int n = viewer_layout_buttons(v->client_w, btns);
    for (int i = 0; i < n; i++) {
        if (x >= btns[i].x && x < btns[i].x + btns[i].w &&
            y >= btns[i].y && y < btns[i].y + btns[i].h)
            return btns[i].id;
    }
    return 0;
}

/* Convert an ARGB constant (COL_*) to the BGR-packed COLORREF that GDI
   wants. Alpha is dropped since GDI ignores it here. */
static COLORREF col_to_ref(uint32_t c) {
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

/* Transform a 32-bpp DIB into a new one. op:
     0 = rotate 90° clockwise      (swaps dimensions)
     1 = rotate 90° counterclockwise (swaps dimensions)
     2 = flip horizontal
     3 = flip vertical
   Returns a fresh HBITMAP (caller deletes the old one). NULL on error. */
static HBITMAP viewer_bmp_transform(HBITMAP src, int op, int* out_w, int* out_h) {
    DIBSECTION ds = {0};
    if (GetObject(src, sizeof(ds), &ds) == 0 || !ds.dsBm.bmBits) return NULL;
    int sw = ds.dsBm.bmWidth;
    int sh = (ds.dsBm.bmHeight < 0) ? -ds.dsBm.bmHeight : ds.dsBm.bmHeight;
    int src_topdown = (ds.dsBmih.biHeight < 0);
    int stride = ds.dsBm.bmWidthBytes;
    unsigned char* sbits = (unsigned char*)ds.dsBm.bmBits;

    int dw = sw, dh = sh;
    if (op == 0 || op == 1) { dw = sh; dh = sw; }

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = dw;
    bmi.bmiHeader.biHeight = -dh;   /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* dbits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP dst = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &dbits, NULL, 0);
    DeleteDC(dc);
    if (!dst || !dbits) { if (dst) DeleteObject(dst); return NULL; }
    int dstride = dw * 4;

    for (int y = 0; y < sh; y++) {
        int src_y = src_topdown ? y : (sh - 1 - y);
        unsigned char* srow = sbits + src_y * stride;
        for (int x = 0; x < sw; x++) {
            unsigned char* sp = srow + x * 4;
            int dx, dy;
            switch (op) {
                case 0: dx = sh - 1 - y; dy = x;             break;
                case 1: dx = y;          dy = sw - 1 - x;    break;
                case 2: dx = sw - 1 - x; dy = y;             break;
                case 3: dx = x;          dy = sh - 1 - y;    break;
                default: dx = x; dy = y;                     break;
            }
            unsigned char* dp = (unsigned char*)dbits + dy * dstride + dx * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
        }
    }
    if (out_w) *out_w = dw;
    if (out_h) *out_h = dh;
    return dst;
}

/* Session-scoped record of transforms applied per image, so navigating
   away and coming back preserves rotation / flip until the app exits. */
#define VIEWER_XFORM_CAP  500
#define VIEWER_XFORM_OPS  16
typedef struct {
    char          path[MAX_PATH];
    unsigned char ops[VIEWER_XFORM_OPS];
    int           n_ops;
} ViewerXform;
static ViewerXform g_viewer_xforms[VIEWER_XFORM_CAP];
static int         g_viewer_xform_count = 0;

static ViewerXform* viewer_xform_lookup(const char* path, int create) {
    for (int i = 0; i < g_viewer_xform_count; i++) {
        if (_stricmp(g_viewer_xforms[i].path, path) == 0) return &g_viewer_xforms[i];
    }
    if (!create) return NULL;
    if (g_viewer_xform_count >= VIEWER_XFORM_CAP) {
        /* Evict oldest (simple FIFO) */
        memmove(&g_viewer_xforms[0], &g_viewer_xforms[1],
                (VIEWER_XFORM_CAP - 1) * sizeof(ViewerXform));
        g_viewer_xform_count = VIEWER_XFORM_CAP - 1;
    }
    ViewerXform* x = &g_viewer_xforms[g_viewer_xform_count++];
    strncpy(x->path, path, MAX_PATH-1); x->path[MAX_PATH-1] = 0;
    x->n_ops = 0;
    return x;
}

static void viewer_full_path(ViewerState* v, char* out, int n) {
    if (v->cur_index < 0 || v->cur_index >= v->sibling_count) { out[0] = 0; return; }
    _snprintf(out, n, "%s\\%s", v->dir, v->siblings[v->cur_index]);
}

/* Apply any saved ops to v->bmp; called from the loaded-image handler so
   navigation restores the rotation state. */
static void viewer_replay_saved_ops(ViewerState* v) {
    char full[MAX_PATH];
    viewer_full_path(v, full, MAX_PATH);
    if (!full[0] || !v->bmp) return;
    ViewerXform* x = viewer_xform_lookup(full, 0);
    if (!x || x->n_ops == 0) return;
    for (int i = 0; i < x->n_ops; i++) {
        int nw = 0, nh = 0;
        HBITMAP nb = viewer_bmp_transform(v->bmp, x->ops[i], &nw, &nh);
        if (!nb) break;
        DeleteObject(v->bmp);
        v->bmp = nb; v->img_w = nw; v->img_h = nh;
    }
}

static void viewer_apply_transform(ViewerState* v, int op) {
    if (!v->bmp) return;
    int nw = 0, nh = 0;
    HBITMAP nb = viewer_bmp_transform(v->bmp, op, &nw, &nh);
    if (!nb) return;
    DeleteObject(v->bmp);
    v->bmp = nb;
    v->img_w = nw; v->img_h = nh;
    v->zoom = 1.0f; v->pan_x = v->pan_y = 0;

    /* Remember this op so we can replay it after Next/Prev round-trip */
    char full[MAX_PATH];
    viewer_full_path(v, full, MAX_PATH);
    if (full[0]) {
        ViewerXform* x = viewer_xform_lookup(full, 1);
        if (x) {
            if (x->n_ops >= VIEWER_XFORM_OPS) {
                /* Cap reached — shift left and keep the newest */
                for (int i = 0; i < VIEWER_XFORM_OPS - 1; i++) x->ops[i] = x->ops[i+1];
                x->n_ops = VIEWER_XFORM_OPS - 1;
            }
            x->ops[x->n_ops++] = (unsigned char)op;
        }
    }
    InvalidateRect(v->hwnd, NULL, FALSE);
}

static void viewer_paint(ViewerState* v, HDC hdc) {
    RECT rc; GetClientRect(v->hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    v->client_w = cw; v->client_h = ch;

    /* Colours from the active theme */
    COLORREF t_bg      = col_to_ref(COL_BG);
    COLORREF t_text    = col_to_ref(COL_TEXT);
    COLORREF t_subtext = col_to_ref(COL_SUBTEXT);
    COLORREF t_header  = col_to_ref(COL_HEADER);
    COLORREF t_hover   = col_to_ref(COL_HOVER);
    COLORREF t_border  = col_to_ref(COL_BORDER);
    COLORREF t_accent  = col_to_ref(COL_ACCENT);
    COLORREF t_red     = col_to_ref(COL_RED);

    /* Double-buffer to avoid flicker on resize / zoom updates */
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP back = CreateCompatibleBitmap(hdc, cw, ch);
    HBITMAP oldb = (HBITMAP)SelectObject(mem, back);
    HBRUSH bg = CreateSolidBrush(t_bg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    if (!v->bmp || v->img_w <= 0 || v->img_h <= 0) {
        SetTextColor(mem, t_subtext);
        SetBkMode(mem, TRANSPARENT);
        if (v->loading) {
            WCHAR buf[128];
            if (v->sibling_count > 0) {
                _snwprintf(buf, 128, L"Loading…   %d / %d",
                           v->cur_index + 1, v->sibling_count);
            } else {
                _snwprintf(buf, 128, L"Loading…");
            }
            DrawTextW(mem, buf, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            DrawTextW(mem, L"(no image)", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else {
        float sx = (float)cw / v->img_w;
        float sy = (float)ch / v->img_h;
        float fit = sx < sy ? sx : sy;
        if (fit > 1.0f) fit = 1.0f;
        float scale = fit * v->zoom;
        int dw = (int)(v->img_w * scale + 0.5f);
        int dh = (int)(v->img_h * scale + 0.5f);
        int dx = (cw - dw) / 2 + (int)v->pan_x;
        int dy = (ch - dh) / 2 + (int)v->pan_y;

        HDC src = CreateCompatibleDC(hdc);
        HBITMAP olds = (HBITMAP)SelectObject(src, v->bmp);
        SetStretchBltMode(mem, HALFTONE);
        SetBrushOrgEx(mem, 0, 0, NULL);
        StretchBlt(mem, dx, dy, dw, dh, src, 0, 0, v->img_w, v->img_h, SRCCOPY);
        SelectObject(src, olds);
        DeleteDC(src);

        if (v->sibling_count > 1) {
            WCHAR buf[64];
            _snwprintf(buf, 64, L"%d / %d", v->cur_index + 1, v->sibling_count);
            SetTextColor(mem, t_subtext);
            SetBkMode(mem, TRANSPARENT);
            RECT rc2 = { 0, ch - 30, cw, ch - 6 };
            DrawTextW(mem, buf, -1, &rc2, DT_CENTER | DT_SINGLELINE);
        }
    }

    /* ---- Rename panel (when EDIT is active) ---- */
    if (v->edit_hwnd) {
        RECT er;
        GetWindowRect(v->edit_hwnd, &er);
        POINT tl = { er.left, er.top }, br = { er.right, er.bottom };
        ScreenToClient(v->hwnd, &tl);
        ScreenToClient(v->hwnd, &br);
        int px = tl.x - 16;
        int py = tl.y - 44;
        int pw = (br.x - tl.x) + 32;
        int ph = (br.y - tl.y) + 82;   /* label top + edit + hint below */

        /* Drop shadow (2 offset copies for soft feel) */
        HBRUSH sh1 = CreateSolidBrush(RGB(0, 0, 0));
        RECT sr1 = { px + 3, py + 4, px + pw + 3, py + ph + 4 };
        int old_mode = SetROP2(mem, R2_MASKPEN);
        FillRect(mem, &sr1, sh1);
        SetROP2(mem, old_mode);
        DeleteObject(sh1);

        /* Panel bg + border */
        HBRUSH pb = CreateSolidBrush(t_header);
        HPEN   pp = CreatePen(PS_SOLID, 1, t_border);
        HGDIOBJ ob = SelectObject(mem, pb);
        HGDIOBJ op = SelectObject(mem, pp);
        RoundRect(mem, px, py, px + pw, py + ph, 12, 12);
        SelectObject(mem, ob); SelectObject(mem, op);
        DeleteObject(pb); DeleteObject(pp);

        /* Label */
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, t_subtext);
        RECT lr = { px + 16, py + 10, px + pw - 16, py + 28 };
        DrawTextW(mem, L"Rename file", -1, &lr, DT_LEFT | DT_SINGLELINE);

        /* Hint below the EDIT */
        RECT hr = { px + 16, br.y + 10, px + pw - 16, py + ph - 8 };
        SetTextColor(mem, t_subtext);
        DrawTextW(mem, L"Enter to save   ·   Esc to cancel", -1, &hr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    /* ---- Toolbar buttons ---- */
    {
        ViewerBtn btns[6];
        int nb_btns = viewer_layout_buttons(cw, btns);
        /* Use a slightly larger font for the arrow glyphs so they're not tiny */
        HFONT bfont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Segoe UI Symbol");
        HGDIOBJ oldfnt = SelectObject(mem, bfont);
        SetBkMode(mem, TRANSPARENT);
        for (int i = 0; i < nb_btns; i++) {
            int hover = (v->hover_btn == btns[i].id);
            COLORREF accent =
                (btns[i].accent_kind == 2) ? t_red :
                (btns[i].accent_kind == 1) ? t_accent : t_accent;
            RECT br = { btns[i].x, btns[i].y,
                        btns[i].x + btns[i].w, btns[i].y + btns[i].h };
            HBRUSH bb = CreateSolidBrush(hover ? t_hover : t_header);
            HPEN   border = CreatePen(PS_SOLID, 1, hover ? accent : t_border);
            HGDIOBJ old_b = SelectObject(mem, bb);
            HGDIOBJ old_p = SelectObject(mem, border);
            RoundRect(mem, br.left, br.top, br.right, br.bottom, 10, 10);
            SelectObject(mem, old_b); SelectObject(mem, old_p);
            DeleteObject(bb); DeleteObject(border);
            SetTextColor(mem, hover ? accent : t_text);
            DrawTextW(mem, btns[i].label, -1, &br,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(mem, oldfnt);
        DeleteObject(bfont);
    }

    BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldb);
    DeleteObject(back);
    DeleteDC(mem);
}

static LRESULT CALLBACK viewer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ViewerState* v = (ViewerState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (v) viewer_paint(v, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        if (!v) break;
        if (wp == VK_LEFT)  { viewer_next(v, -1); return 0; }
        if (wp == VK_RIGHT || wp == VK_SPACE) { viewer_next(v, +1); return 0; }
        if (wp == VK_HOME)  { v->cur_index = 0; viewer_load_current(v); return 0; }
        if (wp == VK_END)   { v->cur_index = v->sibling_count - 1; viewer_load_current(v); return 0; }
        if (wp == VK_ESCAPE){ DestroyWindow(hwnd); return 0; }
        if (wp == '0')      { v->zoom = 1.0f; v->pan_x = v->pan_y = 0;
                              InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (wp == VK_F2)    { viewer_rename_start(v); return 0; }
        if (wp == VK_DELETE){ viewer_delete(v); return 0; }
        if (wp == 'R')      { viewer_apply_transform(v, 0); return 0; }  /* Rotate right */
        if (wp == 'L')      { viewer_apply_transform(v, 1); return 0; }  /* Rotate left */
        if (wp == 'H')      { viewer_apply_transform(v, 2); return 0; }  /* Flip horizontal */
        if (wp == 'V')      { viewer_apply_transform(v, 3); return 0; }  /* Flip vertical */
        break;
    case WM_MOUSEWHEEL: {
        if (!v) break;
        short delta = GET_WHEEL_DELTA_WPARAM(wp);
        float factor = (delta > 0) ? 1.2f : (1.0f / 1.2f);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        float cx = pt.x - v->client_w * 0.5f - v->pan_x;
        float cy = pt.y - v->client_h * 0.5f - v->pan_y;
        v->zoom *= factor;
        if (v->zoom < 0.05f) v->zoom = 0.05f;
        if (v->zoom > 40.0f) v->zoom = 40.0f;
        v->pan_x -= cx * (factor - 1.0f);
        v->pan_y -= cy * (factor - 1.0f);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (!v) break;
        {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int bhit = viewer_hit_btn(v, x, y);
            if (bhit == 1) { viewer_rename_start(v); return 0; }
            if (bhit == 2) { viewer_delete(v); return 0; }
            if (bhit == 3) { viewer_apply_transform(v, 1); return 0; }  /* Rotate Left */
            if (bhit == 4) { viewer_apply_transform(v, 0); return 0; }  /* Rotate Right */
            if (bhit == 5) { viewer_apply_transform(v, 2); return 0; }  /* Flip H */
            if (bhit == 6) { viewer_apply_transform(v, 3); return 0; }  /* Flip V */
            int edge = v->client_w / 6;
            if (x < edge)                 { viewer_next(v, -1); return 0; }
            if (x > v->client_w - edge)   { viewer_next(v, +1); return 0; }
        }
        SetCapture(hwnd);
        v->dragging = 1;
        v->drag_x0 = GET_X_LPARAM(lp);
        v->drag_y0 = GET_Y_LPARAM(lp);
        v->pan_x0 = v->pan_x; v->pan_y0 = v->pan_y;
        return 0;
    case WM_MOUSEMOVE:
        if (!v) return 0;
        if (v->dragging) {
            v->pan_x = v->pan_x0 + (GET_X_LPARAM(lp) - v->drag_x0);
            v->pan_y = v->pan_y0 + (GET_Y_LPARAM(lp) - v->drag_y0);
            InvalidateRect(hwnd, NULL, FALSE);
        } else {
            int new_hover = viewer_hit_btn(v, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (new_hover != v->hover_btn) {
                v->hover_btn = new_hover;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (v) { v->dragging = 0; ReleaseCapture(); }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (v) {
            v->zoom = 1.0f; v->pan_x = v->pan_y = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_VIEWER_LOADED: {
        ViewerLoadResult* res = (ViewerLoadResult*)lp;
        if (!res) return 0;
        if (!v || res->request_id != v->next_request_id) {
            /* Stale result — user already navigated past this image. */
            if (res->bmp) DeleteObject(res->bmp);
            free(res);
            return 0;
        }
        if (v->bmp) DeleteObject(v->bmp);
        v->bmp     = res->bmp;
        v->img_w   = res->w;
        v->img_h   = res->h;
        memcpy(v->date_taken, res->date_taken, sizeof(v->date_taken));
        v->loading = 0;
        free(res);
        /* Replay any rotate / flip ops stashed for this path so
           navigating away and back preserves the transform. */
        viewer_replay_saved_ops(v);
        viewer_update_title(v);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        if (!v || (HWND)lp != v->edit_hwnd) break;
        HDC hdc_e = (HDC)wp;
        COLORREF fg = col_to_ref(COL_TEXT);
        COLORREF bg = col_to_ref(COL_BG);
        SetTextColor(hdc_e, fg);
        SetBkColor  (hdc_e, bg);
        static HBRUSH cached = NULL;
        static COLORREF last_bg = 0;
        if (!cached || last_bg != bg) {
            if (cached) DeleteObject(cached);
            cached = CreateSolidBrush(bg);
            last_bg = bg;
        }
        return (LRESULT)cached;
    }
    case WM_DESTROY:
        if (v) {
            /* Bump the request id so any in-flight result is treated as stale
               and freed in WM_VIEWER_LOADED. (Posted messages still arrive
               briefly after the window goes away if SetWindowLongPtrW=0 hasn't
               happened yet; freeing in the worker via PostMessage-failure
               check covers the rest.) */
            v->next_request_id++;
            if (v->bmp) DeleteObject(v->bmp);
            if (v->siblings) free(v->siblings);
            free(v);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void open_image_viewer(const char* path) {
    HINSTANCE hi = GetModuleHandleW(NULL);
    static int registered = 0;
    if (!registered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = viewer_proc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"FilePathXViewerClass";
        wc.hIcon = LoadIconW(hi, L"IDI_APPICON");
        wc.hIconSm = LoadIconW(hi, L"IDI_APPICON");
        if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        RegisterClassExW(&wc);
        registered = 1;
    }
    ViewerState* v = (ViewerState*)calloc(1, sizeof(ViewerState));
    if (!v) return;
    v->zoom = 1.0f;
    const char* slash = strrchr(path, '\\');
    if (!slash) { free(v); return; }
    int dlen = (int)(slash - path);
    if (dlen >= MAX_PATH) dlen = MAX_PATH-1;
    memcpy(v->dir, path, dlen); v->dir[dlen] = 0;
    char startname[MAX_PATH];
    strncpy(startname, slash + 1, MAX_PATH-1); startname[MAX_PATH-1] = 0;
    viewer_collect_siblings(v, startname);

    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    int w = sx * 3 / 4, h = sy * 3 / 4;
    HWND vh = CreateWindowExW(0, L"FilePathXViewerClass", L"FilePathX Viewer",
        WS_OVERLAPPEDWINDOW,
        (sx - w) / 2, (sy - h) / 2, w, h,
        NULL, NULL, hi, NULL);
    if (!vh) { if (v->siblings) free(v->siblings); free(v); return; }
    v->hwnd = vh;
    SetWindowLongPtrW(vh, GWLP_USERDATA, (LONG_PTR)v);
    /* Match DWM title bar to theme brightness — luminance > ~50% → light bar. */
    {
        uint32_t c = COL_BG;
        int lum = (77 * ((c>>16)&0xFF) + 150 * ((c>>8)&0xFF) + 29 * (c&0xFF)) >> 8;
        BOOL dark = (lum < 128);
        DwmSetWindowAttribute(vh, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
    ShowWindow(vh, SW_SHOW);
    viewer_load_current(v);
}

static void do_open_entry(Tab* t, int idx) {
    if (idx < 0 || idx >= t->entry_count) return;
    FileEntry* e = &t->entries[idx];
    if (e->is_dir) {
        if (strcmp(e->name, "..") == 0) tab_go_up(t);
        else {
            char p[MAX_PATH];
            _snprintf(p, MAX_PATH, "%s\\%s", t->path, e->name);
            tab_navigate(t, p, 1);
        }
    } else {
        if (is_image_ext(e->name)) {
            char fullp[MAX_PATH];
            _snprintf(fullp, MAX_PATH, "%s\\%s", t->path, e->name);
            open_image_viewer(fullp);
        } else {
            WCHAR wp[MAX_PATH];
            path_join_w(t->path, e->name, wp, MAX_PATH);
            open_file_async(wp);
        }
    }
}

/* ---- Context menu ---- */
static void handle_context_cmd(int cmd, int item_idx) {
    Tab* t = active_tab();
    switch (cmd) {
    case IDM_OPEN:
        do_open_entry(t, item_idx);
        break;
    case IDM_OPEN_TAB:
        if (item_idx >= 0 && t->entries[item_idx].is_dir) {
            char p[MAX_PATH];
            if (strcmp(t->entries[item_idx].name, "..") == 0)
                { char tmp[MAX_PATH]; strncpy(tmp, t->path, MAX_PATH); PathRemoveFileSpecA(tmp); new_tab(tmp); }
            else
                { _snprintf(p, MAX_PATH, "%s\\%s", t->path, t->entries[item_idx].name); new_tab(p); }
        }
        break;
    case IDM_COPY:
        if (sel_count(t) > 0) clipboard_set_selected(t, DROPEFFECT_COPY);
        break;
    case IDM_CUT:
        if (sel_count(t) > 0) clipboard_set_selected(t, DROPEFFECT_MOVE);
        break;
    case IDM_PASTE:
        clipboard_paste(t->path);
        scan_directory(t);
        break;
    case IDM_DELETE:
        if (sel_count(t) > 0) {
            do_delete_selected(t);
            scan_directory(t);
        }
        break;
    case IDM_RENAME:
        do_rename(t, item_idx);
        break;
    case IDM_NEW_FOLDER:
        do_new_folder(t);
        break;
    case IDM_NEW_FILE:
        do_new_file(t);
        break;
    case IDM_REFRESH:
        scan_directory(t);
        break;
    case IDM_OPEN_TERMINAL: {
        char args[MAX_PATH + 8];
        _snprintf(args, sizeof(args), "-d \"%s\"", t->path);
        WCHAR wargs[MAX_PATH + 8], wtpath[MAX_PATH];
        u8_to_w(args, wargs, MAX_PATH + 8);
        u8_to_w(t->path, wtpath, MAX_PATH);
        HINSTANCE hi = ShellExecuteW(NULL, L"open", L"wt.exe", wargs, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hi <= 32)
            ShellExecuteW(NULL, L"open", L"cmd.exe", NULL, wtpath, SW_SHOWNORMAL);
        break;
    }
    case IDM_ADD_BOOKMARK:
        if (item_idx >= 0 && t->entries[item_idx].is_dir) {
            char fp[MAX_PATH];
            _snprintf(fp, MAX_PATH, "%s\\%s", t->path, t->entries[item_idx].name);
            bookmark_add(fp);
        }
        break;
    case IDM_COPY_PATH:
        if (item_idx >= 0) {
            char p[MAX_PATH];
            _snprintf(p, MAX_PATH, "%s\\%s", t->path, t->entries[item_idx].name);
            clipboard_copy_text(p);
        }
        break;
    case IDM_OPEN_WITH:
        if (item_idx >= 0) {
            WCHAR wp[MAX_PATH];
            path_join_w(t->path, t->entries[item_idx].name, wp, MAX_PATH);
            /* Windows "Open with" chooser dialog */
            SHELLEXECUTEINFOW sei = {0};
            sei.cbSize = sizeof(sei);
            sei.hwnd   = g_hwnd;
            sei.lpVerb = L"openas";
            sei.lpFile = wp;
            sei.nShow  = SW_SHOWNORMAL;
            sei.fMask  = SEE_MASK_INVOKEIDLIST;
            ShellExecuteExW(&sei);
        }
        break;
    case IDM_PROPERTIES: {
        char p[MAX_PATH];
        if (item_idx >= 0)
            _snprintf(p, MAX_PATH, "%s\\%s", t->path, t->entries[item_idx].name);
        else
            strncpy(p, t->path, MAX_PATH);
        do_properties(p);
        break;
    }
    }
    g_needs_redraw = 1;
}

static int entry_to_row(Tab* t, int idx) {
    if (idx < 0 || idx >= t->entry_count) return -1;
    if (!t->use_groups) return idx;
    int ri = 0, prev_group = -1;
    for (int i = 0; i < t->entry_count; i++) {
        FileEntry* e = &t->entries[i];
        if (strcmp(e->name, "..") == 0) {
            if (i == idx) return ri;
            ri++;
            continue;
        }
        if (e->group != prev_group) {
            prev_group = e->group;
            ri++;
        }
        if (t->group_collapsed[e->group]) {
            if (i == idx) return -1;
        } else {
            if (i == idx) return ri;
            ri++;
        }
    }
    return -1;
}

static void scroll_to_entry(Tab* t, int idx) {
    if (idx < 0 || idx >= t->entry_count) return;
    if (t->use_groups && strcmp(t->entries[idx].name, "..") != 0) {
        int gi = t->entries[idx].group;
        if (t->group_collapsed[gi]) t->group_collapsed[gi] = 0;
    }
    int r = entry_to_row(t, idx);
    if (r < 0) return;
    float lh = (float)(g_height - CONTENT_TOP - COL_HDR_H - STATUS_BAR_H);
    float ry = (float)r * ROW_H;
    /* If already comfortably in view (top half not too high, bottom not clipped), leave it. */
    float margin = ROW_H * 2;
    if (ry >= t->target_scroll + margin && ry + ROW_H <= t->target_scroll + lh - margin) return;
    /* Otherwise center the entry in the viewport. */
    t->target_scroll = ry + ROW_H * 0.5f - lh * 0.5f;
    if (t->target_scroll < 0) t->target_scroll = 0;
}

/* ---- Selection helpers ---- */
static void sel_clear(Tab* t) { memset(t->sel_mask, 0, sizeof(t->sel_mask)); }

static void sel_only(Tab* t, int i) {
    sel_clear(t);
    if (i >= 0 && i < t->entry_count) t->sel_mask[i] = 1;
    t->sel_anchor = i;
    t->selected = i;
}

static void sel_range(Tab* t, int from, int to) {
    sel_clear(t);
    if (from < 0 || to < 0) return;
    if (from > to) { int x = from; from = to; to = x; }
    if (to >= t->entry_count) to = t->entry_count - 1;
    for (int i = from; i <= to; i++)
        if (strcmp(t->entries[i].name, "..") != 0) t->sel_mask[i] = 1;
}

static int sel_count(Tab* t) {
    int c = 0;
    for (int i = 0; i < t->entry_count; i++) if (t->sel_mask[i]) c++;
    return c;
}

static int row_to_entry(Tab* t, int row) {
    if (row < 0) return -1;
    if (!t->use_groups) {
        return (row < t->entry_count) ? row : -1;
    }
    int ri = 0, prev_group = -1;
    for (int i = 0; i < t->entry_count; i++) {
        FileEntry* e = &t->entries[i];
        if (strcmp(e->name, "..") == 0) {
            if (ri == row) return i;
            ri++;
            continue;
        }
        if (e->group != prev_group) {
            prev_group = e->group;
            if (ri == row) return -1; /* header row */
            ri++;
        }
        if (!t->group_collapsed[e->group]) {
            if (ri == row) return i;
            ri++;
        }
    }
    return -1;
}

static int has_image_ext(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char* exts[] = {
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".tif", ".tiff",
        ".heic", ".heif", ".raw", ".cr2", ".nef", ".arw", ".dng", ".ico",
        ".jfif", ".avif"
    };
    for (size_t i = 0; i < sizeof(exts)/sizeof(*exts); i++)
        if (_stricmp(dot, exts[i]) == 0) return 1;
    return 0;
}

/* Open a file by invoking the default verb via its parent folder's IContextMenu.
   For image files, pass PIDLs of ALL image siblings so UWP viewers (Photos)
   enable gallery next/prev navigation. */
static int open_via_shell_context(HWND hwnd, const char* dir_u8, const char* name_u8) {
    WCHAR wdir[MAX_PATH], wname[MAX_PATH];
    u8_to_w(dir_u8, wdir, MAX_PATH);
    u8_to_w(name_u8, wname, MAX_PATH);

    LPITEMIDLIST dir_pidl = NULL;
    if (SHParseDisplayName(wdir, NULL, &dir_pidl, 0, NULL) != S_OK || !dir_pidl) return 0;

    IShellFolder* desktop = NULL;
    if (SHGetDesktopFolder(&desktop) != S_OK) { CoTaskMemFree(dir_pidl); return 0; }
    IShellFolder* folder = NULL;
    HRESULT hr = IShellFolder_BindToObject(desktop, dir_pidl, NULL, &IID_IShellFolder, (void**)&folder);
    IShellFolder_Release(desktop);
    CoTaskMemFree(dir_pidl);
    if (FAILED(hr) || !folder) return 0;

    LPITEMIDLIST target = NULL;
    ULONG eaten = 0;
    int ok = 0;
    if (FAILED(IShellFolder_ParseDisplayName(folder, NULL, NULL, wname, &eaten, &target, NULL))
        || !target) {
        IShellFolder_Release(folder);
        return 0;
    }

    /* Build PIDL array. For images, enumerate folder and put target first. */
    LPCITEMIDLIST* arr = NULL;
    LPITEMIDLIST*  arr_owned = NULL;
    int n = 1;
    int img_mode = has_image_ext(name_u8);

    if (img_mode) {
        IEnumIDList* en = NULL;
        if (SUCCEEDED(IShellFolder_EnumObjects(folder, hwnd,
                          SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &en)) && en) {
            int cap = 256;
            arr_owned = (LPITEMIDLIST*)calloc(cap, sizeof(LPITEMIDLIST));
            arr_owned[0] = target; /* target first */
            n = 1;
            LPITEMIDLIST pidl = NULL;
            ULONG fetched = 0;
            while (en->lpVtbl->Next(en, 1, &pidl, &fetched) == S_OK && fetched == 1) {
                STRRET sret = {0};
                if (SUCCEEDED(IShellFolder_GetDisplayNameOf(folder, pidl, SHGDN_FORPARSING, &sret))) {
                    WCHAR wbuf[MAX_PATH] = {0};
                    StrRetToBufW(&sret, pidl, wbuf, MAX_PATH);
                    char nbuf[MAX_PATH]; w_to_u8(wbuf, nbuf, MAX_PATH);
                    const char* nm = strrchr(nbuf, '\\');
                    nm = nm ? nm + 1 : nbuf;
                    if (has_image_ext(nm) && _stricmp(nm, name_u8) != 0) {
                        if (n >= cap) { cap *= 2; arr_owned = (LPITEMIDLIST*)realloc(arr_owned, cap*sizeof(LPITEMIDLIST)); }
                        arr_owned[n++] = pidl;
                        continue;
                    }
                }
                CoTaskMemFree(pidl);
            }
            en->lpVtbl->Release(en);
        }
    }
    if (!arr_owned) {
        arr_owned = (LPITEMIDLIST*)calloc(1, sizeof(LPITEMIDLIST));
        arr_owned[0] = target;
        n = 1;
    }
    arr = (LPCITEMIDLIST*)arr_owned;

    /* If a UWP app is the default handler, use ActivateForFile (Photos etc.) */
    if (img_mode) {
        WCHAR aumid[256] = {0};
        DWORD aumid_len = 256;
        const char* dot = strrchr(name_u8, '.');
        if (dot) {
            WCHAR wext[16] = {0};
            u8_to_w(dot, wext, 16);
            if (SUCCEEDED(AssocQueryStringW(0, ASSOCSTR_APPID, wext, NULL, aumid, &aumid_len))
                && aumid[0]) {
                IShellItemArray* items = NULL;
                if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(n, arr, &items)) && items) {
                    IApplicationActivationManager* aam = NULL;
                    if (SUCCEEDED(CoCreateInstance(&CLSID_ApplicationActivationManager, NULL,
                                                    CLSCTX_LOCAL_SERVER,
                                                    &IID_IApplicationActivationManager,
                                                    (void**)&aam)) && aam) {
                        DWORD pid = 0;
                        if (SUCCEEDED(aam->lpVtbl->ActivateForFile(aam, aumid, items, L"open", &pid)))
                            ok = 1;
                        aam->lpVtbl->Release(aam);
                    }
                    items->lpVtbl->Release(items);
                }
                if (ok) goto cleanup;
            }
        }
    }

    IContextMenu* ctx = NULL;
    if (SUCCEEDED(IShellFolder_GetUIObjectOf(folder, hwnd, n, arr,
                                              &IID_IContextMenu, NULL, (void**)&ctx)) && ctx) {
        char dir_ansi[MAX_PATH];
        strncpy(dir_ansi, dir_u8, MAX_PATH-1); dir_ansi[MAX_PATH-1] = 0;
        CMINVOKECOMMANDINFOEX ici = {0};
        ici.cbSize       = sizeof(ici);
        ici.fMask        = CMIC_MASK_UNICODE;
        ici.hwnd         = hwnd;
        ici.lpVerb       = "open";
        ici.lpVerbW      = L"open";
        ici.lpDirectory  = dir_ansi;
        ici.lpDirectoryW = wdir;
        ici.nShow        = SW_SHOWNORMAL;
        if (SUCCEEDED(IContextMenu_InvokeCommand(ctx, (CMINVOKECOMMANDINFO*)&ici))) ok = 1;
        IContextMenu_Release(ctx);
    }

cleanup:
    for (int i = 0; i < n; i++) CoTaskMemFree(arr_owned[i]);
    free(arr_owned);
    IShellFolder_Release(folder);
    return ok;
}

static int shell_item_unwanted(const char* text) {
    /* Skip duplicates of our own menu + clutter from common shell extensions */
    static const char* bl[] = {
        "Open", "Open with", "Cut", "Copy", "Paste", "Delete", "Rename", "Properties",
        "Copy as path", "Share", "Create shortcut",
        "Open in File Pilot", "Add to Favorites", "Pin to Quick access",
        "Include in library", "Pin to Start", "Restore previous versions",
        "Set as desktop background", "Give access to",
    };
    for (size_t i = 0; i < sizeof(bl)/sizeof(*bl); i++)
        if (_stricmp(text, bl[i]) == 0) return 1;
    if (_strnicmp(text, "Edit with ",   10) == 0) return 1;
    if (_strnicmp(text, "Create with ", 12) == 0) return 1;
    if (_strnicmp(text, "Combine files in ", 17) == 0) return 1;
    if (_strnicmp(text, "Convert to ", 11) == 0) return 1;
    if (_strnicmp(text, "Rotate ",     7) == 0) return 1;
    if (_strnicmp(text, "Scan with ",  10) == 0) return 1;
    if (_strnicmp(text, "Open in File Pilot", 18) == 0) return 1;
    return 0;
}

static void filter_shell_menu(HMENU menu, int start_pos) {
    /* Walk backward and delete unwanted named items */
    int count = GetMenuItemCount(menu);
    for (int i = count - 1; i >= start_pos; i--) {
        char text[256] = {0};
        int len = GetMenuStringA(menu, i, text, sizeof(text) - 1, MF_BYPOSITION);
        if (len == 0) continue; /* separator or owner-drawn */
        /* Strip '&' shortcuts and tab-accelerators for comparison */
        char clean[256]; int o = 0;
        for (int k = 0; text[k] && o < (int)sizeof(clean) - 1; k++) {
            if (text[k] == '&') continue;
            if (text[k] == '\t') break;
            clean[o++] = text[k];
        }
        clean[o] = 0;
        if (shell_item_unwanted(clean))
            DeleteMenu(menu, i, MF_BYPOSITION);
    }
    /* Collapse consecutive separators and trim trailing separator */
    count = GetMenuItemCount(menu);
    int prev_sep = 1;
    for (int i = start_pos; i < count; ) {
        MENUITEMINFOA mi = {0}; mi.cbSize = sizeof(mi); mi.fMask = MIIM_FTYPE;
        if (!GetMenuItemInfoA(menu, i, TRUE, &mi)) { i++; continue; }
        int is_sep = (mi.fType & MFT_SEPARATOR) != 0;
        if (is_sep && prev_sep) { DeleteMenu(menu, i, MF_BYPOSITION); count--; continue; }
        prev_sep = is_sep;
        i++;
    }
    /* Trim trailing separator */
    count = GetMenuItemCount(menu);
    if (count > start_pos) {
        MENUITEMINFOA mi = {0}; mi.cbSize = sizeof(mi); mi.fMask = MIIM_FTYPE;
        if (GetMenuItemInfoA(menu, count - 1, TRUE, &mi) && (mi.fType & MFT_SEPARATOR))
            DeleteMenu(menu, count - 1, MF_BYPOSITION);
    }
}

/* Append Windows Shell context menu items to `menu` for the currently-selected
   files in tab `t`. Returns IContextMenu* (caller releases) or NULL. */
static IContextMenu* build_shell_menu(HMENU menu, HWND hwnd, Tab* t, UINT first_id) {
    /* Collect selected names */
    int idxs[MAX_ENTRIES], n = 0;
    for (int i = 0; i < t->entry_count; i++) {
        if (t->sel_mask[i] && strcmp(t->entries[i].name, "..") != 0)
            idxs[n++] = i;
    }
    if (n == 0) return NULL;

    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, t->path, -1, wpath, MAX_PATH);
    LPITEMIDLIST parent_pidl = NULL;
    if (SHParseDisplayName(wpath, NULL, &parent_pidl, 0, NULL) != S_OK || !parent_pidl) return NULL;
    IShellFolder* desktop = NULL;
    if (SHGetDesktopFolder(&desktop) != S_OK) { CoTaskMemFree(parent_pidl); return NULL; }
    IShellFolder* folder = NULL;
    HRESULT hr = IShellFolder_BindToObject(desktop, parent_pidl, NULL, &IID_IShellFolder, (void**)&folder);
    IShellFolder_Release(desktop);
    CoTaskMemFree(parent_pidl);
    if (FAILED(hr) || !folder) return NULL;

    LPITEMIDLIST* pidls = (LPITEMIDLIST*)calloc(n, sizeof(LPITEMIDLIST));
    int got = 0;
    for (int i = 0; i < n; i++) {
        WCHAR wname[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, t->entries[idxs[i]].name, -1, wname, MAX_PATH);
        ULONG eaten = 0;
        if (IShellFolder_ParseDisplayName(folder, NULL, NULL, wname, &eaten, &pidls[got], NULL) == S_OK
            && pidls[got])
            got++;
    }
    if (got == 0) { free(pidls); IShellFolder_Release(folder); return NULL; }

    IContextMenu* ctx = NULL;
    hr = IShellFolder_GetUIObjectOf(folder, hwnd, got, (LPCITEMIDLIST*)pidls,
                                     &IID_IContextMenu, NULL, (void**)&ctx);
    if (SUCCEEDED(hr) && ctx) {
        int shell_start = GetMenuItemCount(menu);
        IContextMenu_QueryContextMenu(ctx, menu, shell_start,
                                       first_id, first_id + 0x7FFF, CMF_NORMAL | CMF_EXPLORE);
        filter_shell_menu(menu, shell_start);
    } else {
        ctx = NULL;
    }

    for (int i = 0; i < got; i++) CoTaskMemFree(pidls[i]);
    free(pidls);
    IShellFolder_Release(folder);
    return ctx;
}

static void invoke_shell_command(HWND hwnd, IContextMenu* ctx, Tab* t, UINT cmd_idx) {
    CMINVOKECOMMANDINFO ici = {0};
    ici.cbSize = sizeof(ici);
    ici.hwnd   = hwnd;
    ici.lpVerb = (LPCSTR)MAKEINTRESOURCEA(cmd_idx);
    ici.nShow  = SW_SHOWNORMAL;
    ici.lpDirectory = t->path;
    IContextMenu_InvokeCommand(ctx, &ici);
}

static void show_context_menu(HWND hwnd, int mx, int my) {
    /* Focus follows right-click: in split mode, right-clicking on the inactive
       panel needs to switch the active panel BEFORE we resolve active_tab(),
       otherwise the hit-test would index the wrong panel's entries. */
    float split_x_full = SIDEBAR_W + g_app.split_ratio * (g_width - SIDEBAR_W);
    if (g_app.split_active && mx >= SIDEBAR_W && my >= TAB_BAR_H) {
        g_app.active_panel = (mx < split_x_full) ? 0 : 1;
    }
    Tab* t = active_tab();
    /* Per-panel file-list bounds */
    float lx = SIDEBAR_W;
    float lw = (float)g_width - SIDEBAR_W;
    if (g_app.split_active) {
        if (g_app.active_panel == 0) { lw = split_x_full - SIDEBAR_W; }
        else                         { lx = split_x_full; lw = g_width - split_x_full; }
    }
    float ly = CONTENT_TOP + COL_HDR_H;
    float lh = (float)g_height - CONTENT_TOP - COL_HDR_H - STATUS_BAR_H;
    /* In icon view modes there's no column header — the file list starts at
       CONTENT_TOP directly. */
    if (t->view_mode != VM_DETAILS) {
        ly = CONTENT_TOP;
        lh = (float)g_height - CONTENT_TOP - STATUS_BAR_H;
    }

    /* Sidebar bookmark right-click */
    int bm_idx = -1;
    if (sidebar_hit_bookmark(mx, my, &bm_idx)) {
        HMENU bmenu = CreatePopupMenu();
        AppendMenuA(bmenu, MF_STRING, IDM_REMOVE_BOOKMARK, "Remove from bookmarks");
        POINT pt = { mx, my };
        ClientToScreen(hwnd, &pt);
        int cmd = TrackPopupMenu(bmenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(bmenu);
        if (cmd == IDM_REMOVE_BOOKMARK) {
            bookmark_remove_at(bm_idx);
            g_needs_redraw = 1;
        }
        return;
    }

    /* Only show context menu when right-click is inside the file list area */
    if (mx < lx || my < ly || mx >= lx + lw || my >= ly + lh) return;

    int on_item = -1;
    if (t->view_mode == VM_DETAILS) {
        int row = (int)((my - ly + t->scroll_y) / ROW_H);
        on_item = row_to_entry(t, row);
    } else {
        /* Icon grid hit-test — mirror build_file_list layout. ".." is hidden
           in icon views so we count over visible entries only. */
        int item_w = (t->view_mode == VM_SMALL_ICONS) ? 90  : 180;
        int item_h = (t->view_mode == VM_SMALL_ICONS) ? 100 : 180;
        float rw = lw - 8;
        int cols = (int)(rw / item_w); if (cols < 1) cols = 1;
        int col = (int)((mx - lx - 4) / item_w);
        int row = (int)((my - ly - 4 + t->scroll_y) / item_h);
        if (col >= 0 && col < cols && row >= 0) {
            int target_slot = row * cols + col;
            int slot = 0;
            for (int i = 0; i < t->entry_count; i++) {
                if (strcmp(t->entries[i].name, "..") == 0) continue;
                if (slot == target_slot) { on_item = i; break; }
                slot++;
            }
        }
    }
    /* If right-clicked on an unselected item, make it the only selection */
    if (on_item >= 0 && !t->sel_mask[on_item]) sel_only(t, on_item);
    else if (on_item >= 0) t->selected = on_item;

    int has_clip = IsClipboardFormatAvailable(CF_HDROP);
    int is_dotdot = (on_item >= 0 && strcmp(t->entries[on_item].name, "..") == 0);

    HMENU menu = CreatePopupMenu();
    if (on_item >= 0) {
        FileEntry* e = &t->entries[on_item];
        /* --- Open group (top) --- */
        AppendMenuA(menu, MF_STRING, IDM_OPEN, "Open");
        if (e->is_dir)
            AppendMenuA(menu, MF_STRING, IDM_OPEN_TAB, "Open in New Tab");
        if (!e->is_dir && !is_dotdot)
            AppendMenuA(menu, MF_STRING, IDM_OPEN_WITH, "Open with...");
        AppendMenuA(menu, is_dotdot ? MF_GRAYED : MF_STRING, IDM_COPY_PATH, "Copy Path");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        /* --- Clipboard --- */
        AppendMenuA(menu, is_dotdot ? MF_GRAYED : MF_STRING, IDM_CUT, "Cut\tCtrl+X");
        AppendMenuA(menu, is_dotdot ? MF_GRAYED : MF_STRING, IDM_COPY, "Copy\tCtrl+C");
        AppendMenuA(menu, has_clip ? MF_STRING : MF_GRAYED, IDM_PASTE, "Paste\tCtrl+V");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        /* --- File ops --- */
        AppendMenuA(menu, is_dotdot ? MF_GRAYED : MF_STRING, IDM_RENAME, "Rename\tF2");
        AppendMenuA(menu, is_dotdot ? MF_GRAYED : MF_STRING, IDM_DELETE, "Delete\tDel");
        if (e->is_dir && !is_dotdot) {
            char fp[MAX_PATH];
            _snprintf(fp, MAX_PATH, "%s\\%s", t->path, e->name);
            int already = bookmark_find(fp) >= 0;
            AppendMenuA(menu, already ? MF_GRAYED : MF_STRING, IDM_ADD_BOOKMARK,
                        already ? "Add to bookmarks (already added)" : "Add to bookmarks");
        }
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        /* --- Misc --- */
        AppendMenuA(menu, MF_STRING, IDM_OPEN_TERMINAL, "Open Terminal\tCtrl+D");
        AppendMenuA(menu, MF_STRING, IDM_PROPERTIES, "Properties");
    } else {
        AppendMenuA(menu, MF_STRING, IDM_NEW_FOLDER, "New Folder");
        AppendMenuA(menu, MF_STRING, IDM_NEW_FILE, "New Text File");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, has_clip ? MF_STRING : MF_GRAYED, IDM_PASTE, "Paste\tCtrl+V");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, MF_STRING, IDM_REFRESH, "Refresh\tF5");
        AppendMenuA(menu, MF_STRING, IDM_OPEN_TERMINAL, "Open Terminal\tCtrl+D");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, MF_STRING, IDM_PROPERTIES, "Properties");
    }

    /* Append Windows Shell context menu (Send To, WinRAR, Git, etc.) */
    UINT shell_first = 5000;
    IContextMenu* shell_ctx = NULL;
    if (on_item >= 0) {
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        shell_ctx = build_shell_menu(menu, hwnd, t, shell_first);
        if (shell_ctx) {
            IContextMenu_QueryInterface(shell_ctx, &IID_IContextMenu2, (void**)&g_shell_ctx2);
            IContextMenu_QueryInterface(shell_ctx, &IID_IContextMenu3, (void**)&g_shell_ctx3);
        }
    }

    POINT pt = { mx, my };
    ClientToScreen(hwnd, &pt);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);

    if (cmd >= shell_first && shell_ctx) {
        invoke_shell_command(hwnd, shell_ctx, t, cmd - shell_first);
        scan_directory(t);
    } else if (cmd) {
        handle_context_cmd(cmd, on_item);
    }

    if (g_shell_ctx3) { IContextMenu3_Release(g_shell_ctx3); g_shell_ctx3 = NULL; }
    if (g_shell_ctx2) { IContextMenu2_Release(g_shell_ctx2); g_shell_ctx2 = NULL; }
    if (shell_ctx)    IContextMenu_Release(shell_ctx);
}

/* ================================================================
 *  BUILD UI
 * ================================================================ */

static void build_window_buttons(void) {
    Renderer* r = &g_renderer;
    float w = (float)g_width;
    float btn_y = 0, btn_h = TAB_BAR_H;
    float close_x = w - WIN_BTN_W;
    float max_x   = close_x - WIN_BTN_W;
    float min_x   = max_x - WIN_BTN_W;
    /* Minimize */
    {
        int hov = ui_hover(&g_ui, min_x, btn_y, WIN_BTN_W, btn_h);
        if (hov) render_quad(r, min_x, btn_y, WIN_BTN_W, btn_h, COL_HOVER);
        float isz = 10;
        render_mdl2(r, ICON_MINIMIZE, min_x + (WIN_BTN_W-isz)/2, btn_y + (btn_h-isz)/2, isz,
                    hov ? COL_TEXT : COL_SUBTEXT);
        if (ui_clicked(&g_ui, 190, min_x, btn_y, WIN_BTN_W, btn_h))
            ShowWindow(g_hwnd, SW_MINIMIZE);
    }
    /* Maximize / Restore */
    {
        int is_max = IsZoomed(g_hwnd);
        int hov = ui_hover(&g_ui, max_x, btn_y, WIN_BTN_W, btn_h);
        if (hov) render_quad(r, max_x, btn_y, WIN_BTN_W, btn_h, COL_HOVER);
        float isz = 10;
        render_mdl2(r, is_max ? ICON_RESTORE : ICON_MAXIMIZE,
                    max_x + (WIN_BTN_W-isz)/2, btn_y + (btn_h-isz)/2, isz,
                    hov ? COL_TEXT : COL_SUBTEXT);
        if (ui_clicked(&g_ui, 191, max_x, btn_y, WIN_BTN_W, btn_h))
            ShowWindow(g_hwnd, is_max ? SW_RESTORE : SW_MAXIMIZE);
    }
    /* Close */
    {
        int hov = ui_hover(&g_ui, close_x, btn_y, WIN_BTN_W, btn_h);
        if (hov) render_quad(r, close_x, btn_y, WIN_BTN_W, btn_h, 0xFFE81123);
        float isz = 10;
        render_mdl2(r, ICON_CLOSE, close_x + (WIN_BTN_W-isz)/2, btn_y + (btn_h-isz)/2, isz,
                    hov ? COL_TEXT : COL_SUBTEXT);
        if (ui_clicked(&g_ui, 192, close_x, btn_y, WIN_BTN_W, btn_h))
            PostQuitMessage(0);
    }
    /* ---- Logo (rendered in top-left, shared) ---- */
    if (g_logo_tex) {
        float lsz = TAB_BAR_H - 8;
        render_icon(r, g_logo_tex, 4, (TAB_BAR_H - lsz) / 2, lsz, lsz);
    } else {
        render_text(r, "FPX", 6, (TAB_BAR_H - r->font_height) / 2, COL_ACCENT);
    }
}

/* Render tab strip for current panel into [tabs_xmin, tabs_xmax] horizontal range.
   `tabs_xmax` is the rightmost coordinate available (window buttons / split edge). */
static void build_tab_bar(float tabs_xmin, float tabs_xmax) {
    Renderer* r = &g_renderer;

    /* Background for this panel's strip */
    render_quad(r, tabs_xmin, 0, tabs_xmax - tabs_xmin, TAB_BAR_H, COL_MANTLE);

    /* ---- Tabs ---- */
    float tabs_x0  = tabs_xmin;
    float tab_w    = 160;
    float tab_gap  = 2;
    float max_tab_x = tabs_xmax;
    float total_w  = g_app.panels[g_app.active_panel].tab_count * (tab_w + tab_gap);

    /* Reserve right-side button slot: when overflow, show [<][>][+]; else just [+] */
    float btn_w   = 22;
    float btns_w  = 28;
    int   overflow_estimate = (total_w > max_tab_x - tabs_x0 - btns_w);
    if (overflow_estimate) btns_w = btn_w * 2 + 4 + 28;
    float tabs_x1 = max_tab_x - btns_w;
    float visible_w = tabs_x1 - tabs_x0;
    if (visible_w < 0) visible_w = 0;

    int overflow = (total_w > visible_w);
    float max_scroll = overflow ? (total_w - visible_w) : 0;
    if (g_app.panels[g_app.active_panel].tab_scroll_target > max_scroll) g_app.panels[g_app.active_panel].tab_scroll_target = max_scroll;
    if (g_app.panels[g_app.active_panel].tab_scroll_target < 0) g_app.panels[g_app.active_panel].tab_scroll_target = 0;
    /* Smooth animation toward target */
    g_app.panels[g_app.active_panel].tab_scroll += (g_app.panels[g_app.active_panel].tab_scroll_target - g_app.panels[g_app.active_panel].tab_scroll) * 0.35f;
    if (g_app.panels[g_app.active_panel].tab_scroll < 0) g_app.panels[g_app.active_panel].tab_scroll = 0;
    if (g_app.panels[g_app.active_panel].tab_scroll > max_scroll) g_app.panels[g_app.active_panel].tab_scroll = max_scroll;
    float d = g_app.panels[g_app.active_panel].tab_scroll_target - g_app.panels[g_app.active_panel].tab_scroll;
    if (d > 0.5f || d < -0.5f) g_needs_redraw = 1;
    else g_app.panels[g_app.active_panel].tab_scroll = g_app.panels[g_app.active_panel].tab_scroll_target;

    /* Drag only applies to the panel where it started */
    int drag_here = (g_tab_drag_panel == g_app.active_panel);
    int drag_target = g_tab_drag_idx;
    if (drag_here && g_tab_drag_idx >= 0 && g_tab_drag_active) {
        float lx = (float)g_mouse_x - (float)g_tab_drag_offset - tabs_x0 + g_app.panels[g_app.active_panel].tab_scroll;
        drag_target = (int)((lx + (tab_w + tab_gap) / 2.0f) / (tab_w + tab_gap));
        if (drag_target < 0) drag_target = 0;
        if (drag_target >= g_app.panels[g_app.active_panel].tab_count) drag_target = g_app.panels[g_app.active_panel].tab_count - 1;
    }

    render_scissor(r, (int)tabs_x0, 0, (int)visible_w, TAB_BAR_H);
    int last_tab_right = (int)tabs_x0;
    for (int i = 0; i < g_app.panels[g_app.active_panel].tab_count; i++) {
        if (drag_here && g_tab_drag_active && i == g_tab_drag_idx) continue;

        int slot = i;
        if (drag_here && g_tab_drag_active) {
            if (g_tab_drag_idx < drag_target) {
                if (i > g_tab_drag_idx && i <= drag_target) slot = i - 1;
            } else {
                if (i >= drag_target && i < g_tab_drag_idx) slot = i + 1;
            }
        }
        float tx = tabs_x0 + slot * (tab_w + tab_gap) - g_app.panels[g_app.active_panel].tab_scroll;

        if (tx + tab_w <= tabs_x0) continue;
        if (tx >= tabs_x1)         continue;

        int is_partial_right = (tx + tab_w > tabs_x1);
        int saved_mx = 0, mx_was_masked = 0;
        int mx_actual = g_ui.input.mouse_x;

        /* For partial tabs, suppress ui_tab's own hit-test entirely (mask
           the cursor for the duration of the call) and then run a single
           clipped ui_clicked below. This way the close-X — visually clipped
           past tabs_x1 — can never fire, and the visible part of the tab
           strictly maps to "switch tab". */
        if ((drag_here && g_tab_drag_active) || is_partial_right) {
            saved_mx = g_ui.input.mouse_x;
            g_ui.input.mouse_x = -10000;
            mx_was_masked = 1;
        }
        uint32_t accent = (g_app.active_panel == g_focused_panel) ? COL_ACCENT : COL_DIM;
        int res = ui_tab(&g_ui, UIID(100 + i), tx, 2, tab_w, TAB_BAR_H - 2,
                         g_app.panels[g_app.active_panel].tabs[i].title,
                         i == g_app.panels[g_app.active_panel].active_tab,
                         get_file_icon("folder", 1),
                         accent);
        if (mx_was_masked) g_ui.input.mouse_x = saved_mx;

        /* Clipped click for partial tabs: only the visible body
           [tx, tabs_x1) counts. Cursor in chevron/+ area passes through to
           those buttons (they run after the loop). */
        if (is_partial_right && !(drag_here && g_tab_drag_active)) {
            float vis_w = (float)((int)tabs_x1 - (int)tx);
            if (vis_w > 0 && ui_clicked(&g_ui, UIID(100 + i),
                                        tx, 2, vis_w, TAB_BAR_H - 2)) {
                res = 1;
            }
        }

        if (res == 1) {
            g_app.panels[g_app.active_panel].active_tab = i;
            Tab* nt = &g_app.panels[g_app.active_panel].tabs[i];
            if (!nt->use_groups) {
                int sc = g_app.sort_col, sa = g_app.sort_asc;
                sort_prefs_lookup(nt->path, &sc, &sa);
                g_app.sort_col = sc; g_app.sort_asc = sa;
            }
            watch_start(nt->path);
            tabs_save();
            g_needs_redraw = 1;
        }
        if (res == 2) { render_scissor_reset(r); close_tab(i); return; }
        /* Track rightmost edge of any rendered tab body, clamped to the
           clip boundary. WM_NCHITTEST uses this to decide HTCLIENT vs
           HTCAPTION — without this, the visible portion of a partial tab
           was treated as title bar (clicks ignored, double-click maximised
           the window). */
        {
            int r_end = (int)(tx + tab_w);
            if (r_end > (int)tabs_x1) r_end = (int)tabs_x1;
            if (r_end > last_tab_right) last_tab_right = r_end;
        }

        int hov = ui_hover(&g_ui, tx, 2, tab_w, TAB_BAR_H - 2);
        if (hov && g_ui.input.mouse_clicked && g_tab_drag_idx < 0 && !mx_was_masked) {
            g_tab_drag_idx    = i;
            g_tab_drag_panel  = g_app.active_panel;
            g_tab_drag_x0     = g_mouse_x;
            g_tab_drag_offset = g_mouse_x - (int)tx;
        }
    }

    if (drag_here && g_tab_drag_active && g_tab_drag_idx >= 0 && g_tab_drag_idx < g_app.panels[g_app.active_panel].tab_count) {
        float drag_tx = (float)(g_mouse_x - g_tab_drag_offset);
        int saved_mx = g_ui.input.mouse_x;
        g_ui.input.mouse_x = -10000;
        ui_tab(&g_ui, UIID(9999), drag_tx, 2, tab_w, TAB_BAR_H - 2,
               g_app.panels[g_app.active_panel].tabs[g_tab_drag_idx].title,
               g_tab_drag_idx == g_app.panels[g_app.active_panel].active_tab,
               get_file_icon("folder", 1),
               (g_app.active_panel == g_focused_panel) ? COL_ACCENT : COL_DIM);
        g_ui.input.mouse_x = saved_mx;
        g_needs_redraw = 1;
    }
    render_scissor_reset(r);
    g_tab_bar_tabs_end_x = last_tab_right;

    /* Right-side controls: scroll arrows (if overflow) + "+" */
    float bx = tabs_x1 + 2;
    g_tab_bar_btns_start_x = (int)bx;
    float step = tab_w + tab_gap;
    if (overflow) {
        if (ui_mdl2_btn(&g_ui, UIID(197), bx, 5, btn_w, TAB_BAR_H - 10, ICON_CHEVRON_LEFT, 10,
                        g_app.panels[g_app.active_panel].tab_scroll_target > 0)) {
            g_app.panels[g_app.active_panel].tab_scroll_target -= step;
            if (g_app.panels[g_app.active_panel].tab_scroll_target < 0) g_app.panels[g_app.active_panel].tab_scroll_target = 0;
            g_needs_redraw = 1;
        }
        if (ui_hover(&g_ui, bx, 5, btn_w, TAB_BAR_H - 10))
            tt_set("Scroll tabs left", (int)(bx + btn_w/2), 5);
        bx += btn_w;
        if (ui_mdl2_btn(&g_ui, UIID(198), bx, 5, btn_w, TAB_BAR_H - 10, ICON_CHEVRON_RIGHT, 10,
                        g_app.panels[g_app.active_panel].tab_scroll_target < max_scroll)) {
            g_app.panels[g_app.active_panel].tab_scroll_target += step;
            if (g_app.panels[g_app.active_panel].tab_scroll_target > max_scroll) g_app.panels[g_app.active_panel].tab_scroll_target = max_scroll;
            g_needs_redraw = 1;
        }
        if (ui_hover(&g_ui, bx, 5, btn_w, TAB_BAR_H - 10))
            tt_set("Scroll tabs right", (int)(bx + btn_w/2), 5);
        bx += btn_w + 4;
    }
    if (ui_mdl2_btn(&g_ui, UIID(199), bx, 5, 24, TAB_BAR_H - 10, ICON_ADD, 10, 1))
        new_tab(active_tab()->path);
    if (ui_hover(&g_ui, bx, 5, 24, TAB_BAR_H - 10))
        tt_set("New tab in current folder  (Ctrl+T)", (int)(bx + 12), 5);
    g_tab_bar_btn_end_x = (int)(bx + 24);

    /* Wheel over tab area scrolls horizontally */
    if (overflow &&
        g_mouse_y >= 0 && g_mouse_y < TAB_BAR_H &&
        g_mouse_x >= tabs_x0 && g_mouse_x < tabs_x1 + btns_w &&
        g_scroll_delta != 0) {
        g_app.panels[g_app.active_panel].tab_scroll_target -= g_scroll_delta * step;
        if (g_app.panels[g_app.active_panel].tab_scroll_target < 0) g_app.panels[g_app.active_panel].tab_scroll_target = 0;
        if (g_app.panels[g_app.active_panel].tab_scroll_target > max_scroll) g_app.panels[g_app.active_panel].tab_scroll_target = max_scroll;
        g_scroll_delta = 0;
        g_needs_redraw = 1;
    }

    /* Auto-scroll active tab into view ONLY when active tab actually changes. */
    if (overflow && g_app.panels[g_app.active_panel].active_tab >= 0 && g_app.panels[g_app.active_panel].active_tab != g_app.panels[g_app.active_panel].tab_scroll_last_active) {
        float a_x = tabs_x0 - g_app.panels[g_app.active_panel].tab_scroll_target + g_app.panels[g_app.active_panel].active_tab * step;
        if (a_x < tabs_x0) g_app.panels[g_app.active_panel].tab_scroll_target -= (tabs_x0 - a_x);
        else if (a_x + tab_w > tabs_x1) g_app.panels[g_app.active_panel].tab_scroll_target += (a_x + tab_w - tabs_x1);
        if (g_app.panels[g_app.active_panel].tab_scroll_target < 0) g_app.panels[g_app.active_panel].tab_scroll_target = 0;
        if (g_app.panels[g_app.active_panel].tab_scroll_target > max_scroll) g_app.panels[g_app.active_panel].tab_scroll_target = max_scroll;
    }
    g_app.panels[g_app.active_panel].tab_scroll_last_active = g_app.panels[g_app.active_panel].active_tab;

    /* Drag threshold + commit logic only for the panel where drag started */
    if (drag_here) {
        if (g_tab_drag_idx >= 0 && g_mouse_down && !g_tab_drag_active) {
            int dx = g_mouse_x - g_tab_drag_x0;
            if (dx*dx > 25) { g_tab_drag_active = 1; g_needs_redraw = 1; }
        }
        if (!g_mouse_down && g_tab_drag_idx >= 0) {
            if (g_tab_drag_active && drag_target != g_tab_drag_idx &&
                drag_target >= 0 && drag_target < g_app.panels[g_app.active_panel].tab_count) {
                Tab tmp = g_app.panels[g_app.active_panel].tabs[g_tab_drag_idx];
                int was_active = (g_app.panels[g_app.active_panel].active_tab == g_tab_drag_idx);
                if (g_tab_drag_idx < drag_target) {
                    for (int k = g_tab_drag_idx; k < drag_target; k++)
                        g_app.panels[g_app.active_panel].tabs[k] = g_app.panels[g_app.active_panel].tabs[k + 1];
                } else {
                    for (int k = g_tab_drag_idx; k > drag_target; k--)
                        g_app.panels[g_app.active_panel].tabs[k] = g_app.panels[g_app.active_panel].tabs[k - 1];
                }
                g_app.panels[g_app.active_panel].tabs[drag_target] = tmp;
                if (was_active) g_app.panels[g_app.active_panel].active_tab = drag_target;
                else if (g_app.panels[g_app.active_panel].active_tab > g_tab_drag_idx && g_app.panels[g_app.active_panel].active_tab <= drag_target)
                    g_app.panels[g_app.active_panel].active_tab--;
                else if (g_app.panels[g_app.active_panel].active_tab < g_tab_drag_idx && g_app.panels[g_app.active_panel].active_tab >= drag_target)
                    g_app.panels[g_app.active_panel].active_tab++;
                tabs_save();
            }
            g_tab_drag_idx    = -1;
            g_tab_drag_panel  = -1;
            g_tab_drag_active = 0;
        }
    }
}

static void build_toolbar(float x0, float x1) {
    Renderer* r = &g_renderer;
    Tab* t = active_tab();
    float y = TAB_BAR_H;
    float w = x1 - x0;

    render_quad(r, x0, y, w, TOOLBAR_H, COL_HEADER);

    float bx = x0 + 4;
    if (ui_mdl2_btn(&g_ui, UIID(200), bx, y+2, 26, TOOLBAR_H-4, ICON_ARROW_BACK, 12, t->hist_pos > 0))
        tab_go_back(t);
    if (ui_hover(&g_ui, bx, y+2, 26, TOOLBAR_H-4))
        tt_set("Back  (Alt+Left)", (int)(bx + 13), (int)(y + 2));
    bx += 28;
    if (ui_mdl2_btn(&g_ui, UIID(201), bx, y+2, 26, TOOLBAR_H-4, ICON_ARROW_FWD, 12, t->hist_pos < t->hist_count-1))
        tab_go_forward(t);
    if (ui_hover(&g_ui, bx, y+2, 26, TOOLBAR_H-4))
        tt_set("Forward  (Alt+Right)", (int)(bx + 13), (int)(y + 2));
    bx += 28;
    if (ui_mdl2_btn(&g_ui, UIID(202), bx, y+2, 26, TOOLBAR_H-4, ICON_ARROW_UP, 12, strlen(t->path) > 3))
        tab_go_up(t);
    if (ui_hover(&g_ui, bx, y+2, 26, TOOLBAR_H-4))
        tt_set("Up  (Backspace)", (int)(bx + 13), (int)(y + 2));
    bx += 30;

    /* Separator after navigation buttons */
    render_quad(r, bx, y+6, 1, TOOLBAR_H-12, COL_BORDER);
    bx += 8;

    /* Bookmark toggle for current folder */
    {
        int bm_idx = bookmark_find(t->path);
        float bm_w = 22, bm_h = TOOLBAR_H - 4;
        int bm_hov = ui_hover(&g_ui, bx, y+2, bm_w, bm_h);
        if (bm_hov) render_quad(r, bx, y+2, bm_w, bm_h, COL_HOVER);
        float iw = 10, ih = 14;
        draw_bookmark(r, bx + (bm_w - iw) / 2, y + (TOOLBAR_H - ih) / 2, iw, ih,
                      bm_idx >= 0 ? COL_YELLOW : COL_SUBTEXT);
        if (bm_hov) tt_set(bm_idx >= 0 ? "Remove this folder from bookmarks"
                                       : "Add this folder to bookmarks",
                           (int)(bx + bm_w / 2), (int)(y + 2));
        if (ui_clicked(&g_ui, UIID(203), bx, y+2, bm_w, bm_h)) {
            if (bm_idx >= 0) bookmark_remove_at(bm_idx);
            else bookmark_add(t->path);
        }
        bx += bm_w;
    }
    bx += 6;

    /* Separator after bookmark icon */
    render_quad(r, bx, y+6, 1, TOOLBAR_H-12, COL_BORDER);
    bx += 8;

    /* Kebab (Settings) at the far right of the toolbar — 3 vertical dots */
    float kb_w = 26;
    float kb_x = x1 - kb_w - 4;
    {
        int hov = ui_hover(&g_ui, kb_x, y+2, kb_w, TOOLBAR_H-4);
        if (hov) render_quad(r, kb_x, y+2, kb_w, TOOLBAR_H-4, COL_HOVER);
        uint32_t dot_col = hov ? COL_TEXT : COL_SUBTEXT;
        float ds = 2;                    /* dot size */
        float dg = 2;                    /* gap between dots */
        float dx = kb_x + (kb_w - ds) / 2;
        float cy = y + TOOLBAR_H / 2;
        render_quad(r, dx, cy - ds - dg - ds/2, ds, ds, dot_col);
        render_quad(r, dx, cy - ds/2,           ds, ds, dot_col);
        render_quad(r, dx, cy + dg + ds/2,      ds, ds, dot_col);
        if (hov) tt_set("Settings", (int)(kb_x + kb_w/2), (int)(y + 2));
        if (ui_clicked(&g_ui, UIID(210), kb_x, y+2, kb_w, TOOLBAR_H-4)) {
            g_settings_open = 1;
            g_needs_redraw = 1;
        }
    }

    /* Breadcrumb (shrunk to leave room for kebab) */
    float crumb_x0 = bx;
    float crumb_w  = kb_x - bx - 4;
    int addr_here = (g_addr_hwnd && g_addr_panel == g_app.active_panel);
    if (addr_here) {
        int nx = (int)crumb_x0, ny = (int)(y+3);
        int nw = (int)crumb_w,  nh = TOOLBAR_H-6;
        if (nx != g_addr_x || ny != g_addr_y || nw != g_addr_w || nh != g_addr_h) {
            g_addr_x = nx; g_addr_y = ny; g_addr_w = nw; g_addr_h = nh;
            SetWindowPos(g_addr_hwnd, NULL, nx, ny, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
            addr_apply_round_region(nw, nh);
            int fh = r->font_height;
            int top = (nh - fh) / 2; if (top < 0) top = 0;
            RECT fmt = { 6, top, nw - 6, top + fh };
            SendMessageA(g_addr_hwnd, EM_SETRECT, 0, (LPARAM)&fmt);
        }
        render_round_rect(r, (float)g_addr_x, (float)g_addr_y,
                          (float)g_addr_w, (float)g_addr_h, COL_SELECTED);
    } else {
        Crumb crumbs[MAX_CRUMBS];
        int nc = parse_breadcrumbs(t->path, crumbs);
        float cx = bx, ty = y + (TOOLBAR_H - r->font_height) / 2;

        render_scissor(r, (int)cx, (int)y, (int)(x1 - cx), TOOLBAR_H);
        for (int i = 0; i < nc; i++) {
            if (i > 0) {
                cx += 4;
                render_mdl2(r, ICON_CHEVRON_RIGHT, cx, y + (TOOLBAR_H - 8) / 2, 8, COL_DIM);
                cx += 12;
            }
            int tw = render_text_width(r, crumbs[i].label);
            int is_last = (i == nc - 1);
            int hov = ui_hover(&g_ui, cx, y, (float)tw, TOOLBAR_H);
            render_text(r, crumbs[i].label, cx, ty,
                        is_last ? COL_TEXT : (hov ? COL_ACCENT : COL_SUBTEXT));
            if (!is_last && ui_clicked(&g_ui, UIID(250 + i), cx, y, (float)tw, TOOLBAR_H))
                tab_navigate(t, crumbs[i].path, 1);
            cx += tw;
        }
        render_scissor_reset(r);
        /* Empty-area click activates path edit (only fires if no crumb claimed click) */
        if (ui_clicked(&g_ui, UIID(260), crumb_x0, y, crumb_w, TOOLBAR_H))
            addr_edit_start(crumb_x0, y+3, crumb_w, TOOLBAR_H-6);
    }
    render_quad(r, x0, y + TOOLBAR_H - 1, w, 1, COL_BORDER);
}

static void build_sidebar(void) {
    Renderer* r = &g_renderer;
    Tab* t = active_tab();
    float x = 0, w = SIDEBAR_W, y = TAB_BAR_H, h = g_height - TAB_BAR_H;

    render_quad(r, x, y, w, h, COL_HEADER);
    render_scissor(r, 0, (int)y, (int)w, (int)h);

    static const int sec_icons[] = { ICON_BOOKMARK, ICON_STORAGE, ICON_PLACES };
    float sy = y + 4;
    for (int s = 0; s < g_app.section_count; s++) {
        SidebarSection* sec = &g_app.sections[s];
        int icon = (s == BOOKMARKS_SECTION) ? -1 : sec_icons[s];
        if (ui_section(&g_ui, 300+s, x, sy, w, 22, sec->title, sec->expanded, icon)) {
            sec->expanded = !sec->expanded;
            g_needs_redraw = 1;
        }
        if (s == BOOKMARKS_SECTION)
            draw_bookmark(r, x + 16, sy + (22 - 13) / 2, 9, 13, COL_SUBTEXT);
        sy += 22;
        if (!sec->expanded) continue;
        float section_first_iy = sy;
        for (int i = 0; i < sec->item_count; i++) {
            SidebarItem* it = &sec->items[i];
            float iy = sy;
            int hov = ui_hover(&g_ui, x, iy, w, 21);
            int active = (_stricmp(t->path, it->path) == 0);
            int is_dragged = (s == BOOKMARKS_SECTION && g_bm_drag_active && g_bm_drag_idx == i);
            if (is_dragged) {
                /* dim the source row while dragging */
                render_quad(r, x+2, iy, w-4, 21, 0x33000000);
            } else if (active) render_quad(r, x+2, iy, w-4, 21, COL_SELECTED);
            else if (hov) render_quad(r, x+2, iy, w-4, 21, COL_HOVER);
            float ty = iy + (21 - r->font_height) / 2;
            if (it->icon_tex)
                render_icon(r, it->icon_tex, x+12, iy+2, ICON_SIZE, ICON_SIZE);
            else
                render_folder_icon(r, x+14, iy+5, 11, it->icon_color);
            render_text(r, it->name, x+32, ty, COL_TEXT);
            if (it->short_path[0]) {
                int nw = render_text_width(r, it->name);
                float px = x + 32 + nw + 4;
                if (px + 20 < w) {
                    render_text(r, ">", px, ty, COL_DIM);
                    px += render_text_width(r, "> ");
                    int mp = (int)(w - px - 4);
                    if (mp > 10) {
                        int sl = (int)strlen(it->short_path);
                        while (sl > 0 && render_text_width_n(r, it->short_path, sl) > mp) sl--;
                        render_text_n(r, it->short_path, sl, px, ty, COL_DIM);
                    }
                }
            }
            /* Begin bookmark drag on mouse press inside item */
            if (s == BOOKMARKS_SECTION && hov && g_ui.input.mouse_clicked && g_bm_drag_idx < 0) {
                g_bm_drag_idx    = i;
                g_bm_drag_y0     = g_mouse_y;
                g_bm_drag_active = 0;
            }
            int suppress_click = (s == BOOKMARKS_SECTION && g_bm_drag_active && g_bm_drag_idx == i);
            if (ui_clicked(&g_ui, 400+s*20+i, x, iy, w, 21) && !suppress_click) {
                if (strncmp(it->path, "shell:", 6) == 0) {
                    WCHAR wsp[MAX_PATH]; u8_to_w(it->path, wsp, MAX_PATH);
                    ShellExecuteW(NULL, L"open", wsp, NULL, NULL, SW_SHOWNORMAL);
                }
                else
                    tab_navigate(t, it->path, 1);
            }
            sy += 21;
        }
        /* ---- Bookmark drag: compute drop slot, render indicator ---- */
        if (s == BOOKMARKS_SECTION && g_bm_drag_idx >= 0) {
            if (g_mouse_down) {
                int dy = g_mouse_y - g_bm_drag_y0;
                if (!g_bm_drag_active && (dy > 5 || dy < -5)) g_bm_drag_active = 1;
                if (g_bm_drag_active) {
                    /* Slot 0..item_count, computed against item centres */
                    int slot = sec->item_count;
                    for (int i = 0; i < sec->item_count; i++) {
                        float centre = section_first_iy + i * 21 + 10.5f;
                        if (g_mouse_y < centre) { slot = i; break; }
                    }
                    /* Don't show indicator at "no-op" positions (source row's
                       own edge or just past it) */
                    if (slot != g_bm_drag_idx && slot != g_bm_drag_idx + 1) {
                        float dy_px = section_first_iy + slot * 21;
                        render_quad(r, x + 2, dy_px - 1, w - 4, 2, COL_ACCENT);
                    }
                    g_bm_drop_slot = slot;
                    g_needs_redraw = 1;
                }
            } else {
                /* Released: commit reorder if it was a real drag */
                if (g_bm_drag_active && g_bm_drop_slot >= 0 &&
                    g_bm_drop_slot != g_bm_drag_idx &&
                    g_bm_drop_slot != g_bm_drag_idx + 1) {
                    SidebarItem moved = sec->items[g_bm_drag_idx];
                    int dst = g_bm_drop_slot;
                    if (dst > g_bm_drag_idx) dst--;  /* account for own removal */
                    if (g_bm_drag_idx < dst) {
                        for (int k = g_bm_drag_idx; k < dst; k++)
                            sec->items[k] = sec->items[k+1];
                    } else {
                        for (int k = g_bm_drag_idx; k > dst; k--)
                            sec->items[k] = sec->items[k-1];
                    }
                    sec->items[dst] = moved;
                    bookmarks_save();
                }
                g_bm_drag_idx    = -1;
                g_bm_drag_active = 0;
                g_bm_drop_slot   = -1;
            }
        }
        sy += 4;
    }
    render_scissor_reset(r);
    render_quad(r, SIDEBAR_W-1, CONTENT_TOP, 1, h, COL_BORDER);

    /* Theme picker moved to Settings modal (open with kebab in toolbar). */
}

static void build_column_headers(float lx, float ly, float lw) {
    Renderer* r = &g_renderer;
    render_quad(r, lx, ly, lw, COL_HDR_H, COL_HEADER);
    float right = lx + lw - 10;
    float size_x = right - COLW_SIZE;
    float date_x = size_x - COLW_DATE;
    float type_x = date_x - COLW_TYPE;
    float ty = ly + (COL_HDR_H - g_renderer.font_height) / 2;

    struct { const char* label; float x, w; int col; } cols[] = {
        {"Name", lx+28, type_x-lx-28, 0}, {"Type", type_x, COLW_TYPE, 1},
        {"Date Modified", date_x, COLW_DATE, 2}, {"Size", size_x, COLW_SIZE, 5},
    };
    for (int i = 0; i < 4; i++) {
        int hov = ui_hover(&g_ui, cols[i].x, ly, cols[i].w, COL_HDR_H);
        render_text(r, cols[i].label, cols[i].x+4, ty, hov ? COL_TEXT : COL_SUBTEXT);
        if (g_app.sort_col == cols[i].col) {
            int tw = render_text_width(r, cols[i].label);
            render_mdl2(r, g_app.sort_asc ? ICON_CHEVRON_UP : ICON_CHEVRON_DOWN,
                        cols[i].x+tw+8, ly + (COL_HDR_H - 8) / 2, 8, COL_ACCENT);
        }
        if (ui_clicked(&g_ui, UIID(500+i), cols[i].x, ly, cols[i].w, COL_HDR_H)) {
            if (g_app.sort_col == cols[i].col) g_app.sort_asc = !g_app.sort_asc;
            else { g_app.sort_col = cols[i].col; g_app.sort_asc = 1; }
            Tab* tab = active_tab();
            /* Persist for every folder — Downloads no longer overrides;
               it simply switches back to date-grouping when the user
               picks date-desc again. */
            sort_prefs_set(tab->path, g_app.sort_col, g_app.sort_asc);
            scan_directory(tab);
        }
        if (i > 0) render_quad(r, cols[i].x-1, ly+4, 1, COL_HDR_H-8, COL_BORDER);
    }
    render_quad(r, lx, ly+COL_HDR_H-1, lw, 1, COL_BORDER);
}

static void build_file_list(float lx, float ly, float lw, float lh) {
    Renderer* r = &g_renderer;
    Tab* t = active_tab();

    render_quad(r, lx, ly, lw, lh, COL_BG);
    render_scissor(r, (int)lx, (int)ly, (int)lw, (int)lh);

    /* Icon grid view modes (small/large) — separate render path */
    if (t->view_mode != VM_DETAILS) {
        int item_w  = (t->view_mode == VM_SMALL_ICONS) ? 90  : 180;
        int item_h  = (t->view_mode == VM_SMALL_ICONS) ? 100 : 180;
        int icon_sz = (t->view_mode == VM_SMALL_ICONS) ? 48  : 130;
        float rw = lw - 8;
        int cols = (int)(rw / item_w); if (cols < 1) cols = 1;
        /* Count entries excluding ".." which we hide in icon views */
        int visible_count = 0;
        for (int i = 0; i < t->entry_count; i++) {
            if (strcmp(t->entries[i].name, "..") != 0) visible_count++;
        }
        int row_count_g = (visible_count + cols - 1) / cols;
        float content_h = (float)row_count_g * item_h + 8;

        int hover_this = (g_mouse_x >= lx && g_mouse_x < lx + lw &&
                          g_mouse_y >= ly && g_mouse_y < ly + lh);
        if (g_scroll_delta != 0 && (hover_this || g_sync_scroll)) {
            t->target_scroll -= g_scroll_delta * item_h;
            if (!g_sync_scroll) g_scroll_delta = 0;
        }
        float max_scroll = content_h - lh;
        if (max_scroll < 0) max_scroll = 0;
        if (t->target_scroll < 0) t->target_scroll = 0;
        if (t->target_scroll > max_scroll) t->target_scroll = max_scroll;
        t->scroll_y += (t->target_scroll - t->scroll_y) * 0.3f;
        if (t->scroll_y < 0) t->scroll_y = 0;
        if (t->scroll_y > max_scroll) t->scroll_y = max_scroll;
        float scroll_gap = t->target_scroll - t->scroll_y;
        if (scroll_gap < 0) scroll_gap = -scroll_gap;
        if (scroll_gap > 0.5f) g_needs_redraw = 1;
        /* When the user is mid-scroll, don't enqueue thumbnail decodes for
           items they're about to scroll past — the queue would fill with
           stale work and the items currently under their finger would wait.
           Threshold = 2 rows: small overshoot is fine, big throws are not. */
        int scrolling_fast = (scroll_gap > (float)(item_h * 2));

        int slot = 0;
        for (int i = 0; i < t->entry_count; i++) {
            if (strcmp(t->entries[i].name, "..") == 0) continue;
            int row = slot / cols, col = slot % cols;
            slot++;
            float ix = lx + 4 + col * item_w;
            float iy = floorf(ly + 4 + row * item_h - t->scroll_y);
            if (iy + item_h < ly || iy > ly + lh) continue;

            float iw = item_w - 4, ih = item_h - 4;
            int hov = ui_hover(&g_ui, ix, iy, iw, ih);
            int sel = t->sel_mask[i];
            if (sel) render_quad(r, ix, iy, iw, ih, COL_SELECTED);
            else if (hov) render_quad(r, ix, iy, iw, ih, COL_HOVER);

            FileEntry* e = &t->entries[i];

            { /* SMALL & LARGE share grid layout: icon centered top, name below */
                char fullp[MAX_PATH];
                _snprintf(fullp, MAX_PATH, "%s\\%s", t->path, e->name);
                GLuint thumb = 0;
                int tw_img = 0, th_img = 0;
                if (strcmp(e->name, "..") != 0) {
                    int tci = thumb_cache_find(fullp);
                    if (tci >= 0) {
                        thumb = g_thumb_cache[tci].texture;
                        tw_img = g_thumb_cache[tci].w;
                        th_img = g_thumb_cache[tci].h;
                        g_thumb_cache[tci].last_used = g_thumb_frame;
                    } else if (!scrolling_fast) {
                        thumb_request(fullp);
                    }
                }
                float ic_box = (float)icon_sz;
                float ic_x = ix + (iw - ic_box) / 2;
                float ic_y = iy + 10;
                if (thumb && tw_img > 0 && th_img > 0) {
                    float aspect = (float)tw_img / (float)th_img;
                    float dw = ic_box, dh = ic_box;
                    if (aspect > 1.0f) dh = ic_box / aspect;
                    else if (aspect < 1.0f) dw = ic_box * aspect;
                    render_icon(r, thumb, ic_x + (ic_box - dw)/2, ic_y + (ic_box - dh)/2, dw, dh);
                } else {
                    /* Placeholder while thumbnail loads (or for "..") — vector
                       shapes scale cleanly, no pixelation. */
                    if (e->is_dir) {
                        render_folder_icon(r, ic_x, ic_y + ic_box * 0.15f,
                                           ic_box, COL_YELLOW);
                    } else {
                        render_file_icon(r, ic_x + ic_box * 0.15f,
                                         ic_y + ic_box * 0.05f,
                                         ic_box * 0.7f, COL_SUBTEXT);
                    }
                }
                float ty = iy + icon_sz + 18;
                int is_batch_here = (g_batch_active && g_batch_panel == g_app.active_panel &&
                                     t->sel_mask[i]);
                int is_edit_here  = (g_edit_idx == i && g_edit_panel == g_app.active_panel);
                if (is_batch_here) {
                    char stem[MAX_PATH], extp[MAX_PATH];
                    split_stem_ext(e->name, stem, extp, MAX_PATH);
                    int slen = (int)strlen(stem);
                    int cl = g_batch_chop_left;  if (cl > slen) cl = slen;
                    int cr = g_batch_chop;       if (cr > slen - cl) cr = slen - cl;
                    int mid_len = slen - cl - cr;
                    int pfx_w  = (g_batch_prefix_len > 0) ? render_text_width(r, g_batch_prefix) : 0;
                    int mid_w  = (mid_len > 0) ? render_text_width_n(r, stem + cl, mid_len) : 0;
                    int sfx_w  = (g_batch_typed_len > 0)  ? render_text_width(r, g_batch_typed) : 0;
                    int ext_w  = extp[0] ? render_text_width(r, extp) : 0;
                    int total  = pfx_w + mid_w + sfx_w + 2 + (ext_w ? ext_w + 3 : 0);
                    float nx = ix + (iw - total) / 2;
                    if (g_batch_prefix_len > 0) nx += render_text(r, g_batch_prefix, nx, ty, COL_GREEN);
                    if (g_batch_focus == 1) {
                        render_quad(r, nx + 1, ty - 2, 1, r->font_height + 4, COL_TEXT);
                        nx += 2;
                    }
                    if (mid_len > 0) nx += render_text_n(r, stem + cl, mid_len, nx, ty, COL_TEXT);
                    if (g_batch_focus == 0) {
                        render_quad(r, nx + 1, ty - 2, 1, r->font_height + 4, COL_TEXT);
                        nx += 2;
                    }
                    if (g_batch_typed_len > 0) nx += render_text(r, g_batch_typed, nx, ty, COL_GREEN);
                    if (extp[0]) render_text(r, extp, nx + 3, ty, COL_DIM);
                } else if (!is_edit_here) {
                    int nl = (int)strlen(e->name);
                    while (nl > 0 && render_text_width_n(r, e->name, nl) > iw - 8) nl--;
                    int tw = render_text_width_n(r, e->name, nl);
                    render_text_n(r, e->name, nl, ix + (iw - tw) / 2, ty, COL_TEXT);
                }
            }

            /* Drag-press detection */
            if (hov && g_ui.input.mouse_clicked && g_drag_idx < 0) {
                g_drag_idx   = i;
                g_drag_panel = g_app.active_panel;
                g_drag_x0    = g_mouse_x;
                g_drag_y0    = g_mouse_y;
                int ctrl_p  = GetKeyState(VK_CONTROL) < 0;
                int shift_p = GetKeyState(VK_SHIFT) < 0;
                if (!ctrl_p && !shift_p && !t->sel_mask[i] &&
                    strcmp(e->name, "..") != 0) sel_only(t, i);
            }

            /* Clamp click bounds to file list area so partial-bottom items
               don't steal clicks meant for the status bar below */
            float c_h = ih, c_y = iy;
            if (iy < ly) { c_h -= (ly - iy); c_y = ly; }
            if (c_y + c_h > ly + lh) c_h = (ly + lh) - c_y;
            if (c_h > 0 && ui_clicked(&g_ui, UIID(1000 + i), ix, c_y, iw, c_h)) {
                DWORD now = GetTickCount();
                int ctrl  = GetKeyState(VK_CONTROL) < 0;
                int shift = GetKeyState(VK_SHIFT) < 0;
                if (g_app.panels[g_app.active_panel].last_click_item == i &&
                    (now - g_app.panels[g_app.active_panel].last_click_time) < 400 &&
                    !ctrl && !shift) {
                    do_open_entry(t, i);
                    g_app.panels[g_app.active_panel].last_click_item = -1;
                } else {
                    if (shift) {
                        int from = (t->sel_anchor >= 0) ? t->sel_anchor : i;
                        sel_range(t, from, i); t->selected = i;
                    } else if (ctrl) {
                        if (strcmp(e->name, "..") != 0) {
                            t->sel_mask[i] = !t->sel_mask[i];
                            t->selected = i; t->sel_anchor = i;
                        }
                    } else { sel_only(t, i); }
                    g_app.panels[g_app.active_panel].last_click_item = i;
                    g_app.panels[g_app.active_panel].last_click_time = now;
                }
            }
        }

        /* Drag-out + scrollbar */
        if (g_drag_idx >= 0 && g_drag_panel == g_app.active_panel && g_mouse_down) {
            int dx = g_mouse_x - g_drag_x0;
            int dy = g_mouse_y - g_drag_y0;
            if (dx*dx + dy*dy > 25) { g_drag_idx = -1; start_drag_out(t); }
        }
        if (!g_mouse_down) { g_drag_idx = -1; g_drag_panel = -1; }

        /* ---- Marquee (rubber-band) selection ---- */
        {
            int mouse_in_list = (g_mouse_x >= lx && g_mouse_x < lx + lw &&
                                 g_mouse_y >= ly && g_mouse_y < ly + lh);
            int ctrl_p = GetKeyState(VK_CONTROL) < 0;
            /* Start: mouse press inside list, no item was clicked (g_drag_idx still -1) */
            if (!g_marquee_active && g_drag_idx < 0 &&
                g_ui.input.mouse_clicked && mouse_in_list) {
                g_marquee_active = 1;
                g_marquee_panel  = g_app.active_panel;
                g_marquee_x0     = (float)g_mouse_x - lx;
                g_marquee_y0     = (float)g_mouse_y - ly + t->scroll_y;
                g_marquee_additive = ctrl_p;
                if (ctrl_p) memcpy(g_marquee_anchor, t->sel_mask, sizeof(g_marquee_anchor));
                else        { memset(g_marquee_anchor, 0, sizeof(g_marquee_anchor));
                              sel_clear(t); t->selected = -1; t->sel_anchor = -1; }
            }
            /* Update + draw */
            if (g_marquee_active && g_marquee_panel == g_app.active_panel && g_mouse_down) {
                float mx_c = (float)g_mouse_x - lx;
                float my_c = (float)g_mouse_y - ly + t->scroll_y;
                float rx0 = g_marquee_x0 < mx_c ? g_marquee_x0 : mx_c;
                float rx1 = g_marquee_x0 < mx_c ? mx_c        : g_marquee_x0;
                float ry0 = g_marquee_y0 < my_c ? g_marquee_y0 : my_c;
                float ry1 = g_marquee_y0 < my_c ? my_c        : g_marquee_y0;
                /* Update selection based on item content rects */
                int s = 0;
                for (int i = 0; i < t->entry_count; i++) {
                    if (strcmp(t->entries[i].name, "..") == 0) continue;
                    int rr = s / cols, cc = s % cols;
                    s++;
                    float ix_c = 4 + cc * item_w;
                    float iy_c = 4 + rr * item_h;
                    float iw_c = item_w - 4, ih_c = item_h - 4;
                    int hit = !(ix_c > rx1 || ix_c + iw_c < rx0 ||
                                iy_c > ry1 || iy_c + ih_c < ry0);
                    if (g_marquee_additive)
                        t->sel_mask[i] = g_marquee_anchor[i] || hit;
                    else
                        t->sel_mask[i] = hit;
                }
                /* Draw band in screen coords */
                float sx0 = rx0 + lx;
                float sx1 = rx1 + lx;
                float sy0 = ry0 + ly - t->scroll_y;
                float sy1 = ry1 + ly - t->scroll_y;
                if (sx0 < lx) sx0 = lx;
                if (sx1 > lx + lw) sx1 = lx + lw;
                if (sy0 < ly) sy0 = ly;
                if (sy1 > ly + lh) sy1 = ly + lh;
                if (sx1 > sx0 && sy1 > sy0) {
                    render_quad(r, sx0, sy0, sx1 - sx0, sy1 - sy0, 0x4035BCFE);
                    render_quad(r, sx0, sy0, sx1 - sx0, 1,         0xFF35BCFE);
                    render_quad(r, sx0, sy1 - 1, sx1 - sx0, 1,     0xFF35BCFE);
                    render_quad(r, sx0, sy0, 1, sy1 - sy0,         0xFF35BCFE);
                    render_quad(r, sx1 - 1, sy0, 1, sy1 - sy0,     0xFF35BCFE);
                }
                g_needs_redraw = 1;
            }
            if (g_marquee_active && !g_mouse_down) {
                g_marquee_active = 0;
                g_marquee_panel  = -1;
            }
        }

        if (content_h > lh) {
            float new_scroll = ui_scrollbar(&g_ui, UIID(999), lx, ly, lw, lh, content_h, t->scroll_y);
            if (new_scroll != t->scroll_y) {
                t->scroll_y = new_scroll;
                t->target_scroll = new_scroll;
            }
        }
        /* Reposition inline-rename EDIT widget in grid coords */
        if (g_edit_hwnd && g_edit_idx >= 0 && g_edit_panel == g_app.active_panel &&
            g_edit_idx < t->entry_count) {
            int slot_idx = 0;
            for (int k = 0; k < g_edit_idx; k++)
                if (strcmp(t->entries[k].name, "..") != 0) slot_idx++;
            int row = slot_idx / cols, col_g = slot_idx % cols;
            float ix = lx + 4 + col_g * item_w;
            float iy = ly + 4 + row * item_h - t->scroll_y;
            float name_y = iy + icon_sz + 14;
            float name_h = (float)(item_h - icon_sz - 18);
            if (iy + item_h < ly || iy > ly + lh) inline_rename_cancel();
            else SetWindowPos(g_edit_hwnd, NULL, (int)ix, (int)name_y,
                              (int)(item_w - 4), (int)name_h,
                              SWP_NOZORDER | SWP_NOACTIVATE);
        }
        render_scissor_reset(r);
        return;
    }

    /* Drag-out: only trigger in the panel where drag started */
    if (g_drag_idx >= 0 && g_drag_panel == g_app.active_panel && g_mouse_down) {
        int dx = g_mouse_x - g_drag_x0;
        int dy = g_mouse_y - g_drag_y0;
        if (dx*dx + dy*dy > 25) {
            g_drag_idx = -1;
            start_drag_out(t);
        }
    }
    if (!g_mouse_down) { g_drag_idx = -1; g_drag_panel = -1; }

    /* Build row layout. Positive = entry_idx+1, Negative = -(group_idx+1). */
    static int rows[MAX_ENTRIES + MAX_GROUPS];
    static int entry_row[MAX_ENTRIES];
    int row_count = 0;
    for (int i = 0; i < t->entry_count; i++) entry_row[i] = -1;
    if (t->use_groups) {
        int prev_group = -1;
        for (int i = 0; i < t->entry_count; i++) {
            FileEntry* e = &t->entries[i];
            if (strcmp(e->name, "..") == 0) {
                entry_row[i] = row_count;
                rows[row_count++] = i + 1;
                continue;
            }
            if (e->group != prev_group) {
                prev_group = e->group;
                rows[row_count++] = -(e->group + 1);
            }
            if (!t->group_collapsed[e->group]) {
                entry_row[i] = row_count;
                rows[row_count++] = i + 1;
            }
        }
    } else {
        for (int i = 0; i < t->entry_count; i++) {
            entry_row[i] = row_count;
            rows[row_count++] = i + 1;
        }
    }

    float content_h = row_count * ROW_H;
    /* Scroll: only the panel under the cursor consumes the wheel event,
       unless sync-scroll is on (both panels move together). */
    int hover_this = (g_mouse_x >= lx && g_mouse_x < lx + lw &&
                      g_mouse_y >= ly && g_mouse_y < ly + lh);
    if (g_scroll_delta != 0 && (hover_this || g_sync_scroll)) {
        t->target_scroll -= g_scroll_delta * ROW_H * 3;
        if (!g_sync_scroll) g_scroll_delta = 0; /* consume so other panel won't see it */
    }
    float max_scroll = content_h - lh;
    if (max_scroll < 0) max_scroll = 0;
    if (t->target_scroll < 0) t->target_scroll = 0;
    if (t->target_scroll > max_scroll) t->target_scroll = max_scroll;
    t->scroll_y += (t->target_scroll - t->scroll_y) * 0.3f;
    if (t->scroll_y < 0) t->scroll_y = 0;
    if (t->scroll_y > max_scroll) t->scroll_y = max_scroll;
    if (t->target_scroll - t->scroll_y > 0.5f || t->target_scroll - t->scroll_y < -0.5f)
        g_needs_redraw = 1;

    float right = lx + lw - 10;
    float size_x = right - COLW_SIZE;
    float date_x = size_x - COLW_DATE;
    float type_x = date_x - COLW_TYPE;

    int first = (int)(t->scroll_y / ROW_H);
    int last = first + (int)(lh / ROW_H) + 2;
    if (first < 0) first = 0;
    if (last > row_count) last = row_count;

    for (int ri = first; ri < last; ri++) {
        /* Snap row Y to integer pixel — otherwise smooth-scroll's fractional
           scroll_y offsets every glyph a sub-pixel, and the atlas's LINEAR
           filter turns antialised text into a soft ghost. */
        float ry = floorf(ly + ri * ROW_H - t->scroll_y);
        float rw = lw - 8;
        int code = rows[ri];

        if (code < 0) {
            int gi = -code - 1;
            int hov = ui_hover(&g_ui, lx, ry, rw, ROW_H);
            if (hov) render_quad(r, lx, ry, rw, ROW_H, COL_HOVER);
            float ty = ry + (ROW_H - r->font_height) / 2;
            int collapsed = t->group_collapsed[gi];
            render_mdl2(r, collapsed ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_DOWN,
                        lx+8, ry+(ROW_H-12)/2, 12, COL_ACCENT);
            render_text(r, GROUP_LABELS[gi], lx+26, ty, COL_ACCENT);
            if (ui_clicked(&g_ui, UIID(8000 + gi), lx, ry, rw, ROW_H)) {
                t->group_collapsed[gi] = !t->group_collapsed[gi];
                g_needs_redraw = 1;
            }
            continue;
        }

        int i = code - 1;
        FileEntry* e = &t->entries[i];
        int hov = ui_hover(&g_ui, lx, ry, rw, ROW_H);
        int sel = t->sel_mask[i];

        if (sel) render_quad(r, lx, ry, rw, ROW_H, COL_SELECTED);
        else if (hov) render_quad(r, lx, ry, rw, ROW_H, COL_HOVER);

        float ty = ry + (ROW_H - r->font_height) / 2;
        float item_x = lx + (t->use_groups ? 16 : 0);
        {
            GLuint ico = get_file_icon(e->name, e->is_dir);
            if (ico) render_icon(r, ico, item_x+4, ry+3, ICON_SIZE, ICON_SIZE);
            else if (e->is_dir) render_folder_icon(r, item_x+6, ry+5, 12, COL_YELLOW);
            else render_file_icon(r, item_x+6, ry+4, 13, COL_SUBTEXT);
        }

        if (g_batch_active && g_batch_panel == g_app.active_panel &&
            t->sel_mask[i] && strcmp(e->name, "..") != 0) {
            char stem[MAX_PATH], ext[MAX_PATH];
            split_stem_ext(e->name, stem, ext, MAX_PATH);
            int slen = (int)strlen(stem);
            int cl = g_batch_chop_left;  if (cl > slen) cl = slen;
            int cr = g_batch_chop;       if (cr > slen - cl) cr = slen - cl;
            int mid_len = slen - cl - cr;
            float nx = item_x + 24;
            if (g_batch_prefix_len > 0) nx += render_text(r, g_batch_prefix, nx, ty, COL_GREEN);
            if (g_batch_focus == 1) {
                render_quad(r, nx + 1, ry + 3, 1, ROW_H - 6, COL_TEXT);
                nx += 2;
            }
            if (mid_len > 0) nx += render_text_n(r, stem + cl, mid_len, nx, ty, COL_TEXT);
            if (g_batch_focus == 0) {
                render_quad(r, nx + 1, ry + 3, 1, ROW_H - 6, COL_TEXT);
                nx += 2;
            }
            if (g_batch_typed_len > 0) nx += render_text(r, g_batch_typed, nx, ty, COL_GREEN);
            if (ext[0]) render_text(r, ext, nx + 3, ty, COL_DIM);
        } else if (!(g_edit_idx == i && g_edit_panel == g_app.active_panel)) {
            float nmw = type_x - item_x - 28;
            int nl = (int)strlen(e->name);
            while (nl > 0 && render_text_width_n(r, e->name, nl) > (int)nmw) nl--;
            render_text_n(r, e->name, nl, item_x+24, ty, COL_TEXT);
        }
        /* Type/Date/Size use smaller font (size 11), vertically centered with same baseline */
        float ty_sm = ry + (ROW_H - g_renderer.fonts[1].font_height) / 2;
        render_text_small(r, e->is_dir ? "File folder" : (strrchr(e->name,'.') ? strrchr(e->name,'.') : "File"),
                          type_x+4, ty_sm, COL_SUBTEXT);
        { char db[48]; format_time(e->modified, db, sizeof(db));
          render_text_small(r, db, date_x+4, ty_sm, COL_SUBTEXT); }
        if (e->is_dir) render_text_small(r, "--", size_x+14, ty_sm, COL_DIM);
        else { char sz[32]; format_size(e->size, sz, sizeof(sz));
               render_text_small(r, sz, size_x+4, ty_sm, COL_SUBTEXT); }

        /* Record drag-start info on press so cursor movement past threshold
           can initiate DoDragDrop. Pre-select the file if it isn't already. */
        if (hov && g_ui.input.mouse_clicked && g_drag_idx < 0) {
            g_drag_idx   = i;
            g_drag_panel = g_app.active_panel;
            g_drag_x0    = g_mouse_x;
            g_drag_y0    = g_mouse_y;
            int ctrl_p  = GetKeyState(VK_CONTROL) < 0;
            int shift_p = GetKeyState(VK_SHIFT) < 0;
            if (!ctrl_p && !shift_p && !t->sel_mask[i] &&
                strcmp(e->name, "..") != 0) {
                sel_only(t, i);
            }
        }

        /* Clamp click bounds to the file list area so rows that extend below
           (into the status bar) don't steal clicks. */
        float click_h = ROW_H;
        if (ry + click_h > ly + lh) click_h = (ly + lh) - ry;
        if (ry < ly) click_h -= (ly - ry);
        if (click_h <= 0) continue;
        float click_y = (ry < ly) ? ly : ry;
        if (ui_clicked(&g_ui, UIID(1000+i), lx, click_y, rw, click_h)) {
            if (g_batch_active && g_batch_panel == g_app.active_panel) batch_rename_commit();
            DWORD now = GetTickCount();
            int ctrl  = GetKeyState(VK_CONTROL) < 0;
            int shift = GetKeyState(VK_SHIFT) < 0;
            if (g_app.panels[g_app.active_panel].last_click_item == i && (now - g_app.panels[g_app.active_panel].last_click_time) < 400 && !ctrl && !shift) {
                /* Route through do_open_entry so image files land in the
                   built-in viewer instead of the system default app. */
                do_open_entry(t, i);
                g_app.panels[g_app.active_panel].last_click_item = -1;
            } else {
                if (shift) {
                    int from = (t->sel_anchor >= 0) ? t->sel_anchor : i;
                    sel_range(t, from, i);
                    t->selected = i;
                } else if (ctrl) {
                    if (strcmp(e->name, "..") != 0) {
                        t->sel_mask[i] = !t->sel_mask[i];
                        t->selected = i;
                        t->sel_anchor = i;
                    }
                } else {
                    sel_only(t, i);
                }
                g_app.panels[g_app.active_panel].last_click_item = i;
                g_app.panels[g_app.active_panel].last_click_time = now;
            }
        }
    }
    /* Reposition inline edit if active */
    if (g_edit_hwnd && g_edit_idx >= 0 && g_edit_panel == g_app.active_panel) {
        int er = (g_edit_idx < t->entry_count) ? entry_row[g_edit_idx] : -1;
        if (er < 0) {
            inline_rename_cancel();
        } else {
            float ry = ly + er * ROW_H - t->scroll_y;
            float name_x = lx + (t->use_groups ? 16 : 0) + 23;
            float name_w = type_x - name_x - 2;
            if (ry < ly || ry + ROW_H > ly + lh)
                inline_rename_cancel();
            else
                SetWindowPos(g_edit_hwnd, NULL, (int)name_x, (int)ry, (int)name_w, ROW_H,
                             SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    if (content_h > lh) {
        float new_scroll = ui_scrollbar(&g_ui, UIID(999), lx, ly, lw, lh, content_h, t->scroll_y);
        if (new_scroll != t->scroll_y) {
            t->scroll_y = new_scroll;
            t->target_scroll = new_scroll;
        }
    }
    render_scissor_reset(r);
}

/* Draw simple view-mode icons */
/* Minimalist icons — thin 1-px strokes, rounded feel via corner nips, wide
   whitespace. Designed for 14 px canvas. */

static void _pip(Renderer* r, float x, float y, uint32_t col) {
    /* Rounded 2×2 pip (dot) used as a "bullet". */
    render_quad(r, x, y, 2, 2, col);
}

static void _round_stroke(Renderer* r, float x, float y, float w, float h, uint32_t col) {
    /* 1-px rounded outline (radius 1) — 4 sides with corner nips clipped. */
    render_quad(r, x + 1, y,           w - 2, 1,     col);  /* top   */
    render_quad(r, x + 1, y + h - 1,   w - 2, 1,     col);  /* bot   */
    render_quad(r, x,     y + 1,       1,     h - 2, col);  /* left  */
    render_quad(r, x + w - 1, y + 1,   1,     h - 2, col);  /* right */
}

static void _round_fill(Renderer* r, float x, float y, float w, float h, uint32_t col) {
    /* 1-px rounded filled block (radius 1). */
    render_quad(r, x + 1, y,         w - 2, 1,     col);
    render_quad(r, x,     y + 1,     w,     h - 2, col);
    render_quad(r, x + 1, y + h - 1, w - 2, 1,     col);
}

static void draw_view_icon(Renderer* r, int mode, float x, float y, float sz, uint32_t col, uint32_t bg) {
    (void)bg;
    if (mode == VM_DETAILS) {
        /* Three list rows — small bullet on the left + horizontal line. */
        float rows[3] = { y + 1, y + sz * 0.5f - 1, y + sz - 3 };
        for (int i = 0; i < 3; i++) {
            _pip(r, x, rows[i], col);
            render_quad(r, x + 4, rows[i] + 1, sz - 4, 1, col);   /* single-pixel line */
        }
    } else if (mode == VM_SMALL_ICONS) {
        /* 2×2 grid of small rounded blocks. */
        float ss = (sz - 3) / 2;
        _round_fill(r, x,             y,             ss, ss, col);
        _round_fill(r, x + ss + 3,    y,             ss, ss, col);
        _round_fill(r, x,             y + ss + 3,    ss, ss, col);
        _round_fill(r, x + ss + 3,    y + ss + 3,    ss, ss, col);
    } else { /* LARGE */
        _round_stroke(r, x, y, sz, sz, col);
    }
}

/* Minimalist split-view indicator: two thin rounded outlines side by side. */
static void draw_split_icon(Renderer* r, float x, float y, float sz, uint32_t color, uint32_t bg) {
    (void)bg;
    float gap = 2;
    float rw  = (sz - gap) / 2;
    _round_stroke(r, x,             y, rw, sz, color);   /* left  */
    _round_stroke(r, x + rw + gap,  y, rw, sz, color);   /* right */
}

/* Render one panel's status section: counts on left + [view] [split or sync]
   on right. panel_idx 0 → right-most button is split toggle, 1 → sync toggle. */
static void render_panel_status(Renderer* r, int panel_idx, float x0, float x1, float y) {
    Panel* P = &g_app.panels[panel_idx];
    Tab*   t = &P->tabs[P->active_tab];
    int folders = 0, files = 0;
    for (int i = 0; i < t->entry_count; i++) {
        if (strcmp(t->entries[i].name, "..") == 0) continue;
        if (t->entries[i].is_dir) folders++; else files++;
    }
    int sel = sel_count(t);
    char info[128];
    if (sel > 0)
        _snprintf(info, sizeof(info), "%d folders, %d files  (%d selected)", folders, files, sel);
    else
        _snprintf(info, sizeof(info), "%d folders, %d files", folders, files);
    float ty = y + (STATUS_BAR_H - g_renderer.fonts[1].font_height) / 2;
    render_text_small(r, info, x0 + 10, ty, COL_SUBTEXT);

    float btn_sz = 14, btn_pad = 6;
    float btn_w  = btn_sz + btn_pad * 2;
    float btn_h  = STATUS_BAR_H - 2;
    float btn_y  = y + 1;
    float btn_x  = x1 - btn_w - 4;

    /* Right-most button: panel 0 = split toggle, panel 1 = sync toggle */
    if (panel_idx == 0) {
        int hov = ui_hover(&g_ui, btn_x, btn_y, btn_w, btn_h);
        if (hov) render_quad(r, btn_x, btn_y, btn_w, btn_h, COL_HOVER);
        uint32_t col = g_app.split_active ? COL_ACCENT : (hov ? COL_TEXT : COL_SUBTEXT);
        draw_split_icon(r, btn_x + btn_pad, btn_y + (btn_h - btn_sz) / 2, btn_sz, col, COL_HEADER);
        if (hov) tt_set(g_app.split_active ? "Close split view  (Ctrl+\\)"
                                           : "Split view  (Ctrl+\\)",
                        (int)(btn_x + btn_w / 2), (int)btn_y);
        if (ui_clicked(&g_ui, 600, btn_x, btn_y, btn_w, btn_h)) {
            g_app.split_active = !g_app.split_active;
            if (g_app.split_active && g_app.panels[1].tab_count == 0) {
                int saved = g_app.active_panel;
                g_app.active_panel = 1;
                new_tab(g_app.panels[0].tabs[g_app.panels[0].active_tab].path);
                g_app.active_panel = saved;
            }
            tabs_save();
            g_needs_redraw = 1;
        }
    } else {
        int hov = ui_hover(&g_ui, btn_x, btn_y, btn_w, btn_h);
        if (hov) render_quad(r, btn_x, btn_y, btn_w, btn_h, COL_HOVER);
        uint32_t col = g_sync_scroll ? COL_ACCENT : (hov ? COL_TEXT : COL_SUBTEXT);
        render_mdl2(r, ICON_LINK, btn_x + btn_pad, btn_y + (btn_h - btn_sz) / 2, btn_sz, col);
        if (hov) tt_set(g_sync_scroll ? "Sync scroll: on (both panels scroll together)"
                                      : "Sync scroll: off",
                        (int)(btn_x + btn_w / 2), (int)btn_y);
        if (ui_clicked(&g_ui, 601, btn_x, btn_y, btn_w, btn_h)) {
            g_sync_scroll = !g_sync_scroll;
            g_needs_redraw = 1;
        }
    }

    /* View-mode button (left of the right-most button) */
    float vx = btn_x - btn_w - 2;
    int vhov = ui_hover(&g_ui, vx, btn_y, btn_w, btn_h);
    if (vhov) render_quad(r, vx, btn_y, btn_w, btn_h, COL_HOVER);
    uint32_t vcol = vhov ? COL_TEXT : COL_SUBTEXT;
    draw_view_icon(r, t->view_mode, vx + btn_pad, btn_y + (btn_h - btn_sz) / 2,
                   btn_sz, vcol, COL_HEADER);
    if (vhov) {
        const char* label =
            (t->view_mode == VM_DETAILS)     ? "View: Details  (click to change)" :
            (t->view_mode == VM_SMALL_ICONS) ? "View: Small Icons  (click to change)" :
                                               "View: Large Icons  (click to change)";
        tt_set(label, (int)(vx + btn_w / 2), (int)btn_y);
    }
    if (ui_clicked(&g_ui, 602 + panel_idx * 100, vx, btn_y, btn_w, btn_h)) {
        HMENU vm = CreatePopupMenu();
        AppendMenuA(vm, MF_STRING | (t->view_mode == VM_LARGE_ICONS ? MF_CHECKED : 0),
                    IDM_VIEW_LARGE,   "Large Icons");
        AppendMenuA(vm, MF_STRING | (t->view_mode == VM_SMALL_ICONS ? MF_CHECKED : 0),
                    IDM_VIEW_SMALL,   "Small Icons");
        AppendMenuA(vm, MF_STRING | (t->view_mode == VM_DETAILS ? MF_CHECKED : 0),
                    IDM_VIEW_DETAILS, "Details");
        POINT pt = { (int)vx, (int)btn_y };
        ClientToScreen(g_hwnd, &pt);
        int cmd = TrackPopupMenu(vm, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                 pt.x, pt.y, 0, g_hwnd, NULL);
        DestroyMenu(vm);
        int new_mode = -1;
        if (cmd == IDM_VIEW_DETAILS) new_mode = VM_DETAILS;
        if (cmd == IDM_VIEW_SMALL)   new_mode = VM_SMALL_ICONS;
        if (cmd == IDM_VIEW_LARGE)   new_mode = VM_LARGE_ICONS;
        if (new_mode >= 0 && new_mode != t->view_mode) {
            t->view_mode = new_mode;
            t->scroll_y = 0;
            t->target_scroll = 0;
            view_prefs_set(t->path, new_mode);
            g_needs_redraw = 1;
        }
    }

    /* Thumbnail progress (only on panel 0 status, right of counts) */
    if (panel_idx == 0) {
        int pending = 0;
        EnterCriticalSection(&g_thumb_cs);
        pending = (g_thumb_req_tail - g_thumb_req_head + THUMB_Q_CAP) % THUMB_Q_CAP;
        LeaveCriticalSection(&g_thumb_cs);
        if (pending > 0) {
            char prog[64];
            _snprintf(prog, sizeof(prog), "Loading thumbnails: %d", pending);
            int pw = render_text_width_small(r, prog);
            render_text_small(r, prog, vx - pw - 8, ty, COL_ACCENT);
            g_needs_redraw = 1;
        }
    }
}

/* ---- Fuzzy finder overlay render ---- */
static void build_fuzzy_finder(void) {
    if (!g_ff_active || g_ff_panel != g_app.active_panel) return;
    Renderer* r = &g_renderer;

    /* Panel bounds (active panel content area) */
    float panel_x = SIDEBAR_W;
    float panel_w = (float)g_width - SIDEBAR_W;
    if (g_app.split_active) {
        float split_x = floorf(SIDEBAR_W + g_app.split_ratio * (g_width - SIDEBAR_W));
        if (g_app.active_panel == 0) { panel_w = split_x - SIDEBAR_W; }
        else                         { panel_x = split_x; panel_w = g_width - split_x; }
    }
    float panel_y = CONTENT_TOP;
    float panel_h = (float)g_height - CONTENT_TOP - STATUS_BAR_H;

    /* Panel dimensions */
    float ff_w = panel_w * 0.62f; if (ff_w < 420) ff_w = 420; if (ff_w > 900) ff_w = 900;
    if (ff_w > panel_w - 24) ff_w = panel_w - 24;
    float row_h = 28.0f;
    int   max_rows = (int)((panel_h - 130) / row_h);
    if (max_rows > 15) max_rows = 15;
    if (max_rows < 5)  max_rows = 5;
    float ff_h = 40 + 40 + row_h * max_rows + 26;   /* input + spacer + rows + footer */
    float ff_x = panel_x + (panel_w - ff_w) / 2;
    float ff_y = panel_y + 20;
    if (ff_y + ff_h > panel_y + panel_h - 4) ff_h = panel_h - 24;

    /* Dim the panel underneath */
    render_quad(r, panel_x, panel_y, panel_w, panel_h, 0x88000000);

    /* Popup background */
    render_quad(r, ff_x - 1, ff_y - 1, ff_w + 2, ff_h + 2, COL_BORDER);
    render_quad(r, ff_x, ff_y, ff_w, ff_h, COL_HEADER);

    /* Search input row */
    float in_h = 36;
    render_quad(r, ff_x + 8, ff_y + 8, ff_w - 16, in_h, COL_BG);
    if (g_ff_scanning) {
        static const char* fr2[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧"};
        int fi2 = (GetTickCount() / 80) & 7;
        render_text(r, fr2[fi2], ff_x + 18, floorf(ff_y + 8 + (in_h - r->font_height) / 2), COL_ACCENT);
    } else {
        render_mdl2(r, ICON_SEARCH, ff_x + 18, ff_y + 8 + (in_h - 14) / 2, 14, COL_SUBTEXT);
    }
    float tx = ff_x + 40;
    float ty = floorf(ff_y + 8 + (in_h - r->font_height) / 2);
    if (g_ff_query_len > 0) render_text_n(r, g_ff_query, g_ff_query_len, tx, ty, COL_TEXT);
    else                    render_text(r, "Type to filter…", tx, ty, COL_DIM);
    /* Blinking cursor at g_ff_cursor position */
    int cur_w = (g_ff_cursor > 0)
        ? render_text_width_n(r, g_ff_query, g_ff_cursor)
        : 0;
    if ((GetTickCount() / 500) & 1)
        render_quad(r, tx + cur_w + 1, ty - 2, 1, r->font_height + 4, COL_TEXT);

    /* Recursive checkbox on the right */
    float cb_sz = 14;
    float cb_x  = ff_x + ff_w - 20 - cb_sz - render_text_width(r, "Recursive");
    float cb_y  = ff_y + 8 + (in_h - cb_sz) / 2;
    render_quad(r, cb_x - 1, cb_y - 1, cb_sz + 2, cb_sz + 2, COL_BORDER);
    render_quad(r, cb_x, cb_y, cb_sz, cb_sz, g_ff_recursive ? COL_ACCENT : COL_BG);
    if (g_ff_recursive)
        render_mdl2(r, ICON_CHECK, cb_x + 2, cb_y + 2, cb_sz - 4, COL_TEXT);
    render_text(r, "Recursive", cb_x + cb_sz + 6, ty, COL_SUBTEXT);
    /* Click checkbox */
    if (ui_hover(&g_ui, cb_x - 4, cb_y - 4, cb_sz + 8 + render_text_width(r, "Recursive") + 12, cb_sz + 8) &&
        g_ui.input.mouse_clicked) {
        g_ff_recursive = !g_ff_recursive;
        if (g_ff_recursive) ff_kick_recursive_scan();
        else {
            ff_stop_scan();
            ff_build_index_from_tab();
        }
        ff_refresh_results();
        g_needs_redraw = 1;
    }

    /* Results list */
    float list_x = ff_x + 8;
    float list_y = ff_y + 8 + in_h + 8;
    float list_w = ff_w - 16;
    /* Clamp scroll so selection stays visible */
    if (g_ff_selected < g_ff_scroll) g_ff_scroll = g_ff_selected;
    if (g_ff_selected >= g_ff_scroll + max_rows) g_ff_scroll = g_ff_selected - max_rows + 1;
    if (g_ff_scroll < 0) g_ff_scroll = 0;

    render_scissor(r, (int)list_x, (int)list_y, (int)list_w, (int)(row_h * max_rows));
    int shown = 0;
    for (int i = g_ff_scroll; i < g_ff_result_count && shown < max_rows; i++, shown++) {
        FFResult* res = &g_ff_results[i];
        FFEntry*  fe  = &g_ff_index[res->entry_idx];
        float ry = list_y + shown * row_h;
        int is_sel = (i == g_ff_selected);
        if (is_sel) render_quad(r, list_x, ry, list_w, row_h, COL_SELECTED);

        /* Icon — use the same shell-icon path as the file list so .js /
           .exe / .png etc show their real per-type glyph. */
        {
            GLuint ico = get_file_icon(fe->name, fe->is_dir);
            if (ico)
                render_icon(r, ico, list_x + 6, ry + (row_h - ICON_SIZE) / 2,
                            ICON_SIZE, ICON_SIZE);
            else if (fe->is_dir)
                render_folder_icon(r, list_x + 8, ry + (row_h - 10) / 2, 11, COL_YELLOW);
            else
                render_file_icon(r, list_x + 8, ry + (row_h - 12) / 2, 11, COL_SUBTEXT);
        }

        /* Name with per-char highlight */
        float nx = list_x + 28;
        float ny = floorf(ry + (row_h - r->font_height) / 2);
        int mi = 0;
        int nlen = (int)strlen(fe->name);
        int seg_start = 0;
        for (int p = 0; p <= nlen; p++) {
            int is_mark = (mi < res->n_marks && res->marks[mi] == p);
            if (is_mark || p == nlen) {
                if (p > seg_start) {
                    nx += render_text_n(r, fe->name + seg_start, p - seg_start,
                                        nx, ny, is_sel ? COL_TEXT : COL_SUBTEXT);
                }
                if (is_mark) {
                    nx += render_text_n(r, fe->name + p, 1, nx, ny, COL_YELLOW);
                    mi++;
                    seg_start = p + 1;
                } else {
                    seg_start = p;
                }
            }
        }

        /* Relative path (rel + \) shown dim to the right of the name */
        if (fe->rel[0]) {
            float px = nx + 12;
            /* Truncate rel if it doesn't fit */
            int rl = (int)strlen(fe->rel);
            int max_w = (int)(list_x + list_w - 8 - px);
            while (rl > 0 && render_text_width_n(r, fe->rel, rl) > max_w) rl--;
            if (rl > 0) render_text_n(r, fe->rel, rl, px, ny, COL_DIM);
        }
    }
    render_scissor_reset(r);

    /* Footer */
    float footer_y = list_y + row_h * max_rows + 6;
    render_quad(r, ff_x + 8, footer_y - 2, ff_w - 16, 1, COL_BORDER);
    float footer_ty = footer_y + (24 - r->fonts[1].font_height) / 2;

    if (g_ff_scanning) {
        /* Animated spinner: 8-frame braille (renders via dynamic glyph
           cache — atlas grows on demand). */
        static const char* frames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧"};
        int fi = (GetTickCount() / 80) & 7;
        float sx = ff_x + 12;
        int sw = render_text(r, frames[fi], sx, footer_ty, COL_ACCENT);
        sx += sw + 6;
        char status[128];
        _snprintf(status, sizeof(status),
                  "Indexing…  %d files  ·  %d matches",
                  g_ff_index_count, g_ff_result_count);
        render_text(r, status, sx, footer_ty, COL_TEXT);

        /* Right-aligned Stop button */
        const char* stop_lbl = "Stop  (Ctrl+.)";
        int stop_w = render_text_width(r, stop_lbl) + 20;
        int stop_h = 22;
        float stop_x = ff_x + ff_w - 12 - stop_w;
        float stop_y = footer_y + (24 - stop_h) / 2;
        int stop_hov = ui_hover(&g_ui, stop_x, stop_y, stop_w, stop_h);
        render_quad(r, stop_x, stop_y, stop_w, stop_h, stop_hov ? COL_HOVER : COL_HEADER);
        render_quad(r, stop_x, stop_y, stop_w, 1, COL_BORDER);
        render_quad(r, stop_x, stop_y + stop_h - 1, stop_w, 1, COL_BORDER);
        render_quad(r, stop_x, stop_y, 1, stop_h, COL_BORDER);
        render_quad(r, stop_x + stop_w - 1, stop_y, 1, stop_h, COL_BORDER);
        float lbl_tw = (float)render_text_width(r, stop_lbl);
        render_text(r, stop_lbl,
                    stop_x + (stop_w - lbl_tw) / 2,
                    stop_y + (stop_h - r->font_height) / 2,
                    stop_hov ? COL_RED : COL_TEXT);
        if (stop_hov && g_ui.input.mouse_clicked) {
            ff_stop_scan();
            g_needs_redraw = 1;
        }
    } else {
        char footer[192];
        _snprintf(footer, sizeof(footer),
                  "%d / %d  ·  ↑↓ navigate  ·  Enter open  ·  Ctrl+R recursive  ·  Esc close",
                  g_ff_result_count, g_ff_index_count);
        render_text(r, footer, ff_x + 12, footer_ty, COL_DIM);
    }
    /* Repaint continuously for spinner animation and cursor blink */
    g_needs_redraw = 1;
}

/* ---- Settings modal ---- */
static int settings_row_button(Renderer* r, float x, float y, float w, float h,
                               const char* label, int selected, int id)
{
    int hov = ui_hover(&g_ui, x, y, w, h);
    uint32_t bg = selected ? COL_ACCENT : (hov ? COL_HOVER : COL_HEADER);
    uint32_t fg = selected ? COL_MANTLE : COL_TEXT;
    render_quad(r, x, y, w, h, bg);
    render_quad(r, x, y, w, 1, COL_BORDER);
    render_quad(r, x, y + h - 1, w, 1, COL_BORDER);
    render_quad(r, x, y, 1, h, COL_BORDER);
    render_quad(r, x + w - 1, y, 1, h, COL_BORDER);
    int tw = render_text_width(r, label);
    render_text(r, label,
                floorf(x + (w - tw) / 2),
                floorf(y + (h - r->font_height) / 2),
                fg);
    return (hov && ui_clicked(&g_ui, id, x, y, w, h));
}

static void build_settings_modal(void) {
    if (!g_settings_open) return;
    Renderer* r = &g_renderer;

    /* Backdrop */
    render_quad(r, 0, 0, (float)g_width, (float)g_height, 0xAA000000);

    /* Panel */
    float pw = 520, ph = 380;
    if (pw > g_width - 40) pw = g_width - 40;
    if (ph > g_height - 40) ph = g_height - 40;
    float px = floorf((g_width  - pw) / 2);
    float py = floorf((g_height - ph) / 2);

    render_quad(r, px - 1, py - 1, pw + 2, ph + 2, COL_BORDER);
    render_quad(r, px, py, pw, ph, COL_HEADER);

    /* Header */
    render_text(r, "Settings", px + 20, py + 16, COL_TEXT);
    /* Close X */
    float cx = px + pw - 32, cy = py + 12;
    int chov = ui_hover(&g_ui, cx, cy, 24, 24);
    if (chov) render_quad(r, cx, cy, 24, 24, COL_HOVER);
    render_mdl2(r, ICON_CLOSE, cx + 7, cy + 7, 10, chov ? COL_RED : COL_SUBTEXT);
    /* Click-outside-panel closes the modal. Direct hit-test — avoids
       ui_clicked's active_id race with the theme buttons below. */
    int mx = g_ui.input.mouse_x, my = g_ui.input.mouse_y;
    int inside_panel = (mx >= px && mx < px + pw && my >= py && my < py + ph);
    if (g_ui.input.mouse_clicked && !inside_panel) {
        g_settings_open = 0;
        g_needs_redraw = 1;
        return;
    }
    /* Close X (draws on top of panel) */
    if (ui_clicked(&g_ui, UIID(700), cx, cy, 24, 24)) {
        g_settings_open = 0;
        g_needs_redraw = 1;
        return;
    }
    render_quad(r, px, py + 48, pw, 1, COL_BORDER);

    /* Content */
    float cy2 = py + 68;
    float lx  = px + 24;
    float rx2 = px + pw - 24;

    /* --- Theme --- */
    render_text(r, "Theme", lx, cy2, COL_SUBTEXT);
    cy2 += 22;
    /* Rescan themes each frame — cheap & keeps drop-ins live */
    theme_discover();
    float chip_h = 32;
    float chip_gap = 6;
    for (int i = 0; i < g_theme_count; i++) {
        float chip_w = (pw - 48 - chip_gap * 2) / 3;
        int col_i = i % 3;
        int row_i = i / 3;
        float bx = lx + col_i * (chip_w + chip_gap);
        float by = cy2 + row_i * (chip_h + chip_gap);
        int selected = (_stricmp(g_theme_current, g_themes[i].display) == 0);
        if (settings_row_button(r, bx, by, chip_w, chip_h,
                                g_themes[i].display, selected, UIID(710 + i))) {
            theme_apply_file(g_themes[i].path);
        }
    }
    int rows_used = (g_theme_count + 2) / 3;
    cy2 += rows_used * (chip_h + chip_gap) + 10;

    /* --- Row density --- */
    render_text(r, "Row density", lx, cy2, COL_SUBTEXT);
    cy2 += 22;
    {
        struct { const char* label; int rh; } opts[3] = {
            { "Compact", 17 }, { "Normal", 20 }, { "Roomy", 24 }
        };
        float ow = (pw - 48 - chip_gap * 2) / 3;
        for (int i = 0; i < 3; i++) {
            int selected = (g_pref_row_h == opts[i].rh);
            if (settings_row_button(r, lx + i * (ow + chip_gap), cy2,
                                    ow, chip_h,
                                    opts[i].label, selected, UIID(720 + i))) {
                g_pref_row_h = opts[i].rh;
                g_row_h      = opts[i].rh;
                settings_save();
            }
        }
        cy2 += chip_h + 10;
    }

    /* --- Font size --- */
    render_text(r, "Font size (applies on next launch)", lx, cy2, COL_SUBTEXT);
    cy2 += 22;
    {
        float bw = 34, bh = chip_h;
        /* [ − ]  N pt  [ + ] */
        if (settings_row_button(r, lx, cy2, bw, bh, "-", 0, UIID(730))) {
            if (g_pref_font_size > 9) { g_pref_font_size--; g_pref_font_needs_restart = 1; settings_save(); }
        }
        char pt[16];
        _snprintf(pt, sizeof(pt), "%d pt", g_pref_font_size);
        render_text(r, pt, lx + bw + 20,
                    floorf(cy2 + (bh - r->font_height) / 2), COL_TEXT);
        if (settings_row_button(r, lx + bw + 90, cy2, bw, bh, "+", 0, UIID(731))) {
            if (g_pref_font_size < 16) { g_pref_font_size++; g_pref_font_needs_restart = 1; settings_save(); }
        }
        if (g_pref_font_needs_restart)
            render_text_small(r, "Restart to apply new font size",
                              lx + bw + 90 + bw + 20,
                              floorf(cy2 + (bh - r->fonts[1].font_height) / 2),
                              COL_ACCENT);
        cy2 += bh + 10;
    }

    /* --- Footer hint --- */
    render_text_small(r, "Press Esc to close  ·  Ctrl+Shift+T reloads theme from disk",
                      lx, py + ph - 22, COL_DIM);
}

static void build_status_bar(void) {
    Renderer* r = &g_renderer;
    float y = (float)g_height - STATUS_BAR_H;
    float w = (float)g_width;
    /* Status bar covers panel area only — sidebar extends full height below */
    render_quad(r, SIDEBAR_W, y, w - SIDEBAR_W, STATUS_BAR_H, COL_HEADER);
    render_quad(r, SIDEBAR_W, y, w - SIDEBAR_W, 1, COL_BORDER);
    if (g_app.split_active) {
        float split_x = floorf(SIDEBAR_W + g_app.split_ratio * (g_width - SIDEBAR_W));
        render_panel_status(r, 0, SIDEBAR_W, split_x, y);
        render_panel_status(r, 1, split_x, w, y);
        render_quad(r, split_x - 1, y, 2, STATUS_BAR_H, COL_BORDER);
    } else {
        render_panel_status(r, 0, SIDEBAR_W, w, y);
    }
}

static void build_ui(void) {
    UIInput input = {0};
    input.mouse_x = g_mouse_x; input.mouse_y = g_mouse_y;
    input.mouse_down = g_mouse_down;
    input.mouse_clicked = g_mouse_clicked;
    input.mouse_released = g_mouse_released;
    input.mouse_dblclick = g_mouse_dblclick;
    input.scroll_delta = g_scroll_delta;
    ui_begin(&g_ui, input);
    /* Reset tooltip; each hovered icon widget refreshes it during render. */
    g_tt_text[0] = 0;

    /* Modal input capture — while Settings is open, hide all mouse events
       from the widgets below so a click on a chip doesn't also trigger a
       click on the file / tab / button under it. Real input is restored
       right before the modal renders (bottom of build_ui). */
    int  saved_mx = g_ui.input.mouse_x, saved_my = g_ui.input.mouse_y;
    int  saved_clk = g_ui.input.mouse_clicked, saved_rel = g_ui.input.mouse_released;
    int  saved_dbl = g_ui.input.mouse_dblclick, saved_dn = g_ui.input.mouse_down;
    if (g_settings_open) {
        /* Push mouse far off-screen and clear click flags so nothing hovers
           or triggers underneath the modal. */
        g_ui.input.mouse_x = -30000;
        g_ui.input.mouse_y = -30000;
        g_ui.input.mouse_clicked = 0;
        g_ui.input.mouse_released = 0;
        g_ui.input.mouse_dblclick = 0;
        g_ui.input.mouse_down = 0;
    }

    /* Focus follows click: in split mode, clicking in either panel makes it active */
    if (g_app.split_active && g_mouse_clicked && g_mouse_x >= SIDEBAR_W &&
        g_mouse_y >= TAB_BAR_H) {
        float split_x = SIDEBAR_W + g_app.split_ratio * (g_width - SIDEBAR_W);
        g_app.active_panel = (g_mouse_x < split_x) ? 0 : 1;
    }

    /* Sidebar + window buttons rendered once (shared) */
    build_sidebar();
    build_window_buttons();

    int win_btns_left = g_width - WIN_BTN_W * 3;
    float content_left = SIDEBAR_W;
    float content_right = (float)g_width;
    /* Snap to integer pixel so the right panel renders at whole-pixel x —
       otherwise text glyphs land at fractional positions and bilinear
       sampling on the font atlas makes them look fuzzy. */
    float split_x = floorf(content_left + g_app.split_ratio * (content_right - content_left));
    if (!g_app.split_active) split_x = content_right;

    /* Tab bar tabs_xmin = 38 (after logo) for panel 0, split_x for panel 1.
       For tabs_xmax: stop before window buttons OR at split_x. */
    int n_panels = g_app.split_active ? 2 : 1;
    int saved_active = g_app.active_panel;
    g_focused_panel = saved_active;
    for (int p = 0; p < n_panels; p++) {
        g_app.active_panel = p;
        g_ui_id_base = p * 10000;
        float panel_x  = (p == 0) ? content_left : split_x;
        float panel_w  = (p == 0) ? (split_x - content_left) : (content_right - split_x);
        float tabs_xmin = (p == 0) ? 38.0f : split_x;
        float tabs_xmax = (g_app.split_active && p == 0) ? split_x : (float)win_btns_left;
        if (tabs_xmax > tabs_xmin + 40) tabs_xmax -= 0; /* keep flush */

        build_tab_bar(tabs_xmin, tabs_xmax);
        build_toolbar(panel_x, panel_x + panel_w);
        build_column_headers(panel_x, CONTENT_TOP, panel_w);
        build_file_list(panel_x, CONTENT_TOP + COL_HDR_H,
                        panel_w, g_height - CONTENT_TOP - COL_HDR_H - STATUS_BAR_H);
    }
    g_app.active_panel = saved_active;
    g_ui_id_base = 0;

    /* Drop target highlight: outline panel under cursor during drag-in */
    if (g_drop_hover_panel >= 0) {
        Renderer* r = &g_renderer;
        float dx = (g_drop_hover_panel == 0) ? content_left : split_x;
        float dw = (g_drop_hover_panel == 0 && g_app.split_active) ? (split_x - content_left)
                 : (g_drop_hover_panel == 1) ? (content_right - split_x)
                 : (content_right - content_left);
        float dy = CONTENT_TOP;
        float dh = (float)(g_height - CONTENT_TOP - STATUS_BAR_H);
        /* 2px outline */
        render_quad(r, dx, dy, dw, 2, COL_ACCENT);
        render_quad(r, dx, dy+dh-2, dw, 2, COL_ACCENT);
        render_quad(r, dx, dy, 2, dh, COL_ACCENT);
        render_quad(r, dx+dw-2, dy, 2, dh, COL_ACCENT);
    }

    /* Splitter handle (drag to resize) */
    if (g_app.split_active) {
        Renderer* r = &g_renderer;
        float hit_x = split_x - SPLITTER_HIT_W / 2.0f;
        int hov = ui_hover(&g_ui, hit_x, TAB_BAR_H,
                            (float)SPLITTER_HIT_W, (float)(g_height - TAB_BAR_H - STATUS_BAR_H));
        if (hov && g_ui.input.mouse_clicked) g_splitter_dragging = 1;
        if (g_splitter_dragging && !g_ui.input.mouse_down) {
            g_splitter_dragging = 0;
            tabs_save();
        }
        if (g_splitter_dragging) {
            float content_left  = SIDEBAR_W;
            float content_right = (float)g_width;
            float new_x = (float)g_mouse_x;
            float min_x = content_left + 200;
            float max_x = content_right - 200;
            if (new_x < min_x) new_x = min_x;
            if (new_x > max_x) new_x = max_x;
            g_app.split_ratio = (new_x - content_left) / (content_right - content_left);
            g_needs_redraw = 1;
        }
        uint32_t col = (hov || g_splitter_dragging) ? COL_ACCENT : COL_BORDER;
        render_quad(r, split_x - 1, TAB_BAR_H, 2, g_height - TAB_BAR_H - STATUS_BAR_H, col);
    }

    build_status_bar();
    build_fuzzy_finder();
    if (g_settings_open) {
        g_ui.input.mouse_x = saved_mx;
        g_ui.input.mouse_y = saved_my;
        g_ui.input.mouse_clicked  = saved_clk;
        g_ui.input.mouse_released = saved_rel;
        g_ui.input.mouse_dblclick = saved_dbl;
        g_ui.input.mouse_down     = saved_dn;
    }
    build_settings_modal();
    tt_draw();
    ui_end(&g_ui);
}

/* ================================================================
 *  BORDERLESS WINDOW
 * ================================================================ */

static LRESULT handle_nccalcsize(HWND hwnd, WPARAM wp, LPARAM lp) {
    if (!wp) return DefWindowProcW(hwnd, WM_NCCALCSIZE, wp, lp);
    NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
    if (IsZoomed(hwnd)) {
        /* When maximized, set client area = monitor work area so content
           doesn't overflow past screen edges and bottom isn't hidden
           behind taskbar. */
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi; mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(mon, &mi)) {
            p->rgrc[0] = mi.rcWork;
        }
    }
    return 0;
}

static LRESULT handle_nchittest(HWND hwnd, LPARAM lp) {
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(hwnd, &pt);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;

    /* Resize borders (not when maximized) */
    if (!IsZoomed(hwnd)) {
        int b = RESIZE_BORDER;
        int left   = pt.x < b;
        int right  = pt.x >= w - b;
        int top    = pt.y < b;
        int bottom = pt.y >= h - b;
        if (top && left)      return HTTOPLEFT;
        if (top && right)     return HTTOPRIGHT;
        if (bottom && left)   return HTBOTTOMLEFT;
        if (bottom && right)  return HTBOTTOMRIGHT;
        if (left)             return HTLEFT;
        if (right)            return HTRIGHT;
        if (top)              return HTTOP;
        if (bottom)           return HTBOTTOM;
    }

    /* Tab bar area: caption for dragging in non-interactive zones. */
    if (pt.y < TAB_BAR_H) {
        int min_x = w - WIN_BTN_W * 3;   /* left edge of window buttons */
        if (pt.x >= min_x)                          return HTCLIENT;   /* min/max/close */
        if (pt.x < 38)                              return HTCAPTION;  /* logo grip */
        if (pt.x < g_tab_bar_tabs_end_x)            return HTCLIENT;   /* over a tab */
        if (pt.x < g_tab_bar_btns_start_x)          return HTCAPTION;  /* gap between tabs and buttons */
        if (pt.x < g_tab_bar_btn_end_x)             return HTCLIENT;   /* chevrons + "+" */
        return HTCAPTION;                                                /* gap before window buttons */
    }

    return HTCLIENT;
}

/* ---- Window proc ---- */
static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE: return handle_nccalcsize(hwnd, wp, lp);
    case WM_NCHITTEST:  return handle_nchittest(hwnd, lp);

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (g_tab_drag_active) {
                SetCursor(LoadCursor(NULL, IDC_SIZEALL));
                return TRUE;
            }
            if (g_splitter_dragging) {
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                return TRUE;
            }
            if (g_app.split_active) {
                POINT pt; GetCursorPos(&pt); ScreenToClient(g_hwnd, &pt);
                float split_x = SIDEBAR_W + g_app.split_ratio * (g_width - SIDEBAR_W);
                if (pt.x >= split_x - 3 && pt.x < split_x + 3 &&
                    pt.y >= TAB_BAR_H && pt.y < g_height - STATUS_BAR_H) {
                    SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                    return TRUE;
                }
            }
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_NCACTIVATE:
        /* Prevent default non-client rendering; return TRUE to allow activation */
        return TRUE;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 640;
        mmi->ptMinTrackSize.y = 400;
        return 0;
    }

    case WM_SIZE:
        g_width = LOWORD(lp); g_height = HIWORD(lp);
        g_needs_redraw = 1; return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        g_needs_redraw = 1;
        return 0;
    }

    case WM_MOUSEMOVE:
        g_mouse_x = GET_X_LPARAM(lp);
        g_mouse_y = GET_Y_LPARAM(lp);
        g_needs_redraw = 1; return 0;

    case WM_LBUTTONDOWN:
        g_mouse_down = 1; g_mouse_clicked = 1;
        SetCapture(hwnd); g_needs_redraw = 1; return 0;

    case WM_LBUTTONUP:
        g_mouse_down = 0; g_mouse_released = 1;
        ReleaseCapture(); g_needs_redraw = 1; return 0;

    case WM_LBUTTONDBLCLK:
        g_mouse_down = 1; g_mouse_clicked = 1; g_mouse_dblclick = 1;
        SetCapture(hwnd); g_needs_redraw = 1; return 0;

    case WM_MOUSEWHEEL:
        if (g_edit_hwnd) inline_rename_cancel();
        g_scroll_delta = (float)GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
        g_needs_redraw = 1; return 0;

    case WM_RBUTTONUP:
        show_context_menu(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_DEVICECHANGE:
        /* Volume mount/unmount events arrive here. The wParam is one of the
           DBT_DEVICE* codes; we only care about volume arrival/removal which
           affect the drive list. */
        if (wp == 0x8000 /* DBT_DEVICEARRIVAL */ ||
            wp == 0x8004 /* DBT_DEVICEREMOVECOMPLETE */) {
            refresh_storage_drives();
        }
        return TRUE;

    case WM_CHAR: {
        if (g_edit_hwnd || g_addr_hwnd) break;
        WCHAR wc = (WCHAR)wp;
        char enc[5] = {0};
        int enc_len = 0;
        if (wc < 0x80) { enc[0] = (char)wc; enc_len = 1; }
        else if (wc < 0x800) {
            enc[0] = (char)(0xC0 | (wc >> 6));
            enc[1] = (char)(0x80 | (wc & 0x3F));
            enc_len = 2;
        } else {
            enc[0] = (char)(0xE0 | (wc >> 12));
            enc[1] = (char)(0x80 | ((wc >> 6) & 0x3F));
            enc[2] = (char)(0x80 | (wc & 0x3F));
            enc_len = 3;
        }
        if (g_ff_active) {
            if (wp >= 32 && wp != 127 &&
                g_ff_query_len + enc_len < (int)sizeof(g_ff_query)) {
                /* Insert at cursor position */
                int tail = g_ff_query_len - g_ff_cursor;
                memmove(g_ff_query + g_ff_cursor + enc_len,
                        g_ff_query + g_ff_cursor, tail);
                memcpy(g_ff_query + g_ff_cursor, enc, enc_len);
                g_ff_query_len += enc_len;
                g_ff_cursor    += enc_len;
                g_ff_query[g_ff_query_len] = 0;
                g_ff_selected = 0;
                g_ff_scroll   = 0;
                ff_refresh_results();
                g_needs_redraw = 1;
            }
            return 0;
        }
        if (g_batch_active) {
            if (wp >= 32 && wp != 127) {
                if (g_batch_focus == 1) {
                    /* Prefix side */
                    if (g_batch_prefix_len + enc_len < (int)sizeof(g_batch_prefix)) {
                        memcpy(g_batch_prefix + g_batch_prefix_len, enc, enc_len);
                        g_batch_prefix_len += enc_len;
                        g_batch_prefix[g_batch_prefix_len] = 0;
                        g_needs_redraw = 1;
                    }
                } else {
                    /* Suffix side */
                    if (g_batch_typed_len + enc_len < (int)sizeof(g_batch_typed)) {
                        memcpy(g_batch_typed + g_batch_typed_len, enc, enc_len);
                        g_batch_typed_len += enc_len;
                        g_batch_typed[g_batch_typed_len] = 0;
                        g_needs_redraw = 1;
                    }
                }
            }
            return 0;
        }
        if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) break;
        if (wp < 32 || wp == 127) break;
        Tab* t = active_tab();
        DWORD now = GetTickCount();
        if (now - g_typeahead_time > TYPEAHEAD_RESET_MS) g_typeahead_len = 0;
        g_typeahead_time = now;
        if (g_typeahead_len + enc_len < (int)sizeof(g_typeahead_buf)) {
            memcpy(g_typeahead_buf + g_typeahead_len, enc, enc_len);
            g_typeahead_len += enc_len;
            g_typeahead_buf[g_typeahead_len] = 0;
        }
        int n = g_typeahead_len;
        int start = 0;
        if (n == 1 && t->selected >= 0 &&
            _strnicmp(t->entries[t->selected].name, g_typeahead_buf, n) == 0) {
            start = t->selected + 1;
        }
        int found = -1;
        for (int k = 0; k < t->entry_count; k++) {
            int i = (start + k) % t->entry_count;
            if (_strnicmp(t->entries[i].name, g_typeahead_buf, n) == 0) {
                found = i; break;
            }
        }
        if (found >= 0) {
            sel_only(t, found);
            scroll_to_entry(t, found);
            g_needs_redraw = 1;
        }
        return 0;
    }

    case WM_KEYDOWN: {
        Tab* t = active_tab();
        int ctrl  = GetKeyState(VK_CONTROL) < 0;
        int shift = GetKeyState(VK_SHIFT) < 0;
        if (g_ff_active) {
            if (wp == VK_ESCAPE) { ff_close(); return 0; }
            if (wp == VK_RETURN) { ff_open_selected(); return 0; }
            if (wp == VK_UP) {
                if (g_ff_selected > 0) g_ff_selected--;
                g_needs_redraw = 1; return 0;
            }
            if (wp == VK_DOWN) {
                if (g_ff_selected + 1 < g_ff_result_count) g_ff_selected++;
                g_needs_redraw = 1; return 0;
            }
            if (wp == VK_PRIOR) {   /* Page Up */
                g_ff_selected -= 10;
                if (g_ff_selected < 0) g_ff_selected = 0;
                g_needs_redraw = 1; return 0;
            }
            if (wp == VK_NEXT) {    /* Page Down */
                g_ff_selected += 10;
                if (g_ff_selected >= g_ff_result_count) g_ff_selected = g_ff_result_count - 1;
                if (g_ff_selected < 0) g_ff_selected = 0;
                g_needs_redraw = 1; return 0;
            }
            if (wp == VK_BACK) {
                if (g_ff_cursor > 0) {
                    int new_cur = ctrl
                        ? ff_prev_word_boundary(g_ff_query, g_ff_cursor)
                        : ff_prev_cp(g_ff_query, g_ff_cursor);
                    int del = g_ff_cursor - new_cur;
                    int tail = g_ff_query_len - g_ff_cursor;
                    memmove(g_ff_query + new_cur, g_ff_query + g_ff_cursor, tail);
                    g_ff_query_len -= del;
                    g_ff_cursor     = new_cur;
                    g_ff_query[g_ff_query_len] = 0;
                    g_ff_selected = 0; g_ff_scroll = 0;
                    ff_refresh_results();
                    g_needs_redraw = 1;
                }
                return 0;
            }
            if (wp == VK_DELETE) {
                if (g_ff_cursor < g_ff_query_len) {
                    int end = ctrl
                        ? ff_next_word_boundary(g_ff_query, g_ff_query_len, g_ff_cursor)
                        : ff_next_cp(g_ff_query, g_ff_query_len, g_ff_cursor);
                    int del = end - g_ff_cursor;
                    int tail = g_ff_query_len - end;
                    memmove(g_ff_query + g_ff_cursor, g_ff_query + end, tail);
                    g_ff_query_len -= del;
                    g_ff_query[g_ff_query_len] = 0;
                    g_ff_selected = 0; g_ff_scroll = 0;
                    ff_refresh_results();
                    g_needs_redraw = 1;
                }
                return 0;
            }
            if (wp == VK_LEFT) {
                g_ff_cursor = ctrl
                    ? ff_prev_word_boundary(g_ff_query, g_ff_cursor)
                    : ff_prev_cp(g_ff_query, g_ff_cursor);
                g_needs_redraw = 1; return 0;
            }
            if (wp == VK_RIGHT) {
                g_ff_cursor = ctrl
                    ? ff_next_word_boundary(g_ff_query, g_ff_query_len, g_ff_cursor)
                    : ff_next_cp(g_ff_query, g_ff_query_len, g_ff_cursor);
                g_needs_redraw = 1; return 0;
            }
            if (wp == VK_HOME) { g_ff_cursor = 0;              g_needs_redraw = 1; return 0; }
            if (wp == VK_END)  { g_ff_cursor = g_ff_query_len; g_needs_redraw = 1; return 0; }
            if (wp == 'R' && ctrl) {
                g_ff_recursive = !g_ff_recursive;
                if (g_ff_recursive) ff_kick_recursive_scan();
                else {
                    ff_stop_scan();
                    ff_build_index_from_tab();
                }
                ff_refresh_results();
                g_needs_redraw = 1;
                return 0;
            }
            /* Ctrl+.  (or Ctrl+period) → stop the recursive scan without
               closing the finder. Also VK_OEM_PERIOD. */
            if (ctrl && (wp == VK_OEM_PERIOD || wp == 0xBE)) {
                if (g_ff_scanning) {
                    ff_stop_scan();
                    g_needs_redraw = 1;
                }
                return 0;
            }
            /* Swallow all other keys while active */
            return 0;
        }
        if (wp == 'F' && ctrl && !shift) { ff_open(); return 0; }
        if (g_batch_active) {
            if (wp == VK_RETURN) { batch_rename_commit(); return 0; }
            if (wp == VK_ESCAPE) { batch_rename_cancel(); return 0; }
            if (wp == VK_LEFT || wp == VK_HOME) {
                g_batch_focus = 1;   /* prefix side */
                g_needs_redraw = 1;
                return 0;
            }
            if (wp == VK_RIGHT || wp == VK_END) {
                g_batch_focus = 0;   /* suffix side */
                g_needs_redraw = 1;
                return 0;
            }
            if (wp == VK_BACK) {
                if (g_batch_focus == 1) {
                    if (g_batch_prefix_len > 0) {
                        int p = g_batch_prefix_len - 1;
                        while (p > 0 && (g_batch_prefix[p] & 0xC0) == 0x80) p--;
                        g_batch_prefix_len = p;
                        g_batch_prefix[p] = 0;
                    } else {
                        g_batch_chop_left++;
                    }
                } else {
                    if (g_batch_typed_len > 0) {
                        int p = g_batch_typed_len - 1;
                        while (p > 0 && (g_batch_typed[p] & 0xC0) == 0x80) p--;
                        g_batch_typed_len = p;
                        g_batch_typed[p] = 0;
                    } else {
                        g_batch_chop++;
                    }
                }
                g_needs_redraw = 1;
                return 0;
            }
            if (wp == VK_DELETE) {
                /* Forward-delete: eat one stem char on the opposite side */
                if (g_batch_focus == 1) g_batch_chop_left++;
                else                    g_batch_chop++;
                g_needs_redraw = 1;
                return 0;
            }
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (g_settings_open) { g_settings_open = 0; g_needs_redraw = 1; return 0; }
            g_typeahead_len = 0; g_needs_redraw = 1; return 0;
        }
        if (wp == VK_BACK) { tab_go_up(t); g_needs_redraw = 1; }
        else if (wp == VK_F5) { thumb_cache_clear(); scan_directory(t); }
        else if (wp == VK_F2 && t->selected >= 0) { do_rename(t, t->selected); }
        else if (wp == VK_DELETE && sel_count(t) > 0) {
            if (shift) do_permanent_delete_selected(t);
            else       do_delete_selected(t);
            scan_directory(t);
        }
        else if (wp == VK_RETURN && t->selected >= 0) {
            do_open_entry(t, t->selected);
            g_needs_redraw = 1;
        }
        else if (wp=='A' && ctrl) {
            sel_clear(t);
            for (int i = 0; i < t->entry_count; i++)
                if (strcmp(t->entries[i].name, "..") != 0) t->sel_mask[i] = 1;
            g_needs_redraw = 1;
        }
        else if (wp=='C' && ctrl && sel_count(t) > 0) {
            clipboard_set_selected(t, DROPEFFECT_COPY);
        }
        else if (wp=='X' && ctrl && sel_count(t) > 0) {
            clipboard_set_selected(t, DROPEFFECT_MOVE);
        }
        else if (wp=='V' && ctrl) {
            clipboard_paste(t->path);
            scan_directory(t);
        }
        else if (wp=='D' && ctrl) { handle_context_cmd(IDM_OPEN_TERMINAL, -1); }
        else if (wp=='N' && ctrl && shift) { do_new_folder(t); }
        else if (wp=='N' && ctrl) { do_new_file(t); }
        else if (wp=='T' && ctrl && shift) { theme_load(); }
        else if (wp=='T' && ctrl) { new_tab(t->path); g_needs_redraw=1; }
        else if (wp == VK_TAB && (ctrl || shift)) {
            Panel* p = &g_app.panels[g_app.active_panel];
            if (p->tab_count > 1) {
                int dir = shift ? -1 : 1;
                p->active_tab = (p->active_tab + dir + p->tab_count) % p->tab_count;
                tabs_save();
                g_needs_redraw = 1;
            }
            return 0;
        }
        else if (wp == VK_TAB && g_app.split_active && !ctrl && !shift) {
            g_app.active_panel = 1 - g_app.active_panel;
            tabs_save();
            g_needs_redraw = 1;
            return 0;
        }
        else if (wp==VK_OEM_5 && ctrl) {  /* Ctrl+\ → toggle split */
            g_app.split_active = !g_app.split_active;
            if (g_app.split_active && g_app.panels[1].tab_count == 0) {
                int saved = g_app.active_panel;
                g_app.active_panel = 1;
                new_tab(g_app.panels[0].tabs[g_app.panels[0].active_tab].path);
                g_app.active_panel = saved;
            }
            tabs_save();
            g_needs_redraw = 1;
        }
        else if (wp=='W' && ctrl) { close_tab(g_app.panels[g_app.active_panel].active_tab); g_needs_redraw=1; }
        else if (wp==VK_UP && t->selected > 0) {
            t->selected--;
            if (shift) {
                int from = (t->sel_anchor >= 0) ? t->sel_anchor : t->selected;
                sel_range(t, from, t->selected);
            } else {
                sel_only(t, t->selected);
            }
            scroll_to_entry(t, t->selected);
            g_needs_redraw = 1;
        }
        else if (wp==VK_DOWN && t->selected < t->entry_count-1) {
            t->selected++;
            if (shift) {
                int from = (t->sel_anchor >= 0) ? t->sel_anchor : t->selected;
                sel_range(t, from, t->selected);
            } else {
                sel_only(t, t->selected);
            }
            scroll_to_entry(t, t->selected);
            g_needs_redraw = 1;
        }
        g_needs_redraw = 1;
        return 0;
    }

    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_MENUCHAR:
        if (g_shell_ctx3) {
            LRESULT res = 0;
            if (IContextMenu3_HandleMenuMsg2(g_shell_ctx3, msg, wp, lp, &res) == S_OK) return res;
        } else if (g_shell_ctx2) {
            IContextMenu2_HandleMenuMsg(g_shell_ctx2, msg, wp, lp);
            return 0;
        }
        break;

    case WM_APP + 1:
        /* Deferred inline rename commit (from WM_KILLFOCUS) */
        if (g_edit_hwnd) inline_rename_commit();
        return 0;

    case WM_APP + 20:
        /* Recursive scan progress tick — re-run match against grown index */
        if (g_ff_active) {
            ff_refresh_results();
            g_needs_redraw = 1;
        }
        return 0;

    case WM_APP + 2:
        if (g_addr_hwnd) addr_edit_commit();
        return 0;

    case WM_APP + 3: {
        /* Drain completed thumbnails: upload to GL + put in cache */
        ThumbDone done[THUMB_Q_CAP];
        int n = 0;
        EnterCriticalSection(&g_thumb_cs);
        n = g_thumb_done_count;
        if (n > 0) {
            memcpy(done, g_thumb_done, n * sizeof(ThumbDone));
            g_thumb_done_count = 0;
        }
        LeaveCriticalSection(&g_thumb_cs);
        for (int i = 0; i < n; i++) {
            int w, h;
            GLuint tex = hbmp_to_gl(done[i].bmp, &w, &h, done[i].flip_v);
            DeleteObject(done[i].bmp);
            if (tex) thumb_cache_put(done[i].path, tex, w, h);
        }
        if (n > 0) g_needs_redraw = 1;
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        if ((HWND)lp != g_edit_hwnd && (HWND)lp != g_addr_hwnd) break;
        HDC hdc_edit = (HDC)wp;
        /* Pull colours from the active theme so light themes get dark text
           on a light background instead of the old hardcoded pair. Cached
           brushes are dropped and recreated when the colour changes so
           Ctrl+Shift+T (theme reload) picks up immediately. */
        COLORREF fg = RGB((COL_TEXT     >> 16) & 0xFF,
                          (COL_TEXT     >> 8)  & 0xFF,
                           COL_TEXT             & 0xFF);
        COLORREF bg = RGB((COL_SELECTED >> 16) & 0xFF,
                          (COL_SELECTED >> 8)  & 0xFF,
                           COL_SELECTED         & 0xFF);
        SetTextColor(hdc_edit, fg);
        SetBkColor  (hdc_edit, bg);
        HBRUSH* slot = ((HWND)lp == g_edit_hwnd) ? &g_edit_bg_brush : &g_addr_bg_brush;
        static COLORREF last_edit_bg = 0, last_addr_bg = 0;
        COLORREF* last = ((HWND)lp == g_edit_hwnd) ? &last_edit_bg : &last_addr_bg;
        if (!*slot || *last != bg) {
            if (*slot) DeleteObject(*slot);
            *slot = CreateSolidBrush(bg);
            *last = bg;
        }
        return (LRESULT)*slot;
    }

    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- OpenGL context ---- */
static HGLRC create_gl_context(HDC hdc) {
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32; pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    SetPixelFormat(hdc, ChoosePixelFormat(hdc, &pfd), &pfd);
    HGLRC legacy = wglCreateContext(hdc);
    wglMakeCurrent(hdc, legacy);
    PFN_wglCreateContextAttribsARB wglCCA =
        (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!wglCCA) return legacy;
    int att[] = { WGL_CONTEXT_MAJOR_VERSION_ARB,3, WGL_CONTEXT_MINOR_VERSION_ARB,3,
                  WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    HGLRC core = wglCCA(hdc, NULL, att);
    wglMakeCurrent(hdc, core);
    wglDeleteContext(legacy);
    PFN_wglSwapIntervalEXT si = (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT");
    if (si) si(1);
    return core;
}

/* ---- Entry point ---- */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int cmdShow) {
    (void)hPrev; (void)cmdLine;

    /* Declare per-monitor DPI awareness so mouse coords and window size stay
       in the same coord space when on a high-DPI / scaled monitor. Resolved
       dynamically so the binary still loads on Windows < 10 1607 (older
       OSes silently fall back to whatever DPI awareness the manifest set). */
    {
        typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32) {
            PFN_SetProcessDpiAwarenessContext set_dpi =
                (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
            if (set_dpi) set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    OleInitialize(NULL);
    g_drop_target.lpVtbl = &g_dt_vtbl;
    g_drop_target.refs = 1;
    thumb_init();

    char* prof = getenv("USERPROFILE");
    strncpy(g_user_profile, prof ? prof : "C:\\Users\\user", MAX_PATH);
    _snprintf(g_downloads_path, MAX_PATH, "%s\\Downloads", g_user_profile);
    g_cf_drop_effect = RegisterClipboardFormatA("Preferred DropEffect");
    /* Preferences must load before font creation so font_size applies. */
    settings_load();

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"FilePathXClass";
    wc.hIcon = LoadIconW(hInst, L"IDI_APPICON");
    wc.hIconSm = LoadIconW(hInst, L"IDI_APPICON");
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(
        0, L"FilePathXClass", L"FilePathX",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        (sx-g_width)/2, (sy-g_height)/2, g_width, g_height,
        NULL, NULL, hInst, NULL);

    /* Dark/light title bar based on theme background luminance */
    {
        uint32_t c = COL_BG;
        int lum = (77 * ((c>>16)&0xFF) + 150 * ((c>>8)&0xFF) + 29 * (c&0xFF)) >> 8;
        BOOL dark = (lum < 128);
        DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }

    /* Extend DWM frame for shadow on borderless window */
    MARGINS margins = {0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);

    /* Register drop target so files dragged into our window are received */
    RegisterDragDrop(g_hwnd, (IDropTarget*)&g_drop_target);

    /* Force WM_NCCALCSIZE to fire so frame is recalculated without title bar */
    SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    HDC hdc = GetDC(g_hwnd);
    HGLRC glrc = create_gl_context(hdc);
    const char* fail = NULL;
    if (!glrc) fail =
        "Could not create an OpenGL 3.3 context.\n\n"
        "This usually means:\n"
        "  - the GPU driver doesn't support OpenGL 3.3 (very old GPU,\n"
        "    Windows running over Remote Desktop, or a VM without\n"
        "    GPU acceleration), or\n"
        "  - no GPU driver is installed and Windows is using the\n"
        "    basic display adapter (OpenGL 1.1 only).\n\n"
        "Try updating your graphics driver from the GPU vendor\n"
        "(Intel/AMD/NVIDIA). Windows Update's driver is sometimes too\n"
        "old to expose OpenGL 3.3.";
    else if (!render_load_gl_functions()) fail =
        "An OpenGL 3.3 context was created but required OpenGL\n"
        "functions could not be resolved. Update your GPU driver.";
    else if (!render_init(&g_renderer)) fail =
        "OpenGL shader / VBO setup failed.";
    else if (!render_create_font(&g_renderer, "Segoe UI", g_pref_font_size)) fail =
        "Could not create the primary font 'Segoe UI'.\n"
        "Install the Segoe UI font family.";
    else if (!render_create_font_small(&g_renderer, "Segoe UI", g_pref_font_size - 1)) fail =
        "Could not create the small font 'Segoe UI'.";
    else if (!render_create_icons(&g_renderer, 16)) fail =
        "Could not load the MDL2 icon font 'Segoe MDL2 Assets'.\n"
        "This font ships with Windows 10 and 11. On older Windows you\n"
        "would need to install it manually.";
    if (fail) {
        MessageBoxA(NULL, fail, "FilePathX — failed to initialize", MB_OK | MB_ICONERROR);
        return 1;
    }

    ui_init(&g_ui, &g_renderer);
    g_logo_tex = load_logo_texture(hInst);
    g_app.sort_asc = 1;
    g_app.split_ratio = 0.5f;
    g_app.panels[0].last_click_item = -1;
    g_app.panels[1].last_click_item = -1;
    sort_prefs_load();
    view_prefs_load();
    theme_load();
    theme_discover();
    init_sidebar();

    char cwd[MAX_PATH] = {0};
    WCHAR wcwd[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, wcwd) == 0) strcpy(cwd, "C:\\");
    else w_to_u8(wcwd, cwd, MAX_PATH);
    tabs_load(cwd);

    ShowWindow(g_hwnd, cmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (1) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        /* Non-blocking watcher check — happens every iteration so we don't miss
           events when the loop is busy redrawing (mouse move etc.). */
        if (g_watch_handle != INVALID_HANDLE_VALUE &&
            WaitForSingleObject(g_watch_handle, 0) == WAIT_OBJECT_0) {
            scan_directory(active_tab());
            FindNextChangeNotification(g_watch_handle);
            g_needs_redraw = 1;
        }
        if (g_needs_redraw) {
            g_needs_redraw = 0;
            RECT rc; GetClientRect(g_hwnd, &rc);
            g_width = rc.right; g_height = rc.bottom;
            if (g_width > 0 && g_height > 0) {
                render_begin(&g_renderer, g_width, g_height);
                build_ui();
                render_end(&g_renderer);
                SwapBuffers(hdc);
            }
            g_mouse_clicked = 0; g_mouse_released = 0;
            g_mouse_dblclick = 0; g_scroll_delta = 0;
        } else {
            HANDLE handles[1];
            DWORD nh = 0;
            if (g_watch_handle != INVALID_HANDLE_VALUE) handles[nh++] = g_watch_handle;
            if (nh > 0) MsgWaitForMultipleObjects(nh, handles, FALSE, INFINITE, QS_ALLINPUT);
            else        WaitMessage();
            /* Watcher event is handled by the top-of-loop non-blocking check */
        }
    }
done:
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(glrc);
    ReleaseDC(g_hwnd, hdc);
    return 0;
}
