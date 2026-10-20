/* gcc -Isrc tests/layout_test.c src/layout.c -o build/layout_test && build/layout_test */
#include "layout.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    Layout L;
    layout_init(&L, 0);
    layout_compute(&L, 0, 0, 1000, 600, 0, 0);
    assert(layout_panel_at(&L, 500, 300) == 0);

    /* 0 | 1 side by side, then 1 split into 1 over 2 */
    assert(layout_split(&L, 0, 1, 1) >= 0);
    assert(layout_split(&L, 1, 0, 2) >= 0);
    assert(layout_valid(&L) && layout_panel_count(&L) == 3);
    layout_compute(&L, 0, 0, 1000, 600, 0, 0);
    assert(layout_panel_at(&L, 100, 300) == 0);
    assert(layout_panel_at(&L, 900, 100) == 1);
    assert(layout_panel_at(&L, 900, 500) == 2);
    LNode* n2 = &L.n[layout_leaf(&L, 2)];
    assert(n2->x == 500 && n2->y == 300 && n2->w == 500 && n2->h == 300);

    /* same-direction split adds a sibling instead of nesting */
    assert(layout_split(&L, 0, 1, 3) >= 0);
    layout_compute(&L, 0, 0, 900, 600, 0, 0);
    assert(L.n[layout_leaf(&L, 3)].x == 300 && L.n[layout_leaf(&L, 1)].x == 600);

    /* splitter between 0 and 3 */
    int s = layout_splitter_at(&L, 300, 50, 3, 28);
    assert(s == layout_leaf(&L, 0));
    assert(layout_splitter_at(&L, 300, 10, 3, 28) < 0);   /* tab bar row */
    layout_splitter_drag(&L, s, 450, 0, 100, 0);
    layout_compute(&L, 0, 0, 900, 600, 0, 0);
    assert(L.n[layout_leaf(&L, 3)].x == 450);
    layout_splitter_drag(&L, s, 5000, 0, 100, 0);       /* clamped */
    layout_compute(&L, 0, 0, 900, 600, 0, 0);
    assert(L.n[layout_leaf(&L, 3)].w == 100);

    /* order: 0, 3, 1, 2 */
    assert(layout_panel_step(&L, 0, 1) == 3 && layout_panel_step(&L, 2, 1) == 0);
    assert(layout_panel_step(&L, 0, -1) == 2);

    /* removing 2 collapses the 1-over-2 group back into a leaf */
    layout_remove(&L, 2);
    assert(layout_valid(&L) && layout_panel_count(&L) == 3);
    layout_compute(&L, 0, 0, 900, 600, 0, 0);
    assert(L.n[layout_leaf(&L, 1)].h == 600);

    /* nested same-direction group is spliced into its parent */
    Layout M;
    layout_init(&M, 0);
    layout_split(&M, 0, 1, 1);          /* V[0,1] */
    layout_split(&M, 1, 0, 2);          /* V[0,H[1,2]] */
    layout_split(&M, 2, 1, 3);          /* V[0,H[1,V[2,3]]] */
    layout_remove(&M, 1);               /* -> V[0,2,3] */
    assert(layout_valid(&M));
    int root = M.root, k = 0;
    for (int c = M.n[root].child; c >= 0; c = M.n[c].next) assert(M.n[c].panel >= 0), k++;
    assert(k == 3);
    layout_compute(&M, 0, 0, 1000, 100, 0, 0);
    assert(M.n[layout_leaf(&M, 2)].x == 500 && M.n[layout_leaf(&M, 3)].x == 750);

    /* pool exhaustion fails cleanly */
    Layout F; layout_init(&F, 0);
    int ok = 0;
    for (int p = 1; p < MAX_PANELS; p++) ok += layout_split(&F, p - 1, p & 1, p) >= 0;
    assert(ok == MAX_PANELS - 1 && layout_valid(&F));

    /* minimum sizes: V[0,H[1,2]] needs 2 leaves across and 2 down */
    Layout N;
    float mw, mh;
    layout_init(&N, 0);
    layout_split(&N, 0, 1, 1);
    layout_split(&N, 1, 0, 2);
    layout_min_size(&N, 100, 50, &mw, &mh);
    assert(mw == 200 && mh == 100);
    /* a sliver ratio is pinned at the minimum while there is room... */
    N.n[layout_leaf(&N, 0)].ratio = 0.02f;
    N.n[N.n[layout_leaf(&N, 0)].next].ratio = 0.98f;
    layout_compute(&N, 0, 0, 1000, 600, 100, 50);
    assert(N.n[layout_leaf(&N, 0)].w == 100 && N.n[layout_leaf(&N, 1)].w == 900);
    /* ...the ratio itself is kept, so it comes back without the minimum... */
    layout_compute(&N, 0, 0, 1000, 600, 0, 0);
    assert(N.n[layout_leaf(&N, 0)].w == 20);
    /* ...and a rect smaller than the minimum just shares by ratio. */
    layout_compute(&N, 0, 0, 150, 600, 100, 50);
    assert(N.n[layout_leaf(&N, 1)].x + N.n[layout_leaf(&N, 1)].w == 150);
    /* drag clamps to the neighbour group's minimum */
    layout_compute(&N, 0, 0, 1000, 600, 100, 50);
    layout_splitter_drag(&N, layout_leaf(&N, 0), 990, 0, 100, 50);
    layout_compute(&N, 0, 0, 1000, 600, 100, 50);
    assert(N.n[layout_leaf(&N, 1)].w == 100);

    /* last panel can't be removed; corrupt trees are rejected */
    layout_init(&L, 0); layout_remove(&L, 0);
    assert(layout_panel_count(&L) == 1);
    L.n[L.root].panel = MAX_PANELS;
    assert(!layout_valid(&L));
    layout_split(&M, 0, 0, 4);
    assert(layout_valid(&M));
    M.n[layout_leaf(&M, 4)].ratio *= 0.5f;          /* shares no longer add up */
    assert(!layout_valid(&M));
    layout_init(&L, 0);                             /* group with one child */
    layout_split(&L, 0, 1, 1);
    L.n[L.n[L.root].child].next = -1;
    L.n[layout_leaf(&L, 1)].used = 0;
    L.n[L.n[L.root].child].ratio = 1.0f;
    assert(!layout_valid(&L));
    puts("layout ok");
    return 0;
}
