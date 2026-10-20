#ifndef LAYOUT_H
#define LAYOUT_H

/* Panel layout tree, modeled on File Pilot: every node is either a leaf
   (shows one panel) or a group that splits its rect among its children —
   side by side when vertical, stacked otherwise — by each child's ratio. */

#define MAX_PANELS  8
#define MAX_LNODES  (MAX_PANELS * 2)

typedef struct {
    int   used;
    int   parent, child, next, prev;   /* node indices, -1 = none */
    int   panel;                        /* leaf: panel index; group: -1 */
    int   vertical;                     /* group: 1 = children left->right */
    float ratio;                        /* share of parent's extent */
    float x, y, w, h;                   /* pixel rect from layout_compute */
} LNode;

typedef struct {
    LNode n[MAX_LNODES];
    int   root;
} Layout;

void layout_init(Layout* L, int panel);
int  layout_leaf(const Layout* L, int panel);          /* node of panel, -1 */
int  layout_panel_count(const Layout* L);
/* Split panel's leaf; new_panel goes right (vertical) or below. Returns
   the new leaf node, or -1 if the node pool is full. */
int  layout_split(Layout* L, int panel, int vertical, int new_panel);
/* Remove panel's leaf; its space goes to the neighbour. No-op on the last. */
void layout_remove(Layout* L, int panel);
/* Lay the tree out in a rect; no leaf goes below min_w x min_h while the
   rect has room for that. */
void layout_compute(Layout* L, float x, float y, float w, float h, float min_w, float min_h);
/* Smallest rect the whole tree fits in with every leaf at its minimum. */
void layout_min_size(const Layout* L, float min_w, float min_h, float* w, float* h);
int  layout_panel_at(const Layout* L, float x, float y);   /* panel, -1 */
/* Next/previous panel in tree order (wraps). */
int  layout_panel_step(const Layout* L, int panel, int dir);
/* Splitter after node c (between c and c->next). Returns c, or -1. */
int  layout_splitter_at(const Layout* L, float x, float y, float slop);
/* Drag splitter c to mouse position; keeps both sides at their minimum. */
void layout_splitter_drag(Layout* L, int c, float mx, float my, float min_w, float min_h);
/* Even out the ratios of panel's siblings. */
void layout_even(Layout* L, int panel);
/* Validate a loaded tree; returns 1 if usable. */
int  layout_valid(const Layout* L);

#endif
