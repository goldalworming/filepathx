#ifndef UI_H
#define UI_H

#include "render.h"

typedef struct {
    int mouse_x, mouse_y;
    int mouse_down;
    int mouse_clicked;
    int mouse_released;
    int mouse_dblclick;
    float scroll_delta;
} UIInput;

typedef struct {
    Renderer* renderer;
    UIInput   input;
    int       hot_id;
    int       active_id;
    float     drag_offset;  /* for scrollbar drag: mouse_y - thumb_y at grab */
} UI;

void ui_init(UI* ui, Renderer* r);
void ui_begin(UI* ui, UIInput input);
void ui_end(UI* ui);

int ui_hover(UI* ui, float x, float y, float w, float h);
int ui_clicked(UI* ui, int id, float x, float y, float w, float h);
int ui_dblclicked(UI* ui, int id, float x, float y, float w, float h);

/* Tab: returns 0=nothing, 1=tab selected, 2=close clicked
   accent_col is the color of the 2px underline shown when active. */
int ui_tab(UI* ui, int id, float x, float y, float w, float h,
           const char* title, int active, GLuint icon_tex, uint32_t accent_col);

/* Text icon button */
int ui_icon_btn(UI* ui, int id, float x, float y, float w, float h,
                const char* label, int enabled);

/* MDL2 icon button */
int ui_mdl2_btn(UI* ui, int id, float x, float y, float w, float h,
                int icon_id, float icon_sz, int enabled);

/* Sidebar section header (collapsible) with section icon */
int ui_section(UI* ui, int id, float x, float y, float w, float h,
               const char* title, int expanded, int section_icon);

/* Scrollbar */
float ui_scrollbar(UI* ui, int id, float x, float y, float w, float h,
                   float content_h, float scroll_y);

#endif
