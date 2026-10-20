#include "layout.h"
#include <string.h>
#include <math.h>

static int node_alloc(Layout* L) {
    for (int i = 0; i < MAX_LNODES; i++) {
        if (L->n[i].used) continue;
        LNode* n = &L->n[i];
        memset(n, 0, sizeof(*n));
        n->used = 1;
        n->parent = n->child = n->next = n->prev = n->panel = -1;
        n->ratio = 1.0f;
        return i;
    }
    return -1;
}

static int free_nodes(const Layout* L) {
    int k = 0;
    for (int i = 0; i < MAX_LNODES; i++) k += !L->n[i].used;
    return k;
}

void layout_init(Layout* L, int panel) {
    memset(L, 0, sizeof(*L));
    L->root = node_alloc(L);
    L->n[L->root].panel = panel;
}

int layout_leaf(const Layout* L, int panel) {
    for (int i = 0; i < MAX_LNODES; i++)
        if (L->n[i].used && L->n[i].panel == panel && L->n[i].child < 0) return i;
    return -1;
}

int layout_panel_count(const Layout* L) {
    int k = 0;
    for (int i = 0; i < MAX_LNODES; i++) k += L->n[i].used && L->n[i].panel >= 0;
    return k;
}

static void even_children(Layout* L, int g) {
    int k = 0;
    for (int c = L->n[g].child; c >= 0; c = L->n[c].next) k++;
    for (int c = L->n[g].child; c >= 0; c = L->n[c].next) L->n[c].ratio = 1.0f / (float)k;
}

/* If the parent runs the other way, the leaf turns into a group holding a
   copy of itself; the new leaf is linked in after it and all siblings get
   an even share. */
int layout_split(Layout* L, int panel, int vertical, int new_panel) {
    int leaf = layout_leaf(L, panel);
    if (leaf < 0) return -1;
    int p = L->n[leaf].parent;
    int wrap = (p < 0 || L->n[p].vertical != vertical);
    if (free_nodes(L) < 1 + wrap) return -1;
    if (wrap) {
        int c = node_alloc(L);
        L->n[c].panel  = panel;
        L->n[c].parent = leaf;
        L->n[leaf].panel    = -1;
        L->n[leaf].vertical = vertical;
        L->n[leaf].child    = c;
        p = leaf; leaf = c;
    }
    int nn = node_alloc(L);
    L->n[nn].panel  = new_panel;
    L->n[nn].parent = p;
    L->n[nn].prev   = leaf;
    L->n[nn].next   = L->n[leaf].next;
    if (L->n[leaf].next >= 0) L->n[L->n[leaf].next].prev = nn;
    L->n[leaf].next = nn;
    even_children(L, p);
    return nn;
}

static void unlink_node(Layout* L, int c) {
    LNode* n = &L->n[c];
    if (n->prev >= 0) L->n[n->prev].next = n->next;
    else if (n->parent >= 0) L->n[n->parent].child = n->next;
    if (n->next >= 0) L->n[n->next].prev = n->prev;
    n->used = 0;
}

/* Close: the neighbour takes the freed share.
   A group left with one child collapses into that child, and a group that
   ends up inside a same-direction parent is spliced into it. */
void layout_remove(Layout* L, int panel) {
    int c = layout_leaf(L, panel);
    if (c < 0 || c == L->root) return;
    int p = L->n[c].parent;
    int nb = L->n[c].prev >= 0 ? L->n[c].prev : L->n[c].next;
    if (nb >= 0) L->n[nb].ratio += L->n[c].ratio;
    unlink_node(L, c);

    int k = L->n[p].child;
    if (k < 0 || L->n[k].next >= 0) return;
    /* p has one child k: move k's content into p's slot. */
    L->n[p].panel    = L->n[k].panel;
    L->n[p].vertical = L->n[k].vertical;
    L->n[p].child    = L->n[k].child;
    for (int g = L->n[p].child; g >= 0; g = L->n[g].next) L->n[g].parent = p;
    L->n[k].used = 0;

    int gp = L->n[p].parent;
    if (L->n[p].child < 0 || gp < 0 || L->n[gp].vertical != L->n[p].vertical) return;
    /* p is a group inside a same-direction parent: splice its children in. */
    float share = L->n[p].ratio;
    int before = L->n[p].prev, after = L->n[p].next;
    int first = L->n[p].child, last = first;
    for (int g = first; g >= 0; g = L->n[g].next) {
        L->n[g].parent = gp;
        L->n[g].ratio *= share;
        last = g;
    }
    L->n[first].prev = before;
    L->n[last].next  = after;
    if (before >= 0) L->n[before].next = first; else L->n[gp].child = first;
    if (after >= 0) L->n[after].prev = last;
    L->n[p].used = 0;
}

/* Smallest size node i can take when every leaf needs at least mw x mh. */
static void min_size(const Layout* L, int i, float mw, float mh, float* w, float* h) {
    const LNode* n = &L->n[i];
    if (n->child < 0) { *w = mw; *h = mh; return; }
    *w = *h = 0;
    for (int c = n->child; c >= 0; c = L->n[c].next) {
        float cw, ch;
        min_size(L, c, mw, mh, &cw, &ch);
        if (n->vertical) { *w += cw; if (ch > *h) *h = ch; }
        else             { *h += ch; if (cw > *w) *w = cw; }
    }
}

static float min_extent(const Layout* L, int c, int vertical, float mw, float mh) {
    float w, h;
    min_size(L, c, mw, mh, &w, &h);
    return vertical ? w : h;
}

void layout_min_size(const Layout* L, float mw, float mh, float* w, float* h) {
    min_size(L, L->root, mw, mh, w, h);
}

static void compute(Layout* L, int i, float x, float y, float w, float h, float mw, float mh) {
    LNode* n = &L->n[i];
    n->x = x; n->y = y; n->w = w; n->h = h;
    float ext = n->vertical ? w : h, start = n->vertical ? x : y;
    int ids[MAX_LNODES], pinned[MAX_LNODES] = {0}, k = 0;
    float sz[MAX_LNODES], mn[MAX_LNODES], mn_total = 0;
    for (int c = n->child; c >= 0; c = L->n[c].next) {
        ids[k] = c;
        mn[k] = min_extent(L, c, n->vertical, mw, mh);
        mn_total += mn[k++];
    }
    /* Children share ext by ratio; one that would fall below its minimum is
       pinned there and the rest re-share what is left. The ratios are left
       alone, so the layout comes back when the window grows again. */
    for (int changed = mn_total <= ext; changed; ) {
        float free_ext = ext, free_ratio = 0;
        for (int j = 0; j < k; j++) {
            if (pinned[j]) free_ext -= mn[j];
            else           free_ratio += L->n[ids[j]].ratio;
        }
        changed = 0;
        for (int j = 0; j < k; j++) {
            if (pinned[j]) continue;
            float s = free_ratio > 0 ? free_ext * L->n[ids[j]].ratio / free_ratio : 0;
            if (s < mn[j]) pinned[j] = changed = 1;
        }
    }
    int any_pinned = 0;
    for (int j = 0; j < k; j++) any_pinned |= pinned[j];
    if (any_pinned) {
        float free_ext = ext, free_ratio = 0;
        for (int j = 0; j < k; j++) {
            if (pinned[j]) free_ext -= mn[j];
            else           free_ratio += L->n[ids[j]].ratio;
        }
        for (int j = 0; j < k; j++)
            sz[j] = pinned[j] ? mn[j] : free_ext * L->n[ids[j]].ratio / free_ratio;
    } else {
        for (int j = 0; j < k; j++) sz[j] = ext * L->n[ids[j]].ratio;
    }
    float acc = 0;
    for (int j = 0; j < k; j++) {
        /* Snap edges to whole pixels; the last child takes the remainder. */
        float a = floorf(start + acc);
        acc += sz[j];
        float b = j + 1 < k ? floorf(start + acc) : start + ext;
        if (n->vertical) compute(L, ids[j], a, y, b - a, h, mw, mh);
        else             compute(L, ids[j], x, a, w, b - a, mw, mh);
    }
}

void layout_compute(Layout* L, float x, float y, float w, float h, float min_w, float min_h) {
    compute(L, L->root, x, y, w, h, min_w, min_h);
}

int layout_panel_at(const Layout* L, float x, float y) {
    for (int i = 0; i < MAX_LNODES; i++) {
        const LNode* n = &L->n[i];
        if (n->used && n->panel >= 0 &&
            x >= n->x && x < n->x + n->w && y >= n->y && y < n->y + n->h)
            return n->panel;
    }
    return -1;
}

static void leaves(const Layout* L, int i, int* out, int* k) {
    if (L->n[i].child < 0) { out[(*k)++] = L->n[i].panel; return; }
    for (int c = L->n[i].child; c >= 0; c = L->n[c].next) leaves(L, c, out, k);
}

int layout_panel_step(const Layout* L, int panel, int dir) {
    int list[MAX_LNODES], k = 0;
    leaves(L, L->root, list, &k);
    for (int i = 0; i < k; i++)
        if (list[i] == panel) return list[(i + dir + k) % k];
    return k ? list[0] : panel;
}

int layout_splitter_at(const Layout* L, float x, float y, float slop) {
    for (int c = 0; c < MAX_LNODES; c++) {
        const LNode* n = &L->n[c];
        if (!n->used || n->next < 0 || n->parent < 0) continue;
        const LNode* g = &L->n[n->parent];
        if (g->vertical) {
            float e = n->x + n->w;
            if (fabsf(x - e) <= slop && y >= g->y && y < g->y + g->h) return c;
        } else {
            float e = n->y + n->h;
            if (fabsf(y - e) <= slop && x >= g->x && x < g->x + g->w) return c;
        }
    }
    return -1;
}

void layout_splitter_drag(Layout* L, int c, float mx, float my, float min_w, float min_h) {
    if (c < 0 || !L->n[c].used || L->n[c].next < 0) return;
    LNode* a = &L->n[c];
    LNode* b = &L->n[a->next];
    int v = L->n[a->parent].vertical;
    /* Work in the pixels the two panels have now, so a neighbour pinned at
       its minimum by layout_compute doesn't skew the drag. */
    float span  = v ? a->w + b->w : a->h + b->h;
    float pos   = v ? mx - a->x : my - a->y;
    float min_a = min_extent(L, c, v, min_w, min_h);
    float min_b = min_extent(L, a->next, v, min_w, min_h);
    if (span <= 0 || span < min_a + min_b) return;
    if (pos < min_a) pos = min_a;
    if (pos > span - min_b) pos = span - min_b;
    float total = a->ratio + b->ratio;
    a->ratio = total * pos / span;
    b->ratio = total - a->ratio;
}

void layout_even(Layout* L, int panel) {
    int c = layout_leaf(L, panel);
    if (c >= 0 && L->n[c].parent >= 0) even_children(L, L->n[c].parent);
}

static int check(const Layout* L, int i, int parent, int depth, int* seen, int* panels) {
    if (i < 0 || i >= MAX_LNODES || !L->n[i].used || seen[i] || depth > MAX_LNODES) return 0;
    const LNode* n = &L->n[i];
    seen[i] = 1;
    if (n->parent != parent || !(n->ratio > 0.0f && n->ratio <= 1.0f)) return 0;
    if (n->child < 0) {
        if (n->panel < 0 || n->panel >= MAX_PANELS || panels[n->panel]) return 0;
        panels[n->panel] = 1;
        return 1;
    }
    if (n->panel >= 0) return 0;
    int prev = -1;
    for (int c = n->child; c != -1; c = L->n[c].next) {
        if (c < 0 || c >= MAX_LNODES || L->n[c].prev != prev) return 0;
        if (!check(L, c, i, depth + 1, seen, panels)) return 0;
        prev = c;
    }
    return 1;
}

int layout_valid(const Layout* L) {
    int seen[MAX_LNODES] = {0}, panels[MAX_PANELS] = {0};
    if (L->root < 0 || L->root >= MAX_LNODES) return 0;
    if (!check(L, L->root, -1, 0, seen, panels)) return 0;
    for (int i = 0; i < MAX_LNODES; i++)
        if (L->n[i].used && !seen[i]) return 0;
    return 1;
}
