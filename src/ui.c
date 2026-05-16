#include "ui.h"
#include <string.h>

void ui_init(UI* ui, Renderer* r) { memset(ui, 0, sizeof(*ui)); ui->renderer = r; }

void ui_begin(UI* ui, UIInput input) {
    ui->input = input;
    ui->hot_id = 0;
}

void ui_end(UI* ui) {
    if (!ui->input.mouse_down) ui->active_id = 0;
}

int ui_hover(UI* ui, float x, float y, float w, float h) {
    int mx = ui->input.mouse_x, my = ui->input.mouse_y;
    return (mx >= x && mx < x+w && my >= y && my < y+h);
}

int ui_clicked(UI* ui, int id, float x, float y, float w, float h) {
    int hov = ui_hover(ui, x, y, w, h);
    if (hov) {
        ui->hot_id = id;
        if (ui->input.mouse_clicked && ui->active_id == 0) ui->active_id = id;
    }
    return (ui->active_id == id && ui->input.mouse_released && hov);
}

int ui_dblclicked(UI* ui, int id, float x, float y, float w, float h) {
    int hov = ui_hover(ui, x, y, w, h);
    return (hov && ui->input.mouse_dblclick);
}

/* ---- Tab widget ---- */
int ui_tab(UI* ui, int id, float x, float y, float w, float h,
           const char* title, int active, GLuint icon_tex, uint32_t accent_col)
{
    Renderer* r = ui->renderer;
    int hov = ui_hover(ui, x, y, w, h);
    int result = 0;

    uint32_t bg = active ? COL_BG : (hov ? COL_HOVER : COL_HEADER);
    render_quad(r, x, y, w, h, bg);
    if (active) render_quad(r, x, y + h - 2, w, 2, accent_col);

    /* Folder icon */
    float ty = y + (h - r->font_height) / 2.0f;
    if (icon_tex)
        render_icon(r, icon_tex, x + 6, y + (h - 16) / 2, 16, 16);
    else
        render_folder_icon(r, x + 8, y + (h - 10) / 2, 11, COL_YELLOW);

    /* Title - truncate if needed */
    float max_text_w = w - 40;
    int len = (int)strlen(title);
    while (len > 0 && render_text_width_n(r, title, len) > (int)max_text_w) len--;
    render_text_n(r, title, len, x + 24, ty, active ? COL_TEXT : COL_SUBTEXT);

    /* Close button (MDL2 icon) */
    float csz = 10;
    float cx = x + w - 18, cy = y + (h - 14) / 2;
    int close_hov = ui_hover(ui, cx, cy, 14, 14);
    if (close_hov) render_quad(r, cx, cy, 14, 14, COL_OVERLAY);
    render_mdl2(r, ICON_CLOSE, cx + 2, cy + 2, csz, close_hov ? COL_RED : COL_DIM);

    if (ui->input.mouse_clicked && close_hov) { result = 2; }
    else if (ui_clicked(ui, id, x, y, w, h)) { result = 1; }
    return result;
}

/* ---- Icon button (text label) ---- */
int ui_icon_btn(UI* ui, int id, float x, float y, float w, float h,
                const char* label, int enabled)
{
    Renderer* r = ui->renderer;
    int hov = ui_hover(ui, x, y, w, h) && enabled;
    uint32_t col = enabled ? (hov ? COL_TEXT : COL_SUBTEXT) : COL_DIM;
    if (hov) render_quad(r, x, y, w, h, COL_HOVER);
    int tw = render_text_width(r, label);
    float tx = x + (w - tw) / 2, ty = y + (h - r->font_height) / 2;
    render_text(r, label, tx, ty, col);
    return (hov && ui_clicked(ui, id, x, y, w, h));
}

/* ---- MDL2 icon button ---- */
int ui_mdl2_btn(UI* ui, int id, float x, float y, float w, float h,
                int icon_id, float icon_sz, int enabled)
{
    Renderer* r = ui->renderer;
    int hov = ui_hover(ui, x, y, w, h) && enabled;
    uint32_t col = enabled ? (hov ? COL_TEXT : COL_SUBTEXT) : COL_DIM;
    if (hov) render_quad(r, x, y, w, h, COL_HOVER);
    float ix = x + (w - icon_sz) / 2, iy = y + (h - icon_sz) / 2;
    render_mdl2(r, icon_id, ix, iy, icon_sz, col);
    return (hov && ui_clicked(ui, id, x, y, w, h));
}

/* ---- Sidebar section header ---- */
int ui_section(UI* ui, int id, float x, float y, float w, float h,
               const char* title, int expanded, int section_icon)
{
    Renderer* r = ui->renderer;
    int hov = ui_hover(ui, x, y, w, h);
    if (hov) render_quad(r, x, y, w, h, COL_HOVER);
    float ty = y + (h - r->font_height) / 2;

    /* Chevron (MDL2) */
    float csz = 8;
    float cy = y + (h - csz) / 2;
    render_mdl2(r, expanded ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, x + 4, cy, csz, COL_DIM);

    /* Section icon */
    if (section_icon >= 0) {
        float isz = 11;
        float iy = y + (h - isz) / 2;
        render_mdl2(r, section_icon, x + 15, iy, isz, COL_SUBTEXT);
    }

    /* Title */
    render_text(r, title, x + 30, ty, COL_SUBTEXT);

    return ui_clicked(ui, id, x, y, w, h);
}

/* ---- Scrollbar ---- */
float ui_scrollbar(UI* ui, int id, float x, float y, float w, float h,
                   float content_h, float scroll_y)
{
    Renderer* r = ui->renderer;
    if (content_h <= h) {
        if (ui->active_id == id) ui->active_id = 0;
        return 0;
    }
    float sb_w = 6, sb_x = x + w - sb_w - 1;
    float ratio = h / content_h;
    float thumb_h = h * ratio;
    if (thumb_h < 20) thumb_h = 20;
    float max_scroll = content_h - h;
    float track_h   = h - thumb_h;
    float thumb_y   = y + (scroll_y / max_scroll) * track_h;

    /* Wider hit area than visual thumb for easier grabbing */
    int track_hov = ui_hover(ui, sb_x - 4, y, sb_w + 8, h);
    int thumb_hov = (track_hov &&
                     ui->input.mouse_y >= thumb_y &&
                     ui->input.mouse_y < thumb_y + thumb_h);

    /* Grab the thumb */
    if (thumb_hov && ui->input.mouse_clicked && ui->active_id == 0) {
        ui->active_id = id;
        ui->drag_offset = (float)ui->input.mouse_y - thumb_y;
    }
    /* Click on track outside thumb → jump one page */
    else if (track_hov && !thumb_hov && ui->input.mouse_clicked && ui->active_id == 0) {
        if (ui->input.mouse_y < thumb_y) scroll_y -= h;
        else scroll_y += h;
    }

    /* Active drag */
    if (ui->active_id == id && ui->input.mouse_down && track_h > 0) {
        float new_thumb_y = (float)ui->input.mouse_y - ui->drag_offset;
        float t_min = y, t_max = y + track_h;
        if (new_thumb_y < t_min) new_thumb_y = t_min;
        if (new_thumb_y > t_max) new_thumb_y = t_max;
        scroll_y = ((new_thumb_y - y) / track_h) * max_scroll;
    }

    if (scroll_y < 0) scroll_y = 0;
    if (scroll_y > max_scroll) scroll_y = max_scroll;

    thumb_y = y + (scroll_y / max_scroll) * track_h;
    int active = (ui->active_id == id);
    uint32_t col = (active || thumb_hov) ? COL_ACCENT : COL_SCROLLBAR;
    render_quad(r, sb_x, thumb_y, sb_w, thumb_h, col);
    return scroll_y;
}
