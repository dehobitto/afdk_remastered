/* feat3.c - wire (line) editing: pull the free tip of a line with the left button, delete the line under the
   pointer with the right button. Included from feat.c.

   Scheme object layout (from the decompiled code): scheme[4] = elements list, scheme[5] = junction list,
   scheme[6] = wires list (list object: first dword = count; items through FUN_0045235c(list, index 1-based)).
   Wire: +0x14 style/name, +0x18 selected, +0x24/+0x28 start (x,y), +0x2c/+0x30 offset of the end point,
   +0x34/+0x38 start/end junction.  Junction: +4 wire list, +8 pin list, +0x14/+0x18 position.  */

#define A_MAINFORM_VAR   0x4e4b20       /* **(TMainForm**) : the main form with the tool buttons */
#define MF_TOOL_SELECT   0x374          /* TToolButton fields, byte +0x12a = Down */
#define MF_TOOL_LINE     0x37c
#define F_LIST_ITEM      0x45235c       /* (list, index) -> item */
#define F_REBUILD        0x4d22d4       /* (scheme) rebuild junctions after an edit (also marks the document modified) */
#define F_AFTER_EDIT     0x4d2bb0       /* (scheme) */
#define F_DESELECT_ALL   0x4d1c20       /* (scheme) */
#define F_DELETE_SEL     0x4d1bcc       /* (scheme) delete the selected items */
#define F_WIRE_SELECT    0x4d0a28       /* (wire, 1, flag, y, x) */

static unsigned main_form(void) { unsigned p = *(unsigned *)A_MAINFORM_VAR; return p ? *(unsigned *)p : 0; }

static int tool_down(int field)
{
    unsigned mf = main_form(), b = mf ? (unsigned)F32(mf, field) : 0;
    return b && readable((void *)(b + 0x12a), 1) && U8(b, 0x12a) != 0;
}

static unsigned list_item(unsigned list, int i) { return dcall((void *)F_LIST_ITEM, list, (unsigned)i, 0, 0, 0, 0); }


/* geometry helpers: model -> canvas client pixels */
typedef struct { void *cv; int level, hp, vp; } VIEW;
static void view_of(void *cv, VIEW *v)
{
    v->cv = cv;
    v->level = U8(cv, CV_LEVEL);
    v->hp = F32(F32(cv, CV_HBAR), BAR_POS);
    v->vp = F32(F32(cv, CV_VBAR), BAR_POS);
}
static int vx(const VIEW *v, int mx) { return mx * v->level - v->hp; }
static int vy(const VIEW *v, int my) { return my * v->level - v->vp; }

static int dist_to_seg2(int px, int py, int x1, int y1, int x2, int y2)   /* squared distance */
{
    double dx = x2 - x1, dy = y2 - y1, t, cx, cy;
    double len2 = dx * dx + dy * dy;
    if (len2 <= 0) { cx = x1; cy = y1; }
    else {
        t = ((px - x1) * dx + (py - y1) * dy) / len2;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        cx = x1 + t * dx; cy = y1 + t * dy;
    }
    return (int)((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

/* wire whose segment passes within 'rad' pixels of (px,py) */
static unsigned find_wire(unsigned scheme, const VIEW *v, int px, int py, int rad)
{
    unsigned wl = (unsigned)F32(scheme, 0x18), w, best = 0;
    int i, n, bestd = rad * rad + 1;
    if (!wl) return 0;
    n = F32(wl, 0);
    for (i = 1; i <= n; i++) {
        int d;
        w = list_item(wl, i);
        if (!w) continue;
        d = dist_to_seg2(px, py, vx(v, F32(w, 0x24)), vy(v, F32(w, 0x28)),
                         vx(v, F32(w, 0x24) + F32(w, 0x2c)), vy(v, F32(w, 0x28) + F32(w, 0x30)));
        if (d < bestd) { bestd = d; best = w; }
    }
    return best;
}

/* ---------- dragging the end of a line ----------
   Grab any end of a line (free, on a junction or on an element pin) and drag it: the line keeps its direction up to the
   pointer's column/row and then turns towards the pointer with a second segment, so it can be routed around corners. */
static unsigned new_wire_like(unsigned scheme, unsigned like);

static unsigned g_tip_wire, g_tip_scheme, g_tip_elbow;
static int g_tip_fx, g_tip_fy, g_tip_axis;      /* fixed end of the line; axis: 0 = horizontal, 1 = vertical, -1 = not yet known */
static HWND g_tip_cvw;
static unsigned g_tip_nodes[8];
static int g_tip_nnodes;

/* does any other line touch model point (x,y) (an end there, or passing through it)?  Named nodes are zero-length lines
   and do not count: they travel with the end they sit on. */
static int point_shared(unsigned wl, unsigned self, int x, int y)
{
    int i, n = F32(wl, 0);
    for (i = 1; i <= n; i++) {
        unsigned w = list_item(wl, i);
        int x1, y1, x2, y2;
        if (!w || w == self || (!F32(w, 0x2c) && !F32(w, 0x30))) continue;
        x1 = F32(w, 0x24); y1 = F32(w, 0x28); x2 = x1 + F32(w, 0x2c); y2 = y1 + F32(w, 0x30);
        if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
        if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
        if (x >= x1 && x <= x2 && y >= y1 && y <= y2) return 1;
    }
    return 0;
}

/* the line with an end within 'rad' pixels of (px,py) that no other line touches (a free end or one on an element pin);
   *end = 0 for its start, 1 for its end */
static unsigned find_tip(unsigned scheme, const VIEW *v, int px, int py, int rad, int *end)
{
    unsigned wl = (unsigned)F32(scheme, 0x18), w, best = 0;
    int i, n, bestd = 0x7fffffff;
    if (!wl) return 0;
    n = F32(wl, 0);
    for (i = 1; i <= n; i++) {
        int x1, y1, x2, y2, d1, d2, ds;
        w = list_item(wl, i);
        if (!w) continue;
        x1 = F32(w, 0x24); y1 = F32(w, 0x28);
        x2 = x1 + F32(w, 0x2c); y2 = y1 + F32(w, 0x30);
        if (F32(w, 0x2c) && F32(w, 0x30)) continue;                            /* only straight segments */
        if (!F32(w, 0x2c) && !F32(w, 0x30)) continue;                          /* a node: the original moves it */
        d1 = (vx(v, x1) - px) * (vx(v, x1) - px) + (vy(v, y1) - py) * (vy(v, y1) - py);
        d2 = (vx(v, x2) - px) * (vx(v, x2) - px) + (vy(v, y2) - py) * (vy(v, y2) - py);
        if (d1 > rad * rad || point_shared(wl, w, x1, y1)) d1 = 0x7fffffff;
        if (d2 > rad * rad || point_shared(wl, w, x2, y2)) d2 = 0x7fffffff;
        if (d1 == 0x7fffffff && d2 == 0x7fffffff) continue;
        ds = d1 < d2 ? d1 : d2;
        if (ds < bestd) { bestd = ds; best = w; *end = d2 < d1; }
    }
    return best;
}

static void wire_set(unsigned w, int x1, int y1, int x2, int y2)
{
    F32(w, 0x24) = x1; F32(w, 0x28) = y1; F32(w, 0x2c) = x2 - x1; F32(w, 0x30) = y2 - y1;
}

static void tip_apply(void *cv, int mx, int my)
{
    unsigned w = g_tip_wire;
    int fx = g_tip_fx, fy = g_tip_fy, cx, cy;
    if (g_tip_axis < 0) {                                                  /* a dot: the first move decides the direction */
        if (mx == fx && my == fy) return;
        g_tip_axis = abs(mx - fx) >= abs(my - fy) ? 0 : 1;
    }
    if (g_tip_axis == 0) { cx = mx; cy = fy; } else { cx = fx; cy = my; }  /* the corner */
    if (cx == fx && cy == fy) {                                            /* straight back across: one segment, other axis */
        wire_set(w, fx, fy, mx, my);
        if (g_tip_elbow) wire_set(g_tip_elbow, mx, my, mx, my);
    } else {
        wire_set(w, fx, fy, cx, cy);
        if (cx != mx || cy != my) {
            if (!g_tip_elbow) g_tip_elbow = new_wire_like(g_tip_scheme, w);
            if (g_tip_elbow) wire_set(g_tip_elbow, cx, cy, mx, my);
        } else if (g_tip_elbow) wire_set(g_tip_elbow, mx, my, mx, my);
    }
    {
        int i;
        for (i = 0; i < g_tip_nnodes; i++) { F32(g_tip_nodes[i], 0x24) = mx; F32(g_tip_nodes[i], 0x28) = my; }
    }
    dcall((void *)F_REFRESH, (unsigned)cv, 0, 0, 0, 0, 0);
}

static int tip_begin(HWND cvw, void *cv, int px, int py)
{
    unsigned pf = MAINFORM(), doc = pf ? (unsigned)F32(pf, 0x314) : 0, scheme = doc ? (unsigned)F32(doc, 0x34) : 0, w;
    int end = 0;
    VIEW v;
    if (!scheme || (unsigned)F32(pf, 0x308) != (unsigned)cv || !doc_edit_mode(doc)) return 0;
    if (!tool_down(MF_TOOL_SELECT)) return 0;
    if (F32(scheme, 0) && U8(F32(scheme, 0), 0x18)) return 0;                  /* a wire is being drawn */
    view_of(cv, &v);
    w = find_tip(scheme, &v, px, py, g_cfg_hit, &end);
    if (!w) return 0;
    hist_note(doc);
    dcall((void *)F_DESELECT_ALL, scheme, 0, 0, 0, 0, 0);
    g_tip_wire = w; g_tip_scheme = scheme; g_tip_cvw = cvw; g_tip_elbow = 0;
    if (end) { g_tip_fx = F32(w, 0x24); g_tip_fy = F32(w, 0x28); }
    else { g_tip_fx = F32(w, 0x24) + F32(w, 0x2c); g_tip_fy = F32(w, 0x28) + F32(w, 0x30); }
    g_tip_axis = F32(w, 0x30) == 0 && F32(w, 0x2c) != 0 ? 0 : F32(w, 0x2c) == 0 && F32(w, 0x30) != 0 ? 1 : -1;
    {   /* named nodes sitting on the grabbed end move with it */
        unsigned wl = (unsigned)F32(scheme, 0x18), n2;
        int i, n = F32(wl, 0), tx = end ? F32(w, 0x24) + F32(w, 0x2c) : F32(w, 0x24), ty = end ? F32(w, 0x28) + F32(w, 0x30) : F32(w, 0x28);
        g_tip_nnodes = 0;
        for (i = 1; i <= n && g_tip_nnodes < 8; i++) {
            n2 = list_item(wl, i);
            if (n2 && n2 != w && !F32(n2, 0x2c) && !F32(n2, 0x30) && F32(n2, 0x24) == tx && F32(n2, 0x28) == ty)
                g_tip_nodes[g_tip_nnodes++] = n2;
        }
    }
    return 1;
}

static void tip_move(void *cv, int px, int py)
{
    int mx = (int)dcall((void *)F_VIEW2MODEL_X, (unsigned)cv, (unsigned)px, 0, 0, 0, 0);
    int my = (int)dcall((void *)F_VIEW2MODEL_Y, (unsigned)cv, (unsigned)py, 0, 0, 0, 0);
    tip_apply(cv, mx, my);
}

static void delete_wire(unsigned scheme, unsigned w)
{
    unsigned wl = (unsigned)F32(scheme, 0x18);
    dcall((void *)F_DESELECT_ALL, scheme, 0, 0, 0, 0, 0);
    U8(w, 0x18) = 1;
    F32(wl, 0x30) = 1;                                                         /* selected-wire counter */
    dcall((void *)F_DELETE_SEL, scheme, 0, 0, 0, 0, 0);
    F32(wl, 0x30) = 0;
}

static void tip_end(void *cv)
{
    unsigned pf = MAINFORM(), doc = pf ? (unsigned)F32(pf, 0x314) : 0;
    if (g_tip_elbow && !F32(g_tip_elbow, 0x2c) && !F32(g_tip_elbow, 0x30)) delete_wire(g_tip_scheme, g_tip_elbow);
    if (!F32(g_tip_wire, 0x2c) && !F32(g_tip_wire, 0x30)) delete_wire(g_tip_scheme, g_tip_wire);   /* pulled back to nothing */
    dcall((void *)F_REBUILD, g_tip_scheme, 0, 0, 0, 0, 0);
    dcall((void *)F_AFTER_EDIT, g_tip_scheme, 0, 0, 0, 0, 0);
    dcall((void *)F_REFRESH, (unsigned)cv, 0, 0, 0, 0, 0);
    g_tip_wire = 0; g_tip_elbow = 0; g_tip_cvw = NULL;
    if (doc) hist_note(doc);
}

/* ---------- right button: delete the wire under the pointer ---------- */
static int wire_delete_at(HWND cvw, void *cv, int px, int py)
{
    unsigned pf = MAINFORM(), doc = pf ? (unsigned)F32(pf, 0x314) : 0, scheme = doc ? (unsigned)F32(doc, 0x34) : 0, w;
    VIEW v;
    if (!scheme || (unsigned)F32(pf, 0x308) != (unsigned)cv || !doc_edit_mode(doc)) return 0;
    if (!tool_down(MF_TOOL_SELECT) && !tool_down(MF_TOOL_LINE)) return 0;
    if (F32(scheme, 0) && U8(F32(scheme, 0), 0x18)) return 0;                  /* a wire is being drawn: right button ends it */
    view_of(cv, &v);
    w = find_wire(scheme, &v, px, py, g_cfg_hit);
    if (!w) return 0;
    /* an element under the pointer wins (its context menu stays available) */
    hist_note(doc);
    delete_wire(scheme, w);
    dcall((void *)F_REFRESH, (unsigned)cv, 0, 0, 0, 0, 0);
    hist_note(doc);
    return 1;
}


/* ---------- arrow keys move the selection (same code path as dragging it with the mouse) ---------- */
#define F_MOUSE_DOWN   0x4d1cf0         /* (scheme, x, y, button, ctrl) */
#define F_MOUSE_DRAG   0x4d29f0         /* (scheme, x, y) */
#define F_MOUSE_UP     0x4d1e1c         /* (scheme, x, y) */

static void mv_capture(unsigned scheme);
static void mv_apply(void);

static int nudge_selection(int dx, int dy)
{
    unsigned pf = MAINFORM(), doc = pf ? (unsigned)F32(pf, 0x314) : 0, scheme = doc ? (unsigned)F32(doc, 0x34) : 0;
    unsigned el, wl, w;
    int i, n, px = 0, py = 0, have = 0;
    if (!scheme || !doc_edit_mode(doc) || !tool_down(MF_TOOL_SELECT)) return 0;
    if (F32(scheme, 0) && U8(F32(scheme, 0), 0x18)) return 0;
    if (dcall((void *)0x4d1cc0, scheme, 0, 0, 0, 0, 0) <= 0) return 0;          /* nothing selected */
    el = (unsigned)F32(scheme, 0x10); wl = (unsigned)F32(scheme, 0x18);
    for (i = 1, n = el ? F32(el, 0) : 0; i <= n && !have; i++) {
        unsigned e = list_item(el, i);
        if (e && U8(e, 0x50)) {            /* a point inside the element body (centre of its definition box) */
            unsigned def = (unsigned)F32(e, 8);
            px = F32(e, 0x44) + (def ? F32(def, 0x21c) / 2 : 0);
            py = F32(e, 0x48) + (def ? F32(def, 0x220) / 2 : 0);
            have = 1;
        }
    }
    for (i = 1, n = wl ? F32(wl, 0) : 0; i <= n && !have; i++) {
        w = list_item(wl, i);
        if (w && U8(w, 0x18)) {
            px = F32(w, 0x24) + F32(w, 0x2c) / 2; py = F32(w, 0x28) + F32(w, 0x30) / 2; have = 1;
        }
    }
    fx_log("nudge: dx=%d dy=%d at %d,%d" NL, dx, dy, px, py);
    if (!have) return 0;
    hist_note(doc);
    mv_capture(scheme);
    dcall((void *)F_MOUSE_DOWN, scheme, (unsigned)px, (unsigned)py, 2, 0, 0);       /* press on a selected item ... */
    dcall((void *)F_MOUSE_DRAG, scheme, (unsigned)(px + dx), (unsigned)(py + dy), 0, 0, 0);
    dcall((void *)F_MOUSE_UP, scheme, (unsigned)(px + dx), (unsigned)(py + dy), 0, 0, 0);
    mv_apply();
    dcall((void *)F_REFRESH, (unsigned)F32(pf, 0x308), 0, 0, 0, 0, 0);
    hist_note(doc);
    return 1;
}
